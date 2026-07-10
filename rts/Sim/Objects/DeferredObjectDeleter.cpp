/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "DeferredObjectDeleter.h"

#include <cassert>
#include <cstring> // memset

#include "Game/Game.h" // PR 44c: the valve's forced tail publish (ProduceEpochForPoolValve)
#include "Rendering/Common/RenderEventQueue.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureMemPool.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileMemPool.h"
#include "Rendering/GroundFlash.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitMemPool.h"
#include "System/Log/ILog.h"
#include "System/MainDefines.h"
#include "System/Misc/SpringTime.h"
#include "System/Platform/Threading.h"
#include "System/Platform/Watchdog.h"
#include "System/SimDrawSplit.h"

DeferredObjectDeleter deferredObjectDeleter;


// free page capacity left in a pool (pages never handed out plus pages
// returned for reuse); parked shells count as live until released
template<typename Pool>
static size_t FreePoolPages(const Pool& pool)
{
	const size_t livePages = (pool.alloc_size() - pool.freed_size()) / Pool::PAGE_SIZE();
	const size_t poolPages = Pool::NUM_PAGES();

	assert(livePages <= poolPages);
	return (poolPages - livePages);
}

#if !(defined(__x86_64) || defined(__x86_64__) || defined(_M_X64))
// FixedDynMemPool: NUM_PAGES() is per-chunk, NUM_CHUNKS() many chunks
template<size_t S, size_t N, size_t K, size_t A>
static size_t FreePoolPages(const FixedDynMemPool<S, N, K, A>& pool)
{
	using Pool = FixedDynMemPool<S, N, K, A>;

	const size_t livePages = (pool.alloc_size() - pool.freed_size()) / Pool::PAGE_SIZE();
	const size_t poolPages = Pool::NUM_CHUNKS() * Pool::NUM_PAGES();

	assert(livePages <= poolPages);
	return (poolPages - livePages);
}
#endif


size_t DeferredObjectDeleter::FreePoolHeadroom(ObjKind kind) const
{
	switch (kind) {
		case ObjKind::Unit       : return FreePoolPages(unitMemPool   );
		case ObjKind::Feature    : return FreePoolPages(featureMemPool);
		case ObjKind::Projectile :
		case ObjKind::GroundFlash: return FreePoolPages(projMemPool   );
	}

	return 0;
}

void DeferredObjectDeleter::WaitForEpochRetirementAtValve(ObjKind kind)
{
	// PR 44c (§3.4): the pool valve is "sim WAITS for epoch retirement".
	// Sim thread, MID-frame, out of pool headroom. Per round:
	//  - reclaim the pages of every epoch the draw side retired since the
	//    last check (ServiceRetiredReleases -- pools are sim-owned, and this
	//    IS the sim thread);
	//  - if headroom is back, resume the frame;
	//  - if nothing reclaimable remains anywhere in the pipeline
	//    (outstandingShells == 0), resume too -- the pool is near its cap
	//    with LIVE objects and waiting cannot help (the flag-off branch
	//    proceeds identically after its flush);
	//  - otherwise publish the mid-frame tail as a NORMAL producer epoch
	//    (ProduceEpochForPoolValve -> ProduceEpochAtSimEdge(force), pacing-
	//    gated inside: at most one publish per draw consume). The mid-frame
	//    rows/records are the same value-consistent momentary state the
	//    PR-44b valve produced; the §7.7 dead rows come from the pending
	//    ledger, so §3.6a id coverage holds for the tail batch. Once the
	//    tail (or an empty retirement-advancing epoch) is published, the
	//    draw side's ordinary consume -> ack -> next-acquire-retires flow
	//    makes its shells releasable -- it needs NOTHING from the sim, which
	//    is the §3.4 deadlock argument;
	//  - park for ~1ms (SimDrawSplit::ValveParkWait). The wait presents as a
	//    parked sim, so a concurrent RequestPause (lifecycle/lazy/input
	//    parks) is satisfied and holds full quiescence until its release.
	valveEngages += 1;

	const spring_time t0 = spring_gettime();

	for (;;) {
		ServiceRetiredReleases();

		if (FreePoolHeadroom(kind) >= EMERGENCY_HEADROOM_PAGES)
			break;
		if (outstandingShells.load(std::memory_order_relaxed) == 0)
			break;

		if (game != nullptr) {
			game->ProduceEpochForPoolValve();
			valveProducePasses += 1;
		}

		valveRounds += 1;
		Watchdog::ClearTimer(WDT_SIM);

		if (!SimDrawSplit::ValveParkWait())
			break; // sim-thread exit requested
	}

	valveWaitMs += (spring_gettime() - t0).toMilliSecsf();
}

void DeferredObjectDeleter::Park(ObjKind kind, void* obj)
{
	const size_t freePages = FreePoolHeadroom(kind);

	if (freePages < EMERGENCY_HEADROOM_PAGES) {
		// pool-pressure valve: a game near a pool cap must not run out of
		// pages just because shells wait for a dispatch that has not happened
		// yet (long catch-up burst)
		if (SimDrawSplit::Enabled() && Threading::IsSimThread()) {
			// under the split the queue and the drawer containers belong to
			// the draw side -- PR 44c (§3.4): publish the mid-frame tail and
			// wait for epoch retirement to return pages (no main-thread
			// valve service exists). The reclaimable check is the cross-stage
			// atomic: pending/poisoned are split across threads and the
			// backlog can sit in the sealed slot batches or the releasable
			// list, both invisible to the flag-off containers' emptiness.
			if (outstandingShells.load(std::memory_order_relaxed) > 0) {
				LOG_L(L_WARNING, "[DeferredObjectDeleter::%s] pool pressure (kind=%d, freePages=" _STPF_ "), waiting for epoch retirement", __func__, int(kind), freePages);

				WaitForEpochRetirementAtValve(kind);
			}
		} else if (!(pending.empty() && poisoned.empty())) {
			// dispatch the queued records in order right here -- exactly what
			// the queue did for every destroy before PR 13 -- and give all
			// slots back
			LOG_L(L_WARNING, "[DeferredObjectDeleter::%s] pool pressure (kind=%d, freePages=" _STPF_ "), emergency flush", __func__, int(kind), freePages);

			renderEventQueue.Flush();
			AckDrainedDestroys();
			ReleaseAcked();
		}
	}

	outstandingShells.fetch_add(1, std::memory_order_relaxed);
	pending.push_back({kind, obj});
}

void DeferredObjectDeleter::Defer(CUnit* unit)
{
	unit->PreDestruct();
	Park(ObjKind::Unit, unit);
}

void DeferredObjectDeleter::Defer(CFeature* feature)
{
	feature->PreDestruct();
	Park(ObjKind::Feature, feature);
}

void DeferredObjectDeleter::Defer(CProjectile* proj)
{
	proj->PreDestruct();
	Park(ObjKind::Projectile, proj);
}

void DeferredObjectDeleter::Defer(CGroundFlash* flash)
{
	// no PreDestruct: ground flashes have no sync-observable teardown; the
	// whole point is keeping the shell readable for the draw side's
	// barrier-copied flash list (PR 27b)
	Park(ObjKind::GroundFlash, flash);
}


void DeferredObjectDeleter::DestructAndPoison(const Entry& e) const
{
	switch (e.kind) {
		case ObjKind::Unit: {
			static_cast<CUnit*>(e.obj)->~CUnit();
			std::memset(e.obj, POISON_BYTE, UnitMemPool::PAGE_SIZE());
		} break;
		case ObjKind::Feature: {
			static_cast<CFeature*>(e.obj)->~CFeature();
			std::memset(e.obj, POISON_BYTE, FeatureMemPool::PAGE_SIZE());
		} break;
		case ObjKind::Projectile: {
			static_cast<CProjectile*>(e.obj)->~CProjectile();
			std::memset(e.obj, POISON_BYTE, ProjMemPool::PAGE_SIZE());
		} break;
		case ObjKind::GroundFlash: {
			static_cast<CGroundFlash*>(e.obj)->~CGroundFlash();
			std::memset(e.obj, POISON_BYTE, ProjMemPool::PAGE_SIZE());
		} break;
	}
}

void DeferredObjectDeleter::ReleaseSlot(const Entry& e) const
{
#ifndef NDEBUG
	// a non-poison byte here means something wrote through a dangling
	// pointer after the destroy was acked -- a found bug, root-cause it
	size_t pageSize = 0;

	switch (e.kind) {
		case ObjKind::Unit       : { pageSize = UnitMemPool::PAGE_SIZE();    } break;
		case ObjKind::Feature    : { pageSize = FeatureMemPool::PAGE_SIZE(); } break;
		case ObjKind::Projectile :
		case ObjKind::GroundFlash: { pageSize = ProjMemPool::PAGE_SIZE();    } break;
	}

	const uint8_t* bytes = static_cast<const uint8_t*>(e.obj);

	for (size_t i = 0; i < pageSize; ++i) {
		assert(bytes[i] == POISON_BYTE);
	}
#endif

	switch (e.kind) {
		case ObjKind::Unit       : { unitMemPool.freeMem(e.obj);    } break;
		case ObjKind::Feature    : { featureMemPool.freeMem(e.obj); } break;
		case ObjKind::Projectile :
		case ObjKind::GroundFlash: { projMemPool.freeMem(e.obj);    } break;
	}

	outstandingShells.fetch_sub(1, std::memory_order_relaxed);
}


void DeferredObjectDeleter::AckDrainedDestroys()
{
	// a previous Draw that returned early never reached ReleaseAcked();
	// those slots are long past their window, give them back first
	ReleaseAcked();

	for (const Entry& e: pending) {
		DestructAndPoison(e);
	}

	poisoned.swap(pending);
	epoch += 1;
}

void DeferredObjectDeleter::AckDrainedDestroysEpoch(uint64_t epochId)
{
	// PR 43: no auto-release of earlier poisoned slots here -- their pool
	// return is keyed to epoch retirement (ReleaseRetired), which the barrier
	// runs when the ring retires their epoch
	for (Entry& e: pending) {
		DestructAndPoison(e);
		e.ackEpoch = epochId;
	}

	poisoned.insert(poisoned.end(), pending.begin(), pending.end());
	pending.clear();
	epoch += 1;
}

void DeferredObjectDeleter::SealPendingBatch(int slot)
{
	// PR 44b: sim thread, at the produce edge -- move the batch's shells into
	// the epoch's slot (see the header); the slot's previous batch was acked
	// and cleared when that epoch dispatched
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);
	assert(slotBatches[slot].empty());

	slotBatches[slot].swap(pending);
	pending.clear();
}

void DeferredObjectDeleter::AckSealedDestroysEpoch(int slot, uint64_t epochId)
{
	// PR 44a/44b: ack ONLY the held epoch's sealed batch -- the tail's
	// destroy records have not dispatched (they ride the next epoch), so
	// those shells must stay readable. Runs on the main thread; destruct +
	// poison touch only the shells' own memory (PreDestruct already ran all
	// sync-observable teardown on the sim side), the pool return is deferred
	// to the sim (ReleaseRetired -> ServiceRetiredReleases).
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);

	std::vector<Entry>& batch = slotBatches[slot];

	if (batch.empty())
		return;

	for (Entry& e: batch) {
		DestructAndPoison(e);
		e.ackEpoch = epochId;
	}

	poisoned.insert(poisoned.end(), batch.begin(), batch.end());
	batch.clear();
	epoch += 1;
}

void DeferredObjectDeleter::ReleaseRetired(uint64_t retiredEpochId)
{
	// PR 44b: pools are sim-owned and the sim may be running -- move the
	// retired slots to the releasable list; the pool return happens on the
	// sim thread (ServiceRetiredReleases) or at a provably-quiescent
	// ReleaseAcked (lockstep / valve / teardown)
	size_t kept = 0;
	{
		std::lock_guard<std::mutex> lock(releasableMtx);

		for (Entry& e: poisoned) {
			if (e.ackEpoch <= retiredEpochId) {
				releasable.push_back(e);
				continue;
			}
			poisoned[kept++] = e;
		}
	}

	poisoned.resize(kept);
}

void DeferredObjectDeleter::ServiceRetiredReleases()
{
	// swap out under the lock, free outside it (pool ops are the caller's
	// thread's own -- sim thread at its edge, or quiescent main)
	static std::vector<Entry> draining;
	{
		std::lock_guard<std::mutex> lock(releasableMtx);

		if (releasable.empty())
			return;

		std::swap(draining, releasable);
	}

	for (const Entry& e: draining) {
		ReleaseSlot(e);
	}

	draining.clear();
}

void DeferredObjectDeleter::ReleaseAcked()
{
	// caller guarantees sim quiescence (flag-off end-of-Draw, valve service,
	// lockstep, teardown) -- give back the retired backlog too
	ServiceRetiredReleases();

	for (const Entry& e: poisoned) {
		ReleaseSlot(e);
	}

	poisoned.clear();
}

void DeferredObjectDeleter::Clear()
{
	for (const Entry& e: pending) {
		DestructAndPoison(e);
	}

	poisoned.insert(poisoned.end(), pending.begin(), pending.end());
	pending.clear();

	for (auto& batch: slotBatches) {
		for (const Entry& e: batch) {
			DestructAndPoison(e);
		}

		poisoned.insert(poisoned.end(), batch.begin(), batch.end());
		batch.clear();
	}

	ReleaseAcked();
}

void DeferredObjectDeleter::DumpValveStats() const
{
	// PR 44c telemetry (quiescent teardown read; zero-line suppressed --
	// the valve never engages unless a pool nears its cap)
	if (valveEngages == 0)
		return;

	LOG("[PoolValveStats] engages=%llu rounds=%llu producePasses=%llu totalWaitMs=%.1f",
	    (unsigned long long)valveEngages,
	    (unsigned long long)valveRounds,
	    (unsigned long long)valveProducePasses,
	    valveWaitMs);
}
