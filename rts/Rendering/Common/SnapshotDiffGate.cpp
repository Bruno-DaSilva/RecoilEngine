/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotDiffGate.h"

#include <cstring>
#include <vector>

#include "SimSnapshot.h"
#include "DrawMapMirrors.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/Wind.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name
#include "Rendering/Models/3DModelPiece.hpp" // PR 33 S3DModelPiece::name (ppro->omp)
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
#include "Lua/LuaSnapshotServe.h" // sim|draw PR 30: command-queue serving-cache compare
#include "System/Log/ILog.h"

SnapshotDiffGate snapshotDiffGate;

// bit-exact float compare: master copies raw values into the snapshot, so the
// expected relation is bit equality, not numeric equality. memcmp treats a NaN
// as equal to the identically-encoded NaN it was copied from (whereas == would
// spuriously flag it) and -0.0 as distinct from +0.0 (a real torn-copy signal).
static bool BitEqual(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
static bool BitEqual(const float3& a, const float3& b) { return std::memcmp(&a, &b, sizeof(float3)) == 0; }
static bool BitEqual(const float4& a, const float4& b) { return std::memcmp(&a, &b, sizeof(float4)) == 0; }

static constexpr const char* FIELD_NAMES[] = {
	"validity",
	"pos",
	"midPos",
	"aimPos",
	"speed",
	"health",
	"maxHealth",
	"paralyzeDamage",
	"captureProgress",
	"team",
	"allyTeam",
	"defID",
	"buildProgress",
	"beingBuilt",
	"stunned",
	"radius",
	"relMidPos",
	"frontdir",
	"updir",
	"rightdir",
	"posErrorVector",
	"leavesGhost",
	"losStatusAll",
	"posErrorBits",
	"globals",
	"maskedErrorVec",
	"noSelect",
	"inVoid",
	"selVol",
	"inRadarAll",
	"stateFlags",
	"heading/buildFacing",
	"unitScalars",
	"unitEco",
	"sensorRadii",
	"unitMiscInts",
	"blockingBits",
	"proj:validity",
	"proj:pos",
	"proj:speed",
	"proj:allyTeam",
	"proj:ownerID",
	"proj:isWeapon",
	"proj:weaponDefID",
	"proj:target",
	"proj:inLosAll",
	"proj:dir",
	"proj:gravity",
	"proj:teamID",
	"proj:ttlFlags",
	"feat:validity",
	"feat:pos",
	"feat:midPos",
	"feat:aimPos",
	"feat:relMidPos",
	"feat:radius",
	"feat:allyTeam",
	"feat:defID",
	"feat:flags",
	"feat:selVol",
	"feat:inLosAll",
	"feat:team",
	"feat:scalars",
	"feat:speed",
	"feat:dirMatrix",
	"feat:resources",
	"feat:blockingBits",
	"feat:resurrect",
	"feat:globals",
	"team:globals",
	"team:state",
	"team:res",
	"team:stats",
	"team:color",
	"team:strings",
	"player:globals",
	"player:info",
	"player:state",
	"player:net",
	"player:opts",
	"glob:frame",
	"glob:speed",
	"glob:flags",
	"glob:wind",
	"glob:heights",
	"glob:globalLos",
	// map-layer mirrors (PR 28)
	"map:los",
	"map:terrainTypes",
	"map:smoothMesh",
	"map:origHeight",
	"map:radarError",
	// command-queue serving cache (sim|draw PR 30)
	"cq:presence",
	"cq:queue",
	"cq:descs",
	"cq:worker",
	"cq:factory",
	// PR 33 piece/script family (appended last, matches P_PIECEPARAMS)
	"proj:pieceParams",
	// PR 34 (spatial/list remainder): appended to match the enum tail P_RADIUS
	"proj:radius",
};

// structural compare for the copied customOpts maps (emilib::HashMap has no
// operator==); sizes equal + every key of a maps to an equal value in b
static bool OptsEqual(const spring::unordered_map<std::string, std::string>& a,
                      const spring::unordered_map<std::string, std::string>& b)
{
	if (a.size() != b.size())
		return false;

	for (const auto& [key, value] : a) {
		const auto it = b.find(key);
		if (it == b.end() || it->second != value)
			return false;
	}

	return true;
}

// GetSolidObjectBlocking's seven pushed booleans as one byte, bit i = push
// slot i; must stay identical to SimSnapshot.cpp's extraction-side twin
static uint8_t PackBlockingBits(const CSolidObject* o)
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

// field-wise CollisionVolume compare over exactly the params the hit-test reads
// (avoids memcmp padding-byte false positives between two field-wise copies)
static bool ColVolEqual(const CollisionVolume& a, const CollisionVolume& b)
{
	return BitEqual(a.GetScales(), b.GetScales())
		&& BitEqual(a.GetOffsets(), b.GetOffsets())
		&& a.GetVolumeType() == b.GetVolumeType()
		&& a.GetPrimaryAxis() == b.GetPrimaryAxis()
		&& a.GetSecondaryAxis(0) == b.GetSecondaryAxis(0)
		&& a.GetSecondaryAxis(1) == b.GetSecondaryAxis(1)
		&& a.IgnoreHits() == b.IgnoreHits()
		&& a.UseContHitTest() == b.UseContHitTest()
		&& a.DefaultToPieceTree() == b.DefaultToPieceTree()
		&& a.DefaultToFootPrint() == b.DefaultToFootPrint();
}


void SnapshotDiffGate::Arm()
{
	ResetCounters();
	armed = true;
	LOG("[SnapshotDiffGate] armed: verifying SimSnapshot-served values against live sim at each draw boundary");
}

void SnapshotDiffGate::Disarm()
{
	if (!armed) {
		LOG_L(L_WARNING, "[SnapshotDiffGate] not armed");
		return;
	}
	Report("disarm");
	armed = false;
}

void SnapshotDiffGate::Dump() const
{
	if (!armed) {
		LOG_L(L_WARNING, "[SnapshotDiffGate] not armed");
		return;
	}
	Report("dump");
}

void SnapshotDiffGate::FlushPartial()
{
	if (!armed)
		return;
	Report("game-end");
	armed = false;
}

void SnapshotDiffGate::ResetCounters()
{
	// a name/enum drift would silently mislabel every report row
	static_assert(sizeof(FIELD_NAMES) / sizeof(FIELD_NAMES[0]) == F_COUNT, "FIELD_NAMES out of sync with the field enum");

	boundaryChecks = 0;

	for (int f = 0; f < F_COUNT; ++f)
		fields[f] = FieldCounter{FIELD_NAMES[f]};

	callouts.clear();
}

bool SnapshotDiffGate::Bump(FieldCounter& fc, bool equal)
{
	fc.checked++;
	if (equal)
		return false;

	fc.mismatched++;
	if (fc.logged >= kMaxLogged)
		return false;

	fc.logged++;
	return true;
}

void SnapshotDiffGate::Report(const char* reason) const
{
	uint64_t totalMismatch = 0;
	for (const FieldCounter& fc : fields)
		totalMismatch += fc.mismatched;
	for (const auto& [name, fc] : callouts)
		totalMismatch += fc.mismatched;

	LOG("[SnapshotDiffGate] ===== report (%s) : %s =====",
		reason, (totalMismatch == 0) ? "PASS (0 mismatches)" : "FAIL");
	LOG("[SnapshotDiffGate] boundary checks=%llu, total mismatches=%llu",
		(unsigned long long)boundaryChecks, (unsigned long long)totalMismatch);

	for (const FieldCounter& fc : fields) {
		LOG("[SnapshotDiffGate]   %-28s checked=%-14llu mismatched=%llu",
			fc.name, (unsigned long long)fc.checked, (unsigned long long)fc.mismatched);
	}
	for (const auto& [name, fc] : callouts) {
		LOG("[SnapshotDiffGate]   callout:%-20s checked=%-14llu mismatched=%llu",
			name.c_str(), (unsigned long long)fc.checked, (unsigned long long)fc.mismatched);
	}
}


void SnapshotDiffGate::CountCallout(const char* callout, bool equal, const char* detail)
{
	if (!armed)
		return;

	FieldCounter& fc = callouts[callout];

	if (Bump(fc, equal))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d callout=%s %s",
			gs->frameNum, callout, detail);
}


void SnapshotDiffGate::CheckBoundary()
{
	if (!armed)
		return;

	const SimSnapshot::UnitRows& rows = simSnapshot.Read();

	// after simSnapshot.Update() the front buffer always describes the current
	// sim frame (Update() is due whenever simFrame != frameNum). If that does
	// not hold something published the wrong buffer -- bail rather than emit a
	// storm of false positives.
	if (rows.simFrame != gs->frameNum) {
		LOG_L(L_ERROR, "[SnapshotDiffGate] snapshot frame %d != sim frame %d; skipping boundary check",
			rows.simFrame, gs->frameNum);
		return;
	}

	boundaryChecks++;

	const int numAllyTeams = rows.numAllyTeams;
	const size_t maxUnits = rows.MaxUnits();

	// global block: alliance matrix + radar-error scalars (one bundled counter)
	{
		bool globalsEqual = (numAllyTeams == teamHandler.ActiveAllyTeams());
		globalsEqual = globalsEqual && BitEqual(rows.baseRadarErrorSize, losHandler->GetBaseRadarErrorSize());
		for (int at = 0; globalsEqual && at < numAllyTeams; ++at)
			globalsEqual = BitEqual(rows.radarErrorSizes[at], losHandler->GetAllyTeamRadarErrorSize(at));
		for (int a = 0; globalsEqual && a < numAllyTeams; ++a)
			for (int b = 0; globalsEqual && b < numAllyTeams; ++b)
				globalsEqual = (rows.Allied(a, b) == teamHandler.Ally(a, b));

		if (Bump(fields[F_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=globals mismatch (numAllyTeams snap=%d live=%d)",
				gs->frameNum, numAllyTeams, teamHandler.ActiveAllyTeams());
	}

	for (size_t i = 0; i < maxUnits; ++i) {
		const int id = static_cast<int>(i);
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits

		const bool snapValid = (rows.valid[i] != 0);
		const bool liveValid = (u != nullptr);

		if (Bump(fields[F_VALIDITY], snapValid == liveValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=validity snap=%d live=%d",
				gs->frameNum, id, int(snapValid), int(liveValid));

		// field comparisons only make sense where both agree the row exists;
		// a one-sided validity mismatch was already reported above
		if (!snapValid || !liveValid)
			continue;

		const auto checkF3 = [&](int f, const float3& snap, const float3& live) {
			if (Bump(fields[f], BitEqual(snap, live)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
					gs->frameNum, id, FIELD_NAMES[f], snap.x, snap.y, snap.z, live.x, live.y, live.z);
		};
		const auto checkF = [&](int f, float snap, float live) {
			if (Bump(fields[f], BitEqual(snap, live)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=%.9g live=%.9g",
					gs->frameNum, id, FIELD_NAMES[f], snap, live);
		};
		const auto checkI = [&](int f, int snap, int live) {
			if (Bump(fields[f], snap == live))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=%d live=%d",
					gs->frameNum, id, FIELD_NAMES[f], snap, live);
		};

		checkF3(F_POS, rows.pos[i], u->pos);
		checkF3(F_MIDPOS, rows.midPos[i], u->midPos);
		checkF3(F_AIMPOS, rows.aimPos[i], u->aimPos);

		if (Bump(fields[F_SPEED], BitEqual(rows.speed[i], u->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=speed snap=(%.9g,%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.speed[i].x, rows.speed[i].y, rows.speed[i].z, rows.speed[i].w,
				u->speed.x, u->speed.y, u->speed.z, u->speed.w);

		checkF(F_HEALTH, rows.health[i], u->health);
		checkF(F_MAXHEALTH, rows.maxHealth[i], u->maxHealth);
		checkF(F_PARALYZE, rows.paralyzeDamage[i], u->paralyzeDamage);
		checkF(F_CAPTURE, rows.captureProgress[i], u->captureProgress);
		checkI(F_TEAM, rows.team[i], static_cast<uint8_t>(u->team));
		checkI(F_ALLYTEAM, rows.allyTeam[i], static_cast<uint8_t>(u->allyteam));
		checkI(F_DEFID, rows.defID[i], u->unitDef->id);
		checkF(F_BUILDPROGRESS, rows.buildProgress[i], u->buildProgress);
		checkI(F_BEINGBUILT, rows.beingBuilt[i], int(u->beingBuilt));
		checkI(F_STUNNED, rows.stunned[i], int(u->IsStunned()));
		checkF(F_RADIUS, rows.radius[i], u->radius);
		checkF3(F_RELMIDPOS, rows.relMidPos[i], u->relMidPos);
		checkF3(F_FRONTDIR, rows.frontdir[i], u->frontdir);
		checkF3(F_UPDIR, rows.updir[i], u->updir);
		checkF3(F_RIGHTDIR, rows.rightdir[i], u->rightdir);
		checkF3(F_POSERRORVEC, rows.posErrorVector[i], u->posErrorVector);
		checkI(F_LEAVESGHOST, rows.leavesGhost[i], int(u->leavesGhost));
		checkI(F_NOSELECT, int(rows.noSelect[i]), int(u->noSelect));
		checkI(F_INVOID, int(rows.inVoid[i]), int(u->IsInVoid()));

		if (Bump(fields[F_SELVOL], ColVolEqual(rows.selVol[i], u->selectionVolume)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=selVol mismatch (type snap=%d live=%d)",
				gs->frameNum, id, rows.selVol[i].GetVolumeType(), u->selectionVolume.GetVolumeType());

		// PR 27a unit rows (grouped counters, see the enum comments)
		{
			const bool stateEqual =
				(rows.isDead[i] == uint8_t(u->isDead)) &&
				(rows.neutral[i] == uint8_t(u->neutral)) &&
				(rows.activated[i] == uint8_t(u->activated)) &&
				(rows.isCloaked[i] == uint8_t(u->isCloaked)) &&
				(rows.armoredState[i] == uint8_t(u->armoredState));
			if (Bump(fields[F_STATEFLAGS], stateEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=stateFlags mismatch", gs->frameNum, id);

			const bool headingEqual =
				(rows.heading[i] == int16_t(u->heading)) &&
				(rows.buildFacing[i] == int16_t(u->buildFacing));
			if (Bump(fields[F_HEADINGFACING], headingEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=heading/buildFacing snap=%d/%d live=%d/%d",
					gs->frameNum, id, int(rows.heading[i]), int(rows.buildFacing[i]), int(u->heading), int(u->buildFacing));

			const bool scalarsEqual =
				BitEqual(rows.height[i], u->height) &&
				BitEqual(rows.mass[i], u->mass) &&
				BitEqual(rows.maxRange[i], u->maxRange) &&
				BitEqual(rows.seismicSignature[i], u->seismicSignature) &&
				BitEqual(rows.armoredMultiple[i], u->armoredMultiple) &&
				BitEqual(rows.experience[i], u->experience) &&
				BitEqual(rows.limExperience[i], u->limExperience);
			if (Bump(fields[F_UNITSCALARS], scalarsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitScalars mismatch", gs->frameNum, id);

			const auto packEqual = [](const SResourcePack& a, const SResourcePack& b) {
				return BitEqual(a.metal, b.metal) && BitEqual(a.energy, b.energy);
			};
			const bool ecoEqual =
				packEqual(rows.resourcesMake[i], u->resourcesMake) &&
				packEqual(rows.resourcesUse[i], u->resourcesUse) &&
				packEqual(rows.harvested[i], u->harvested) &&
				packEqual(rows.harvestStorage[i], u->harvestStorage) &&
				packEqual(rows.cost[i], u->cost) &&
				BitEqual(rows.buildTime[i], u->buildTime);
			if (Bump(fields[F_UNITECO], ecoEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitEco mismatch", gs->frameNum, id);

			const bool sensorsEqual =
				(rows.losRadius[i] == u->losRadius) &&
				(rows.airLosRadius[i] == u->airLosRadius) &&
				(rows.radarRadius[i] == u->radarRadius) &&
				(rows.sonarRadius[i] == u->sonarRadius) &&
				(rows.seismicRadius[i] == u->seismicRadius) &&
				(rows.jammerRadius[i] == u->jammerRadius) &&
				(rows.sonarJamRadius[i] == u->sonarJamRadius);
			if (Bump(fields[F_SENSORRADII], sensorsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=sensorRadii mismatch", gs->frameNum, id);

			const bool intsEqual =
				(rows.selfDCountdown[i] == u->selfDCountdown) &&
				(rows.moveDefID[i] == ((u->moveDef != nullptr) ? int32_t(u->moveDef->pathType) : -1));
			if (Bump(fields[F_UNITMISCINTS], intsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitMiscInts mismatch (selfD snap=%d live=%d)",
					gs->frameNum, id, rows.selfDCountdown[i], u->selfDCountdown);

			if (Bump(fields[F_BLOCKINGBITS], rows.blockingBits[i] == PackBlockingBits(u)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=blockingBits snap=0x%02x live=0x%02x",
					gs->frameNum, id, int(rows.blockingBits[i]), int(PackBlockingBits(u)));
		}

		// per-allyteam stride rows + the masked-value sweep: for every POV,
		// UnitRows::ErrorVector is the exact masking function the Lua serving
		// twins apply (SimSnapshot.h masking policy) -- diff it against the
		// live formula. The at == -1 iteration exercises the invalid-allyteam
		// branch (a fullRead-less handle without a read allyteam).
		for (int at = -1; at < numAllyTeams; ++at) {
			if (at >= 0) {
				if (Bump(fields[F_LOSSTATUS], rows.losStatusAll[at * maxUnits + i] == u->losStatus[at]))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=losStatusAll[ally %d] snap=0x%02x live=0x%02x",
						gs->frameNum, id, at, rows.losStatusAll[at * maxUnits + i], u->losStatus[at]);

				if (Bump(fields[F_POSERRORBIT], rows.posErrorBits[at * maxUnits + i] == uint8_t(u->GetPosErrorBit(at))))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=posErrorBits[ally %d] snap=%d live=%d",
						gs->frameNum, id, at, int(rows.posErrorBits[at * maxUnits + i]), int(u->GetPosErrorBit(at)));

				if (Bump(fields[F_INRADAR], (rows.inRadarAll[at * maxUnits + i] != 0) == losHandler->InRadar(u, at)))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=inRadarAll[ally %d] snap=%d live=%d",
						gs->frameNum, id, at, int(rows.inRadarAll[at * maxUnits + i]), int(losHandler->InRadar(u, at)));
			}

			const float3 snapErr = rows.ErrorVector(id, at);
			const float3 liveErr = u->GetErrorVector(at);

			if (Bump(fields[F_MASKEDERRVEC], BitEqual(snapErr, liveErr)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=maskedErrorVec[ally %d] snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
					gs->frameNum, id, at, snapErr.x, snapErr.y, snapErr.z, liveErr.x, liveErr.y, liveErr.z);
		}
	}

	CheckProjectileRows();
	CheckFeatureRows();
	CheckTeamPlayerRows();
	CheckGlobalRows();
	CheckMapMirrors();
	CheckCmdQueueRows();
}

void SnapshotDiffGate::CheckProjectileRows()
{
	const SimSnapshot::ProjectileRows& rows = simSnapshot.ReadProjectiles();
	const size_t slots = rows.MaxSlots();
	const int numAllyTeams = rows.numAllyTeams;

	// validity both ways: every valid row must resolve to a live synced
	// projectile and every live one must have a valid row (checked via its id)
	std::vector<uint8_t> liveSeen(slots, 0);
	const auto& pc = projectileHandler.GetActiveProjectiles(true);

	for (size_t i = 0; i < pc.size(); ++i) {
		const CProjectile* p = pc[i];
		const int id = p->id;

		if (static_cast<size_t>(id) < slots)
			liveSeen[id] = 1;

		const bool snapValid = rows.Valid(id);

		if (Bump(fields[P_VALIDITY], snapValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:validity snap=0 live=1",
				gs->frameNum, id);

		if (!snapValid)
			continue;

		if (Bump(fields[P_POS], BitEqual(rows.pos[id], p->pos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:pos snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.pos[id].x, rows.pos[id].y, rows.pos[id].z, p->pos.x, p->pos.y, p->pos.z);

		if (Bump(fields[P_SPEED], BitEqual(rows.speed[id], p->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:speed", gs->frameNum, id);

		if (Bump(fields[P_ALLYTEAM], rows.allyTeam[id] == p->GetAllyteamID()))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:allyTeam snap=%d live=%d",
				gs->frameNum, id, rows.allyTeam[id], p->GetAllyteamID());

		if (Bump(fields[P_OWNERID], rows.ownerID[id] == p->GetOwnerID()))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:ownerID snap=%d live=%d",
				gs->frameNum, id, rows.ownerID[id], p->GetOwnerID());

		if (Bump(fields[P_ISWEAPON], rows.isWeapon[id] == uint8_t(p->weapon)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:isWeapon snap=%d live=%d",
				gs->frameNum, id, int(rows.isWeapon[id]), int(p->weapon));

		// PR 27a projectile rows
		if (Bump(fields[P_DIR], BitEqual(rows.dir[id], p->dir)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:dir snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.dir[id].x, rows.dir[id].y, rows.dir[id].z, p->dir.x, p->dir.y, p->dir.z);

		if (Bump(fields[P_GRAVITY], BitEqual(rows.mygravity[id], p->mygravity)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:gravity snap=%.9g live=%.9g",
				gs->frameNum, id, rows.mygravity[id], p->mygravity);

		if (Bump(fields[P_TEAMID], rows.teamID[id] == static_cast<int32_t>(p->GetTeamID())))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:teamID snap=%d live=%d",
				gs->frameNum, id, rows.teamID[id], int(p->GetTeamID()));

		// PR 34 (spatial/list remainder): projectile radius (GetProjectilesInSphere)
		if (Bump(fields[P_RADIUS], BitEqual(rows.radius[id], p->radius)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:radius snap=%.9g live=%.9g",
				gs->frameNum, id, rows.radius[id], p->radius);

		{
			int32_t liveTtl = 0;
			uint8_t liveIntercepted = 0;
			if (p->weapon) {
				const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
				liveTtl = wpro->GetTimeToLive();
				liveIntercepted = wpro->IsBeingIntercepted();
			}
			const bool ttlFlagsEqual =
				(rows.ttl[id] == liveTtl) &&
				(rows.intercepted[id] == liveIntercepted) &&
				(rows.isPiece[id] == uint8_t(p->piece));
			if (Bump(fields[P_TTLFLAGS], ttlFlagsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:ttlFlags mismatch (ttl snap=%d live=%d)",
					gs->frameNum, id, rows.ttl[id], liveTtl);
		}

		// PR 33 piece-projectile params (GetPieceProjectileParams/Name serving)
		{
			int32_t liveExplFlags = 0;
			float liveSpinAngle = 0.0f;
			float liveSpinSpeed = 0.0f;
			float3 liveSpinVec;
			std::string liveName;
			if (p->piece) {
				const CPieceProjectile* ppro = static_cast<const CPieceProjectile*>(p);
				liveExplFlags = ppro->explFlags;
				liveSpinAngle = ppro->spinAngle;
				liveSpinSpeed = ppro->spinSpeed;
				liveSpinVec = ppro->spinVec;
				if (ppro->omp != nullptr)
					liveName = ppro->omp->name;
			}
			const bool pieceEqual =
				(rows.pieceExplFlags[id] == liveExplFlags) &&
				BitEqual(rows.pieceSpinAngle[id], liveSpinAngle) &&
				BitEqual(rows.pieceSpinSpeed[id], liveSpinSpeed) &&
				BitEqual(rows.pieceSpinVec[id], liveSpinVec) &&
				(rows.pieceName[id] == liveName);
			if (Bump(fields[P_PIECEPARAMS], pieceEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:pieceParams mismatch (explFlags snap=%d live=%d)",
					gs->frameNum, id, rows.pieceExplFlags[id], liveExplFlags);
		}

		if (p->weapon) {
			const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
			const WeaponDef* wdef = wpro->GetWeaponDef();
			const int liveWdefID = (wdef != nullptr) ? wdef->id : -1;

			if (Bump(fields[P_WDEFID], rows.weaponDefID[id] == liveWdefID))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:weaponDefID snap=%d live=%d",
					gs->frameNum, id, rows.weaponDefID[id], liveWdefID);

			// same type resolution as the extraction / the live callout
			const CWorldObject* wtgt = wpro->GetTargetObject();
			uint8_t liveType = 0;
			int liveID = 0;
			float3 livePos;
			if (wtgt == nullptr) {
				liveType = 'g';
				livePos = wpro->GetTargetPos();
			} else if (dynamic_cast<const CUnit*>(wtgt) != nullptr) {
				liveType = 'u'; liveID = wtgt->id;
			} else if (dynamic_cast<const CFeature*>(wtgt) != nullptr) {
				liveType = 'f'; liveID = wtgt->id;
			} else if (dynamic_cast<const CWeaponProjectile*>(wtgt) != nullptr) {
				liveType = 'p'; liveID = wtgt->id;
			}

			const bool targetEqual =
				(rows.targetType[id] == liveType) &&
				(rows.targetID[id] == liveID || liveType == 'g') &&
				(BitEqual(rows.targetPos[id], livePos) || liveType != 'g');

			if (Bump(fields[P_TARGET], targetEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:target snapType=%d liveType=%d",
					gs->frameNum, id, int(rows.targetType[id]), int(liveType));
		}

		// masking input: the extraction-time positional-LOS answer per allyteam
		for (int at = 0; at < numAllyTeams; ++at) {
			const bool snapLos = (rows.inLosAll[at * slots + id] != 0);
			const bool liveLos = losHandler->InLos(p->pos, at);

			if (Bump(fields[P_INLOS], snapLos == liveLos))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:inLosAll[ally %d] snap=%d live=%d",
					gs->frameNum, id, at, int(snapLos), int(liveLos));
		}
	}

	// reverse validity: valid rows with no live projectile
	for (size_t id = 0; id < slots; ++id) {
		if (rows.valid[id] == 0 || liveSeen[id] != 0)
			continue;

		if (Bump(fields[P_VALIDITY], false))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:validity snap=1 live=0",
				gs->frameNum, int(id));
	}
}

void SnapshotDiffGate::CheckFeatureRows()
{
	const SimSnapshot::FeatureRows& rows = simSnapshot.ReadFeatures();
	const size_t slots = rows.MaxSlots();
	const int numAllyTeams = rows.numAllyTeams;

	// globals the feature visibility mirror depends on
	{
		const bool globalsEqual =
			(rows.featureVisibility == modInfo.featureVisibility) &&
			(rows.gaiaAllyTeam == std::max(0, teamHandler.GaiaAllyTeamID())) &&
			(numAllyTeams == teamHandler.ActiveAllyTeams());
		if (Bump(fields[FT_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=feat:globals mismatch (featVis snap=%d live=%d)",
				gs->frameNum, rows.featureVisibility, modInfo.featureVisibility);
	}

	std::vector<uint8_t> liveSeen(slots, 0);
	const auto& activeIDs = featureHandler.GetActiveFeatureIDs();

	for (const int id : activeIDs) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr)
			continue;

		if (static_cast<size_t>(id) < slots)
			liveSeen[id] = 1;

		const bool snapValid = rows.Valid(id);
		if (Bump(fields[FT_VALIDITY], snapValid)) {
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:validity snap=0 live=1",
				gs->frameNum, id);
			continue;
		}

		if (Bump(fields[FT_POS], BitEqual(rows.pos[id], f->pos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:pos snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.pos[id].x, rows.pos[id].y, rows.pos[id].z, f->pos.x, f->pos.y, f->pos.z);

		if (Bump(fields[FT_MIDPOS], BitEqual(rows.midPos[id], f->midPos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:midPos", gs->frameNum, id);
		if (Bump(fields[FT_AIMPOS], BitEqual(rows.aimPos[id], f->aimPos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:aimPos", gs->frameNum, id);

		if (Bump(fields[FT_RELMIDPOS], BitEqual(rows.relMidPos[id], static_cast<float3>(f->relMidPos))))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:relMidPos", gs->frameNum, id);

		if (Bump(fields[FT_RADIUS], BitEqual(rows.radius[id], f->radius)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:radius snap=%.9g live=%.9g",
				gs->frameNum, id, rows.radius[id], f->radius);

		if (Bump(fields[FT_ALLYTEAM], rows.allyTeam[id] == f->allyteam))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:allyTeam snap=%d live=%d",
				gs->frameNum, id, rows.allyTeam[id], f->allyteam);

		if (Bump(fields[FT_DEFID], rows.defID[id] == f->def->id))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:defID snap=%d live=%d",
				gs->frameNum, id, rows.defID[id], f->def->id);

		const bool flagsEqual =
			(int(rows.alwaysVisible[id]) == int(f->alwaysVisible)) &&
			(int(rows.noSelect[id]) == int(f->noSelect)) &&
			(int(rows.inVoid[id]) == int(f->IsInVoid()));
		if (Bump(fields[FT_FLAGS], flagsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:flags mismatch", gs->frameNum, id);

		if (Bump(fields[FT_SELVOL], ColVolEqual(rows.selVol[id], f->selectionVolume)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:selVol mismatch", gs->frameNum, id);

		// PR 27a feature rows (grouped counters, see the enum comments)
		if (Bump(fields[FT_TEAM], rows.team[id] == f->team))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:team snap=%d live=%d",
				gs->frameNum, id, rows.team[id], f->team);

		const bool scalarsEqual =
			BitEqual(rows.health[id], f->health) &&
			BitEqual(rows.resurrectProgress[id], f->resurrectProgress) &&
			BitEqual(rows.height[id], f->height) &&
			BitEqual(rows.mass[id], f->mass) &&
			(rows.heading[id] == int16_t(f->heading)) &&
			(rows.buildFacing[id] == int16_t(f->buildFacing));
		if (Bump(fields[FT_SCALARS], scalarsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:scalars mismatch", gs->frameNum, id);

		if (Bump(fields[FT_SPEED], BitEqual(rows.speed[id], f->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:speed", gs->frameNum, id);

		{
			const CMatrix44f& fm = f->GetTransformMatrixRef();
			const bool dirMatEqual =
				BitEqual(rows.matXdir[id], fm.GetX()) &&
				BitEqual(rows.matYdir[id], fm.GetY()) &&
				BitEqual(rows.matZdir[id], fm.GetZ());
			if (Bump(fields[FT_DIRMAT], dirMatEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:dirMatrix mismatch", gs->frameNum, id);
		}

		const bool resourcesEqual =
			BitEqual(rows.resources[id].metal, f->resources.metal) &&
			BitEqual(rows.resources[id].energy, f->resources.energy) &&
			BitEqual(rows.defResources[id].metal, f->defResources.metal) &&
			BitEqual(rows.defResources[id].energy, f->defResources.energy) &&
			BitEqual(rows.reclaimLeft[id], f->reclaimLeft) &&
			BitEqual(rows.reclaimTime[id], f->reclaimTime);
		if (Bump(fields[FT_RESOURCES], resourcesEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:resources mismatch", gs->frameNum, id);

		if (Bump(fields[FT_BLOCKINGBITS], rows.blockingBits[id] == PackBlockingBits(f)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:blockingBits snap=0x%02x live=0x%02x",
				gs->frameNum, id, int(rows.blockingBits[id]), int(PackBlockingBits(f)));

		if (Bump(fields[FT_RESURRECT], rows.resurrectDefID[id] == ((f->udef != nullptr) ? f->udef->id : -1)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:resurrect snap=%d live=%d",
				gs->frameNum, id, rows.resurrectDefID[id], (f->udef != nullptr) ? f->udef->id : -1);

		for (int at = 0; at < numAllyTeams; ++at) {
			const bool snapLos = (rows.inLosAll[at * slots + id] != 0);
			const bool liveLos = losHandler->InLos(f->pos, at);
			if (Bump(fields[FT_INLOS], snapLos == liveLos))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:inLosAll[ally %d] snap=%d live=%d",
					gs->frameNum, id, at, int(snapLos), int(liveLos));
		}
	}

	// reverse validity: valid rows with no live feature
	for (size_t id = 0; id < slots; ++id) {
		if (rows.valid[id] == 0 || liveSeen[id] != 0)
			continue;
		if (Bump(fields[FT_VALIDITY], false))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:validity snap=1 live=0",
				gs->frameNum, int(id));
	}
}

void SnapshotDiffGate::CheckTeamPlayerRows()
{
	// team/player boundary copy (PR 26): re-extracted unconditionally right
	// before this check, so every compare must trivially pass -- the pass
	// verifies the extraction copies every field the serving twins read
	const SimSnapshot::TeamRows& trows = simSnapshot.ReadTeams();
	const SimSnapshot::PlayerRows& prows = simSnapshot.ReadPlayers();

	{
		const bool globalsEqual =
			(trows.activeTeams == teamHandler.ActiveTeams()) &&
			(trows.activeAllyTeams == teamHandler.ActiveAllyTeams()) &&
			(trows.gaiaTeamID == teamHandler.GaiaTeamID()) &&
			(bool(trows.useLuaGaia) == bool(gs->useLuaGaia)) &&
			(bool(trows.gameOver) == (game != nullptr && game->IsGameOver()));
		if (Bump(fields[T_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=team:globals mismatch (activeTeams snap=%d live=%d)",
				gs->frameNum, trows.activeTeams, teamHandler.ActiveTeams());
	}

	for (int t = 0; t < trows.activeTeams && t < teamHandler.ActiveTeams(); ++t) {
		const CTeam* team = teamHandler.Team(t);

		const bool stateEqual =
			(trows.leader[t] == team->GetLeader()) &&
			(bool(trows.isDead[t]) == team->isDead) &&
			(bool(trows.hasAIs[t]) == skirmishAIHandler.HasSkirmishAIsInTeam(t)) &&
			(trows.allyTeam[t] == teamHandler.AllyTeam(t)) &&
			BitEqual(trows.incomeMultiplier[t], team->GetIncomeMultiplier()) &&
			(trows.numUnits[t] == static_cast<int32_t>(unitHandler.NumUnitsByTeam(t)));
		if (Bump(fields[T_STATE], stateEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:state mismatch", gs->frameNum, t);

		const auto packEqual = [](const SResourcePack& a, const SResourcePack& b) {
			return BitEqual(a.metal, b.metal) && BitEqual(a.energy, b.energy);
		};
		const bool resEqual =
			packEqual(trows.res[t], team->res) &&
			packEqual(trows.resStorage[t], team->resStorage) &&
			packEqual(trows.resPrevPull[t], team->resPrevPull) &&
			packEqual(trows.resPrevIncome[t], team->resPrevIncome) &&
			packEqual(trows.resPrevExpense[t], team->resPrevExpense) &&
			packEqual(trows.resShare[t], team->resShare) &&
			packEqual(trows.resPrevSent[t], team->resPrevSent) &&
			packEqual(trows.resPrevReceived[t], team->resPrevReceived) &&
			packEqual(trows.resPrevExcess[t], team->resPrevExcess);
		if (Bump(fields[T_RES], resEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:res mismatch (metal snap=%.9g live=%.9g)",
				gs->frameNum, t, trows.res[t].metal, team->res.metal);

		// TeamStatistics is #pragma pack(1): no padding, memcmp-safe
		if (Bump(fields[T_STATS], std::memcmp(&trows.currentStats[t], &team->GetCurrentStats(), sizeof(TeamStatistics)) == 0))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:stats mismatch", gs->frameNum, t);

		const bool colorEqual =
			(std::memcmp(trows.color[t].data(), team->color, 4) == 0) &&
			(std::memcmp(trows.origColor[t].data(), team->origColor, 4) == 0);
		if (Bump(fields[T_COLOR], colorEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:color mismatch", gs->frameNum, t);

		const bool stringsEqual =
			(trows.sideName[t] == team->GetSideName()) &&
			OptsEqual(trows.customOpts[t], team->GetAllValues());
		if (Bump(fields[T_STRINGS], stringsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:strings mismatch", gs->frameNum, t);
	}

	{
		const bool globalsEqual =
			(prows.activePlayers == static_cast<int32_t>(playerHandler.ActivePlayers())) &&
			(bool(prows.hostDemo) == gameSetup->hostDemo);
		if (Bump(fields[PL_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=player:globals mismatch (activePlayers snap=%d live=%d)",
				gs->frameNum, prows.activePlayers, int(playerHandler.ActivePlayers()));
	}

	for (int p = 0; p < prows.activePlayers && p < static_cast<int>(playerHandler.ActivePlayers()); ++p) {
		const CPlayer* player = playerHandler.Player(p);

		const bool infoEqual =
			(prows.name[p] == player->name) &&
			(prows.countryCode[p] == player->countryCode) &&
			(prows.rank[p] == player->rank) &&
			(bool(prows.isFromDemo[p]) == player->isFromDemo);
		if (Bump(fields[PL_INFO], infoEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:info mismatch", gs->frameNum, p);

		const bool stateEqual =
			(bool(prows.active[p]) == player->active) &&
			(bool(prows.spectator[p]) == player->spectator) &&
			(prows.team[p] == player->team) &&
			(bool(prows.desynced[p]) == player->desynced);
		if (Bump(fields[PL_STATE], stateEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:state mismatch", gs->frameNum, p);

		const bool netEqual =
			(prows.ping[p] == player->ping) &&
			BitEqual(prows.cpuUsage[p], player->cpuUsage);
		if (Bump(fields[PL_NET], netEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:net mismatch (ping snap=%d live=%d)",
				gs->frameNum, p, prows.ping[p], player->ping);

		if (Bump(fields[PL_OPTS], OptsEqual(prows.customOpts[p], player->GetAllValues())))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:opts mismatch", gs->frameNum, p);
	}
}

void SnapshotDiffGate::CheckGlobalRows()
{
	// global-scalar boundary copy (PR 27a): re-extracted unconditionally right
	// before this check, so every compare must trivially pass -- the pass
	// verifies the extraction copies every scalar the serving twins read
	const SimSnapshot::GlobalRows& grows = simSnapshot.ReadGlobals();

	if (Bump(fields[G_FRAME], grows.luaSimFrame == gs->GetLuaSimFrame()))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:frame mismatch (snap=%d live=%d)",
			gs->frameNum, grows.luaSimFrame, gs->GetLuaSimFrame());

	const bool speedEqual =
		BitEqual(grows.wantedSpeedFactor, gs->wantedSpeedFactor) &&
		BitEqual(grows.speedFactor, gs->speedFactor) &&
		(bool(grows.paused) == gs->paused);
	if (Bump(fields[G_SPEED], speedEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:speed mismatch", gs->frameNum);

	const bool flagsEqual =
		(bool(grows.cheatEnabled) == gs->cheatEnabled) &&
		(grows.godMode == gs->godMode) &&
		(bool(grows.editDefsEnabled) == gs->editDefsEnabled) &&
		(bool(grows.noHelperAIs) == gs->noHelperAIs) &&
		(bool(grows.defsNoCost) == (unitDefHandler != nullptr && unitDefHandler->GetNoCost())) &&
		(bool(grows.doneLoading) == (game != nullptr && game->IsDoneLoading())) &&
		(bool(grows.savedGame) == (game != nullptr && game->IsSavedGame())) &&
		(bool(grows.clientPaused) == (game != nullptr && game->IsClientPaused()));
	if (Bump(fields[G_FLAGS], flagsEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:flags mismatch", gs->frameNum);

	const bool windEqual =
		BitEqual(grows.windVec.x, envResHandler.GetCurrentWindVec().x) &&
		BitEqual(grows.windVec.y, envResHandler.GetCurrentWindVec().y) &&
		BitEqual(grows.windVec.z, envResHandler.GetCurrentWindVec().z) &&
		BitEqual(grows.windDir.x, envResHandler.GetCurrentWindDir().x) &&
		BitEqual(grows.windDir.y, envResHandler.GetCurrentWindDir().y) &&
		BitEqual(grows.windDir.z, envResHandler.GetCurrentWindDir().z) &&
		BitEqual(grows.windStrength, envResHandler.GetCurrentWindStrength());
	if (Bump(fields[G_WIND], windEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:wind mismatch", gs->frameNum);

	const bool heightsEqual =
		BitEqual(grows.initMinHeight, readMap->GetInitMinHeight()) &&
		BitEqual(grows.initMaxHeight, readMap->GetInitMaxHeight()) &&
		BitEqual(grows.currMinHeight, readMap->GetCurrMinHeight()) &&
		BitEqual(grows.currMaxHeight, readMap->GetCurrMaxHeight());
	if (Bump(fields[G_HEIGHTS], heightsEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:heights mismatch", gs->frameNum);

	bool losEqual = (grows.numAllyTeams == teamHandler.ActiveAllyTeams());
	for (int at = 0; losEqual && at < grows.numAllyTeams; ++at)
		losEqual = (bool(grows.globalLos[at]) == losHandler->GetGlobalLOS(at));
	if (Bump(fields[G_GLOBALLOS], losEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:globalLos mismatch", gs->frameNum);
}

// PR 28: the DrawMapMirrors compare pass. Drained at the barrier (before the
// snapshot publish) and nothing runs sim between there and here, so every
// mirror must bit-match the live source; a mismatch means a missed choke-point
// dirty mark (the deterministic detector the mirror-verification rule asks for).
void SnapshotDiffGate::CheckMapMirrors()
{
	if (!drawMapMirrors.Ready() || losHandler == nullptr || readMap == nullptr || mapInfo == nullptr)
		return;

	// --- LOS layers (per losType, per allyTeam whole-map memcmp) ---
	const ILosType* lts[DrawMapMirrors::LOS_MIRROR_TYPE_COUNT];
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_LOS]          = &losHandler->los;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_AIRLOS]       = &losHandler->airLos;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_RADAR]        = &losHandler->radar;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SONAR]        = &losHandler->sonar;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_JAMMER]       = &losHandler->jammer;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SEISMIC]      = &losHandler->seismic;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SONAR_JAMMER] = &losHandler->sonarJammer;

	for (int t = 0; t < DrawMapMirrors::LOS_MIRROR_TYPE_COUNT; ++t) {
		const int nAlly = static_cast<int>(lts[t]->losMaps.size());
		for (int at = 0; at < nAlly; ++at) {
			const std::vector<uint16_t>* mm = drawMapMirrors.LosMap(t, at);
			const auto& live = lts[t]->losMaps[at].GetLosMap();

			bool eq = (mm != nullptr) && (mm->size() == live.size()) &&
				(mm->empty() || std::memcmp(mm->data(), live.data(), mm->size() * sizeof(uint16_t)) == 0);
			if (Bump(fields[MM_LOS], eq))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:los mismatch (type=%d ally=%d)",
					gs->frameNum, t, at);
		}
	}

	// --- terrain-type table ---
	bool ttEqual = (drawMapMirrors.TerrainTypeCount() == CMapInfo::NUM_TERRAIN_TYPES);
	for (int i = 0; ttEqual && i < CMapInfo::NUM_TERRAIN_TYPES; ++i) {
		const DrawMapMirrors::TerrainType& m = drawMapMirrors.TerrainTypeAt(i);
		const CMapInfo::TerrainType& l = mapInfo->terrainTypes[i];
		ttEqual =
			(m.name == l.name) &&
			BitEqual(m.hardness, l.hardness) &&
			BitEqual(m.tankSpeed, l.tankSpeed) &&
			BitEqual(m.kbotSpeed, l.kbotSpeed) &&
			BitEqual(m.hoverSpeed, l.hoverSpeed) &&
			BitEqual(m.shipSpeed, l.shipSpeed) &&
			(m.receiveTracks == l.receiveTracks);
	}
	if (Bump(fields[MM_TERRAINTYPES], ttEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:terrainTypes mismatch", gs->frameNum);

	// --- smooth-height mesh ---
	{
		const std::vector<float>& mm = drawMapMirrors.SmoothMeshData();
		const size_t n = static_cast<size_t>(smoothGround.GetMaxX()) * static_cast<size_t>(smoothGround.GetMaxY());
		const bool eq = (mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), smoothGround.GetMeshData(), n * sizeof(float)) == 0);
		if (Bump(fields[MM_SMOOTHMESH], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:smoothMesh mismatch", gs->frameNum);
	}

	// --- original heightmap ---
	{
		const std::vector<float>& mm = drawMapMirrors.OrigHeightMap();
		const size_t n = static_cast<size_t>(mapDims.mapxp1) * static_cast<size_t>(mapDims.mapyp1);
		const bool eq = (mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), readMap->GetOriginalHeightMapSynced(), n * sizeof(float)) == 0);
		if (Bump(fields[MM_ORIGHEIGHT], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:origHeight mismatch", gs->frameNum);
	}

	// --- radar-error scalars ---
	{
		bool eq =
			(drawMapMirrors.NumAllyTeams() == teamHandler.ActiveAllyTeams()) &&
			BitEqual(drawMapMirrors.BaseRadarErrorSize(), losHandler->GetBaseRadarErrorSize()) &&
			BitEqual(drawMapMirrors.BaseRadarErrorMult(), losHandler->GetBaseRadarErrorMult());
		for (int at = 0; eq && at < drawMapMirrors.NumAllyTeams(); ++at)
			eq = BitEqual(drawMapMirrors.AllyTeamRadarErrorSize(at), losHandler->GetAllyTeamRadarErrorSize(at));
		if (Bump(fields[MM_RADARERR], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:radarError mismatch", gs->frameNum);
	}
}

// sim|draw PR 30 mirror-verification: the command-queue/cmd-desc serving cache is
// rebuilt (LuaSnapshotServe::RefreshCommandQueues) right before this check at the
// same boundary, so every compare must trivially pass -- a missed dirty-mark
// (queue version / cmdDescVersion / worker re-decode) becomes a deterministic
// gate failure. The per-slot compare lives in LuaSnapshotServe (where the cache
// is a file-static and the sim includes are already present, and where it binds
// live queues through const refs so the version-bumping accessors are never
// invoked); this pass just drives it over every unit id and tallies the fields.
void SnapshotDiffGate::CheckCmdQueueRows()
{
	const size_t maxUnits = unitHandler.MaxUnits();

	for (size_t id = 0; id < maxUnits; ++id) {
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits
		const bool liveValid = (u != nullptr);

		const LuaSnapshotServe::CmdQueueCompareResult r = LuaSnapshotServe::CompareCmdQueueSlot(int(id), u);

		if (Bump(fields[CQ_PRESENCE], r.present == liveValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:presence snap=%d live=%d",
				gs->frameNum, int(id), int(r.present), int(liveValid));

		if (!r.present || !liveValid)
			continue;

		if (Bump(fields[CQ_QUEUE], r.queueOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:queue mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_DESCS], r.descsOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:descs mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_WORKER], r.workerOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:worker mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_FACTORY], r.factoryOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:factory mismatch", gs->frameNum, int(id));
	}
}
