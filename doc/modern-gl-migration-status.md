# A/B frame-compare restructure + modern-GL divergence hunt — COMPLETED (2026-07-03/04)

All milestones of the original plan landed, plus two follow-on increments. The living document is `doc/bar-gl4-immediate-mode-inventory.md` (changelog + "Divergence-hunting plan" section); memory `project_ab_test_combat_shimmer.md` has the condensed state.

## Outcome

**The whole-frame compare is byte-perfect both ways** on the fightertest benchmark (650-fighter mass combat, f=0–2100):
- `[L,L,L,L]` null gate: control=0 AND signal=0 on every in-game frame.
- `[settle,L,L,M]` modern gate: control 0/165, signal 0/165 — with modern gl.Rect/TexRect, promoted non-textured gl.BeginEnd, font uMVP, and the mirror-fed MVP all live.
- The old paired-frame "±1–2 LSB GPU nondeterminism floor" conclusion is DISPROVEN — it was cross-iteration draw-prep divergence + first-render lazy-init classes.

## What shipped (RecoilEngine, branch bruno/gl4-phase1-matrix-tracker)

- `5398c9736e` — restructure to one Update + N in-frame renders; cross-iteration pins deleted; sim full speed; `Spring.GetABPassIndex`; `SetABCompareActive` regression re-wired.
- `a819b62b17` — 4-pass [settle,L,L,M]: pass 0 uncompared, absorbs once-per-drawFrame lazy work (found via apitrace pass-diffing: dlist rebuilds shifting font stream-VBO aliasing, chat run-twice RTT, one-time fxShader SMOOTH_PARTICLES SetFlag-after-Enable relink); control = p1↔p2, signal = p2↔p3; control-pair dumps.
- `1e74b7a554` — real modern bug caught by the gate: font flush inside `gl.CreateList` compiles isn't list-safe (glUseProgram/glUniform execute-not-record → uMVP world text vanished); fixed via recordable FF immediate emission + texture-space-matrix dlist.
- `47403b839b` — cleanup: modern gl.BeginEnd promoted (env gate removed, re-gated green), FontShaderMVPCompare comparator ripped out, dead helpers removed.
- `487d2c5fd3` — **FF matrix mirror (`GL::ffMirror`)**: modern MVP now CPU-sourced (Phase-0 prerequisite); scaffolding seeds exact values incl. `__spring_glOrtho` clip-space composition; all gl.* matrix callouts mirrored (incl. previously-untracked gl.PushPopMatrix, gl.RenderToTexture); CreateList skips, CallList taints → glGetFloatv fallback; shadow-compare under the gate. Validated: gate 0/0 + zero shadow divergences.

## What shipped (Beyond-All-Reason, branch bruno/ab-compare-widget-guards, `0b3038426d`)

screencopy per-pass dedup key, bloom/diaglines accumulator freezes, CAS skip, ssao SLOWFUSE freeze, dbg_benchmark overlay pass guard. All no-ops in normal play.

## Next steps (see doc "Divergence-hunting plan")

1. ~~Re-gate with BeginEnd promoted~~ done, green.
2. Coverage breadth: gate over a water-map real-game replay (reflection/refraction subpasses, gui_pip) + idle base-building UI. Burn new control leaks to 0 via the three widget patterns, then judge signal.
3. Daily-drive `LuaModernGLBackend=1` + `FontUseMVPUniform=1`.
4. Expand modern surface toward RenderDoc (order): textured gl.BeginEnd (texenv-aware) → gl.Shape/DrawGroundQuad → **uniform/UBO MVP for RenderBuffers.inl + font fed from GL::ffMirror** (last gl_ModelViewProjectionMatrix consumers) → delete FF matrix set-calls in scaffolding → Phase 2 display lists (biggest legacy surface; mirror's only taint source) → attrib-stack/ResetGLState cleanup → first RenderDoc capture.

## How to run the gate

`GLFrameABCompare=1` (+`GLFrameABCompareDump=1` for PNG triplets) in the write-dir config, then the fightertest startscript; ~2 min. `AB_FORCE_LEGACY=1` env ⇒ null test. Log: `[Frame A/B] control(L<->L)=N px (max D), signal(L<->M)=M / total px (masked, max D)` — control must be 0; any signal is a real backend bug. Debug: dump PNGs → LuaGLCompareMode per-call → apitrace pass-diff (segment on per-pass glReadPixels, multiset-diff (prog,fn,count) per FBO — order-based alignment misleads).
