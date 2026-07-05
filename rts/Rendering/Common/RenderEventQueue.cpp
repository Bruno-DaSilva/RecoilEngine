/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RenderEventQueue.h"

#include <cassert>

#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Features/Feature.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Units/Unit.h"
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

void RenderEventQueue::Dispatch(const Record& record)
{
	using T = Record::Type;

	switch (record.type) {
		case T::UnitPreCreated: {
			eventHandler.RenderUnitPreCreated(static_cast<const CUnit*>(record.obj));
		} break;
		case T::UnitCreated: {
			eventHandler.RenderUnitCreated(static_cast<const CUnit*>(record.obj), record.arg1);
		} break;
		case T::UnitDestroyed: {
			eventHandler.RenderUnitDestroyed(static_cast<const CUnit*>(record.obj));
		} break;

		case T::FeaturePreCreated: {
			eventHandler.RenderFeaturePreCreated(static_cast<const CFeature*>(record.obj));
		} break;
		case T::FeatureCreated: {
			eventHandler.RenderFeatureCreated(static_cast<const CFeature*>(record.obj));
		} break;
		case T::FeatureDestroyed: {
			eventHandler.RenderFeatureDestroyed(static_cast<const CFeature*>(record.obj));
		} break;

		case T::ProjectileCreated: {
			eventHandler.RenderProjectileCreated(static_cast<const CProjectile*>(record.obj));
		} break;
		case T::ProjectileDestroyed: {
			eventHandler.RenderProjectileDestroyed(static_cast<const CProjectile*>(record.obj));
		} break;

		case T::UnitEnteredLos: {
			CUnitDrawer::ApplyUnitEnteredLos(static_cast<const CUnit*>(record.obj), record.arg1, record.arg2 != 0);
		} break;
		case T::UnitLeftLos: {
			CUnitDrawer::ApplyUnitLeftLos(static_cast<const CUnit*>(record.obj), record.arg1, record.arg2 != 0);
		} break;
		case T::UnitEnteredRadar:
		case T::UnitLeftRadar: {
			CUnitDrawer::ApplyUnitRadarChanged(static_cast<const CUnit*>(record.obj), record.arg1);
		} break;
		case T::UnitLeavesGhostChanged: {
			assert(size_t(record.arg1) < ghostMasks.size());
			CUnitDrawer::ApplyUnitLeavesGhostChanged(static_cast<const CUnit*>(record.obj), ghostMasks[record.arg1]);
		} break;
	}
}


void RenderEventQueue::RenderUnitPreCreated(const CUnit* unit)
{
	Push({ Record::Type::UnitPreCreated, false, unit->id, 0, 0, unit });
}

void RenderEventQueue::RenderUnitCreated(const CUnit* unit, int cloaked)
{
	Push({ Record::Type::UnitCreated, false, unit->id, cloaked, 0, unit });
}

void RenderEventQueue::RenderUnitDestroyed(const CUnit* unit)
{
	// PR-13 handoff point: flush + in-place dispatch becomes Push(record)
	// once object lifetime extends past the draw boundary (see class docs)
	Flush();
	Dispatch({ Record::Type::UnitDestroyed, false, unit->id, 0, 0, unit });
}


void RenderEventQueue::RenderFeaturePreCreated(const CFeature* feature)
{
	Push({ Record::Type::FeaturePreCreated, false, feature->id, 0, 0, feature });
}

void RenderEventQueue::RenderFeatureCreated(const CFeature* feature)
{
	Push({ Record::Type::FeatureCreated, false, feature->id, 0, 0, feature });
}

void RenderEventQueue::RenderFeatureDestroyed(const CFeature* feature)
{
	// PR-13 handoff point, as above
	Flush();
	Dispatch({ Record::Type::FeatureDestroyed, false, feature->id, 0, 0, feature });
}


void RenderEventQueue::RenderProjectileCreated(const CProjectile* p)
{
	Push({ Record::Type::ProjectileCreated, p->synced, p->id, 0, 0, p });
}

void RenderEventQueue::RenderProjectileDestroyed(const CProjectile* p)
{
	// PR-13 handoff point, as above
	Flush();
	Dispatch({ Record::Type::ProjectileDestroyed, p->synced, p->id, 0, 0, p });
}


void RenderEventQueue::UnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhost)
{
	Push({ Record::Type::UnitEnteredLos, false, unit->id, allyTeam, leavesGhost, unit });
}

void RenderEventQueue::UnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhost)
{
	Push({ Record::Type::UnitLeftLos, false, unit->id, allyTeam, leavesGhost, unit });
}

void RenderEventQueue::UnitEnteredRadar(const CUnit* unit, int allyTeam)
{
	Push({ Record::Type::UnitEnteredRadar, false, unit->id, allyTeam, 0, unit });
}

void RenderEventQueue::UnitLeftRadar(const CUnit* unit, int allyTeam)
{
	Push({ Record::Type::UnitLeftRadar, false, unit->id, allyTeam, 0, unit });
}

void RenderEventQueue::UnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask)
{
	const auto maskIdx = static_cast<int32_t>(ghostMasks.size());

	ghostMasks.push_back(deadGhostAllyMask);
	Push({ Record::Type::UnitLeavesGhostChanged, false, unit->id, maskIdx, 0, unit });
}
