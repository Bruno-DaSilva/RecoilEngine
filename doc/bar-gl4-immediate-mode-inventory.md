# BAR → modern-GL migration: living inventory & plan

Working document for moving Beyond-All-Reason (`/www/projects/Beyond-All-Reason`) and the Recoil engine off deprecated fixed-function / immediate-mode OpenGL, so a live BAR frame can be captured in RenderDoc. Iterate on this as we learn more — especially after runtime tracing (see Runtime findings log).

## Goal

**Definition #1 — a RenderDoc-capturable BAR frame.** Remove every OpenGL call RenderDoc rejects (immediate mode, fixed-function matrix stack, display lists, attrib stack, alpha test, line stipple, point sprite, fixed-function lighting) from a normal in-match frame. The engine can keep requesting a **compatibility** GL context (`GlobalRendering.cpp:512`); RenderDoc tolerates a compat context, it only chokes on the rejected *calls*. We are **not** targeting a strict GL4 core-profile context (that would additionally require the whole engine, not just BAR-triggered paths, to be core-clean, and the engine to flip its context request).

## Status / changelog

- 2026-07-04 — **Coverage breadth (divergence-plan step 2) — dev-content gates all 0/0; three engine parity-bug families found and fixed by the replay content.** Ran the gate beyond fightertest: a Supreme Isthmus 24-player replay (draft mode, reflection/refraction, chat/playerlist churn), a local Supreme Isthmus water test (BumpWater reflections + PiP + camera waypoints over water + buildmenu/build orders) and an idle base-building start. Engine fixes (all found by the gate, localized via the null test + apitrace pass-diff + dump PNGs, exactly per the documented recipe):
  1. **Font uniform-cache desync via display-list compiles** (`778abc2866`): glUseProgram/glUniform are RECORDED into `gl.CreateList` bodies, not executed (earlier comments had this backwards!) — `CglShaderFontRenderer::PushGLState`'s cached `SetUniform("uUseMVP",…)` during a compile updated the CPU cache but never the GPU, so later "redundant" legacy writes were skipped and whole text elements vanished from legacy passes (replay pregame "Victory condition" text, ~12.5k px on 40% of frames, control=0, null-clean; apitrace showed the identical draw with no glUniform reaching the legacy pass). Fix: skip the entire shader path when `GL_LIST_INDEX != 0` (the recordable-FF flush needs no shader).
  2. **Modern gl.Rect/TexRect/BeginEnd inside gl.CreateList** (`92c205e135`): the modern flush recorded glDrawElements aliasing the stream VBO (replays whatever occupies those offsets later — vanishing startbox polygons, phantom playerlist icons) + recorded program/uniform calls (bug 1's class). All modern dispatch sites now compile the legacy FF path (glBegin/glEnd IS the recordable representation).
  3. **Polygon-mode + FF-current-color + texture-enable parity** (`92c205e135`, texture-enable follow-up): glRectf honors `glPolygonMode` (unit_share_tracker draws GL_LINE outline rects — per-call compare delta 232; fix dropped replay in-game signal from 904/1043 frames to 7/177); legacy fills/modulates with the FF CURRENT color (not LuaOpenGL's tracked `color[]`, which is blind to dlist replays/recordable font flushes) and leaves it unchanged unless the body called glColor — the flushes now seed from `glGetFloatv(GL_CURRENT_COLOR)` and restore the exact legacy end-state (new `SeedColor`/`Color` split); gl.TexRect only samples when `GL_TEXTURE_2D` is enabled (modern shader would sample regardless).
  - **Replay-gating infrastructure discovery:** demos mount the demo's pinned release `.sdp` archive, NOT `games/BAR.sdd` — BAR-branch guards are invisible in replay runs. Replay gating needs write-dir shadow widgets = release widgets (GitHub raw at the recorded commit, e.g. `test-30440-535fc79` → `535fc79`) + the guard patterns. Shadow set built for screencopy (per-passIndex key — REQUIRED for the signal to be meaningful), guishader, flowui, bloom, pregameui×2; with it the replay pregame control went 9 leaking frames → 0/2934. See `test/gl-ab-compare/README.md` (drivers + local content scripts committed there; `AB_DUMP_MIN_FRAME` env keeps the dump budget for in-game frames).
  - BAR-branch guards (`f90a198daa`): pregameui/draft countdown blink timer advanced per DrawScreen = per pass (233↔255 flip = the max-22 fightertest pregame leak); guishader stencil/delete-queue per-pass freeze; FlowUI guishader-dlist lifecycle freeze.
  - **Results after the first batch:** fightertest 279/279, water-map 644/644, idle-start 410/410 — control=0 AND signal=0 (incl. pregame). The replay still had a modern-linked residual tail, burned to zero the same day (below).
- 2026-07-04 (later) — **REPLAY GATE FULLY BYTE-PERFECT: control=0 AND signal=0 on all 3871 pregame + 1071 in-game compared frames of the Supreme Isthmus multiplayer replay (to f=30000), plus fightertest 271/271, water-map 652/652, idle-start 410/410 with the same binary.** The residual tail decomposed into four more engine fixes, all localized with the null test + a 31 GB apitrace of the pregame→in-game window (glretrace crashed on the trace, so the decisive evidence came from a full normalized call-stream diff of pass 2 vs pass 3 plus blob extraction — vertex payloads identical, index shifts uniform, leaving only the color calls):
  1. **Overbright color wrap** (`fe76b4acb0`, the dominant class): widgets set out-of-range colors (minimap camera box: `glColor4fv(.., b=1.15)`). FF clamps at rasterization (blue→255) but keeps the unclamped value in the CURRENT-color state; SColor's raw float→uint8 cast WRAPS (1.15·255=293→37) — the box drew blue in legacy, mustard in modern (the max-218 class, apitrace-confirmed digit for digit: trailing `glColor4ub(186,170,37)`). Also explained the playerlist-icon and late-game classes. Fix: `ClampedColor` (clamp + round) everywhere the modern path quantizes.
  2. **Quantized fade alphas** (same commit): animated fades (α=0.1595653…) aren't 8-bit-representable → max 4–12 px tails. Single-color streams (gl.Rect, gl.TexRect, single-color BeginEnd = every fade) now pass the exact float color via a new `uColor` uniform (vertex colors white); `FlushLegacy` replays original float colors; flushes restore the FF current color from floats (`glColor4fv`), so unclamped state round-trips.
  3. **GameSetupDrawer countdown per-pass advance** (`ad8ff67244`): the engine's "Starting in n" pregame text (drawn as fallback in replays — BAR's pregame widget doesn't claim GameSetup for spectators) advanced `readyCountdown` by raw wall clock once per PASS; the digit could flip between compared passes. Pinned to the first pass via `LuaUnsyncedRead::IsABDuplicatePassRaw()`.
  4. **TypedRenderBuffer mid-frame grow corrupted the VAO** (`05cfcd8901`, latent vanilla bug): `Resize()` recreates the buffer object but the VAO (initialized once) kept referencing the deleted old buffer — uploads went to the new storage while draws sourced the stale old one. Found because 4 in-frame passes accumulate 4× stream data. Fix: re-`InitVAO` + full re-upload after a grow.
  - Also `452d3d4ce1`: modern gl.TexRect falls back to legacy when `GL_TEXTURE_2D` is disabled (legacy doesn't sample then).
- 2026-06-30 — initial consolidation from static analysis. Key conclusions below are static-grep-derived and **must be confirmed at runtime** (`DeprecatedGLWarnLevel=2` + RenderDoc reject log). Nothing built yet.
- 2026-06-30 — closed the tracer-coverage gap: added `CondWarnDeprecatedGL(L, __func__)` to `Rect`, `TexRect`, `DrawGroundQuad`, `PushAttrib`, `PopAttrib` in `rts/Lua/LuaOpenGL.cpp` (they previously fired no warning). `DeprecatedGLWarnLevel=2` now covers all relevant Lua-side blockers. Deliberately NOT added to `gl.PolygonMode` and `gl.Texture` — both are core-valid (not RenderDoc blockers) and would flood the log with false positives.
- 2026-06-30 — adopted a **test-forward, oracle-backed** method: legacy path (behind the flag) is the oracle; added a Compatibility contract (C1–C6), a write-test-first loop with a green-suite commit gate, harness positive/negative controls, and per-phase done-gates. Legacy is not deleted until the whole contract is green over unit scenes + a replay bake.
- 2026-06-30 — **In-engine compare is now a committed, repeatable integration test.** `test/engine/Rendering/glcompare_integration.sh` (+ `glcompare_startscript.txt`, `README.md`) runs the GL `spring` with `LuaGLCompareMode=1` over a BAR startscript and exits non-zero if any wired `gl.*` callin diverges >1 byte, the compare FBOs fail, or nothing was compared. Turns the ad-hoc validation into a one-command PASS/FAIL artifact. Current result: **PASS — 89 wired callins, max byte delta 1.** (Manual/integration: needs a display + local BAR data + the GL binary; not a ctest unit test.)
- 2026-06-30 — **Broadened emitter unit coverage** (`testGLImmediateEmitter`, now 13 cases/68 asserts): all primitive modes (TRIANGLE_FAN/STRIP, QUAD_STRIP, LINE_STRIP/LOOP, POINTS) A/B ≤1 LSB; **C4 translucent draw-order** (two overlapping half-alpha quads, SRC_ALPHA blend, legacy vs modern composite identically); **C5 edge cases** (empty BeginEnd, single vertex, degenerate zero-area triangle — no crash, no GL error). - 2026-06-30 — **Attrib-stack swap analyzed — deferred, bundle with FF-reset removal (not a standalone win).** Investigated replacing `glPushAttrib`/`glPopAttrib` (the one seemingly content-independent Phase-0 piece). Conclusion: it can't improve RenderDoc-cleanliness on its own, because restoring the saved `AttribBits`/`ResetGLState` state (mostly fixed-function: alphaFunc/shadeModel/logicOp/texEnv/lighting/texgen/lineStipple/clipPlanes/pointSprite/material/current-color) requires re-emitting those deprecated FF calls. Also `State.h` is a RAII state-*management* system (`GL::SubState`), not a save/restore snapshot. **Remaining task: do the attrib-stack swap together with turning `ResetGLState`'s FF resets into no-ops/shader-defaults**, once content no longer depends on FF state — see the annotated Phase-0 fix list. Until then there's no incremental Phase-0 win; the lever is converting content off FF (uniform/UBO-MVP, display lists, residuals).
- 2026-06-30 — **Phase-0 uniform-MVP prerequisite documented; general textured BeginEnd attempted and (correctly) deferred.** Added the Phase-0 ⚠ TODO above (uniform/UBO MVP conversion is the real blocker — the font renderer + RenderBuffer shaders read `gl_ModelViewProjectionMatrix`). Then tried modernizing *general* textured `gl.BeginEnd` (`VA_TYPE_TC` + the MODULATE shader). The in-engine compare immediately caught it: `gui_pip`'s textured overlays (`DrawWaterAndLOSOverlays`, a `drawFn`) diverged Δ255 — real widgets use texture-unit/texenv setups a single MODULATE-on-unit-0 shader can't reproduce, and `gui_pip` leans on `gl.UseShader` + multitexture. Reverted textured BeginEnd to the exact legacy replay (Δ0); `gl.TexRect` (a controlled single MODULATE quad) stays modernized. Net: `TriangulateForModern` is now generic (`VA_TYPE_C`/`VA_TYPE_TC`) for when textured BeginEnd is revisited with texenv/unit awareness. The harness catching this pre-merge is exactly the test-forward payoff.
- 2026-06-30 — **Smooth-shaded QUADS/POLYGON now tested — no gap.** A per-vertex-colored `GL_QUADS` and `GL_POLYGON` A/B vs legacy come out **≤1 LSB** on llvmpipe: the modern path's `v0-v2` quad / fan triangulation matches Mesa's FF decomposition diagonal even with interpolated colors. (A driver whose FF used the *other* diagonal could differ near the diagonal for smooth fills, but the modern triangulation is the canonical result going forward — the legacy compare is only the transitional oracle.) All Tier-1/2 contract dimensions now have coverage.
- 2026-06-30 — **C3 deepened to a full state snapshot.** The state-leak test now captures program, VAO, `GL_ARRAY_BUFFER_BINDING`, blend enable + func, depth test + write-mask, active texture unit and bound `GL_TEXTURE_2D` before/after `FlushModern` and asserts the whole set is preserved (with a distinctive non-default pre-state). Result: the modern flush leaves all observable restorable GL state intact (incl. the array-buffer binding) — FF-only state it intentionally abandons (current color, matrix stack) is excluded as an expected divergence, not a leak.
- 2026-06-30 — **Matrix-tracker randomized property test** (`testGLMatrixStateTracker`, now 18 cases / ~4285 asserts): 3000 random ops (Translate/Scale/Rotate/Push/Pop/mode-switch) mirrored into an independent per-mode `std::vector<CMatrix44f>` stack, asserting the active matrix matches after every op — exercises the stack/mode bookkeeping far beyond the fixed cases. Fixed seed for reproducibility.
- 2026-06-30 — **`gl.BeginEnd`/`Vertex`/`Color`/`TexCoord` wired (gated) + validated in-engine.** `gl.BeginEnd` now uses capture-then-render when `(LuaModernGLBackend || LuaGLCompareMode)` and no shader is bound: the callback's `gl.Vertex`/`Color`/`TexCoord` route into `LuaImmediateBuffer` (no FF calls) via an `inModernBeginEnd` flag; after the callback the captured stream is compared (both flushes) and/or rendered (modern or legacy). The emitter now stores `VA_TYPE_TC` (captures texcoords) + a `textured` flag; `FlushLegacy` replays texcoords for an exact legacy equivalent, and `FlushModern` falls back to `FlushLegacy` for textured streams (general textured BeginEnd deferred). MVP from `glGetFloatv` (Phase-0 bridge); `NoShaderBound()` guard. Unit tests extended (textured-BeginEnd capture/fallback; 10 cases/43 assertions). **In-engine compare re-run (Starwatcher, f400): 86 wired call-sites — 83×Δ0, 3×Δ1, zero Δ>1, no FBO-unavailable** — `gl.BeginEnd` across real BAR widgets (LuaIntro, unit_seismic_ping, gfx_unit_shield_effects, …) is pixel-identical (≤1 LSB) to legacy. Legacy path byte-unchanged when both flags off.
- 2026-06-30 — **FIRST IN-ENGINE VALIDATION over a real BAR frame — passed, and caught+fixed a real bug.** Ran `build/spring` (GL/legacy) with `LuaGLCompareMode=1` on a short auto-quitting BAR startscript (Starwatcher map, `setspeed 20`, `quitforce` at f400; `DISPLAY=:0 SDL_VIDEODRIVER=x11`, `--isolation --write-dir … --config <tmp>`), reached **in-game frame ~410**, and the comparator logged real widgets' `gl.Rect`/`gl.TexRect` (`gui_top_bar`, `gui_given_units`, `unit_respawning`, LuaIntro loading screen, …). **First run: 27×Δ0, 1×Δ1, and 2×Δ255** in the offline LUT bakes `GenBrdfLut.lua`/`GenEnvLut.lua`. Root cause (a real wiring bug): `gl.UseShader` calls `glUseProgram` **directly** (LuaShaders.cpp:835) without notifying `shaderHandler`, so the `GetCurrentlyBoundProgram()==nullptr` guard missed the bound LUT shader and the modern path overrode it. **Fix:** guard on the live GL state via `glGetIntegerv(GL_CURRENT_PROGRAM)` (`NoShaderBound()`), which catches engine *and* Lua-bound shaders. **Re-run: 27×Δ0, 2×Δ1, zero Δ>1** — the modern `gl.Rect`/`gl.TexRect` path is now pixel-identical (≤1 LSB) to legacy across every real BAR UI call-site, and shader-bound primitives correctly fall back to legacy. (This is the C6 evidence the unit tests couldn't give.)
- 2026-06-30 — **In-engine `LuaGLCompareMode` (closes the wired-shim validation gap).** New config `LuaGLCompareMode` (bool, default off). When on, each wired callin (`gl.Rect`/`gl.TexRect`, no user shader bound) is drawn **both ways** — legacy + modern — into two viewport-sized offscreen FBOs over a common cleared bg, read back, and the **max per-byte delta is logged once per Lua call-site** (`[file]:line`); the real frame still renders normally. Implemented as `LuaGLCompare::CompareDraws(w,h, legacyFn, modernFn)` in `LuaImmediateBuffer.cpp` (saves/restores FBO binding, viewport, clear color) + a dedup logger in `LuaOpenGL.cpp`. The `CompareDraws` helper is unit-tested (confirm: identical draws → Δ0; deny: shifted rect → Δ>0). This is the live-replay vehicle (contract C6) to pixel-validate the wired path over real BAR frames — run a replay with `LuaGLCompareMode=1` and watch the log. `gl.Rect`/`gl.TexRect` refactored to legacy/modern draw lambdas reused for both compare and the normal draw. Engine-headless + engine-legacy compile/link clean; full suite green (bar the 2 env failures). (Still can't be *run* in this sandbox — needs BAR content.)
- 2026-06-30 — **First live-path wiring: `gl.Rect` + `gl.TexRect` dispatch to the modern emitter (gated).** Added config `LuaModernGLBackend` (bool, default **off**). When on (and no user shader is bound — else legacy must feed that shader), `gl.Rect`/`gl.TexRect` draw via `LuaImmediateBuffer` (modern) instead of `glRectf`/`glBegin`; off ⇒ legacy path byte-unchanged. `LuaImmediateBuffer.cpp` added to the engine build (`rts/Lua/CMakeLists.txt`); engine-headless + engine-legacy both compile/link clean. MVP bridge: until Phase 0, the wiring reads the current MVP off the fixed-function stack via `glGetFloatv(GL_PROJECTION/MODELVIEW_MATRIX)` (a query, not a deprecated set-call) so the modern shader gets the engine-scaffolding transform; setting `LuaModernGLBackend` also forces matrix tracking on. **Validation gap (honest):** compile-verified + gate-off-safe + the emitter is unit-validated, but the *wired shim* (Lua args→emitter, glGetFloatv MVP, shader-bound guard) is not yet pixel-validated in a running engine — needs the in-engine `LuaGLCompareMode` or a manual BAR run with the flag on. Deferred: `gl.BeginEnd`/`Vertex`/`Color` wiring (stateful routing + custom-shader interaction).
- 2026-06-30 — **Emitter C3 (GL state side-effects) check.** Added a test asserting the modern flush leaks no transient GL state — after `FlushModern`, `GL_CURRENT_PROGRAM` and `GL_VERTEX_ARRAY_BINDING` are restored to their pre-flush values and `glGetError()` is clean (shader `Enable`/`Disable` and VAO bind/unbind balance). Note: FF-only state the modern path intentionally abandons (`GL_CURRENT_COLOR`, the matrix stack) is an *expected* divergence from legacy, not a leak — C3 here verifies the modern backend restores what it temporarily touches. Emitter now covers (all A/B green on llvmpipe): BeginEnd pos+color, QUADS/QUAD_STRIP/POLYGON triangulation, LINES, sticky color, TexRect (MODULATE), and no-state-leak.
- 2026-06-30 — **Emitter textures: `TexRect` (the dominant textured primitive, 46 BAR files).** Added `LuaImmediateBuffer::SetTexRect` + `FlushTexRect{Legacy,Modern}`: legacy = `glBegin(GL_QUADS)` + `glTexCoord`/`glColor`/`glVertex` (FF MODULATE); modern = `TypedRenderBuffer<VA_TYPE_TC>` (2 tris) + a textured uniform-MVP shader (`outColor = texture(tex,uv) * vcolor`, MODULATE), sampler on unit 0, no FF matrix/enable. Caller binds the texture (as `gl.Texture` does). Test: a 2×2 NEAREST texture, white color, A/B legacy vs modern ≤1 LSB, plus an absolute texel check (lower-left quadrant = red) proving the modern path is actually textured.
- 2026-06-30 — **Emitter primitive coverage: QUADS/QUAD_STRIP/POLYGON triangulation.** `LuaImmediateBuffer::FlushModern` now triangulates the non-core modes (QUADS→2 tris, QUAD_STRIP, POLYGON→fan) since the modern/core path can't draw them; other modes pass through. Tests added: `GL_QUADS` flat fill is **bit-exact** vs legacy (triangle union == FF quad coverage), `GL_POLYGON` flat pentagon and `GL_LINES` (width 1) within ≤1 LSB. Smooth-shaded quads/polys can still differ by the diagonal/fan choice — deferred until needed (flat fills are the common case).
- 2026-06-30 — **Unified `lua_State`-free emitter landed (`LuaImmediateBuffer`).** New `rts/Lua/LuaImmediateBuffer.{h,cpp}`: accumulates a pos+color immediate-mode stream (sticky color), then flushes it either **Legacy** (`glBegin`/`glColor`/`glVertex`, fixed-function matrix) or **Modern** (`TypedRenderBuffer` + its own uniform-mat4-MVP shader, zero FF matrix calls; MVP via `SetMVP`, in-engine from `GLMatrixStateTracker`). Both flushes consume the SAME accumulated stream, so the two paths get byte-identical input by construction — this is the backend-select-by-flag + run-both-to-compare spine the doc's method needs. Test `testGLImmediateEmitter` drives one scene and flushes it both ways into two FBOs: flat quad, smooth-colored triangle, and sticky-color cases all match within ≤1 LSB under llvmpipe. The shader handles the explicit/non-explicit attrib-location cases like the engine's RenderBufferShader. Scope of this iteration: pos+color (VA_TYPE_C) + core primitive modes; **next:** QUADS/POLYGON triangulation, textures (TexRect/`VA_TYPE_TC`), GL-state side-effects (C3), then wire `gl.BeginEnd`/`Rect`/`TexRect` to dispatch to the emitter (flag-gated, legacy fallback). Test CMake refactored to a shared `add_engine_gl_test` macro + engine-source list.
- 2026-06-30 — **Modern backend validated against the real engine geometry path.** Added a case to `testGLRenderBufferCompare`: engine `TypedRenderBuffer<VA_TYPE_C>` geometry + a **uniform-`mat4`-MVP engine shader** (built via the real `shaderHandler`; attribs at explicit locations 0/1 matching `VA_TYPE_C::attributeDefs`, so it consumes the engine VAO directly) that reads ONLY the `uMVP` uniform and **makes zero fixed-function matrix calls** (ignores `gl_ModelViewProjectionMatrix`). A/B vs legacy `glBegin`: ≤1 LSB, quad-color asserted at center. This is the RenderDoc-clean modern draw proven on the engine's real vertex machinery. **Decision: do NOT add the uniform to the shared `RenderBuffers.inl` shader** — `gl_ModelViewProjectionMatrix * transformMatrix * v` would impose a per-vertex `mat4*mat4` on every engine RenderBuffer user (fonts/particles/UI). The modern path gets its OWN shader (zero blast radius); its permanent home is the Lua-immediate backend (next: factor the `lua_State`-free emitter and wire `gl.BeginEnd`/`Rect`/`TexRect`, feeding `uMVP` from the `GLMatrixStateTracker`).
- 2026-06-30 — **Tier-2 A/B against the REAL engine renderer landed** (`test/engine/Rendering/testGLRenderBufferCompare.cpp`, target `testGLRenderBufferCompare`). Renders a flat quad via the actual engine path — `RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>()` + `Shader`/`VAO`/`VBO`/`StreamBuffer` — and A/B-compares vs legacy `glBegin` with the same MVP: **matches within ≤1 LSB**, and the engine buffer is asserted to actually contain the quad color at center (guards against a both-draw-nothing false pass). **This solves "how to unit-test the engine GL path" — the recipe:** (1) put the test in a subdir (`test/engine/Rendering/CMakeLists.txt`) that does `remove_definitions(-DUNIT_TEST)` (engine GL headers `#error` under `UNIT_TEST`); (2) compile with `-DNOT_USING_CREG` (drops `CMatrix44f`'s creg) and `-ffunction-sections -fdata-sections` + link `-Wl,--gc-sections` (strips `Shader.cpp`'s file/Lua shader-loading paths — `CFileHandler`, `LuaMatTexture`, `ParseTextureImage`, `LoadFromLua` — that the inline-shader RenderBuffer path never calls); (3) link the few light real sources the draw path needs (`MemoryOverride`, `StringUtil`, `Config/ConfigVariable`, plus the GL/Shaders `.cpp`s); (4) provide tiny inert stubs in `GLEngineStubs.cpp` for the rest — a zeroed `globalRendering` with `supportExplicitAttribLoc`/`active` set, `configHandler=nullptr` (only used behind a short-circuited `oldValid &&`, never dereferenced first compile), a no-op `glClearErrors`, and the never-executed `CFileHandler` ctor/Close/Read/virtuals (so its vtable links). No VFS, no window system, no config files. SKIPs headless; green under llvmpipe.
- 2026-06-30 — **Tier-2 modern-vs-legacy A/B landed + key shader finding.** Added `test/engine/Rendering/testGLMatrixDrawCompare.cpp` (target `testGLMatrixDrawCompare`) and factored the shared harness into `test/engine/Rendering/GLTestHarness.h` (context/FBO/comparator/legacy helpers; `testGLImmediateCompare` refactored onto it, still green). The new test renders the *same geometry* two ways — legacy fixed-function `glBegin` vs a modern **VAO + uniform-`mat4`-MVP GLSL-150 shader** — both fed the **identical `CMatrix44f` MVP built by the Phase-1 `GLMatrixStateTracker`** (loaded into the FF stack for legacy, set as the uniform for modern, so any delta is purely backend). Results under llvmpipe: **flat axis-aligned quad is bit-exact (maxAbsDelta=0)**; smooth-colored triangle and a tracker-transformed (Translate+Rotate) quad within ≤1 LSB. This exercises the Phase-1 tracker end-to-end on the GPU and proves a RenderDoc-clean draw reproduces legacy output. **IMPORTANT architectural finding:** the engine's stock `TypedRenderBuffer` shader (`RenderBuffers.inl`) transforms via `gl_ModelViewProjectionMatrix` — the deprecated fixed-function builtin, set by exactly the `glMatrixMode`/`glLoadMatrixf` calls we must remove for RenderDoc. So "re-back `gl.BeginEnd` onto `TypedRenderBuffer`" is **not** sufficient as-is; the modern backend needs a **uniform-MVP shader variant fed by the matrix tracker** (which is why this increment validated that path with a minimal hand-rolled VAO+shader rather than the stock TypedRenderBuffer). **Next:** bring up `globalRendering`(+configHandler)+`CFBO`+`TypedRenderBuffer` and add a uniform-MVP shader variant, then A/B the *engine* path; and factor the emitter `lua_State`-free for the LuaOpenGL integration.
- 2026-06-30 — **Tier-2 harness + controls landed** (`test/engine/Rendering/testGLImmediateCompare.cpp`, target `testGLImmediateCompare`). Hidden SDL2 compat context + glad + raw-GL FBO + glReadPixels + the comparator's positive/negative controls (incl. a 1-texel-flip deny case). Green under `xvfb-run`/llvmpipe; `SKIP`s with no display so GL-less CI stays green. SDL2+glad only (no engine globals yet) — the `globalRendering`+`CFBO`+`TypedRenderBuffer` "new backend" A/B cases are the next increment, gated on factoring the emitter `lua_State`-free. See the Tier-2 section for details.
- 2026-06-30 — **Phase 1, iteration 0 (matrix tracker) landed test-first.** Wrote `test/engine/System/testGLMatrixStateTracker.cpp` (Catch2, wired via `add_spring_test` in `test/CMakeLists.txt`, modeled on `testMatrix44fRotation`) covering C1/C5-matrix: post-multiply order/side for `Translate`/`Scale`/`Rotate`/`MultMatrix`, glRotatef-style axis normalization, bit-exact push/pop round-trip + nesting, three independent mode stacks, `Ortho`/`Frustum` vs `CMatrix44f::OrthoProj`/`PerspProj` goldens, and depth over/underflow vs `HasMatrixStateError`. Then extended `GLMatrixStateTracker` (`rts/Rendering/GL/MatrixStateTracker.h`) from depth/mode-only to hold real `CMatrix44f` value stacks per mode (the existing depth counters stay the canonical balance check and are kept in sync). Group-B Lua callouts (`gl.Translate/Scale/Rotate/Ortho/Frustum/LoadIdentity/LoadMatrix/MultMatrix/Billboard`) now drive the tracker, gated behind a new `LuaMatrixTracking` config flag (default **off** ⇒ byte-identical to the legacy FF-only path). To keep Tier-1 truly GL-free (`myGL.h` `#error`s on `UNIT_TEST`), the tracker's value-math compiles without the GL loader under `UNIT_TEST` (3 mode enums defined locally; the GL-calling `HandleMatrixStateError` is `#ifndef UNIT_TEST`). **Known limitations (follow-ups):** (1) the new value stacks are not yet isolated across nested callins / display lists (the `PushMatrixState`/`PopMatrixState` swap only moves the depth counters) — Phase 0 will seed/restore them per callin; (2) `Ortho`/`Frustum` track the plain `OrthoProj`/`PerspProj`, not the engine's clip-space-control variant (`__spring_glOrtho`'s extra translate/scale) — reconcile when the shader path actually consumes the MVP.
- 2026-06-30 — added **Testing & validation** section: Tier 1 = pure Catch2 matrix-tracker unit tests (no GL, like `testMatrix44f.cpp`); Tier 2 = offscreen dual-FBO A/B render test (new infra — engine has no GL-context test today; needs SDL compat context + `globalRendering` cap init + CFBO + Xvfb/llvmpipe). Prereq for Tier 2: factor the immediate emitter `lua_State`-free.
- 2026-06-30 — **first real runtime trace** (GL 4.6 Compat, master `2026.06.06-80-gf898efc`, includes the 5 new hooks — confirmed `Rect`/`TexRect`/`PushAttrib`/`PopAttrib` fire). See Runtime findings below. Headline: **Chili/Chobby/LuaMenu is a whole immediate-mode UI layer the static inventory missed** (menu-scope), and the static matrix undercounted because it never tracked groups B/Vertex/Color.
- 2026-07-01 — **Font renderer uMVP conversion landed + validated.** Both font vertex shaders (`vsFont330`/`vsFont130`, now in `rts/Rendering/Fonts/glFontRendererShaders.h`) gained `uniform mat4 uMVP; uniform bool uUseMVP;` with `gl_Position = (uUseMVP ? uMVP : gl_ModelViewProjectionMatrix) * vec4(pos,1)`; gated by config `FontUseMVPUniform` (default **off** ⇒ byte-identical legacy builtin path). MVP from the `glGetFloatv(PROJECTION)*glGetFloatv(MODELVIEW)` Phase-0 bridge, set per-draw in `PushGLState` (runtime-switchable). Validated two ways: Tier-2 unit test `testGLFontMVPCompare` (uUseMVP=0 vs =1 on a VA_TYPE_TC quad, ≤1 LSB) **and** an in-engine same-frame self-comparator (config `FontShaderMVPCompare`) that renders both branches and logs the delta — result **`[Font MVP compare] uMVP path matches builtin: max byte delta = 0`**. This is the #1 Phase-0 consumer of the FF matrix builtin; still needs the bridge swapped for a real CPU/UBO source to be FF-independent.
- 2026-07-01 — **Whole-frame A/B harness built (`GLFrameABCompare`) — and found a real modern-backend bug the per-call compare missed (contract-C4 class).** Splits `CGame::Draw`, re-renders the visible frame 3× flipping a global `LuaOpenGL::SetModernImmediate` + `CglShaderFontRenderer::SetUseMVPUniform` toggle (legacy/legacy/modern), reads back each via `glReadPixels`, and reports `net = |pass2−pass3| − |pass1−pass2|` (adjacent-pass diff cancels temporal drift; control = pass1↔pass2). On BAR it flagged the **gui_pip PiP minimap rendering ~1.4–2.4× brighter (washed out)** under the modern immediate backend — a state leak onto a *later* draw that per-call `LuaGLCompareMode` cannot see (every individual wrapped draw compared identical; the leak hits the engine/gui_pip minimap render that runs after).
- 2026-07-01 — **RenderDoc is blocked until FF is gone (verified), so debugging uses apitrace.** Engine creates a **compatibility-profile** context (`rts/Rendering/GlobalRendering.cpp:512`, `SDL_GL_CONTEXT_PROFILE_COMPATIBILITY`) because it still uses FF immediate mode + the FF matrix stack; RenderDoc only supports core 3.2+, so it can't capture the current engine — RenderDoc is the *destination* of the migration, not a tool available now. **apitrace does work** (traces compat/FF/immediate mode). Recipe used: prebuilt `apitrace-14.0-Linux` (GitHub release, no root); `apitrace trace -o t.trace -- env … ./spring …`; `apitrace dump --grep/--calls` to find frame/draw boundaries; **`glretrace -D <call> --dump-format=json`** to dump full GL state at a call; compare two dumps' `parameters` blocks (or `apitrace diff-state`). This is the concrete "GL state diff" method for the "identical pixels, different leaked state" bug class.
- 2026-07-01 — **Minimap-wash root-caused via apitrace and FIXED.** apitrace state-dump *at the minimap terrain draw* (`glDrawArraysInstanced` under gui_pip's shader) showed the only meaningful difference: **`GL_CURRENT_COLOR = [1,1,1,0.09]` modern vs `[1,1,1,1]` legacy**. gui_pip's minimap shader (compatibility profile) reads the built-in `gl_Color` for opacity, so the terrain drew at ~9% alpha and blended toward the bright background (lifted blacks / washed). **Mechanism:** gui_pip draws the minimap background quads with `gl.Color(1,1,1,1)` set *outside* the BeginEnd and a body that only emits TexCoord+Vertex — legacy `glBegin/glEnd` inherits that current color, but the modern capture reused `LuaImmediateBuffer`'s **stale `curColor`** (0.09 from a prior BeginEnd) because `Begin()` never seeded it. **Fix (3 legacy-parity changes):** (1) `LuaOpenGL::BeginEnd` seeds the capture's current color from the inherited `color[]` before running the callback; (2) `FlushModern` leaves FF current color = last vertex color (like legacy's per-vertex `glColor`); (3) `FlushTexRectModern` calls `glColor4ub(texRect.c)` matching `FlushTexRectLegacy`. **Validated by clean single-run screenshot** (harness-independent): minimap brightness ratio 1.4–2.4× → **1.000**. Earlier failed hypotheses (all tested + rejected before apitrace): stray non-vertex calls in the body, nested BeginEnd, FF-current-color-sync-on-gl.Color, FlushModern-last-vertex — none worked because the color was inherited from *outside* the body, invisible to all of them.
- 2026-07-01 — **The `GLFrameABCompare` harness itself is unreliable for whole-frame validation — its modern re-render pass corrupts the UI.** Comparing the harness's saved passes: legacy pass renders a perfect frame, but the **modern pass draws a full-screen gray wash with the command panel/build-icons missing** — yet a **plain modern render (`LuaModernGLBackend=1`, no harness) is completely correct** (full UI, correct minimap, crisp text). So the breakage is the harness flipping `SetModernImmediate` **mid-frame during a re-render** (pass 3), not the modern backend; its `net` numbers therefore overstate differences and its "residuals" (the ~8–30k net, a ~10% brightness lift on the "Game starting…" countdown text) are largely **harness artifacts**, not real modern-vs-legacy deltas. **Implication:** trust clean single-run screenshots + per-call `LuaGLCompareMode` (both harness-independent, both green) — not the whole-frame `net` — until the harness re-render is fixed. **In progress: root-causing why the mid-frame modern toggle + re-render stretches the UI panel background to full-screen** (likely screen-matrix / `SetupScreenMatrices` state not re-established for the extra passes).
- 2026-07-03 — **A/B harness restructured: in-frame 3-pass [L,L,M] (one Update, three renders).** Replaces the paired-consecutive-frames design wholesale. Each iteration now runs sim + `UpdateUnsynced` ONCE, then renders the visible frame three times — legacy (reference, presented), legacy (same-frame control), modern (test) — and compares in-frame. Draw-prep (drawFrame, timeOffset, camera, shadow fit, cull flags, particle draw-pos bake, per-draw UBO, buffer uploads) is identical for all passes **by construction**, killing the cross-iteration pin whack-a-mole (the draw-radius leak, camera-frustum pin, and `newSimFrame==false` alternate-path bugs are structurally impossible now; those pins are deleted). Sim runs FULL speed (no `abSuppressSim`/half-fps). Retained pins: exactly two — draw clock (`PinDrawTime`/os.clock) and unsynced RNGs (guRNG + Lua math.random), snapshot before pass 0, replayed before passes 1–2. Per-pass work is **from scratch** (own clear, DrawGenesis, **shadow-map render** — now backend-compared too — world + screen); once-per-iteration exceptions: `minimap->Update()` (wall-clock-throttled cache) and the reflection-cubemap round-robin + consume-once sky/shading updates. The same-frame L↔L control builds a per-frame noise mask AND is itself the pixel-perfect gate: any nonzero control px is a real per-pass leak with an in-frame repro. `AB_FORCE_LEGACY` ⇒ [L,L,L] null test. Also fixed a regression: `SetABCompareActive` was never wired (lost in the 2026-07-03 checkout accident) — `Spring.GetABCompareActive()` and the BAR CAS/bloom/diaglines guards were dead; now set from `CGame::Draw` after a new `GLFrameABCompareWarmup` (default 120 frames, lets accumulators build state). New Lua API: `Spring.GetABPassIndex()` (0/1/2); `Spring.GetABDuplicatePass()` redefined as passIndex>0. BAR-side: `api_screencopy_manager.lua` dedup now keys on (drawFrame, passIndex) so each pass re-copies its own backbuffer (else the modern pass composites legacy pixels through CAS/DoF/glass); `gfx_ssao.lua` SLOWFUSE periodic fuse frozen while compare active. Engine ordering change in the normal frame (compare off): DrawGenesis now runs after minimap/IBL updates, and the shadow map renders after DrawGenesis. All three engine targets compile+link. **Status: NOT yet runtime-validated — next is the [L,L,L] null gate on fightertest (control must be 0), then [L,L,M].**
- 2026-07-04 — **Modern MVP now comes from a CPU-side FF matrix mirror (`GL::ffMirror`, commit `487d2c5fd3`) — the Phase-0 prerequisite step.** The per-callin scaffolding seeds it with exact values (incl. the `__spring_glOrtho` clip-space composition via new `ComposeSpringOrtho/Frustum`; `gl.Ortho`/`gl.Frustum` tracking fixed to match — closes the tracker's documented plain-OrthoProj limitation), every `gl.*` matrix callout replays into it (incl. `gl.PushPopMatrix`, previously untracked, and `gl.RenderToTexture`'s identity push/pop), `gl.CreateList` compiles skip it, and `gl.CallList` taints it (consumers fall back to the `glGetFloatv` bridge until the next reseed). Under the A/B gate every mirror read is shadow-verified against `glGetFloatv`. **Validated: gate byte-perfect with the mirror live (control 0/165, signal 0/165) and ZERO shadow-compare divergences** — CPU matrix composition is pixel-compatible with the driver's FF stack. The bridge is now only the taint-fallback + shadow reference; the font's uMVP still uses the bridge (fonts also fire from unmirrored engine paths — migrates with the UBO phase). Next per the plan: coverage breadth, then the shared RenderBuffer/font uniform-MVP (UBO) conversion fed from the mirror, then removing the FF matrix set-calls.
- 2026-07-03 — **BOTH GATES GREEN, BYTE-PERFECT: the whole-frame compare now reports control=0 AND modern-vs-legacy signal=0 on every in-game frame of the fightertest mass-combat benchmark.** Three iterations got there: **(1) First 3-pass null run**: calm frames 0 but combat median control ~500 px with 10k–45k spikes. apitrace of the run (the per-pass `glReadPixels` are perfect pass boundary markers; segment + histogram draws per pass) showed EVERY recurring leak was first-render-of-the-drawFrame lazy work — widget dlist rebuilds (which shift the font stream-VBO aliasing recorded inside lists), chat's run-twice RTT workaround, a one-time `fxShader` `SMOOTH_PARTICLES` `SetFlag`-after-`Enable` relink landing mid-pass — and that passes 1↔2 were already draw-identical everywhere. **(2) Restructured to 4 passes [settle, L, L, M]** (commit `a819b62b17`): pass 0 is an uncompared settle pass absorbing all once-per-drawFrame lazy work (and is what's presented); control = p1↔p2, signal = p2↔p3; dumps now save the control pair when it leaks. Result: control 0 on 70% of combat frames, residual ~100–330 px = the fightertest `dbg_benchmark.lua` overlay's smoothed ms digits advancing per pass (guarded on `GetABDuplicatePass`, also fixes its stats quadruple-counting) → **[L,L,L,L] null gate: 0/0 on all 153 in-game frames**. **(3) First real modern-backend catch**: [L,L,L,M] showed 6 frames (2.2k–8.6k px) where the modern pass DROPPED `gui_messages`' typewriter event text ("Battle Started/Enemy Units Detected") — `font:Print` compiled into `gl.CreateList` bodies breaks under `FontUseMVPUniform` because `glUseProgram`/`glUniform*` execute immediately during list compile and are NOT recorded, so the replayed glyph draw uses stale shader state (text transforms by another draw's uMVP → vanishes); plus the latent stream-VBO offset-aliasing fragility that affects the legacy path too. **Fix (commit `1e74b7a554`)**: `CglShaderFontRenderer` detects `GL_LIST_INDEX != 0` and emits the accumulated glyph quads as recordable fixed-function immediate mode (texel→UV via a shared texture-space-matrix dlist recompiled on atlas resize, the `CglNoShaderFontRenderer` trick) → **[L,L,L,M]: control 0/160, signal 0/160**. The modern gl.Rect/TexRect + font-uMVP backend is pixel-identical to legacy over a full mass-combat run; the whole-frame gate is trustworthy and CHEAP to re-run (`GLFrameABCompare=1`, fightertest, ~2 min). BAR-side uncommitted: screencopy per-pass key, ssao SLOWFUSE guard, dbg_benchmark pass guard + the 3 existing freeze guards.
- 2026-07-01 — **Modern `gl.BeginEnd` status:** the capture path (FF-free for non-textured geometry — renders through the `uMVP` shader) is correct with the 3 fixes above and validated in a plain render; it is currently behind a **temporary `MODERN_BEGINEND_CAPTURE` env gate** while the harness is untrustworthy, with the default being deferral to legacy `glBegin/glEnd`. Promotion to default is pending the harness re-render fix (so whole-frame `net` can confirm no regressions). `gl.Rect`/`gl.TexRect` remain modern.
- (add dated entries as we learn more)

## TL;DR — scope & sequencing

The work splits into one centralized engine core plus a small per-widget tail. Order:

- **Phase 0 — per-callin scaffolding (the gate, content-independent).** LuaOpenGL wraps *every* draw callin in fixed-function setup that fires regardless of what the widget draws. Until this is modern, no BAR frame is capturable even if every widget is clean. Centralized in ~10 functions in `rts/Lua/LuaOpenGL.cpp`. **Do this first.**
- **Phase 1 — primitives + matrix machinery (A + B).** Re-back `gl.BeginEnd`/`Shape`/`Vertex`/`Rect`/`TexRect` onto `GL::TypedRenderBuffer`; add CPU-side matrix tracking so a shader gets the MVP. Shares the matrix code with Phase 0.
- **Phase 2 — display lists (E).** Re-back `gl.CreateList`/`CallList` onto an engine-managed retained VBO, same Lua API. Largest file footprint, but migrates with zero widget changes if done engine-side.
- **Phase 3 — residuals.** Per-widget: alpha-test→shader-discard, `gl.LineStipple` (dashed lines), `gl.PushAttrib`/`PopAttrib`, `gl.PointSprite`. Concrete file lists below.
- **Phase 4 — engine C++.** SMF void-edge `GL_ALPHA_TEST` (void maps only).

Group **D (fixed-function lighting) is not used by BAR at all — skip it.**

The big lever: Phase 0 + Phase 1 are centralized in `LuaOpenGL.cpp`. Done engine-side behind the unchanged `gl.*` API, the 110 widget files below mostly come along for free; the genuine per-file work is only the Phase 3 residuals.

## Working method: test-forward, oracle-backed

This work is unusually well-suited to a test-first workflow because **the legacy immediate-mode path is a perfect oracle** — it stays behind a config flag, so for any input we can render/compute both ways and assert equality. We exploit that to make "compatible" a *measurable gate*, not a judgment call. Rule: **no change is committed unless the full test suite is green**, and the legacy path is not deleted until the entire contract below is green over both unit scenes and a replay bake.

### Compatibility contract (the definition of "the same")

Every migrated primitive must preserve all of these. Each is a test type, each must be green to call a phase done:

| # | Dimension | "Same" means | Test type |
|---|---|---|---|
| C1 | **Transform values** | tracked `CMatrix44f` == GL FF matrix, bit-exact | Tier-1 unit + shadow assert |
| C2 | **Pixel output** | framebuffer identical (bit-exact where expected; bounded+investigated ±1 LSB for rotated-quad/interp) | Tier-2 dual-FBO A/B |
| C3 | **GL state side-effects** | after a callin, observable state (current color, active texture, blend/enable bits, matrix mode + stack depth) matches legacy | state-snapshot test |
| C4 | **Draw order / compositing** | translucent overlaps and rect↔text interleave composite identically within a frame | Tier-2 order cases + live replay compare |
| C5 | **Error / edge behavior** | empty/degenerate prims, Lua error mid-`BeginEnd`, stack over/underflow → same outcome, no crash, balanced stack | unit + state test |
| C6 | **Coverage** | every call site seen in the `DeprecatedGLWarnLevel=2` trace is exercised by a unit scene or the live replay compare | coverage checklist vs trace |

### The loop (per increment = one matrix-op group or one primitive)

1. **Write the test first**, from the contract. It is red (assertion fails) or, if the new backend for that primitive doesn't exist yet, explicitly **pending/SKIP** — never silently absent.
2. Implement the minimal code (or flip the per-primitive flag).
3. Test goes green (new ≡ legacy).
4. **Run the full suite** — all prior tests still green = no regression.
5. **Commit gate:** commit only when the whole suite is green.

### Validate the harness before trusting it

Before the A/B comparator is allowed to gate anything, prove it can both confirm *and* deny:
- **Positive control:** render the same primitive twice via the legacy path → must be byte-identical (proves readback/determinism).
- **Negative control:** feed the comparator two deliberately-different renders → must FAIL (proves it isn't a no-op that always passes).

A comparator that can't fail gives false confidence; these two controls are themselves committed tests.

### Per-phase done-gates

- **Phase 1 (matrix):** Tier-1 suite green (C1, C5-matrix) — pure unit, no GL, authored before the tracker code.
- **Phase 0 (scaffolding) + Phase 1 (primitives A):** Tier-2 A/B suite green for all primitive cases (C2, C4); state-snapshot tests green (C3); harness controls green.
- **Phase 2 (display lists), Phase 3 (residuals):** their own A/B + state cases green.
- **Whole-program gate (before deleting legacy):** C6 satisfied — `LuaGLCompareMode` over a representative replay reports zero pixel deltas across the full `DeprecatedGLWarnLevel` caller set, plus a clean RenderDoc capture.

---

## The migration-unit model

A re-backed `gl.BeginEnd` draws with a **shader**, which needs as uniforms/inputs everything the fixed-function pipeline supplied implicitly. So the geometry functions and the state functions that feed them are one coupled unit.

| Group | Lua API | Underlying GL | Becomes | BAR uses? |
|---|---|---|---|---|
| **A. Geometry** | `BeginEnd`, `Shape`, `Vertex`, `Normal`, `TexCoord`, `MultiTexCoord`, `Color`, `SecondaryColor`, `FogCoord`, `EdgeFlag`, `Rect`, `TexRect` | `glBegin`/`glEnd`, `glVertex*`, `glColor*`, `glTexCoord*`, `glNormal*`, `glRectf` | `TypedRenderBuffer.AddVertex` + `DrawArrays(mode)` | yes (heavy) |
| **B. Transform** | `MatrixMode`, `Push/PopMatrix`, `LoadIdentity`, `LoadMatrix`, `MultMatrix`, `Translate`, `Rotate`, `Scale`, `Ortho`, `Frustum` | `glMatrixMode`, `glPush/PopMatrix`, `glLoadMatrixf`, `glMultMatrixf`, `glTranslatef`, `glRotatef`, `glScalef`, `glOrtho`, `glFrustum` | CPU-tracked `CMatrix44f` stack → shader MVP uniform | yes (heavy) |
| **C. Fragment/raster state** | `TexEnv`, `AlphaTest`, `Fog`, `ShadeModel`, `PolygonMode`, `LineWidth`, `LineStipple`, `PointSize`, `ClipPlane` | `glTexEnv*`, `glAlphaFunc`, `glShadeModel`, `glPolygonMode`, `glLineWidth`, `glLineStipple`, `glPointSize`, `glClipPlane` | shader flags/uniforms; some are core-valid no-ops | partial (see below) |
| **D. Lighting** | `Light`, `LightModel`, `Material`, `Lighting` | `glLight*`, `glMaterial*`, `glLightModeli` | FF lighting in shader | **NO — 0 files** |
| **E. Display lists** | `CreateList`, `CallList`, `RunList`, `DeleteList` | `glGenLists`/`glNewList`/`glCallList` | engine-managed retained VBO behind same API | yes (heavy) |

### Group C detail (what BAR actually uses)

Confirmed BAR usage of the C/residual functions (static grep):

| Function | Files | RenderDoc blocker? | Action |
|---|--:|---|---|
| `gl.AlphaTest` | 8 | **Yes** (`glAlphaFunc`/`GL_ALPHA_TEST` removed) | shader `discard` |
| `gl.LineStipple` | 6 | **Yes** (removed in core) | shader dashed-line (1D tex or `gl_FragCoord` discard) |
| `gl.PushAttrib`/`PopAttrib` | 3 | **Yes** (removed in core) | explicit state save/restore |
| `gl.PointSprite` | 1 | **Yes** (implicit in core) | drop enable, use core point sprites |
| `gl.LineWidth` | 33 | No — core entry point | none (thick lines may not render in strict core, but capture is fine) |
| `gl.PolygonOffset` | 12 | No — core | none |
| `gl.PolygonMode` | 6 | No — core | none |
| `gl.PointSize` | 2 | No — core | none |
| `gl.TexEnv`, `gl.Fog`, `gl.ClipPlane`, `gl.ShadeModel`, `gl.Lighting`, `gl.LogicOp`, `gl.PointParameters` | 0 | — | none |

---

## Phase 0 — the per-callin scaffolding (the real gate)

Every Lua draw callin is wrapped by `EnableCommon`/`Reset*` in `rts/Lua/LuaOpenGL.cpp`, which unconditionally emits RenderDoc-rejected calls **regardless of widget content**. A widget drawing only text (already on the modern shader font renderer) still triggers all of this every frame:

- `EnableCommon` (`:599`) → `glPushAttrib(AttribBits)` (`:604`) … `glPopAttrib()` (`:620`).
- `ResetGLState()` (`:497`) → `glAlphaFunc`, `glShadeModel`, `glLightModeli`, `glTexEnvi(GL_MODULATE)` (`:543`), `glLogicOp`, `glDisable(GL_LIGHTING / GL_TEXTURE_GEN_*)`.
- `SetupScreenMatrices()` (`:980`) → `glLightModeli`, `glMatrixMode`, `glLoadMatrixf`. `RevertScreenMatrices()` → `glMatrixMode`/`glLoadIdentity`/`gluOrtho2D`. Same for the `EnableDrawWorld*`/`Shadow`/`Reflection`/`Refraction` matrix setups.

This is why Phase 0 is mandatory and first. The fix (all three are gated on the FF content being gone — see the finding below):

- **Attrib-stack swap** — replace `EnableCommon`'s `glPushAttrib(AttribBits)` / `DisableCommon`'s `glPopAttrib()` with explicit modern state save/restore (engine state abstraction in `rts/Rendering/GL/State.h`, which is RAII `GL::SubState`, not a snapshot API). **Finding (2026-06-30): this is NOT a content-independent win.** `AttribBits` (COLOR_BUFFER|DEPTH_BUFFER|ENABLE|LIGHTING|LINE|POINT|POLYGON|VIEWPORT) and what `ResetGLState` changes is mostly **fixed-function** state (`glAlphaFunc`, `glShadeModel`, `glLogicOp`, `glTexEnvi(MODULATE)`, `glDisable(GL_LIGHTING)`, texgen, line stipple, clip planes, point sprite/params, current color, material). Restoring that without `glPopAttrib` means re-emitting those same deprecated FF calls — so swapping the attrib stack removes `glPushAttrib`/`glPopAttrib` but adds back `glAlphaFunc`/`glShadeModel`/… for the restore: **net zero RenderDoc benefit while any frame content still needs FF state**. So this only pays off once `ResetGLState`'s FF resets are gone (next bullet) and no widget needs FF state restored. Do it together with — not before — the FF-reset removal.
- Turn `ResetGLState`'s FF resets into shader-state defaults / no-ops (depends on no widget relying on FF state).
- Route `SetupScreenMatrices` and the world/shadow matrix setups through the CPU-tracked matrix system (shared with Phase 1 / group B) — **blocked by the uniform/UBO-MVP conversion below**.

> ### ⚠ TODO — Phase 0 prerequisite: uniform/UBO MVP shader conversion (the actual blocker)
> Phase 0 is the **final** gate, not an incremental step: removing the FF matrix scaffolding (`SetupScreenMatrices`'s `glMatrixMode`/`glLoadMatrixf`) is impossible while **any shader in the frame still reads `gl_ModelViewProjectionMatrix`** — they'd all transform by identity. Confirmed consumers (2026-06-30):
> - **The font renderer** — `rts/Rendering/Fonts/glFontRenderer.cpp:30,90` (`gl_Position = gl_ModelViewProjectionMatrix * …`, already tagged `// TODO: move to UBO`). BAR draws *all* text through this, so it fires every frame.
> - **The stock `TypedRenderBuffer` shader** — `rts/Rendering/GL/RenderBuffers.inl` (same builtin); used engine-wide.
> - **Our `LuaImmediateBuffer` modern path already avoids this** (uniform `uMVP`), but it currently sources that MVP via `glGetFloatv(GL_*_MATRIX)` — a bridge that only works *because* the FF matrix is still set by the scaffolding.
>
> **Prerequisite work, in order:** (1) convert the font renderer + the shared RenderBuffer shader to a uniform/UBO MVP (a global UBO updated once per view is cleanest, so per-callin code needn't set a uniform); (2) feed that UBO from the matrix tracker (Phase 1) instead of `glGetFloatv`; (3) only *then* can `SetupScreenMatrices`/`ResetGLState`/`glPushAttrib` be removed. Until (1)+(2), Phase 0 stays blocked and the modern primitive path keeps using the `glGetFloatv` bridge. The `glPushAttrib`/`glPopAttrib` attrib-stack replacement (via `State.h`) is the one Phase-0 piece that *is* content-independent and could land earlier.

Chokepoint functions to modernize (all in `LuaOpenGL.cpp`): `ResetGLState` (`:497`), `EnableCommon` (`:599`), `ResetDrawGenesis` (`:652`), `EnableDrawWorld` (`:666`)/`ResetDrawWorld` (`:682`), `EnableDrawWorldPreUnit` (`:696`), `EnableDrawWorldShadow` (`:726`), `EnableDrawWorldReflection` (`:768`), `EnableDrawWorldRefraction` (`:798`), `EnableDrawScreenCommon` (`:827`), `SetupScreenMatrices`/`RevertScreenMatrices` (`:980`).

---

## The matrix-tracking gap (infra audit)

**Exists and reusable:**
- `GL::TypedRenderBuffer<VertType>` (`rts/Rendering/GL/RenderBuffers.h`) — VAO/VBO/IBO accumulator (`AddVertex`/`DrawArrays`/`DrawElements`, quad-index helpers). The draw target.
- `CMatrix44f` — full translate/rotate/scale/ortho/frustum/multiply math already in C++.
- `LuaOpenGLUtils::GetNamedMatrix` + `gl.GetMatrixData` — Lua can already read known engine matrices.
- `GLMatrixStateTracker` (`rts/Rendering/GL/MatrixStateTracker.h`), stored per-Lua-context (`LuaContextData.h:118 glMatrixTracker`) — already hooks `MatrixMode`/`Push/PopMatrix`, tracks mode + stack depth, validates balance.
- `rts/Rendering/GL/State.h` — modern GL state abstraction (for Phase 0 attrib save/restore).

**The gap (the one enabling change):**
> `GLMatrixStateTracker` tracks stack **depth and mode only**, not matrix **values**. The actual modelview/projection/texture values live solely in the GL fixed-function stack. `gl.Translate` is literally `glTranslatef(...)` (`:5150`) with no CPU mirror; `gl.PushMatrix` bumps a depth counter then `glPushMatrix()` (`:5582`).

So a re-backed `gl.BeginEnd` has no CPU-side MVP for its shader. **Fix:** extend `GLMatrixStateTracker` to hold real `CMatrix44f` stacks for the three modes (it already models the three modes + their stacks; today they're `int` depths where they need `std::stack<CMatrix44f>`), and have group-B functions mutate those stacks using existing `CMatrix44f` math. On flush, read `tracker.GetMatrixState()` → compose proj·modelview → upload as the shader uniform.

---

## Residuals — concrete per-file lists (Phase 3)

**`gl.AlphaTest` (8) → shader discard:**
- `luarules/gadgets/gfx_energy_explosion_particles_gl4.lua`
- `luarules/gadgets/gfx_nano_particles_gl4.lua`
- `luarules/gadgets/gui_display_dps.lua`
- `luarules/gadgets/mo_battle_royale.lua`
- `luaui/Widgets/gfx_airjets_gl4.lua`
- `luaui/Widgets/gui_com_nametags.lua`
- `luaui/Widgets/gui_pip.lua`
- `luaui/Widgets/gui_rank_icons_gl4.lua`

**`gl.LineStipple` (6) → shader dashed-line:**
- `luarules/gadgets/unit_target_on_the_move.lua`
- `luaui/Widgets/cmd_customformations2.lua`
- `luaui/Widgets/gui_pip.lua`
- `luaui/Widgets/gui_pregame_build.lua`
- `luaui/Widgets/gui_selectionbox.lua`
- `luaui/Widgets/unit_waypoint_dragger_2.lua`

**`gl.PushAttrib`/`PopAttrib` (3) → explicit state save/restore:**
- `luarules/gadgets/unit_target_on_the_move.lua`
- `luaui/barwidgets.lua`
- `luaui/Widgets/cmd_customformations2.lua`

**`gl.PointSprite` (1):**
- `luaui/Widgets/gfx_snow.lua`

---

## Engine-side (Recoil) inventory

### BAR triggers these (must change for #1)

| File | Site | Call | Notes |
|---|---|---|---|
| `rts/Lua/LuaOpenGL.cpp` | scaffolding (Phase 0) | attrib stack, matrix, alpha/texenv/shademodel | Fires every callin. See Phase 0. |
| `rts/Lua/LuaOpenGL.cpp` | `BeginEnd` 2389, `Shape` 2337, `Rect` 2822, `TexRect` 2877, `DrawGroundQuad` 2189 | `glBegin`/`glRectf` | Group A. |
| `rts/Lua/LuaOpenGL.cpp` | matrix family (`Translate` 5150, `PushMatrix` 5582, `LoadMatrix` 5488, `MultMatrix` 5547, `MatrixMode` 5411, …) | `glTranslatef`/`glLoadMatrixf`/… | Group B. |
| `rts/Lua/LuaOpenGL.cpp` | display-list API | `glGenLists`/`glNewList`/`glCallList` | Group E. |
| `rts/Map/SMF/SMFGroundDrawer.cpp` | 241-242, 290-291 | `GL_ALPHA_TEST` + `glAlphaFunc` | Void maps only (`voidAlphaMin`). Shader discard. Phase 4. |

### Inert under BAR (no action for #1) — confirmed by `springconfig.lua`

`GrassDetail=0`, `ForceDisableShaders=0`, `LuaShaders=1`, BumpWater, GL4 materials.

| File | Why inert |
|---|---|
| `rts/Rendering/Env/GrassDrawer.cpp` | `GrassDetail=0` → early-return before list creation; BAR uses `map_grass_gl4.lua`. |
| `rts/Rendering/Units/UnitDrawer.cpp` 301/325, `Features/FeatureDrawer.cpp` 157/163 | `if(preList!=0)`; BAR materials never install lists. |
| `rts/Rendering/Models/LocalModelPiece.cpp` 294 | Legacy no-shader model path; BAR is GL4/GLSL. |
| `rts/Rendering/Fonts/glFontRenderer.cpp` 280-380 | No-shader renderer only on AMD-hack/fallback. |
| `rts/Rendering/Env/DynWater.cpp` 681/748/1002 | `Water=2`; BAR uses BumpWater. |
| `rts/Rendering/Env/Particles/ProjectileDrawer.cpp` 1023 | `glAlphaFunc` commented out; uses shader `alphaCtrl`. |
| `rts/Rendering/Common/ModelDrawer*.{h,cpp}` | Behind `IsLegacy()`. |
| `rts/Game/UI/GuiHandler.cpp` (~40), `rts/aGui/*`, `TooltipConsole.cpp`, `GameControllerTextInput.cpp`, `Game.cpp` 2083 | Engine default UI / menus / chat / loading bar; BAR replaces or not in-frame. |
| Debug drawers (QTPFS/HAPFS/ROAM/SmoothHeightMesh/GeometryBuffer/Combiner), `HUDDrawer.cpp` | Debug toggles / FPS-control only. |

---

## BAR-side master work-list (per-file primitive matrix)

Columns: `BE`=BeginEnd, `Sh`=Shape, `Rc`=Rect, `TR`=TexRect, `CrL`=CreateList, `CaL`=CallList, `FUI`=WG.FlowUI.Draw consumer. Sorted by total. A file with only `FUI`>0 migrates for free once the engine path is modern. Static counts (BAR branch `bruno/init-nano-particles-during-load`).

Aggregate file counts: `gl.BeginEnd` 41 · `gl.Shape` 4 · `gl.Rect` 25 · `gl.TexRect` 46 · `gl.CreateList` 62 · `gl.CallList` 60 · `WG.FlowUI.Draw` 40. **110 unique files.** Text (`gl.Text`, 128 callers) already modern — excluded.

### Keystone

| File | BE | Sh | Rc | TR | CrL | CaL | FUI |
|---|--:|--:|--:|--:|--:|--:|--:|
| `luaui/Widgets/gui_flowui.lua` | 11 | 1 | 0 | 2 | 1 | 0 | 70 |

### High weight

| File | BE | Sh | Rc | TR | CrL | CaL | FUI |
|---|--:|--:|--:|--:|--:|--:|--:|
| `luaintro/Addons/main.lua` (loading screen, not in-game) | 14 | 0 | 11 | 12 | 0 | 1 | 0 |
| `luaui/Widgets/gui_pip.lua` | 4 | 0 | 0 | 1 | 18 | 4 | 3 |
| `luaui/Widgets/gui_options.lua` | 0 | 0 | 2 | 1 | 8 | 1 | 8 |
| `luaui/Widgets/gui_top_bar.lua` | 1 | 0 | 1 | 2 | 1 | 1 | 13 |
| `luaui/Widgets/gui_buildbar.lua` | 2 | 0 | 0 | 1 | 8 | 2 | 4 |
| `luaui/Widgets/gui_gridmenu.lua` | 1 | 0 | 0 | 1 | 4 | 1 | 8 |
| `luaui/Widgets/camera_player_tv.lua` | 0 | 0 | 0 | 0 | 7 | 6 | 1 |
| `luaui/Widgets/widget_selector.lua` | 0 | 0 | 2 | 0 | 5 | 3 | 3 |
| `luaui/Widgets/gui_buildmenu.lua` | 0 | 0 | 0 | 1 | 2 | 2 | 7 |
| `luaui/Widgets/gui_chat.lua` | 0 | 0 | 4 | 0 | 1 | 1 | 4 |
| `luarules/gadgets/unit_icongenerator.lua` (icon bake) | 0 | 1 | 0 | 9 | 0 | 0 | 0 |
| `luarules/gadgets/cmd_get_player_data.lua` | 0 | 0 | 3 | 4 | 1 | 2 | 0 |
| `luaui/Widgets/gui_pregameui_draft.lua` | 1 | 0 | 1 | 0 | 3 | 1 | 3 |
| `luaui/Widgets/gui_ordermenu.lua` | 1 | 0 | 1 | 1 | 1 | 0 | 5 |
| `luaui/Include/AtlasOnDemand.lua` (shared text/atlas helper) | 0 | 0 | 1 | 8 | 0 | 0 | 0 |

### Medium weight

| File | BE | Sh | Rc | TR | CrL | CaL | FUI |
|---|--:|--:|--:|--:|--:|--:|--:|
| `luaui/Widgets/gui_mission_info.lua` | 0 | 0 | 0 | 0 | 5 | 1 | 3 |
| `luaui/Widgets/gui_changelog_info.lua` | 0 | 0 | 0 | 0 | 5 | 1 | 3 |
| `luaui/Widgets/gui_scavenger_info.lua` | 0 | 0 | 0 | 0 | 4 | 1 | 3 |
| `luaui/Widgets/gui_keybind_info.lua` | 0 | 0 | 0 | 1 | 4 | 1 | 2 |
| `luaui/Widgets/gui_gameinfo.lua` | 0 | 0 | 0 | 0 | 4 | 1 | 3 |
| `luaui/Widgets/map_startbox.lua` | 1 | 0 | 0 | 1 | 3 | 2 | 0 |
| `luaui/Widgets/gui_unit_stats.lua` | 0 | 0 | 0 | 0 | 3 | 1 | 3 |
| `luaui/Widgets/gui_rejoinprogress.lua` | 1 | 0 | 0 | 0 | 2 | 1 | 3 |
| `luaui/Widgets/gui_advplayerslist_music_new.lua` | 0 | 0 | 0 | 1 | 1 | 1 | 4 |
| `luaui/Widgets/gui_advplayerslist.lua` | 1 | 0 | 1 | 0 | 1 | 1 | 3 |
| `luaui/Widgets/gui_vote_interface.lua` | 0 | 0 | 0 | 0 | 2 | 1 | 3 |
| `luaui/Widgets/gui_spectator_hud.lua` | 0 | 0 | 1 | 0 | 2 | 2 | 1 |
| `luaui/Widgets/gui_selectionbox.lua` | 2 | 0 | 3 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_pregameui.lua` | 0 | 0 | 0 | 0 | 2 | 2 | 2 |
| `luaui/Widgets/gui_ecostats.lua` | 0 | 0 | 0 | 1 | 2 | 1 | 2 |
| `luarules/gadgets/dbg_gadget_profiler.lua` (debug) | 0 | 0 | 6 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/map_start_position_suggestions.lua` | 1 | 0 | 0 | 0 | 2 | 2 | 0 |
| `luaui/Widgets/gui_scavStatsPanel.lua` | 0 | 0 | 0 | 1 | 2 | 2 | 0 |
| `luaui/Widgets/gui_replaybuttons.lua` | 0 | 0 | 0 | 0 | 2 | 1 | 2 |
| `luaui/Widgets/gui_raptorStatsPanel.lua` | 0 | 0 | 0 | 1 | 2 | 2 | 0 |
| `luaui/Widgets/gui_info.lua` | 1 | 0 | 0 | 1 | 0 | 0 | 3 |
| `luaui/Widgets/gui_converter_usage.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 2 |
| `luaui/Widgets/gui_awards.lua` | 0 | 0 | 0 | 1 | 2 | 1 | 1 |
| `luaui/Widgets/cmd_factoryqmanager.lua` | 0 | 0 | 3 | 0 | 0 | 0 | 2 |
| `luarules/gadgets/gui_display_dps.lua` | 0 | 0 | 0 | 0 | 4 | 1 | 0 |
| `luaui/Widgets/gui_teamstats.lua` | 0 | 0 | 0 | 0 | 1 | 1 | 2 |
| `luaui/Widgets/gui_reclaim_field_highlight.lua` | 1 | 0 | 0 | 1 | 1 | 1 | 0 |
| `luaui/Widgets/gui_advplayerslist_unittotals.lua` | 0 | 0 | 0 | 0 | 1 | 1 | 2 |
| `luaui/Widgets/gui_advplayerslist_gameinfo.lua` | 0 | 0 | 0 | 0 | 1 | 1 | 2 |
| `luaui/Widgets/dbg_jitter_timer.lua` (debug) | 0 | 0 | 4 | 0 | 0 | 0 | 0 |
| `luaui/RmlWidgets/gui_quick_start/gui_quick_start.lua` | 1 | 0 | 0 | 0 | 2 | 1 | 0 |
| `luarules/gadgets/unit_respawning.lua` | 0 | 0 | 0 | 1 | 2 | 1 | 0 |
| `luarules/gadgets/unit_evolution.lua` | 0 | 0 | 0 | 1 | 2 | 1 | 0 |
| `luarules/gadgets/gfx_unit_shield_effects.lua` | 1 | 0 | 0 | 0 | 2 | 1 | 0 |

### FlowUI-only (migrate with FlowUI backend; no direct primitives)

`gui_unitgroups.lua` (3), `gui_idle_builders.lua` (3), `gui_factionpicker.lua` (3), `gui_tooltip.lua` (2, +1 CaL), `gui_minimap.lua` (2).

### Low weight (1–2 deprecated calls each)

| File | BE | Sh | Rc | TR | CrL | CaL | FUI |
|---|--:|--:|--:|--:|--:|--:|--:|
| `luaui/Widgets/gui_selfd_icons.lua` | 0 | 0 | 0 | 1 | 2 | 0 | 0 |
| `luaui/Widgets/gui_projectile_target_aoe.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_mouse_fx.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_mapinfo.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_infolos.lua` | 0 | 0 | 0 | 3 | 0 | 0 | 0 |
| `luaui/Widgets/gui_given_units.lua` | 0 | 0 | 0 | 1 | 2 | 0 | 0 |
| `luaui/Widgets/gui_com_nametags.lua` | 0 | 0 | 0 | 1 | 1 | 1 | 0 |
| `luaui/Widgets/gui_commanderhurt.lua` | 0 | 0 | 0 | 1 | 1 | 1 | 0 |
| `luaui/Widgets/gui_clearmapmarks.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_build_placement_extension.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_attack_aoe.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_ally_cursors.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_advplayerslist_mascot.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gfx_volumetric_clouds.lua` | 0 | 0 | 0 | 3 | 0 | 0 | 0 |
| `luaui/Widgets/gfx_snow.lua` (also `gl.PointSprite`) | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gfx_guishader.lua` | 0 | 0 | 1 | 1 | 0 | 1 | 0 |
| `luaui/Widgets/dbg_ffa_startpoints_picker.lua` (debug) | 0 | 0 | 0 | 0 | 1 | 1 | 1 |
| `luaui/Widgets/cmd_share_unit.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luarules/gadgets/unit_seismic_ping.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luarules/gadgets/unit_juno_damage_mini.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luarules/gadgets/unit_juno_damage.lua` | 1 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/unit_share_tracker.lua` | 0 | 1 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/snd_volume_osd.lua` | 0 | 0 | 1 | 0 | 0 | 0 | 1 |
| `luaui/Widgets/gui_transport_weight_limit.lua` | 1 | 0 | 0 | 0 | 1 | 0 | 0 |
| `luaui/Widgets/gui_show_orders.lua` | 0 | 0 | 1 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_pregame_build.lua` (also `gl.LineStipple`) | 0 | 2 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gui_pausescreen.lua` | 0 | 0 | 1 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_messages.lua` | 0 | 0 | 0 | 0 | 1 | 1 | 0 |
| `luaui/Widgets/gui_cache_icons.lua` | 0 | 0 | 0 | 2 | 0 | 0 | 0 |
| `luaui/Widgets/dbg_startbox_editor.lua` (debug) | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/dbg_deferred_buffer_visualizer.lua` (debug) | 0 | 0 | 0 | 2 | 0 | 0 | 0 |
| `luaui/Widgets/cmd_customformations2.lua` (also Stipple/PushAttrib) | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/api_los_combiner_gl4.lua` | 0 | 0 | 0 | 2 | 0 | 0 | 0 |
| `luaui/Widgets/unit_waypoint_dragger_2.lua` (also Stipple) | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/unit_loop_select.lua` | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/unit_alt_set_target_type.lua` | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gui_tax_buildspeed_debuff.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_sensor_ranges_sonar.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_sensor_ranges_radar.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_sensor_ranges_los.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_sensor_ranges_jammer.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gui_prospector.lua` | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gui_mapmarks_fx.lua` | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gui_easyFacing.lua` | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gfx_unit_stencil_gl4.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gfx_dof.lua` | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/gfx_deferred_rendering_GL4.lua` | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/gfx_darken_map.lua` | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/dbg_widget_profiler.lua` (debug) | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/dbg_quadtree_atlas_tester.lua` (debug) | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| `luaui/Widgets/dbg_deferred_test.lua` (debug) | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luaui/Widgets/cmd_extractor_snap.lua` | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luarules/gadgets/unit_target_on_the_move.lua` (also Stipple/PushAttrib) | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| `luarules/gadgets/include/GenEnvLut.lua` (offline bake) | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| `luarules/gadgets/include/GenBrdfLut.lua` (offline bake) | 0 | 0 | 0 | 1 | 0 | 0 | 0 |

---

## Runtime verification

Static grep proves presence, not absence, and can't see runtime frequency, dynamically-built call names, or other LuaOpenGL helpers that internally use immediate mode (`gl.DrawGroundQuad` does; ground-circle/shape-style helpers might). Confirm with:

### Run the tracer
1. Set `DeprecatedGLWarnLevel = 2` in `springsettings.cfg`.
2. Play/replay a normal match; capture `infolog.txt`. Level 2 logs each deprecated `gl.*` with its Lua caller (`[file]:line Caller: <fn>`), deduplicated.
3. **Coverage (done 2026-06-30):** `Rect`, `TexRect`, `DrawGroundQuad`, `PushAttrib`, `PopAttrib` now call `CondWarnDeprecatedGL`, so the tracer catches all relevant Lua-side blockers. Note `gl.PolygonMode`/`gl.Texture` are intentionally unhooked (core-valid, not blockers). Also note the tracer is Lua-side only — it will NOT report the engine's own per-callin scaffolding (Phase 0), which fires from C++; that's known statically and doesn't need the tracer.

### RenderDoc cross-check
- RenderDoc's first-frame API/error log names the exact rejected call. Compare against this doc.
- Repeat across: void vs non-void maps (engine void-edge alpha test), water modes, and representative widget loadouts (active set varies per player config).

---

## Runtime findings

### Run 2026-06-30 — GL 4.6 Compat, master `2026.06.06-80-gf898efc`, `infolog.txt`

A full match (reached `f≈64405`). **1350 deprecated-GL warnings, 26 distinct `gl.*` functions.** Frame split: **994 at `f=-000001`** (pre-game: loading screen + Chobby menu), **~356 at `f≥0`** (in-game). Warnings are deduplicated per call-site (first occurrence only) — so the `f≥0` set is a *lower bound* on in-game callers (a widget first seen drawing the loading screen is logged at `f=-1` even though it also draws in-game; e.g. `gui_flowui` shows only 7 `f≥0` but is obviously the #1 in-game UI).

**Function histogram (all frames):** Vertex 361 · Color 236 · DeleteList 126 · BeginEnd 90 · CreateList 76 · Translate 70 · TexCoord 54 · TexRect 53 · MultiTexCoord 43 · PushMatrix 39 · PopMatrix 39 · Scale 38 · LineWidth 36 · CallList 32 · Rect 16 · Rotate 9 · AlphaTest 9 · MatrixMode 7 · LoadIdentity 5 · Normal 3 · Shape 2 · Ortho 2 · PushPopMatrix 1 · PushAttrib 1 · PopAttrib 1 · Billboard 1.

#### Headline finding — a missed UI layer: Chili / Chobby / LuaMenu

The single largest non-BAR-content source is the **Chili UI framework** (the Chobby lobby/menu), which the static inventory never scanned (`libs/chiliui/`, `LuaMenu/` are outside `luaui/luarules/luaintro`):

- `libs/chiliui/chili/headers/skinutils.lua` — **159 sites** (overall #3)
- `LuaMenu/Widgets/api_chili.lua` (14), `LuaMenu/widgets/chobby/components/priority_popup.lua` (12), `modules/graphics/r2thelper.lua` (7), `libs/chiliui/chili/controls/{control,font,image}.lua` (7/5/4)

**All Chili/LuaMenu warnings fire at `f=-000001` and zero in-game** — so this is **menu/lobby scope, a separate goal from an in-game frame capture.** If we ever want to RenderDoc the menu, Chili is the dominant immediate-mode target; for in-game capture it's irrelevant.

#### In-game callers (`f≥0`, definitely draw during a match)

Authoritative work-list for the in-game goal (lower bound per dedup caveat). Functions firing in-game: DeleteList 80 · Color 65 · Vertex 45 · LineWidth 24 (core-valid, not a blocker) · TexRect 23 · Translate 20 · CallList 12 · BeginEnd 12 · PushMatrix/PopMatrix 11 · CreateList 10 · TexCoord 9 · Scale 8 · Rotate 8 · Rect 8 · AlphaTest 8 · Shape 2.

| Caller | f≥0 sites | | Caller | f≥0 sites |
|---|--:|---|---|--:|
| `gui_pip.lua` | 72 | | `gui_messages.lua` | 8 |
| `gui_mapinfo.lua` | 44 | | `gui_awards.lua` | 8 |
| `gui_ecostats.lua` | 24 | | `gui_flowui.lua` (undercounted) | 7 |
| `gui_chat.lua` | 22 | | `gfx_guishader.lua` | 7 |
| `gui_projectile_target_aoe.lua` | 17 | | `unit_seismic_ping.lua` (gadget) | 7 |
| `gui_com_nametags.lua` | 17 | | `gui_tooltip.lua` | 6 |
| `gui_top_bar.lua` | 15 | | `gui_options.lua` | 5 |
| `gui_converter_usage.lua` | 14 | | `gui_build_eta.lua` | 5 |
| `gui_info.lua` | 10 | | + ~25 more at 1–4 sites | |
| `unit_share_tracker.lua` | 9 | | (map_startbox, gui_attackrange_gl4, gui_defenserange_gl4, gui_commands_fx, gui_rank_icons_gl4, gfx_airjets_gl4, gfx_nano_particles_gl4, gfx_energy_explosion_particles_gl4, gui_advplayerslist*, widget_selector, gui_teamstats, gui_sensor_ranges_jammer, gui_gridmenu, gui_keybind_info, gui_gameinfo, gui_commanderhurt, gui_clearmapmarks, gui_changelog_info, cmd_share_unit, unit_respawning, unit_evolution, gfx_unit_shield_effects, gui_transport_weight_limit, gui_mouse_fx, gui_cache_icons, gui_pregameui_draft) | |
| `gui_advplayerslist_music_new.lua` | 9 | | | |

### Reconciliation with static inventory
- **Confirmed:** `gui_flowui`, `gui_pip`, `gui_top_bar`, `gui_chat`, `gui_ecostats`, `map_startbox` etc. all present — static inventory's BAR set is broadly right.
- **Static matrix undercounted everywhere:** it only tracked BeginEnd/Shape/Rect/TexRect/CreateList/CallList/FlowUI, never group B (matrix) or `gl.Vertex`/`gl.Color`. Runtime shows Vertex (361) + Color (236) + Translate (70) are the *highest-volume* calls. This is why `gui_build_eta` (uses only `local glColor/glTranslate/glScale` aliases) and `gui_mapinfo` (44 in-game!) barely registered or were absent in the matrix. **The caller files are right; the per-file weights in the matrix are not — trust this runtime list for the in-game set.**
- **In-game callers not in the primitive matrix — but almost all already known.** Diffing the 47 in-game callers against the matrix flagged 8 "new" files; on inspection the trace revealed **no significant unanticipated in-game source**:
  - 4 (`gfx_airjets_gl4`, `gfx_energy_explosion_particles_gl4`, `gfx_nano_particles_gl4`, `gui_rank_icons_gl4`) fire `gl.AlphaTest` and are **already in the Phase-3 AlphaTest residual list** — they were absent from the *matrix* only because it doesn't have an AlphaTest column.
  - 2 (`gui_attackrange_gl4`, `gui_defenserange_gl4`) fire **only `gl.LineWidth`** — core-valid, not blockers; correctly excluded.
  - 2 are **genuinely new and real, both trivial**: `gui_build_eta` (`gl.Color` + `gl.PushMatrix`/`Translate`/`PopMatrix` via aliases) and `gui_commands_fx` (a lone `gl.Color`). Group A/B, covered by the engine-side re-backing with no special handling.
  - `gui_mapinfo` (44 in-game) was in the matrix but weighted near-zero because its calls are matrix/Color — a weighting error, not a new file.
- **Hooks validated:** `gl.TexRect` (53), `gl.Rect` (16), `gl.PushAttrib` (1), `gl.PopAttrib` (1) appear → the 5 new `CondWarnDeprecatedGL` calls compiled and fire.
- **Noise to ignore:** `gl.LineWidth` (36 total / 24 in-game) is logged but core-valid — not a RenderDoc blocker.
- **Engine void-edge alpha test** (SMFGroundDrawer) — not visible here (Lua-only tracer); check the map used and RenderDoc's reject log separately.

### Open follow-ups from this run
- Decide scope: **in-game frame only** (ignore Chili/Chobby) vs **also menu** (Chili `skinutils.lua` becomes the #1 target). The engine-side `RenderBuffer` re-backing helps both for free since Chili also calls the same `gl.*`.
- The dedup hides true in-game frequency for widgets that also draw at load. If we want exact in-game-only attribution, add a frame-phase tag to the warn (or clear `deprecatedGLWarned` at `GameStart`).

---

## Testing & validation

Goal: prove each migrated `gl.*` primitive renders identically to the legacy immediate-mode version, reproducibly, in CI. Two tiers, because the risk splits cleanly.

### Tier 1 — matrix tracking: pure Catch2 unit tests (no GL, runs in normal CI)

**Landed 2026-06-30:** `test/engine/System/testGLMatrixStateTracker.cpp` (target `testGLMatrixStateTracker`). Authored test-first, then made green by extending `GLMatrixStateTracker` with `CMatrix44f` value stacks (see changelog). Note `myGL.h` `#error`s when `UNIT_TEST` is defined, so the tracker header was made to compile GL-free under `UNIT_TEST` rather than pulling the GL loader into the test.

The highest-risk piece (the `GLMatrixStateTracker` value stack from Phase 1) is pure CPU math and needs no GL context. Add a Catch2 test beside the existing `test/engine/System/testMatrix44f.cpp` / `testRotationMatrix44f.cpp` (same `add_spring_test` macro, no extra libs).

What it pins:
- **Multiply order/side** — `glTranslatef` post-multiplies (`M' = M·T`, vertex transformed as `M·T·v`). The likeliest bug. Assert op sequences against hand-built `CMatrix44f` goldens. `CMatrix44f` is column-major `m[16]` with `operator==` (bit-exact SSE) and `equals()` (epsilon).
- **Push/pop round-trip** — `m0 = Get(); Push(); mutate; Pop(); CHECK(Get() == m0)`.
- **Mode independence** — `GL_MODELVIEW`/`PROJECTION`/`TEXTURE` are three independent stacks.
- **`MultMatrix`/`LoadMatrix`/`LoadIdentity`/`Ortho`/`Frustum`** vs goldens; depth over/underflow matches `HasMatrixStateError`.

Why this suffices for the transform layer: `CMatrix44f` is already the engine's matrix type (with its own passing tests), so its GL-convention correctness is established; these tests only prove the tracker *drives* it with GL semantics. Optional belt-and-suspenders: a one-time shadow assert in a GL build comparing the tracker against `glGetFloatv(GL_MODELVIEW_MATRIX)` — note `gl.GetMatrixData` already does exactly that `glGetFloatv`.

### Tier 2 — pixel equivalence: offscreen dual-FBO render test (new infra)

**Harness + self-validation landed 2026-06-30:** `test/engine/Rendering/testGLImmediateCompare.cpp` (target `testGLImmediateCompare`). This first increment is deliberately the *harness and its controls only* — it brings up a hidden SDL2 **compatibility** context + glad, renders into a raw-GL RGBA8 FBO, reads back, and runs the comparator's **positive control** (same legacy `glBegin` scene twice ⇒ byte-identical) and **negative control** (different scenes ⇒ unequal; plus a 1-texel-flip case so the comparator can't pass by ignoring small diffs). Verified green under `xvfb-run … LIBGL_ALWAYS_SOFTWARE=1` (Mesa llvmpipe) and it `SKIP`s cleanly with no display (4 skipped, exit 0), so GL-less CI stays green. CMake only builds it `if (TARGET SDL2::SDL2 AND TARGET glad)`.

Scoped out of this increment (next step): it depends only on SDL2 + glad — **no** `globalRendering`/`configHandler`/`CFBO`/`TypedRenderBuffer` yet, because the "new backend" side needs the immediate-mode emitter factored `lua_State`-free first (see below). The context bring-up, comparator, readback, SKIP behavior and the xvfb/llvmpipe recipe all carry forward; only the FBO-creation lines (raw GL → `CFBO`) and the addition of the real legacy-vs-new A/B cases change.

Pixels need a real context — the engine has **no GL-context test today** (all current tests run on the headless stub). This is the one new piece to build. Design it as an in-process **A/B compare** (render legacy backend and new backend in the *same* context, same frame, assert equal) rather than golden-PNG comparison — so GPU/driver pixel quirks cancel and there are no frozen artifacts to maintain or drift.

**Dependency reality (scoped):**
- `shaderHandler` — lazy singleton (`#define shaderHandler (CShaderHandler::GetInstance())`), self-initializing. Free.
- `globalRendering` (`CGlobalRendering`) — **the only real bring-up.** `TypedRenderBuffer`'s shader-header generation reads `globalRendering->supportExplicitAttribLoc`. The test must instantiate `globalRendering` and set its GL-capability fields after the context is current (or run its capability detection).
- Legacy backend (`glBegin`) needs only the context; new backend (`TypedRenderBuffer` + the migration shader) needs the two globals above.

**Design implication (do this in the migration):** factor the new immediate-mode emitter into a **`lua_State`-free class** (e.g. `LuaImmediateBuffer` that `BeginEnd`/`Vertex`/`Rect`/`TexRect` call into). Then the test drives it directly — no `LuaHandle`, no drawing-enabled gating, no Lua. The legacy comparator is raw `glBegin`/`glVertex`. This keeps the test small and makes the backend independently testable; the `LuaOpenGL` functions become thin shims.

**Harness components:**
1. **Context** — `SDL_Init(VIDEO)` + hidden window + `SDL_GL_CreateContext` with `SDL_GL_CONTEXT_PROFILE_COMPATIBILITY` (the engine's own fallback path), then `glad` load. Compat is required so both `glBegin` *and* GLSL work in one context.
2. **`globalRendering`** — instantiate, make context current, populate capability flags.
3. **Render target** — `CFBO` (`rts/Rendering/GL/FBO.h`: `AttachTexture`/`Bind`/`Unbind`/`IsValid`) with a color texture, fixed size (e.g. 512×512), fixed clear color.
4. **Per case:** render legacy → FBO_A → `glReadPixels` → bufA; render new → FBO_B → bufB; `CHECK(bufA == bufB)` (or `maxΔ` within a documented per-case bound).
5. **Graceful skip** — if context creation fails (CI box without GL), Catch2 `SKIP`, don't fail. Keeps existing CI green.

**CMake wiring:** new `add_spring_test(GLImmediateCompare ...)` target linking the rendering object lib + `SDL2` + `glad` + `Catch2`, with a `main` that creates the context (not the shared `catch_main.cpp`). Model on the existing `add_spring_test` macro but add the GL/SDL libs.

**CI reproducibility recipe:** run under **`xvfb-run`** with **`LIBGL_ALWAYS_SOFTWARE=1`** → Mesa **llvmpipe**, a deterministic, GPU-independent compatibility context that supports FF + GLSL. `xvfb-run ctest -R testGLImmediateCompare`. llvmpipe output is stable across runs/machines, so the A/B compare is reproducible; and because both backends render on the same llvmpipe, vendor differences are irrelevant anyway.

**Cases (one primitive per FBO render, exactness expectation per case):**

| Case | Expect |
|---|---|
| `gl.Rect` opaque; `gl.TexRect` (default/flipS/flipT/explicit s,t) | bit-exact (AE=0) |
| `BeginEnd` TRIANGLES / FAN / STRIP; `gl.Shape` mixed attrs | bit-exact |
| `GL_QUADS` axis-aligned; rect inside Push/Translate/Rotate/Scale/Pop | bit-exact (transform already proven in Tier 1) |
| `GL_QUADS` rotated 30°; `GL_POLYGON` pentagon | ≤±1 LSB at diagonal — investigate, don't tolerate silently |
| smooth tri (3 colors); textured+per-vertex color (MODULATE) | ≤±1 LSB interpolation |
| attribute stickiness (color on vert 1 only) | bit-exact |
| LINES/LINE_LOOP/POINTS (width 1) | document; width-1 should match |
| two overlapping translucent rects (A then B); rect→text→rect | correctness (draw order / blend / z-interleave) |
| empty `BeginEnd`; single vertex; callback that errors | no crash, no state corruption |

### Tier 3 — GL state side-effects (contract C3)

A primitive can produce identical pixels yet leave different *state* behind, silently corrupting the next draw. So alongside the pixel A/B, snapshot observable state after a legacy callin vs a new callin and assert equality: current color (`GL_CURRENT_COLOR`), active texture + binding, blend/enable bits, matrix mode + per-mode stack depth (`GLMatrixStateTracker` already exposes this), scissor/viewport. Same offscreen harness, `glGet*` after each case. This catches the "batched buffer flush left blend enabled" / "didn't restore current color" class of bug that pixels alone miss.

**Reuse:** the same A/B comparator is the in-engine **`LuaGLCompareMode`** for running over real replays (render each callin to two FBOs, report `maxΔ`) — coverage driven by the `DeprecatedGLWarnLevel=2` caller list. One comparator, two harnesses: fixed unit scenes (CI) + live replay (broad coverage).

### Scope / effort summary

- **Tier 1:** small — a single Catch2 file, no new infra, runs everywhere. Land it with Phase 1.
- **Tier 2:** one-time harness build (~SDL offscreen context + `globalRendering` cap init + CFBO + readback + `add_spring_test` target + Xvfb/llvmpipe CI job). The main prerequisite is factoring the emitter `lua_State`-free. After that, adding cases is trivial and it's CI-enforceable.
- **Risks/unknowns:** `globalRendering` capability init outside full engine boot may need a few fields set by hand (enumerate them when building); llvmpipe must be present in the CI image; confirm `CFBO` doesn't pull additional globals (it shouldn't — it's a thin GL wrapper).

## Whole-frame A/B "test mode" (config `GLFrameABCompare`) — in-frame 4-pass design

A validation harness that renders each display frame **four times within the same iteration** — [legacy settle (presented, uncompared), legacy, legacy (same-frame control pair), modern (test)] — and compares the readbacks in-frame. One `CGame::Update` + one `UpdateUnsynced` per iteration means every pass consumes byte-identical draw-prep (drawFrame, timeOffset, camera, shadow fit, cull sets, particle draw-pos bake, per-draw UBO, buffer uploads) **by construction**; the sim runs at full speed and the cost is ~4× GPU per displayed frame. The settle pass absorbs all once-per-drawFrame lazy work (widget dlist rebuilds, RTT cache refreshes, run-twice workarounds, lazy shader recompiles) — trace analysis showed every recurring L↔L divergence class lands there and is settled by pass 1. **Status: byte-perfect — control=0 and signal=0 on every in-game frame of the fightertest benchmark (see 2026-07-03 changelog).** (History: the previous design drew each sim state on two consecutive real frames with the sim frozen on the duplicate — it worked, but needed 8 fragile cross-iteration pins and produced a whole bug class of pin leaks.)

Only two things advance while a pass renders, so only two pins remain, snapshot before pass 0 and replayed before passes 1–2:

- **`guRNG` + Lua `math.random`** snapshot/restore (`spring_lua_unsynced_rand_save/restore_state`) — FX draw jitter consumed during draw callins
- **draw clock** — `Spring.GetTimer`/`GetTimerMicros` pinned; **`os.clock`** replaced in unsynced states (`LuaLibs::OpenUnsynced` → `LuaUnsyncedRead::OsClock`) — wind-turbine etc.

Per-pass work is redone **from scratch**: own FBO/viewport restore, `DrawGenesis`, **shadow-map render** (so `DrawWorldShadow` Lua is backend-compared too), `worldDrawer.Draw()` (own clear + water RTT), and the full screen block. Two deliberate once-per-iteration exceptions (identical for all passes, just not backend-compared): `minimap->Update()` (wall-clock-throttled RTT cache) and the reflection-cubemap face round-robin + consume-once sky/specular/shading updates.

The same-frame L↔L control serves double duty: its >1-LSB pixels build the per-frame noise mask for the L↔M signal, and its count is the **pixel-perfect gate** — any nonzero control is a real per-pass leak (stateful Lua draw callin, temporal RTT accumulator, per-pass GL nondeterminism) with an in-frame repro (one apitrace captures all three passes). `AB_FORCE_LEGACY=1` (env) turns the run into [L,L,L] — the null test where control **and** signal must be zero. `GLFrameABCompareWarmup` (default 120) renders that many frames normally first so Lua temporal accumulators build real state before the freeze guards engage.

**Lua pass protocol.** `Spring.GetABCompareActive()` — true for all passes of an active compare; widgets with same-frame temporal-feedback accumulators (bloom `historyTex`, diaglines `coverageTex`, CAS, ssao SLOWFUSE fuse) freeze those updates while true. `Spring.GetABPassIndex()` (0/1/2) — for per-pass *recompute* dedups: `api_screencopy_manager.lua` keys its copy on (drawFrame, passIndex) so each pass re-copies its own backbuffer (a frozen copy would make the modern pass composite legacy pixels through CAS/DoF/glass). `Spring.GetABDuplicatePass()` (= passIndex > 0) — for stateful draw-callin animation (`self.fade += n`): guard the advance so repeat passes redraw pass 0's phase.

Historical results from the paired-frame era (still-relevant conclusions): calm/settled frames byte-identical; combat residual was 94–99.8 % ±1–2 LSB and **backend-independent** (same under force-legacy) — GPU/driver-level, below the API; visible content divergence (delta>32) small and localized. The in-frame design eliminates the cross-iteration sources (different draw-prep paths, buffer re-upload vs reuse), so the [L,L,L] null gate must be re-measured — it is the new baseline for what "pixel-perfect" can reach.

## Divergence-hunting plan (post-gate, 2026-07-03)

The gate is byte-perfect and cheap; the loop for every step below is: run `GLFrameABCompare=1` → control must be 0 (a nonzero control is a per-pass widget/engine leak — fix via the three patterns: `GetABCompareActive` freeze / `GetABPassIndex` dedup key / `GetABDuplicatePass` advance guard) → any signal is a real modern-backend bug; localize via the dump PNGs → `LuaGLCompareMode` per-call attribution → apitrace pass-diff (segment on the per-pass `glReadPixels`, multiset-diff (prog,fn,count) per FBO).

1. ~~**Re-gate with modern `gl.BeginEnd` promoted**~~ done 2026-07-03, green.
2. ~~**Coverage breadth**~~ DONE 2026-07-04, fully green (see changelog): ALL gates byte-perfect — the Supreme Isthmus multiplayer replay (0/4942 frames incl. pregame, draft UI, reflections, chat/playerlist churn), fightertest (0/271), local water test (0/652, BumpWater+PiP), idle start (0/410). Seven engine parity-bug families found and fixed by the breadth content. Fast local loop established (~1 min/gate cycle via `test/gl-ab-compare/` scripts — replays cost ~10 min because the recorded lobby replays at 1x, so iterate locally and confirm on the replay).
3. **Daily-drive modern** — `LuaModernGLBackend=1` + `FontUseMVPUniform=1` in normal play (no compare cost); anything that looks off gets reproduced under the gate.
4. **Expand the modern surface**, each increment closed by the same loop: textured `gl.BeginEnd` (texenv/multi-unit aware — currently exact legacy replay), `gl.Shape`/`gl.DrawGroundQuad`, group-B uMVP fed from `GLMatrixStateTracker` instead of the `glGetFloatv` bridge (Phase-0 prereq), then Phase 2 display lists (the font-in-list lesson: lists need real capture semantics, not stream aliasing), then the Phase-0 endgame (UBO MVP everywhere → remove `SetupScreenMatrices` FF + attrib stack + `ResetGLState` FF resets) → first RenderDoc capture attempt.

## Open questions / to-verify

- Which LuaOpenGL helpers beyond the A/B/E set internally emit immediate mode? (`gl.DrawGroundQuad` confirmed; audit ground-circle, shape, unit/feature draw helpers.)
- Does `gl.CreateList`'s engine implementation capture recorded calls in a way that can be re-backed onto a retained VBO without changing the Lua API? (Determines whether Phase 2 is zero-BAR-change.)
- Exact `AttribBits` set saved by `EnableCommon`'s `glPushAttrib` — needed to build the explicit modern save/restore.
- Are the offline/bake gadgets (`unit_icongenerator`, `Gen*Lut`) ever run during a live frame, or only at load? (If load-only, they don't affect in-match capture.)
- Tier-2 test: exactly which `globalRendering` capability fields must be set for `TypedRenderBuffer::GetShader()` to build (enumerate when wiring the harness; `supportExplicitAttribLoc` confirmed, likely also GL version/extension flags).
- Is Mesa `llvmpipe` available in the project's CI image? If not, the Tier-2 test only runs locally / on GL-capable runners (it SKIPs gracefully elsewhere).
- Does `CFBO` pull any globals beyond a current context? (Spot-check when building the harness — expected to be a thin GL wrapper.)

## History

`flowui_gl4.lua` (2589 lines) was a prior GL4 reimplementation of the FlowUI `Draw.*` API by Beherith — born 2021 on the GL4 branch, reverted once (2021), re-landed and developed 2022–early 2024, then sat dormant/unloaded (only an inert `gfx_ssao.lua` hook referenced it) and was removed in PR #7990 (2026-06-17, empty body, self-merged in 10s). Zero of the 40 `WG.FlowUI.Draw` widgets ever adopted it. Lesson: the migration died of non-adoption, not infeasibility — which is why the engine-side (behind unchanged `gl.*` API) approach is preferred here over a per-widget rewrite. Related: BAR PR #7813 (open) adds no-geometry-shader paths for ~22 GL4 widgets on macOS/Apple Silicon — a portability tax specific to the per-widget GL4 approach.
