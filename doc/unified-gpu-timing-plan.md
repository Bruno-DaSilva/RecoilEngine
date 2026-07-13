# Plan: Unified GPU timing — engine passes + Lua, one ring, multiple sinks

Status: proposal.

The two sibling plans each cover one slice of GPU timing; this one unifies them and adds the missing piece (engine render-pass times reaching Lua).

- [Tracy OpenGL GPU profiling](tracy-opengl-gpu-profiling-plan.md) — adds engine + Lua GPU zones, but only into **Tracy** (i.e. `-DTRACY_ENABLE=ON` dev builds). Implemented (Layer 1 + the `tracy.GpuProfile` Lua primitive).
- [Tracy-independent async GPU timing for Lua](lua-async-gpu-timer-queries-plan.md) — gives **stock builds** widget-level GPU timing via thin `gl.QueryCounter`/`GetQueryAvailable`/`GetQueryResult` bindings + a ring buffer living in the `dbg_gpu_profiler.lua` widget. Does **not** surface the engine's own render-pass times (they're C++ `SCOPED_GL_GPU_ZONE` sites, invisible to Lua).

Gap this plan closes: a single engine-side timing ring that is fed by the engine's *own* pass boundaries **and** Lua, readable from stock-build Lua, and optionally mirrored to Tracy — so the in-game profiler can show one GPU hierarchy spanning engine passes down to individual widgets.

## The key lever

`SCOPED_GL_GPU_ZONE(name)` already brackets every engine GPU-work boundary (~18 sites: `Draw::World::*`, `Draw::Screen::*`, water, ROAM). Today it feeds two sinks (KHR_debug marker + Tracy zone). Add a **third sink** so the same annotation also records `(begin, end, name)` into an engine timing ring. Engine pass times then appear in Lua for free — no per-site edits, and every future pass is covered automatically.

## Architecture — `GL::GpuTiming`

A small engine service (in / beside `CGlobalRendering`) owning:

- a ring of `glQueryCounter(GL_TIMESTAMP)` query objects (timestamps, not `GL_TIME_ELAPSED` — they nest freely; `GL_TIME_ELAPSED`/`GL_SAMPLES_PASSED` allow only one active query, which is why `gl.RunQuery` is documented non-recursive).
- a pending list mapping `(beginId, endId, key, source)` per in-flight span.
- a per-key accumulator of completed elapsed times.
- a runtime `enabled` flag (the profiling toggle).
- a `Collect()` that drains *ready* results each frame (poll `GL_QUERY_RESULT_AVAILABLE`, never block) and folds `end − begin` into the accumulator.

Directly mirrors the engine's existing `CGlobalRendering::CalcGLDeltaTime` (double-buffered, read-next-frame), generalized from 8 fixed slots to a keyed ring. `Collect()` runs once per frame in `SpringApp::Update`, next to `TracyGpuCollect` (steady-state, primary context).

## One macro, three independently-gated sinks

`SCOPED_GL_GPU_ZONE(name)` expands to:

1. **KHR_debug marker** — RenderDoc/Nsight. When KHR_debug is present.
2. **Ring span** (`GL::GpuTimerScope`) — RAII begin/end into `GL::GpuTiming`. Gated on the runtime `enabled` flag (cheap no-op otherwise). **Not** `TRACY_ENABLE`-gated — works in stock builds.
3. **Tracy zone** — `TracyGpuNamedZone`. Gated on `TRACY_ENABLE` + connected.

Sink 2 is the new one. Because it's a no-op unless profiling is toggled on, the always-running engine passes cost nothing in normal play.

## Lua surface

- **Producer (higher-order, safe):** `gl.GpuProfile(key, fn, ...)` — brackets a `pcall(fn)` with a ring span (and a Tracy zone when enabled), balanced even on Lua error. Same shape as the `tracy.GpuProfile` already implemented; **fold that one into this** so there is a single primitive, with Tracy as an optional internal sink rather than a separate function.
- **Consumer:** `gl.GetGpuProfilingData()` → `{ [key] = { ms = …, source = "engine"|"lua" } }`, the few-frames-delayed results delivered since the last poll. The profiler reads this once per frame.

The thin bindings from the async-timer plan (`gl.QueryCounter` etc.) remain useful for widgets that want to scatter raw timestamps inside their own call-in; `gl.GpuProfile` is the safe, keyed convenience for the common "time this addon's draw" case.

## You get a tree, not a list

Engine names nest (`Draw` ⊃ `Draw::World` ⊃ `Draw::World::Models::Opaque`), timestamps nest, and per-widget zones sit inside the per-phase Lua zone inside `Draw::Screen`. The `::`-delimited keys reconstruct one hierarchy from engine passes down to widgets. Parents *include* their children — present it as a tree (inclusive/self time), never a flat sum, or you multi-count.

## Relationship to the two existing plans

- Supersedes the **engine-pass-to-Lua** gap neither sibling covers.
- Composes with, does not replace, the async-timer plan's thin bindings (keep them for widget self-instrumentation).
- The implemented `tracy.GpuProfile` becomes the Tracy sink of `gl.GpuProfile` rather than a standalone function.

Open architectural decision — **where the ring lives:**

- *Engine-side ring (this plan):* required for the engine-pass feed (those are C++ sites); gives one unified source and tree; more engine machinery.
- *Lua-side ring (async-timer plan):* simpler engine (thin bindings only), maximal Lua flexibility; but cannot see engine passes.

Recommendation: engine-side ring, because the engine-pass exposure is the whole point and only it can provide that; keep the thin bindings alongside for flexibility.

## Properties and caveats

- **Drift-immune.** Elapsed is `end − begin` in the GPU clock, so the CPU/GPU calibration issue (fixed separately in the Tracy backend) does not affect these numbers.
- **Async delay.** Results lag a few frames — fine for a rolling-average overlay, not for same-frame "measure and branch" logic.
- **GPU ≠ CPU.** Most widgets are CPU-bound and read ~0 GPU; show GPU as a **separate column**, never replacing CPU time.
- **Opt-in cost.** Per-pass/per-widget timestamping has real (small, async) cost; gate behind the profiler's "enable GPU timing" toggle, which also flips `GL::GpuTiming::enabled`.
- **Tiny draws / ring limits / driver noise.** Bracket meaningful chunks, not individual `gl.Rect`s (below timer resolution); size the ring and skip timing on exhaustion (never stall/crash); AMD-reset / Intel-rollover HW is noisier.
- **Headless.** Stub the new bindings; the ring is `!HEADLESS` only.

## Sequencing

1. `GL::GpuTiming` service: ring + pending list + accumulator + per-frame `Collect()` (model on `CalcGLDeltaTime`), behind an `enabled` flag.
2. Add sink 3 (`GL::GpuTimerScope`) to `SCOPED_GL_GPU_ZONE` → engine passes flow into the ring. Verify end-to-end with a temporary log dump.
3. `gl.GetGpuProfilingData()` consumer; surface engine-pass times in a test overlay.
4. `gl.GpuProfile(key, fn, ...)`; fold in `tracy.GpuProfile` as its Tracy sink.
5. Wire the `dbg_gpu_profiler.lua` widget to read engine passes + widget keys from one source; drop any `gl.Finish` path.

Each step is independently shippable and a no-op unless the profiling toggle is on.
