# Modern-GL migration — status & handoff (updated 2026-07-04, coverage-breadth session COMPLETE)

The living document is `doc/bar-gl4-immediate-mode-inventory.md` (changelog + "Divergence-hunting plan"); memory `project_ab_test_combat_shimmer.md` has the condensed harness state. This file is the session-level status + next steps.

## Where things stand: EVERY gate is byte-perfect

Divergence-plan step 2 (coverage breadth) is done, at the 0-pixel bar, with the modern surface live (gl.Rect, gl.TexRect, non-textured gl.BeginEnd, font uMVP, mirror-fed MVP):

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
