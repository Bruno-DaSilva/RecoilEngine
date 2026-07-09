/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "Sim/Misc/GlobalConstants.h"
#include "System/UnorderedMap.hpp"

class CUnit;
class CFeature;
class CProjectile;

// one bit per allyteam (allyteam count is bounded by MAX_TEAMS); carries the
// event-time dead-ghost decision of UnitLeavesGhostChanged records, which
// cannot be recomputed at drain time because sim clears the LOS_PREVLOS bits
// it is derived from right after firing the event (CUnit::SetLeavesGhost)
using GhostAllyMask = std::array<uint64_t, (MAX_TEAMS + 63) / 64>;

/**
 * @brief Sim -> render event delta queue
 *
 * Coupling inventory section B of doc/sim-draw-thread-decoupling-research.md
 * (PR 12): sim used to notify the drawers of object creation, destruction and
 * LOS transitions synchronously from inside SimFrame, mutating drawer
 * containers, ghost lists, decal owners and the SSBO allocation maps mid-sim.
 * These notifications are now plain-data records appended in fire order
 * during the sim phase and drained -- everything, in exact append order, once
 * -- at the start of CGame::Draw, before any draw-side consumer reads the
 * drawer containers. This formalizes the sim->render event ordering for the
 * future sim/draw split; it does not add threads.
 *
 * Semantics:
 *  - Catch-up: any number of sim frames may append records before one drain
 *    (fast-forward, minDrawFPS budgeting). The drain applies them all, in
 *    order. Handlers run against object state as of the drain, which is safe
 *    only for fields that are immutable (model, def) or where latest-state
 *    reads are end-state-equivalent (icon state); any fact that sim may
 *    overwrite before the drain is captured into the record at fire time
 *    (the leavesGhost bit of LOS records, the dead-ghost allyteam mask).
 *  - Destruction queues like everything else (PR 13). The three destroy
 *    sites no longer free the object: they run PreDestruct() -- the
 *    sync-observable destructor half, at unchanged sim time -- and park the
 *    undestructed shell in DeferredObjectDeleter. The shell's plain fields
 *    and localModel stay readable, so destroy records (and any earlier
 *    records of the same object: creation, LOS) dispatch against valid
 *    memory in exact fire order. Right after Drain() returns, CGame::Draw
 *    calls DeferredObjectDeleter::AckDrainedDestroys(), which destructs and
 *    poisons the shells; their pool slots are released at the end of that
 *    Draw. Consequence for handlers: after the drain, drawer containers
 *    hold live objects only -- every parked shell had its destroy record
 *    dispatched by then.
 *  - Outside the sim phase the queue is in immediate mode and dispatches in
 *    place: game-load object creation and draw-callin projectile spawns
 *    (e.g. Lua Spring.SpawnCEG from DrawWorld) keep master's timing. The
 *    queue is empty whenever immediate mode is active (the boundary drain is
 *    what leaves the sim phase), so in-place dispatch cannot overtake queued
 *    records.
 *  - Known benign reorder: decal bookkeeping driven by non-queued events
 *    (UnitMoved/UnitLoaded/FeatureMoved) can now run before the create record
 *    of the same object is applied; all those paths are keyed lookups that
 *    tolerate the missing entry (GroundDecalHandler::MoveSolidObject
 *    add-or-update), and end state matches unless an object with both a
 *    ground-plate and a track decal moves in its spawn burst (cosmetic).
 *  - Single-threaded by design: records are appended from the main thread
 *    (sim phase) or dispatched in place on the loading thread (immediate
 *    mode); the drain runs on the main thread. Not serialized by creg --
 *    records pending at save time are lost, matching a queue drained at the
 *    save boundary.
 *
 * The heightmap's unsyncedHeightMapUpdates queue (ReadMap.cpp) is the in-tree
 * precedent for the pattern.
 */
class RenderEventQueue {
public:
	// these records are the seed of the recorded-stream event schema
	// (doc/replay-seeking-architecture.md): plain data, no object pointers
	// (PR 14). Dispatch resolves <id> back to the object: the live one from
	// its handler, or -- for records fired by an object generation that died
	// before the drain -- the parked shell tracked in <pendingDestroyShells>.
	struct Record {
		enum class Type : uint8_t {
			UnitPreCreated,
			UnitCreated,
			UnitDestroyed,
			FeaturePreCreated,
			FeatureCreated,
			FeatureDestroyed,
			ProjectileCreated,
			ProjectileDestroyed,
			UnitEnteredLos,
			UnitLeftLos,
			UnitEnteredRadar,
			UnitLeftRadar,
			UnitLeavesGhostChanged,
		};

		Type type;
		bool syncedProj;  // projectile records: which of the two ID namespaces <id> lives in
		int32_t id;       // unitID / featureID / projectileID
		int32_t arg1;     // UnitCreated: cloaked; LOS records: allyTeam; UnitLeavesGhostChanged: ghost-mask pool index
		int32_t arg2;     // UnitEnteredLos/UnitLeftLos: unit->leavesGhost at fire time
	};
public:
	/// opens the deferral window; called at the top of CGame::Update, ahead
	/// of net-message processing and the SimFrames it issues
	void BeginSimPhase() { deferring = true; }

	/// the draw boundary: applies all pending records in order and returns
	/// the queue to immediate mode; called at the start of CGame::Draw
	void Drain() {
		// no dispatched handler enqueues further records today, but loop so
		// the immediate-mode empty-queue invariant holds even if one ever does
		while (!records.empty())
			Flush();

		// every queued destroy record just dispatched, popping its shell
		assert(pendingDestroyShells.empty());

		deferring = false;
	}

	/// drops all pending records without applying them; game teardown only
	/// (CGame::KillRendering), where the referenced objects and the drawers
	/// are both about to die without further drains
	void Clear() {
		records.clear();
		ghostMasks.clear();
		pendingDestroyShells.clear();
		boundaryDestroyedUnits.clear();
		boundaryDestroyedProjectiles.clear();
		boundaryDeadUnits.clear();
		boundaryDeadFeatures.clear();
		boundaryDeadProjectiles.clear();
		batchCoverageRefs.clear();
		deferring = false;
	}

	// PR 27b: shells of the objects whose destroy records dispatched since
	// the last consume -- the boundary replacement for draw-side death-
	// dependences (selection, wait-AI, tracked lights). Only populated when
	// the split flag is on; consumed by CGame right after the drain (the
	// shells stay readable until the barrier's ack).
	const std::vector<const CUnit*>& BoundaryDestroyedUnits() const { return boundaryDestroyedUnits; }
	const std::vector<const CProjectile*>& BoundaryDestroyedProjectiles() const { return boundaryDestroyedProjectiles; }
	void ClearBoundaryDestroys() {
		boundaryDestroyedUnits.clear();
		boundaryDestroyedProjectiles.clear();
	}

	// PR 27b: id->shell resolution for the boundary drain window. The
	// deferred unsynced dispatches (step 7) replay events for objects that
	// died later in the same sim burst; master ran those handlers mid-frame
	// with the object alive, so their Spring.Get*/gl.Set*BufferUniforms
	// reads must resolve to the (readable-until-ack) shell instead of nil --
	// BAR's unsynced-LuaRules ecosystem breaks at scale otherwise (windowed
	// dogfood, 2026-07-06). Populated at destroy-record dispatch, cleared
	// right before the ack poisons the shells.
	const CUnit* ResolveBoundaryDeadUnit(int id) const {
		const auto it = boundaryDeadUnits.find(id);
		return (it == boundaryDeadUnits.end()) ? nullptr : it->second;
	}
	const CFeature* ResolveBoundaryDeadFeature(int id) const {
		const auto it = boundaryDeadFeatures.find(id);
		return (it == boundaryDeadFeatures.end()) ? nullptr : it->second;
	}
	void ClearBoundaryDeadShells() {
		boundaryDeadUnits.clear();
		boundaryDeadFeatures.clear();
		boundaryDeadProjectiles.clear();
	}

	// PR 43 §2.6/§3.6a: the ARMED ID-COVERAGE GATE's input -- every object id
	// referenced by a record dispatched in this batch (unit/feature ids + the
	// SYNCED-namespace projectile ids; unsynced projectiles have no rows by
	// design and are excluded per the fd41dbdd92 namespace rule). After the
	// publish, CGame runs SimSnapshot::CheckEpochIdCoverage over this list:
	// every id must resolve in the published epoch as ACTIVE or
	// DEAD_THIS_BATCH -- 44b's make-or-break invariant, proven under the park
	// now. Populated only while draining under the split; cleared with the
	// dead-shell maps at the step-8 window close (or by the valve service,
	// which cannot check -- it does not publish).
	struct CoverageRef {
		uint8_t kind; // 0 unit, 1 feature, 2 synced projectile
		int32_t id;
	};
	const std::vector<CoverageRef>& BatchCoverageRefs() const { return batchCoverageRefs; }
	void ClearBatchCoverageRefs() { batchCoverageRefs.clear(); }

	// PR 43 §7.7: the batch's died-in-batch (id -> shell) maps, consumed by
	// the producer (SimSnapshot::ExtractDeadRowsFromShells) to extract genuine
	// at-death DEAD_THIS_BATCH rows at the frame edge. Same population/clear
	// lifecycle as the Resolve* read maps above (destroy-record dispatch /
	// step-8 window close); the projectile map is filtered to the SYNCED id
	// namespace at dispatch (the fd41dbdd92 rule -- ProjectileRows holds only
	// synced projectiles, so an unsynced destroy id must never mark one).
	const spring::unordered_map<int, const CUnit*>& BoundaryDeadUnits() const { return boundaryDeadUnits; }
	const spring::unordered_map<int, const CFeature*>& BoundaryDeadFeatures() const { return boundaryDeadFeatures; }
	const spring::unordered_map<int, const CProjectile*>& BoundaryDeadProjectiles() const { return boundaryDeadProjectiles; }

	bool Empty() const { return records.empty(); }

	/// dispatches all pending records in order without closing the deferral
	/// window; pool-pressure valve of DeferredObjectDeleter only (this is
	/// the same in-place dispatch every destroy performed before PR 13)
	void Flush();

	// mid-sim-phase resolution of drawer-container ids (PR 14): the post-drain
	// invariant (every id in a drawer container resolves to a live object)
	// only holds between the boundary drain and the next sim phase. Event
	// handlers that run *inside* the sim phase and read drawer containers
	// (CUnitDrawerData::PlayerChanged via net-message processing, Lua
	// Spring.GetRenderUnits from unsynced gadget sim-context handlers) can
	// meet ids whose owner died since the last drain -- deregistered from the
	// handler, destroy record pending, shell parked (PR 13). These return
	// that shell (readable until the drain by the PR-13 contract), preserving
	// master's semantics where the container held the still-readable pointer.
	// nullptr when no such shell is pending.
	const CUnit* FindPendingDestroyUnit(int32_t id) const {
		return static_cast<const CUnit*>(FindDestroyShell(ShellKey(ObjKind::Unit, false, id)));
	}
	const CFeature* FindPendingDestroyFeature(int32_t id) const {
		return static_cast<const CFeature*>(FindDestroyShell(ShellKey(ObjKind::Feature, false, id)));
	}

	// sim-side fire sites call these instead of eventHandler.Render*
	void RenderUnitPreCreated(const CUnit* unit);
	void RenderUnitCreated(const CUnit* unit, int cloaked);
	void RenderUnitDestroyed(const CUnit* unit);
	void RenderFeaturePreCreated(const CFeature* feature);
	void RenderFeatureCreated(const CFeature* feature);
	void RenderFeatureDestroyed(const CFeature* feature);
	void RenderProjectileCreated(const CProjectile* p);
	void RenderProjectileDestroyed(const CProjectile* p);

	// LOS-transition / ghost deltas, enqueued by CUnitDrawerData's event
	// handlers (the events themselves also have sim-time consumers -- Lua,
	// AI -- so only the drawer side defers); applied via CUnitDrawer::Apply*
	void UnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhost);
	void UnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhost);
	void UnitEnteredRadar(const CUnit* unit, int allyTeam);
	void UnitLeftRadar(const CUnit* unit, int allyTeam);
	void UnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask);
private:
	void Push(const Record& record);
	void Dispatch(const Record& record);

	// dispatch-time id -> object resolution (PR 14). Keyed by object kind +
	// (for projectiles) id namespace + id; the value is the FIFO of parked
	// shells whose destroy records are still queued. A queued record fired by
	// generation N of a reused id resolves to generation N's shell because
	// fire order interleaves generations strictly (a PreCreated of gen N+1
	// can only follow the Destroyed of gen N, which pops gen N's shell); with
	// no pending destroy for the id, the handler lookup returns the live
	// object, which by the same ordering is the generation that fired the
	// record. Only populated while deferring: in immediate mode destroy
	// records dispatch at the fire site, where the object is still registered
	// in its handler (all three destroy sites fire before deregistering).
	enum class ObjKind : uint8_t { Unit, Feature, Projectile };
	static uint64_t ShellKey(ObjKind kind, bool synced, int32_t id) {
		return (uint64_t(kind) << 40) | (uint64_t(synced) << 39) | uint32_t(id);
	}
	void PushDestroyShell(uint64_t key, const void* obj);
	const void* PopDestroyShell(uint64_t key);      // dispatch of a *Destroyed record
	const void* FindDestroyShell(uint64_t key) const;

	const CUnit* ResolveUnit(int32_t id) const;
	const CFeature* ResolveFeature(int32_t id) const;
	const CProjectile* ResolveProjectile(int32_t id, bool synced) const;
private:
	std::vector<Record> records;
	// PR 27b boundary death relay (see BoundaryDestroyedUnits)
	std::vector<const CUnit*> boundaryDestroyedUnits;
	std::vector<const CProjectile*> boundaryDestroyedProjectiles;

	// see ResolveBoundaryDeadUnit/Feature + the PR-43 BoundaryDead* accessors
	spring::unordered_map<int, const CUnit*> boundaryDeadUnits;
	spring::unordered_map<int, const CFeature*> boundaryDeadFeatures;
	// PR 43: synced-namespace-only (see BoundaryDeadProjectiles)
	spring::unordered_map<int, const CProjectile*> boundaryDeadProjectiles;
	// PR 43: see BatchCoverageRefs
	std::vector<CoverageRef> batchCoverageRefs;
	// side pool for the rare records that carry a per-allyteam mask; indexed
	// by Record::arg1, cleared together with <records>
	std::vector<GhostAllyMask> ghostMasks;

	// parked shells (DeferredObjectDeleter) with a queued destroy record, in
	// fire order per key; see the resolution comment above
	spring::unordered_map<uint64_t, std::vector<const void*>> pendingDestroyShells;

	bool deferring = false;
	bool draining = false;
};

extern RenderEventQueue renderEventQueue;
