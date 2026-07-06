# SimSnapshot differential gate (PR 17, extended by PR 18)

TEST-ONLY harness that verifies the extracted `SimSnapshot` (PR 15) bit-matches live sim state, and — since PR 18 — that the snapshot-served Lua callout families return bit-identical values to the live bodies they replaced. Each converted family lands pre-verified by this gate.

By construction the snapshot and the live sim describe the *same completed sim frame* (extraction runs in `CGame::Draw` right after the render-event drain; in today's single-threaded tree the sim never advances between extraction and any draw-side read). So the invariant is **exact equality, no tolerance** — any mismatch is a real contract violation (torn extraction, missed field, stale-row / validity bug, or a masking mistake).

## What the gate checks

Engine side: `rts/Rendering/Common/SnapshotDiffGate.{h,cpp}`, hooked from `CGame::Draw` right after `simSnapshot.Update()`. When armed, at each draw boundary it:

- **Field pass** — walks every unit id and compares, bit-exact against the live `CUnit`:
  - validity both ways (a valid snapshot row iff `unitHandler.GetUnit(id) != nullptr`);
  - every extracted field for valid rows, including the PR-18 additions (midPos/aimPos, paralyzeDamage/captureProgress, beingBuilt/stunned, relMidPos + front/up/rightdir, posErrorVector, leavesGhost), the per-allyteam `losStatusAll`/`posErrorBits` stride rows, and the per-buffer global block (alliance matrix, radar-error scalars).
- **Masked-value sweep (PR 18)** — for every valid unit and every allyteam POV (plus the invalid-allyteam branch), diffs `SimSnapshot::UnitRows::ErrorVector` — the exact masking function the Lua serving twins apply — against live `CUnit::GetErrorVector`. This verifies the masking math for ALL POVs every boundary, independent of which POV the local handles run at.
- **Serving-path dual-run (PR 18)** — while armed, every redirected callout invocation (`LuaSnapshotServe::Route`) executes BOTH real paths — the live body and the snapshot twin — and bit-compares the actual Lua return slots (gating, masking and values included), then serves the snapshot values. Reported per callout name (`callout:GetUnitPosition` etc.). No third copy of any formula exists in the verifier.

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

Env knobs: `SPRING_DATADIR` (default `/www/projects/bar-data`), `DG_QUIT_FRAME` (0 = run to game over / demo end), `DG_FF` (fast-forward via `/setspeed 20` + `/speedcontrol 0`), `DG_EXERCISE` (widget calls the redirected family over all units each draw-callin frame), `DG_POV_FRAME`/`DG_POV_SPAN`/`DG_POV_TEAM` (mid-replay single-team POV segment: drops fullview via `specfullview 0` + `specteam N` so the dual-run comparator also runs at a non-fullRead POV — real errorVector/LOS masking; defaults 6000/6000/auto).

The driver widget (`callout_diff_driver.lua`) is unsynced-only (synced gadgets would perturb the demo sync hash) and is installed into the write-dir on each run; on a fresh data dir the first run only registers it (disabled) — either re-run once or set `["Callout Diff Gate Driver"] = 1000` in `LuaUI/Config/BYAR.lua`. The exercise loop runs from `widget:DrawGenesis` (plus `DrawWorld` when the world pass runs) — only `Draw*` callins set the draw-context flag the PR-18 redirect branches on, so an `Update`-loop would exercise the live path only, and headless runs never get past DrawGenesis (`CGame::Draw` early-outs on `!globalRendering->active` before DrawWorld/DrawScreen).

The script greps the infolog for the gate report and for `DESYNC`: a clean run reports `GATE : PASS (0 mismatches)` and `SYNC : clean`. Any `[DESYNC WARNING]` means the resim diverged from the demo (build/demo mismatch or a sync bug) — do a full consistent rebuild before concluding anything.

## Adding the next family (PR 18 pattern)

Follow §E.2 of `doc/sim-draw-thread-decoupling-research.md`: add the snapshot rows (SimSnapshot.h add-a-field recipe, incl. the SnapshotHash word and the field-pass compare here), write the serving twin in `LuaSnapshotServe.cpp` (line-by-line mirror, identical error text and float op order), turn the registered entry into a `LuaSnapshotServe::Route(L, __func__, &<Name>Live, &LuaSnapshotServe::<Name>)` call, add the family's callouts to the driver's `ExerciseFamily()` loop, then run this gate over both gate replays (zero mismatches) and the two master-demo resim gates.
