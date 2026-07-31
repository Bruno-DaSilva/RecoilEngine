# Whole-frame A/B compare gate — drivers & content scripts

Harness content for the `GLFrameABCompare` whole-frame pixel gate (see `doc/bar-gl4-immediate-mode-inventory.md`, "Whole-frame A/B test mode" + "Divergence-hunting plan"). The gate renders each frame 4× in-frame `[settle, legacy, legacy, modern]`; control(L↔L) must be 0, any signal(L↔M) is a real modern-backend bug.

## Running

`run_gate.sh` is the whole recipe, and it fails loudly instead of reading clean:

```
AB_WRITE_DIR=<write-dir> ./run_gate.sh watertest      # or: idletest, or a path to any startscript/replay
```

It re-applies the config keys (the engine **strips `GLFrameABCompare` on exit**, so a second run without them compares nothing), truncates the infolog, runs the engine by absolute path under a timeout, and then asserts — a run that compared no frames reads exactly like a clean one otherwise:

- the engine exited 0 (a timeout or a 127 leaves a plausible-looking log behind),
- at least `--min-compares` frames were actually compared (default 400),
- control(L↔L) = 0 and signal(L↔M) = 0 on every one of them.

Options: `--write-dir`, `--spring`, `--min-compares`, `--timeout`, `--force-legacy` (the `[L,L,L,L]` null test — control *and* signal must be 0). Doing it by hand instead:

```
printf "GLFrameABCompare = 1\nGLFrameABCompareDump = 1\n" >> <write-dir>/springsettings.cfg
DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
  ./spring --isolation --write-dir <write-dir> <startscript-or-replay.sdfz>
```

- `AB_DUMP_MIN_FRAME=<simframe>` → only save dump triplets at/after that sim frame (keeps the 8-dump budget for in-game frames on replays with leaky pregames).
- Results: `grep "Frame A/B" <write-dir>/infolog.txt`; dump PNGs land as `<write-dir>/frameab_NN_{legacy,modern,diff}.png`.

## Content scripts (fast local loop, ~1 min per gate cycle)

- `idletest_startscript.txt` — idle base-building start on Starwatcher: pregame countdown UI, buildmenu/gridmenu via commander selection, build orders.
- `watertest_startscript.txt` — Supreme Isthmus v2.1: BumpWater reflection/refraction subpasses, PiP, camera waypoints over water.

Copy to the repo root (or pass by path). Both auto-quit via the `debugcommands` modoption.

## Driver widgets (install into `<write-dir>/LuaUI/Widgets/`)

- `ab_idle_driver.lua` — local-game driver: selects the commander (buildmenu visible), queues a few build orders, optional camera waypoint cycling. Active only when config `ABIdleDriver=1` and not a replay. Knobs: `ABDriverCamPeriod` (seconds, 0=off).
- `ab_replay_driver.lua` — replay driver: playback speed (`ABDriverSpeed`), camera waypoints (`ABDriverCamPeriod`), opens Picture-in-Picture (`ABDriverPip`), quits at `ABDriverQuitFrame`.

BAR registers new user widgets DISABLED — after the first run, set their order ≥1 in `<write-dir>/LuaUI/Config/BYAR.lua` (`["AB Gate Idle Driver"] = 1000,` etc.).

## Replay caveat (important)

Replays mount the demo's pinned release game archive (`.sdp` pool), NOT `games/BAR.sdd` — BAR-branch widget guards do not apply. To gate a replay, build write-dir shadow widgets: fetch each widget from the BAR repo at the release commit (the version suffix, e.g. `test-30440-535fc79` → commit `535fc79`) and apply the guard patterns (`Spring.GetABCompareActive` freeze / `GetABPassIndex` dedup key / `GetABDuplicatePass` advance guard). The shadow set used in 2026-07-04 validation: api_screencopy_manager (per-passIndex copy key — REQUIRED for the signal to be meaningful), gfx_guishader, gui_flowui, gfx_bloom_shader_deferred, gui_pregameui, gui_pregameui_draft. Remove the shadows before local (BAR.sdd) runs — they would override the dev widgets.
