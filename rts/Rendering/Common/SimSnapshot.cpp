/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimSnapshot.h"

#include "Game/GlobalUnsynced.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/TimeProfiler.h"

SimSnapshot simSnapshot;

void SimSnapshot::Update()
{
	const bool due =
		(front->simFrame != gs->frameNum) ||
		(front->viewAllyTeam != gu->myAllyTeam) ||
		(front->aliveCount != static_cast<int32_t>(unitHandler.GetActiveUnits().size()));

	if (!due)
		return;

	SCOPED_TIMER("Update::SimSnapshot");

	const spring_time t0 = spring_gettime();

	Extract(*back);
	std::swap(front, back);
	generation += 1;

	const float dt = (spring_gettime() - t0).toMilliSecsf();
	sumExtractMs += dt;
	maxExtractMs = std::max(maxExtractMs, dt);
	numExtractions += 1;
	peakAliveCount = std::max(peakAliveCount, front->aliveCount);
}

void SimSnapshot::Clear()
{
	if (numExtractions > 0) {
		const size_t rowBytes =
			sizeof(uint8_t) + sizeof(float3) + sizeof(float4) + 2 * sizeof(float) +
			2 * sizeof(uint8_t) + sizeof(int32_t) + sizeof(float) + sizeof(uint8_t);
		LOG("[SimSnapshot] extractions=%u avgMs=%.4f maxMs=%.4f peakUnits=%d memKB=%.1f",
			numExtractions, sumExtractMs / numExtractions, maxExtractMs, peakAliveCount,
			(2.0f * rowBytes * front->valid.size()) / 1024.0f);
	}

	for (UnitRows& rows : buffers) {
		rows.simFrame = -1;
		rows.viewAllyTeam = -1;
		rows.aliveCount = 0;
	}

	generation = 0;
	sumExtractMs = 0.0f;
	maxExtractMs = 0.0f;
	numExtractions = 0;
	peakAliveCount = 0;
}

void SimSnapshot::Resize(UnitRows& rows, size_t maxUnits)
{
	rows.valid.resize(maxUnits, 0);
	rows.pos.resize(maxUnits);
	rows.speed.resize(maxUnits);
	rows.health.resize(maxUnits);
	rows.maxHealth.resize(maxUnits);
	rows.team.resize(maxUnits);
	rows.allyTeam.resize(maxUnits);
	rows.defID.resize(maxUnits);
	rows.buildProgress.resize(maxUnits);
	rows.losStatus.resize(maxUnits);
}

void SimSnapshot::Extract(UnitRows& rows)
{
	if (rows.valid.size() != unitHandler.MaxUnits())
		Resize(rows, unitHandler.MaxUnits());

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	const int viewAllyTeam = gu->myAllyTeam;
	const auto& activeUnits = unitHandler.GetActiveUnits();

	for (const CUnit* u : activeUnits) {
		const int id = u->id;

		rows.valid[id] = 1;
		rows.pos[id] = u->pos;
		rows.speed[id] = u->speed;
		rows.health[id] = u->health;
		rows.maxHealth[id] = u->maxHealth;
		rows.team[id] = static_cast<uint8_t>(u->team);
		rows.allyTeam[id] = static_cast<uint8_t>(u->allyteam);
		rows.defID[id] = u->unitDef->id;
		rows.buildProgress[id] = u->buildProgress;
		rows.losStatus[id] = u->losStatus[viewAllyTeam];
	}

	rows.simFrame = gs->frameNum;
	rows.viewAllyTeam = viewAllyTeam;
	rows.aliveCount = static_cast<int32_t>(activeUnits.size());
}
