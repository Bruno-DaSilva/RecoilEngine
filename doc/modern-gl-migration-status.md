# Modern-GL migration — status & handoff (updated 2026-07-04 night, Phase-2 command lists LANDED)

The living document is `doc/bar-gl4-immediate-mode-inventory.md` (changelog + "Divergence-hunting plan"); memory `project_ab_test_combat_shimmer.md` has the condensed harness state. This file is the session-level status + next steps.

## Session 2026-07-04c: Phase-2 Lua command lists IMPLEMENTED and byte-perfect

`9415529616` — gl.CreateList capture/replay behind config `LuaCommandLists` (default off). The design committed in the previous session is now code, validated at the 0-pixel bar: **watertest 0 imperfect, idletest 0/406, fightertest 0/293, Supreme Isthmus replay signal(L<->M)=0 on ALL 4,623 compared frames** (remaining nonzero lines are the known control-only pinned-archive gadget per-pass counters).

Mechanism (all in `rts/Lua/LuaOpenGL.cpp` + `rts/Lua/LuaCommandList.h` + `rts/Lua/LuaDisplayLists.h`):

- **Capture = glad pointer swap.** During the gl.CreateList body, ~50 recordable GL entry points are swapped for recorder functions (X-macro `CMDLIST_RECORDERS`) — exact glNewList record semantics at GL-call granularity, no Lua-level parsing duplication, engine-helper GL captured too. glBegin..glEnd runs become `ImmStreamData` (posUV + exact float colors + seed-vertex counts); everything else becomes typed `Cmd`s. `compilingDisplayList` stays true so the Lua dispatch takes the (captured) legacy branches and the FF mirror is suppressed; the per-context glMatrixTracker bookkeeping (matData) is unchanged.
- **Replay through the live backend.** gl.CallList on a captured list executes state cmds as real GL, feeds matrix cmds to BOTH GL and the FF mirror (captured lists no longer Taint the mirror — that was the mirror's only taint source), and flushes streams through LuaImmediateBuffer under exactly the live gl.BeginEnd dispatch conditions (modern uMVP shader when the gates allow, exact legacy replay otherwise). Seed-class vertices (before the list's first glColor) get the REPLAY-time current color, matching real display-list inherit semantics; end-state current color/texcoord follow the live flushes.
- **Materialize fallback.** Recordable-but-unsupported calls (glUseProgram, glPushAttrib, glNormal3f, glTexImage2D, ... — X-macro `CMDLIST_MATERIALIZERS`) convert the capture into a real GL display list mid-body: open glNewList, replay captured cmds into the compile (seed vertices emitted color-less to preserve inherit), restore pointers, body continues recording natively. Per-list hybrid — unsupported lists behave exactly as today. Measured BAR coverage: only glNormal3f, glPush/PopAttrib, glTexParameteri, glTexImage2D materialize (fonts/unit shapes), logged once per function.
- Nested cases: gl.CreateList inside a capture → real list with the capture suspended (pointer restore/re-swap); gl.CallList of a captured list inside a capture → command splice with seed-color rebasing; inside a real compile → records the exact legacy replay; nested real lists → recorded `CallGLList`, replayed with glCallList + mirror Taint.

### Two NEW parity laws the command-list gate exposed (both now legacy-fallback gates in LuaImmediateBuffer)

1. **Ortho-only composition (`OrthoProjection`)**: the screen-aligned-MV composition-exactness measurement only holds under an ortho-like projection (w row {0,0,0,*}). BAR's tilted top-bar UI draws through a PERSPECTIVE projection inside lists; the perspective divide amplifies CPU-vs-driver P*MV composition ULPs into subpixel shifts, which LINEAR-sampled high-frequency textures (glyph caches, icons) turn into whole-shade deltas (~4-7k px/frame, max ~200) while flat fills stay byte-exact. Bisected with a temporary `AB_CMDLIST_MODERN=off|untex|tex` knob: mechanics green, untextured green, textured diverged → matrices.
2. **Texture completeness (`MipIncompleteTexture2D`)**: FF samples an INCOMPLETE texture as if texturing were disabled (flat vertex color); GLSL `texture()` returns (0,0,0,1). The replay's draft-spot octagons (a LIVE gl.BeginEnd path, never exposed by local-gate content) bind a mip-filtered texture with no mip chain — legacy drew white fills, modern black. Gate detects: mipmapping min filter + MAX_LEVEL≠0 + no level-1 image. Applied to stream AND TexRect flushes.

### What this unlocks (next steps, in dependency order)

1. Soak `LuaCommandLists=1` + `LuaModernGLBackend=1` in normal play; then consider default-on for the branch.
2. Task #6: with captured lists replaying through the live backend, the FF-interop bridges (glGetFloatv matrices, glColor4fv/glTexCoord2f end-state restores) remain the modern flushes' only legacy calls — replaceable by tracked state once FF consumers die.
3. Task #5: mirror-fed MVP (drop gl_ModelViewProjectionMatrix from RenderBuffers.inl/font) — still blocked by dense-MV/perspective composition parity while the FF comparator exists; becomes possible after FF matrix set-call deletion (post-#6).
4. Task #8: apitrace scan of a LuaCommandLists+modern frame for residual legacy calls from modern paths; first RenderDoc capture attempt.

## Session 2026-07-04b: modern surface EXPANDED, replay signal ZERO

New increments, all byte-perfect on every gate (watertest 0/642, idletest 0/408, fightertest 0/275, Supreme Isthmus replay signal 0 on ALL ~4700 compared frames):

- `2c1ded416e` — textured gl.BeginEnd modern behind an exact-parity texenv gate (unit-0 GL_TEXTURE_2D, MODULATE, no texgen, identity texmat, no other units, non-GL_ALPHA format; else exact legacy replay). Watertest coverage: 1025+ textured flushes modern, 1 fallback.
- `0d23b66f45` — modern flushes moved off the 8-bit color attribute onto the emitter's own interleaved float stream (pos3/uv2/color4, orphaned stream VBO + persistent VAO): vertex colors bit-exact by construction (stacked additive glow quads amplified 8-bit rounding to visible deltas); FF current-texcoord end-state restored after textured flushes.
- `0ae5745b62` — the TypedRenderBuffer default shader takes uMVP per draw (config RenderBufferUseMVPUniform, per-pass toggled by the gate) via a non-creating probe in DrawArrays/DrawElements: every engine call site covered with zero per-site changes. AB_DUMP_MIN_PIXELS env knob; glGetTexEnviv headless stub.
- `a1aff837a0` — FF fog parity via SEPARATE program variants ({untex,tex} x {fogless,fogged}; per-vertex LINEAR fog factor, gl_Fog state, engaged only when the factor dips below 1) + the SCREEN-ALIGNED-MV gate (below).

## HARD-WON LESSON: dense-matrix composition is not reproducible

No CPU/GPU recipe provably bit-matches the driver's own P*MV composition on dense (rotated/world) matrices: float-composed, double-composed and two-step P*(MV*v) variants each flip DIFFERENT thin-primitive edge pixels (measured classes: gl.Rotate'd loading-spinner arcs, world-camera lines/quads in PiP/minimap views; the driver's ordering is undocumented, and prints hide last-ULP differences). ALSO measured: merely touching proven shader source perturbs compiled gl_Position math enough to flip edge pixels — never restructure a parity-proven shader; add program VARIANTS.

Resolution: modern immediate flushes and the RenderBuffer uMVP hook engage only under a screen-aligned modelview (no rotation/shear terms — covers virtually all BAR UI); dense-MV draws keep the exact legacy path until Phase 2 deletes the FF pipeline and bit-parity against it stops being a requirement. The immediate backend's matrices come from the glGetFloatv bridge (the mirror re-derives glRotatef trig, ULP-differs); the mirror stays shadow-verified for Phase 2.

Debug-loop additions: `test/gl-ab-compare/`-adjacent write-dir widget `ab_fogprobe.lua` (config ABFogProbe=1, local-only — REPLAY-GATED, a harness widget without the replay gate contaminated several replay measurements) draws fogged world quads/lines out to 43k elmos so fog parity is exercised by the 1-minute gates.

## FINAL 2026-07-04b RESULT: replay signal ZERO on all 4667 compared frames

Modern-vs-legacy is byte-perfect on every gate: watertest 0/642, idletest 0/408, fightertest 0/275, Supreme Isthmus replay signal(L<->M)=0 on ALL 4667 compares. The replay's only 2 nonzero lines are CONTROL (legacy-vs-legacy) noise, fully root-caused:

- apitrace pass-diff of a null-test run ([L,L,L,L]) showed pass2 containing a 126-call block absent from pass1: the lazy `muzzleside.tga` load + first instanced flameVBO draw of BAR's `gfx_missile_thruster_gl4.lua` GADGET. Its `idleSkipCounter` ticks once per DrawWorld CALL, so under the 4-pass gate a missile launched after an idle period pops into existence mid-pass-sequence.
- Fix landed on BAR branch `bruno/ab-compare-widget-guards` (`60eeae158f`): guard the decrement with `Spring.GetABDuplicatePass`. Local content (BAR.sdd) is clean; the 2026-06-13 replay's PINNED release archive cannot be patched (gadgets are synced-loaded from the archive; write-dir shadows only work for LuaUI widgets), so those 2 control frames are a permanent measurement artifact of old replays — engine-independent, masked out of signal by harness design.

## Phase-2 scoping: display-list content is a ~27-function surface (measured)

apitrace histogram of ALL glNewList..glEndList blocks in a full watertest run (57,249 list compiles): the recorded content is only ~27 distinct GL functions, dominated by immediate-mode geometry the emitter already captures — glVertex3f 376,796 / glColor4fv 144,219 / glTexCoord2f 132,044 / glColor4ub 75,556 / glScalef 55,394 (the font's texture-space matrix) / glNormal3f 19,200 / glBegin+glEnd 4,210 each — plus a small state set (glEnable/Disable 2.6k/2.0k, glBlendFunc 1.5k, glMatrixMode 1.5k, glBindTexture 1.5k, Push/PopMatrix ~430, glTranslatef 401, Push/PopAttrib 367) and 367 NESTED glCallList. (The glGet*/glGenTextures inside compiles EXECUTE rather than record — our own list-index probes and named-texture lazy loads.)

**Phase-2 design — Lua command lists:** gl.CreateList stops calling glNewList. Capture mode appends (op, args) records as the body executes once (queries still execute — matches GL semantics); at gl.EndList the geometry runs (Begin/Vertex/Color/TexCoord/Normal/End) are BAKED into prebuilt float-stream vertex batches; gl.CallList replays state/matrix commands through the normal LuaOpenGL dispatch (so matrix ops feed the mirror — no more taint — and draws go through the CURRENT backend) and submits baked batches as single VBO draws (no per-vertex replay cost). Nested CallList replays recursively. Any list whose body hits an op without capture support falls back to compiling a REAL display list for that one list (per-list hybrid — correctness preserved while coverage grows, same migration pattern as everything else). This removes: recorded glUseProgram/glUniform (the uniform-cache class), the mirror's only taint source, the modern backend's list-compile legacy fallbacks, and the single biggest RenderDoc blocker.

---

# Previous session (2026-07-04a): coverage breadth COMPLETE

## Where things stood: EVERY gate byte-perfect

Divergence-plan step 2 (coverage breadth) done, at the 0-pixel bar, with the modern surface live (gl.Rect, gl.TexRect, non-textured gl.BeginEnd, font uMVP, mirror-fed MVP):

- **Supreme Isthmus multiplayer replay** (24 players, draft mode, reflections, chat/playerlist churn, PiP profile): control=0 AND signal=0 on ALL 3871 pregame + 1071 in-game compared frames (to f=30000).
- fightertest (650-fighter mass combat): 0/271.
- Local Supreme Isthmus water test (BumpWater reflection/refraction, PiP open, camera waypoints over water, buildmenu + build orders): 0/652.
- Idle base-building start (pregame countdown, buildmenu/gridmenu): 0/410.
- Zero `[FFMatrixMirror]` shadow-compare warnings anywhere.

Run recipes + driver widgets are committed under `test/gl-ab-compare/` (README covers config, env knobs, and the replay caveat). Local gate cycles take ~1 minute — iterate there, not on replays (a replay costs ~10 min because the recorded lobby replays at 1x).

## What the breadth content caught (seven engine parity-bug families, all fixed on this branch)

Details + evidence in the inventory changelog (2026-07-04 entries). Headlines:

- `778abc2866` — font shader uniform-cache desync: glUseProgram/glUniform are RECORDED into display-list compiles, not executed (prior comments had this backwards); the cached uUseMVP write during a compile desynced cache-vs-GPU and made legacy text vanish. PushGLState now skips the shader path inside compiles.
- `92c205e135` — modern gl.Rect/TexRect/BeginEnd are not list-safe (stream-VBO aliasing + recorded program/uniform calls) → legacy fallback inside `gl.CreateList`; glPolygonMode parity (GL_LINE outline rects); FF current-color seeding from `glGetFloatv(GL_CURRENT_COLOR)`.
- `452d3d4ce1` — modern gl.TexRect requires `GL_TEXTURE_2D` enabled (legacy doesn't sample when off).
- `fe76b4acb0` — float-exact color parity: SColor's raw float→uint8 cast WRAPS overbright components (glColor 1.15 → blue 37 instead of clamped 255 — BAR uses overbright colors; the dominant replay class) → clamp+round everywhere; single-color streams (incl. every animated fade) route the exact float color through the new `uColor` uniform; flushes restore FF current color in float precision; FlushLegacy replays original float colors.
- `ad8ff67244` — GameSetupDrawer advanced its "Starting in n" countdown by raw wall clock once per render pass → pinned to pass 0 via `LuaUnsyncedRead::IsABDuplicatePassRaw()`.
- `05cfcd8901` — latent vanilla bug: a mid-frame TypedRenderBuffer grow (`Resize()`) recreated the buffer object while the VAO kept referencing the deleted one → post-grow draws rendered stale data. Re-`InitVAO` + re-upload.
- `336f11c833` — `AB_DUMP_MIN_FRAME` env reserves the 8-dump budget for in-game frames.

BAR branch `bruno/ab-compare-widget-guards` gained `f90a198daa` (pregame countdown per-pass timer guard, guishader stencil/delete freezes, FlowUI dlist-lifecycle freeze).

## Key infrastructure discovery: replays bypass BAR.sdd

Demo playback mounts the demo's pinned release `.sdp` archive — BAR-branch widget guards do NOT apply to replay runs. Replay gating therefore uses write-dir shadow widgets: fetch each widget from the BAR repo at the release commit (version suffix = commit, e.g. `test-30440-535fc79`), apply the guard patterns, drop into `<write-dir>/LuaUI/Widgets/`. The validated shadow set (screencopy per-passIndex key — mandatory for signal validity — guishader, flowui, bloom, pregameui×2) lives in `/www/projects/bar-data/LuaUI/Widgets.replay-shadows/`; install for replay runs, REMOVE for local runs (they'd override the dev widgets). With shadows, replay pregame control = 0.

## Next steps (roadmap)

1. **Daily-drive modern** — `LuaModernGLBackend=1` + `FontUseMVPUniform=1` in normal play (no compare cost); anything that looks off gets reproduced under the gate.
2. Expand modern surface toward RenderDoc (order): textured gl.BeginEnd (texenv-aware) → gl.Shape/DrawGroundQuad → uniform/UBO MVP for RenderBuffers.inl + font fed from GL::ffMirror (last gl_ModelViewProjectionMatrix consumers) → delete FF matrix set-calls in scaffolding → Phase 2 display lists (the biggest legacy surface; the list-compile fallbacks added this session are the interim answer; the mirror's only taint source) → attrib-stack/ResetGLState cleanup → first RenderDoc capture.
   NOTE for the display-list phase: display lists RECORD glUseProgram/glUniform — engine uniform caches are structurally incompatible with lists that record uniform calls on shared programs (see memory `project_dlist_compile_records_program_calls.md`). Any modern increment must stay recordable-FF inside compiles until lists get real capture semantics.
   NOTE on colors: keep new modern paths float-exact — quantize only via `LuaImmediateBuffer::ClampedColor` (clamp+round; raw SColor float casts WRAP overbright input), and prefer the uColor-uniform pattern for whole-draw colors.

## How to run the gate

`GLFrameABCompare=1` (+`GLFrameABCompareDump=1` for PNG triplets) in the write-dir config (re-add before EVERY run — the engine rewrites the file on exit), then a `test/gl-ab-compare/` startscript (~1 min) or a replay (~10 min). `AB_FORCE_LEGACY=1` env ⇒ null test; `AB_DUMP_MIN_FRAME=<simframe>` gates the dump budget. Log format: `[Frame A/B] control(L<->L)=N px (max D), signal(L<->M)=M / total px (masked, max D)` — control must be 0; any signal is a real modern-vs-legacy divergence. Debug loop: dump PNGs → `LuaGLCompareMode=1` per-call attribution (NOTE: its logger dedups per callsite on FIRST result — state-dependent intermittents can hide) → apitrace pass-diff (segment on the per-pass glReadPixels, multiset-diff (prog,fn,count) per FBO; if glretrace crashes on huge traces, a full normalized call-stream diff of two passes plus `--blobs` payload comparison is the fallback that cracked the overbright-wrap class).
