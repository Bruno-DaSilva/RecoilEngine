# Sim/draw split — draw-side placement-test re-host (plan)

**Status: UNSCHEDULED — evidence-gated.** Written 2026-07-10, following the external review of the query/reply design and the SnapshotPickGrid precedent discussion. This is the promotion plan for the §8 punch-list item "draw-side TestUnitBuildSquare verdict" (handbook `sim-draw-split-agent-handbook.md` §8; `sim-draw-pr45-followups-design.md` items 6/7). Do not start this work without an operator ruling; the scheduling trigger is below.

**Trigger to schedule:** the operator's live-skirmish gate (handbook §5.8 — replays cannot drive interactive input) shows perceptible build-preview verdict lag or flicker — specifically: red/green flicker when the cursor crosses build-grid cells, first-touch conservative-default flashes, or `ClosestBuildPos`/`TestMoveOrder` visibly trailing the cursor. If live play shows nothing perceptible, this plan stays shelved: the query/reply design is strictly cheaper and sim-exact.

**Companion plan:** `sim-draw-trace-rehost-plan.md` (weapon trace tests). Independent; this one is the priority if both ever trigger, because placement is the mouse-coupled family.

## 0. Read-first / environment (fresh-agent preamble)

- Read `doc/sim-draw-split-agent-handbook.md` FIRST — §4 (environment facts: worktree, build, replays, datadirs, flags) and §5 (gate recipes) govern all work here. Work in the worktree `/www/projects/RecoilEngine/.claude/worktrees/epoch-integration`; never edit the main checkout's tracked files.
- **Path corrections vs handbook §2 (verified 2026-07-10):** `SimSnapshot.{h,cpp}`, `SnapshotHash.{h,cpp}`, `SnapshotDiffGate.{h,cpp}`, and `DrawMapMirrors.{h,cpp}` all live in `rts/Rendering/Common/` — NOT `rts/Sim/Misc/` as the handbook says. `LuaSnapshotServe.{h,cpp}` and `LuaSplitContract.{h,cpp}` are in `rts/Lua/` as stated.
- Binding prior rulings this plan must not contradict: contract-refresh §7.3 (query keying), specs §E.2 (serving recipe + choke-point discipline), the PR 29 escalation (no forked recompute — §2 below).
- Live-code anchors (verified): `CGameHelper::TestUnitBuildSquare` at `rts/Game/GameHelper.cpp:1362`, `TestBuildSquare` at `:1518`; the placement channel contract comment at `rts/Lua/LuaSnapshotServe.h` (block starting ~line 658, "PR 38e"); the blocking-map mirror contract at `rts/Rendering/Common/DrawMapMirrors.h` (~lines 94-100 + `BlockedAt` ~line 200); the mirror choke-point list (`Mark*Dirty`) at `DrawMapMirrors.h` ~lines 105-140.

## 1. What exists today (the mechanism this replaces)

`Spring.TestBuildOrder` / `TestMoveOrder` / `ClosestBuildPos` are served by the sim-side placement query/reply channel (PR 38e, `LuaSnapshotServe.h` ~line 658 block; the contract-refresh doc's `:628` is a stale line number): flag-on, the callout enqueues a query and returns the last boundary's sim-exact reply; `EvaluatePlacementQueries()` runs the EXACT live predicate at the frame edge. Flag-off, `RoutePlacementQuery` runs the live body inline (bit-identical to master).

Keying (operator-ruled, PR 43 §7.3, contract-refresh doc): `TestBuildOrder` keyed by build-grid-SNAPPED pos (verdict is grid-quantized, so within-cell cursor motion is a warm hit — correct values while the cursor moves; only a fresh-cell crossing pays one boundary); `ClosestBuildPos`/`TestMoveOrder` use the standing-latest-query model (≤1 boundary late, never-default after the first boundary).

Enumerated deviations carried today: (a) replies reflect a ≤1-boundary-old cursor position; (b) same-frame same-key different-pos queries collide, last registered wins.

Separately, the C++ (non-Lua) preview paths are NOT on the channel — they take input-gated narrow parks: `GuiHandler.cpp:4007/4077/4203` (DrawMapStuff build-preview / weapon-range interior reads, Prereq D residual) and the punch-list item-7 set (`TestUnitBuildSquare` minimap proxy, `GetBuildPositions`, …). ≈0 engages in replays; a live builder-heavy game is what would light them up.

## 2. Why the naive alternative was rejected (and stays rejected)

The PR 29 escalation (recorded in the `LuaSnapshotServe.h` placement block) rejected a draw-side *recompute* because: (a) a forked reimplementation of `CGameHelper::TestUnitBuildSquare` + `CMoveMath` drifts, and the outputs are DISCRETE (BLOCKED/OPEN/RECLAIMABLE/OCCUPIED) — "nearly correct" means crisply wrong in some cells, concentrated at decision boundaries where the player is actually looking; (b) there is no epsilon gate for an enum, so the diff-gate methodology goes blind on a fork; (c) `Route()` serves the value even flag-off, so an approximate body would break flag-off byte-identity.

This plan does NOT revisit that rejection. It replaces the *fork* with a *parameterization* — see §3. A copied/approximated implementation remains off the table.

## 3. Binding design principle: one implementation, two state backends

Refactor the live predicate stack into pure functions over an abstract state view, instantiated twice:

- **Live view** (sim thread + flag-off): backed by `CGround`/`groundBlockingObjectMap`/`losHandler`/live objects — byte-identical behavior, zero-overhead instantiation (templates/static dispatch, NO virtuals in the hot path — `TestUnitBuildSquare` is called from synced builder logic every frame).
- **Epoch view** (draw thread, flag-on): backed by the epoch's mirrors and rows.

Functions to parameterize: `CGameHelper::TestUnitBuildSquare` → `TestBuildSquare` → (`CheckTerrainConstraints`, `GetYardMapIndex`, `TestBlockSquareForBuildOnly`), `CGameHelper::ClosestBuildPos`(+`Pos2BuildPos`), `MoveDef::TestMoveSquare`, and the `CMoveMath` reads they hit (`IsNonBlocking`, `GetPosSpeedMod` chain).

Why this restores the verification story instead of abandoning it: in the armed flag-off dual-run, epoch state == live state at the boundary (same thread, same frame edge), so the epoch-backed instantiation must match the live one BIT-FOR-BIT — the existing SnapshotDiffGate dual-run enforces it. The only flag-on difference is then input staleness (the epoch is one frame edge old) — the same enumerated-deviation class as every other served value in the split. The approximation is relocated from logic (unboundable, untestable) to data age (bounded at one frame, already the system-wide contract).

Precedent that the pattern works on exactly this code family: `SnapshotPickGrid` + `CCollisionHandler::MouseHit` already run dual-use (live-backed sim-side, snapshot-backed draw-side) for all picking under the split (PR 25).

## 4. State-view inventory (what the epoch backend needs)

Reads of `TestBuildSquare` + `TestUnitBuildSquare` + `ClosestBuildPos` + `TestMoveSquare`, audited against what the epoch already carries:

| State read | Live source | Epoch status | Work |
|---|---|---|---|
| Ground height (approx + real), water | `CGround` over heightmaps | Heightmap mirrored w/ dirty rects | Epoch-backed `CGround` accessor shims (may partially exist for GuiTraceRay ground march — audit first) |
| Slope | `CGround::GetSlope` (slopemap/centernormals) | NOT mirrored | New mirror layer; terraform-driven, piggyback the existing heightmap dirty-rect choke |
| Typemap / terrain speedmod inputs | `readMap` typemap + slope + height | Typemap mirrored | Speedmod recomputes from mirrored layers once slope lands |
| Building mask | `buildingMaskMap` | NOT mirrored (verified: absent from the `Mark*Dirty` choke list) | New mirror; Lua-mutated, rare → version-per-layer whole copy is fine |
| Yardmap open/closed status | `yardmapStatusEffectsMap` | NOT mirrored (verified: absent from the choke list) | New mirror; mutates on factory yard open/close → footprint-rect dirty tracking (blocking-map precedent) |
| Blocking-map occupant | `groundBlockingObjectMap.GroundBlocked()` → bare `CSolidObject*`, then dereferenced | **LANDED (PR 29):** the mirror already stores per-cell occupant id + kind (`BlockedAt(x, z, kindOut)` → id + `BLOCK_KIND_UNIT/FEATURE/NONE`), cell[0] semantics matching the live `GroundBlocked` dynamic_cast chain — the header comment says it was built for "the placement twins" | None on the mirror itself; attribute reads JOIN the id against rows (never a pointer) |
| Occupant attributes: pos, immobile, per-allyteam LOS status, physical/ground state, height/extents, moveDef id | live object fields | pos/LOS/immobile largely in rows (`rts/Rendering/Common/SimSnapshot.h` UnitRows/FeatureRows) | Read-set audit of `TestBuildSquare`/`IsNonBlocking` occupant dereferences vs rows; extend rows with the missing scalars (physical-state bits, extents, moveDef id) |
| Occupant def data: yardmap, reclaimable, crushability | unitDef/featureDef | Static, immutable | None — read defs directly |
| Querying-allyteam LOS (`losHandler->InLos`) | live LOS maps | Mirrored (7 types × allyteams) | None |
| Out-param feature id (`TestBuildOrder` returns a blocking feature) | live `CFeature*` | ids resolvable in epoch (ACTIVE / DEAD_THIS_BATCH) | Return the id from the joined row; id-coverage gate applies |

Every new/updated mirror layer needs the standard choke-point funnel + grep-audit table in the commit (specs §E.2 discipline), SnapshotHash membership, and a SnapshotDiffGate field pass.

Cost expectation (Addendum A rule — copy proportional to change, not world size): slope rides existing terraform dirty rects; mask/yard layers are low-churn; the blocking-map occupant upgrade reuses the landed footprint-rect incremental machinery. No new per-epoch cost class expected; verify with `/epochstats` before/after.

## 5. What gets retired on completion

- The placement query/reply channel (`RoutePlacementQuery`, `EvaluatePlacementQueries`, `ClearPlacementQueryChannel`, `PlacementKind`) and its two enumerated keying deviations. The trace channel (PR 35) STAYS — see companion plan.
- The GuiHandler build-preview interior narrow parks (`GuiHandler.cpp:4007/4077/4203`, build-square part) — the C++ preview path calls the epoch-backed instantiation directly. Weapon-range interior reads at the same sites belong to the trace/weapon-state plan.
- The punch-list item-7 input-gated parks in the placement family (`TestUnitBuildSquare` minimap proxy, `GetBuildPositions`).
- New enumerated deviation REPLACING the old ones: verdicts are computed at the CURRENT cursor position against ≤1-frame-old world state (query/reply was the inverse: exact state, stale position). Strictly better for mouse UI; still advisory — the authoritative test re-runs synced at command execution, unchanged.

## 6. Staging (single PR-stack, land in order)

1. **Mirror layers** (slope, building mask, yardmap status) + occupant-attribute row extensions from the §4 read-set audit (the blocking-map occupant id/kind is already landed — PR 29). Flag-off inert; armed diff-gate field passes prove them. No consumer yet.
2. **State-view refactor of the live stack** (pure functions over the view; live instantiation only). The dangerous step: touches synced hot code — any accidental float-op reorder breaks sync. MUST be byte-identical: gate with flag-off full-length resim on BOTH replays before proceeding. No behavior change intended anywhere.
3. **Epoch-backed instantiation + armed dual-run.** Wire the epoch view; dual-run epoch-vs-live in the armed gate (bit-equal required, flag-off). Serve the three Lua callouts through it flag-on; delete the query/reply routing; convert the C++ preview/park sites.
4. **Retirement + deviation bookkeeping.** Remove the channel, update the contract docs' deviation table, update handbook §8, record the operator ruling.

## 7. Gates (per handbook §5, plus specifics)

- Recipe 1 flag-off byte-identity after EVERY step (step 2 is the one that can realistically fail it).
- Recipe 2 armed diff-gate with the new field passes + the epoch-vs-live dual-run of all three callouts: PASS (0 mismatches) required.
- Recipe 3 strict contract: zero denials; the converted park sites must show zero engages in `[SimPauseSurvey]`.
- Recipe 4 headful flag-ON full-length, both replays at stack end.
- Live-skirmish re-verification of the original symptom (this plan only exists if live play showed one) — builder-heavy play, rapid cursor drags across blocked/free boundaries, queued line-builds, factory yard toggles under the cursor.
- `/epochstats` before/after: new mirror bytes and extraction time within the Addendum-A envelope (steady-state epoch production stays ~1–2 ms class).

## 8. Risks and open operator rulings

- **Synced-refactor risk (step 2)** is the dominant one; it is fully covered by the flag-off resim gate but expensive to iterate. Keep the refactor mechanical (lift reads behind the view; no logic edits in the same commit).
- **Zero-overhead requirement:** the live instantiation must compile to the current code (templates; verify no regression in the sim-frame profile — `TestUnitBuildSquare` is hot in builder-heavy games).
- **Ruling needed:** confirm the deviation swap in §5 (current-position/stale-state replaces stale-position/exact-state) as the recorded enumerated deviation.
- **Cell[0]-only granularity:** the mirror stores only the first blocking object per cell, matching live `GroundBlocked`'s return — verify during the read-set audit that no reachable path iterates the full cell object list (if one does, that's a mirror extension + choke audit, and worth flagging to the operator early).
- **Ruling needed:** PR numbering/slotting relative to whatever the program is doing when the trigger fires.

## 9. Non-goals

- No change to synced placement semantics: the synced execution-time test is untouched.
- No serving of the trace/weapon family here (companion plan).
- No attempt to serve `GetDefaultCommand`'s deep command-AI resolution — stays on its existing mechanism regardless of this plan.
- No refcount-shared mirror memory scheme (the §7.1 memory revisit stays a separate shelved item).

## 10. LANDED (2026-07-11)

Implemented and gated on `epoch-integration`. The placement predicate stack now
runs over an abstract state view, instantiated `LiveView` (sim, byte-identical) +
`EpochView` (draw, mirror/row-backed); all three placement callouts are served
draw-side from the published epoch.

**Commits (in order):**
- `b23e6859d9` (1) — mirror layers (center/max height, slope, centerNormals2D,
  build-mask, yard-status) + full-cell blocking mirror + occupant rows
  (immobile/yardOpen/physicalState/crushResistance/isIdle/isPushResistant on units,
  physicalState/crushResistance on features) + 9 SnapshotDiffGate field passes.
- `d681f37f63` (2a) / `c7137ab9f6` (2b) — templatize the build predicates
  (`placement::`) and the move leaf verdicts (`movemath::IsNonBlockingT/
  CrushResistantT/ObjectBlockTypeT/GetPosSpeedModT`) over the view; `CGameHelper`/
  `CMoveMath` members become `LiveView` wrappers (zero caller changes; the
  cell-iteration + `mtTempNum` machinery is untouched).
- `6a5a43254b` (3a) — `EpochView` + serve `TestBuildOrder`.
- `bfcd5c6986` (3b) — serve `ClosestBuildPos` (`ClosestBuildPosT`, explicit
  LiveView/EpochView instantiations in GameHelper.cpp).
- `d3ddceea70` (3c) — serve `TestMoveOrder` (`EpochTestMoveSquare` +
  `EpochRangeIsBlocked` over the full-cell mirror; value-passing
  `CheckCollisionQuery::UpdateElevationForPos(int2,float)`).
- `dbc7e933e8` — fix a latent stage-1 dead-shell segfault (isIdle/IsPushResistant
  deref freed commandAI/moveType in `ExtractDeadRowsFromShells`; only the flip
  producer hits it, so only a flag-ON run surfaced it).
- `eb35709eed` (4) — retire the PR 38e sim-side placement query/reply channel
  (now dead): removed ServePlacementQuery / EvaluatePlacementQueries /
  ClearPlacementQueryChannel / EvaluatePlacementQueryLive / the queue+reply maps /
  the keying helpers + call sites; surgically stripped placement from the
  trace-shared `EvaluateQueriesAtSimEdge`/`CommitStagedQueryReplies`. Kept
  BuildPlacementQuery/PushPlacementReply/PlacementQuery (arg-parse + reply-format).

**Design notes vs the original plan:**
- The current CENTER heightmap has no unsynced variant, so it needed a mirror too
  (beyond the plan's §4 list). `GetBuildHeight`/`Pos2BuildPos` did NOT need
  templatizing (synced=false reads the draw-safe unsynced corner map + the existing
  `currHeightBounds` override).
- The move path's cell-iteration is NOT shared: `EpochRangeIsBlocked` reimplements
  `RangeIsBlockedMt`'s ~10-line footprint walk over the mirror (dropping the
  `mtTempNum` dedup — `ObjectBlockType` is OR-idempotent, so the result is
  identical). Only the LEAF verdicts are the one shared implementation.
- Serving reuses the channel's arg-parse (`BuildPlacementQuery`) + reply-format
  (`PushPlacementReply`); the queue is replaced by synchronous `EvaluatePlacement-
  QueryEpoch`. `PlacementKindEpochReady` gated the per-callout migration.

**Gates:**
- Recipe 1 (flag-off byte-identity): stage-2 full-length resim BOTH replays
  (Rosetta f=44537, ATG f=44880), 0 DESYNC — the templatized synced predicate stack
  is bit-identical.
- Recipe 2 (armed dual-run epoch-vs-live): the driver widget drives all three
  callouts across the map/units/features every draw frame; segments showed
  `TestBuildOrder`/`ClosestBuildPos`/`TestMoveOrder` all 0 mismatches (184k/79k/79k
  checks), overall PASS. Full-length armed run: see the final gate.
- Recipe 3/4 (flag-ON strict serving): Rosetta f=12050, SIGSEGV=0, DESYNC=0,
  placement denials=0, no placement lua errors.

**Recorded enumerated deviation (the §5 swap — operator: build-preview family):**
> Build-preview placement verdicts (`Spring.TestBuildOrder`, `ClosestBuildPos`,
> `TestMoveOrder`) are now computed at the CURRENT query position against
> ≤1-boundary-old world state, REPLACING the query/reply channel's inverse
> (exact state, ≤1-boundary-old position). Strictly better for mouse-coupled UI
> (the cursor moves fast; the world barely changes in 1/30 s). Still advisory-only:
> the authoritative synced placement test re-runs at command execution, unchanged.

**Remaining follow-up (non-blocking — the served path + channel retirement are done):**
- Convert the C++ build-preview park sites (`GuiHandler.cpp` minimap build-proxy at
  ~:1128 + `ShowUnitBuildSquare` in UnitDrawer, the `TestUnitBuildSquare`
  commands-overload) to `EpochView` — templatize that overload for the epoch and
  drop the narrow parks. INTERACTIVE-ONLY: replays cannot drive build preview
  (handbook §5.8), so this cannot be headless-validated — it pairs with the
  operator's live-skirmish gate (below) rather than a diff-gate.
- Live-skirmish re-verification of the original symptom (interactive placement;
  replays cannot drive it — handbook §5.8). This is also where the park conversion
  above gets validated.
