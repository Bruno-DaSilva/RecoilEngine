/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RenderEventQueue.h"

#include <cassert>

#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/Platform/Threading.h"
#include "System/SimDrawSplit.h"
#include "System/Sync/SyncChecker.h"
#include "System/Threading/ThreadPool.h"

RenderEventQueue renderEventQueue;


void RenderEventQueue::Push(const Record& record)
{
	// PR 27b: between barriers the record containers belong to the sim
	// thread; a main-thread Push while it runs would race (game-load and
	// boundary dispatches happen with the sim thread parked or not spawned).
	// Workers in the sim's own fork-join (MT projectile spawns, serialized
	// by CProjectile::mut) are sim-side and legal.
	// the hazard is a MAIN-thread Push while the sim runs unparked; no
	// per-thread flag identifies sim-side workers reliably
	// (~MultithreadedSection clears unconditionally), so assert the inverse
	assert(!SimDrawSplit::Enabled() || !SimDrawSplit::SimThreadRunning() ||
	       !Threading::IsMainThread() || SimDrawSplit::IsSimParked());

	if (!deferring) {
		// immediate mode is only ever active with an empty queue (Drain is
		// what ends the sim phase), so dispatching in place preserves order
		assert(records.empty());
		Dispatch(record, ghostMasks);
		ghostMasks.clear();
		return;
	}

	records.push_back(record);
}

void RenderEventQueue::Flush()
{
	if (records.empty())
		return;

	DispatchRange(records.size());
}

void RenderEventQueue::SealEpochBatch(int slot)
{
	// PR 44b: sim thread, at the produce edge -- MOVE the pending records +
	// their ghost-mask pool into the epoch's slot batch (the slot's previous
	// batch was consumed when that epoch dispatched; produce-after-consume
	// pacing guarantees no still-pending batch is overwritten). New pushes
	// start a fresh live container (mask indices restart at 0 with it).
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);
	assert(epochBatches[slot].records.empty());

	epochBatches[slot].records.swap(records);
	epochBatches[slot].ghostMasks.swap(ghostMasks);
	records.clear();
	ghostMasks.clear();
}

void RenderEventQueue::DispatchSealedBatch(int slot)
{
	// PR 44a/44b consumer half: dispatch exactly the held epoch's sealed
	// batch; the post-seal tail (sim-owned live container) belongs to the
	// next epoch, and the deferral window stays open (the sim phase never
	// ends under the flip)
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);

	EpochBatch& batch = epochBatches[slot];

	if (batch.records.empty())
		return;

	DispatchSpan(batch.records, batch.ghostMasks);

	batch.records.clear();
	batch.ghostMasks.clear();
}

void RenderEventQueue::DispatchRange(const size_t numRecords)
{
	assert(!draining);
	draining = true;

	// index loop by value: dispatched handlers do not fire further queued
	// events today, but stay safe against appends invalidating iterators
	for (size_t i = 0; i < numRecords; ++i) {
		const Record record = records[i];
		DispatchOne(record, ghostMasks);
	}

	records.erase(records.begin(), records.begin() + numRecords);

	if (records.empty())
		ghostMasks.clear();

	draining = false;
}

void RenderEventQueue::DispatchSpan(const std::vector<Record>& span, const std::vector<GhostAllyMask>& masks)
{
	assert(!draining);
	draining = true;

	for (const Record& record: span)
		DispatchOne(record, masks);

	draining = false;
}

// per-record coverage-ref collection + dispatch (shared by the live-range
// and slot-batch loops)
void RenderEventQueue::DispatchOne(const Record& record, const std::vector<GhostAllyMask>& masks)
{
	// PR 43 §2.6: collect the batch's referenced ids for the epoch
	// id-coverage gate (checked after the publish; see BatchCoverageRefs).
	// Unsynced projectile records are excluded (no row namespace).
	if (SimDrawSplit::Enabled()) {
		switch (record.type) {
			case Record::Type::UnitPreCreated:
			case Record::Type::UnitCreated:
			case Record::Type::UnitDestroyed:
			case Record::Type::UnitEnteredLos:
			case Record::Type::UnitLeftLos:
			case Record::Type::UnitEnteredRadar:
			case Record::Type::UnitLeftRadar:
			case Record::Type::UnitLeavesGhostChanged:
				batchCoverageRefs.push_back({0, record.id});
				break;
			case Record::Type::FeaturePreCreated:
			case Record::Type::FeatureCreated:
			case Record::Type::FeatureDestroyed:
				batchCoverageRefs.push_back({1, record.id});
				break;
			case Record::Type::ProjectileCreated:
			case Record::Type::ProjectileDestroyed:
				if (record.syncedProj)
					batchCoverageRefs.push_back({2, record.id});
				break;
		}
	}

	Dispatch(record, masks);
}


void RenderEventQueue::PushDestroyShell(uint64_t key, const void* obj)
{
	std::lock_guard<std::mutex> lock(shellMtx);
	pendingDestroyShells[key].push_back(obj);
}

const void* RenderEventQueue::FindDestroyShell(uint64_t key) const
{
	std::lock_guard<std::mutex> lock(shellMtx);
	const auto it = pendingDestroyShells.find(key);

	if (it == pendingDestroyShells.end())
		return nullptr;

	assert(!it->second.empty());
	return it->second.front();
}

const void* RenderEventQueue::PopDestroyShell(uint64_t key)
{
	std::lock_guard<std::mutex> lock(shellMtx);
	const auto it = pendingDestroyShells.find(key);

	// immediate mode: the destroy dispatched at the fire site, before
	// Defer() parked the shell -- nothing was registered
	if (it == pendingDestroyShells.end())
		return nullptr;

	assert(!it->second.empty());
	const void* obj = it->second.front();

	it->second.erase(it->second.begin());

	if (it->second.empty())
		pendingDestroyShells.erase(it);

	return obj;
}


void RenderEventQueue::CollectPendingDeadShells(
	std::vector<std::pair<int, const CUnit*>>& outUnits,
	std::vector<std::pair<int, const CFeature*>>& outFeatures,
	std::vector<std::pair<int, const CProjectile*>>& outProjectiles) const
{
	// PR 44b: sim thread at the produce edge; the main thread pops/reads
	// concurrently (destroy-record dispatch / resolution cold-misses)
	std::lock_guard<std::mutex> lock(shellMtx);

	for (const auto& [key, fifo] : pendingDestroyShells) {
		assert(!fifo.empty());

		const ObjKind kind = static_cast<ObjKind>(key >> 40);
		const bool synced = ((key >> 39) & 1) != 0;
		const int32_t id = static_cast<int32_t>(static_cast<uint32_t>(key & 0xffffffffull));

		// newest generation per id: the FIFO back (fire order interleaves
		// generations strictly, see the resolution comment in the header)
		const void* shell = fifo.back();

		switch (kind) {
			case ObjKind::Unit: {
				outUnits.emplace_back(id, static_cast<const CUnit*>(shell));
			} break;
			case ObjKind::Feature: {
				outFeatures.emplace_back(id, static_cast<const CFeature*>(shell));
			} break;
			case ObjKind::Projectile: {
				// fd41dbdd92: SYNCED namespace only (ProjectileRows holds only
				// synced projectiles; an unsynced destroy id must never mark one)
				if (synced)
					outProjectiles.emplace_back(id, static_cast<const CProjectile*>(shell));
			} break;
		}
	}
}

const CUnit* RenderEventQueue::ResolveUnit(int32_t id) const
{
	if (const void* shell = FindDestroyShell(ShellKey(ObjKind::Unit, false, id)))
		return static_cast<const CUnit*>(shell);

	const CUnit* unit = unitHandler.GetUnit(id);
	assert(unit != nullptr);
	return unit;
}

const CFeature* RenderEventQueue::ResolveFeature(int32_t id) const
{
	if (const void* shell = FindDestroyShell(ShellKey(ObjKind::Feature, false, id)))
		return static_cast<const CFeature*>(shell);

	const CFeature* feature = featureHandler.GetFeature(id);
	assert(feature != nullptr);
	return feature;
}

const CProjectile* RenderEventQueue::ResolveProjectile(int32_t id, bool synced) const
{
	if (const void* shell = FindDestroyShell(ShellKey(ObjKind::Projectile, synced, id)))
		return static_cast<const CProjectile*>(shell);

	const CProjectile* proj = synced ?
		projectileHandler.GetProjectileBySyncedID(id) :
		projectileHandler.GetProjectileByUnsyncedID(id);

	if (proj == nullptr)
		LOG_L(L_ERROR, "[RenderEventQueue::%s] no live object or shell for id=%d synced=%d (pending=%d)", __func__, id, int(synced), int(records.size()));

	assert(proj != nullptr);
	return proj;
}


void RenderEventQueue::Dispatch(const Record& record, const std::vector<GhostAllyMask>& masks)
{
	using T = Record::Type;

	switch (record.type) {
		case T::UnitPreCreated: {
			eventHandler.RenderUnitPreCreated(ResolveUnit(record.id));
		} break;
		case T::UnitCreated: {
			eventHandler.RenderUnitCreated(ResolveUnit(record.id), record.arg1);
		} break;
		case T::UnitDestroyed: {
			const CUnit* unit = ResolveUnit(record.id);

			eventHandler.RenderUnitDestroyed(unit);
			PopDestroyShell(ShellKey(ObjKind::Unit, false, record.id));

			// PR 27b: draw-owned dependents (selection, wait-AI, lights) get
			// their death notifications from the boundary instead of
			// death-dependences; CGame consumes this after the drain
			if (SimDrawSplit::Enabled()) {
				boundaryDestroyedUnits.push_back(unit);
				boundaryDeadUnits[record.id] = unit; // drain-window shell resolution
			}
		} break;

		case T::FeaturePreCreated: {
			eventHandler.RenderFeaturePreCreated(ResolveFeature(record.id));
		} break;
		case T::FeatureCreated: {
			eventHandler.RenderFeatureCreated(ResolveFeature(record.id));
		} break;
		case T::FeatureDestroyed: {
			const CFeature* feature = ResolveFeature(record.id);

			eventHandler.RenderFeatureDestroyed(feature);
			PopDestroyShell(ShellKey(ObjKind::Feature, false, record.id));

			// PR 27b: drain-window shell resolution (see the unit case)
			if (SimDrawSplit::Enabled())
				boundaryDeadFeatures[record.id] = feature;
		} break;

		case T::ProjectileCreated: {
			eventHandler.RenderProjectileCreated(ResolveProjectile(record.id, record.syncedProj));
		} break;
		case T::ProjectileDestroyed: {
			const CProjectile* proj = ResolveProjectile(record.id, record.syncedProj);

			eventHandler.RenderProjectileDestroyed(proj);
			PopDestroyShell(ShellKey(ObjKind::Projectile, record.syncedProj, record.id));

			// PR 27b: see the UnitDestroyed case (lights can track projectiles)
			if (SimDrawSplit::Enabled()) {
				boundaryDestroyedProjectiles.push_back(proj);

				// PR 43 §7.7: dead-id -> shell map for the producer's
				// DEAD_THIS_BATCH row extraction. SYNCED namespace only (the
				// fd41dbdd92 rule): ProjectileRows holds only synced
				// projectiles, and an unsynced destroy id could otherwise
				// mark a live synced projectile's row dead (id collision).
				if (record.syncedProj)
					boundaryDeadProjectiles[record.id] = proj;
			}
		} break;

		case T::UnitEnteredLos: {
			CUnitDrawer::ApplyUnitEnteredLos(ResolveUnit(record.id), record.arg1, record.arg2 != 0);
		} break;
		case T::UnitLeftLos: {
			CUnitDrawer::ApplyUnitLeftLos(ResolveUnit(record.id), record.arg1, record.arg2 != 0);
		} break;
		case T::UnitEnteredRadar:
		case T::UnitLeftRadar: {
			CUnitDrawer::ApplyUnitRadarChanged(ResolveUnit(record.id), record.arg1);
		} break;
		case T::UnitLeavesGhostChanged: {
			assert(size_t(record.arg1) < masks.size());
			CUnitDrawer::ApplyUnitLeavesGhostChanged(ResolveUnit(record.id), masks[record.arg1]);
		} break;
	}
}


void RenderEventQueue::RenderUnitPreCreated(const CUnit* unit)
{
	Push({ Record::Type::UnitPreCreated, false, unit->id, 0, 0 });
}

void RenderEventQueue::RenderUnitCreated(const CUnit* unit, int cloaked)
{
	Push({ Record::Type::UnitCreated, false, unit->id, cloaked, 0 });
}

void RenderEventQueue::RenderUnitDestroyed(const CUnit* unit)
{
	// PR 13: the deferred-deletion epoch (DeferredObjectDeleter) keeps the
	// object shell readable until after the drain, so destroys queue like
	// every other record; the shell resolves this id until the destroy
	// record dispatches (see the resolution comment in the header)
	if (deferring)
		PushDestroyShell(ShellKey(ObjKind::Unit, false, unit->id), unit);

	Push({ Record::Type::UnitDestroyed, false, unit->id, 0, 0 });
}


void RenderEventQueue::RenderFeaturePreCreated(const CFeature* feature)
{
	Push({ Record::Type::FeaturePreCreated, false, feature->id, 0, 0 });
}

void RenderEventQueue::RenderFeatureCreated(const CFeature* feature)
{
	Push({ Record::Type::FeatureCreated, false, feature->id, 0, 0 });
}

void RenderEventQueue::RenderFeatureDestroyed(const CFeature* feature)
{
	// PR 13: queued, as above
	if (deferring)
		PushDestroyShell(ShellKey(ObjKind::Feature, false, feature->id), feature);

	Push({ Record::Type::FeatureDestroyed, false, feature->id, 0, 0 });
}


void RenderEventQueue::RenderProjectileCreated(const CProjectile* p)
{
	Push({ Record::Type::ProjectileCreated, p->synced, p->id, 0, 0 });
}

void RenderEventQueue::RenderProjectileDestroyed(const CProjectile* p)
{
	// PR 13: queued, as above
	if (deferring)
		PushDestroyShell(ShellKey(ObjKind::Projectile, p->synced, p->id), p);

	Push({ Record::Type::ProjectileDestroyed, p->synced, p->id, 0, 0 });
}


void RenderEventQueue::UnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhost)
{
	Push({ Record::Type::UnitEnteredLos, false, unit->id, allyTeam, leavesGhost });
}

void RenderEventQueue::UnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhost)
{
	Push({ Record::Type::UnitLeftLos, false, unit->id, allyTeam, leavesGhost });
}

void RenderEventQueue::UnitEnteredRadar(const CUnit* unit, int allyTeam)
{
	Push({ Record::Type::UnitEnteredRadar, false, unit->id, allyTeam, 0 });
}

void RenderEventQueue::UnitLeftRadar(const CUnit* unit, int allyTeam)
{
	Push({ Record::Type::UnitLeftRadar, false, unit->id, allyTeam, 0 });
}

void RenderEventQueue::UnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask)
{
	const auto maskIdx = static_cast<int32_t>(ghostMasks.size());

	ghostMasks.push_back(deadGhostAllyMask);
	Push({ Record::Type::UnitLeavesGhostChanged, false, unit->id, maskIdx, 0 });
}
