# Reading `/profiledump` output (CSV + `.folded`)

Guide for interpreting the CPU-attribution profiler's dumps. See `cpu-attribution-profiling-plan.md` for the design and `project_cpu_attribution_profiler` / `project_tracy_zone_profiler_integration` memories for context.

## Producing a dump

Set at launch in `springsettings.cfg`: `LuaTrackCalloutCounts = 2` (0=off, 1=call counts only, 2=counts + callout body timing + per-widget zones + unified flamegraph). The value gates a lot — `>0` is needed for per-addon `W:`/`G:` zones and for the engine to keep BAR's tracy-zone calls; `2` is needed for callout body times.

In-game / via `Spring.SendCommands`: `/profiledump <startFrame> <endFrame> [outPath]`. It force-enables the profiler over `[f0,f1]`, samples once per `SimFrame`, and writes the CSV when the game reaches `f1` (you must let it run to the end frame). Relative paths resolve to the **write data-dir**. A sibling `<out>.folded` is written alongside.

## CSV schema

Header: `frame,name,self_ms,incl_ms,self_sim_ms,incl_sim_ms,count`

- `frame` — the sim frame this sample was taken at.
- `name` — zone/timer/callout name. **Can contain commas** (e.g. `Update::WorldDrawer::{Sky,Water}`), so parse fields from the right, not by naive comma split.
- `self_ms` / `incl_ms` — self (exclusive) vs inclusive (subtree) time for this name, as a **delta since the previous sampled frame**.
- `self_sim_ms` / `incl_sim_ms` — the portion that ran inside `CGame::SimFrame` (the sim phase). `self_ms - self_sim_ms` is the draw/update portion.
- `count` — `0` for zones/timers; `>0` for **callout rows** (number of calls that frame).

Each row is a per-sim-frame **delta of cumulative totals**. For per-frame averages, sum a name's column over all rows and divide by the frame count. Draw-side numbers aggregate *all* render frames that occurred in that sim interval (sim is ~30 Hz, draw is uncapped) — so "Draw 40 ms/frame" is draw work per sim tick, not one DrawWorld call.

## Row types by name

- `Sim::*`, `Draw::*`, `Update::*`, `Misc::*` — engine C++ zones. LuaJIT/FFI cannot help these.
- `Lua::Callins::{Synced,Unsynced}` and `::<callin>` — per-callin aggregate + per-callin timers (the C++→Lua boundary).
- `W:<callin>:<widget>`, `G:<callin>:<gadget>` — **per-addon zones** (driven from BAR's tracy zones; need `LuaTrackCalloutCounts>0`). This is the per-widget/gadget attribution.
- bare name with `count>0` — a **Lua callout** (`Texture`, `RenderToTexture`, `GetUnitPosition`, …).

### Zone rows (count==0)
`self_ms` = time in this zone excluding differently-named child *zones*; `incl_ms` = whole subtree. Note: callout body time is part of a zone's `self` in the CSV (callouts are separate accounting) — the `.folded` unified flamegraph is where callouts get split out of the zone self.

### Callout rows (count>0, deep mode only)
`self_ms` = body time **excluding nested callouts**; `incl_ms` = body **including** nested callouts. For **leaf** callouts `self==incl`. For **higher-order** callouts (`RenderToTexture`, `BeginEnd`, `ActiveTexture`, and event-triggering ones like `CreateUnit`/`DestroyUnit`/`GiveOrderToUnit`) `incl > self`, because they run nested callouts / Lua / event cascades. **Sum the `self` column for an honest callout total** — summing `incl` double-counts (higher-order rows contain the leaves).

## `/boundarydump` (sim|draw boundary sizes)

`/boundarydump <startFrame> <endFrame> [out.csv]` is the companion boundary-size dump (one fixed-width row per sim frame: transform/uniform storage sizes and dirty rates, piece-pose churn, command-queue mutations and length distribution, projectile churn by class, unit/feature/LOS event rates, hot-field change rates). It keeps its own small buffer, so full-game ranges are fine where /profiledump's per-name rows would be too heavy. Columns are self-describing; `d_` prefixes are per-frame deltas of cumulative counters, `hf_` are per-unit hot-field exact-change counts, and `sample_cost_ms` is the sampler's own per-frame cost.

## `.folded` (flamegraph)

Brendan-Gregg collapsed-stack format: one line per unique call path, `root;child;…;leaf <microseconds>`, summed over the whole dump window. Weight is **self**-microseconds; the flamegraph tool sums children to get inclusive widths. Import into speedscope (drag the file in) or feed to `flamegraph.pl`.

The tree is the self-time stack: engine zones → callins → `W:`/`G:` addon zones. In **deep mode** callouts are woven in as leaves under their zone (e.g. `…;W:DrawScreen:Top Bar;RenderToTexture;Texture`), with higher-order callouts nesting correctly. It is self-weighted and double-count-free (each callout's time is subtracted from its enclosing zone's folded self). Without deep mode the tree bottoms out at the zone level.

## Caveats that change conclusions

- **Probe overhead.** `Misc::Profiler::AddTime` (~4–5 ms/frame) is the profiler's own bookkeeping, present only during a dump and redistributed into parents' self-time. Absolute self-times are inflated — treat them as **ordinal**, not exact. Counts and callout-self are the most trustworthy numbers.
- **Hub inflation.** Dispatch/glue nodes with many child timers absorb their children's bookkeeping cost into their own self.
- **ThreadPool is not serial.** `ThreadPool::RunTask`/`AddTask`/`WaitFor` are summed across worker threads (self==incl), not main-thread wall time. The signal there is `WaitFor`'s `self_sim` = main thread *blocked* waiting on sim workers.
- **Mode matters.** `count>0` rows have non-zero body times only in deep mode (`=2`). In counts mode (`=1`) you get `count` but `self/incl = 0`.
- **GPU-bound vs CPU-bound.** Check `Misc::SwapBuffers` self: high (tens of ms) ⇒ the CPU is stalling on the GPU/vsync, so trimming CPU time won't raise FPS. Low (~1 ms) ⇒ CPU-bound, savings convert to FPS. State this before drawing optimization conclusions.

## Parsing recipe

Split fields from the right (names contain commas):

```python
import collections
def load(path):
    agg = collections.defaultdict(lambda: [0.0,0.0,0.0,0.0,0]); frames=set()
    with open(path) as fh:
        next(fh)
        for line in fh:
            p = line.rstrip('\n').split(',')
            if len(p) < 7: continue
            fr=p[0]; count=int(p[-1]); incl_sim=float(p[-2]); self_sim=float(p[-3])
            incl=float(p[-4]); self=float(p[-5]); name=','.join(p[1:-5])
            frames.add(int(fr)); a=agg[name]
            a[0]+=self; a[1]+=incl; a[2]+=self_sim; a[3]+=incl_sim; a[4]+=count
    return agg, len(frames)   # divide by len(frames) for per-frame averages
```

For `.folded`: each line is `path... <int_us>`; split on the last space for the weight, `;` for the stack.

## Interpreting for the LuaJIT/optimization decision

1. **Budget**: top-level `Sim` vs `Draw` vs `Update` inclusive. Then `SwapBuffers` to decide CPU- vs GPU-bound.
2. **Engine vs Lua**: top self zones that are *not* `W:`/`G:`/callouts are engine C++ — out of scope for LuaJIT.
3. **FFI ceiling**: sum callout `self` where `ns/call` (`self*1e6/count`) is low (≲300 ns) — boundary-dominated calls. That's what FFI/batching can reclaim. High-frequency cheap callouts (`Texture`, `GetProjectile*`, `GetUnit*`, `Uniform`) are the targets.
4. **Cache candidates**: high-self `W:DrawScreen:*` widgets driving `RenderToTexture`/`CopyToTexture` re-render static UI every frame — dirty-flag/cache the texture (engine-agnostic, not a LuaJIT change).
5. **Per-callout `ns/call`**: low + high-frequency ⇒ FFI. High `ns/call` (e.g. `RenderToTexture`, `GetVisibleProjectiles`, `CallList`) ⇒ real work, needs caching/algorithmic fixes, not FFI.
6. **Pure VM** ≈ (addon-zone self) − (callout self). LuaJIT already JITs this; further wins are algorithmic. The unified flamegraph shows it directly (zone self after callouts are split out).
