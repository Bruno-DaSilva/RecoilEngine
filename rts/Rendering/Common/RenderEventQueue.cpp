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

RenderEventQueue renderEventQueue;


void RenderEventQueue::Push(const Record& record)
{
	if (!deferring) {
		// immediate mode is only ever active with an empty queue (Drain is
		// what ends the sim phase), so dispatching in place preserves order
		assert(records.empty());
		Dispatch(record);
		ghostMasks.clear();
		return;
	}

	records.push_back(record);
}

void RenderEventQueue::Flush()
{
	if (records.empty())
		return;

	assert(!draining);
	draining = true;

	// index loop by value: dispatched handlers do not fire further queued
	// events today, but stay safe against appends invalidating iterators
	const size_t numRecords = records.size();
	for (size_t i = 0; i < numRecords; ++i) {
		const Record record = records[i];
		Dispatch(record);
	}

	records.erase(records.begin(), records.begin() + numRecords);

	if (records.empty())
		ghostMasks.clear();

	draining = false;
}


void RenderEventQueue::PushDestroyShell(uint64_t key, const void* obj)
{
	pendingDestroyShells[key].push_back(obj);
}

const void* RenderEventQueue::FindDestroyShell(uint64_t key) const
{
	const auto it = pendingDestroyShells.find(key);

	if (it == pendingDestroyShells.end())
		return nullptr;

	assert(!it->second.empty());
	return it->second.front();
}

const void* RenderEventQueue::PopDestroyShell(uint64_t key)
{
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

	assert(proj != nullptr);
	return proj;
}


void RenderEventQueue::Dispatch(const Record& record)
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
			eventHandler.RenderUnitDestroyed(ResolveUnit(record.id));
			PopDestroyShell(ShellKey(ObjKind::Unit, false, record.id));
		} break;

		case T::FeaturePreCreated: {
			eventHandler.RenderFeaturePreCreated(ResolveFeature(record.id));
		} break;
		case T::FeatureCreated: {
			eventHandler.RenderFeatureCreated(ResolveFeature(record.id));
		} break;
		case T::FeatureDestroyed: {
			eventHandler.RenderFeatureDestroyed(ResolveFeature(record.id));
			PopDestroyShell(ShellKey(ObjKind::Feature, false, record.id));
		} break;

		case T::ProjectileCreated: {
			eventHandler.RenderProjectileCreated(ResolveProjectile(record.id, record.syncedProj));
		} break;
		case T::ProjectileDestroyed: {
			eventHandler.RenderProjectileDestroyed(ResolveProjectile(record.id, record.syncedProj));
			PopDestroyShell(ShellKey(ObjKind::Projectile, record.syncedProj, record.id));
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
			assert(size_t(record.arg1) < ghostMasks.size());
			CUnitDrawer::ApplyUnitLeavesGhostChanged(ResolveUnit(record.id), ghostMasks[record.arg1]);
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
