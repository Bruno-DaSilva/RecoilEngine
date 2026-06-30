# Engine-GL rendering tests

Tests for the modern-GL migration (see `doc/bar-gl4-immediate-mode-inventory.md`).

## Catch2 unit tests (run by ctest)

Built only when `SDL2` + `glad` targets exist; each `SKIP`s if no GL context
can be created (headless box), so they stay green everywhere. Run them under a
software GL stack for determinism:

```
xvfb-run -a -s "-screen 0 64x64x24" env LIBGL_ALWAYS_SOFTWARE=1 \
    ctest -R "GLImmediateCompare|GLMatrixDrawCompare|GLRenderBufferCompare|GLImmediateEmitter"
```

- `testGLImmediateCompare` — offscreen A/B harness self-validation (the
  comparator's positive/negative controls).
- `testGLMatrixDrawCompare` — legacy `glBegin` vs a uniform-MVP shader, MVP
  from the Phase-1 matrix tracker.
- `testGLRenderBufferCompare` — legacy vs the real engine `TypedRenderBuffer`.
- `testGLImmediateEmitter` — the `LuaImmediateBuffer` backend: every primitive
  mode, textures, blending/draw-order (C4), edge cases (C5), state-leak (C3).

Engine-GL tests live in this subdir because they need the real GL headers
(`myGL.h` `#error`s under `UNIT_TEST`); the subdir `CMakeLists.txt` removes
`-DUNIT_TEST` and links the rendering sources with minimal stubs
(`GLEngineStubs.cpp`).

## In-engine integration test (manual; needs a display + local BAR data)

`glcompare_integration.sh` runs the real GL `spring` with `LuaGLCompareMode=1`
over a BAR startscript/replay and fails if any wired `gl.*` callin diverges
from legacy by more than 1 byte. This is the contract-C6 live check that
exercises real BAR widgets — it is **not** a ctest unit test.

```
test/engine/Rendering/glcompare_integration.sh \
    build/spring /path/to/bar-data \
    test/engine/Rendering/glcompare_startscript.txt
```

`glcompare_startscript.txt` is a short auto-quitting BAR scenario (Starwatcher
map, spawns a fight, `setspeed 20`, `quitforce` at frame 400). Its `gametype=`
and `mapname=` may need updating to match the locally installed game/map
version. A replay (`.sdfz`) can be passed instead of the startscript.
