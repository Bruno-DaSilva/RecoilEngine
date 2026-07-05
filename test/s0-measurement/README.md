# S0 measurement pass (sim|draw boundary sizing)

Harness for the S0 stage of `doc/replay-seeking-architecture.md`: replay a demo headless at max speed with the boundary-size instrumentation (`/boundarydump`, PR 1) and the draw-context callout census (`/calloutcensus`, PR 2) armed, and verify sync-cleanliness at the same time.

## Usage

```bash
# full-length measurement run (outputs land in $SPRING_DATADIR)
test/s0-measurement/run_s0.sh replays/<demo>.sdfz <label>

# instrumentation overhead: base binary vs instrumented idle vs instrumented active
test/s0-measurement/run_overhead.sh replays/<demo>.sdfz <base-bin> <instr-bin> [quitframe]

# stats + census summary
python3 test/s0-measurement/analyze_s0.py <label>_boundary.csv <label>_callouts.csv
```

Knobs (env): `SPRING_DATADIR` (default `/www/projects/bar-data`), `S0_QUIT_FRAME` (0 = run to game over / demo end), `S0_DUMP_END`, `S0_PROF_START`/`S0_PROF_LEN` (optional `/profiledump` window), `S0_CALLOUTS` (`LuaTrackCalloutCounts` level), `S0_BOUNDARY` (arm `/boundarydump` or not).

The driver widget (`s0_stats_driver.lua`) is unsynced-only (synced gadgets would perturb the demo sync hash) and is installed into the write-dir on each run; on a fresh data dir the first run only registers it (disabled) — either re-run once or set `["S0 Stats Driver"] = 1000` in `LuaUI/Config/BYAR.lua`.

A clean run reports `SYNC : clean`; any `[DESYNC WARNING]` means the resim diverged from the demo (instrumentation sync bug, engine-version mismatch with the demo, or a stale mixed build — do a full consistent rebuild before concluding anything).

## results/

2026-07-05 dataset from the two 2026.06.10 rewind-test demos (Rosetta 1.4.4, All That Glitters Extended v1.0.2), full length, sync-clean, engine = `bruno/poc-split-sim-draw` at the census commit:

- `<label>_analysis.txt` — analyze_s0.py output (full-run + last-third stats, census top-30).
- `<label>_callouts.csv` — full per-callout census (`name,count,count_draw,count_sim,count_other`).
- The raw per-frame `<label>_boundary.csv` files (~8 MB each) are not committed; regenerate with `run_s0.sh` (a few minutes per demo).

Headline numbers are written into `doc/replay-seeking-architecture.md` ("Sizes") and `doc/sim-draw-thread-decoupling-research.md` (§E.1).
