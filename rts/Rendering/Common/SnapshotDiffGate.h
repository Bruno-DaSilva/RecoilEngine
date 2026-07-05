/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

#include "System/float3.h"

/**
 * @brief SnapshotDiffGate -- TEST-ONLY snapshot-vs-live differential verifier
 *
 * PR 17 of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md,
 * §E / §E.1 and the "Lua API compatibility tiers" note). PR 18 will redirect the
 * hot Lua callout families (Spring.GetUnitPosition / GetUnitViewPosition /
 * IsUnitVisible ...) to read the extracted SimSnapshot (PR 15) instead of live
 * sim objects when they are called from draw context. This gate is the verifier
 * that lets each converted family land pre-verified: while armed, for every value
 * the snapshot would serve it ALSO computes the live-sim answer and diffs them.
 *
 * The invariant is exact equality, no tolerance: by construction the snapshot and
 * the live sim describe the SAME completed sim frame (extraction happens in
 * CGame::Draw right after the render-event drain; in today's single-threaded tree
 * the sim never advances between extraction and any draw-side read). So any
 * mismatch is a real contract violation -- a torn extraction, a missed field, a
 * stale-row / validity bug, or a masking mistake.
 *
 * Project convention: diagnostic logs over asserts (never crash a live-gameplay
 * run) -- mismatches are reported via LOG_L(L_ERROR, ...) and counted; the run
 * keeps going. Zero cost when unarmed (a single branch at each hook site).
 *
 * Two surfaces, one shared reporting/counter path:
 *
 *  1. Field-level pass -- CheckBoundary(), driven from CGame::Draw right after
 *     simSnapshot.Update(). Walks every unit id and verifies:
 *       - validity both ways: a valid snapshot row iff a live unit occupies that
 *         id (unitHandler.GetUnit(id) != nullptr). units[id] is set/cleared in
 *         lockstep with active-unit-list membership (CUnitHandler::InsertActiveUnit
 *         / DeleteUnit), which is exactly what SimSnapshot::Extract iterates, so
 *         at the same frame the two sets are identical -- dying-but-not-deleted
 *         and recycled ids included (SimSnapshot.h validity contract).
 *       - every v1 field for valid rows: pos, speed, health, maxHealth, team,
 *         allyTeam, defID, buildProgress, and the losStatus row against
 *         unit->losStatus[snapshot viewAllyTeam].
 *
 *  2. Callout-level comparator -- CheckCalloutUnitPosition() and peers. This is
 *     the plug PR 18 fills: a converted callout family calls the comparator with
 *     the live value it would otherwise have returned, and the gate diffs it
 *     against the value the snapshot accessor serves, routing through the same
 *     counters/logging. Implemented now for the positions family as proof-of-use,
 *     driven from CheckBoundary itself (this PR redirects no Lua callout -- it
 *     only builds and exercises the comparison plumbing). The comparator takes the
 *     un-errored base position (o->pos, what SimSnapshot stores and what
 *     LuaSyncedRead::GetUnitPosition pushes before GetLuaErrorVector); the LOS/
 *     errorVec masking layer is settled separately at conversion time (see the
 *     losStatus masking note in SimSnapshot.h).
 *
 * Reporting: per-field checked/mismatched counters, dumped on /snapshotdiffgate
 * dump, on disarm, and at game end while armed (FlushPartial). Pass criterion:
 * zero mismatches over a full replay.
 */
class SnapshotDiffGate
{
public:
	/// begin verifying at each boundary; resets counters
	void Arm();
	/// report totals and stop verifying
	void Disarm();
	/// report running totals without stopping
	void Dump() const;
	/// game teardown while armed: report whatever was collected
	void FlushPartial();

	bool Armed() const { return armed; }

	/// field-level pass over the whole snapshot surface; call right after
	/// simSnapshot.Update() in CGame::Draw. No-op unless armed.
	void CheckBoundary();

	/// callout-level comparator (the PR 18 plug): diff the snapshot-served base
	/// position against the live value a converted GetUnitPosition-family callout
	/// would return (o->pos, pre-errorVec). No-op unless armed.
	void CheckCalloutUnitPosition(int unitID, const float3& liveBasePos);

private:
	// one field's running tally; kMaxLogged caps the LOG_L spam per field per
	// run while the counters keep the true totals
	struct FieldCounter {
		const char* name = "";
		uint64_t checked = 0;
		uint64_t mismatched = 0;
		uint64_t logged = 0;
	};
	static constexpr uint64_t kMaxLogged = 32;

	// counter bump + log-gate: returns true iff the caller should emit a
	// LOG_L(L_ERROR) line for this mismatch (mismatch and under the per-field cap)
	bool Bump(FieldCounter& fc, bool equal);

	void Report(const char* reason) const;
	void ResetCounters();

private:
	bool armed = false;

	uint64_t boundaryChecks = 0; // CheckBoundary() invocations while armed

	FieldCounter cValidity{"validity"};
	FieldCounter cPos{"pos"};
	FieldCounter cSpeed{"speed"};
	FieldCounter cHealth{"health"};
	FieldCounter cMaxHealth{"maxHealth"};
	FieldCounter cTeam{"team"};
	FieldCounter cAllyTeam{"allyTeam"};
	FieldCounter cDefID{"defID"};
	FieldCounter cBuildProgress{"buildProgress"};
	FieldCounter cLosStatus{"losStatus"};

	FieldCounter cCalloutPos{"callout:GetUnitPosition"};
};

extern SnapshotDiffGate snapshotDiffGate;
