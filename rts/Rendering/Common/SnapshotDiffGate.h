/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <map>
#include <string>

/**
 * @brief SnapshotDiffGate -- TEST-ONLY snapshot-vs-live differential verifier
 *
 * PR 17 of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md,
 * §E / §E.1 and the "Lua API compatibility tiers" note), extended by PR 18 when
 * the positions/status callout family became snapshot-served. While armed, for
 * every value the snapshot serves it ALSO computes the live-sim answer and diffs
 * them.
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
 * Three surfaces, one shared reporting/counter path:
 *
 *  1. Field-level pass -- CheckBoundary(), driven from CGame::Draw right after
 *     simSnapshot.Update(). Walks every unit id and verifies:
 *       - validity both ways: a valid snapshot row iff a live unit occupies that
 *         id (unitHandler.GetUnit(id) != nullptr). units[id] is set/cleared in
 *         lockstep with active-unit-list membership (CUnitHandler::InsertActiveUnit
 *         / DeleteUnit), which is exactly what SimSnapshot::Extract iterates, so
 *         at the same frame the two sets are identical -- dying-but-not-deleted
 *         and recycled ids included (SimSnapshot.h validity contract).
 *       - every extracted field for valid rows, including the per-allyteam
 *         losStatusAll/posErrorBits stride rows and the per-buffer global block
 *         (alliance matrix, radar-error scalars).
 *
 *  2. Masked-value sweep (PR 18, the masking-aware layer) -- part of
 *     CheckBoundary: for every valid unit and every allyteam POV (plus the
 *     invalid-allyteam branch), diff UnitRows::ErrorVector -- the exact function
 *     the Lua serving twins add to positions -- against live
 *     CUnit::GetErrorVector. This verifies the masking math for ALL POVs every
 *     boundary, independent of which POV the local handles happen to run.
 *
 *  3. Serving-path comparator -- CountCallout(), fed by LuaSnapshotServe::Route:
 *     while armed every redirected callout runs BOTH real paths (live body and
 *     snapshot twin) and bit-compares the actual Lua return slots, so the
 *     composed gate+mask+value behavior is verified per invocation at the
 *     handle's real POV. Counter key = callout name.
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

	/// field-level pass + masked-value sweep over the whole snapshot surface;
	/// call right after simSnapshot.Update() in CGame::Draw. No-op unless armed.
	void CheckBoundary();

	/// serving-path comparator sink (see LuaSnapshotServe::Route): `detail` is
	/// logged on the first kMaxLogged mismatches per callout
	void CountCallout(const char* callout, bool equal, const char* detail);

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

	// fixed field-pass counters (order = report order)
	enum {
		F_VALIDITY = 0,
		F_POS,
		F_MIDPOS,
		F_AIMPOS,
		F_SPEED,
		F_HEALTH,
		F_MAXHEALTH,
		F_PARALYZE,
		F_CAPTURE,
		F_TEAM,
		F_ALLYTEAM,
		F_DEFID,
		F_BUILDPROGRESS,
		F_BEINGBUILT,
		F_STUNNED,
		F_RADIUS,
		F_RELMIDPOS,
		F_FRONTDIR,
		F_UPDIR,
		F_RIGHTDIR,
		F_POSERRORVEC,
		F_LEAVESGHOST,
		F_LOSSTATUS,
		F_POSERRORBIT,
		F_GLOBALS,
		F_MASKEDERRVEC,
		// picking gates (PR 25)
		F_NOSELECT,
		F_INVOID,
		F_SELVOL,
		F_INRADAR,
		// PR 27a unit rows
		F_STATEFLAGS,   // isDead / neutral / activated / isCloaked / armoredState
		F_HEADINGFACING,// heading / buildFacing
		F_UNITSCALARS,  // height / mass / maxRange / seismicSignature / armoredMultiple / experience / limExperience
		F_UNITECO,      // resourcesMake/Use, harvested/harvestStorage, cost, buildTime
		F_SENSORRADII,  // the seven per-unit sensor radii
		F_UNITMISCINTS, // selfDCountdown / moveDefID
		F_BLOCKINGBITS, // GetSolidObjectBlocking's seven booleans
		// projectile rows (second family)
		P_VALIDITY,
		P_POS,
		P_SPEED,
		P_ALLYTEAM,
		P_OWNERID,
		P_ISWEAPON,
		P_WDEFID,
		P_TARGET,
		P_INLOS,
		// PR 27a projectile rows
		P_DIR,
		P_GRAVITY,
		P_TEAMID,
		P_TTLFLAGS,     // ttl / intercepted / isPiece
		// feature rows (PR 25 family)
		FT_VALIDITY,
		FT_POS,
		FT_MIDPOS,
		FT_AIMPOS,
		FT_RELMIDPOS,
		FT_RADIUS,
		FT_ALLYTEAM,
		FT_DEFID,
		FT_FLAGS,       // alwaysVisible / noSelect / inVoid
		FT_SELVOL,
		FT_INLOS,
		// PR 27a feature rows
		FT_TEAM,
		FT_SCALARS,     // health / resurrectProgress / height / mass / heading / buildFacing
		FT_SPEED,
		FT_DIRMAT,      // transMatrix direction columns
		FT_RESOURCES,   // resources / defResources / reclaimLeft / reclaimTime
		FT_BLOCKINGBITS,
		FT_RESURRECT,   // resurrectDefID
		FT_GLOBALS,     // featureVisibility / gaiaAllyTeam
		// team/player boundary copy (PR 26, section E.3)
		T_GLOBALS,      // activeTeams / activeAllyTeams / gaiaTeamID / useLuaGaia / gameOver
		T_STATE,        // leader / isDead / hasAIs / allyTeam / incomeMultiplier / numUnits
		T_RES,          // the 9 GetTeamResources packs
		T_STATS,        // current TeamStatistics
		T_COLOR,        // color / origColor
		T_STRINGS,      // sideName / customOpts
		PL_GLOBALS,     // activePlayers / hostDemo
		PL_INFO,        // name / countryCode / rank / isFromDemo
		PL_STATE,       // active / spectator / team / desynced
		PL_NET,         // ping / cpuUsage
		PL_OPTS,        // customOpts
		// global-scalar boundary copy (PR 27a)
		G_FRAME,        // luaSimFrame
		G_SPEED,        // wantedSpeedFactor / speedFactor / paused
		G_FLAGS,        // cheat/god/editDefs/noHelperAIs/noCost + game state flags
		G_WIND,         // windVec / windDir / windStrength
		G_HEIGHTS,      // init/curr ground extremes
		G_GLOBALLOS,    // per-allyteam globalLOS
		F_COUNT
	};

	// counter bump + log-gate: returns true iff the caller should emit a
	// LOG_L(L_ERROR) line for this mismatch (mismatch and under the per-field cap)
	bool Bump(FieldCounter& fc, bool equal);

	// projectile-row / feature-row / team+player / global halves of the field
	// pass (from CheckBoundary)
	void CheckProjectileRows();
	void CheckFeatureRows();
	void CheckTeamPlayerRows();
	void CheckGlobalRows();

	void Report(const char* reason) const;
	void ResetCounters();

private:
	bool armed = false;

	uint64_t boundaryChecks = 0; // CheckBoundary() invocations while armed

	FieldCounter fields[F_COUNT];

	// serving-path counters, keyed by callout name (LuaSnapshotServe::Route)
	std::map<std::string, FieldCounter> callouts;
};

extern SnapshotDiffGate snapshotDiffGate;
