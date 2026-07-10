/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "DeferredObjectDeleter.h"

#include <cassert>
#include <cstring> // memset

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
#include "System/Platform/Threading.h"
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


void DeferredObjectDeleter::Park(ObjKind kind, void* obj)
{
	size_t freePages = 0;

	switch (kind) {
		case ObjKind::Unit       : { freePages = FreePoolPages(unitMemPool   ); } break;
		case ObjKind::Feature    : { freePages = FreePoolPages(featureMemPool); } break;
		case ObjKind::Projectile :
		case ObjKind::GroundFlash: { freePages = FreePoolPages(projMemPool   ); } break;
	}

	if (freePages < EMERGENCY_HEADROOM_PAGES && !(pending.empty() && poisoned.empty())) {
		// pool-pressure valve: a game near a pool cap must not run out of
		// pages just because shells wait for a drain that has not happened
		// yet (long catch-up burst)
		if (SimDrawSplit::Enabled() && Threading::IsSimThread()) {
			// under the split the queue and the drawer containers belong to
			// the draw side -- "sim waits for the boundary" (the PR-13 plan):
			// park mid-frame; CGame::AcquireSimPause services the park by
			// flushing + acking + releasing on the main thread, then resumes
			// us with fresh pages
			LOG_L(L_WARNING, "[DeferredObjectDeleter::%s] pool pressure (kind=%d, freePages=" _STPF_ "), parking for the boundary", __func__, int(kind), freePages);

			SimDrawSplit::ParkAtValve();
		} else {
			// dispatch the queued records in order right here -- exactly what
			// the queue did for every destroy before PR 13 -- and give all
			// slots back
			LOG_L(L_WARNING, "[DeferredObjectDeleter::%s] pool pressure (kind=%d, freePages=" _STPF_ "), emergency flush", __func__, int(kind), freePages);

			renderEventQueue.Flush();
			AckDrainedDestroys();
			ReleaseAcked();
		}
	}

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
