# WS-6 — Estimated-Path Demand Gate

Status: DESIGN. Quick-win workstream WS-6 of the epoch-produce cost program (parent: `doc/sim-draw-split-optimization/00-program-overview.md` §4, strategy `DG` per §3). Independent of the other quick wins; one small PR. All code references are against the `bruno/poc-split-sim-draw` main checkout.

## 1. Problem

`ExtractUnitMoveType` (rts/Rendering/Common/SimSnapshot.cpp:1026) is called once per active unit per epoch inside the `Update::SimSnapshot::UnitsBuildMove` pass (SimSnapshot.cpp:1245-1250, 778 µs late-game per §1 baseline). For every ground unit with a live path it copies the FULL estimated path into the epoch rows (SimSnapshot.cpp:1066-1073):

```cpp
if (const unsigned int pathID = g->GetPathID(); pathID != 0) {
	rows.estPathHasPath[id] = 1;
	std::vector<int> starts;                                          // per-unit temp alloc
	pathManager->GetPathWayPoints(pathID, rows.estPathPoints[id], starts);
	rows.estPathStarts[id].assign(starts.begin(), starts.end());     // + a second copy
}
```

`GetPathWayPoints` (HAPFS: rts/Sim/Path/HAPFS/PathManager.cpp:945-982) walks and reverse-copies three waypoint lists (`maxResPath`, `medResPath`, `lowResPath`) into `points`, pushing three segment starts. The QTPFS twin (rts/Sim/Path/QTPFS/PathManager.cpp:1858-1882) resizes to `path->NumPoints()` and copies point-by-point. Either way this is an O(path length) gather + `float3` vector copy, per ground unit with an active path, every epoch. Late game (many units with active paths) this is the dominant, variable share of the `UnitsBuildMove` pass. The remaining `UnitsBuildMove` cost — `ExtractUnitBuildState` and the `MoveTypeBlock` scalar copies (SimSnapshot.cpp:1030-1118) — is a fixed per-unit scalar write and is out of WS-6 scope (WS-3 stage 5).

This entire payload exists solely to serve the UNSYNCED snapshot twin `LuaSnapshotServe::GetUnitEstimatedPath` (rts/Lua/LuaSnapshotServe.cpp:10420-10439), which reads `rows.estPathPoints[id]` / `rows.estPathStarts[id]` from the held slot and pushes them to Lua via `PushPathNodesFromSnap` (LuaSnapshotServe.cpp:10395-10416). Per program insight §2.1 this is pure re-gather of state nothing on the draw side asked for.

Also to fix (trivial, same PR): the per-unit `std::vector<int> starts` temp allocation (SimSnapshot.cpp:1070) and the redundant `.assign` copy at line 1072.

## 2. Evidence: does stock BAR ever query `Spring.GetUnitEstimatedPath`?

Two independent lines of evidence say the SNAPSHOT twin is never served in stock BAR:

1. **Source grep.** Grepping the full BAR game tree (`/www/projects/Beyond-All-Reason`) for `GetUnitEstimatedPath` returns exactly one caller: `luarules/gadgets/unit_weapon_smart_select_helper.lua` (lines 73 and 250). That file is a SYNCED gadget — it early-returns for the unsynced half at line 15 (`if not gadgetHandler:IsSyncedCode() then return end`). A synced caller runs `LuaSyncedRead::GetUnitEstimatedPath`, which reads the live `pathManager` directly and does NOT touch the snapshot rows at all. No widget (LuaUI) and no unsynced gadget path references the callout anywhere in the tree.

2. **Callout census.** Every replay census under `/www/projects/bar-data2` and `/www/projects/bar-data` records `GetUnitEstimatedPath,0,0,0,0` (columns `count,count_draw,count_sim,count_other`) — zero invocations across ~44k-frame late-game replays (e.g. `bar-data2/atg_callouts.csv:553`, `bar-data2/rosetta_callouts.csv:559`, `bar-data2/livecensus_atg_callouts.csv:609`, `smoke_callouts.csv:270`, and every other census file). The census instruments the served (draw/unsynced) surface; the sole synced caller does not appear because it bypasses the snapshot.

Conclusion: in stock BAR the snapshot est-path extraction serves ZERO queries and its steady-state cost under a demand gate is essentially zero (one byte-vector clear per epoch — see §4). The first-touch machinery below is a correctness safety net for non-stock addons (or the diff-gate dual-run), not a hot path.

## 3. Mechanism (DG — first-touch demand gate, pieces precedent)

Copy the PR 46 pieces read-set pattern (LuaSnapshotServe.cpp:7275-7292, 7390-7433, 7786-7861), specialized to est-path. The producer captures a waypoint block ONLY for unit ids that have been queried at least once; everything else pays a single byte write per epoch.

### 3.1 Granularity: id-granular, no whole-family arming

The register unit is the individual unit id (a `std::vector<uint8_t> estPathReadSet` [maxUnits], producer-owned, exactly like `unitPieceReadSet` at LuaSnapshotServe.cpp:7284). Whole-family arming (extract est-path for ALL ground units the instant ANY query arrives) is rejected — it would re-introduce the O(world) gather that this workstream removes. Unlike pieces, est-path needs NO coarse `unitPieceDemandSeen`-style gate and NO pre-registration heuristic: the nano-job pre-registration (LuaSnapshotServe.cpp:7817-7834) exists because BAR's gl4 nano widget predictably queries a lathing unit's pieces on the first draw frame of a job; est-path has no analogous predictable-first-touch consumer, so the gate is pure lazy first-touch. This makes WS-6 structurally SIMPLER than the pieces gate.

### 3.2 Producer side (edge, on the sim thread)

In the `UnitsBuildMove` pass, replace the unconditional est-path copy in `ExtractUnitMoveType` with a read-set check. Concretely (schematic):

- Drain the draw→producer pending-registration mailbox (`pendingEstPathRegs`, mutex-guarded, mirroring `pendingUnitPieceRegs` at LuaSnapshotServe.cpp:7786-7804) into `estPathReadSet` before the pass. The read-set/mailbox live in the serving layer (LuaSnapshotServe) alongside the piece read-set, since that is where first touch is detected; the producer hands `estPathReadSet` to `Extract` (or `Extract` consults it) the same way the piece producer consults its own read-set.
- For each active ground unit `id`: if `estPathReadSet[id]` is unset, write `rows.estPathCaptured[id] = 0` and skip the copy entirely. If set and the unit still has a ground move type: do the existing `GetPathWayPoints` copy AND set `rows.estPathCaptured[id] = 1`. Prune on the fly: if a registered id is dead / no longer ground-move, clear `estPathReadSet[id]` (mirrors the produce-loop prune at LuaSnapshotServe.cpp:7853-7857).
- The always-present scalar `MoveTypeBlock` / `moveTypeKind` / `mtGoalPos` fields (SimSnapshot.cpp:1030-1118) are unchanged — only the variable-length waypoint block is gated.

`estPathCaptured` is a new `std::vector<uint8_t>` [maxUnits] field on `UnitRows` (declared alongside the est-path block at SimSnapshot.h:461-472). It is per-ring-slot (it lives inside `UnitRows`, which is `buffers[EPOCH_RING_SLOTS]` at SimSnapshot.h:1476) — see §5 for why that matters. Note the distinction from pieces: piece slots live in a SEPARATE producer-owned cache array keyed by `HeldSlot()` (LuaSnapshotServe.cpp:7270-7273), whereas est-path data already lives inside the `UnitRows` ring, so the "captured" marker rides the ring too rather than needing a parallel cache.

### 3.3 First touch (draw thread) — value served THIS frame must be correct

`LuaSnapshotServe::GetUnitEstimatedPath` (LuaSnapshotServe.cpp:10420) is amended to mirror `GetUnitPieceSlot`'s first-touch structure (LuaSnapshotServe.cpp:7406-7432):

1. Run the existing POV / validity / move-type gates FIRST, against always-present rows: `rows.Valid(unitID)`, `rows.PovAlliedUnit(...)`, `rows.moveTypeKind[unitID] != 1` (LuaSnapshotServe.cpp:10427-10432). These reject invalid / non-allied / non-ground ids and return 0 WITHOUT any park.
2. If `rows.estPathCaptured[unitID]` is set (registered and captured into this held slot): serve from rows exactly as today — `PushPathNodesFromSnap(L, rows.estPathPoints[unitID], rows.estPathStarts[unitID])`.
3. Otherwise this is a FIRST TOUCH of a valid allied ground unit not yet captured in this slot: enqueue `unitID` into `pendingEstPathRegs` (mutex), take `CGame::ScopedExternalSimPause{SimPauseSite::EST_PATH_FIRST_TOUCH}`, then read the LIVE path and serve it directly — under the park the sim is quiesced, so `pathManager->GetPathWayPoints(g->GetPathID(), ...)` on the live `CGroundMoveType` is safe (same safety model as the pieces first-touch reading live `unitHandler`/`localModel`). Push those freshly-read local vectors via `PushPathNodesFromSnap`. This yields the exact current-frame value even though the held slot's rows lack it. The id is now registered, so the producer captures it at every subsequent edge until it dies.

A live unit with a ground move type but `pathID == 0` (no active path) is a valid registered case: first touch computes `hasPath == 0` live, serves 0 tables (matching the `estPathHasPath == 0` early return at LuaSnapshotServe.cpp:10435), and registers. Next epoch the producer sets `estPathCaptured[id] = 1` with `estPathHasPath[id] = 0`. So "no path" is a captured state, not a perpetual miss.

### 3.4 The pieces dead-miss / tombstone lesson

The pieces gate needed a `deadMiss` tombstone (LuaSnapshotServe.cpp:7250-7252, 7402-7404, 7420-7424) because `GetUnitPieceSlot` parks on ANY unregistered id before it can tell whether the object is alive; a dead-but-queried id would re-park every frame forever (the "re-park storm" fixed in commit 7c367fe65d). WS-6 structurally avoids this: the POV/validity/`moveTypeKind` gates in §3.3 step 1 run against always-present rows BEFORE any park, so dead ids (`!rows.Valid`), enemy ids (`!PovAlliedUnit`), and non-ground ids (`moveTypeKind != 1`) all return 0 with no park and no registration. The only id that reaches the park is a valid, allied, GROUND unit not yet captured — and registration + the "no path is a captured state" rule (§3.3) guarantee it becomes captured within a few epochs and stops parking. No est-path tombstone is required.

Residual (bounded, accepted, identical to pieces): between the first-touch frame and the epoch at which the newly-captured slot rotates into the draw's held slot (`EPOCH_RING_SLOTS == 3`, SimSnapshot.h:1439), repeated queries of the same fresh id re-park because `estPathCaptured` is still 0 in the older held slots. This is the same short transient the pieces gate accepts; with stock BAR issuing zero queries it never occurs. `[SimPauseSurvey]`/`[SimParkStats]` telemetry (per §7 of the overview) will count `EST_PATH_FIRST_TOUCH`; expected 0 in stock BAR.

### 3.5 Kill the `starts` temp (same PR)

Independent trivial fix at SimSnapshot.cpp:1070-1072. Preferred: change `UnitRows::estPathStarts` (SimSnapshot.h:472) from `std::vector<std::vector<int32_t>>` to `std::vector<std::vector<int>>` and pass `rows.estPathStarts[id]` straight into `GetPathWayPoints` (whose signature is `std::vector<int>&`, HAPFS PathManager.h:170 / IPathManager.h:108) — this deletes BOTH the per-unit temp alloc and the `.assign` copy. `int32_t == int` on every supported target (as the existing comment at SimSnapshot.cpp:1068-1069 already notes), so the diff-gate compare (SnapshotDiffGate.cpp:829-830) and the serve (`starts[i] + 1` at LuaSnapshotServe.cpp:10411) are unaffected. Fallback if the type change ripples too far: hoist a single reused scratch `std::vector<int>` out of the pass loop and `.clear()` it per unit. Either eliminates the allocation churn; with the demand gate this now only runs for registered ids anyway, so the churn is largely gone regardless.

## 4. New SimPauseSite enum value

Extend the `CGame::SimPauseSite` enum (rts/Game/Game.h:117-138) with one telemetry-only value, appended immediately before `COUNT` to preserve the id stability the enum comment requires (Game.h:130-132):

```cpp
	PIECE_FIRST_TOUCH,   // (existing)
	EST_PATH_FIRST_TOUCH,// WS-6: read-set est-path serving, first query of an
	                     // unregistered ground unit (rare; 0 in stock BAR)
	COUNT
```

The tag is passed to `ScopedExternalSimPause` at the first-touch site (§3.3) and is dumped by `DumpSimPauseSurvey` (Game.h:139). No behavior beyond telemetry attribution.

## 5. Ring / slot lifecycle analysis

The est-path payload and the new `estPathCaptured` marker both live INSIDE `UnitRows`, which is the epoch ring itself (`buffers[EPOCH_RING_SLOTS]`, SimSnapshot.h:1476; `EPOCH_RING_SLOTS == 3`, SimSnapshot.h:1439). The producer writes the slot it is currently building; the draw serves from the held slot (`Read()` → `buffers[HeldIdx()]`, SimSnapshot.h:1274). These are distinct slots while a consumer holds one, which is what makes the design race-free (§6).

- **Registered id captured per epoch into each slot.** Each produce writes `estPathCaptured[id]` for the slot it builds: 0 for unregistered ids, 1 (with the fresh waypoint copy) for registered live ground units. As slots rotate, a registered id's data is refreshed in whichever slot is being built, so all three slots converge to captured=1 with current-frame data within `EPOCH_RING_SLOTS` epochs of registration.
- **Late-registered id must not serve garbage from older slots.** A newly registered id has `estPathCaptured == 0` in the older held slots (it was never extracted there; the residual `estPathPoints[id]` vector in those slots holds whatever a PRIOR occupant left). The serve gates strictly on `estPathCaptured` (§3.3 step 2): captured==0 → the stale `estPathPoints[id]` in that slot is NEVER read; the serve takes the first-touch live path instead. So a late-registered id can never emit an older slot's stale/foreign waypoints.
- **Prune on death + id reuse.** When a registered unit dies, the produce-loop prune clears `estPathReadSet[id]` (mirroring LuaSnapshotServe.cpp:7853-7857). Thereafter each rebuilt slot sets `estPathCaptured[id] = 0`, so the marker decays to 0 across all slots as they rotate. An id reused by a NEW unit is unregistered (the old unit's death cleared it), so the new unit's own first touch re-registers it — exactly the pieces "a reused id re-registers through its own first touch" contract (LuaSnapshotServe.cpp:7844-7846). The stale `estPathPoints[id]` vectors from the dead unit are never served because `estPathCaptured` is 0 until the new unit is captured. The only staleness window is the ordinary held-slot-age window every snapshot field shares.
- **creg / load.** Est-path rows are already excluded from `SnapshotHash` (SimSnapshot.h:461-466, variable-size synced state) and are re-extracted from the live world every epoch; the read-set is a runtime serving-layer structure (not serialized), so a checkpoint load simply starts with an empty read-set and re-arms on first touch. No creg surface changes.

## 6. Threading argument

The read-set + pending-registration mailbox reproduce the proven pieces model exactly:

- `estPathReadSet` is producer-owned; it is read and written only on the sim/producer thread at the edge. No draw-thread access.
- The draw thread's only cross-thread write is `pendingEstPathRegs.push_back(unitID)` under `pieceRegMtx` (or a sibling mutex); the producer drains it under the same lock before the pass (mirroring LuaSnapshotServe.cpp:7786-7804). This is the identical mailbox discipline already shipping for pieces.
- `estPathCaptured` and the est-path vectors are ordinary `UnitRows` fields: the producer writes the produce slot, the draw reads the held slot — disjoint slots under the ring's single-writer invariant, so no atomics are needed (same argument as every other `UnitRows` column, overview §6).
- The first-touch live reads (`unitHandler.GetUnit`, `pathManager->GetPathWayPoints`) execute under `ScopedExternalSimPause`, which quiesces the sim thread; this is the same quiescence the pieces first-touch relies on for its live `localModel` reads (LuaSnapshotServe.cpp:7417-7432). No new concurrent access to `pathManager` is introduced (the diff-gate dual-run already reads `pathManager->GetPathWayPoints` from the edge, SnapshotDiffGate.cpp:821).

WS-6 adds no lock-free cross-thread sharing and no new `for_mt`; it is orthogonal to the WS-8 MT ruling.

## 7. Verification plan

- **Existing diff-gate family.** `D_ESTPATH` / `"unit:estPath"` already exists (rts/Rendering/Common/SnapshotDiffGate.h:264; SnapshotDiffGate.cpp:810-834). It currently compares `rows.estPath*` against a live `GetPathWayPoints` for EVERY valid unit unconditionally. After demand-gating, unregistered ids carry `estPathCaptured == 0` and no populated rows, so the gate would false-positive on every unregistered ground unit with a path. The gate MUST be made read-set-aware: for `estPathCaptured[i] == 0`, skip the rows-vs-live compare (that id is served live under park, which is tautologically equal to the live read — there is nothing captured to diverge). For `estPathCaptured[i] == 1`, keep the exact current compare (bit-equal points, exact starts). This makes the gate assert the demand-gate contract: captured rows == live; uncaptured ids are served live. A cheap way to also cover the uncaptured serving path is an oracle-style assertion that first-touch's live serve equals a re-read — but since first touch reads live directly, it is correct by construction; the read-set-aware gate is sufficient.
- **Strict-mode no-denial.** Under the split's deny-until-served contract, the twin must never DENY a query it previously served. The first-touch path (§3.3) always serves a valid answer (live under park) for any id that passes the POV/move-type gates, so no query that would have been served by the eager extractor is denied. The enumerated one-window deviation is the same as pieces: if a unit dies AFTER an epoch edge but BEFORE the draw's first touch, the live read returns "no ground move / no path" and serves 0 — the eager extractor would have served its edge-time capture for that one dispatch window. This matches the documented pieces first-touch deviation (LuaSnapshotServe.cpp:7406-7412) and is within contract.
- **Demo-resim + benchmarks.** Per program rule there is no A/B pixel gate for decoupling work. Verification is: (a) headless demo-resim clean (the extraction is a pure const read of already-synced state; removing it for unqueried ids cannot perturb sync), (b) the late-game replay benchmark reporting the `UnitsBuildMove` zone before/after so the §1 baseline row can be updated in place, (c) `[SimPauseSurvey]` showing `EST_PATH_FIRST_TOUCH == 0` for stock BAR replays (confirming the gate is fully lazy), and (d) an addon-driven spot check: force-query `Spring.GetUnitEstimatedPath` from a test widget over a set of moving ground units with the diff-gate armed, confirming zero `unit:estPath` mismatches and correct first-touch serving.

## 8. Staging

Single small PR:

1. Add `UnitRows::estPathCaptured` field + resize (SimSnapshot.h:461-472, `Resize`), and (preferred) retype `estPathStarts` to `std::vector<std::vector<int>>`.
2. Add `estPathReadSet` + `pendingEstPathRegs` mailbox in the serving layer next to the piece read-set (LuaSnapshotServe.cpp:7284-7288 region); drain/prune in the producer path that feeds `Extract`.
3. Demand-gate the est-path copy in `ExtractUnitMoveType` (SimSnapshot.cpp:1042-1073); kill the `starts` temp.
4. Add the first-touch park + live serve in `LuaSnapshotServe::GetUnitEstimatedPath` (LuaSnapshotServe.cpp:10420).
5. Add `SimPauseSite::EST_PATH_FIRST_TOUCH` (Game.h:117-138) and tag the park site.
6. Make `D_ESTPATH` read-set-aware (SnapshotDiffGate.cpp:810-834).

No dependency on any other WS. Does not block or depend on WS-1/2/3 (different pass/field).

## 9. Expected numbers

Baseline `Update::SimSnapshot::UnitsBuildMove` = 778 µs (§1), of which the est-path copy (`GetPathWayPoints` + three `float3` list copies + the `starts` temp + `.assign`, per ground unit with an active path) is the variable, world-scaling share; the remainder is the fixed `ExtractUnitBuildState` + `MoveTypeBlock` scalar writes.

- **Stock BAR (0 queries, per §2):** the est-path read-set is empty, so the producer extracts est-path for zero units. `UnitsBuildMove` drops by its entire est-path share — estimated ~0.3-0.5 ms late-game (many units with active paths) — leaving only the always-on scalar passes (~0.3-0.45 ms; further reducible by WS-3 stage 5). Per-epoch est-path cost becomes one `estPathCaptured` byte-vector clear plus zero captures ≈ a few µs. The `starts` temp allocation is eliminated for all callers.
- **Addon that DOES query est-path:** cost scales with the (small) queried read-set, not with world size — only the handful of registered ids are copied, plus a bounded number of first-touch parks (counted under `EST_PATH_FIRST_TOUCH`).

Net: WS-6 removes the est-path share of `UnitsBuildMove` outright for stock BAR, contributing to the quick-wins batch (WS-4..7, ~2 ms combined) toward the program's ~3-4 ms target.
