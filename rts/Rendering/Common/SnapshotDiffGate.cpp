/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotDiffGate.h"

#include <cstring>
#include <vector>

#include "SimSnapshot.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
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
	"proj:validity",
	"proj:pos",
	"proj:speed",
	"proj:allyTeam",
	"proj:ownerID",
	"proj:isWeapon",
	"proj:weaponDefID",
	"proj:target",
	"proj:inLosAll",
	"feat:validity",
	"feat:pos",
	"feat:midPos",
	"feat:relMidPos",
	"feat:radius",
	"feat:allyTeam",
	"feat:defID",
	"feat:flags",
	"feat:selVol",
	"feat:inLosAll",
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
