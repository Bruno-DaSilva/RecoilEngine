# Plan: Native CPU attribution profiler — self-time tree, boundary layers, frame-range dump

Status: all six steps implemented (not yet built/run).

## Implemented

- **Self-time (step 1).** `ScopedTimer` keeps a thread-local open-timer stack; each zone charges its inclusive span to its parent so `self = inclusive − differently-named-children`. Same-name recursion skipped via the existing `refCounters` gate. New `TimeRecord` fields; `AddTime`/`AddTimeRaw` take a `selfTime` arg. `self-%` column in `ProfileDrawer`, two extra returns from `Spring.GetProfilerTimeRecord`. The push/pop is factored into shared `timerStackPush/Pop` helpers reused by step 5.
- **Zero cost when off.** Ordinary timers, the per-callin timers, and Lua zones do their bookkeeping only when the profiler is enabled (a dump force-enables it), captured per-timer at construction so push/pop stay balanced across a toggle. Special timers (`Sim`/`Draw`/`Lua::Callins` aggregate) still report when disabled. Net: during normal play ordinary timers no longer even touch `refCounters` — *cheaper* than the pre-existing baseline; the attribution machinery costs nothing until you turn the profiler on.
- **Per-callin keying (step 2a).** `LUA_CALL_IN_CHECK` opens a nested `Lua::Callins::{Synced,Unsynced}::<callin>` timer (via `__func__`, `CallinTimerNames`) *inside* the existing synced/unsynced aggregate — additive, aggregates untouched. Gated by the enable check above.
- **Per-callout counts (step 2b).** `LuaTrackCalloutCounts` config: `0` off, `1` counts, `2` counts+body-time. ≥1 wraps each callout at `REGISTER_LUA_CFUNC` in a counting closure (count aggregated by name; each registration keeps its own func for correct dispatch). Live query `Spring.GetCalloutCounts`. Default off ⇒ callout path byte-for-byte unchanged.
- **Frame-range dump (step 3).** `/profiledump <f0> <f1> [out.csv]` force-enables the profiler over `[f0,f1]`, samples per-name deltas each `CGame::SimFrame`, writes CSV `frame,name,self_ms,incl_ms,self_sim_ms,incl_sim_ms,count` on completion. Engine-side/headless. Callout rows (count>0) arrive via a hook the Lua layer registers.
- **Deep body-timing (step 4).** Level 2 brackets each callout body with a clock in the trampoline; per-name body wall-time (and its sim portion) fold into the callout rows' self/incl columns in the dump.
- **Lua zone primitive (step 5).** `Spring.ProfilerPushZone(name)`/`ProfilerPopZone()` — manual (non-RAII) zones nesting in the same self-time stack via `CTimeProfiler::PushZone/PopZone`, so per-addon self-time, child callouts, phase, and dump come for free. Registered in **both** the synced (gadget) and unsynced (widget) tables; sync-safe since they return nothing. Hardened against misuse: zone frames are tagged so `PopZone` refuses to pop an enclosing RAII timer's frame (over-pop), and a `ScopedZoneStackGuard` at each `LUA_CALL_IN_CHECK` discards zones left open within a callin (under-pop/leak), so a buggy addon can't corrupt enclosing accounting — it just gets a warning.
- **Phase tag (step 6).** `ScopedSimFramePhase` RAII around `CGame::SimFrame` sets a thread-local flag; `AddTimeRaw` accumulates a sim-phase portion (`totalSim`/`selfTotalSim`), exposed as the dump's `*_sim_ms` columns. Charges unsynced widget callins run from `SimFrame` to the sim budget.

Caveats / deferred polish: toggling the profiler mid-frame leaves that one frame's attribution slightly off (timers constructed before the flip aren't tracked) — self-heals next frame. Enabling counts wraps callouts in closures, so a `lua_tocfunction`-identity check would see the trampoline (opt-in only); `funcs` slots are append-only across state reloads. Dump path is CWD-relative (could route through the write-dir resolver). Worker-pool (`ScopedMtTimer`) time counts as non-sim. LuaJIT-internal profiling (`jit.profile`) is the complementary layer below "pure Lua" and is not wired in.



Tracy and the in-engine `CTimeProfiler` both tell you a zone is *hot*; neither tells you what's *responsible* when C++ cascades into Lua and back. `Sim::Weapon` fires a weapon → damage kills a unit → `UnitDamaged`/`UnitDestroyed` callins run a pile of gadget Lua → which calls back into engine callouts — and the whole cascade is charged to `Sim::Weapon`. This plan makes the engine's own profiler answer "where did the frame actually go," natively (no Tracy, no external tooling), dumpable to a file over a chosen frame range. Target use: deciding where to spend caching/batching effort — move a loop to the C++ or Lua side, flatten objects into arrays, drop a callout to FFI, stop crossing the boundary.

Re-lands the opt-in `/luaprofile` prototype (whose findings still hold — see [[project_lua_boundary_profiler_findings]]) permanently in `CTimeProfiler`, and generalizes it. Directly feeds the benchmark that [Move unsynced rendering work out of G:GameFrame](move-unsynced-gameframe-work-todo.md) needs.

## Three axes, each conflated by current tools

Attribution is really three orthogonal questions:

- **Boundary layer — what kind of work.** Of Lua-touching time: C++ callout *body* vs callin *glue* vs callout *glue* vs pure Lua vs GC. Nobody tracks this; `LUA_CALL_IN_CHECK` collapses every callin into two buckets (`Lua::Callins::Synced`/`Unsynced`). **This is the original ask** and the bulk of the work below.
- **Culprit — whose work.** Which zone / callin name / callout name / gadget / widget. Tracy has it live; BAR's `dbg_*_profiler` has it but keyed on synced/unsynced. The "which individual `Gadget::GameFrame` is heavy" question.
- **Phase — which budget.** Sim (~33ms/30Hz critical path) vs Draw vs Update. Useful, but a single orthogonal tag, not the headline — the synced/unsynced split is *not* the phase split (an unsynced widget's `GameFrame` runs inside `CGame::SimFrame` and spends the sim budget), so it has to be engine-side, but it's one bool.

## Root cause: the profiler is inclusive-only

`CTimeProfiler` records **inclusive wall-clock** per name. `ScopedTimer`'s destructor stores the full ctor→dtor span and `AddTimeRaw` just sums it into `total`/`current`/`frames[]`; there is no parent/child accounting anywhere. The only nesting logic, `refCounters`, dedups *same-name* recursion and nothing else. So Lua time triggered inside `Sim::Weapon` is counted in `Sim::Weapon` **and again** in `Lua::Callins::Synced`, with no subtraction — which is exactly why a hot zone can't be decomposed.

## The key lever: one self-time tree, read at increasing depth

Add the one thing the engine lacks — a call tree — by the cheapest means: a thread-local stack of open timers, subtract children.

```cpp
// thread_local std::vector<Frame> stack;   // Frame{ nameHash, childTime, startTime }
ScopedTimer::~ScopedTimer() {
    const spring_time incl = GetDuration();
    Frame f = stack.back(); stack.pop_back();
    const spring_time self = incl - f.childTime;
    if (!stack.empty() && stack.back().nameHash != nameHash)  // skip same-name recursion
        stack.back().childTime += incl;                       // charge my whole span to my parent
    if (--refCounters[nameHash] == 0)
        AddTime(nameHash, startTime, incl, self, ...);        // record BOTH
}
```

Contained to `TimeProfiler.{h,cpp}`, plus a `self` field + ring in `TimeRecord`, a `ProfileDrawer` column, and an extra `Spring.GetProfilerTimeRecord` return. **This same tree is the whole plan** — the other axes are just deeper reads of it and one extra tag:

- Read at **depth 0** (free): existing engine zones + the `Lua::Callins` boundary → `Sim::Weapon` self = real C++ weapon code; the cascaded Lua lands under `Lua::Callins::Synced`. Sum of all self-times = the whole frame, no double-counting → a self-sorted list *is* the optimize-next view (inclusive sorting can't give that — parents always dominate children, same multi-count warning as the GPU tree in [unified GPU timing](unified-gpu-timing-plan.md)).
- Read **deeper** (next two sections): extend the same stack through the `pcall` and the callout trampoline → the boundary-layer split and per-callout culprits.
- Tag every node with **phase** → the sim/draw budget view.

## Boundary layers — the C++-vs-Lua-vs-glue split (the original ask)

You wanted "time in the Lua and the C++↔Lua glue, *separate* from the C++ time running callouts, but also the two altogether." Those map onto nodes of the tree, from the callin boundary down:

- **callin glue** — function lookup, arg push, traceback setup, `pcall` enter/exit, stack check, GL state save/restore, GC stop. = the `Lua::Callins::*` zone's **self-time outside the pcall**.
- **pure Lua** — bytecode run inside the `pcall`, excluding callouts it invokes. = the **pcall span minus its callout-body children**.
- **callout body** — the real C++ engine work + C-side arg marshalling inside each callout. = each trampoline-timed callout span, **summed = "C++ running callouts."**
- **callout glue** — the per-crossing dispatch (LuaJIT C-call overhead; ~zero on FFI paths). = trampoline residual, or a per-call constant measured against a no-op callout.
- **"altogether"** = the `Lua::Callins::*` **inclusive** time — bodies + Lua + glue, already recorded, now phase-correct and self/inclusive-split.

The only new machinery is making callout bodies into child zones, at the `REGISTER_LUA_CFUNC` chokepoint (the single registration path for callouts): wrap each `lua_CFunction` in a trampoline. The trampoline has two modes:

- **cheap (always-on):** `count++` per callout name. No clock reads.
- **deep (opt-in):** also bracket the body with a timer, accumulated per callout name.

In deep mode the tree now spans `Sim::Weapon → Lua::Callins::Synced → (pure Lua | callout bodies)`, and the four layers above are pure subtraction. Because deep mode times all ~27k callouts/frame, it costs ~5ms (per-callout clock reads) that land *inside* the pcall and inflate "pure Lua" — so trust `callout body` directly and derive true pure-Lua as *baseline pcall self (deep mode off) − callout bodies*. That caveat is why body-timing is opt-in and counters are not.

## Culprit attribution — where to look, and the caching payoff

Self-time says *what kind*; this says *which one*, the lever for the LuaJIT-era optimizations you're after:

- **per-callout name: count (always-on) + body-time top-N (deep).** Count alone is the cheapest, highest-signal number for your caching decisions — "`GetUnitPosition` called 9000×/sim-frame from one widget" flags a batch/cache/loop-relocation target, and "`RenderToTexture` 99µs×120/frame" flags an FFI or fewer-crossings target — without timing's perturbation. The prototype saw ~27k callouts/frame; this surfaces *which*.
- **per-callin name** — key the callin timer by callin (`UnitDestroyed`, `GameFrame`, …) instead of two synced/unsynced lumps. With self-time: "`Sim::Weapon` self 3ms, triggered `UnitDestroyed` callins 5ms."
- **per-gadget / per-widget** — the engine sees one merged handler list, so it *can't* know which addon is running unless the handler tells it. Expose a Lua zone primitive — `Spring.Profiler.PushZone(name)` / `PopZone()` (same idea as `gl.GpuProfile` in the GPU plan) — and have the widget/gadget handler open a zone per addon-callin. Those nest *in the native tree*, so per-addon self-time, its triggered callouts as children, phase, and dump all come for free. This **fixes** BAR's `dbg_*_profiler` (feed it correct phase + self-time numbers) rather than duplicating it.

Net: count + body-time per callout name is the map from "hot" to a concrete fix — batch the loop to one crossing, cache the result, back it with a flat array so the body's access is cache-friendly, or convert to FFI to bypass glue + marshalling entirely.

## Phase: one orthogonal tag

A thread-local `inSimFrame` flag set by an RAII bracket around `CGame::SimFrame`; every `AddTime` buckets into sim vs draw/update (a 2-wide accumulator per record). That alone charges `Lua::Callins::Unsynced` run during `SimFrame` to the **sim** budget — the measurement the [unsynced-GameFrame relocation](move-unsynced-gameframe-work-todo.md) work wants ("did moving this body leave the sim critical path?"), per-zone instead of by eyeballing the `Sim` total. It composes with every node above; it is not a separate view.

## Frame-range dump to file

The 128-frame ring in `TimeRecord` is already a capture buffer; generalize it. A console action — `/profiledump <f0> <f1> out.csv` — appends each frame's per-name `{self, inclusive, count, phase}` between the two frames and serializes on stop. Engine-side (not a polling widget) so it works headless and captures even while the on-screen profiler is disabled — fits the replay/benchmark loop in [[project_headless_speed_control]].

## What you'll be able to answer

- Is `Sim::Weapon` hot from C++ or from the Lua it cascades into? → self-time (depth 0).
- Of that Lua, how much is callout bodies vs pure Lua vs glue — and the three altogether? → boundary layers (deep mode).
- Which callout is called so often, or costs so much per call, that caching / batching / FFI pays? → per-callout count + body top-N.
- Which individual gadget/widget callin is heavy? → the Lua zone primitive.
- How much of the ~33ms sim budget is *really* Lua, including unsynced callins inside `SimFrame`? → phase tag.
- Where did the frame go, top to bottom, over frames 5000–5300 of this replay? → self-sorted dump.

## Properties and caveats

- **Free part is free.** Self-time adds a push/pop + subtract per `ScopedTimer` (coarse zones, tens–hundreds/frame); per-callout counters are a single increment. Always-on viable.
- **Deep mode is the heavy one.** Timing 27k callouts/frame adds ~5ms and over-states pure-Lua — opt-in, derive pure-Lua by subtraction (above).
- **Main-thread.** The timer stack and `inSimFrame` are thread-local; worker-pool zones keep using the separate `ScopedMtTimer` path, untouched.
- **Same-name recursion stays correct** — the stack reuses the existing `refCounters` gate; a same-name child isn't subtracted from its parent.
- **Self-time is only as granular as your zones** — it tells you *where to add the next zone* or open a Lua one, not function-level lines.
- **LuaJIT tips the balance toward bodies + marshalling** — the JIT makes pure Lua cheap while C callout bodies don't speed up, so callout body-time and counts only get more decision-relevant on this branch.

## Sequencing

Each step is independently shippable and a no-op (or near) unless engaged.

1. **Self-time** in `CTimeProfiler` (+ `TimeRecord.self`, `ProfileDrawer` column, `Spring.GetProfilerTimeRecord` return). The keystone — every existing zone, `Sim::Weapon` included, answers C++-vs-Lua immediately, and the "altogether" Lua number is just `Lua::Callins::*` inclusive.
2. **Cheap culprit, always-on** — per-callin-name buckets + per-callout volume counters via the `REGISTER_LUA_CFUNC` trampoline (count-only). Highest signal-per-cost for the caching calls.
3. **Frame-range dump** — `/profiledump`, reusing the ring, so everything above is capturable.
4. **Deep mode** — callout-body timing in the trampoline → the full boundary-layer split + per-callout body top-N.
5. **Lua zone primitive** — `Spring.Profiler.PushZone/PopZone`; wire the widget/gadget handler to open per-addon zones in the native tree (fixes BAR's profiler).
6. **Phase tag** — `inSimFrame` RAII + 2-wide accumulators; fold into step 1's records when convenient.
