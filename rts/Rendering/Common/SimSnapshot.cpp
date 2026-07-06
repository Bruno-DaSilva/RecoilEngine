/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimSnapshot.h"

#include <cstring>

#include "SnapshotHash.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/Wind.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/TimeProfiler.h"

SimSnapshot simSnapshot;

// assign-if-different helpers for the team/player boundary copy: the copy is
// re-extracted EVERY boundary (values must be fresh), but the alloc-carrying
// fields (strings, customOpts maps) are almost always unchanged -- comparing
// first turns the steady-state cost into reads only, no allocations
static inline void CopyString(std::string& dst, const char* src)
{
	if (dst != src)
		dst = src;
}

static inline void CopyString(std::string& dst, const std::string& src)
{
	if (dst != src)
		dst = src;
}

static inline void CopyOpts(spring::unordered_map<std::string, std::string>& dst,
                            const spring::unordered_map<std::string, std::string>& src)
{
	const auto equal = [&]() {
		if (dst.size() != src.size())
			return false;
		for (const auto& [key, value] : src) {
			const auto it = dst.find(key);
			if (it == dst.end() || it->second != value)
				return false;
		}
		return true;
	};

	if (!equal())
		dst = src;
}

// GetSolidObjectBlocking's seven pushed booleans as one byte, bit i = push
// slot i (see the UnitRows::blockingBits layout comment)
static inline uint8_t PackBlockingBits(const CSolidObject* o)
{
	return static_cast<uint8_t>(
		(o->HasPhysicalStateBit(CSolidObject::PSTATE_BIT_BLOCKING)       << 0) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_SOLIDOBJECTS) << 1) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_PROJECTILES ) << 2) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS ) << 3) |
		(o->crushable          << 4) |
		(o->blockEnemyPushing  << 5) |
		(o->blockHeightChanges << 6));
}

bool SimSnapshot::UnitRows::PovUnitVisible(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	return ((losStatusAll[readAllyTeam * MaxUnits() + unitID] & (LOS_INLOS | LOS_INRADAR)) != 0);
}

bool SimSnapshot::UnitRows::PovUnitInLos(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	return ((losStatusAll[readAllyTeam * MaxUnits() + unitID] & LOS_INLOS) != 0);
}

bool SimSnapshot::UnitRows::PovUnitTyped(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	// LuaUtils::IsUnitTyped mirror: currently in LOS, or not lost from radar
	// since last being visible
	const uint8_t losStatus = losStatusAll[readAllyTeam * MaxUnits() + unitID];
	constexpr uint8_t prevMask = (LOS_PREVLOS | LOS_CONTRADAR);

	return ((losStatus & LOS_INLOS) != 0 || (losStatus & prevMask) == prevMask);
}

float3 SimSnapshot::UnitRows::ErrorVector(int unitID, int argAllyTeam) const
{
	// bit-for-bit mirror of CUnit::GetErrorVector (Unit.cpp) from extracted
	// inputs; keep the float expression order identical to the live code
	if (argAllyTeam < 0 || argAllyTeam >= numAllyTeams)
		return (posErrorVector[unitID] * baseRadarErrorSize * 2.0f);

	const int atErrorMask = (posErrorBits[argAllyTeam * MaxUnits() + unitID] != 0);
	const int atSightMask = losStatusAll[argAllyTeam * MaxUnits() + unitID];

	const int isVisible = 2 * ((atSightMask & LOS_INLOS  ) != 0 || Allied(argAllyTeam, allyTeam[unitID])); // in LOS or allied, no error
	const int seenGhost = 4 * ((atSightMask & LOS_PREVLOS) != 0 && leavesGhost[unitID] != 0);              // seen ghosted immobiles, no error
	const int isOnRadar = 8 * ((atSightMask & LOS_INRADAR) != 0                                         ); // current radar contact

	float errorMult = 0.0f;

	switch (isVisible | seenGhost | isOnRadar) {
		case  0: { errorMult = baseRadarErrorSize * 2.0f        ; } break; //  !isVisible && !seenGhost  && !isOnRadar
		case  8: { errorMult = radarErrorSizes[argAllyTeam]     ; } break; //  !isVisible && !seenGhost  &&  isOnRadar
		default: {                                                } break; // ( isVisible ||  seenGhost) && !isOnRadar
	}

	return (posErrorVector[unitID] * errorMult * (atErrorMask != 0));
}

bool SimSnapshot::FeatureRows::IsInLosForAllyTeam(int id, int argAllyTeam) const
{
	// bit-for-bit mirror of CFeature::IsInLosForAllyTeam from extracted inputs
	if (alwaysVisible[id] != 0 || argAllyTeam == -1)
		return true;

	const bool isGaia = (allyTeam[id] == gaiaAllyTeam);

	switch (featureVisibility) {
		case CModInfo::FEATURELOS_NONE:
		default:
			return InLos(id, argAllyTeam);
		case CModInfo::FEATURELOS_GAIAONLY:
			return (isGaia || InLos(id, argAllyTeam));
		case CModInfo::FEATURELOS_GAIAALLIED:
			return (isGaia || allyTeam[id] == argAllyTeam || InLos(id, argAllyTeam));
		case CModInfo::FEATURELOS_ALL:
			return true;
	}
}

void SimSnapshot::Update()
{
	SCOPED_TIMER("Update::SimSnapshot");

	// team/player boundary copy (PR 26, section E.3): re-extracted EVERY
	// boundary, no due check -- net messages mutate these tables between sim
	// frames (share/resign transfers, NETMSG_PLAYERINFO ping/cpu at net rate),
	// invisibly to the frameNum/aliveCount checks below, and the copy is KBs
	ExtractTeams(*teamBack);
	ExtractPlayers(*playerBack);
	// global-scalar copy (PR 27a): same unconditional lifecycle -- speed/
	// pause/cheat state mutates via net between frames, and it is ~100 bytes
	ExtractGlobals(*globBack);
	std::swap(teamFront, teamBack);
	std::swap(playerFront, playerBack);
	std::swap(globFront, globBack);

	const bool due =
		mutatedOutsideFrame ||
		(front->simFrame != gs->frameNum) ||
		(front->aliveCount != static_cast<int32_t>(unitHandler.GetActiveUnits().size()));

	if (!due)
		return;

	mutatedOutsideFrame = false;

	const spring_time t0 = spring_gettime();

	Extract(*back);
	ExtractProjectiles(*projBack);
	ExtractFeatures(*featBack);
	std::swap(front, back);
	std::swap(projFront, projBack);
	std::swap(featFront, featBack);
	generation += 1;

	const float dt = (spring_gettime() - t0).toMilliSecsf();
	sumExtractMs += dt;
	maxExtractMs = std::max(maxExtractMs, dt);
	numExtractions += 1;
	peakAliveCount = std::max(peakAliveCount, front->aliveCount);
}

void SimSnapshot::HashCompletedFrame(int frameNum)
{
	if (!SnapshotHash::Armed())
		return;

	// Extract a private copy of the same rows the published buffer holds, but
	// from the just-completed sim frame's live state (Extract stamps
	// gs->frameNum, which equals frameNum here). front/back and generation are
	// untouched, so nothing draw-side observes this. Player rows are not
	// hashed at all (net-layer state, see the PlayerRows comment), so no
	// player scratch exists.
	Extract(hashScratch);
	ExtractProjectiles(hashProjScratch);
	ExtractFeatures(hashFeatScratch);
	ExtractTeams(hashTeamScratch);
	SnapshotHash::HashFrame(frameNum, hashScratch, hashProjScratch, hashFeatScratch, hashTeamScratch);
}

void SimSnapshot::Clear()
{
	if (numExtractions > 0) {
		const auto& r = *front;
		const size_t bufBytes =
			r.radarErrorSizes.size() * sizeof(float) + r.allied.size() +
			r.valid.size() + r.team.size() + r.allyTeam.size() +
			r.beingBuilt.size() + r.stunned.size() + r.leavesGhost.size() +
			r.losStatusAll.size() + r.posErrorBits.size() + r.inRadarAll.size() +
			(r.pos.size() + r.midPos.size() + r.aimPos.size() + r.relMidPos.size() +
			 r.frontdir.size() + r.updir.size() + r.rightdir.size() +
			 r.posErrorVector.size()) * sizeof(float3) +
			r.speed.size() * sizeof(float4) +
			(r.health.size() + r.maxHealth.size() + r.paralyzeDamage.size() +
			 r.captureProgress.size() + r.buildProgress.size() + r.radius.size()) * sizeof(float) +
			r.defID.size() * sizeof(int32_t) +
			r.noSelect.size() + r.inVoid.size() + r.selVol.size() * sizeof(CollisionVolume) +
			// PR 27a rows
			r.isDead.size() + r.neutral.size() + r.activated.size() +
			r.isCloaked.size() + r.armoredState.size() + r.blockingBits.size() +
			(r.heading.size() + r.buildFacing.size()) * sizeof(int16_t) +
			(r.armoredMultiple.size() + r.height.size() + r.mass.size() +
			 r.maxRange.size() + r.seismicSignature.size() + r.experience.size() +
			 r.limExperience.size() + r.buildTime.size()) * sizeof(float) +
			(r.selfDCountdown.size() + r.losRadius.size() + r.airLosRadius.size() +
			 r.radarRadius.size() + r.sonarRadius.size() + r.seismicRadius.size() +
			 r.jammerRadius.size() + r.sonarJamRadius.size() + r.moveDefID.size()) * sizeof(int32_t) +
			(r.resourcesMake.size() + r.resourcesUse.size() + r.harvested.size() +
			 r.harvestStorage.size() + r.cost.size()) * sizeof(SResourcePack);
		LOG("[SimSnapshot] extractions=%u avgMs=%.4f maxMs=%.4f peakUnits=%d memKB=%.1f projSlots=%d featSlots=%d",
			numExtractions, sumExtractMs / numExtractions, maxExtractMs, peakAliveCount,
			(2.0f * bufBytes) / 1024.0f, int(projFront->MaxSlots()), int(featFront->MaxSlots()));
	}

	for (UnitRows& rows : buffers) {
		rows.simFrame = -1;
		rows.aliveCount = 0;
	}
	for (ProjectileRows& rows : projBuffers)
		std::fill(rows.valid.begin(), rows.valid.end(), 0);
	for (FeatureRows& rows : featBuffers)
		std::fill(rows.valid.begin(), rows.valid.end(), 0);
	for (TeamRows& rows : teamBuffers)
		rows.activeTeams = 0;
	for (PlayerRows& rows : playerBuffers)
		rows.activePlayers = 0;

	generation = 0;
	mutatedOutsideFrame = false;
	sumExtractMs = 0.0f;
	maxExtractMs = 0.0f;
	numExtractions = 0;
	peakAliveCount = 0;
}

void SimSnapshot::Resize(UnitRows& rows, size_t maxUnits, int numAllyTeams)
{
	rows.numAllyTeams = numAllyTeams;
	rows.radarErrorSizes.resize(numAllyTeams);
	rows.allied.resize(size_t(numAllyTeams) * numAllyTeams);

	rows.valid.resize(maxUnits, 0);
	rows.pos.resize(maxUnits);
	rows.midPos.resize(maxUnits);
	rows.aimPos.resize(maxUnits);
	rows.speed.resize(maxUnits);
	rows.health.resize(maxUnits);
	rows.maxHealth.resize(maxUnits);
	rows.paralyzeDamage.resize(maxUnits);
	rows.captureProgress.resize(maxUnits);
	rows.team.resize(maxUnits);
	rows.allyTeam.resize(maxUnits);
	rows.defID.resize(maxUnits);
	rows.buildProgress.resize(maxUnits);
	rows.beingBuilt.resize(maxUnits);
	rows.stunned.resize(maxUnits);
	rows.radius.resize(maxUnits);
	rows.selVol.resize(maxUnits);
	rows.noSelect.resize(maxUnits);
	rows.inVoid.resize(maxUnits);
	rows.isDead.resize(maxUnits);
	rows.neutral.resize(maxUnits);
	rows.activated.resize(maxUnits);
	rows.isCloaked.resize(maxUnits);
	rows.armoredState.resize(maxUnits);
	rows.armoredMultiple.resize(maxUnits);
	rows.heading.resize(maxUnits);
	rows.buildFacing.resize(maxUnits);
	rows.height.resize(maxUnits);
	rows.mass.resize(maxUnits);
	rows.maxRange.resize(maxUnits);
	rows.seismicSignature.resize(maxUnits);
	rows.experience.resize(maxUnits);
	rows.limExperience.resize(maxUnits);
	rows.selfDCountdown.resize(maxUnits);
	rows.losRadius.resize(maxUnits);
	rows.airLosRadius.resize(maxUnits);
	rows.radarRadius.resize(maxUnits);
	rows.sonarRadius.resize(maxUnits);
	rows.seismicRadius.resize(maxUnits);
	rows.jammerRadius.resize(maxUnits);
	rows.sonarJamRadius.resize(maxUnits);
	rows.moveDefID.resize(maxUnits);
	rows.resourcesMake.resize(maxUnits);
	rows.resourcesUse.resize(maxUnits);
	rows.harvested.resize(maxUnits);
	rows.harvestStorage.resize(maxUnits);
	rows.cost.resize(maxUnits);
	rows.buildTime.resize(maxUnits);
	rows.blockingBits.resize(maxUnits);
	rows.relMidPos.resize(maxUnits);
	rows.frontdir.resize(maxUnits);
	rows.updir.resize(maxUnits);
	rows.rightdir.resize(maxUnits);
	rows.posErrorVector.resize(maxUnits);
	rows.leavesGhost.resize(maxUnits);
	rows.losStatusAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.posErrorBits.resize(size_t(numAllyTeams) * maxUnits);
	rows.inRadarAll.resize(size_t(numAllyTeams) * maxUnits);
}

void SimSnapshot::Extract(UnitRows& rows)
{
	const size_t maxUnits = unitHandler.MaxUnits();
	const int numAllyTeams = teamHandler.ActiveAllyTeams();

	if (rows.valid.size() != maxUnits || rows.numAllyTeams != numAllyTeams)
		Resize(rows, maxUnits, numAllyTeams);

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	// global block
	rows.baseRadarErrorSize = losHandler->GetBaseRadarErrorSize();
	for (int at = 0; at < numAllyTeams; ++at)
		rows.radarErrorSizes[at] = losHandler->GetAllyTeamRadarErrorSize(at);
	for (int a = 0; a < numAllyTeams; ++a)
		for (int b = 0; b < numAllyTeams; ++b)
			rows.allied[a * numAllyTeams + b] = teamHandler.Ally(a, b);

	const auto& activeUnits = unitHandler.GetActiveUnits();

	for (const CUnit* u : activeUnits) {
		const int id = u->id;

		rows.valid[id] = 1;
		rows.pos[id] = u->pos;
		rows.midPos[id] = u->midPos;
		rows.aimPos[id] = u->aimPos;
		rows.speed[id] = u->speed;
		rows.health[id] = u->health;
		rows.maxHealth[id] = u->maxHealth;
		rows.paralyzeDamage[id] = u->paralyzeDamage;
		rows.captureProgress[id] = u->captureProgress;
		rows.team[id] = static_cast<uint8_t>(u->team);
		rows.allyTeam[id] = static_cast<uint8_t>(u->allyteam);
		rows.defID[id] = u->unitDef->id;
		rows.buildProgress[id] = u->buildProgress;
		rows.beingBuilt[id] = u->beingBuilt;
		rows.stunned[id] = u->IsStunned();
		rows.radius[id] = u->radius;
		rows.selVol[id] = u->selectionVolume;
		rows.noSelect[id] = u->noSelect;
		rows.inVoid[id] = u->IsInVoid();
		rows.isDead[id] = u->isDead;
		rows.neutral[id] = u->neutral;
		rows.activated[id] = u->activated;
		rows.isCloaked[id] = u->isCloaked;
		rows.armoredState[id] = u->armoredState;
		rows.armoredMultiple[id] = u->armoredMultiple;
		rows.heading[id] = u->heading;
		rows.buildFacing[id] = u->buildFacing;
		rows.height[id] = u->height;
		rows.mass[id] = u->mass;
		rows.maxRange[id] = u->maxRange;
		rows.seismicSignature[id] = u->seismicSignature;
		rows.experience[id] = u->experience;
		rows.limExperience[id] = u->limExperience;
		rows.selfDCountdown[id] = u->selfDCountdown;
		rows.losRadius[id] = u->losRadius;
		rows.airLosRadius[id] = u->airLosRadius;
		rows.radarRadius[id] = u->radarRadius;
		rows.sonarRadius[id] = u->sonarRadius;
		rows.seismicRadius[id] = u->seismicRadius;
		rows.jammerRadius[id] = u->jammerRadius;
		rows.sonarJamRadius[id] = u->sonarJamRadius;
		rows.moveDefID[id] = (u->moveDef != nullptr) ? static_cast<int32_t>(u->moveDef->pathType) : -1;
		rows.resourcesMake[id] = u->resourcesMake;
		rows.resourcesUse[id] = u->resourcesUse;
		rows.harvested[id] = u->harvested;
		rows.harvestStorage[id] = u->harvestStorage;
		rows.cost[id] = u->cost;
		rows.buildTime[id] = u->buildTime;
		rows.blockingBits[id] = PackBlockingBits(u);
		rows.relMidPos[id] = u->relMidPos;
		rows.frontdir[id] = u->frontdir;
		rows.updir[id] = u->updir;
		rows.rightdir[id] = u->rightdir;
		rows.posErrorVector[id] = u->posErrorVector;
		rows.leavesGhost[id] = u->leavesGhost;

		for (int at = 0; at < numAllyTeams; ++at) {
			rows.losStatusAll[at * maxUnits + id] = u->losStatus[at];
			rows.posErrorBits[at * maxUnits + id] = u->GetPosErrorBit(at);
			rows.inRadarAll[at * maxUnits + id] = losHandler->InRadar(u, at);
		}
	}

	rows.simFrame = gs->frameNum;
	rows.aliveCount = static_cast<int32_t>(activeUnits.size());
}

void SimSnapshot::ExtractProjectiles(ProjectileRows& rows)
{
	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	const auto& pc = projectileHandler.GetActiveProjectiles(true);

	// synced projectile ids are free-list ints; rows grow-only to the max id
	// seen so slot indices stay stable across extractions
	int maxID = -1;
	for (size_t i = 0; i < pc.size(); ++i)
		maxID = std::max(maxID, pc[i]->id);

	const size_t wantSlots = static_cast<size_t>(maxID + 1);

	if (rows.valid.size() < wantSlots || rows.numAllyTeams != numAllyTeams) {
		const size_t n = std::max(wantSlots, rows.valid.size());
		rows.numAllyTeams = numAllyTeams;
		rows.valid.resize(n, 0);
		rows.pos.resize(n);
		rows.speed.resize(n);
		rows.allyTeam.resize(n);
		rows.ownerID.resize(n);
		rows.isWeapon.resize(n);
		rows.weaponDefID.resize(n);
		rows.targetType.resize(n);
		rows.targetID.resize(n);
		rows.targetPos.resize(n);
		rows.isPiece.resize(n);
		rows.dir.resize(n);
		rows.mygravity.resize(n);
		rows.teamID.resize(n);
		rows.ttl.resize(n);
		rows.intercepted.resize(n);
		rows.inLosAll.resize(size_t(numAllyTeams) * n);
	}

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	const size_t slots = rows.MaxSlots();

	for (size_t i = 0; i < pc.size(); ++i) {
		const CProjectile* p = pc[i];
		const int id = p->id;

		rows.valid[id] = 1;
		rows.pos[id] = p->pos;
		rows.speed[id] = p->speed;
		rows.allyTeam[id] = p->GetAllyteamID();
		rows.ownerID[id] = p->GetOwnerID();
		rows.isWeapon[id] = p->weapon;
		rows.isPiece[id] = p->piece;
		rows.dir[id] = p->dir;
		rows.mygravity[id] = p->mygravity;
		rows.teamID[id] = static_cast<int32_t>(p->GetTeamID());
		rows.weaponDefID[id] = -1;
		rows.targetType[id] = 0;
		rows.targetID[id] = 0;
		rows.targetPos[id] = ZeroVector;
		rows.ttl[id] = 0;
		rows.intercepted[id] = 0;

		if (p->weapon) {
			const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
			const WeaponDef* wdef = wpro->GetWeaponDef();
			const CWorldObject* wtgt = wpro->GetTargetObject();

			rows.weaponDefID[id] = (wdef != nullptr) ? wdef->id : -1;
			rows.ttl[id] = wpro->GetTimeToLive();
			rows.intercepted[id] = wpro->IsBeingIntercepted();

			// same type resolution as LuaSyncedRead::GetProjectileTarget
			if (wtgt == nullptr) {
				rows.targetType[id] = 'g';
				rows.targetPos[id] = wpro->GetTargetPos();
			} else if (dynamic_cast<const CUnit*>(wtgt) != nullptr) {
				rows.targetType[id] = 'u';
				rows.targetID[id] = wtgt->id;
			} else if (dynamic_cast<const CFeature*>(wtgt) != nullptr) {
				rows.targetType[id] = 'f';
				rows.targetID[id] = wtgt->id;
			} else if (dynamic_cast<const CWeaponProjectile*>(wtgt) != nullptr) {
				rows.targetType[id] = 'p';
				rows.targetID[id] = wtgt->id;
			}
		}

		for (int at = 0; at < numAllyTeams; ++at)
			rows.inLosAll[at * slots + id] = losHandler->InLos(p->pos, at);
	}
}

void SimSnapshot::ExtractFeatures(FeatureRows& rows)
{
	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	const auto& activeIDs = featureHandler.GetActiveFeatureIDs();

	// feature ids are dense but sparse-occupancy; rows grow-only to the max id
	// seen so slot indices stay stable across extractions (projectile pattern)
	int maxID = -1;
	for (const int id : activeIDs)
		maxID = std::max(maxID, id);

	const size_t wantSlots = static_cast<size_t>(maxID + 1);

	if (rows.valid.size() < wantSlots || rows.numAllyTeams != numAllyTeams) {
		const size_t n = std::max(wantSlots, rows.valid.size());
		rows.numAllyTeams = numAllyTeams;
		rows.valid.resize(n, 0);
		rows.pos.resize(n);
		rows.midPos.resize(n);
		rows.aimPos.resize(n);
		rows.relMidPos.resize(n);
		rows.radius.resize(n);
		rows.allyTeam.resize(n);
		rows.defID.resize(n);
		rows.alwaysVisible.resize(n);
		rows.noSelect.resize(n);
		rows.inVoid.resize(n);
		rows.selVol.resize(n);
		rows.team.resize(n);
		rows.health.resize(n);
		rows.resurrectProgress.resize(n);
		rows.height.resize(n);
		rows.mass.resize(n);
		rows.speed.resize(n);
		rows.matXdir.resize(n);
		rows.matYdir.resize(n);
		rows.matZdir.resize(n);
		rows.heading.resize(n);
		rows.buildFacing.resize(n);
		rows.resources.resize(n);
		rows.defResources.resize(n);
		rows.reclaimLeft.resize(n);
		rows.reclaimTime.resize(n);
		rows.blockingBits.resize(n);
		rows.resurrectDefID.resize(n);
		rows.inLosAll.resize(size_t(numAllyTeams) * n);
	}

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	rows.featureVisibility = modInfo.featureVisibility;
	rows.gaiaAllyTeam = std::max(0, teamHandler.GaiaAllyTeamID());

	const size_t slots = rows.MaxSlots();

	for (const int id : activeIDs) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr)
			continue;

		rows.valid[id] = 1;
		rows.pos[id] = f->pos;
		rows.midPos[id] = f->midPos;
		rows.aimPos[id] = f->aimPos;
		rows.relMidPos[id] = f->relMidPos;
		rows.radius[id] = f->radius;
		rows.allyTeam[id] = f->allyteam;
		rows.defID[id] = f->def->id;
		rows.alwaysVisible[id] = f->alwaysVisible;
		rows.noSelect[id] = f->noSelect;
		rows.inVoid[id] = f->IsInVoid();
		rows.selVol[id] = f->selectionVolume;
		rows.team[id] = f->team;
		rows.health[id] = f->health;
		rows.resurrectProgress[id] = f->resurrectProgress;
		rows.height[id] = f->height;
		rows.mass[id] = f->mass;
		rows.speed[id] = f->speed;
		{
			const CMatrix44f& fm = f->GetTransformMatrixRef();
			rows.matXdir[id] = fm.GetX();
			rows.matYdir[id] = fm.GetY();
			rows.matZdir[id] = fm.GetZ();
		}
		rows.heading[id] = f->heading;
		rows.buildFacing[id] = f->buildFacing;
		rows.resources[id] = f->resources;
		rows.defResources[id] = f->defResources;
		rows.reclaimLeft[id] = f->reclaimLeft;
		rows.reclaimTime[id] = f->reclaimTime;
		rows.blockingBits[id] = PackBlockingBits(f);
		rows.resurrectDefID[id] = (f->udef != nullptr) ? f->udef->id : -1;

		for (int at = 0; at < numAllyTeams; ++at)
			rows.inLosAll[at * slots + id] = losHandler->InLos(f->pos, at);
	}
}

void SimSnapshot::ExtractTeams(TeamRows& rows)
{
	const int activeTeams = teamHandler.ActiveTeams();

	// global block
	rows.activeTeams = activeTeams;
	rows.activeAllyTeams = teamHandler.ActiveAllyTeams();
	rows.gaiaTeamID = teamHandler.GaiaTeamID();
	rows.useLuaGaia = gs->useLuaGaia;
	rows.gameOver = (game != nullptr && game->IsGameOver());

	if (rows.leader.size() != static_cast<size_t>(activeTeams)) {
		rows.leader.resize(activeTeams);
		rows.isDead.resize(activeTeams);
		rows.hasAIs.resize(activeTeams);
		rows.allyTeam.resize(activeTeams);
		rows.incomeMultiplier.resize(activeTeams);
		rows.numUnits.resize(activeTeams);
		rows.color.resize(activeTeams);
		rows.origColor.resize(activeTeams);
		rows.sideName.resize(activeTeams);
		rows.currentStats.resize(activeTeams);
		rows.res.resize(activeTeams);
		rows.resStorage.resize(activeTeams);
		rows.resPrevPull.resize(activeTeams);
		rows.resPrevIncome.resize(activeTeams);
		rows.resPrevExpense.resize(activeTeams);
		rows.resShare.resize(activeTeams);
		rows.resPrevSent.resize(activeTeams);
		rows.resPrevReceived.resize(activeTeams);
		rows.resPrevExcess.resize(activeTeams);
		rows.customOpts.resize(activeTeams);
	}

	for (int t = 0; t < activeTeams; ++t) {
		const CTeam* team = teamHandler.Team(t);

		rows.leader[t] = team->GetLeader();
		rows.isDead[t] = team->isDead;
		rows.hasAIs[t] = skirmishAIHandler.HasSkirmishAIsInTeam(t);
		rows.allyTeam[t] = teamHandler.AllyTeam(t);
		rows.incomeMultiplier[t] = team->GetIncomeMultiplier();
		rows.numUnits[t] = static_cast<int32_t>(unitHandler.NumUnitsByTeam(t));
		std::memcpy(rows.color[t].data(), team->color, 4);
		std::memcpy(rows.origColor[t].data(), team->origColor, 4);
		CopyString(rows.sideName[t], team->GetSideName());
		rows.currentStats[t] = team->GetCurrentStats();
		rows.res[t] = team->res;
		rows.resStorage[t] = team->resStorage;
		rows.resPrevPull[t] = team->resPrevPull;
		rows.resPrevIncome[t] = team->resPrevIncome;
		rows.resPrevExpense[t] = team->resPrevExpense;
		rows.resShare[t] = team->resShare;
		rows.resPrevSent[t] = team->resPrevSent;
		rows.resPrevReceived[t] = team->resPrevReceived;
		rows.resPrevExcess[t] = team->resPrevExcess;
		CopyOpts(rows.customOpts[t], team->GetAllValues());
	}
}

void SimSnapshot::ExtractPlayers(PlayerRows& rows)
{
	const int activePlayers = static_cast<int>(playerHandler.ActivePlayers());

	rows.activePlayers = activePlayers;
	rows.hostDemo = gameSetup->hostDemo;

	if (rows.name.size() != static_cast<size_t>(activePlayers)) {
		rows.name.resize(activePlayers);
		rows.countryCode.resize(activePlayers);
		rows.team.resize(activePlayers);
		rows.rank.resize(activePlayers);
		rows.ping.resize(activePlayers);
		rows.cpuUsage.resize(activePlayers);
		rows.active.resize(activePlayers);
		rows.spectator.resize(activePlayers);
		rows.isFromDemo.resize(activePlayers);
		rows.desynced.resize(activePlayers);
		rows.customOpts.resize(activePlayers);
	}

	for (int p = 0; p < activePlayers; ++p) {
		const CPlayer* player = playerHandler.Player(p);

		CopyString(rows.name[p], player->name);
		CopyString(rows.countryCode[p], player->countryCode);
		rows.team[p] = player->team;
		rows.rank[p] = player->rank;
		rows.ping[p] = player->ping;
		rows.cpuUsage[p] = player->cpuUsage;
		rows.active[p] = player->active;
		rows.spectator[p] = player->spectator;
		rows.isFromDemo[p] = player->isFromDemo;
		rows.desynced[p] = player->desynced;
		CopyOpts(rows.customOpts[p], player->GetAllValues());
	}
}

void SimSnapshot::ExtractGlobals(GlobalRows& rows)
{
	rows.luaSimFrame = gs->GetLuaSimFrame();

	rows.wantedSpeedFactor = gs->wantedSpeedFactor;
	rows.speedFactor = gs->speedFactor;
	rows.paused = gs->paused;

	rows.cheatEnabled = gs->cheatEnabled;
	rows.godMode = gs->godMode;
	rows.editDefsEnabled = gs->editDefsEnabled;
	rows.noHelperAIs = gs->noHelperAIs;
	rows.defsNoCost = (unitDefHandler != nullptr && unitDefHandler->GetNoCost());

	rows.doneLoading = (game != nullptr && game->IsDoneLoading());
	rows.savedGame = (game != nullptr && game->IsSavedGame());
	rows.clientPaused = (game != nullptr && game->IsClientPaused());

	rows.windVec = envResHandler.GetCurrentWindVec();
	rows.windDir = envResHandler.GetCurrentWindDir();
	rows.windStrength = envResHandler.GetCurrentWindStrength();

	rows.initMinHeight = readMap->GetInitMinHeight();
	rows.initMaxHeight = readMap->GetInitMaxHeight();
	rows.currMinHeight = readMap->GetCurrMinHeight();
	rows.currMaxHeight = readMap->GetCurrMaxHeight();

	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	rows.numAllyTeams = numAllyTeams;
	rows.globalLos.resize(numAllyTeams);
	for (int at = 0; at < numAllyTeams; ++at) {
		rows.globalLos[at] = losHandler->GetGlobalLOS(at);
	}
}
