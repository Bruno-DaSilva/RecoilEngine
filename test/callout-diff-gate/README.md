# SimSnapshot differential gate (PR 17)

TEST-ONLY harness that verifies the extracted `SimSnapshot` (PR 15) bit-matches live sim state. It is the verifier PR 18 builds on: PR 18 redirects the hot Lua callout families (`Spring.GetUnitPosition`-class) to read the snapshot instead of live sim objects in draw context, and each converted family lands pre-verified by this gate.

By construction the snapshot and the live sim describe the *same completed sim frame* (extraction runs in `CGame::Draw` right after the render-event drain; in today's single-threaded tree the sim never advances between extraction and any draw-side read). So the invariant is **exact equality, no tolerance** — any mismatch is a real contract violation (torn extraction, missed field, stale-row / validity bug, or a masking mistake).

## What the gate checks

Engine side: `rts/Rendering/Common/SnapshotDiffGate.{h,cpp}`, hooked from `CGame::Draw` right after `simSnapshot.Update()`. When armed, at each draw boundary it:

- **Field pass** — walks every unit id and compares, bit-exact against the live `CUnit`:
  - validity both ways (a valid snapshot row iff `unitHandler.GetUnit(id) != nullptr`);
  - every v1 field for valid rows: `pos`, `speed`, `health`, `maxHealth`, `team`, `allyTeam`, `defID`, `buildProgress`, and the `losStatus` row vs `unit->losStatus[snapshot viewAllyTeam]`.
- **Callout comparator** — `CheckCalloutUnitPosition(id, livePos)`, the plug PR 18 fills. Diffs the snapshot-served base position (`SimSnapshot::Read().Pos(id)`, the exact accessor PR 18 will call) against the live value `LuaSyncedRead::GetUnitPosition` would push (`o->pos`, pre-`errorVec`). Exercised here as proof-of-use, driven from the field pass itself — this PR redirects **no** Lua callout, it only builds and proves the comparison plumbing.

Mismatches are `LOG_L(L_ERROR, ...)` (never crash — project convention) with frame, unitID, field name, and both values; per-field checked/mismatched counters are reported on dump / disarm / game end. Zero cost when unarmed (a single branch).

## Command

```
/snapshotdiffgate [arm|disarm|dump]
```

- `arm` (default) — start verifying at each boundary; resets counters.
- `dump` — print the running per-field totals without stopping.
- `disarm` — print totals and stop. (The engine also prints a final report at game teardown if the gate was left armed.)

Pass criterion: the `[SnapshotDiffGate]` report prints `PASS (0 mismatches)` and no per-field mismatches over a full replay.

## Usage

```bash
# full-length gate run over a demo (report lands in the infolog)
test/callout-diff-gate/run_diff_gate.sh replays/<demo>.sdfz <label>
```

Env knobs: `SPRING_DATADIR` (default `/www/projects/bar-data`), `DG_QUIT_FRAME` (0 = run to game over / demo end), `DG_FF` (fast-forward via `/setspeed 20` + `/speedcontrol 0`), `DG_EXERCISE` (widget calls the positions-family callouts over all units each draw frame to keep the draw surface hot).

The driver widget (`callout_diff_driver.lua`) is unsynced-only (synced gadgets would perturb the demo sync hash) and is installed into the write-dir on each run; on a fresh data dir the first run only registers it (disabled) — either re-run once or set `["Callout Diff Gate Driver"] = 1000` in `LuaUI/Config/BYAR.lua`.

The script greps the infolog for the gate report and for `DESYNC`: a clean run reports `GATE : PASS (0 mismatches)` and `SYNC : clean`. Any `[DESYNC WARNING]` means the resim diverged from the demo (build/demo mismatch or a sync bug) — do a full consistent rebuild before concluding anything.

## What PR 18 plugs into

When PR 18 converts a callout family (e.g. `Spring.GetUnitPosition`) to snapshot-serve in draw context, it calls the matching `SnapshotDiffGate::CheckCallout*` comparator from the callout body while `snapshotDiffGate.Armed()`, passing the live value it would otherwise have returned. The comparator diffs it against the snapshot-served value through the same counters/logging, so the new family lands verified by the same replay pass. New snapshot-served value shapes add a `CheckCallout*` peer next to `CheckCalloutUnitPosition`.
