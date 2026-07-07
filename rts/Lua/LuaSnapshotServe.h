/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

struct lua_State;
class CUnit; // sim|draw PR 30: CompareCmdQueueSlot's live-unit argument

/**
 * @brief LuaSnapshotServe -- snapshot-backed serving twins for draw-context Lua callouts
 *
 * PR 18 of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md
 * §E / §E.1b): the hot sim-state callout families are redirected, per handle
 * context, to read the published SimSnapshot instead of live sim objects when
 * invoked from a Draw* callin. This file owns the redirection pattern every
 * later family copies:
 *
 *  - ShouldServe(L) is the redirect predicate: an *unsynced* handle context
 *    (CLuaHandle::GetHandleSynced) currently inside a Draw* callin (the PR-2
 *    ScopedDrawCallinContext TLS bracket). Sim-phase and other-context calls
 *    keep the live path -- the same callout name serves both sides (census
 *    E.1: GetGameFrame is 39% sim-phase), so the branch must be per call, not
 *    per registered function.
 *
 *  - Each redirected callout gets a serving twin here: a line-by-line mirror
 *    of the live body with every sim-object read replaced by a SimSnapshot
 *    row read (and drawer-owned reads kept pointer-free via id-keyed drawer
 *    accessors). Signatures, argument parsing, error messages, return counts
 *    and float expression order are IDENTICAL by construction; UnitDef
 *    derefs (immutable game data) stay direct. Visibility gates and
 *    errorVector masking run against the snapshot's POV-complete masking
 *    inputs via the SimSnapshot::UnitRows Pov predicates and ErrorVector
 *    helpers -- see the masking-policy block in SimSnapshot.h.
 *
 *  - Uncovered ids (dead, out of range, spawned after the last boundary --
 *    impossible mid-draw in the single-threaded tree) return the same nil
 *    shape as the live path's "no such unit", deterministically; there is NO
 *    fallback read of live sim state from a served path. Divergence between
 *    the snapshot's valid set and the live one is a contract violation whose
 *    detector is the armed SnapshotDiffGate, not a per-miss warning (widgets
 *    legitimately query dead ids constantly).
 *
 *  - Route() wires the family through the diff gate: unarmed it simply picks
 *    live or twin; armed (test runs) it executes BOTH real paths, bit-compares
 *    the actual Lua return slots (masking included), reports through the
 *    gate's counters, and returns the snapshot-served values. No third copy
 *    of any formula exists in the verifier.
 */
namespace LuaSnapshotServe {
	// serving-twin signature: mirrors the live bodies' (lua_State, caller) shape
	using ServeFn = int (*)(lua_State* L, const char* caller);

	/// the redirect predicate (see above); cheap enough for every callout entry
	bool ShouldServe(lua_State* L);

	/// entry-point glue: live path when not draw-context, twin when it is,
	/// dual-run + compare + serve-twin when the SnapshotDiffGate is armed.
	/// `caller` doubles as the gate's per-callout counter name.
	int Route(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn);

	// serving twins (positions/status family, E.1b order: first family)
	int GetUnitPosition(lua_State* L, const char* caller);      // also GetUnitBasePosition
	int GetUnitHealth(lua_State* L, const char* caller);
	int GetUnitIsStunned(lua_State* L, const char* caller);
	int GetUnitViewPosition(lua_State* L, const char* caller);  // LuaUnsyncedRead

	// unit-family stragglers surfaced by the E.1b census (LosState is the
	// non-spectating hot one; Visible/Icon read drawer + camera + snapshot)
	int GetUnitLosState(lua_State* L, const char* caller);
	int IsUnitVisible(lua_State* L, const char* caller);        // LuaUnsyncedRead
	int IsUnitIcon(lua_State* L, const char* caller);           // LuaUnsyncedRead

	// projectile family (E.1b order: second family)
	int GetProjectilePosition(lua_State* L, const char* caller);
	int GetProjectileVelocity(lua_State* L, const char* caller);
	int GetProjectileDefID(lua_State* L, const char* caller);
	int GetProjectileTarget(lua_State* L, const char* caller);
	int GetProjectileOwnerID(lua_State* L, const char* caller);

	// team/player-table family (PR 26; served from the SimSnapshot team/player
	// boundary copy, section E.3 field spec -- POV gates via the TeamRows
	// IsAlliedTeam mirror)
	// global-scalar family (PR 27a; served from SimSnapshot::GlobalRows --
	// the no-object callouts hot at draw time: game clock, speed, wind,
	// cheat flags, ground extremes, global LOS)
	int GetGameFrame(lua_State* L, const char* caller);
	int GetGameSeconds(lua_State* L, const char* caller);
	int GetGameSecondsInterpolated(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetGameSpeed(lua_State* L, const char* caller);               // LuaUnsyncedRead
	int GetWind(lua_State* L, const char* caller);
	int IsCheatingEnabled(lua_State* L, const char* caller);
	int IsGodModeEnabled(lua_State* L, const char* caller);
	int IsEditDefsEnabled(lua_State* L, const char* caller);
	int AreHelperAIsEnabled(lua_State* L, const char* caller);
	int IsNoCostEnabled(lua_State* L, const char* caller);
	int IsGameOver(lua_State* L, const char* caller);
	int GetGroundExtremes(lua_State* L, const char* caller);
	int GetGlobalLos(lua_State* L, const char* caller);

	// PR 27a row-backed tail: the remaining per-object callouts whose live
	// reads are fully covered by the PR-27a SimSnapshot rows (unit status/eco
	// scalars, the feature tail, the projectile tail). Same §E.2 recipe as the
	// families above; parse-gate mirrors follow each live body's parse helper.
	int ValidUnitID(lua_State* L, const char* caller);
	int GetUnitDefID(lua_State* L, const char* caller);
	int GetUnitTeam(lua_State* L, const char* caller);
	int GetUnitAllyTeam(lua_State* L, const char* caller);
	int GetUnitNeutral(lua_State* L, const char* caller);
	int GetUnitIsDead(lua_State* L, const char* caller);
	int GetUnitIsBeingBuilt(lua_State* L, const char* caller);
	int GetUnitVelocity(lua_State* L, const char* caller);
	int GetUnitDirection(lua_State* L, const char* caller);
	int GetUnitHeading(lua_State* L, const char* caller);
	int GetUnitVectors(lua_State* L, const char* caller);
	int GetUnitRadius(lua_State* L, const char* caller);
	int GetUnitHeight(lua_State* L, const char* caller);
	int GetUnitMass(lua_State* L, const char* caller);
	int GetUnitExperience(lua_State* L, const char* caller);
	int GetUnitIsActive(lua_State* L, const char* caller);
	int GetUnitIsCloaked(lua_State* L, const char* caller);
	int GetUnitMaxRange(lua_State* L, const char* caller);
	int GetUnitBuildFacing(lua_State* L, const char* caller);
	int GetUnitSensorRadius(lua_State* L, const char* caller);
	int GetUnitSeismicSignature(lua_State* L, const char* caller);
	int GetUnitSelfDTime(lua_State* L, const char* caller);
	int GetUnitArmored(lua_State* L, const char* caller);
	int GetUnitResources(lua_State* L, const char* caller);
	int GetUnitHarvestStorage(lua_State* L, const char* caller);
	int GetUnitCosts(lua_State* L, const char* caller);
	int GetUnitCostTable(lua_State* L, const char* caller);
	int GetUnitMoveDefID(lua_State* L, const char* caller);
	int GetUnitBlocking(lua_State* L, const char* caller);
	int GetUnitLeavesGhost(lua_State* L, const char* caller);
	int GetUnitSeparation(lua_State* L, const char* caller);
	int GetUnitFeatureSeparation(lua_State* L, const char* caller);
	int IsUnitInRadar(lua_State* L, const char* caller); // inRadarAll IS the live InRadar(unit, at) answer
	int IsUnitAllied(lua_State* L, const char* caller);  // LuaUnsyncedRead

	int ValidFeatureID(lua_State* L, const char* caller);
	int GetFeatureDefID(lua_State* L, const char* caller);
	int GetFeatureTeam(lua_State* L, const char* caller);
	int GetFeatureAllyTeam(lua_State* L, const char* caller);
	int GetFeatureHealth(lua_State* L, const char* caller);
	int GetFeatureHeight(lua_State* L, const char* caller);
	int GetFeatureRadius(lua_State* L, const char* caller);
	int GetFeaturePosition(lua_State* L, const char* caller);
	int GetFeatureMass(lua_State* L, const char* caller);
	int GetFeatureDirection(lua_State* L, const char* caller);
	int GetFeatureVelocity(lua_State* L, const char* caller);
	int GetFeatureHeading(lua_State* L, const char* caller);
	int GetFeatureResources(lua_State* L, const char* caller);
	int GetFeatureBlocking(lua_State* L, const char* caller);
	int GetFeatureNoSelect(lua_State* L, const char* caller);
	int GetFeatureResurrect(lua_State* L, const char* caller);
	int GetFeatureSeparation(lua_State* L, const char* caller);

	int GetProjectileDirection(lua_State* L, const char* caller);
	int GetProjectileGravity(lua_State* L, const char* caller);
	int GetProjectileTeamID(lua_State* L, const char* caller);
	int GetProjectileAllyTeamID(lua_State* L, const char* caller);
	int GetProjectileType(lua_State* L, const char* caller);
	int GetProjectileTimeToLive(lua_State* L, const char* caller);
	int GetProjectileIsIntercepted(lua_State* L, const char* caller);

	// spatial-query + team-unit-list families (PR 27b): list twins served from
	// SnapshotPickGrid rect/radius queries (spatial) and a per-boundary
	// team-unit index derived lazily from the UnitRows front buffer (lists).
	// DOCUMENTED DEVIATION: result order is ascending-id, not master's
	// quadfield-walk / creation order; the armed dual-run compares these
	// callouts' result tables as ID sets (see CompareTablesAsIdSet).
	int GetAllUnits(lua_State* L, const char* caller);
	int GetTeamUnits(lua_State* L, const char* caller);
	int GetTeamUnitsSorted(lua_State* L, const char* caller);
	int GetTeamUnitsCounts(lua_State* L, const char* caller);
	int GetTeamUnitsByDefs(lua_State* L, const char* caller);
	int GetTeamUnitDefCount(lua_State* L, const char* caller);
	int GetUnitsInRectangle(lua_State* L, const char* caller);
	int GetUnitsInBox(lua_State* L, const char* caller);
	int GetUnitsInCylinder(lua_State* L, const char* caller);
	int GetUnitsInSphere(lua_State* L, const char* caller);
	int GetFeaturesInRectangle(lua_State* L, const char* caller);
	int GetFeaturesInSphere(lua_State* L, const char* caller);
	int GetFeaturesInCylinder(lua_State* L, const char* caller);
	int GetProjectilesInRectangle(lua_State* L, const char* caller);

	// command-queue family (PR 27b serving batch 2, first family): served from
	// per-unit boundary copies of CCommandAI::commandQue / CFactoryCAI::
	// newUnitCommands (+ the CFactory bugger-off scalars), refreshed by
	// RefreshCommandQueues() below. Copies are flattened (id/tag/options +
	// params in a flat float buffer) because raw Command copies touch the
	// sim-owned cmdParamsPool (pooled >8-param commands acquire/release pool
	// pages on copy/destruct, and read pool storage the sim thread resizes).
	int GetUnitCommands(lua_State* L, const char* caller); // also serves GetCommandQueue (same live body)
	int GetUnitCommandCount(lua_State* L, const char* caller);
	int GetUnitCurrentCommand(lua_State* L, const char* caller);
	int GetFactoryCommands(lua_State* L, const char* caller);
	int GetFactoryCommandCount(lua_State* L, const char* caller);
	int GetFactoryCounts(lua_State* L, const char* caller);
	int GetFactoryBuggerOff(lua_State* L, const char* caller);
	int GetFullBuildQueue(lua_State* L, const char* caller);
	int GetRealBuildQueue(lua_State* L, const char* caller);
	// sim|draw PR 30: cmd-desc surface + builder worker-task twins (served from
	// the same per-unit boundary cache; descs are version-keyed on CCommandAI::
	// GetCmdDescVersion(), the worker-task answer is decoded at extraction)
	int GetUnitCmdDescs(lua_State* L, const char* caller);   // ParseTypedUnit gate
	int FindUnitCmdDesc(lua_State* L, const char* caller);   // ParseTypedUnit gate
	int GetUnitWorkerTask(lua_State* L, const char* caller); // ParseInLosUnit gate

	/// barrier hook (CGame::SimDrawBarrier, right after the snapshot publish --
	/// generation-gated, so queue copies and snapshot rows always describe the
	/// same boundary): re-copies the queues of units whose CCommandQueue
	/// version changed, drops entries of dead units. Sim must be parked (or
	/// single-threaded): walks unitHandler and reads live queues.
	void RefreshCommandQueues();

	// sim|draw PR 30 mirror-verification hook (SnapshotDiffGate::CheckCmdQueueRows):
	// bit-compare the cached command-queue/cmd-desc/worker/factory slot for a unit
	// against live sim state at the armed boundary. Test-only (armed runs). Lives
	// here so the file-static serving cache stays encapsulated. `present` reports
	// whether a cached slot exists (compare with the live unit's validity for the
	// presence field); the *Ok bools are meaningful only when present && liveUnit.
	struct CmdQueueCompareResult {
		bool present  = false;
		bool queueOk  = false;
		bool descsOk  = false;
		bool workerOk = false;
		bool factoryOk = false;
	};
	CmdQueueCompareResult CompareCmdQueueSlot(int unitID, const CUnit* liveUnit);

	// ---- PR 34 (spatial/list remainder) ----
	// The remainder of the spatial/list family: per-team plane test + table
	// centroids over UnitRows, the whole-list projectile/feature twins over
	// ProjectileRows/FeatureRows, and the draw-owned selection/group aggregate
	// twins (defID sort keys from UnitRows; the id containers are draw-owned).
	// List-returning ones share the ascending-id order deviation and compare as
	// ID sets; centroids and the selection/group aggregates are order-exact
	// (they iterate the same Lua table / draw-owned container as the live path).
	int GetUnitArrayCentroid(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetUnitMapCentroid(lua_State* L, const char* caller);     // LuaSyncedRead
	int GetAllProjectiles(lua_State* L, const char* caller);      // LuaSyncedRead
	int GetProjectilesInSphere(lua_State* L, const char* caller); // LuaSyncedRead (projectile radius row)
	int GetAllFeatures(lua_State* L, const char* caller);         // LuaSyncedRead
	int GetSelectedUnitsSorted(lua_State* L, const char* caller); // LuaUnsyncedRead (draw-owned selection set)
	int GetSelectedUnitsCounts(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetGroupUnitsSorted(lua_State* L, const char* caller);    // LuaUnsyncedRead (draw-owned group set)
	int GetGroupUnitsCounts(lua_State* L, const char* caller);    // LuaUnsyncedRead

	/// game teardown: drop the per-generation serving caches (the team-unit
	/// index, the command-queue copies); SimSnapshot's generation counter
	/// resets across games, so a stale cache could otherwise alias a fresh
	/// generation number
	void ClearCaches();

	// unsynced flag/drawer parse-gate family (PR 27b serving batch 2,
	// family 2): payloads are draw-owned, only the ParseUnit/ParseFeature
	// gate is snapshot-served
	int GetUnitLuaDraw(lua_State* L, const char* caller);            // LuaUnsyncedRead
	int GetUnitNoDraw(lua_State* L, const char* caller);             // LuaUnsyncedRead
	int GetUnitNoMinimap(lua_State* L, const char* caller);          // LuaUnsyncedRead
	int GetUnitNoGroup(lua_State* L, const char* caller);            // LuaUnsyncedRead
	int GetUnitNoSelect(lua_State* L, const char* caller);           // LuaUnsyncedRead (unit; feature twin is row-served)
	int GetUnitEngineDrawMask(lua_State* L, const char* caller);     // LuaUnsyncedRead
	int GetUnitAlwaysUpdateMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetUnitDrawFlag(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer-owned flag, pointer via boundary resolve cache + shell
	int UnitIconGetDraw(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer icon-state payload (drawIcon)
	int GetUnitIcon(lua_State* L, const char* caller);       // LuaUnsyncedRead: drawer icon index -> icon name
	int GetUnitIconData(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer icon index -> IconData table
	int IsUnitSelected(lua_State* L, const char* caller);    // LuaUnsyncedRead: selection set is id-keyed, no pointer needed
	int GetUnitGroup(lua_State* L, const char* caller);      // LuaUnsyncedRead: team gate from rows, uiGroupHandlers lookup is id-keyed
	int IsUnitInView(lua_State* L, const char* caller);        // LuaUnsyncedRead
	int GetUnitTransformMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead (drawPos drawer-owned; basis/error mirrored from rows)
	int GetUnitSelectionVolumeData(lua_State* L, const char* caller); // LuaUnsyncedRead (selVol payload stays live: unsynced-owned, read via IdToObject)
	int GetUnitRotation(lua_State* L, const char* caller); // unsynced drawer-matrix branch only (ShouldServe rejects synced handles)
	int GetFeatureLuaDraw(lua_State* L, const char* caller);          // LuaUnsyncedRead
	int GetFeatureNoDraw(lua_State* L, const char* caller);           // LuaUnsyncedRead
	int GetFeatureEngineDrawMask(lua_State* L, const char* caller);   // LuaUnsyncedRead
	int GetFeatureAlwaysUpdateMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetFeatureDrawFlag(lua_State* L, const char* caller);         // LuaUnsyncedRead
	int GetFeatureSelectionVolumeData(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetFeatureTransformMatrix(lua_State* L, const char* caller);  // LuaUnsyncedRead
	int GetFeatureRotation(lua_State* L, const char* caller);

	// ---- PR 28: map-layer mirror family (positional LOS + map info) ----
	// Served from DrawMapMirrors (the boundary-drained draw-owned copies of the
	// per-allyteam LOS/radar/jammer maps, the terrain-type table, the
	// smooth-height mesh, the original heightmap, and the radar-error scalars),
	// NOT from SimSnapshot rows. Same §E.2 Route()/twin recipe; the positional
	// twins mirror the CLosHandler formulas, the map-info twins mirror the
	// ground interpolation math. POV: the LOS maps are per-allyteam like the
	// source, so the twins index the requested allyteam's mirror.
	int IsPosInLos(lua_State* L, const char* caller);
	int IsPosInRadar(lua_State* L, const char* caller);
	int IsPosInAirLos(lua_State* L, const char* caller);
	int GetPositionLosState(lua_State* L, const char* caller);
	int GetRadarErrorParams(lua_State* L, const char* caller);
	int GetTerrainTypeData(lua_State* L, const char* caller);
	int GetSmoothMeshHeight(lua_State* L, const char* caller);
	int GetGroundOrigHeight(lua_State* L, const char* caller);

	// ---- sim|draw PR 29 (blocking-map mirror + placement family) ----
	// GetGroundBlocked reads the DrawMapMirrors blocking mirror (per-square
	// cell[0] id + kind) and gates visibility through the snapshot unit/feature
	// rows. Pos2BuildPos snaps to the build grid over the already-draw-safe
	// unsynced heightmap (no sim-owned mutable state), so its twin reuses the
	// live CGameHelper helper directly with synced=false.
	int GetGroundBlocked(lua_State* L, const char* caller); // LuaSyncedRead
	int Pos2BuildPos(lua_State* L, const char* caller);     // LuaSyncedRead

	int GetGaiaTeamID(lua_State* L, const char* caller);
	int GetAllyTeamList(lua_State* L, const char* caller);
	int GetTeamList(lua_State* L, const char* caller);
	int GetPlayerList(lua_State* L, const char* caller);
	int GetTeamInfo(lua_State* L, const char* caller);
	int GetTeamAllyTeamID(lua_State* L, const char* caller);
	int GetTeamResources(lua_State* L, const char* caller);
	int GetTeamUnitStats(lua_State* L, const char* caller);
	int GetTeamResourceStats(lua_State* L, const char* caller);
	int GetTeamDamageStats(lua_State* L, const char* caller);
	int GetTeamUnitCount(lua_State* L, const char* caller);
	int GetPlayerInfo(lua_State* L, const char* caller);
	int GetTeamColor(lua_State* L, const char* caller);     // LuaUnsyncedRead
	int GetTeamOrigColor(lua_State* L, const char* caller); // LuaUnsyncedRead

	// ---- PR 33 (pieces/scripts family) ----
	// RefreshPieces() runs at the barrier (right after simSnapshot.Update(),
	// generation-gated) and captures per-object piece dynamic state + immutable
	// model metadata so the unit/feature piece twins below serve pointer-free
	// under the running split (see the serving-section header in the .cpp).
	// Piece-projectile params/name are served from SimSnapshot::ProjectileRows.
	void RefreshPieces();

	int GetUnitRootPiece(lua_State* L, const char* caller);
	int GetUnitPieceMap(lua_State* L, const char* caller);
	int GetUnitPieceList(lua_State* L, const char* caller);
	int GetUnitPieceInfo(lua_State* L, const char* caller);
	int GetUnitPiecePosition(lua_State* L, const char* caller);
	int GetUnitPieceDirection(lua_State* L, const char* caller);
	int GetUnitPiecePosDir(lua_State* L, const char* caller);
	int GetUnitPieceMatrix(lua_State* L, const char* caller);
	int GetUnitScriptPiece(lua_State* L, const char* caller);
	int GetUnitScriptNames(lua_State* L, const char* caller);

	int GetFeatureRootPiece(lua_State* L, const char* caller);
	int GetFeaturePieceMap(lua_State* L, const char* caller);
	int GetFeaturePieceList(lua_State* L, const char* caller);
	int GetFeaturePieceInfo(lua_State* L, const char* caller);
	int GetFeaturePiecePosition(lua_State* L, const char* caller);
	int GetFeaturePieceDirection(lua_State* L, const char* caller);
	int GetFeaturePiecePosDir(lua_State* L, const char* caller);
	int GetFeaturePieceMatrix(lua_State* L, const char* caller);
	int GetFeatureCollisionVolumeData(lua_State* L, const char* caller);
	int GetFeaturePieceCollisionVolumeData(lua_State* L, const char* caller);
	int GetFeatureLastAttackedPiece(lua_State* L, const char* caller);

	int GetPieceProjectileParams(lua_State* L, const char* caller);
	int GetPieceProjectileName(lua_State* L, const char* caller);

	// ---- PR 31 (weapon/shield scalar family) ----
	// Served from the SimSnapshot::UnitRows weapon block (per-unit scalars + a
	// flat per-weapon SoA). All gate on the unit's ParseAllyUnit visibility
	// (PovAlliedUnit) except GetUnitShieldState (ParseInLosUnit -> PovUnitInLos).
	// Trace tests (TryTarget/TestTarget/TestRange/HaveFreeLineOfFire) are NOT
	// here -- PR 35 recomputes them against the published collision world.
	int GetUnitStockpile(lua_State* L, const char* caller);
	int GetUnitShieldState(lua_State* L, const char* caller);
	int GetUnitFlanking(lua_State* L, const char* caller);
	int GetUnitWeaponState(lua_State* L, const char* caller);
	int GetUnitWeaponDamages(lua_State* L, const char* caller);
	int GetUnitWeaponVectors(lua_State* L, const char* caller);
	int GetUnitWeaponCanFire(lua_State* L, const char* caller);
	int GetUnitWeaponTarget(lua_State* L, const char* caller);

	// ---- PR 32 (deep per-unit state) ----
	// Served from the PR-32 SimSnapshot::UnitRows deep rows + the full-table
	// moveType block; the collision-volume / last-hit-piece three reuse PR 33's
	// unit piece cache (RefreshPieces now captures colVol + lastHit for units).
	int GetUnitStates(lua_State* L, const char* caller);            // ParseAllyUnit
	int GetUnitStorage(lua_State* L, const char* caller);           // ParseAllyUnit
	int GetUnitMetalExtraction(lua_State* L, const char* caller);   // ParseAllyUnit
	int GetUnitBuildeeRadius(lua_State* L, const char* caller);     // ParseTypedUnit
	int GetUnitPosErrorParams(lua_State* L, const char* caller);    // ParseAllyUnit
	int GetUnitLastAttacker(lua_State* L, const char* caller);      // ParseUnit (attacker visibility gate)
	int GetUnitIsBuilding(lua_State* L, const char* caller);        // ParseAllyUnit
	int GetUnitBuildParams(lua_State* L, const char* caller);       // ParseAllyUnit
	int GetUnitInBuildStance(lua_State* L, const char* caller);     // ParseAllyUnit
	int GetUnitCurrentBuildPower(lua_State* L, const char* caller); // ParseAllyUnit
	int GetUnitEffectiveBuildRange(lua_State* L, const char* caller);// ParseInLosUnit
	int GetUnitNanoPieces(lua_State* L, const char* caller);        // ParseAllyUnit
	int GetUnitTransporter(lua_State* L, const char* caller);       // ParseInLosUnit
	int GetUnitIsTransporting(lua_State* L, const char* caller);    // ParseAllyUnit
	int GetUnitTooltip(lua_State* L, const char* caller);           // ParseTypedUnit
	int GetUnitMoveTypeData(lua_State* L, const char* caller);      // ParseAllyUnit
	int GetUnitCollisionVolumeData(lua_State* L, const char* caller);      // ParseInLosUnit (piece cache)
	int GetUnitPieceCollisionVolumeData(lua_State* L, const char* caller); // ParseInLosUnit (piece cache)
	int GetUnitLastAttackedPiece(lua_State* L, const char* caller);        // ParseAllyUnit (piece cache)
	int IsUnitInLos(lua_State* L, const char* caller);              // ParseTypedUnit
	int IsUnitInAirLos(lua_State* L, const char* caller);           // ParseTypedUnit
	int IsUnitInJammer(lua_State* L, const char* caller);           // ParseTypedUnit

	// team/player misc family (PR 36; TeamRows/PlayerRows extensions + the
	// SimSnapshot map-start cache -- see LuaSnapshotServe.cpp's PR-36 section)
	int GetTeamStartPosition(lua_State* L, const char* caller);
	int GetAllyTeamStartBox(lua_State* L, const char* caller);
	int GetMapStartPositions(lua_State* L, const char* caller);
	int GetTeamMaxUnits(lua_State* L, const char* caller);
	int GetTeamLuaAI(lua_State* L, const char* caller);
	int GetAIInfo(lua_State* L, const char* caller);
	int GetAllyTeamInfo(lua_State* L, const char* caller);
	int AreTeamsAllied(lua_State* L, const char* caller);
	int ArePlayersAllied(lua_State* L, const char* caller);
	int GetPlayerControlledUnit(lua_State* L, const char* caller);
	int GetTeamStatsHistory(lua_State* L, const char* caller);
	int GetPlayerStatistics(lua_State* L, const char* caller); // LuaUnsyncedRead

	// ======================= PR 35: weapon trace tests =======================
	// GetUnitWeaponTryTarget/TestTarget/TestRange/HaveFreeLineOfFire served by a
	// SIM-SIDE QUERY/REPLY channel (Batch-3 amendment; NOT the draw-side
	// recompute the plan doc's PR-35 row and decision-3(A) originally named --
	// that would break flag-off bit-identity, since Route() serves the callout's
	// value even flag-off, so any approximation replaces master's live answer).
	//
	//  - Flag-OFF (no running split): RouteTraceQuery runs the LIVE predicate
	//    inline (bit-identical to master; sim==draw single-threaded, the sim is
	//    parked relative to draw). No queue, no defer.
	//  - Flag-ON (running split): the callout enqueues a trace query {owner+weapon,
	//    target form, arg variant} and returns the LAST boundary's sim-exact reply
	//    (false on first-call/miss). EvaluateTraceQueries() drains the pending
	//    queue at the SimDrawBarrier (sim parked) and evaluates the EXACT live
	//    CWeapon predicate against live sim state -- boundary-deferred (<=1 stale)
	//    but sim-exact, no approximation. Reply state is UNSYNCED (draw-only): no
	//    synced write, no gsRNG, no streflop (the predicates are const reads,
	//    already called from unsynced widgets on master without desync).
	//
	// This request/reply channel is DISTINCT from the published snapshot ring --
	// PR 43's epoch mechanism must carry the query queue + reply map alongside the
	// ring (flagged for the epoch-infra refresh).
	//
	// FLAG-ON DEVIATION (enumerated at integration -- PR 35 review finding): the
	// reply map is keyed by the FULL query, including the float pos/tgt bits. For
	// enemy-form queries {owner,weapon,enemyID} the key is stable, so after the
	// first boundary the reply populates and tracks at <=1 stale (deviation #2).
	// But a POS-FORM / cursor-tracking query whose ground position CHANGES every
	// frame (attack-range / ground-attack placement widgets that follow the mouse
	// cursor) produces a NEW key each frame => every serve is a map miss => the
	// callout returns the default `false` PERSISTENTLY, never a correct answer,
	// for as long as the position keeps moving. This is worse than the sanctioned
	// "<=1 stale" envelope and is ADVISORY-UI-ONLY (no sync/leak consequence --
	// the channel is unsynced draw-state). It is deliberately left forward-fixable
	// rather than fixed here: evaluating pos-form synchronously at request time is
	// UNSAFE under the running split (the predicates read the collision world /
	// CGround / quadfield TraceRay scratch the sim thread is concurrently touching
	// -- the exact race this channel avoids), and a pos-agnostic secondary key
	// (return the most-recent-position reply) carries its own same-frame collision
	// ambiguity between two widgets querying one owner/weapon at different cursor
	// positions. If BAR ships a cursor-following pos-form predicate widget that
	// this breaks, PR 43's epoch refresh is the place to revisit the keying. The
	// windowed targeting-widget gate (batch end) MUST exercise a moving-cursor
	// ground-placement predicate so this class is verified, not silently broken.
	enum class TraceKind { TryTarget = 0, TestTarget = 1, TestRange = 2, HaveFreeLineOfFire = 3 };

	/// entry-point glue for the four trace tests (called from LuaSyncedRead):
	/// dispatches to `liveFn` when the split is off (bit-identical), to the
	/// query/reply channel under the running split, and dual-runs live-vs-query
	/// for coverage when the diff gate is armed flag-off.
	int RouteTraceQuery(lua_State* L, const char* caller, ServeFn liveFn, TraceKind kind);

	/// SimDrawBarrier hook (sim parked): drain the pending trace queries and
	/// evaluate each against live sim state, publishing the replies for the next
	/// draw frame. No-op (empty check) flag-off / when nothing was enqueued.
	void EvaluateTraceQueries();

	/// game teardown: reset the trace-query pending queue + reply map (called from
	/// ClearCaches()). Self-pruning already prevents growth/aliasing; this is the
	/// explicit belt-and-suspenders reset.
	void ClearTraceQueryChannel();
}
