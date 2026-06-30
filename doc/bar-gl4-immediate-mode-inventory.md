# BAR → modern-GL migration: living inventory & plan

Working document for moving Beyond-All-Reason (`/www/projects/Beyond-All-Reason`) and the Recoil engine off deprecated fixed-function / immediate-mode OpenGL, so a live BAR frame can be captured in RenderDoc. Iterate on this as we learn more — especially after runtime tracing (see Runtime findings log).

## Goal

**Definition #1 — a RenderDoc-capturable BAR frame.** Remove every OpenGL call RenderDoc rejects (immediate mode, fixed-function matrix stack, display lists, attrib stack, alpha test, line stipple, point sprite, fixed-function lighting) from a normal in-match frame. The engine can keep requesting a **compatibility** GL context (`GlobalRendering.cpp:512`); RenderDoc tolerates a compat context, it only chokes on the rejected *calls*. We are **not** targeting a strict GL4 core-profile context (that would additionally require the whole engine, not just BAR-triggered paths, to be core-clean, and the engine to flip its context request).

## Status / changelog

- 2026-06-30 — initial consolidation from static analysis. Key conclusions below are static-grep-derived and **must be confirmed at runtime** (`DeprecatedGLWarnLevel=2` + RenderDoc reject log). Nothing built yet.
- 2026-06-30 — closed the tracer-coverage gap: added `CondWarnDeprecatedGL(L, __func__)` to `Rect`, `TexRect`, `DrawGroundQuad`, `PushAttrib`, `PopAttrib` in `rts/Lua/LuaOpenGL.cpp` (they previously fired no warning). `DeprecatedGLWarnLevel=2` now covers all relevant Lua-side blockers. Deliberately NOT added to `gl.PolygonMode` and `gl.Texture` — both are core-valid (not RenderDoc blockers) and would flood the log with false positives.
- 2026-06-30 — adopted a **test-forward, oracle-backed** method: legacy path (behind the flag) is the oracle; added a Compatibility contract (C1–C6), a write-test-first loop with a green-suite commit gate, harness positive/negative controls, and per-phase done-gates. Legacy is not deleted until the whole contract is green over unit scenes + a replay bake.
- 2026-06-30 — **Tier-2 modern-vs-legacy A/B landed + key shader finding.** Added `test/engine/Rendering/testGLMatrixDrawCompare.cpp` (target `testGLMatrixDrawCompare`) and factored the shared harness into `test/engine/Rendering/GLTestHarness.h` (context/FBO/comparator/legacy helpers; `testGLImmediateCompare` refactored onto it, still green). The new test renders the *same geometry* two ways — legacy fixed-function `glBegin` vs a modern **VAO + uniform-`mat4`-MVP GLSL-150 shader** — both fed the **identical `CMatrix44f` MVP built by the Phase-1 `GLMatrixStateTracker`** (loaded into the FF stack for legacy, set as the uniform for modern, so any delta is purely backend). Results under llvmpipe: **flat axis-aligned quad is bit-exact (maxAbsDelta=0)**; smooth-colored triangle and a tracker-transformed (Translate+Rotate) quad within ≤1 LSB. This exercises the Phase-1 tracker end-to-end on the GPU and proves a RenderDoc-clean draw reproduces legacy output. **IMPORTANT architectural finding:** the engine's stock `TypedRenderBuffer` shader (`RenderBuffers.inl`) transforms via `gl_ModelViewProjectionMatrix` — the deprecated fixed-function builtin, set by exactly the `glMatrixMode`/`glLoadMatrixf` calls we must remove for RenderDoc. So "re-back `gl.BeginEnd` onto `TypedRenderBuffer`" is **not** sufficient as-is; the modern backend needs a **uniform-MVP shader variant fed by the matrix tracker** (which is why this increment validated that path with a minimal hand-rolled VAO+shader rather than the stock TypedRenderBuffer). **Next:** bring up `globalRendering`(+configHandler)+`CFBO`+`TypedRenderBuffer` and add a uniform-MVP shader variant, then A/B the *engine* path; and factor the emitter `lua_State`-free for the LuaOpenGL integration.
- 2026-06-30 — **Tier-2 harness + controls landed** (`test/engine/Rendering/testGLImmediateCompare.cpp`, target `testGLImmediateCompare`). Hidden SDL2 compat context + glad + raw-GL FBO + glReadPixels + the comparator's positive/negative controls (incl. a 1-texel-flip deny case). Green under `xvfb-run`/llvmpipe; `SKIP`s with no display so GL-less CI stays green. SDL2+glad only (no engine globals yet) — the `globalRendering`+`CFBO`+`TypedRenderBuffer` "new backend" A/B cases are the next increment, gated on factoring the emitter `lua_State`-free. See the Tier-2 section for details.
- 2026-06-30 — **Phase 1, iteration 0 (matrix tracker) landed test-first.** Wrote `test/engine/System/testGLMatrixStateTracker.cpp` (Catch2, wired via `add_spring_test` in `test/CMakeLists.txt`, modeled on `testMatrix44fRotation`) covering C1/C5-matrix: post-multiply order/side for `Translate`/`Scale`/`Rotate`/`MultMatrix`, glRotatef-style axis normalization, bit-exact push/pop round-trip + nesting, three independent mode stacks, `Ortho`/`Frustum` vs `CMatrix44f::OrthoProj`/`PerspProj` goldens, and depth over/underflow vs `HasMatrixStateError`. Then extended `GLMatrixStateTracker` (`rts/Rendering/GL/MatrixStateTracker.h`) from depth/mode-only to hold real `CMatrix44f` value stacks per mode (the existing depth counters stay the canonical balance check and are kept in sync). Group-B Lua callouts (`gl.Translate/Scale/Rotate/Ortho/Frustum/LoadIdentity/LoadMatrix/MultMatrix/Billboard`) now drive the tracker, gated behind a new `LuaMatrixTracking` config flag (default **off** ⇒ byte-identical to the legacy FF-only path). To keep Tier-1 truly GL-free (`myGL.h` `#error`s on `UNIT_TEST`), the tracker's value-math compiles without the GL loader under `UNIT_TEST` (3 mode enums defined locally; the GL-calling `HandleMatrixStateError` is `#ifndef UNIT_TEST`). **Known limitations (follow-ups):** (1) the new value stacks are not yet isolated across nested callins / display lists (the `PushMatrixState`/`PopMatrixState` swap only moves the depth counters) — Phase 0 will seed/restore them per callin; (2) `Ortho`/`Frustum` track the plain `OrthoProj`/`PerspProj`, not the engine's clip-space-control variant (`__spring_glOrtho`'s extra translate/scale) — reconcile when the shader path actually consumes the MVP.
- 2026-06-30 — added **Testing & validation** section: Tier 1 = pure Catch2 matrix-tracker unit tests (no GL, like `testMatrix44f.cpp`); Tier 2 = offscreen dual-FBO A/B render test (new infra — engine has no GL-context test today; needs SDL compat context + `globalRendering` cap init + CFBO + Xvfb/llvmpipe). Prereq for Tier 2: factor the immediate emitter `lua_State`-free.
- 2026-06-30 — **first real runtime trace** (GL 4.6 Compat, master `2026.06.06-80-gf898efc`, includes the 5 new hooks — confirmed `Rect`/`TexRect`/`PushAttrib`/`PopAttrib` fire). See Runtime findings below. Headline: **Chili/Chobby/LuaMenu is a whole immediate-mode UI layer the static inventory missed** (menu-scope), and the static matrix undercounted because it never tracked groups B/Vertex/Color.
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

This is why Phase 0 is mandatory and first. The fix:

- Replace `glPushAttrib`/`glPopAttrib` with explicit modern state save/restore — the engine already has a state abstraction in `rts/Rendering/GL/State.h`.
- Turn `ResetGLState`'s FF resets into shader-state defaults / no-ops.
- Route `SetupScreenMatrices` and the world/shadow matrix setups through the CPU-tracked matrix system (shared with Phase 1 / group B).

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
