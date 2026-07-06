/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSplitContract.h"

#include <algorithm>
#include <string>
#include <vector>

#include "LuaHandle.h"
#include "LuaInclude.h"

#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Platform/Threading.h"
#include "System/SimDrawSplit.h"
#include "System/UnorderedMap.hpp"
#include "System/UnorderedSet.hpp"

CONFIG(int, SplitDrawContract)
	.defaultValue(0)
	.description("Enforce the sim|draw split's draw-thread Lua contract while still single-threaded (PR 27a dress rehearsal). 0=off (bit-identical legacy behavior), 1=count mode: snapshot-serving widens to the whole draw phase, cross-hops error, direct sim pokes queue to the barrier, every remaining draw-context live sim read is counted per callout; 2=strict: additionally refuse non-sanctioned live reads (the callout returns its 'no such object' nil shape) -- the true 27b preview.");

CONFIG(int, SplitDrawContractWarn)
	.defaultValue(1)
	.description("Log a once-per-callout-name warning when a draw-context live sim read trips the SplitDrawContract (count or strict mode). 0 silences the warnings; the inventory dump still records every trip.");

namespace LuaSplitContract {

namespace {
	// cached config (read once at first use, like LuaCalloutCounters)
	int mode = 0;          // 0 off, 1 count, 2 strict
	bool warnOnTrip = true;
	bool inited = false;

	// single-threaded today; thread_local so the predicates stay correct when
	// 27b puts sim on its own thread (the draw window only ever opens on the
	// draw/main thread)
	thread_local int tlDrawWindowDepth = 0;
	thread_local int tlLiveExceptionDepth = 0;

	struct TripStat {
		uint64_t liveReads = 0;   // sanctioned or count-mode live reads
		uint64_t denials = 0;     // strict-mode nil-shape serves
		uint64_t queuedPokes = 0; // boundary-deferred ctrl applies
		uint64_t syncPokes = 0;   // ctrl pokes that stay synchronous in 27a
		bool warned = false;
	};
	spring::unordered_map<std::string, TripStat> tripStats;

	struct QueuedOp {
		const char* name;         // callout __func__ (static storage)
		std::function<void()> op;
	};
	std::vector<QueuedOp> queuedOps;

	/**
	 * The strict-mode sanctioned-live list -- THE explicit "what 27b may
	 * still not do" set, i.e. the classification table for every sim-reading
	 * callout that is NOT snapshot-served after the PR-27a serving pass.
	 * Every name here keeps its live read even in strict mode (counted, so
	 * the inventory still measures real demand); the split may not enable
	 * until each family is served, boundary-copied, or explicitly re-decided
	 * as a permanent nil. A draw-context live read whose callout is neither
	 * served nor listed here is DENIED in strict mode -- that catches
	 * unclassified surface (new callouts, audit misses) loudly.
	 *
	 * Family -> planned serving mechanism (research doc sections C/E):
	 *  - placement tests / blocking map: draw-side groundBlockingObjectMap
	 *    mirror (heightmap dirty-rect pattern)
	 *  - map info: typemap/terrain-type/smooth-mesh mirrors (smooth mesh is
	 *    already a section-C (c) row)
	 *  - positional LOS: the draw-owned LOS-map double buffer (section-C
	 *    "LOS maps" row, already the planned split action)
	 *  - rules params: dirty-delta boundary copy; the deltas must ride the
	 *    RenderEventQueue so creation-clears order against set-deltas under
	 *    id reuse (same record schema as the stream format)
	 *  - command queues / weapon state: decision-4 dirty-versioned bounded
	 *    copies (mutation-rate cost)
	 *  - pieces/scripts: piece transforms are extracted since PR 8; the
	 *    callout surface needs id-keyed piece-tree serving on top
	 *  - spatial/list queries: SnapshotPickGrid + rows can serve the SET,
	 *    but not master's quadfield/active-list ORDER -- serving needs the
	 *    documented order deviation + set-equality dual-run comparison
	 *  - pathing: sim-owned pathManager; reads need boundary copies,
	 *    RequestPath is a mutation (boundary-apply or contract-error)
	 */
	const spring::unordered_set<std::string> sanctionedLive = {
		// placement/build tests (GuiHandler-driven widgets call these per
		// mouse position from draw context)
		"TestBuildOrder",
		"TestMoveOrder",
		"Pos2BuildPos",
		"ClosestBuildPos",
		"GetGroundBlocked",
		"GetGroundOrigHeight", // reads the SYNCED original heightmap (unlike the rest of the CGround family)
		// map info reads without an unsynced mirror yet
		"GetGroundInfo",
		"GetTerrainTypeData",
		"GetSmoothMeshHeight",
		// positional LOS-map queries (fog/attack-preview widgets)
		"GetPositionLosState",
		"IsPosInLos",
		"IsPosInRadar",
		"IsPosInAirLos",
		"IsUnitInLos",    // CLosHandler::InLos(unit, at): cloak/airLos positional math, not losStatus
		"IsUnitInAirLos",
		"IsUnitInJammer",
		"GetRadarErrorParams",
		// rules params (dirty-delta copy pending)
		"GetGameRulesParam", "GetGameRulesParams",
		"GetTeamRulesParam", "GetTeamRulesParams",
		"GetPlayerRulesParam", "GetPlayerRulesParams",
		"GetUnitRulesParam", "GetUnitRulesParams",
		"GetFeatureRulesParam", "GetFeatureRulesParams",
		// command-queue family (decision-4 copies pending)
		"GetUnitCommandCount", "GetUnitCommands", "GetUnitCurrentCommand",
		"GetFactoryCounts", "GetFactoryCommandCount", "GetFactoryCommands",
		"GetFactoryBuggerOff", "GetCommandQueue", "GetFullBuildQueue",
		"GetRealBuildQueue", "GetUnitCmdDescs", "FindUnitCmdDesc",
		"GetUnitWorkerTask",
		// weapon/shield state family (decision-4 copies pending)
		"GetUnitShieldState", "GetUnitFlanking", "GetUnitWeaponState",
		"GetUnitWeaponDamages", "GetUnitWeaponVectors", "GetUnitWeaponTryTarget",
		"GetUnitWeaponTestTarget", "GetUnitWeaponTestRange",
		"GetUnitWeaponHaveFreeLineOfFire", "GetUnitWeaponCanFire",
		"GetUnitWeaponTarget", "GetUnitStockpile",
		// deep per-unit state (moveType/CAI/second-object derefs)
		"GetUnitStates", "GetUnitMoveTypeData", "GetUnitIsBuilding",
		"GetUnitBuildParams", "GetUnitInBuildStance", "GetUnitNanoPieces",
		"GetUnitEffectiveBuildRange", "GetUnitCurrentBuildPower",
		"GetUnitTransporter", "GetUnitIsTransporting", "GetUnitLastAttacker",
		"GetUnitLastAttackedPiece", "GetUnitTooltip", "GetUnitMetalExtraction",
		"GetUnitPosErrorParams", "GetUnitBuildeeRadius", "GetUnitStorage",
		"GetUnitCollisionVolumeData", "GetUnitPieceCollisionVolumeData",
		// piece/script reads (piece-tree serving pending)
		"GetUnitRootPiece", "GetUnitPieceMap", "GetUnitPieceList",
		"GetUnitPieceInfo", "GetUnitPiecePosition", "GetUnitPieceDirection",
		"GetUnitPiecePosDir", "GetUnitPieceMatrix", "GetUnitScriptPiece",
		"GetUnitScriptNames",
		"GetFeatureRootPiece", "GetFeaturePieceMap", "GetFeaturePieceList",
		"GetFeaturePieceInfo", "GetFeaturePiecePosition",
		"GetFeaturePieceDirection", "GetFeaturePiecePosDir",
		"GetFeaturePieceMatrix", "GetFeatureCollisionVolumeData",
		"GetFeaturePieceCollisionVolumeData", "GetFeatureLastAttackedPiece",
		"GetPieceProjectileParams", "GetPieceProjectileName",
		// spatial/list queries (grid+rows serving with order deviation pending)
		"GetAllUnits", "GetTeamUnits", "GetTeamUnitsSorted",
		"GetTeamUnitsCounts", "GetTeamUnitsByDefs", "GetTeamUnitDefCount",
		"GetUnitsInRectangle", "GetUnitsInBox", "GetUnitsInPlanes",
		"GetUnitsInSphere", "GetUnitsInCylinder", "GetUnitArrayCentroid",
		"GetUnitMapCentroid", "GetFeaturesInRectangle", "GetFeaturesInSphere",
		"GetFeaturesInCylinder", "GetAllProjectiles",
		"GetProjectilesInRectangle", "GetProjectilesInSphere",
		"GetUnitNearestAlly", "GetUnitNearestEnemy", "GetAllFeatures",
		"GetVisibleUnits", "GetVisibleFeatures", "GetVisibleProjectiles",
		"GetUnitsInScreenRectangle", "GetFeaturesInScreenRectangle",
		"GetSelectedUnitsSorted", "GetSelectedUnitsCounts",
		"GetGroupUnitsSorted", "GetGroupUnitsCounts",
		// pathing (sim-owned pathManager)
		"GetUnitEstimatedPath", "RequestPath", "PathFinder::Next",
		"PathFinder::GetPathWayPoints", "PathFinder::DeletePath",
		"InitPathNodeCostsArray", "FreePathNodeCostsArray",
		"SetPathNodeCosts", "GetPathNodeCosts", "SetPathNodeCost",
		"GetPathNodeCost",
		// team/player misc (start data / stats / AI tables; event-rate)
		"GetTeamStatsHistory", "GetTeamLuaAI", "GetTeamMaxUnits",
		"GetTeamStartPosition", "GetAllyTeamStartBox", "GetMapStartPositions",
		"GetPlayerControlledUnit", "GetAIInfo", "GetAllyTeamInfo",
		"AreTeamsAllied", "ArePlayersAllied", "GetPlayerTraffic",
		"GetPlayerStatistics",
		// (b)-class torn-tolerant / wall-clock-dependent
		"GetGameState", // IsSimLagging reads the wall clock; serving it would false-flag the armed dual-run
		// unsynced-owned object flags + drawer-backed reads (LuaUnsyncedRead):
		// the payloads are unsynced/drawer state (luaDraw/noDraw/selection
		// volumes/icons/transform matrices/camera tests), but the ParseUnit/
		// ParseFeature visibility gate is a live losStatus read -- 27b: serve
		// the gate from the snapshot Pov mirrors, payloads stay as-is
		"IsUnitSelected", "GetUnitLuaDraw", "GetUnitNoDraw",
		"GetUnitEngineDrawMask", "GetUnitAlwaysUpdateMatrix", "GetUnitDrawFlag",
		"GetUnitNoMinimap", "GetUnitNoGroup", "GetUnitNoSelect",
		"UnitIconGetDraw", "GetUnitIcon", "GetUnitIconData",
		"GetUnitSelectionVolumeData", "GetUnitTransformMatrix",
		"GetUnitGroup", "IsUnitInView", "IsSphereInView",
		"GetFeatureLuaDraw", "GetFeatureNoDraw", "GetFeatureEngineDrawMask",
		"GetFeatureAlwaysUpdateMatrix", "GetFeatureDrawFlag",
		"GetFeatureSelectionVolumeData", "GetFeatureTransformMatrix",
		// drawer-matrix-backed rotations (payload draw-safe since PR 3; only
		// the parse gate is a sim read, same 27b plan as the flag getters)
		"GetUnitRotation", "GetFeatureRotation",
		// misc
		"GetCEGID", // can LOAD a generator (explGenHandler mutation)
		"GetFeatureFireTime", "GetFeatureSmokeTime",
		"GetProjectileDamages",
	};

	inline void Init()
	{
		if (inited)
			return;

		mode = (configHandler != nullptr) ? configHandler->GetInt("SplitDrawContract") : 0;
		warnOnTrip = (configHandler == nullptr) || (configHandler->GetInt("SplitDrawContractWarn") != 0);
		inited = true;

		if (mode > 0)
			LOG("[SplitDrawContract] enforcement active (mode %d: %s)", mode, (mode >= 2) ? "strict tail" : "count");
	}

	inline void WarnOnce(TripStat& stat, const char* caller, const char* what)
	{
		if (stat.warned || !warnOnTrip)
			return;

		stat.warned = true;
		LOG_L(L_WARNING, "[SplitDrawContract] %s: %s (draw-thread context; further trips counted silently, see /splitcontractdump)", caller, what);
	}
}


bool Enabled()
{
	Init();
	// PR 27b: the running split implies the contract -- unsynced Lua on the
	// main thread executes against the snapshot while the sim advances
	return (mode > 0 || (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning()));
}

bool InDrawWindow() { return (tlDrawWindowDepth > 0); }


bool Enforced(lua_State* L)
{
	if (!Enabled())
		return false;

	// PR 27b: with the sim thread live, EVERY unsynced-handle execution on a
	// non-sim thread is draw-thread context -- input callins included, not
	// just the CGame::Draw window. The barrier and its sanctioned boundary
	// dispatches suppress enforcement via ScopedLiveException (live reads
	// are legal there: the sim is parked).
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning()) {
		if (tlLiveExceptionDepth > 0 || Threading::IsSimThread())
			return false;

		return !CLuaHandle::GetHandleSynced(L);
	}

	if (tlDrawWindowDepth <= 0 || tlLiveExceptionDepth > 0)
		return false;

	return !CLuaHandle::GetHandleSynced(L);
}


bool DenyLiveRead(lua_State* L, const char* caller)
{
	if (!Enforced(L))
		return false;

	TripStat& stat = tripStats[caller];

	if (mode >= 2 && sanctionedLive.find(caller) == sanctionedLive.end()) {
		stat.denials++;
		WarnOnce(stat, caller, "live sim read DENIED, serving the no-such-object nil shape (strict mode, not snapshot-served)");
		return true;
	}

	// PR 27b: the sanctioned-live bridge ends at the flip -- with the sim
	// thread actually running, a "sanctioned" live read walks containers the
	// sim mutates concurrently (the quadfield/team-list/command-queue
	// families are crash-class, not timing edges). Every unserved live read
	// denies deterministically until its family is snapshot-served; the
	// barrier's own dispatches run under ScopedLiveException and never get
	// here. (Decision 5: the boundary serves everything it claims to serve.)
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning() && !SimDrawSplit::IsSimParked()) {
		stat.denials++;
		WarnOnce(stat, caller, "live sim read DENIED under the running split (sim thread active; family not snapshot-served yet)");
		return true;
	}

	stat.liveReads++;
	WarnOnce(stat, caller, "live sim read from draw context (not snapshot-served; must be served, boundary-copied or nil-contracted before the 27b flip)");
	return false;
}


void ErrorOnCrossHop(lua_State* L, const char* what)
{
	if (!Enforced(L))
		return;

	TripStat& stat = tripStats[what];
	stat.denials++;
	stat.warned = true; // the error is its own report

	luaL_error(L, "[SplitDrawContract] %s is forbidden from draw-thread context: it touches the synced lua_State the sim thread owns under the split", what);
}


bool QueueBoundaryApply(lua_State* L, const char* caller, std::function<void()>&& op)
{
	if (!Enforced(L))
		return false;

	TripStat& stat = tripStats[caller];
	stat.queuedPokes++;
	queuedOps.push_back({caller, std::move(op)});
	return true;
}


void CountSanctionedPoke(lua_State* L, const char* caller)
{
	if (!Enforced(L))
		return;

	TripStat& stat = tripStats[caller];
	stat.syncPokes++;
	WarnOnce(stat, caller, "synchronous sim poke from draw context (classified 27b work: draw-owned handler or boundary op capture)");
}


void DrainBoundaryApplies()
{
	if (queuedOps.empty())
		return;

	// ops may not enqueue further ops (they are plain sim-state writes); a
	// swap keeps the invariant checkable and the vector's capacity reusable
	static std::vector<QueuedOp> draining;
	std::swap(draining, queuedOps);

	for (const QueuedOp& q: draining) {
		q.op();
	}

	draining.clear();
	std::swap(draining, queuedOps); // hand the capacity back
}


void DumpInventory(const char* reason)
{
	Init();

	if (tripStats.empty()) {
		LOG("[SplitDrawContract] inventory (%s): no trips recorded", reason);
		return;
	}

	std::vector<std::pair<std::string, TripStat>> rows(tripStats.begin(), tripStats.end());
	std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
		return (a.second.liveReads + a.second.denials + a.second.queuedPokes + a.second.syncPokes) >
		       (b.second.liveReads + b.second.denials + b.second.queuedPokes + b.second.syncPokes);
	});

	LOG("[SplitDrawContract] inventory (%s): %d callouts tripped (mode %d)", reason, int(rows.size()), mode);
	for (const auto& [name, stat]: rows) {
		LOG("[SplitDrawContract]   %-40s liveReads=%llu denials=%llu queuedPokes=%llu syncPokes=%llu%s",
			name.c_str(),
			(unsigned long long)stat.liveReads,
			(unsigned long long)stat.denials,
			(unsigned long long)stat.queuedPokes,
			(unsigned long long)stat.syncPokes,
			(sanctionedLive.find(name) != sanctionedLive.end()) ? " [sanctioned]" : "");
	}
}


void Clear()
{
	if (!tripStats.empty())
		DumpInventory("game end");

	tripStats.clear();
	queuedOps.clear();
}


ScopedDrawWindow::ScopedDrawWindow() { tlDrawWindowDepth++; }
ScopedDrawWindow::~ScopedDrawWindow() { tlDrawWindowDepth--; }

ScopedLiveException::ScopedLiveException() { tlLiveExceptionDepth++; }
ScopedLiveException::~ScopedLiveException() { tlLiveExceptionDepth--; }

}
