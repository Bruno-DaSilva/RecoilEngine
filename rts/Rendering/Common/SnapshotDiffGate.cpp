/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotDiffGate.h"

#include <cstring>

#include "SimSnapshot.h"
#include "Sim/Misc/GlobalSynced.h"
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
	for (FieldCounter* fc : {
		&cValidity, &cPos, &cSpeed, &cHealth, &cMaxHealth, &cTeam,
		&cAllyTeam, &cDefID, &cBuildProgress, &cLosStatus, &cCalloutPos }) {
		fc->checked = 0;
		fc->mismatched = 0;
		fc->logged = 0;
	}
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
	for (const FieldCounter* fc : {
		&cValidity, &cPos, &cSpeed, &cHealth, &cMaxHealth, &cTeam,
		&cAllyTeam, &cDefID, &cBuildProgress, &cLosStatus, &cCalloutPos })
		totalMismatch += fc->mismatched;

	LOG("[SnapshotDiffGate] ===== report (%s) : %s =====",
		reason, (totalMismatch == 0) ? "PASS (0 mismatches)" : "FAIL");
	LOG("[SnapshotDiffGate] boundary checks=%llu, total mismatches=%llu",
		(unsigned long long)boundaryChecks, (unsigned long long)totalMismatch);

	for (const FieldCounter* fc : {
		&cValidity, &cPos, &cSpeed, &cHealth, &cMaxHealth, &cTeam,
		&cAllyTeam, &cDefID, &cBuildProgress, &cLosStatus, &cCalloutPos }) {
		LOG("[SnapshotDiffGate]   %-24s checked=%-14llu mismatched=%llu",
			fc->name, (unsigned long long)fc->checked, (unsigned long long)fc->mismatched);
	}
}


void SnapshotDiffGate::CheckCalloutUnitPosition(int unitID, const float3& liveBasePos)
{
	if (!armed)
		return;

	// the exact accessor PR 18 will call to serve the value; the invalid-id
	// default is part of the SimSnapshot stale/nil contract
	const float3 served = simSnapshot.Read().Pos(unitID);

	if (Bump(cCalloutPos, BitEqual(served, liveBasePos)))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=callout:GetUnitPosition "
			"snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
			gs->frameNum, unitID,
			served.x, served.y, served.z,
			liveBasePos.x, liveBasePos.y, liveBasePos.z);
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

	const int viewAllyTeam = rows.viewAllyTeam;
	const bool losRowValid = (viewAllyTeam >= 0);

	const size_t n = rows.valid.size();
	for (size_t i = 0; i < n; ++i) {
		const int id = static_cast<int>(i);
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits == n

		const bool snapValid = (rows.valid[i] != 0);
		const bool liveValid = (u != nullptr);

		if (Bump(cValidity, snapValid == liveValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=validity snap=%d live=%d",
				gs->frameNum, id, int(snapValid), int(liveValid));

		// field comparisons only make sense where both agree the row exists;
		// a one-sided validity mismatch was already reported above
		if (!snapValid || !liveValid)
			continue;

		if (Bump(cPos, BitEqual(rows.pos[i], u->pos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=pos snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.pos[i].x, rows.pos[i].y, rows.pos[i].z, u->pos.x, u->pos.y, u->pos.z);

		if (Bump(cSpeed, BitEqual(rows.speed[i], u->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=speed snap=(%.9g,%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.speed[i].x, rows.speed[i].y, rows.speed[i].z, rows.speed[i].w,
				u->speed.x, u->speed.y, u->speed.z, u->speed.w);

		if (Bump(cHealth, BitEqual(rows.health[i], u->health)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=health snap=%.9g live=%.9g",
				gs->frameNum, id, rows.health[i], u->health);

		if (Bump(cMaxHealth, BitEqual(rows.maxHealth[i], u->maxHealth)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=maxHealth snap=%.9g live=%.9g",
				gs->frameNum, id, rows.maxHealth[i], u->maxHealth);

		if (Bump(cTeam, rows.team[i] == static_cast<uint8_t>(u->team)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=team snap=%d live=%d",
				gs->frameNum, id, int(rows.team[i]), u->team);

		if (Bump(cAllyTeam, rows.allyTeam[i] == static_cast<uint8_t>(u->allyteam)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=allyTeam snap=%d live=%d",
				gs->frameNum, id, int(rows.allyTeam[i]), u->allyteam);

		if (Bump(cDefID, rows.defID[i] == u->unitDef->id))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=defID snap=%d live=%d",
				gs->frameNum, id, rows.defID[i], u->unitDef->id);

		if (Bump(cBuildProgress, BitEqual(rows.buildProgress[i], u->buildProgress)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=buildProgress snap=%.9g live=%.9g",
				gs->frameNum, id, rows.buildProgress[i], u->buildProgress);

		if (losRowValid) {
			const uint8_t live = u->losStatus[viewAllyTeam];
			if (Bump(cLosStatus, rows.losStatus[i] == live))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=losStatus[ally %d] snap=0x%02x live=0x%02x",
					gs->frameNum, id, viewAllyTeam, rows.losStatus[i], live);
		}

		// proof-of-use of the callout comparator plumbing PR 18 plugs into:
		// exercise the GetUnitPosition-shaped read with the live base position
		CheckCalloutUnitPosition(id, u->pos);
	}
}
