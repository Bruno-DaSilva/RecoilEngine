/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimSnapshot.h"

#include <cstring>

#include "SnapshotHash.h"
#include "ExternalAI/SkirmishAIData.h"     // PR 36: GetTeamLuaAI / GetAIInfo per-team AI block
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Map/MapParser.h"                 // PR 36: GetMapStartPositions capture
#include "Sim/Misc/AllyTeam.h"             // PR 36: GetAllyTeamStartBox / GetAllyTeamInfo
#include "Sim/Misc/GlobalConstants.h"      // PR 36: SQUARE_SIZE / MAX_TEAMS
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
// PR 32 (deep per-unit state): moveType subtypes + CAI/builder/factory derefs
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "Sim/MoveTypes/HoverAirMoveType.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/MoveTypes/StaticMoveType.h"
#include "Sim/MoveTypes/ScriptMoveType.h"
#include "Sim/Misc/GlobalConstants.h" // GAME_SPEED
#include "Sim/Misc/NanoPieceCache.h"
#include "Sim/Units/CommandAI/CommandAI.h"   // repeatOrders
#include "Sim/Units/CommandAI/MobileCAI.h"   // repairBelowHealth
#include "Sim/Units/UnitToolTipMap.hpp"      // GetUnitTooltip custom string
#include "Sim/Units/UnitTypes/Builder.h"     // build-state family
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name
#include "Rendering/Models/3DModelPiece.hpp" // PR 33 S3DModelPiece::name (ppro->omp)
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
// PR 31 (weapon/shield scalar family): live weapon/shield/damages reads
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/BombDropper.h"
#include "Sim/Weapons/WeaponTarget.h"
#include "Sim/Misc/DamageArray.h"
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

// PR 31: flatten a live DynDamageArray into the POD DamagesSnap (see the
// DamagesSnap rationale in SimSnapshot.h). The float vector is assigned in
// place so it reuses capacity across boundaries (numArmorTypes is game-fixed).
// A null source reproduces the live "damages == nullptr" nil shape (valid=0).
static inline void CopyDamages(SimSnapshot::UnitRows::DamagesSnap& dst, const DynDamageArray* src)
{
	if (src == nullptr) {
		dst.valid = 0;
		dst.damages.clear();
		return;
	}

	dst.valid = 1;
	dst.paralyzeDamageTime = src->paralyzeDamageTime;
	dst.impulseFactor = src->impulseFactor;
	dst.impulseBoost = src->impulseBoost;
	dst.craterMult = src->craterMult;
	dst.craterBoost = src->craterBoost;
	dst.dynDamageExp = src->dynDamageExp;
	dst.dynDamageMin = src->dynDamageMin;
	dst.dynDamageRange = src->dynDamageRange;
	dst.dynDamageInverted = src->dynDamageInverted;
	dst.craterAreaOfEffect = src->craterAreaOfEffect;
	dst.damageAreaOfEffect = src->damageAreaOfEffect;
	dst.edgeEffectiveness = src->edgeEffectiveness;
	dst.explosionSpeed = src->explosionSpeed;

	const int n = src->GetNumTypes();
	dst.damages.resize(n);
	for (int i = 0; i < n; ++i)
		dst.damages[i] = src->Get(i);
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

	// PR 36: GetMapStartPositions -- immutable map data, parsed once. Cached
	// here (draw/main thread) rather than inside ExtractTeams, which also runs
	// on the sim thread via HashCompletedFrame; LoadStartPositionsFromMap uses
	// the MapParser and must stay off the sim thread.
	CacheMapStartPositions();

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
			 r.harvestStorage.size() + r.cost.size()) * sizeof(SResourcePack) +
			// PR 32 (deep per-unit state): flat rows + the moveType full-table
			// block + the three LOS-variant strides (variable-size blocks and
			// strings are omitted -- they are near-empty for most units)
			r.storage.size() * sizeof(SResourcePack) +
			r.moveTypeBlock.size() * sizeof(SimSnapshot::MoveTypeBlock) +
			(r.fireState.size() + r.moveState.size() + r.nextPosErrorUpdate.size() +
			 r.lastAttackerID.size() + r.transporterID.size() + r.curBuildID.size()) * sizeof(int32_t) +
			(r.repairBelowHealth.size() + r.metalExtract.size() + r.buildeeRadius.size() +
			 r.buildDistance.size() + r.buildPower.size() + r.mtMaxSpeed.size() +
			 r.mtMaxWantedSpeed.size()) * sizeof(float) +
			(r.posErrorDelta.size() + r.mtGoalPos.size()) * sizeof(float3) +
			r.unitInLosAll.size() + r.unitInAirLosAll.size() + r.unitInJammerAll.size();
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

	// PR 36: force the immutable map-start cache to re-parse for the next game
	mapStartPosCached = false;
	mapStartPos.clear();
	mapStartPosValid.clear();

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

	// PR 31 (weapon/shield family): per-unit rows (flat per-weapon arrays are
	// sized in Extract, where the total live weapon count is known)
	rows.weaponOffset.resize(maxUnits);
	rows.weaponCount.resize(maxUnits);
	rows.reloadSpeed.resize(maxUnits);
	rows.fpsNoFire.resize(maxUnits);
	rows.flankingMode.resize(maxUnits);
	rows.flankingDir.resize(maxUnits);
	rows.flankingMoveFactor.resize(maxUnits);
	rows.flankingAvgDamage.resize(maxUnits);
	rows.flankingDifDamage.resize(maxUnits);
	rows.flankingMobility.resize(maxUnits);
	rows.hasStockpile.resize(maxUnits);
	rows.stockpileNumStockpiled.resize(maxUnits);
	rows.stockpileNumQueued.resize(maxUnits);
	rows.stockpileBuildPercent.resize(maxUnits);
	rows.hasShieldWeapon.resize(maxUnits);
	rows.shieldWeaponEnabled.resize(maxUnits);
	rows.shieldWeaponPower.resize(maxUnits);
	rows.deathExpDamages.resize(maxUnits);
	rows.selfdExpDamages.resize(maxUnits);
	// ---- PR 32 (deep per-unit state) ----
	rows.fireState.resize(maxUnits);
	rows.moveState.resize(maxUnits);
	rows.repairBelowHealth.resize(maxUnits);
	rows.repeatOrders.resize(maxUnits);
	rows.wantCloak.resize(maxUnits);
	rows.useHighTrajectory.resize(maxUnits);
	rows.storage.resize(maxUnits);
	rows.metalExtract.resize(maxUnits);
	rows.buildeeRadius.resize(maxUnits);
	rows.posErrorDelta.resize(maxUnits);
	rows.nextPosErrorUpdate.resize(maxUnits);
	rows.lastAttackerID.resize(maxUnits);
	rows.transporterID.resize(maxUnits);
	rows.builderKind.resize(maxUnits);
	rows.curBuildID.resize(maxUnits);
	rows.buildDistance.resize(maxUnits);
	rows.range3D.resize(maxUnits);
	rows.inBuildStance.resize(maxUnits);
	rows.buildPower.resize(maxUnits);
	rows.customTooltip.resize(maxUnits);
	rows.moveTypeKind.resize(maxUnits);
	rows.mtMaxSpeed.resize(maxUnits);
	rows.mtMaxWantedSpeed.resize(maxUnits);
	rows.mtGoalPos.resize(maxUnits);
	rows.mtProgressState.resize(maxUnits);
	rows.mtAutoLand.resize(maxUnits);
	rows.mtLoopbackAttack.resize(maxUnits);
	rows.moveTypeBlock.resize(maxUnits);
	rows.nanoPieces.resize(maxUnits);
	rows.transportees.resize(maxUnits);
	rows.unitInLosAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.unitInAirLosAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.unitInJammerAll.resize(size_t(numAllyTeams) * maxUnits);
}

// ---- PR 32 (deep per-unit state) extraction helpers ----
// mirror the live dynamic_cast chains; values only, exact accessors so the
// served table/scalars are bit-identical to the live callouts.
static void ExtractUnitBuildState(SimSnapshot::UnitRows& rows, int id, const CUnit* u)
{
	rows.builderKind[id] = 0;
	rows.curBuildID[id] = -1;
	rows.buildDistance[id] = 0.0f;
	rows.range3D[id] = 0;
	rows.inBuildStance[id] = u->inBuildStance;
	rows.buildPower[id] = 0.0f;
	rows.nanoPieces[id].clear();

	if (const CBuilder* builder = dynamic_cast<const CBuilder*>(u); builder != nullptr) {
		rows.builderKind[id] = 1;
		rows.curBuildID[id] = (builder->curBuild != nullptr) ? builder->curBuild->id : -1;
		rows.buildDistance[id] = builder->buildDistance;
		rows.range3D[id] = builder->range3D;
		const NanoPieceCache& npc = builder->GetNanoPieceCache();
		rows.buildPower[id] = npc.GetBuildPower();
		rows.nanoPieces[id] = npc.GetNanoPieces();
		return;
	}
	if (const CFactory* factory = dynamic_cast<const CFactory*>(u); factory != nullptr) {
		rows.builderKind[id] = 2;
		rows.curBuildID[id] = (factory->curBuild != nullptr) ? factory->curBuild->id : -1;
		const NanoPieceCache& npc = factory->GetNanoPieceCache();
		rows.buildPower[id] = npc.GetBuildPower();
		rows.nanoPieces[id] = npc.GetNanoPieces();
		return;
	}
}

static void ExtractUnitMoveType(SimSnapshot::UnitRows& rows, int id, const CUnit* u)
{
	const AMoveType* mt = u->moveType; // never null

	rows.mtMaxSpeed[id] = mt->GetMaxSpeed() * GAME_SPEED;
	rows.mtMaxWantedSpeed[id] = mt->GetMaxWantedSpeed() * GAME_SPEED;
	rows.mtGoalPos[id] = mt->goalPos;
	rows.mtProgressState[id] = static_cast<uint8_t>(mt->progressState); // Done=0/Active=1/Failed=2
	rows.mtAutoLand[id] = 0;
	rows.mtLoopbackAttack[id] = 0;

	SimSnapshot::MoveTypeBlock& b = rows.moveTypeBlock[id];
	b = SimSnapshot::MoveTypeBlock{}; // ids are reused; default-zero for non-dynamic subtypes

	if (const CGroundMoveType* g = dynamic_cast<const CGroundMoveType*>(mt); g != nullptr) {
		rows.moveTypeKind[id] = 1;
		b.turnRate = g->GetTurnRate();
		b.accRate = g->GetAccRate();
		b.decRate = g->GetDecRate();
		b.maxReverseSpeed = g->GetMaxReverseSpeed() * GAME_SPEED;
		b.wantedSpeed = g->GetWantedSpeed() * GAME_SPEED;
		b.currentSpeed = g->GetCurrentSpeed() * GAME_SPEED;
		b.goalRadius = g->GetGoalRadius();
		b.currWayPoint = g->GetCurrWayPoint();
		b.nextWayPoint = g->GetNextWayPoint();
		return;
	}
	if (const CHoverAirMoveType* h = dynamic_cast<const CHoverAirMoveType*>(mt); h != nullptr) {
		rows.moveTypeKind[id] = 2;
		rows.mtAutoLand[id] = h->autoLand;
		b.wantedHeight = h->wantedHeight;
		b.collide = h->collide;
		b.useSmoothMesh = h->useSmoothMesh;
		b.aircraftState = h->aircraftState;
		b.flyState = h->flyState;
		b.goalDistance = h->goalDistance;
		b.bankingAllowed = h->bankingAllowed;
		b.currentBank = h->currentBank;
		b.currentPitch = h->currentPitch;
		b.turnRate = h->turnRate;
		b.accRate = h->accRate;
		b.decRate = h->decRate;
		b.altitudeRate = h->altitudeRate;
		b.dontLand = h->GetAllowLanding(); // pushed under key "dontLand" (== GetAllowLanding())
		b.maxDrift = h->maxDrift;
		return;
	}
	if (const CStrafeAirMoveType* s = dynamic_cast<const CStrafeAirMoveType*>(mt); s != nullptr) {
		rows.moveTypeKind[id] = 3;
		rows.mtAutoLand[id] = s->autoLand;
		rows.mtLoopbackAttack[id] = s->loopbackAttack;
		b.aircraftState = s->aircraftState;
		b.wantedHeight = s->wantedHeight;
		b.collide = s->collide;
		b.useSmoothMesh = s->useSmoothMesh;
		b.myGravity = s->myGravity;
		b.maxBank = s->maxBank;
		b.turnRadius = s->turnRadius;
		b.accRate = s->accRate;
		b.maxAileron = s->maxAileron;
		b.maxElevator = s->maxElevator;
		b.maxRudder = s->maxRudder;
		return;
	}
	if (dynamic_cast<const CStaticMoveType*>(mt) != nullptr) { rows.moveTypeKind[id] = 4; return; }
	if (dynamic_cast<const CScriptMoveType*>(mt) != nullptr) { rows.moveTypeKind[id] = 5; return; }
	rows.moveTypeKind[id] = 0;
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

		// ---- PR 32 (deep per-unit state) ----
		rows.fireState[id] = u->fireState;
		rows.moveState[id] = u->moveState;
		{
			const CMobileCAI* mcai = dynamic_cast<const CMobileCAI*>(u->commandAI);
			rows.repairBelowHealth[id] = (mcai != nullptr) ? mcai->repairBelowHealth : -1.0f;
		}
		rows.repeatOrders[id] = u->commandAI->repeatOrders;
		rows.wantCloak[id] = u->wantCloak;
		rows.useHighTrajectory[id] = u->useHighTrajectory;
		rows.storage[id] = u->storage;
		rows.metalExtract[id] = u->metalExtract;
		rows.buildeeRadius[id] = u->buildeeRadius;
		rows.posErrorDelta[id] = u->posErrorDelta;
		rows.nextPosErrorUpdate[id] = u->nextPosErrorUpdate;
		rows.lastAttackerID[id] = (u->lastAttacker != nullptr) ? u->lastAttacker->id : -1;
		rows.transporterID[id] = (u->GetTransporter() != nullptr) ? u->GetTransporter()->id : -1;
		rows.customTooltip[id] = unitToolTipMap.GetConst(id);
		rows.transportees[id].clear();
		rows.transportees[id].reserve(u->transportedUnits.size());
		for (const CUnit::TransportedUnit& tu : u->transportedUnits)
			rows.transportees[id].push_back(tu.unit->id);
		ExtractUnitBuildState(rows, id, u);
		ExtractUnitMoveType(rows, id, u);

		for (int at = 0; at < numAllyTeams; ++at) {
			rows.losStatusAll[at * maxUnits + id] = u->losStatus[at];
			rows.posErrorBits[at * maxUnits + id] = u->GetPosErrorBit(at);
			rows.inRadarAll[at * maxUnits + id] = losHandler->InRadar(u, at);
			// PR 32 LOS unit variants: store the computed answer (the gates fold
			// cloak/stealth/water/globalLOS logic, like inRadarAll)
			rows.unitInLosAll[at * maxUnits + id] = losHandler->InLos(u, at);
			rows.unitInAirLosAll[at * maxUnits + id] = losHandler->InAirLos(u, at);
			rows.unitInJammerAll[at * maxUnits + id] = losHandler->InJammer(u, at);
		}
	}

	// ================= PR 31: weapon/shield scalar family =================
	// Pass 1: per-unit weapon rows + weaponOffset/weaponCount, accumulating the
	// total live weapon count so the flat per-weapon arrays are sized once.
	int32_t totalWeapons = 0;
	for (const CUnit* u : activeUnits) {
		const int id = u->id;
		const int nw = static_cast<int>(u->weapons.size());

		rows.weaponOffset[id] = totalWeapons;
		rows.weaponCount[id] = nw;
		totalWeapons += nw;

		rows.reloadSpeed[id] = u->reloadSpeed;

		// CanFire's FPS-fire gate captured as one bool (CWeapon::CanFire)
		const CPlayer* fpsPlayer = u->fpsControlPlayer;
		rows.fpsNoFire[id] = (fpsPlayer != nullptr && !fpsPlayer->fpsController.mouse1 && !fpsPlayer->fpsController.mouse2);

		// GetUnitFlanking
		rows.flankingMode[id] = u->flankingBonusMode;
		rows.flankingDir[id] = u->flankingBonusDir;
		rows.flankingMoveFactor[id] = u->flankingBonusMobilityAdd;
		rows.flankingAvgDamage[id] = u->flankingBonusAvgDamage;
		rows.flankingDifDamage[id] = u->flankingBonusDifDamage;
		rows.flankingMobility[id] = u->flankingBonusMobility;

		// GetUnitStockpile (unit->stockpileWeapon; nil shape when null)
		const CWeapon* stockpile = u->stockpileWeapon;
		rows.hasStockpile[id] = (stockpile != nullptr);
		rows.stockpileNumStockpiled[id] = (stockpile != nullptr) ? stockpile->numStockpiled : 0;
		rows.stockpileNumQueued[id] = (stockpile != nullptr) ? stockpile->numStockpileQued : 0;
		rows.stockpileBuildPercent[id] = (stockpile != nullptr) ? stockpile->buildPercent : 0.0f;

		// GetUnitShieldState default case (static_cast in the live path, so a
		// non-null shieldWeapon is a CPlasmaRepulser by construction)
		const CPlasmaRepulser* shield = static_cast<const CPlasmaRepulser*>(u->shieldWeapon);
		rows.hasShieldWeapon[id] = (shield != nullptr);
		rows.shieldWeaponEnabled[id] = (shield != nullptr) ? uint8_t(shield->IsEnabled()) : uint8_t(0);
		rows.shieldWeaponPower[id] = (shield != nullptr) ? shield->GetCurPower() : 0.0f;

		// GetUnitWeaponDamages explosion arrays (unit-level; flattened POD)
		CopyDamages(rows.deathExpDamages[id], u->deathExpDamages);
		CopyDamages(rows.selfdExpDamages[id], u->selfdExpDamages);
	}

	// size the flat per-weapon arrays to the live weapon count (the DamagesSnap
	// float vectors keep their capacity across boundaries)
	{
		const size_t nw = static_cast<size_t>(totalWeapons);
		rows.wAngleGood.resize(nw);
		rows.wReloadStatus.resize(nw);
		rows.wSalvoLeft.resize(nw);
		rows.wNumStockpiled.resize(nw);
		rows.wNextSalvo.resize(nw);
		rows.wReloadTime.resize(nw);
		rows.wReaimTime.resize(nw);
		rows.wAccuracyExp.resize(nw);
		rows.wSprayAngleExp.resize(nw);
		rows.wSalvoError.resize(nw);
		rows.wMoveErrorExp.resize(nw);
		rows.wRange.resize(nw);
		rows.wProjectileSpeed.resize(nw);
		rows.wAutoTargetRangeBoost.resize(nw);
		rows.wSalvoSize.resize(nw);
		rows.wSalvoDelay.resize(nw);
		rows.wSalvoWindup.resize(nw);
		rows.wProjectilesPerShot.resize(nw);
		rows.wAvoidFlags.resize(nw);
		rows.wCollisionFlags.resize(nw);
		rows.wTtl.resize(nw);
		rows.wMuzzlePos.resize(nw);
		rows.wWantedDir.resize(nw);
		rows.wWeaponDir.resize(nw);
		rows.wProjectileType.resize(nw);
		rows.wDefStockpile.resize(nw);
		rows.wDefFireSubmersed.resize(nw);
		rows.wDefMaxFireAngle.resize(nw);
		rows.wIsBombDropper.resize(nw);
		rows.wAimFromPosY.resize(nw);
		rows.wLastRequestedDir.resize(nw);
		rows.wTargetType.resize(nw);
		rows.wTargetIsUser.resize(nw);
		rows.wTargetUnitID.resize(nw);
		rows.wTargetGroundPos.resize(nw);
		rows.wTargetInterceptID.resize(nw);
		rows.wIsShield.resize(nw);
		rows.wShieldEnabled.resize(nw);
		rows.wShieldPower.resize(nw);
		rows.wDamages.resize(nw);
	}

	// Pass 2: flat per-weapon state (computed values -- AccuracyExperience/
	// SprayAngleExperience/SalvoErrorExperience/MoveErrorExperience are stored
	// resolved so the twins push scalars, never reproduce the experience math)
	for (const CUnit* u : activeUnits) {
		const int base = rows.weaponOffset[u->id];
		const auto& weapons = u->weapons;

		for (size_t w = 0; w < weapons.size(); ++w) {
			const CWeapon* weapon = weapons[w];
			const WeaponDef* wdef = weapon->weaponDef;
			const int wi = base + static_cast<int>(w);

			// GetUnitWeaponState
			rows.wAngleGood[wi] = weapon->angleGood;
			rows.wReloadStatus[wi] = weapon->reloadStatus;
			rows.wSalvoLeft[wi] = weapon->salvoLeft;
			rows.wNumStockpiled[wi] = weapon->numStockpiled;
			rows.wNextSalvo[wi] = weapon->nextSalvo;
			rows.wReloadTime[wi] = weapon->reloadTime;
			rows.wReaimTime[wi] = weapon->reaimTime;
			rows.wAccuracyExp[wi] = weapon->AccuracyExperience();
			rows.wSprayAngleExp[wi] = weapon->SprayAngleExperience();
			rows.wSalvoError[wi] = weapon->SalvoErrorExperience();
			rows.wMoveErrorExp[wi] = weapon->MoveErrorExperience();
			rows.wRange[wi] = weapon->range;
			rows.wProjectileSpeed[wi] = weapon->projectileSpeed;
			rows.wAutoTargetRangeBoost[wi] = weapon->autoTargetRangeBoost;
			rows.wSalvoSize[wi] = weapon->salvoSize;
			rows.wSalvoDelay[wi] = weapon->salvoDelay;
			rows.wSalvoWindup[wi] = weapon->salvoWindup;
			rows.wProjectilesPerShot[wi] = weapon->projectilesPerShot;
			rows.wAvoidFlags[wi] = weapon->avoidFlags;
			rows.wCollisionFlags[wi] = weapon->collisionFlags;
			rows.wTtl[wi] = weapon->ttl;

			// GetUnitWeaponVectors (dir switch resolved by the twin from projectileType)
			rows.wMuzzlePos[wi] = weapon->weaponMuzzlePos;
			rows.wWantedDir[wi] = weapon->wantedDir;
			rows.wWeaponDir[wi] = weapon->weaponDir;
			rows.wProjectileType[wi] = static_cast<int32_t>(wdef->projectileType);

			// GetUnitWeaponCanFire inputs (def scalars + runtime state)
			rows.wDefStockpile[wi] = wdef->stockpile;
			rows.wDefFireSubmersed[wi] = wdef->fireSubmersed;
			rows.wDefMaxFireAngle[wi] = wdef->maxFireAngle;
			rows.wIsBombDropper[wi] = (dynamic_cast<const CBombDropper*>(weapon) != nullptr);
			rows.wAimFromPosY[wi] = weapon->aimFromPos.y;
			rows.wLastRequestedDir[wi] = weapon->lastRequestedDir;

			// GetUnitWeaponTarget (SWeaponTarget)
			const SWeaponTarget& tgt = weapon->GetCurrentTarget();
			rows.wTargetType[wi] = static_cast<uint8_t>(tgt.type);
			rows.wTargetIsUser[wi] = tgt.isUserTarget;
			rows.wTargetUnitID[wi] = (tgt.type == Target_Unit && tgt.unit != nullptr) ? tgt.unit->id : 0;
			rows.wTargetGroundPos[wi] = (tgt.type == Target_Pos) ? tgt.groundPos : ZeroVector;
			rows.wTargetInterceptID[wi] = (tgt.type == Target_Intercept && tgt.intercept != nullptr) ? tgt.intercept->id : 0;

			// GetUnitShieldState explicit-weapon case (dynamic_cast in the live path)
			const CPlasmaRepulser* repulser = dynamic_cast<const CPlasmaRepulser*>(weapon);
			rows.wIsShield[wi] = (repulser != nullptr);
			rows.wShieldEnabled[wi] = (repulser != nullptr) ? uint8_t(repulser->IsEnabled()) : uint8_t(0);
			rows.wShieldPower[wi] = (repulser != nullptr) ? repulser->GetCurPower() : 0.0f;

			// GetUnitWeaponDamages per-weapon (flattened POD)
			CopyDamages(rows.wDamages[wi], weapon->damages);
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
		// PR 33 piece-projectile params
		rows.pieceExplFlags.resize(n);
		rows.pieceSpinAngle.resize(n);
		rows.pieceSpinSpeed.resize(n);
		rows.pieceSpinVec.resize(n);
		rows.pieceName.resize(n);
		rows.radius.resize(n); // PR 34 (spatial/list remainder)
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
		rows.radius[id] = p->radius; // PR 34 (spatial/list remainder)
		rows.teamID[id] = static_cast<int32_t>(p->GetTeamID());
		rows.weaponDefID[id] = -1;
		rows.targetType[id] = 0;
		rows.targetID[id] = 0;
		rows.targetPos[id] = ZeroVector;
		rows.ttl[id] = 0;
		rows.intercepted[id] = 0;
		// PR 33 piece-projectile params (default; filled for piece projectiles)
		rows.pieceExplFlags[id] = 0;
		rows.pieceSpinAngle[id] = 0.0f;
		rows.pieceSpinSpeed[id] = 0.0f;
		rows.pieceSpinVec[id] = ZeroVector;
		rows.pieceName[id].clear();

		if (p->piece) {
			// GetPieceProjectileParams/Name serving (all synced state; the
			// piece-projectile ctor passes isSynced=true so these ids resolve
			// via GetProjectileBySyncedID, exactly as the live callouts require)
			const CPieceProjectile* ppro = static_cast<const CPieceProjectile*>(p);
			rows.pieceExplFlags[id] = ppro->explFlags;
			rows.pieceSpinAngle[id] = ppro->spinAngle;
			rows.pieceSpinSpeed[id] = ppro->spinSpeed;
			rows.pieceSpinVec[id] = ppro->spinVec;
			if (ppro->omp != nullptr)
				rows.pieceName[id] = ppro->omp->name;
		}

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
		// ---- PR 36: team-misc per-team vectors (same activeTeams sizing) ----
		rows.startPos.resize(activeTeams);
		rows.hasValidStartPos.resize(activeTeams);
		rows.maxUnits.resize(activeTeams);
		rows.hasLuaAI.resize(activeTeams);
		rows.luaAIName.resize(activeTeams);
		rows.aiHasAI.resize(activeTeams);
		rows.aiID.resize(activeTeams);
		rows.aiName.resize(activeTeams);
		rows.aiHostPlayer.resize(activeTeams);
		rows.aiIsLocal.resize(activeTeams);
		rows.aiShortName.resize(activeTeams);
		rows.aiVersion.resize(activeTeams);
		rows.aiOptions.resize(activeTeams);
		rows.statHistory.resize(activeTeams);
	}

	// PR 36: per-allyteam block (GetAllyTeamStartBox / GetAllyTeamInfo); sized
	// to activeAllyTeams, which differs from activeTeams -> its own resize guard
	const int activeAllyTeams = teamHandler.ActiveAllyTeams();
	if (rows.allyStartBox.size() != static_cast<size_t>(activeAllyTeams)) {
		rows.allyStartBox.resize(activeAllyTeams);
		rows.allyTeamOpts.resize(activeAllyTeams);
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

		// ---- PR 36: team-misc ----
		rows.startPos[t] = team->GetStartPos();
		rows.hasValidStartPos[t] = team->HasValidStartPos();
		rows.maxUnits[t] = static_cast<int32_t>(team->GetMaxUnits());
		// the back() entry is the mutating currentStats, so this is copied every
		// boundary (vector assign reuses capacity; the history is short)
		rows.statHistory[t] = team->statHistory;

		// GetTeamLuaAI: first isLuaAI shortName ("" = none)
		const std::vector<uint8_t>& teamAIs = skirmishAIHandler.GetSkirmishAIsInTeam(t);
		const std::string* luaAIName = nullptr;
		for (uint8_t id: teamAIs) {
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(id);
			if (!aiData->isLuaAI)
				continue;
			luaAIName = &aiData->shortName;
			break;
		}
		rows.hasLuaAI[t] = (luaAIName != nullptr);
		CopyString(rows.luaAIName[t], (luaAIName != nullptr) ? *luaAIName : std::string());

		// GetAIInfo: teamAIs[0] block
		if (teamAIs.empty()) {
			rows.aiHasAI[t] = 0;
			rows.aiID[t] = -1;
			rows.aiHostPlayer[t] = -1;
			rows.aiIsLocal[t] = 0;
			CopyString(rows.aiName[t], std::string());
			CopyString(rows.aiShortName[t], std::string());
			CopyString(rows.aiVersion[t], std::string());
			if (!rows.aiOptions[t].empty())
				rows.aiOptions[t].clear();
		} else {
			const size_t skirmishAIId = teamAIs[0];
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(skirmishAIId);
			rows.aiHasAI[t] = 1;
			rows.aiID[t] = static_cast<int32_t>(skirmishAIId);
			rows.aiHostPlayer[t] = aiData->hostPlayer;
			CopyString(rows.aiName[t], aiData->name);
			rows.aiIsLocal[t] = skirmishAIHandler.IsLocalSkirmishAI(skirmishAIId);
			if (rows.aiIsLocal[t] != 0) {
				CopyString(rows.aiShortName[t], aiData->shortName);
				CopyString(rows.aiVersion[t], aiData->version);
				CopyOpts(rows.aiOptions[t], aiData->options);
			} else {
				CopyString(rows.aiShortName[t], std::string());
				CopyString(rows.aiVersion[t], std::string());
				if (!rows.aiOptions[t].empty())
					rows.aiOptions[t].clear();
			}
		}
	}

	// PR 36: per-allyteam start box (live float order) + custom options
	for (int at = 0; at < activeAllyTeams; ++at) {
		const AllyTeam& ally = teamHandler.GetAllyTeam(at);
		rows.allyStartBox[at] = float4(
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectLeft,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectTop,
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectRight,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectBottom);
		CopyOpts(rows.allyTeamOpts[at], ally.GetAllValues());
	}
}

// PR 36: parse the map-defined start positions once (LoadStartPositionsFromMap
// re-parses the map file on every call, so the live GetMapStartPositions cost is
// paid a single time here). Runs at the boundary with the sim parked (or single-
// threaded), the same context the live callout ran in.
void SimSnapshot::CacheMapStartPositions()
{
	if (mapStartPosCached)
		return;

	mapStartPos.assign(MAX_TEAMS, float3());
	mapStartPosValid.assign(MAX_TEAMS, uint8_t(0));

	if (gameSetup != nullptr) {
		gameSetup->LoadStartPositionsFromMap(MAX_TEAMS, [&](MapParser& mapParser, int teamNum) {
			float3 pos;
			if (!mapParser.GetStartPos(teamNum, pos))
				return false;
			if (teamNum >= 0 && teamNum < MAX_TEAMS) {
				mapStartPos[teamNum] = pos;
				mapStartPosValid[teamNum] = 1;
			}
			return true;
		});
	}

	mapStartPosCached = true;
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
		// ---- PR 36: GetPlayerControlledUnit / GetPlayerStatistics ----
		rows.controlleeID.resize(activePlayers);
		rows.controlleeAllyTeam.resize(activePlayers);
		rows.currentStats.resize(activePlayers);
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

		// ---- PR 36 ----
		// GetPlayerControlledUnit: the FPS-controlled unit's id + allyteam
		const CUnit* controllee = player->fpsController.GetControllee();
		rows.controlleeID[p] = (controllee != nullptr) ? controllee->id : -1;
		rows.controlleeAllyTeam[p] = (controllee != nullptr) ? controllee->allyteam : -1;
		// GetPlayerStatistics: the input/command stat block (POD copy)
		rows.currentStats[p] = player->currentStats;
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
