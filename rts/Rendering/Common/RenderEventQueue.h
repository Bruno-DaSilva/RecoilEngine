/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Sim/Misc/GlobalConstants.h"

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
 *  - Destruction is synchronous behind the same API. Drawer containers hold
 *    raw CUnit* / CFeature* / CProjectile* and all three destroy sites free the
 *    object immediately after firing, so a Render*Destroyed call first
 *    flushes every pending record (this preserves global order exactly: the
 *    queue is simply drained early) and then dispatches the destroy in place.
 *    PR-13 HANDOFF POINT: once the deferred-deletion epoch extends object
 *    lifetime past the draw boundary, the three Render*Destroyed methods
 *    become a plain Push and the mid-sim flushes disappear. Do not extend
 *    lifetimes here.
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
	// (doc/replay-seeking-architecture.md): keep them plain data. <obj> is a
	// transitional dispatch handle only -- guaranteed live at dispatch time
	// because destruction flushes the queue -- and goes away when PR 14
	// rekeys the drawer containers by ID.
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
		const void* obj;  // transitional, see above
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

		deferring = false;
	}

	/// drops all pending records without applying them; game teardown only
	/// (CGame::KillRendering), where the referenced objects and the drawers
	/// are both about to die without further drains
	void Clear() {
		records.clear();
		ghostMasks.clear();
		deferring = false;
	}

	bool Empty() const { return records.empty(); }

	// sim-side fire sites call these instead of eventHandler.Render*
	void RenderUnitPreCreated(const CUnit* unit);
	void RenderUnitCreated(const CUnit* unit, int cloaked);
	void RenderUnitDestroyed(const CUnit* unit);          // synchronous, see PR-13 handoff above
	void RenderFeaturePreCreated(const CFeature* feature);
	void RenderFeatureCreated(const CFeature* feature);
	void RenderFeatureDestroyed(const CFeature* feature); // synchronous, see PR-13 handoff above
	void RenderProjectileCreated(const CProjectile* p);
	void RenderProjectileDestroyed(const CProjectile* p); // synchronous, see PR-13 handoff above

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
	void Flush();
	void Dispatch(const Record& record);
private:
	std::vector<Record> records;
	// side pool for the rare records that carry a per-allyteam mask; indexed
	// by Record::arg1, cleared together with <records>
	std::vector<GhostAllyMask> ghostMasks;

	bool deferring = false;
	bool draining = false;
};

extern RenderEventQueue renderEventQueue;
