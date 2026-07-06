/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

struct lua_State;

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

	/// game teardown: drop the per-generation serving caches (the team-unit
	/// index); SimSnapshot's generation counter resets across games, so a
	/// stale cache could otherwise alias a fresh generation number
	void ClearCaches();

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
}
