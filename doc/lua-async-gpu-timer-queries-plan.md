# Plan: Tracy-independent async GPU timing for Lua (no `TRACY_ENABLE`)

Status: TODO / proposal.

Related: [Tracy OpenGL GPU profiling](tracy-opengl-gpu-profiling-plan.md) — that plan adds GPU zones to **Tracy-enabled builds**. This one is orthogonal: it gives the same async mechanism to **stock builds** with Tracy off (the normal case), by exposing GL timer queries to Lua so the `dbg_gpu_profiler.lua` widget can stop stalling.

## Motivation

Tracy GPU zones only exist in a `-DTRACY_ENABLE=ON` build, which is **not** how the game normally runs. The `dbg_gpu_profiler.lua` widget (BAR game repo, `luaui/Widgets/dbg_gpu_profiler.lua`) exists to give per-widget / whole-frame GPU timing in a stock build, but it can only sync via `gl.Finish()` — a full pipeline stall that inflates frame times. Its "widget" mode serializes CPU↔GPU around every hooked draw call-in; even "overall" mode pays one finish/frame for the GPU tail.

Goal: give that widget Tracy's *mechanism* — async `glQueryCounter(GL_TIMESTAMP)` + deferred, non-blocking readback — **without requiring Tracy**, so it reports accurate GPU time with no stall.

## Root cause

The Lua GL API exposes **only occlusion queries**. `gl.CreateQuery`/`RunQuery`/`GetQuery` (`rts/Lua/LuaOpenGL.cpp:6490+`) are hardcoded to `GL_SAMPLES_PASSED`, and `GetQuery` reads with the *blocking* `GL_QUERY_RESULT`. There is no way from Lua to issue a timestamp, poll availability non-blockingly, or read back a uint64 ns value. `gl.Finish` is the only GPU-sync primitive Lua has — hence the widget uses it.

## How Tracy does it (the mechanism to copy)

`rts/lib/tracy/public/tracy/TracyOpenGL.hpp`, `GpuCtx`: a ring of 64K query objects (`glGenQueries`); `glQueryCounter(id, GL_TIMESTAMP)` at each zone begin/end (non-blocking markers recording the GPU clock when the GPU's command processor reaches them); once per frame `TracyGpuCollect` polls `glGetQueryObjectiv(GL_QUERY_RESULT_AVAILABLE)` and reads back only the *ready* results with `glGetQueryObjectui64v(GL_QUERY_RESULT)` — never waiting. Results lag the CPU a few frames; the CPU never blocks on the GPU. The engine already drives this exact call for its built-in frame timer (`CGlobalRendering::SetGLTimeStamp`, guarded by `GLAD_GL_ARB_timer_query`).

## Engine work — expose timer queries to Lua (`rts/Lua/LuaOpenGL.cpp`)

Mirror the existing occlusion-query tracking (`occlusionQueries` vector + userdata index scheme; reuse it for cleanup on shutdown / context loss). Add:

- `gl.CreateQuery([target])` — let create take a target; default occlusion for back-compat, allow `GL_TIMESTAMP`. Gate on `GLAD_GL_ARB_timer_query`; return `nil` if absent or `GL_QUERY_COUNTER_BITS == 0`.
- `gl.QueryCounter(q)` → `glQueryCounter(q->id, GL_TIMESTAMP)`. No begin/end, no LIFO constraint — safe to interleave across widgets/phases (the reason Tracy uses timestamps over `GL_TIME_ELAPSED`).
- `gl.GetQueryAvailable(q)` → `glGetQueryObjectiv(q->id, GL_QUERY_RESULT_AVAILABLE)` → bool. Non-blocking.
- `gl.GetQueryResult(q)` → uint64 ns, **non-blocking by design**: internally check availability and return `nil` if not ready, so the binding cannot be misused to reintroduce a stall (key safety decision — see risks).
- Headless: stub the new entry points in `rts/lib/headlessStubs/gladstub.cpp` or the link breaks.

## Widget rewrite (game repo `luaui/Widgets/dbg_gpu_profiler.lua`)

Port Tracy's `GpuCtx` ring-buffer approach into Lua: keep a ring of query objects; bracket each hooked widget draw call-in (and the whole-frame span for overall mode) with two `gl.QueryCounter` calls; a per-frame collector drains the *ready* queries (`GetQueryAvailable` / `GetQueryResult`) and attributes `(end − begin)` ns to that widget. Results lag a few frames; **no `gl.Finish` anywhere**, in either mode. Degrade gracefully to the current `gl.Finish` path (or disable) when `gl.CreateQuery(GL_TIMESTAMP)` returns `nil`.

## Use case: a widget self-profiling its own call-ins

These bindings aren't only for the central `dbg_gpu_profiler` widget — any widget can self-instrument. `gl.QueryCounter(q)` just inserts a timestamp token into the command stream at the call site, so a widget can bracket sub-spans inside its *own* call-in and attribute GPU time to its internal sections:

```lua
gl.QueryCounter(qA0);  drawTerrainOverlay();  gl.QueryCounter(qA1)
gl.QueryCounter(qB0);  drawRangeRings();       gl.QueryCounter(qB1)
-- a few frames later, once available:
local ms_overlay = (gl.GetQueryResult(qA1) - gl.GetQueryResult(qA0)) / 1e6
```

No engine cooperation beyond the bindings; timestamps don't nest or need balancing, so a widget can scatter as many as it wants in any order. This is the per-widget idea applied recursively — a widget profiling its own internals (e.g. discovering "my fullscreen effect is the 4 ms, not my UI quads").

Two constraints to document for widget authors:

- **Granularity — group by expensive GPU work, not by individual GL call.** `GL_TIMESTAMP` records the GPU clock when the command processor crosses the token, after prior work drains the pipeline. GPUs pipeline/overlap adjacent draws heavily, so bracketing individual tiny draws (a `gl.Rect`, a small quad) yields noisy / near-zero / mutually-overlapping numbers that don't sum cleanly, and the per-timestamp overhead can exceed the work measured. Bracket *meaningful* chunks — a fullscreen shader pass, a large VBO draw, a heavy blur — same rule as the Tracy plan's "pass-level is the sweet spot, don't go per-draw by default."
- **Deferred readback, not same-frame.** To stay stall-free the result is read a few frames later (`GetQueryAvailable` → `nil` until ready). Good for a live overlay (numbers lag a handful of frames); not usable for in-line "measure this section and branch on it this frame" logic — that would require the blocking readback we deliberately avoid.

## Risks (and mitigations)

1. **Blocking-readback footgun (main one).** `glGetQueryObjectui64v(GL_QUERY_RESULT)` blocks until the GPU passes that timestamp; reading without first checking availability re-creates the `glFinish` stall — subtler and harder to spot (the existing `gl.GetQuery` already has this shape). → Make `GetQueryResult` non-blocking by construction (return `nil` when unavailable); misuse becomes impossible, not just discouraged.
2. **Driver/hardware support.** Timer queries are core GL 3.3 (`ARB_timer_query`) and near-universal on desktop, but not guaranteed. Tracy disables GPU profiling on macOS (`__APPLE__`) due to flaky Apple timer support. → Capability-check, return `nil`, widget degrades (same as its VRAM-on-Intel fallback).
3. **Resource lifetime.** Query objects are context-owned; the widget's ring (hundreds–thousands) must be freed on shutdown and recreated on context loss. → Reuse the `occlusionQueries` tracking so cleanup is shared.
4. **Use-after-delete.** Reading a deleted-but-in-flight query reads a freed id (same class as current `GetQuery`). → Ring buffer never deletes in-flight queries.

**Not risks:** nesting (timestamps have no LIFO/balance constraint, unlike `GL_TIME_ELAPSED` and unlike the engine's own `TracyGpuZone` LIFO stream); conflict with the engine's Tracy GPU context (separate query objects); security/sandbox (only exposes timing; worst case is a stall, and `gl.Finish` is already exposed — mitigation #1 closes even that, so no new attack surface).

## Sequencing

1. Engine bindings in `LuaOpenGL.cpp` (+ headless stubs), with capability check and non-blocking `GetQueryResult`.
2. Widget ring buffer + per-frame collector; convert "overall" mode (one timestamp pair per frame, no finish).
3. Convert "widget" mode to per-call-in timestamp pairs; drop the `gl.Finish` brackets.
4. Graceful fallback when timer queries are unavailable.
