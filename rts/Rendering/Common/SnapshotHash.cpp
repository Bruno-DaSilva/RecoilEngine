/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotHash.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "System/Log/ILog.h"
#include "lib/xxhash/xxh3.h"

namespace SnapshotHash {

// Fixed seeds define the hash and must never change (would break cross-run/
// cross-build comparability of dumps). 64-bit XXH3 keeps root/bucket rollup
// collisions negligible over a full 40k-frame game.
static constexpr uint64_t UNIT_SEED   = 0x53696d55'6e697448ull; // "SimUnitH"
static constexpr uint64_t BUCKET_SEED = 0x42756b65'74483136ull; // "BuketH16"
static constexpr uint64_t ROOT_SEED   = 0x526f6f74'48313600ull; // "RootH16"

// number of dense unit ids folded into one bucket. Powers of two keep id/bucket
// cheap; 1024 gives ~a few dozen buckets at BAR's late-game unit counts.
static constexpr uint32_t BUCKET_SIZE = 1024;

static inline uint64_t Mix(const void* p, size_t n, uint64_t seed)
{
	return XXH3_64bits_withSeed(p, n, static_cast<XXH64_hash_t>(seed));
}

// Per-unit row hash over exactly the SimSnapshot field set, in a fixed order,
// each value as a fixed-width word (floats via their IEEE-754 bit pattern). No
// padding and no pointers are hashed, so identical content hashes identically on
// every run and platform. If a field is added to SimSnapshot, add a word here.
// The per-allyteam stride rows (losStatusAll/posErrorBits) are folded into one
// word each with an FNV-1a-style mix since their width is game-dependent; every
// field is synced state, so all of it belongs in the desync-localization hash.
static inline uint64_t HashUnitRow(const SimSnapshot::UnitRows& r, int id)
{
	const auto f3 = [](uint32_t* w, const float3& v) {
		w[0] = std::bit_cast<uint32_t>(v.x);
		w[1] = std::bit_cast<uint32_t>(v.y);
		w[2] = std::bit_cast<uint32_t>(v.z);
	};
	const auto pack = [](uint32_t* w, const SResourcePack& p) {
		w[0] = std::bit_cast<uint32_t>(p.metal);
		w[1] = std::bit_cast<uint32_t>(p.energy);
	};

	// PR 27a tail ends at w[70]; PR 31 weapon fold occupies w[71];
	// PR 32 deep-state words occupy w[72]..w[97].
	uint32_t w[98];
	w[0]  = static_cast<uint32_t>(id);
	f3(&w[1], r.pos[id]);
	w[4]  = std::bit_cast<uint32_t>(r.speed[id].x);
	w[5]  = std::bit_cast<uint32_t>(r.speed[id].y);
	w[6]  = std::bit_cast<uint32_t>(r.speed[id].z);
	w[7]  = std::bit_cast<uint32_t>(r.speed[id].w);
	w[8]  = std::bit_cast<uint32_t>(r.health[id]);
	w[9]  = std::bit_cast<uint32_t>(r.maxHealth[id]);
	w[10] = static_cast<uint32_t>(r.team[id])
	      | (static_cast<uint32_t>(r.allyTeam[id])    << 8)
	      | (static_cast<uint32_t>(r.beingBuilt[id])  << 16)
	      | (static_cast<uint32_t>(r.stunned[id])     << 24);
	w[11] = static_cast<uint32_t>(r.defID[id]);
	w[12] = std::bit_cast<uint32_t>(r.buildProgress[id]);
	f3(&w[13], r.midPos[id]);
	f3(&w[16], r.aimPos[id]);
	w[19] = std::bit_cast<uint32_t>(r.paralyzeDamage[id]);
	w[20] = std::bit_cast<uint32_t>(r.captureProgress[id]);
	f3(&w[21], r.relMidPos[id]);
	f3(&w[24], r.frontdir[id]);
	f3(&w[27], r.updir[id]);
	f3(&w[30], r.rightdir[id]);
	f3(&w[33], r.posErrorVector[id]);
	w[36] = static_cast<uint32_t>(r.leavesGhost[id])
	      | (std::bit_cast<uint32_t>(r.radius[id]) & 0xffffff00u); // low byte free for the flag

	uint32_t losAcc = 2166136261u;
	uint32_t errAcc = 2166136261u;
	for (int at = 0; at < r.numAllyTeams; ++at) {
		losAcc = (losAcc ^ r.losStatusAll[at * r.MaxUnits() + id]) * 16777619u;
		errAcc = (errAcc ^ r.posErrorBits[at * r.MaxUnits() + id]) * 16777619u;
		// fold the per-allyteam InRadar answer (PR 25) into the los word
		losAcc = (losAcc ^ (r.inRadarAll[at * r.MaxUnits() + id] << 1)) * 16777619u;
	}
	w[37] = losAcc;
	w[38] = errAcc;
	w[39] = static_cast<uint32_t>(r.numAllyTeams);
	// picking gates (PR 25); noSelect/inVoid are synced state. selVol is
	// UNSYNCED per-object state (a widget can mutate it via the unsynced
	// SetUnitSelectionVolumeData), so it is intentionally NOT folded into the
	// synced-desync hash -- it is covered by the diff gate instead.
	w[40] = static_cast<uint32_t>(r.noSelect[id])
	      | (static_cast<uint32_t>(r.inVoid[id]) << 8);

	// PR 27a rows, fixed order appended after the PR 25 words; all synced state
	w[41] = static_cast<uint32_t>(r.isDead[id])
	      | (static_cast<uint32_t>(r.neutral[id])   << 8)
	      | (static_cast<uint32_t>(r.activated[id]) << 16)
	      | (static_cast<uint32_t>(r.isCloaked[id]) << 24);
	w[42] = static_cast<uint32_t>(static_cast<uint16_t>(r.heading[id]))
	      | (static_cast<uint32_t>(static_cast<uint16_t>(r.buildFacing[id])) << 16);
	w[43] = static_cast<uint32_t>(r.armoredState[id])
	      | (static_cast<uint32_t>(r.blockingBits[id]) << 8);
	w[44] = std::bit_cast<uint32_t>(r.armoredMultiple[id]);
	w[45] = std::bit_cast<uint32_t>(r.height[id]);
	w[46] = std::bit_cast<uint32_t>(r.mass[id]);
	w[47] = std::bit_cast<uint32_t>(r.maxRange[id]);
	w[48] = std::bit_cast<uint32_t>(r.seismicSignature[id]);
	w[49] = std::bit_cast<uint32_t>(r.experience[id]);
	w[50] = std::bit_cast<uint32_t>(r.limExperience[id]);
	w[51] = static_cast<uint32_t>(r.selfDCountdown[id]);
	w[52] = static_cast<uint32_t>(r.losRadius[id]);
	w[53] = static_cast<uint32_t>(r.airLosRadius[id]);
	w[54] = static_cast<uint32_t>(r.radarRadius[id]);
	w[55] = static_cast<uint32_t>(r.sonarRadius[id]);
	w[56] = static_cast<uint32_t>(r.seismicRadius[id]);
	w[57] = static_cast<uint32_t>(r.jammerRadius[id]);
	w[58] = static_cast<uint32_t>(r.sonarJamRadius[id]);
	w[59] = static_cast<uint32_t>(r.moveDefID[id]);
	pack(&w[60], r.resourcesMake[id]);
	pack(&w[62], r.resourcesUse[id]);
	pack(&w[64], r.harvested[id]);
	pack(&w[66], r.harvestStorage[id]);
	pack(&w[68], r.cost[id]);
	w[70] = std::bit_cast<uint32_t>(r.buildTime[id]);

	// PR 31 (weapon/shield family): the whole per-unit weapon block folded into
	// one word (variable-length per unit, like the losStatusAll fold above). All
	// synced state; only the independent discrete runtime + config scalars are
	// folded (the derived experience values AccuracyExperience/... are redundant
	// with `experience`, already hashed, and the position-derived vectors track
	// pos/target which are hashed too -- keeps this cheap and low-noise). The
	// flattened damage arrays are def-/Lua-ctrl-derived and excluded like selVol.
	uint32_t wpnAcc = 2166136261u;
	const auto fnvU32 = [&wpnAcc](uint32_t v) { wpnAcc = (wpnAcc ^ v) * 16777619u; };
	const auto fnvF   = [&](float v) { fnvU32(std::bit_cast<uint32_t>(v)); };
	fnvU32(static_cast<uint32_t>(r.weaponCount[id]));
	fnvU32(static_cast<uint32_t>(r.flankingMode[id]));
	fnvF(r.flankingDir[id].x); fnvF(r.flankingDir[id].y); fnvF(r.flankingDir[id].z);
	fnvF(r.flankingMoveFactor[id]);
	fnvF(r.flankingAvgDamage[id]);
	fnvF(r.flankingDifDamage[id]);
	fnvF(r.flankingMobility[id]);
	fnvU32(static_cast<uint32_t>(r.hasStockpile[id]) | (static_cast<uint32_t>(r.hasShieldWeapon[id]) << 8)
	     | (static_cast<uint32_t>(r.shieldWeaponEnabled[id]) << 16));
	fnvU32(static_cast<uint32_t>(r.stockpileNumStockpiled[id]));
	fnvU32(static_cast<uint32_t>(r.stockpileNumQueued[id]));
	fnvF(r.stockpileBuildPercent[id]);
	fnvF(r.shieldWeaponPower[id]);
	{
		const int base = r.weaponOffset[id];
		for (int wn = 0; wn < r.weaponCount[id]; ++wn) {
			const int wi = base + wn;
			fnvU32(static_cast<uint32_t>(r.wReloadStatus[wi]));
			fnvU32(static_cast<uint32_t>(r.wSalvoLeft[wi]));
			fnvU32(static_cast<uint32_t>(r.wNumStockpiled[wi]));
			fnvU32(static_cast<uint32_t>(r.wNextSalvo[wi]));
			fnvU32(static_cast<uint32_t>(r.wAngleGood[wi])
			     | (static_cast<uint32_t>(r.wTargetType[wi]) << 8)
			     | (static_cast<uint32_t>(r.wIsShield[wi])   << 16)
			     | (static_cast<uint32_t>(r.wShieldEnabled[wi]) << 24));
			fnvU32(static_cast<uint32_t>(r.wTargetUnitID[wi]));
			fnvU32(static_cast<uint32_t>(r.wTargetInterceptID[wi]));
			fnvF(r.wTargetGroundPos[wi].x); fnvF(r.wTargetGroundPos[wi].y); fnvF(r.wTargetGroundPos[wi].z);
			fnvF(r.wRange[wi]);
			fnvF(r.wShieldPower[wi]);
		}
	}
	w[71] = wpnAcc;

	// ---- PR 32 (deep per-unit state) words, appended in fixed order after the
	// PR 31 weapon fold (w[71]). All synced sim state. EXCLUDED (not independent
	// synced state): nanoPieces (model-derived, defID-deterministic), transportees
	// (the inverse relation is captured by each transportee's transporterID word),
	// and the customTooltip string (SetUnitTooltip content, string; covered by the
	// diff gate's serving dual-run, like sideName). The moveType full-table block
	// and the three LOS-variant strides are each folded into one word.
	w[72] = static_cast<uint32_t>(r.fireState[id]);
	w[73] = static_cast<uint32_t>(r.moveState[id]);
	w[74] = std::bit_cast<uint32_t>(r.repairBelowHealth[id]);
	w[75] = static_cast<uint32_t>(r.repeatOrders[id])
	      | (static_cast<uint32_t>(r.wantCloak[id])         << 8)
	      | (static_cast<uint32_t>(r.useHighTrajectory[id]) << 16)
	      | (static_cast<uint32_t>(r.inBuildStance[id])     << 24);
	pack(&w[76], r.storage[id]);
	w[78] = std::bit_cast<uint32_t>(r.metalExtract[id]);
	w[79] = std::bit_cast<uint32_t>(r.buildeeRadius[id]);
	f3(&w[80], r.posErrorDelta[id]);
	w[83] = static_cast<uint32_t>(r.nextPosErrorUpdate[id]);
	w[84] = static_cast<uint32_t>(r.lastAttackerID[id]);
	w[85] = static_cast<uint32_t>(r.transporterID[id]);
	w[86] = static_cast<uint32_t>(r.curBuildID[id]);
	w[87] = static_cast<uint32_t>(r.builderKind[id])
	      | (static_cast<uint32_t>(r.range3D[id])         << 8)
	      | (static_cast<uint32_t>(r.moveTypeKind[id])    << 16)
	      | (static_cast<uint32_t>(r.mtProgressState[id]) << 24);
	w[88] = std::bit_cast<uint32_t>(r.buildDistance[id]);
	w[89] = std::bit_cast<uint32_t>(r.buildPower[id]);
	w[90] = std::bit_cast<uint32_t>(r.mtMaxSpeed[id]);
	w[91] = std::bit_cast<uint32_t>(r.mtMaxWantedSpeed[id]);
	f3(&w[92], r.mtGoalPos[id]);
	w[95] = static_cast<uint32_t>(r.mtAutoLand[id])
	      | (static_cast<uint32_t>(r.mtLoopbackAttack[id]) << 8);

	// moveType full-table block folded into one word (padding-free field fold)
	{
		const SimSnapshot::MoveTypeBlock& b = r.moveTypeBlock[id];
		const uint32_t bw[] = {
			std::bit_cast<uint32_t>(b.turnRate), std::bit_cast<uint32_t>(b.accRate), std::bit_cast<uint32_t>(b.decRate),
			std::bit_cast<uint32_t>(b.maxReverseSpeed), std::bit_cast<uint32_t>(b.wantedSpeed), std::bit_cast<uint32_t>(b.currentSpeed),
			std::bit_cast<uint32_t>(b.goalRadius),
			std::bit_cast<uint32_t>(b.currWayPoint.x), std::bit_cast<uint32_t>(b.currWayPoint.y), std::bit_cast<uint32_t>(b.currWayPoint.z),
			std::bit_cast<uint32_t>(b.nextWayPoint.x), std::bit_cast<uint32_t>(b.nextWayPoint.y), std::bit_cast<uint32_t>(b.nextWayPoint.z),
			std::bit_cast<uint32_t>(b.wantedHeight),
			static_cast<uint32_t>(b.collide) | (static_cast<uint32_t>(b.useSmoothMesh) << 8)
				| (static_cast<uint32_t>(b.bankingAllowed) << 16) | (static_cast<uint32_t>(b.dontLand) << 24),
			static_cast<uint32_t>(b.aircraftState), static_cast<uint32_t>(b.flyState),
			std::bit_cast<uint32_t>(b.goalDistance), std::bit_cast<uint32_t>(b.currentBank), std::bit_cast<uint32_t>(b.currentPitch),
			std::bit_cast<uint32_t>(b.altitudeRate), std::bit_cast<uint32_t>(b.maxDrift),
			std::bit_cast<uint32_t>(b.myGravity), std::bit_cast<uint32_t>(b.maxBank), std::bit_cast<uint32_t>(b.turnRadius),
			std::bit_cast<uint32_t>(b.maxAileron), std::bit_cast<uint32_t>(b.maxElevator), std::bit_cast<uint32_t>(b.maxRudder),
		};
		uint32_t mtAcc = 2166136261u;
		for (const uint32_t x : bw)
			mtAcc = (mtAcc ^ x) * 16777619u;
		w[96] = mtAcc;
	}

	// LOS-variant strides (unit InLos/InAirLos/InJammer answers) folded like inRadarAll
	{
		uint32_t losVarAcc = 2166136261u;
		for (int at = 0; at < r.numAllyTeams; ++at) {
			losVarAcc = (losVarAcc ^ r.unitInLosAll[at * r.MaxUnits() + id]) * 16777619u;
			losVarAcc = (losVarAcc ^ (r.unitInAirLosAll[at * r.MaxUnits() + id] << 1)) * 16777619u;
			losVarAcc = (losVarAcc ^ (r.unitInJammerAll[at * r.MaxUnits() + id] << 2)) * 16777619u;
		}
		w[97] = losVarAcc;
	}

	return Mix(w, sizeof(w), UNIT_SEED);
}

// Per-feature row hash (PR 25 family); folded into the root like the projectile
// section. selVol excluded for the same reason as the unit rows.
static inline uint64_t HashFeatureRow(const SimSnapshot::FeatureRows& r, int id)
{
	const auto f3 = [](uint32_t* w, const float3& v) {
		w[0] = std::bit_cast<uint32_t>(v.x);
		w[1] = std::bit_cast<uint32_t>(v.y);
		w[2] = std::bit_cast<uint32_t>(v.z);
	};

	uint32_t w[46];
	w[0]  = static_cast<uint32_t>(id);
	w[1]  = std::bit_cast<uint32_t>(r.pos[id].x);
	w[2]  = std::bit_cast<uint32_t>(r.pos[id].y);
	w[3]  = std::bit_cast<uint32_t>(r.pos[id].z);
	w[4]  = std::bit_cast<uint32_t>(r.midPos[id].x);
	w[5]  = std::bit_cast<uint32_t>(r.midPos[id].y);
	w[6]  = std::bit_cast<uint32_t>(r.midPos[id].z);
	w[7]  = std::bit_cast<uint32_t>(r.radius[id]);
	w[8]  = static_cast<uint32_t>(r.allyTeam[id]);
	w[9]  = static_cast<uint32_t>(r.defID[id]);
	w[10] = static_cast<uint32_t>(r.alwaysVisible[id])
	      | (static_cast<uint32_t>(r.noSelect[id]) << 8)
	      | (static_cast<uint32_t>(r.inVoid[id])   << 16);
	w[11] = std::bit_cast<uint32_t>(r.relMidPos[id].x);
	w[12] = std::bit_cast<uint32_t>(r.relMidPos[id].y);
	w[13] = std::bit_cast<uint32_t>(r.relMidPos[id].z);

	uint32_t losAcc = 2166136261u;
	for (int at = 0; at < r.numAllyTeams; ++at)
		losAcc = (losAcc ^ r.inLosAll[at * r.MaxSlots() + id]) * 16777619u;
	w[14] = losAcc;
	w[15] = static_cast<uint32_t>(r.numAllyTeams);

	// PR 27a rows, fixed order appended; all synced state
	w[16] = static_cast<uint32_t>(r.team[id]);
	w[17] = std::bit_cast<uint32_t>(r.health[id]);
	w[18] = std::bit_cast<uint32_t>(r.resurrectProgress[id]);
	w[19] = std::bit_cast<uint32_t>(r.height[id]);
	w[20] = std::bit_cast<uint32_t>(r.mass[id]);
	w[21] = std::bit_cast<uint32_t>(r.speed[id].x);
	w[22] = std::bit_cast<uint32_t>(r.speed[id].y);
	w[23] = std::bit_cast<uint32_t>(r.speed[id].z);
	w[24] = std::bit_cast<uint32_t>(r.speed[id].w);
	f3(&w[25], r.matXdir[id]);
	f3(&w[28], r.matYdir[id]);
	f3(&w[31], r.matZdir[id]);
	w[34] = static_cast<uint32_t>(static_cast<uint16_t>(r.heading[id]))
	      | (static_cast<uint32_t>(static_cast<uint16_t>(r.buildFacing[id])) << 16);
	w[35] = std::bit_cast<uint32_t>(r.resources[id].metal);
	w[36] = std::bit_cast<uint32_t>(r.resources[id].energy);
	w[37] = std::bit_cast<uint32_t>(r.defResources[id].metal);
	w[38] = std::bit_cast<uint32_t>(r.defResources[id].energy);
	w[39] = std::bit_cast<uint32_t>(r.reclaimLeft[id]);
	w[40] = std::bit_cast<uint32_t>(r.reclaimTime[id]);
	w[41] = static_cast<uint32_t>(r.blockingBits[id]);
	w[42] = static_cast<uint32_t>(r.resurrectDefID[id]);
	f3(&w[43], r.aimPos[id]);

	return Mix(w, sizeof(w), UNIT_SEED);
}

// Per-team row hash (PR 26 boundary copy); folded into the root like the
// projectile/feature sections. Only the SYNCED numeric per-team block is
// hashed: res packs, current TeamStatistics, unit count, isDead/allyTeam/
// leader, incomeMultiplier, plus the PR-36 synced-numeric additions startPos
// (net-STARTPOS) and maxUnits (ChangeTeam transfers). Excluded: color
// (unsynced-mutable via Spring.SetTeamColor -- the selVol precedent),
// sideName/customOpts (static strings), the PR-36 AI short-name/version/options
// + luaAIName (the LOCAL machine's view -- GetAIInfo returns SYNCED_* for synced
// handles), the full statHistory (its mutating back() is currentStats, already
// hashed; older entries are frozen), and the whole PlayerRows table (net-layer).
static inline uint64_t HashTeamRow(const SimSnapshot::TeamRows& r, int t)
{
	const auto pack = [](uint32_t* w, const SResourcePack& p) {
		w[0] = std::bit_cast<uint32_t>(p.metal);
		w[1] = std::bit_cast<uint32_t>(p.energy);
	};

	uint32_t w[46];
	w[0] = static_cast<uint32_t>(t);
	w[1] = static_cast<uint32_t>(r.leader[t]);
	w[2] = static_cast<uint32_t>(r.isDead[t])
	     | (static_cast<uint32_t>(r.allyTeam[t]) << 8);
	w[3] = static_cast<uint32_t>(r.numUnits[t]);
	w[4] = std::bit_cast<uint32_t>(r.incomeMultiplier[t]);
	pack(&w[5],  r.res[t]);
	pack(&w[7],  r.resStorage[t]);
	pack(&w[9],  r.resPrevPull[t]);
	pack(&w[11], r.resPrevIncome[t]);
	pack(&w[13], r.resPrevExpense[t]);
	pack(&w[15], r.resShare[t]);
	pack(&w[17], r.resPrevSent[t]);
	pack(&w[19], r.resPrevReceived[t]);
	pack(&w[21], r.resPrevExcess[t]);

	const TeamStatistics& s = r.currentStats[t];
	w[23] = static_cast<uint32_t>(s.frame);
	w[24] = std::bit_cast<uint32_t>(s.metalUsed);
	w[25] = std::bit_cast<uint32_t>(s.energyUsed);
	w[26] = std::bit_cast<uint32_t>(s.metalProduced);
	w[27] = std::bit_cast<uint32_t>(s.energyProduced);
	w[28] = std::bit_cast<uint32_t>(s.metalExcess);
	w[29] = std::bit_cast<uint32_t>(s.energyExcess);
	w[30] = std::bit_cast<uint32_t>(s.metalReceived);
	w[31] = std::bit_cast<uint32_t>(s.energyReceived);
	w[32] = std::bit_cast<uint32_t>(s.metalSent);
	w[33] = std::bit_cast<uint32_t>(s.energySent);
	w[34] = std::bit_cast<uint32_t>(s.damageDealt);
	w[35] = std::bit_cast<uint32_t>(s.damageReceived);
	w[36] = static_cast<uint32_t>(s.unitsProduced);
	w[37] = static_cast<uint32_t>(s.unitsDied);
	w[38] = static_cast<uint32_t>(s.unitsReceived);
	w[39] = static_cast<uint32_t>(s.unitsSent);
	w[40] = static_cast<uint32_t>(s.unitsCaptured)
	      | (static_cast<uint32_t>(s.unitsOutCaptured) << 16);
	w[41] = static_cast<uint32_t>(s.unitsKilled);

	// PR 36: synced-numeric team-misc additions (startPos + maxUnits)
	w[42] = std::bit_cast<uint32_t>(r.startPos[t].x);
	w[43] = std::bit_cast<uint32_t>(r.startPos[t].y);
	w[44] = std::bit_cast<uint32_t>(r.startPos[t].z);
	w[45] = static_cast<uint32_t>(r.maxUnits[t]);

	return Mix(w, sizeof(w), UNIT_SEED);
}

// Per-projectile row hash (second snapshot family); folded into the root hash
// as one section word -- localization to unit granularity stays the unit rows'
// job, projectile rows just extend divergence *detection* coverage.
static inline uint64_t HashProjectileRow(const SimSnapshot::ProjectileRows& r, int id)
{
	uint32_t w[31];
	w[0]  = static_cast<uint32_t>(id);
	w[1]  = std::bit_cast<uint32_t>(r.pos[id].x);
	w[2]  = std::bit_cast<uint32_t>(r.pos[id].y);
	w[3]  = std::bit_cast<uint32_t>(r.pos[id].z);
	w[4]  = std::bit_cast<uint32_t>(r.speed[id].x);
	w[5]  = std::bit_cast<uint32_t>(r.speed[id].y);
	w[6]  = std::bit_cast<uint32_t>(r.speed[id].z);
	w[7]  = std::bit_cast<uint32_t>(r.speed[id].w);
	w[8]  = static_cast<uint32_t>(r.allyTeam[id]);
	w[9]  = static_cast<uint32_t>(r.ownerID[id]);
	w[10] = static_cast<uint32_t>(r.isWeapon[id])
	      | (static_cast<uint32_t>(r.targetType[id]) << 8);
	w[11] = static_cast<uint32_t>(r.weaponDefID[id]);
	w[12] = static_cast<uint32_t>(r.targetID[id]);
	w[13] = std::bit_cast<uint32_t>(r.targetPos[id].x);
	w[14] = std::bit_cast<uint32_t>(r.targetPos[id].y);
	w[15] = std::bit_cast<uint32_t>(r.targetPos[id].z);

	uint32_t losAcc = 2166136261u;
	for (int at = 0; at < r.numAllyTeams; ++at)
		losAcc = (losAcc ^ r.inLosAll[at * r.MaxSlots() + id]) * 16777619u;
	w[16] = losAcc;

	// PR 27a rows, fixed order appended; all synced state
	w[17] = std::bit_cast<uint32_t>(r.dir[id].x);
	w[18] = std::bit_cast<uint32_t>(r.dir[id].y);
	w[19] = std::bit_cast<uint32_t>(r.dir[id].z);
	w[20] = std::bit_cast<uint32_t>(r.mygravity[id]);
	w[21] = static_cast<uint32_t>(r.teamID[id]);
	w[22] = static_cast<uint32_t>(r.ttl[id]);
	w[23] = static_cast<uint32_t>(r.isPiece[id])
	      | (static_cast<uint32_t>(r.intercepted[id]) << 8);

	// PR 33 piece-projectile params, fixed order appended; all synced state
	// (0/default for non-piece projectiles). pieceName is model-derived
	// (defID-deterministic) so it is NOT hashed as independent synced state --
	// any name divergence implies a model/defID divergence already covered.
	w[24] = static_cast<uint32_t>(r.pieceExplFlags[id]);
	w[25] = std::bit_cast<uint32_t>(r.pieceSpinAngle[id]);
	w[26] = std::bit_cast<uint32_t>(r.pieceSpinSpeed[id]);
	w[27] = std::bit_cast<uint32_t>(r.pieceSpinVec[id].x);
	w[28] = std::bit_cast<uint32_t>(r.pieceSpinVec[id].y);
	w[29] = std::bit_cast<uint32_t>(r.pieceSpinVec[id].z);
	// PR 34 (spatial/list remainder), appended in fixed order after PR 33's
	// piece params to keep every prior hash-word index stable
	w[30] = std::bit_cast<uint32_t>(r.radius[id]);

	return Mix(w, sizeof(w), UNIT_SEED);
}


static std::atomic<bool> armed{false};

static bool dumpActive = false;
static int dumpStartFrame = 0;
static int dumpEndFrame = 0;
static Detail dumpDetail = DETAIL_ROOT;
static std::string dumpPath;
static std::string rowBuf; // all emitted lines (header included), persists across rewind reloads

static constexpr const char* FILE_HEADER =
	"# SnapshotHash dump (PR 16). Tab-separated, tag-prefixed lines.\n"
	"# R\\t<frame>\\t<alive>\\t<rootHash>            one per sim frame\n"
	"# B\\t<frame>\\t<bucketIndex>\\t<bucketHash>     occupied id-range buckets (detail>=bucket)\n"
	"# U\\t<frame>\\t<unitID>\\t<unitHash>            valid units (detail>=unit)\n"
	"# A frame appears once per pass; a rewind re-emits the same frame numbers, so\n"
	"# grouping by frame and finding the first frame whose R hashes disagree gives\n"
	"# the first diverging sim frame; the B/U lines localize it in space.\n";


bool Armed()
{
	return armed.load(std::memory_order_relaxed);
}

void StartDump(int startFrame, int endFrame, std::string path, Detail detail)
{
	if (dumpActive) {
		// keep the accumulated buffer (rewind re-arm is intentional, see header)
		LOG_L(L_WARNING, "[SnapshotHash::StartDump] a dump is already active (-> %s); ignoring re-arm", dumpPath.c_str());
		return;
	}
	if (endFrame < startFrame) {
		LOG_L(L_ERROR, "[SnapshotHash::StartDump] end frame %d precedes start frame %d", endFrame, startFrame);
		return;
	}

	dumpActive = true;
	dumpStartFrame = startFrame;
	dumpEndFrame = endFrame;
	dumpDetail = detail;
	dumpPath = std::move(path);

	rowBuf.clear();
	rowBuf.reserve(1 << 20);
	rowBuf += FILE_HEADER;

	armed.store(true, std::memory_order_relaxed);

	LOG("[SnapshotHash::StartDump] armed: sim frames %d..%d detail=%d -> %s",
		startFrame, endFrame, static_cast<int>(detail), dumpPath.c_str());
}

static void WriteOut()
{
	if (dumpPath.empty())
		return;

	std::ofstream f(dumpPath);
	if (!f.good()) {
		LOG_L(L_ERROR, "[SnapshotHash] cannot open %s for writing", dumpPath.c_str());
		return;
	}
	f << rowBuf;
	LOG("[SnapshotHash] wrote snapshot hashes (frames %d..%d) to %s",
		dumpStartFrame, dumpEndFrame, dumpPath.c_str());
}

void StopDump()
{
	if (!dumpActive)
		return;

	dumpActive = false;
	armed.store(false, std::memory_order_relaxed);

	WriteOut();

	rowBuf.clear();
	rowBuf.shrink_to_fit();
}

void FlushPartial()
{
	if (!dumpActive)
		return;

	// Rewrite the file with everything collected so far but do NOT disarm or drop
	// the buffer: ~CGame runs on every rewind reload, and we want the forward and
	// post-rewind passes to accumulate. The final teardown leaves a complete file.
	WriteOut();
}

void HashFrame(int frameNum, const SimSnapshot::UnitRows& rows, const SimSnapshot::ProjectileRows& projRows, const SimSnapshot::FeatureRows& featRows, const SimSnapshot::TeamRows& teamRows)
{
	if (!dumpActive)
		return;
	if (frameNum < dumpStartFrame || frameNum > dumpEndFrame)
		return;

	const size_t maxUnits = rows.valid.size();
	const uint32_t numBuckets = static_cast<uint32_t>((maxUnits + BUCKET_SIZE - 1) / BUCKET_SIZE);

	// seed every bucket by its index so an emptied bucket still differs from a
	// never-populated one, and empty buckets contribute deterministically
	std::vector<uint64_t> bucketHash(numBuckets);
	std::vector<uint32_t> bucketCount(numBuckets, 0);
	for (uint32_t b = 0; b < numBuckets; ++b)
		bucketHash[b] = Mix(&b, sizeof(b), BUCKET_SEED);

	char ub[64];
	uint32_t alive = 0;

	// ascending id order => deterministic fold order within each bucket
	for (size_t id = 0; id < maxUnits; ++id) {
		if (rows.valid[id] == 0)
			continue;

		++alive;
		const uint64_t uh = HashUnitRow(rows, static_cast<int>(id));
		const uint32_t b = static_cast<uint32_t>(id) / BUCKET_SIZE;
		bucketHash[b] = Mix(&uh, sizeof(uh), bucketHash[b]);
		++bucketCount[b];

		if (dumpDetail >= DETAIL_UNIT) {
			const int n = std::snprintf(ub, sizeof(ub), "U\t%d\t%d\t%016llx\n",
				frameNum, static_cast<int>(id), static_cast<unsigned long long>(uh));
			rowBuf.append(ub, n);
		}
	}

	uint64_t rootHash = ROOT_SEED;
	for (uint32_t b = 0; b < numBuckets; ++b)
		rootHash = Mix(&bucketHash[b], sizeof(bucketHash[b]), rootHash);

	// projectile section: one word folded into the root (ascending id order)
	{
		uint64_t projHash = Mix(&ROOT_SEED, sizeof(ROOT_SEED), BUCKET_SEED);
		for (size_t id = 0; id < projRows.MaxSlots(); ++id) {
			if (projRows.valid[id] == 0)
				continue;
			const uint64_t ph = HashProjectileRow(projRows, static_cast<int>(id));
			projHash = Mix(&ph, sizeof(ph), projHash);
		}
		rootHash = Mix(&projHash, sizeof(projHash), rootHash);
	}

	// feature section: one word folded into the root (ascending id order)
	{
		uint64_t featHash = Mix(&BUCKET_SEED, sizeof(BUCKET_SEED), ROOT_SEED);
		for (size_t id = 0; id < featRows.MaxSlots(); ++id) {
			if (featRows.valid[id] == 0)
				continue;
			const uint64_t fh = HashFeatureRow(featRows, static_cast<int>(id));
			featHash = Mix(&fh, sizeof(fh), featHash);
		}
		rootHash = Mix(&featHash, sizeof(featHash), rootHash);
	}

	// team section (PR 26): one word folded into the root (ascending teamID)
	{
		uint64_t teamHash = Mix(&UNIT_SEED, sizeof(UNIT_SEED), ROOT_SEED);
		for (int t = 0; t < teamRows.activeTeams; ++t) {
			const uint64_t th = HashTeamRow(teamRows, t);
			teamHash = Mix(&th, sizeof(th), teamHash);
		}
		rootHash = Mix(&teamHash, sizeof(teamHash), rootHash);
	}

	char rb[64];
	const int rn = std::snprintf(rb, sizeof(rb), "R\t%d\t%u\t%016llx\n",
		frameNum, alive, static_cast<unsigned long long>(rootHash));
	rowBuf.append(rb, rn);

	if (dumpDetail >= DETAIL_BUCKET) {
		char bb[64];
		for (uint32_t b = 0; b < numBuckets; ++b) {
			if (bucketCount[b] == 0)
				continue;
			const int bn = std::snprintf(bb, sizeof(bb), "B\t%d\t%u\t%016llx\n",
				frameNum, b, static_cast<unsigned long long>(bucketHash[b]));
			rowBuf.append(bb, bn);
		}
	}
}

} // namespace SnapshotHash
