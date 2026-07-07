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
	 *  - command queues: SERVED (PR 27b serving batch 2) via CCommandQueue
	 *    version ints + barrier copy-on-change (LuaSnapshotServe::
	 *    RefreshCommandQueues); only the cmd-desc surface remains below
	 *  - weapon state: decision-4 dirty-versioned bounded copies
	 *    (mutation-rate cost)
	 *  - pieces/scripts: piece transforms are extracted since PR 8; the
	 *    callout surface needs id-keyed piece-tree serving on top
	 *  - spatial/list queries: core family SERVED in PR 27b (SnapshotPickGrid
	 *    + team-unit index, ascending-id order deviation, set-mode dual-run
	 *    comparison); PR 34 served the table centroids, whole-list projectile/
	 *    feature twins (projectile radius row added), and the draw-owned
	 *    selection/group aggregates; the remainder below is the camera-frustum
	 *    composites (visible/screen-rect), the CGameHelper nearest search, and
	 *    GetUnitsInPlanes' per-team-overwrite quirk -- each a design edge deferred
	 *  - pathing: sim-owned pathManager; reads need boundary copies,
	 *    RequestPath is a mutation (boundary-apply or contract-error)
	 */
	const spring::unordered_set<std::string> sanctionedLive = {
		// placement/build tests (GuiHandler-driven widgets call these per
		// mouse position from draw context). sim|draw PR 29 landed the
		// DrawMapMirrors blocking-map mirror (per-square cell[0] id + kind) and
		// SERVED Pos2BuildPos (draw-safe unsynced-heightmap build-grid snap) and
		// GetGroundBlocked (blocking mirror + snapshot unit/feature visibility).
		// The three below stay live: their test logic bottoms out in the SYNCED
		// terrain-speedmod arrays (maxHeightMap/typeMap/slopeMap/centerNormals2D
		// via CheckCollisionQuery elevation + CMoveMath::GetPosSpeedMod), which
		// need their own draw-side terrain mirror + a faithful CMoveMath/
		// TestUnitBuildSquare re-host -- a distinct, gate-verified follow-up (the
		// blocking mirror is in place; see the PR 29 escalation note).
		"TestBuildOrder",
		"TestMoveOrder",
		"ClosestBuildPos",
		// map info reads without an unsynced mirror yet: GetGroundOrigHeight
		// (orig-heightmap), GetTerrainTypeData (terrain-type table) and
		// GetSmoothMeshHeight (smooth mesh) are SERVED from DrawMapMirrors
		// (PR 28); GetGroundInfo also reads the metal + type maps (metal-map
		// mirror deferred, headless-stubbed) so it stays live for now
		"GetGroundInfo",
		// positional LOS-map queries (fog/attack-preview widgets): the
		// POSITION family (GetPositionLosState/IsPosIn{Los,Radar,AirLos}) and
		// GetRadarErrorParams are SERVED from the DrawMapMirrors LOS maps +
		// radar-error scalars (PR 28). The UNIT variants (IsUnitIn{Los,AirLos,
		// Jammer}) are SERVED in PR 32 -- the computed losHandler->In*(unit, at)
		// answers are extracted into the unitIn*All stride rows (the inRadarAll
		// precedent), with the new cloak/stealth/water per-unit rows this PR adds.
		// rules params (dirty-delta copy pending)
		"GetGameRulesParam", "GetGameRulesParams",
		"GetTeamRulesParam", "GetTeamRulesParams",
		"GetPlayerRulesParam", "GetPlayerRulesParams",
		"GetUnitRulesParam", "GetUnitRulesParams",
		"GetFeatureRulesParam", "GetFeatureRulesParams",
		// command-queue / cmd-desc / worker-task family: FULLY SERVED. The queue
		// callouts landed in PR 27b serving batch 2; the cmd-desc surface
		// (GetUnitCmdDescs/FindUnitCmdDesc, version-keyed on CCommandAI::
		// GetCmdDescVersion) and the builder worker-task read (GetUnitWorkerTask,
		// decoded at extraction) landed in sim|draw PR 30 -- see
		// LuaSnapshotServe::RefreshCommandQueues. Nothing from this family remains
		// on sanctionedLive.
		// weapon/shield state family: the SCALAR family is SERVED (sim|draw
		// PR 31) via the SimSnapshot::UnitRows weapon block + LuaSnapshotServe
		// twins -- GetUnitWeaponState/Damages/Vectors/Target/CanFire,
		// GetUnitShieldState, GetUnitStockpile, GetUnitFlanking. Only the trace
		// tests remain here: they recompute against the published collision world
		// (pick grid + blocking + LOS mirrors) in PR 35, not scalar rows.
		"GetUnitWeaponTryTarget", "GetUnitWeaponTestTarget",
		"GetUnitWeaponTestRange", "GetUnitWeaponHaveFreeLineOfFire",
		// deep per-unit state (moveType/CAI/second-object derefs): FULLY SERVED
		// in sim|draw PR 32. GetUnitStates/PosErrorParams/Storage/MetalExtraction/
		// BuildeeRadius/LastAttacker + the build-state family (IsBuilding/
		// BuildParams/InBuildStance/NanoPieces/EffectiveBuildRange/CurrentBuildPower)
		// + the transport pair (Transporter/IsTransporting) + Tooltip + the full
		// moveType table (GetUnitMoveTypeData, decision-2 full-copy block) route
		// through their LuaSnapshotServe twins over the new PR-32 SimSnapshot rows.
		// GetUnitCollisionVolumeData/PieceCollisionVolumeData/LastAttackedPiece
		// (the last reassigned to PR 32 by the SPECS amendments) are served from
		// PR 33's unit piece cache (now colVol+lastHit-capturing). Nothing from
		// this family remains sanctioned.
		// piece/script reads: SERVED (PR 33 pieces/scripts) via the
		// barrier-refreshed piece cache (LuaSnapshotServe::RefreshPieces --
		// static model metadata + captured piece transforms) plus the
		// ProjectileRows piece-projectile fields; the unit/feature Get*Piece*
		// + GetUnitScript* + GetFeatureCollisionVolumeData/LastAttackedPiece
		// + GetPieceProjectile* callouts route through their serving twins.
		// (The two UNIT collision-volume reads above are PR 32's, not PR 33's.)
		// spatial/list-query remainder (core family served PR 27b; the table
		// centroids, whole-list projectile/feature twins, projectile radius row,
		// and the draw-owned selection/group aggregates served PR 34)
		"GetUnitsInPlanes", // PR 34 deferred: GetFilteredUnits resets its array
		                    // counter per team, so multi-team allegiances
		                    // overwrite earlier slots -- an order-dependent quirk
		                    // the ascending-id snapshot can't reproduce/verify
		"GetUnitNearestAlly", "GetUnitNearestEnemy", // PR 34 deferred: CGameHelper
		                    // closest-search fidelity/tie-break; scalar return is
		                    // not id-set-verifiable (needs a design sign-off)
		"GetVisibleUnits", "GetVisibleFeatures", "GetVisibleProjectiles", // PR 34
		                    // deferred: camera-frustum + drawer drawflag-by-id
		                    // storage (a draw-side artifact not yet present)
		"GetUnitsInScreenRectangle", "GetFeaturesInScreenRectangle", // PR 34
		                    // deferred: camera screen-projection re-host
		// pathing (sim-owned pathManager)
		"GetUnitEstimatedPath", "RequestPath", "PathFinder::Next",
		"PathFinder::GetPathWayPoints", "PathFinder::DeletePath",
		"InitPathNodeCostsArray", "FreePathNodeCostsArray",
		"SetPathNodeCosts", "GetPathNodeCosts", "SetPathNodeCost",
		"GetPathNodeCost",
		// team/player misc: SERVED (sim|draw PR 36) via the TeamRows/PlayerRows
		// extensions + the SimSnapshot map-start cache; only GetPlayerTraffic
		// remains (its CGame per-player net-traffic map is not a snapshot row)
		"GetPlayerTraffic",
		// (b)-class torn-tolerant / wall-clock-dependent
		"GetGameState", // IsSimLagging reads the wall clock; serving it would false-flag the armed dual-run
		// unsynced-owned object flags + drawer-backed reads (LuaUnsyncedRead):
		// family SERVED (PR 27b serving batch 2, family 2) -- the ParseUnit/
		// ParseFeature gates answer from the snapshot Pov mirrors, payloads
		// stay unsynced/drawer-owned (LuaSnapshotServe parse-gate twins);
		// only the no-object camera test below remains sanctioned
		"IsSphereInView",
		// drawer-matrix-backed rotations: SERVED (PR 27b serving batch 2,
		// family 2; GetUnitRotation/GetFeatureRotation compose from the
		// drawer transform + snapshot basis rows)
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
	//
	// EXCEPT the value-safe subset: pure scalar/grid reads with no sim-owned
	// pointer or container traversal (terrain-type arrays, height grids, LOS
	// bitmaps). Their worst case is a torn word -- the tolerated section-C
	// class -- and denying them broke core UI (gui_info's bottom-left panel
	// dies on the first terrain hover when GetGroundInfo nils, windowed
	// dogfood round 5). Container-walking sanctioned entries (Test*Order,
	// rules params, pathing) stay denied until served.
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning() && !SimDrawSplit::IsSimParked()) {
		static const spring::unordered_set<std::string> splitRunningValueSafe = {
			"GetGroundInfo",       // typemap + terrain-type arrays (floats; SetTerrainTypeData is the rare writer)
			"GetTerrainTypeData",
			"GetSmoothMeshHeight", // float grid, sim-updated in place
			"GetGroundOrigHeight", // float grid
			"GetPositionLosState", // LOS bitmaps: int arrays mutated in place
			"IsPosInLos",
			"IsPosInRadar",
			"IsPosInAirLos",
			"GetRadarErrorParams", // per-allyteam scalars
		};

		if (splitRunningValueSafe.find(caller) != splitRunningValueSafe.end()) {
			stat.liveReads++;
			return false;
		}

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


size_t DrainBoundaryApplies()
{
	if (queuedOps.empty())
		return 0;

	// ops may not enqueue further ops (they are plain sim-state writes); a
	// swap keeps the invariant checkable and the vector's capacity reusable
	static std::vector<QueuedOp> draining;
	std::swap(draining, queuedOps);

	const size_t numApplied = draining.size();

	for (const QueuedOp& q: draining) {
		q.op();
	}

	draining.clear();
	std::swap(draining, queuedOps); // hand the capacity back

	return numApplied;
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
