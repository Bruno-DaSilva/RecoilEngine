# Modern-GL migration — status & handoff (updated 2026-07-04, coverage-breadth session)

The living document is `doc/bar-gl4-immediate-mode-inventory.md` (changelog + "Divergence-hunting plan"); memory `project_ab_test_combat_shimmer.md` has the condensed harness state. This file is the session-level status + next steps.

## Where things stand

**Dev-content gates are byte-perfect across all three content suites** with the full modern surface live (gl.Rect, gl.TexRect, non-textured gl.BeginEnd, font uMVP, mirror-fed MVP):

- fightertest (650-fighter mass combat): 279/279 frames, control=0, signal=0.
- Supreme Isthmus water test (BumpWater reflection/refraction, PiP open, camera waypoints over water, buildmenu + build orders): 644/644, 0/0.
- Idle base-building start (pregame countdown, buildmenu/gridmenu): 410/410, 0/0.
- Zero `[FFMatrixMirror]` shadow-compare warnings anywhere.

Run recipes + driver widgets are committed under `test/gl-ab-compare/` (README covers config, env knobs, and the replay caveat). Local gate cycles take ~1 minute — iterate there, not on replays (a real replay costs ~10 min because the recorded lobby replays at 1x).

## What the coverage-breadth content caught (all fixed, engine branch)

Replay content (24-player Supreme Isthmus, draft mode) found three parity-bug families fightertest could never see — details + evidence in the inventory changelog (2026-07-04 entry):

- `778abc2866` — font shader uniform-cache desync: glUseProgram/glUniform are RECORDED into display-list compiles (not executed — prior understanding was backwards); the cached uUseMVP write during a compile desynced cache-vs-GPU and made legacy text vanish. PushGLState now skips the shader path inside compiles.
- `92c205e135` — modern gl.Rect/TexRect/BeginEnd are not list-safe (stream-VBO aliasing + recorded program/uniform calls) → legacy fallback inside `gl.CreateList`; glPolygonMode parity (GL_LINE outline rects); FF current-color parity (seed from `glGetFloatv(GL_CURRENT_COLOR)`, restore exact legacy end-state via SeedColor/Color split).
- uncommitted-at-time-of-writing follow-up: gl.TexRect requires `GL_TEXTURE_2D` enabled for the modern path (legacy doesn't sample when disabled).
- `336f11c833` — `AB_DUMP_MIN_FRAME` env: reserve the 8-dump budget for in-game frames.

BAR branch `bruno/ab-compare-widget-guards` gained `f90a198daa` (pregame countdown per-pass timer guard, guishader stencil/delete freezes, FlowUI dlist-lifecycle freeze).

## Key infrastructure discovery: replays bypass BAR.sdd

Demo playback mounts the demo's pinned release `.sdp` archive — BAR-branch widget guards do NOT apply to replay runs. Replay gating therefore uses write-dir shadow widgets: fetch each widget from the BAR repo at the release commit (version suffix = commit, e.g. `test-30440-535fc79`), apply the guard patterns, drop into `<write-dir>/LuaUI/Widgets/`. The validated shadow set (screencopy per-passIndex key — mandatory for signal validity — guishader, flowui, bloom, pregameui×2) lives in `/www/projects/bar-data/LuaUI/Widgets.replay-shadows/`; install for replay runs, REMOVE for local runs (they'd override the dev widgets). With shadows, replay pregame control = 0/2934.

## Open: replay-only residual tail (all modern-linked — null test is clean)

Sigs + evidence in the inventory changelog. In priority order:

1. Minimap camera-box class: legacy `(186,170,255)` vs modern `(186,170,37)` (blue channel only), ~0.6–1.3k px, max 218, f≈180–2020. Per-call compare clean ⇒ inter-draw state propagation. Survived the color-parity fix. Suspect: pip-minimap (top-left minimap in the replay layout is pip-based). Next: reproduce locally with pip-as-minimap enabled, then apitrace the in-game window.
2. Playerlist icon class (~160 px, 14×16 icon, max ~210, many pregame frames): the GL_TEXTURE_2D-enable fix targets exactly this shape — re-validate on the replay (fix landed after the last full replay run).
3. "Starting in N" countdown digit (content differs p2 vs p3, ~600 px bursts): drawer unidentified — the string is NOT in the BAR game repo; suspect recorded lobby/console text. Idle test does not reproduce it.
4. Late-game signal classes past f≈3000 (up to 16k px): unlocalized; localize with `AB_DUMP_MIN_FRAME=<frame>` dumps once 1–3 are gone.

## Next steps (roadmap unchanged beyond this)

1. Burn down the replay residual tail (above) — fast-loop locally, confirm on the replay.
2. Daily-drive `LuaModernGLBackend=1` + `FontUseMVPUniform=1`.
3. Expand modern surface toward RenderDoc (order): textured gl.BeginEnd (texenv-aware) → gl.Shape/DrawGroundQuad → uniform/UBO MVP for RenderBuffers.inl + font fed from GL::ffMirror (last gl_ModelViewProjectionMatrix consumers) → delete FF matrix set-calls in scaffolding → Phase 2 display lists (the biggest legacy surface; the list-compile fallbacks added this session are the interim answer; the mirror's only taint source) → attrib-stack/ResetGLState cleanup → first RenderDoc capture.
   NOTE for the display-list phase: display lists RECORD glUseProgram/glUniform — engine uniform caches are structurally incompatible with lists that record uniform calls on shared programs (see memory `project_dlist_compile_records_program_calls.md`). Any modern increment must stay recordable-FF inside compiles until lists get real capture semantics.

## How to run the gate

`GLFrameABCompare=1` (+`GLFrameABCompareDump=1` for PNG triplets) in the write-dir config (re-add before EVERY run — the engine rewrites the file on exit), then a `test/gl-ab-compare/` startscript (~1 min) or a replay (~10 min). `AB_FORCE_LEGACY=1` env ⇒ null test; `AB_DUMP_MIN_FRAME=<simframe>` gates the dump budget. Log format: `[Frame A/B] control(L<->L)=N px (max D), signal(L<->M)=M / total px (masked, max D)` — control must be 0; any signal is a real modern-vs-legacy divergence. Debug loop: dump PNGs → `LuaGLCompareMode=1` per-call attribution (NOTE: its logger dedups per callsite on FIRST result — state-dependent intermittents can hide) → apitrace pass-diff (segment on the per-pass glReadPixels, multiset-diff (prog,fn,count) per FBO; `glretrace -D <call>` for state dumps).
