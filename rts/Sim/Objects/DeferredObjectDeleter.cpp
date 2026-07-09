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
	sealedPending = 0; // PR 44a: an all-shells ack consumes any sealed batch
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
	sealedPending = 0; // PR 44a: an all-shells ack consumes any sealed batch
	epoch += 1;
}

void DeferredObjectDeleter::AckSealedDestroysEpoch(uint64_t epochId)
{
	// PR 44a: ack ONLY the producer-sealed prefix -- the tail's destroy
	// records have not dispatched (they ride the next epoch), so those shells
	// must stay readable
	if (sealedPending == 0)
		return;

	assert(sealedPending <= pending.size());

	for (size_t i = 0; i < sealedPending; ++i) {
		Entry& e = pending[i];
		DestructAndPoison(e);
		e.ackEpoch = epochId;
	}

	poisoned.insert(poisoned.end(), pending.begin(), pending.begin() + sealedPending);
	pending.erase(pending.begin(), pending.begin() + sealedPending);
	sealedPending = 0;
	epoch += 1;
}

void DeferredObjectDeleter::ReleaseRetired(uint64_t retiredEpochId)
{
	size_t kept = 0;

	for (Entry& e: poisoned) {
		if (e.ackEpoch <= retiredEpochId) {
			ReleaseSlot(e);
			continue;
		}
		poisoned[kept++] = e;
	}

	poisoned.resize(kept);
}

void DeferredObjectDeleter::ReleaseAcked()
{
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
	sealedPending = 0;

	ReleaseAcked();
}
