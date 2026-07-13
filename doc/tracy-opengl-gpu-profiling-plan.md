# Plan: Tracy OpenGL GPU profiling

Status: proposal. Scoping/implementation plan for adding Tracy GPU support (timing + memory) to the engine. Nothing here changes default builds — every piece is a no-op unless built with `-DTRACY_ENABLE=ON` and, for the Lua/memory layers, a runtime dev flag.

See also: [Profiling with Tracy](site/content/development/profiling-with-tracy.md) (existing CPU-side setup).

## Background: what already exists

- Tracy **v0.11.1** vendored at `rts/lib/tracy`, linked as `Tracy::TracyClient` (`rts/CMakeLists.txt`).
- **CPU profiling only.** ~4300 `ZoneScoped`/`RECOIL_DETAILED_TRACY_ZONE` sites; `FrameMark` in `CGlobalRendering::SwapBuffers` (`GlobalRendering.cpp:713`); project wrapper `rts/System/Misc/TracyDefs.h`.
- **Existing Lua hook:** `rts/Lua/LuaTracyExtra.cpp` exposes `tracy.LuaTracyPlot` / `LuaTracyPlotConfig` (plots only — no zone begin/end is exposed to Lua, deliberately).
- **CPU memory** is already instrumented via `TracyAlloc`/`TracyFree` (`TraceMemory.cpp`, `MemoryOverride.cpp`), gated by `TRACY_PROFILE_MEMORY` (off by default, "expensive").
- **Build gate:** `rts/lib/CMakeLists.txt:170` declares `option(TRACY_ENABLE OFF)` before `add_subdirectory(tracy)`, so it wins the cache. With it off, **all** Tracy macros (including every GPU macro) compile to no-ops.

## Prerequisites (verified present)

- GLAD (compatibility, gl 4.6) exports everything `TracyOpenGL.hpp` needs: `glGenQueries`, `glQueryCounter`, `glGetQueryObjectui64v`, `glGetInteger64v`, `glGetQueryiv`, `GL_TIMESTAMP`, `GL_QUERY_COUNTER_BITS`.
- The engine already drives `glQueryCounter(..., GL_TIMESTAMP)` for its own built-in GPU frame timer (`CGlobalRendering::SetGLTimeStamp`), guarded by `GLAD_GL_ARB_timer_query` — Tracy's OpenGL backend uses the identical mechanism.
- Total VRAM is already queried via `GL_NVX_gpu_memory_info` / `GetAvailableVideoRAM` (`GlobalRendering.cpp:1011`).

## Tracy's OpenGL API (manual §GPU profiling → OpenGL, `rts/lib/tracy/manual/tracy.tex:1598`)

Four integration points: include `tracy/TracyOpenGL.hpp`; `TracyGpuContext` once after GL load on the render thread; `TracyGpuCollect` once per frame after swap; `TracyGpuZone("name")` (or `...C`/`Named` variants) around GPU work. Tracy requires **one context, one thread, no migration**, and `Collect` on that thread/context.

## Engine-specific constraints

- **Context migration during MT loading.** `LoadScreen.cpp` rebinds the primary context to a hidden window on the game-load thread while the main thread draws the load screen on a *secondary* context — exactly the migration + two-simultaneous-contexts case Tracy warns about. GPU profiling must therefore be confined to steady-state gameplay on the main thread + primary context.
- **`GlobalRendering::SwapBuffers` is shared** — called from the main loop *and* the load screen, splash screen, and Lua (`gl.SwapBuffers`). So `TracyGpuCollect` must **not** live next to the existing `FrameMark` inside `SwapBuffers`; it goes in the real frame loop only.
- **Headless.** `TracyOpenGL.hpp` only stubs out on `__APPLE__`, not headless. All GPU calls must be wrapped in `#ifndef HEADLESS` (matches the existing `#if !defined(HEADLESS)` pattern around context creation).
- **`TRACY_ON_DEMAND`** (enabled here) makes `GpuCtxScope` early-return unless a server is connected, so zones are safe with no profiler attached — *provided the context was created*. If `GLAD_GL_ARB_timer_query` is absent we must skip context creation, and zones would then deref a null ctx if a server connected. In practice every target GL has timer_query; gate context creation on it and document the assumption.

---

## Layer 1 — Engine per-phase GPU zones (base integration)

Goal: GPU timing of the engine's own render passes and, coarsely, of each Lua draw call-in (all widgets/gadgets for a phase lumped together).

### Wiring (context + collect)

- **Context creation** in `CGlobalRendering::PostInit()`: `#ifndef HEADLESS … if (GLAD_GL_ARB_timer_query) TracyGpuContext; #endif`. Query objects are owned by the GL context, so they survive the later load-time thread migrations.
- **Collect** in `SpringApp::Update()` immediately after `globalRendering->SwapBuffers(swap, false)` (`SpringApp.cpp:894`) — the genuine frame loop, primary context, which naturally excludes loading/splash/Lua swaps. `#ifndef HEADLESS TracyGpuCollect; #endif`.

### Zone granularity — reuse the existing debug-group map

The engine **already pairs `SCOPED_TIMER` (CPU) + `SCOPED_GL_DEBUGGROUP` (GPU marker) at every render-pass boundary**, in a nested, named hierarchy (~25 sites across `WorldDrawer.cpp`, `Game.cpp`, `IWater.cpp`, `RoamMeshDrawer.cpp`, `GlobalRendering.cpp`): `Draw` → `Draw::World` → `Draw::World::Models::Opaque`, `Draw::World::Terrain`, `Draw::World::Particles`, `Draw::Water::DrawReflections`, `Draw::Screen::*`, etc. That set is a dev-curated map of exactly the GPU-work boundaries we want zones at — so granularity is largely already decided.

**Approach: fold the GPU zone into the debug-group annotation.** Add a combined scoped macro, e.g. `SCOPED_GL_GPU_ZONE("name")`, that emits **both** the existing KHR_debug marker **and** a `TracyGpuNamedZone` (named variant so it coexists with `SCOPED_TIMER` in one scope; no-op under HEADLESS and when `TRACY_ENABLE` is off). Migrate the ~25 sites mechanically (`SCOPED_GL_DEBUGGROUP` → `SCOPED_GL_GPU_ZONE`). This yields ~25 GPU zones with zero new placement decisions, consistent names, correct nesting — and one annotation keeps the RenderDoc/Nsight marker and the Tracy zone in sync forever, including for future passes.

Note: the Tracy macro uses `__LINE__`/`__FILE__` statics at the call site, so the combined macro must expand both at the site — it can't be hidden inside `GL::DebugGroup::GetScoped` in the .cpp. The migration is purely the macro name.

- **Root zone:** the existing top-level `"Draw"` group (`Game.cpp`) becomes the per-frame GPU root everything nests under.
- **Per-phase Lua zones:** wrap the Lua draw call-in dispatch in `CLuaHandle` (`DrawWorld`, `DrawScreen`, `DrawWorldPreUnit`, …) with `TracyGpuZone`. One GPU bar per phase per Lua state (LuaUI = widgets; LuaRules/LuaGaia = gadgets). Automatic, no Lua-side changes. One call-in runs *all* widgets for that phase in a single Lua state, so this is the **sum** per phase, not per-widget (Layer 2 splits it).

### Granularity rules

- **GPU zones only around real GPU work** (draws, clears, blits, dispatch) — this is the key difference from CPU zones. A GPU zone on a CPU-only scope (culling, matrix setup, a texture *upload* like `UpdateShadingTex`) reads ~0 on the GPU timeline and misleads. Target the **`SCOPED_GL_DEBUGGROUP` subset, not the full `SCOPED_TIMER` set** — it's already filtered to GPU boundaries. A couple of debug-group sites are mixed/CPU-ish (e.g. `UpdateMisc`); skip those during migration.
- **Pass-level is the sweet spot.** Don't go per-draw / per-unit / per-batch by default: GPU timers can't resolve tiny draws and the per-zone timestamp overhead can exceed the work measured.
- **Cost is not a concern at pass level.** Each zone = 2 `glQueryCounter` (async, no pipeline stall) + 2 entries from Tracy's 64K query ring; dozens/frame is trivially within budget.
- **Optional detailed tier:** a `RECOIL_DETAILED_TRACY_GPU` gate (mirroring `RECOIL_DETAILED_TRACY_ZONING`) for temporarily dropping in deeper zones — per-shadow-cascade, water internals — during a targeted hunt, off by default.

Effort: 1 combined macro + a mechanical rename of ~25 sites + the 2 wiring edits (context/collect) + the Lua call-in zones.

---

## Layer 2 — Per-widget GPU zones (safe, opt-in)

Goal: attribute GPU time to individual widgets/gadgets.

Hard constraint: the engine has **no concept of a widget** (confirmed — only doc-comment mentions in `rts/Lua/`). `CLuaUI` is one `CLuaHandle`; the engine makes one call-in per phase and the game's Lua widget handler fans it out. So the per-widget boundary exists **only in game Lua** and the *mark* must originate there — but without handing a footgun to arbitrary widgets:

- **Higher-order primitive, not raw begin/end.** Expose one function — `tracy.GpuProfile(name, fn, ...)` — that in C++ does GPU-zone-begin → `lua_pcall(fn)` → GPU-zone-end, all in the C function's own scope. Begin/end live entirely in C++ and are always balanced, even when the Lua call errors. A caller cannot corrupt the LIFO query stream. (This is why the existing Lua surface exposes only plots, never raw zone begin/end.)
- **Driven from the one trusted dispatch.** The widget handler already has a single central place where it calls each widget's call-in (BAR-style handlers already wrap it for CPU profiling). That one wrapper routes through `tracy.GpuProfile(widgetName, w.DrawScreen, w)`. Individual widgets are untouched and need not know it exists.
- **Absent unless enabled.** Register `tracy.GpuProfile` only when a dev profiling config is set (mirror `TRACY_ENABLE` / `RECOIL_DETAILED_TRACY_ZONING`). When off, the function isn't in the Lua environment at all — no random widget can call what doesn't exist.
- **Engine safety net.** Give `ScopedLuaCall` (the call-in bracket in `RunCallInTraceback`, `LuaHandle.cpp:362`) a GPU-zone-depth guard: record open-zone count on entry, force-close any left open on exit — same shape as the existing `GLMatrixStateTracker.PushMatrixState()/PopMatrixState()` on entry/exit. Guarantees stream balance across the call-in boundary regardless of any Lua bug.

Note: engine-enforced "handler-only, not widgets" scoping isn't available (the widget/handler sandbox is constructed in game Lua, invisible to the engine). Pieces above make that moot: the primitive is safe-by-construction and absent-by-default. If strict handler-only is still wanted, the handler controls what it injects into each widget's environment and simply doesn't forward it.

Caveat: GPU zone = GPU execution time of what the widget submitted (shaders, overdraw, VBO size). "Slow widget" is often CPU-bound (issuing draw calls / Lua) — covered by a CPU zone around the same loop, not this.

---

## Layer 3 — GPU memory tracking (leak detection + attribution)

Goal: find GPU-object leaks and see what/who is using the memory.

Tracy has **no native GPU-memory API** — GPU support is timing only. Repurpose Tracy's **named-pool** memory API (`TracyAllocN(key, bytes, "pool")` / `TracyFreeN(key, "pool")`) as a fake "GL allocator." Tracy then shows, per pool: live byte count, allocation timeline, and the **call stack of every live allocation** — i.e. leak attribution.

Two complementary parts:

- **(A) Total-VRAM plot** — `TracyPlot("GPU VRAM", total-avail)` once per frame from the existing NVX/ATI query. Driver-true, whole-process; catches leaks even in uninstrumented/driver paths, but no attribution. Cheap, do this first.
- **(B) Named-pool accounting** — instrument the GL-object choke points with `TracyAllocN`/`TracyFreeN`. Per-allocation call stacks + live set, but only instrumented paths and only *requested* bytes.

Detect with A, attribute with B; the gap between A's total and B's sum = driver-internal/uninstrumented space.

Choke points (bounded, not scattered raw GL):

- Engine: `Rendering/GL/VBO.cpp`, `RenderBuffers`, `StreamBuffer`; textures `Rendering/Textures/Texture.cpp`, `TextureAtlas.cpp`, `Bitmap.cpp`, `NamedTextures.cpp`.
- **Lua/widget-owned (highest value): `LuaVBO`, `LuaTextures`, `LuaFBOs`, `LuaRBOs`, `LuaAtlasTextures`** — manually managed objects, the common real-world VRAM-leak source. Tag these allocations per Lua handle (or per-widget via the Layer-2 wrapper idea) to answer "which widget is leaking VRAM."

Caveats:

- **Key collisions:** Tracy keys on the "pointer"; GL ids are small ints reused across types (texture 1 vs buffer 1 collide). Namespace by per-type pool or encode type into the key.
- **Exact pairing:** Tracy flags double-free / free-of-unknown as errors — every alloc must pair with its free. Fine for RAII; care needed for ref-counted/shared objects and context-loss teardown.
- **Requested ≠ real VRAM:** mip chains, alignment, compression, driver overhead, resident-vs-spilled are invisible. Solid leak/attribution proxy, not a VRAM accountant (that's layer A).
- **FBOs/RBOs/VAOs are ~0 bytes** (handles); bytes live in the textures/renderbuffers/buffers they reference — account there, not at the FBO.
- **Driver-internal allocs** (shader compiler, default FB, PBO staging) never visible.
- **Overhead low:** GL object alloc/free is far rarer than `malloc` — a better-behaved use of Tracy memory profiling than the CPU `TRACY_PROFILE_MEMORY` path. Gate behind the same dev flag.

Usefulness verdict: strong for the stated goal. Engine GL objects mostly have RAII wrappers so engine leaks are rarer; the high-value target — manually-managed Lua-created objects — is exactly where the "widget leaks VRAM" concern lives, and B gives a call stack landing in the `LuaTextures`/`LuaVBO` wrapper, optionally tagged per widget.

---

## Cross-cutting

- **Gating:** Layer 1 is build-gated only (`TRACY_ENABLE`). Layers 2 and 3 additionally behind a runtime dev config so they're absent in normal sessions.
- **Profiler GUI must be Tracy v0.11.x** (protocol match with the vendored client).
- **Driver-dependent accuracy** (manual "caveat emptor"): AMD/Linux resets the timestamp register on GPU low-power entry (every frame with vsync off); Intel/Linux 36-bit timer rollover. Tracy has heuristics but results can be noisy.
- `TracyGpuCollect` costs a couple µs — fine once per frame.

## Suggested sequencing

1. Layer 1 wiring (context + collect) + the combined `SCOPED_GL_GPU_ZONE` macro, applied first to the root `"Draw"` group — proves the pipeline end to end.
2. Layer 1 mechanical migration of the remaining ~25 debug-group sites + per-phase Lua call-in zones.
3. Layer 3A (VRAM plot) — one-liner, immediate leak-trend visibility.
4. Layer 3B (named-pool accounting at the choke points) + per-handle tagging.
5. Layer 2 (`tracy.GpuProfile` primitive + `ScopedLuaCall` safety net) — per-widget timing, and reuse the tagging for per-widget memory.

Each step is independently shippable and a no-op unless explicitly enabled.
