/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotDiffGate.h"

#include <cstring>

#include "SimSnapshot.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
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
};


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
		checkF3(F_RELMIDPOS, rows.relMidPos[i], u->relMidPos);
		checkF3(F_FRONTDIR, rows.frontdir[i], u->frontdir);
		checkF3(F_UPDIR, rows.updir[i], u->updir);
		checkF3(F_RIGHTDIR, rows.rightdir[i], u->rightdir);
		checkF3(F_POSERRORVEC, rows.posErrorVector[i], u->posErrorVector);
		checkI(F_LEAVESGHOST, rows.leavesGhost[i], int(u->leavesGhost));

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
			}

			const float3 snapErr = rows.ErrorVector(id, at);
			const float3 liveErr = u->GetErrorVector(at);

			if (Bump(fields[F_MASKEDERRVEC], BitEqual(snapErr, liveErr)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=maskedErrorVec[ally %d] snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
					gs->frameNum, id, at, snapErr.x, snapErr.y, snapErr.z, liveErr.x, liveErr.y, liveErr.z);
		}
	}
}
