# BAR → RenderDoc: handoff

State as of 2026-08-02, branch `bruno/gl4-phase1-matrix-tracker`. Nothing pushed.
The living plan is `doc/bar-gl4-immediate-mode-inventory.md`; this file is the short version plus the open problem.

## Where it actually stands

**Constraint 1 — games that are not updated keep working. MET.** Every mechanism is behind config knobs that default to off, and with `FFVertexAttribRewrite = 0` the rewrite returns before touching a byte of shader source. `run_rdoc_capture.sh --legacy` still reports 32 rejected functions and produces no capture, which is the same reading as before any of this work.

**Constraint 2 — BAR is RenderDoc-capturable. NOT MET.** It looked met for several hours and it was not; see the correction below. Rejected GL calls really are at 0, and a `.rdc` really is produced and really does open — but only for frames where BAR's UI is not drawing.

## The open problem, and it is the critical path

With BAR's real widget set enabled, **the modern Lua backend renders differently from the legacy one**:

| widgets loaded | control (L↔L) | signal (L↔M) |
|---|---|---|
| 149 | 0 px | **0 px** over 842 frames |
| 241 | 0 px | **3,062,000 / 3,686,400 px**, every frame, max delta ~140 |

Same binary, same content (`watertest`), only the widget set differs. Control staying at 0 means the harness is sound and the widgets are pass-idempotent, so this is a real rendering difference and not a measurement artifact.

Everything else is downstream of this. Do not spend time on shader ports or capture plumbing until the modern backend matches legacy with the full UI up, because a capture of a frame that renders wrong is worth nothing.

**Why it went unnoticed.** A widget bisect earlier in the session used `luaui disablewidget`, which BAR **persists** to `LuaUI/Config/BYAR.lua`. 69 widgets were zeroed; a partial repair left 159 entries at 0 against 61 in an untouched sibling datadir. BAR's UI silently vanished — `attempt to index field 'fonts' (a nil value)` cascading through ~30 widgets, plus `GetPosition` / `resource_spot_finder` / `spotBuilder` nil, all of them BAR **API** widgets failing to load. The world still renders, so screenshots look plausible and the gate reports 0 px over content that is missing the most immediate-mode-heavy thing in the game.

**Always check `grep -c "Loading widget" infolog.txt` before believing a gate result.** 241 is right for this profile; ~149 means the config is damaged. Repair by copying order values from `/www/projects/bar-data2` for any name zeroed locally but positive there.

### First moves on it

1. `GLFrameABCompareDump = 1` is already set by `run_gate.sh`; the dumps land in the write-dir as `ab*_f<frame>_1st.png` / `_2nd.png`. Diff them and look at **where** on screen the 3M pixels are. 83% of the frame at a moderate delta smells like a fullscreen post-process — `gfx_guishader` (blur), `gfx_bloom_shader_deferred`, or the PiP — rather than 92 widgets each being slightly wrong.
2. If the dump does not name it, bisect the widget list **with a per-run restore of `BYAR.lua`** (`wbisect.sh` in the job tmp did this; the restore is mandatory or every iteration inherits the last one's disables and the search converges on noise). Snapshot `BYAR.lua` first.
3. Whatever it is, the fix is either the modern backend or the widget; both are in scope.

## What is genuinely proven

- **Zero rejected GL calls** in a BAR frame with the migration on, on both the skirmish content and a real 12-player replay.
- **Two shader ports**, `SMF{Vert,Frag}Prog` and `BumpWater{VS,FS}`, migrated to core GLSL 150 and measured **0 px** against the originals on two contents, via the dual-variant harness.
- **A capture opens and replays with the Mesa shader cache on** — 3166 actions / 3070 draws — for frames without BAR's UI. With the UI it still segfaults, at the 47th shader, one of BAR's own Lua shaders declaring `#version 150 compatibility`.

## Tooling built this session

| tool | what it is for |
|---|---|
| `test/gl-ab-compare/rdoc_trigger.c` | LD_PRELOAD shim that asks for a capture through RenderDoc's in-application API, since there is no UI and no F12 here |
| `test/gl-ab-compare/run_rdoc_capture.sh` | takes a capture, and by default opens it again to prove it is one; `--legacy` is the control that must produce nothing |
| `test/gl-ab-compare/rdoc_validate_capture.cpp` | links librenderdoc's replay side and opens a `.rdc` (thumbnail, driver, full action walk). Must export `REPLAY_PROGRAM_MARKER()` and build `-rdynamic` |
| `test/gl-ab-compare/mesa_compat_cache_repro.c` | 40-line repro of the Mesa bug below |
| `test/gl-ab-compare/ab_replay_shot.lua` | drives a replay unattended: fast-forward, screenshot, quit |
| dual-variant programs | `FFExperiment::CoreShaderVariant` + a twin linked from `<name>.core.glsl`; the A/B harness binds the twin on the candidate pass, which is the only pixel-exact way to prove a shader migration here |

## Traps, all of them paid for

- **Mesa's GLSL disk cache is not keyed on GL profile.** A `#version 150 compatibility` shader compiled in the game's compatibility context is served back to RenderDoc's **core** replay context, past the rejection that should stop it, and the linker segfaults. `MESA_SHADER_CACHE_DISABLE=true` is the workaround; `mesa_compat_cache_repro.c` shows all three outcomes.
- **Uniform *values* are per-program state.** A twin program has none, so it draws with everything at zero — that looked like a 3.4M-pixel bad port and was plumbing. `GLSLCopyState` on a variant switch fixes it.
- **A uniform nothing references is dropped by the linker.** Keeping a selector "declared for identification" classified every rewritten program as unfed. Identify by the generated *attribute* instead.
- **`gl_FogFragCoord` and `gl_TexCoord` are cross-stage plumbing**, needing a declared `out`/`in` pair with matching names — not the engine-fed uniform the matrices and fog get.
- **`ninja base/springcontent.sdz` does not drop removed files.** `rm` the `.sdz` first or a deleted shader keeps being served.
- **`luaui disablewidget` persists.** See above. It cost a wrong culprit twice and then the whole UI.
- **A harness widget must never `widgetHandler:RemoveWidget()` itself** — BAR writes that back as `order = 0`, permanently disabling it. Go inert instead.
- **Check `CMAKE_BUILD_TYPE` and the compiler in the cache, not the binary.** `build/` was found configured clang + Debug while `build/spring` was neither; building would have produced a slow, assert-armed, desync-prone binary. Canonical configure is in memory under `project_user_cmake_configure`.
- **The demo desyncs from frame 0** against this engine build — it was recorded on `2026.06.06-80-gf898efc`. Present with the knobs off too, so it is a version mismatch, not the migration.

## After the divergence is fixed

1. Re-verify the capture with the UI present, warm Mesa cache.
2. BAR's Lua shaders declare `#version 150 compatibility`. Stripping the token engine-side was tried at the `LuaShaders.cpp` funnel, gated at 3M px twice (unanchored and anchored to the `#version` line), and reverted. It needs BAR-side shader work, which the brief allows.
3. The remaining pre-core engine shaders (`Model*`, `ShadowGen*`, `Grass*`) are cleanup, not blockers. `Model*` and `ShadowGen{Vert,Frag}Prog` cannot be ported as shaders at all — they read `gl_Vertex`/`gl_Normal` from fixed-function vertex arrays, so migrating them needs the drawer changed, and that drawer already exists as the GL4 path. `IModelDrawerState::EnsureInstance` / `DropDeferredInstance` is scaffolding toward not compiling them; it works but something still requests the legacy slot (an early `SelectImplementation` before the GL4 drawer exists, where `best` falls through to `MODEL_DRAWER_GLSL`).
4. The knobs still default to off. Turning them on for everyone is a separate decision and has never been made.

## Running things

```bash
# gate (must be 0/0; check the widget count first)
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh watertest
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh idletest

# prove a .core.glsl port
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_gate.sh watertest --ff-experiment 15 --mixed-ok

# capture, and open it again
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_capture.sh
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_capture.sh --legacy   # must produce nothing

# offender meter
AB_WRITE_DIR=/www/projects/bar-data ./test/gl-ab-compare/run_rdoc_offenders.sh
```

The six migration knobs: `LuaModernGLBackend`, `LuaCmdListBakedStreams`, `LuaCmdListSuspendOnObjectCreate`, `ModernModelAttribs`, `ModernModelFFShader`, `FFVertexAttribRewrite`. `FFMatrixSuppress` defaults on and is inert without the last one.
