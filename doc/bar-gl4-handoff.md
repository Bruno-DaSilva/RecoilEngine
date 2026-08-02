# BAR → RenderDoc: handoff

State as of 2026-08-02, branch `bruno/gl4-phase1-matrix-tracker` (engine) + `bruno/ab-compare-widget-guards` (BAR checkout at /www/projects/Beyond-All-Reason, symlinked as the loaded game). Nothing pushed.
The living plan is `doc/bar-gl4-immediate-mode-inventory.md`; this file is the short version.

## Where it stands

**Constraint 1 — games that are not updated keep working. MET.** Every mechanism is behind config knobs that default to off, and with `FFVertexAttribRewrite = 0` the rewrite returns before touching a byte of shader source. `run_rdoc_capture.sh --legacy` still reports 32 rejected functions and produces no capture, re-verified after everything below.

**Constraint 2 — BAR is RenderDoc-capturable. MET, including the full UI.** With the migration knobs on and BAR's real 241-widget set loaded: zero rejected GL calls, a 1-frame capture is produced (`rdoc_capture_frame4291.rdc`, 874 MB), and RenderDoc opens and replays it — 3154 actions, 3034 draws, degraded=0, thumbnail shows the complete UI. The replay side runs with `MESA_SHADER_CACHE_DISABLE=true`, which remains the documented workaround for the Mesa cache-vs-profile bug (`mesa_compat_cache_repro.c`).

**The gate is green at full coverage.** watertest 742 frames and idletest 485 frames, control 0 / signal 0, with all 241 widgets loaded. Check `grep -c "Loading widget" infolog.txt` before believing any gate result; 241 is right for this profile.

## The widget-set divergence: what it actually was

The 3M-px-per-frame "modern backend divergence" with the full widget set was **the gate's legacy reference rendering wrong, not the modern backend**. The dumps showed the legacy passes hazed fullscreen and the modern pass crisp; per-draw comparison (`LuaGLCompareMode` + `AB_COMPARE_ONLY=guishader` + `AB_COMPARE_DUMP`) proved the modern arm drew gfx_guishader's stencil-masked blur correctly and the legacy arm drew a flat constant-texcoord wash.

Root cause: the vertex-attribute rewrite had dropped the `gl_Vertex`/`gl_MultiTexCoord0` arm from generated sources (needed for core-clean text), so a forced-legacy `glBegin`/`glTexCoord` submission through a rewritten Lua shader fed builtins the shader no longer read. Position survived only via the attribute-0 alias; texcoords collapsed to a stale constant.

Fix: `FFRewriteBuiltinArm = 1` re-emits the arm behind the per-draw `recoil_ff_useAttrs` selector that `DrawFFAttribStream` already toggles. It is the gate's measurement configuration — `run_gate.sh` owns it on, `run_rdoc_capture.sh` owns it off (the knob persists in springsettings.cfg, and a capture inheriting it would re-pin shaders to the compatibility profile). Default off: shipped sources stay byte-identical and core-clean.

Consequence worth acting on: **every earlier gate verdict taken against rewritten Lua shaders under forced-legacy passes is suspect.** In particular the two reverted attempts to strip the `compatibility` token at the `LuaShaders.cpp` funnel were both "gated at 3M px" — the same signature, almost certainly the same reference corruption. Re-measure that with the fixed gate before writing off engine-side stripping; it may remove the need for BAR-side shader edits for the warm-cache path.

## Also found and fixed on the way

- **Nested `gl.RenderToTexture` restored FBO 0** instead of the caller's framebuffer (LuaOpenGL.cpp) — outer bodies' draws landed on screen at the outer texture's viewport.
- **The FFStandIn circle shader could never compile under the rewrite** — it wrote `gl_ClipVertex`, which fails once the `compatibility` token is dropped, rejecting the stand-in and retaining exactly the FF client-array draws the capture cannot afford. The line is now emitted only in compat mode; clip planes are suppressed state under the rewrite (measured inert in BAR).
- **BAR widget per-pass guards** (BAR branch `bruno/ab-compare-widget-guards`): `gui_cache_icons` advanced its icon-cache position per DrawScreen invocation (different icons per A/B pass); `gui_pip` throttles texture updates on `os.clock()` wall time, firing in whichever repeat pass crossed the interval. Both now update only on the non-duplicate pass, same pattern as gfx_guishader's stencil guard.
- **Open BAR cosmetic bug (not gate-blocking, pass-stable now):** something in gui_pip's LOS/texture pipeline paints a ~128×128 box (LOS texture content: its clear colors and coverage blobs) at the screen's bottom-left corner every frame. It sits under the info panel so players don't see it. Found via `AB_CORNER_PROBE` + the corner-probe user widget; not root-caused to the exact draw.

## Diagnostics built this session (all env-gated, committed)

| env | what it does |
|---|---|
| `AB_SHADER_MAP=1` | logs every Lua program id with VS/FS source heads, so program-id warnings become attributable to a widget |
| `AB_BOUNDSHADER_LEGACY=1` | declines every bound-shader stream flush to the exact legacy replay — isolates that flush as a divergence source |
| `AB_COMPARE_ONLY=substr` | restricts LuaGLCompareMode to matching call sites and logs every occurrence (no once-per-site dedup) |
| `AB_COMPARE_DUMP=1` | writes both arms of the first diverging compares as PPMs |
| `AB_CORNER_PROBE=1` | reads one corner pixel after each phase of each A/B pass |

Trap paid for twice this session: **`LuaGLCompareMode = 1` persists in springsettings.cfg** and without `AB_COMPARE_ONLY` it double-renders + glFinishes + reads back around every immediate draw — a ~30x draw slowdown that reads as "the gate barely compares any frames". Same persistence class as `ABUnitShapeDriver`. Delete the key after diagnostic runs.

## What is genuinely proven now

- Zero rejected GL calls in a BAR frame with the migration on (skirmish + a real 12-player replay).
- The modern Lua backend matches true legacy at **full BAR UI coverage** on two contents (742 + 485 frames, 0/0).
- A capture **with the full UI** opens and replays (cache-off replay).
- Two shader ports (`SMF{Vert,Frag}Prog`, `BumpWater{VS,FS}`) still 0 px via the dual-variant harness.

## Remaining work

1. Re-measure the `LuaShaders.cpp` compatibility-token strip with the now-sound gate (see above). If clean, BAR-side shader edits may be unnecessary for the warm-cache replay path.
2. BAR's own `#version 150 compatibility` Lua shaders keep the capture replay on the cache-off workaround; making them core-clean (BAR-side or via 1.) removes the Mesa-bug exposure entirely.
3. The remaining pre-core engine shaders (`Model*`, `ShadowGen*`, `Grass*`) are cleanup, not blockers; `Model*`/`ShadowGen*` need the drawer change (`IModelDrawerState::EnsureInstance` scaffolding exists; an early `SelectImplementation` still requests the legacy slot).
4. The knobs still default to off. Turning them on for everyone is a separate decision and has never been made.
5. Root-cause the gui_pip corner artifact (BAR-side).

## Running things

```bash
# gate (must be 0/0; check the widget count first — 241 for this profile)
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh watertest
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh idletest

# prove a .core.glsl port
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh watertest --ff-experiment 15 --mixed-ok

# capture, and open it again (forces FFRewriteBuiltinArm=0 itself)
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_capture.sh
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_capture.sh --legacy   # must produce nothing

# offender meter
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_offenders.sh
```

The migration knobs: `LuaModernGLBackend`, `LuaCmdListBakedStreams`, `LuaCmdListSuspendOnObjectCreate`, `ModernModelAttribs`, `ModernModelFFShader`, `FFVertexAttribRewrite`. `FFMatrixSuppress` defaults on and is inert without the last one. `FFRewriteBuiltinArm` is gate-only (see above).

## Traps, all of them paid for (kept from the previous handoff)

- **Mesa's GLSL disk cache is not keyed on GL profile** — `MESA_SHADER_CACHE_DISABLE=true` on the replay side; `mesa_compat_cache_repro.c` shows all three outcomes.
- **Uniform values are per-program state** — a twin program has none; `GLSLCopyState` on variant switch.
- **A uniform nothing references is dropped by the linker** — identify rewritten programs by the generated attribute.
- **`gl_FogFragCoord`/`gl_TexCoord` are cross-stage plumbing** — declared out/in pair, not a uniform.
- **`ninja base/springcontent.sdz` does not drop removed files** — `rm` the `.sdz` first.
- **`luaui disablewidget` persists** to `LuaUI/Config/BYAR.lua`; repair against `/www/projects/bar-data2`. Bisects must restore BYAR.lua per run.
- **A harness widget must never `widgetHandler:RemoveWidget()` itself** — BAR writes `order = 0` back.
- **Check `CMAKE_BUILD_TYPE` and the compiler in the cache** — canonical configure in memory `project_user_cmake_configure`.
- **The demo desyncs from frame 0** against this engine build (recorded on `2026.06.06-80-gf898efc`) — version mismatch, not the migration.
- **New user widgets default to order 0** — enable them explicitly in BYAR.lua's order table or they silently do not load.
