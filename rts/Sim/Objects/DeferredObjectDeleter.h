/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <vector>

class CUnit;
class CFeature;
class CProjectile;

/**
 * @brief Deferred-deletion epoch for pooled sim objects (units, features,
 * projectiles) -- PR 13 of doc/sim-draw-thread-decoupling-research.md.
 *
 * Drawer containers, the render-event queue and Lua render callins reference
 * sim objects by pointer past the sim frame that destroys them, so the
 * mempool slot of a destroyed object must not be reused before the draw side
 * has consumed the object's destroy record at the boundary drain. Sim-side
 * destruction is split in two:
 *
 *  - Defer(obj), called at the exact site that used to call <pool>.free(obj):
 *    runs obj->PreDestruct() -- the old destructor bodies, i.e. every
 *    sync-observable side effect (death-dependence severing, wreck creation,
 *    quadfield/blocking-map removal, weapon/script/AI teardown) -- at
 *    unchanged sim time, then parks the undestructed shell. The shell keeps
 *    its plain fields and localModel readable, so queued render records
 *    (creation, LOS transitions, the destroy itself) dispatch against valid
 *    memory in exact fire order at the drain; nothing needs to be captured.
 *
 *  - AckDrainedDestroys(), the single explicit ack point, called right after
 *    RenderEventQueue::Drain() in CGame::Draw: every destroy record queued
 *    before the drain has now been dispatched, so the draw side is done with
 *    the shells. Runs the real destructors (which skip the PreDestruct()ed
 *    half, leaving only member teardown -- plain memory, sync-blind to
 *    defer) and overwrites each slot with POISON_BYTE.
 *
 *  - ReleaseAcked(), end of the same CGame::Draw: returns the poisoned slots
 *    to their pools (freeMem zeroes them). Between ack and release the slot
 *    holds a recognizable poison pattern: the pools zero pages on free and
 *    ASan cannot see pooled use-after-free, so any draw-side code that still
 *    dereferences the pointer after the destroy was acked crashes on
 *    0xDEDEDE.. instead of silently reading a zeroed or recycled slot. A
 *    crash on the pattern is a dangling-pointer bug found, not caused.
 *
 * Sync safety: PreDestruct() runs at the identical call site and in the
 * identical body order as the old destructors, so all sync-observable timing
 * is bit-identical to master. Deferring the remaining member teardown and
 * the slot reuse only changes allocation order, which synced code provably
 * cannot observe (address-blindness: cross-machine sync already works with
 * different addresses on every client). Verified by the master-demo resim
 * gate.
 *
 * Pool pressure: the pools are fixed-size (MAX_UNITS/MAX_FEATURES/
 * MAX_PROJECTILES pages) and parked shells keep their pages, so a game
 * running at the cap could exhaust a pool during a long catch-up burst.
 * Defer() checks headroom and, below EMERGENCY_HEADROOM_PAGES, flushes the
 * render-event queue in place (same in-order dispatch the queue used for
 * every destroy before PR 13) and releases everything immediately. Under
 * the future sim/draw split this valve becomes "sim waits for the boundary".
 *
 * Single-threaded like the queue it pairs with; not creg-serialized (shells
 * are severed from every sim container before Defer(), so no save can reach
 * them, and loads start with an empty deleter -- Clear() runs on teardown).
 */
class DeferredObjectDeleter {
public:
	// recognizable filler for slots inside the deferred window; reads as
	// 0xDEDEDEDEDEDEDEDE in pointer fields (non-canonical, faults on use)
	static constexpr uint8_t POISON_BYTE = 0xDE;

	static constexpr size_t EMERGENCY_HEADROOM_PAGES = 64;

	// replacements for <pool>.free(obj) at the three destroy sites; run
	// PreDestruct() and park the shell until the destroy record drains
	void Defer(CUnit* unit);
	void Defer(CFeature* feature);
	void Defer(CProjectile* proj);

	// THE ack point: called once, right after renderEventQueue.Drain() in
	// CGame::Draw. Destructs + poisons all parked shells (their destroy
	// records were just dispatched). Also releases slots a short-circuited
	// previous Draw left poisoned.
	void AckDrainedDestroys();

	// returns poisoned slots to their pools; end of CGame::Draw
	void ReleaseAcked();

	// teardown/reload only: destruct and free everything immediately
	void Clear();

	bool Empty() const { return (pending.empty() && poisoned.empty()); }

private:
	enum class ObjKind : uint8_t {
		Unit,
		Feature,
		Projectile,
	};

	struct Entry {
		ObjKind kind;
		void* obj;
	};

	void Park(ObjKind kind, void* obj);
	void DestructAndPoison(const Entry& e) const;
	void ReleaseSlot(const Entry& e) const;

private:
	// PreDestruct()ed shells whose destroy records have not drained yet
	std::vector<Entry> pending;
	// destructed + poisoned slots awaiting return to their pool
	std::vector<Entry> poisoned;

	uint64_t epoch = 0; // completed ack cycles, diagnostics only
};

extern DeferredObjectDeleter deferredObjectDeleter;
