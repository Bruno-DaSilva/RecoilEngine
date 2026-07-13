# Placement rehost — stage 2 design (state-view refactor of the live predicate stack)

**Status: DESIGN LOCKED. Build path implemented + compiling; move path pending.** Stage 1 (mirror layers + occupant rows) is landed and validated — commit `b23e6859d9` on `epoch-integration`, armed diff-gate flag-off full-length Rosetta PASS (all 9 new field passes 0 mismatches, 0 DESYNC). This doc is the binding design for stage 2, the "one implementation, two backends" refactor (plan §3). Operator rulings locked: **full scope incl. the move path / pathfinding refactor**; deviation swap confirmed conceptually (record wording in stage 4).

### Progress checklist (stage 2)
- [x] `rts/Game/PlacementPredicates.h` — `namespace placement` with `LiveView` (occupant = `const CSolidObject*`) + templated `TestBlockSquareForBuildOnlyT` / `TestBuildSquareT` / `TestUnitBuildSquareT`. Compiles clean (linked in GameHelper TU).
- [x] `CGameHelper::TestBlockSquareForBuildOnly` / `TestBuildSquare` / `TestUnitBuildSquare` rewired as `LiveView` wrappers. UI (`commands != nullptr`) path + the synced slow-update block kept as live code; the read-only test routes through the template. Feature out-param threaded as `int featureId`, resolved via `featureHandler.GetFeature`.
- [x] **Build-path byte-identity resim** (flag-off, full-length ATG f=44887) — 0 DESYNC. Committed `d681f37f63`.
- [x] **Move path (pathfinding templatization)** — DONE. `rts/Sim/MoveTypes/MoveMath/MoveMathPredicates.h` (`movemath::IsNonBlockingT/CrushResistantT/ObjectBlockTypeT/GetPosSpeedModT`); the 4 `CMoveMath` leaf members are `LiveView` wrappers. Cell-iteration (`SquareIsBlocked`/`RangeIsBlocked*`/`TestMoveSquare` + `mtTempNum`) UNTOUCHED — auto-routes through the leaves. `*SpeedMod` helpers moved protected→public. `LiveView` extended with move-path occupant + terrain accessors.
- [x] **Full-stack byte-identity resim Rosetta** (flag-off, full-length f=44537) — 0 DESYNC, no crash. Hot synced pathfinding exercised heavily.
- [ ] **Full-stack byte-identity resim ATG** (solo; the concurrent run died on a load-race SIGSEGV during `[Game::Load]`, only libc frames — not our code) — RUNNING.
- Note: concurrent headless load-race → run the two replays SEQUENTIALLY, not concurrently (content-extraction race at load).

### Remaining: move-path templatization (member-wrapper pattern, zero caller changes)
Convert these to templated bodies in a header (light — occupant reads via view), members become `return FooT(placement::LiveView{}, …)` wrappers. These are READ-ONLY (return block-type / speed-mod), so desync risk = a divergent return value, not a reordered mutation — the flag-off resim is the arbiter.
- `CMoveMath::IsNonBlocking` → `IsNonBlockingT` (occupant water/height/blocking/moveDef via view; collider is a local `CheckCollisionQuery`, read directly).
- `CMoveMath::CrushResistant` → `CrushResistantT` (occupant crushable/crushResistance/solid-bit).
- `CMoveMath::ObjectBlockType` → `ObjectBlockTypeT` (→ IsNonBlockingT, CrushResistantT; occupant immobile/isMoving/isPushResistant/isIdle).
- `CMoveMath::GetPosSpeedMod` (both overloads) → `GetPosSpeedModT` (view: TypeMapAt/MaxHeightAtSquare/Slope/CenterNormal2D/TerrainType; SpeedMod helpers stay pure).
- `CMoveMath::SquareIsBlocked` / `RangeIsBlocked*` → `…T` (view.FullCellCount/FullCellObj; drop mtTempNum — re-OR each square, OR-idempotent).
- `MoveDef::TestMoveSquare` / `TestMoveSquareRange` → `…T` (compose GetPosSpeedModT + RangeIsBlockedT; `CheckCollisionQuery::UpdateElevationForPos(int2, float maxHgt)` value-passing overload fed by view.MaxHeightAtSquare).
- Extend `LiveView` with the move-path accessors (MaxHeightAtSquare, CenterNormal2D, TypeMapAt, TerrainType, FullCellCount/FullCellObj, the occupant crush/idle/pushResist/water reads) — the EpochView (stage 3) supplies the mirror-backed versions.
Gate: the ONE flag-off both-replay full-length resim (0 DESYNC) covers build + move together.

## Goal of stage 2 (and ONLY stage 2)

Refactor the live predicate stack into templated pure functions over an abstract `View`, instantiated with `LiveView` only. **No epoch view, no behavior change, no serving.** The sole deliverable is: identical behavior, now templated, so stage 3 can add `EpochView`. Hard requirement: **byte-identical** (these run in synced pathfinding every frame — any float-op reorder desyncs the GAME, not just a Lua preview). Gate: flag-off full-length resim on BOTH replays, 0 DESYNC (user chose one gate for the whole stack).

## The pattern: member-wrapper, zero caller changes

Each existing member function keeps its exact signature and becomes a thin wrapper:
```cpp
CGameHelper::BuildSquareStatus CGameHelper::TestBuildSquare(pos, xr, zr, bi, md, CFeature*& feature, at, synced) {
    int fid = -1;
    auto ret = placement::TestBuildSquareT(placement::LiveView{}, pos, xr, zr, bi, md, fid, at, synced);
    feature = (fid >= 0) ? featureHandler.GetFeature(fid) : nullptr;   // resolve id->ptr for the live API
    return ret;
}
```
Pathfinding/MobileCAI/Factory/GroundMoveType callers are UNCHANGED (19 TestMoveSquare sites, 15 GetPosSpeedMod sites keep calling the member). The templated body is in a header so both LiveView (here) and EpochView (LuaSnapshotServe, stage 3) can instantiate it. LiveView accessors are inline forwarders to today's exact expressions → the compiler inlines → identical codegen.

## Reductions discovered during the audit (shrink the surface)

1. **`GetBuildHeight`/`Pos2BuildPos` need NO templatization.** For `synced=false` (the draw/epoch path) `GetBuildHeight` reads the *unsynced* corner heightmap (draw-safe) and takes `currHeightBoundsOverride` (the PR-29 SimSnapshot currHeightBounds mirror). The epoch path calls `GetBuildHeight(pos, def, false, &epochBounds)` directly. The view exposes `float BuildHeight(pos, def, synced) const` wrapping this (LiveView → `nullptr` override; EpochView → `&epochBounds`).
2. **`CheckTerrainConstraints` and `GetYardMapIndex` are already pure** (scalars + def data, no globals). Call them directly from the templated bodies; do not templatize.
3. **`CheckCollisionQuery::UpdateElevationForPos` gets a value-passing overload** `UpdateElevationForPos(int2 sqr, float maxHgt)` (one impl, height fed from live `readMap` or the mirror) — no templatization of the struct. The existing `(int2 sqr)` overload calls it with `readMap->GetMaxHeightMapSynced()[...]`.
4. **`GroundSpeedMod`/`HoverSpeedMod`/`ShipSpeedMod` are pure** (moveDef, height, slope, dirSlopeMod) — call directly.
5. **`TestUnitBuildSquare`'s `if (synced)` slow-update block is synced-only** (skipped when `synced=false`), so the epoch path never reaches its `quadField` reads. The `needGeo` geo-feature `quadField` query IS reached both ways → view method `bool HasNearbyGeoFeature(testPos, dist)` (LiveView → quadField; EpochView → SnapshotPickGrid::QueryFeaturesInRadius + featureDef->geoThermal).
6. **The `mtTempNum` cross-square dedup is a pure perf optimisation** (ObjectBlockType is OR-idempotent). The templated RangeIsBlocked drops it and re-ORs each square's full cell; the full-cell mirror (stage 1) supplies the objects. Byte-identical RESULT (the OR value is unchanged).

## The `View` interface (union of the whole read-set)

`Occupant` = `const CSolidObject*` (LiveView) / `{int id, uint8_t kind}` (EpochView).

Terrain / map:
- `float ApproxHeightUnsafe(int sqx, int sqz, bool synced) const`
- `float Slope(float x, float z, bool synced) const`
- `float MaxHeightAtSquare(int sqr) const`
- `float3 CenterNormal2D(int sqr) const`
- `int   TypeMapAt(int halfSquare) const`
- `const CMapInfo::TerrainType& TerrainType(int idx) const`
- `bool  BuildingMaskTest(int hx, int hz, uint16_t mask) const`
- `bool  YardBlockBuilding(int x, int z) const`
- `bool  InLos(const float3& pos, int allyTeam) const`
- `float BuildHeight(const float3& pos, const UnitDef* def, bool synced) const`
- `bool  HasNearbyGeoFeature(const float3& testPos, int mindx, int mindz) const`

Occupant access:
- `Occupant GroundBlocked(int x, int z) const`                 // cell[0], build path
- `int  FullCellCount(int x, int z) const`                     // move path
- `Occupant FullCellObj(int x, int z, int i) const`

Occupant attributes (all keyed by `Occupant`):
- `bool OccNull / OccIsFeature / OccIsUnit(Occupant) const`
- `int  OccId(Occupant) const`
- `bool OccFeatureInLos(Occupant, at) / OccUnitInLos(Occupant, at) const`
- `bool OccFeatureReclaimable(Occupant) const`                 // featureDef->reclaimable
- `bool OccImmobile / OccYardOpen(Occupant) const`
- `float3 OccPos(Occupant) const`  · `float OccHeight(Occupant) const`
- `bool OccIsInWater / OccIsUnderWater / OccIsMoving(Occupant) const`   // physicalState bits
- `bool OccHasSolidObjectsBit / OccIsBlocking(Occupant) const`          // blockingBits
- `bool OccCrushable(Occupant) const`  · `float OccCrushResistance(Occupant) const`
- `bool OccIsIdle / OccIsPushResistant(Occupant) const`
- `const MoveDef* OccMoveDef(Occupant) const`                  // moveDefID -> moveDefHandler
- `const YardMapStatus* OccBlockMap(Occupant) const`           // def + facing + yardOpen
- `int  OccXsize / OccZsize / OccBuildFacing(Occupant) const`

## Templatized functions (bodies -> header, members -> wrappers)

GameHelper: `TestUnitBuildSquareT`, `TestBuildSquareT`, `TestBlockSquareForBuildOnlyT`, `ClosestBuildPosT` (loop over `TestUnitBuildSquareT`).
MoveMath:  `IsNonBlockingT`, `CrushResistantT`, `ObjectBlockTypeT`, `GetPosSpeedModT` (both overloads), `SquareIsBlockedT`, `RangeIsBlockedT`.
MoveDefHandler: `TestMoveSquareT`, `TestMoveSquareRangeT`.

Cross-.cpp template calls (e.g. `TestBuildSquareT` in GameHelper calls `IsNonBlockingT` in MoveMath) force the callee templates into a header (`PlacementPredicates.h`); the bodies reference only the `View` interface + MoveDef/UnitDef/scalars (occupant derefs go through the view), so the header stays light — heavy includes (Unit.h/Feature.h/quadField/losHandler) live only in `PlacementStateView.h` where `LiveView`'s accessors are defined.

## Feature out-param

`TestBuildSquare`'s `CFeature*& feature` becomes `int& featureId` in the template (the reclaimable-blocker id, threaded through the footprint loop, last-setter wins). LiveView wrapper resolves `featureId -> CFeature*` via `featureHandler`; EpochView keeps the id (stage 3).

## Gate (stage 2)

Flag-off (`SimDrawSplit=0`), full-length, BOTH replays, `grep -c DESYNC infolog.txt == 0`. The armed diff-gate flag-off also passes trivially (no served value changed). No headful needed (no draw-side behavior yet). This is the ONE gate the operator chose for the whole stack.

## Stage 3 preview (NOT this stage)

Add `EpochView` (mirrors + rows, occupant = id+kind), armed dual-run epoch-vs-live per callout (bit-equal), serve TestBuildOrder/TestMoveOrder/ClosestBuildPos flag-on through the `…T` functions, delete the query/reply routing, convert the C++ build-preview park sites. Gates: armed diff-gate + strict contract + headful flag-ON.
