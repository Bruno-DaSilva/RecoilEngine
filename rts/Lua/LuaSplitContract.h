/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <functional>

struct lua_State;

/**
 * @brief LuaSplitContract -- the draw-thread Lua contract, enforceable pre-split
 *
 * PR 27a of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md
 * Wave 4): the flip-time mechanics for the Lua callout surface, runnable while
 * still single-threaded (the "pretend-split" dress rehearsal for 27b).
 *
 * Under the future split, everything CGame::Draw runs executes on the draw
 * thread while the sim thread advances frame N+1; the synced lua_State and all
 * live sim state belong to the sim thread outside the SimDrawBarrier pause
 * window. This module owns the enforcement machinery for that contract:
 *
 *  - THE DRAW WINDOW: a scoped bracket CGame::Draw opens right after
 *    SimDrawBarrier() returns. Inside it, unsynced-handle Lua execution is
 *    "draw-thread context". The barrier itself (and its sanctioned Render*
 *    event dispatches) runs before the window opens -- under the real split
 *    the sim is paused there, so live reads are legal in that bracket.
 *
 *  - LIVE-READ GATING (DenyLiveRead): every sim-reading callout that is not
 *    snapshot-served checks in at its shared parse helper or entry body.
 *    Dispositions, per callout name (the classification tables live in
 *    LuaSplitContract.cpp and mirror the research doc):
 *      * sanctioned-live -- the enumerated 27b-blocker families (placement
 *        tests, positional LOS, command queues, weapon state, ...): the live
 *        read proceeds, the trip is counted per name. These keep every game
 *        working through 27a; the inventory is the demand data for serving
 *        them, and 27b may not enable until the list is served or decided.
 *      * denied -- with SplitDrawContractStrictTail set, every non-sanctioned
 *        trip is refused: the callout returns its documented "no such object"
 *        nil shape, deterministically, with a once-per-name dev warning. This
 *        is the decision-5 tail contract; strict mode measures end-to-end
 *        what a hard flip would nil.
 *
 *  - CROSS-HOP BANS (ErrorOnCrossHop): unsynced->synced lua_State touches
 *    (the SYNCED proxy table, Spring.GetSyncedGCInfo) hard-error from the
 *    draw window under the flag -- they read/mutate the state the sim thread
 *    will own, and no snapshot can serve them.
 *
 *  - BOUNDARY-APPLY QUEUE (QueueBoundaryApply/DrainBoundaryApplies): the
 *    LuaUnsyncedCtrl direct-sim-poke class (SetUnitNoDraw-family last-wins
 *    scalars) enqueues under the flag and applies inside the next
 *    SimDrawBarrier -- the pause window where sim state is mutable. Ops
 *    capture ids, never pointers, and re-resolve at apply time (an object can
 *    die between queue and drain).
 *
 * Flag off (the default), every predicate is a single cached-bool load and
 * behavior is bit-identical to the pre-27a tree.
 */
namespace LuaSplitContract {
	/// the master enforcement flag (config var SplitDrawContract, cached)
	bool Enabled();

	/// true inside CGame::Draw's post-barrier bracket on this thread
	bool InDrawWindow();

	/// the full predicate: flag on, in the draw window, not inside a
	/// sanctioned live-exception bracket, and L is an unsynced handle
	bool Enforced(lua_State* L);

	/// live-sim-read gate for callout `caller` (its __func__). Returns true
	/// when the callout must serve its "no such object" nil shape instead of
	/// reading live sim state (strict tail mode, non-sanctioned callout).
	/// Counts and warn-logs per the header comment. False = proceed.
	bool DenyLiveRead(lua_State* L, const char* caller);

	/// hard-error (luaL_error, does not return) when `Enforced(L)`:
	/// unsynced->synced lua_State cross-hops have no serveable fallback
	void ErrorOnCrossHop(lua_State* L, const char* what);

	/// LuaUnsyncedCtrl direct-sim-poke gate: under enforcement, queue `op`
	/// for the next SimDrawBarrier and return true; otherwise return false
	/// (caller applies immediately, exactly the pre-27a behavior). Also
	/// counts per name. `op` must capture ids, not object pointers.
	bool QueueBoundaryApply(lua_State* L, const char* caller, std::function<void()>&& op);

	/// inventory-only counter for the ctrl pokes that stay synchronous in
	/// 27a (selection/group family, GiveOrder, SendCommands, ...): they are
	/// classified with a 27b plan (draw-owned handler / op capture) instead
	/// of a deferral this PR could do safely. No behavior change; the trip
	/// shows up in the dump so the classification stays measured.
	void CountSanctionedPoke(lua_State* L, const char* caller);

	/// PR 44b: ENGINE-side boundary apply -- same queue/drain as the Lua ctrl
	/// pokes but with no lua_State/enforcement gate (the caller gates on the
	/// running split). Used by draw-context engine code whose sim writes must
	/// land on the sim thread (CWaitCommandsAI's queued-command wait re-key).
	/// `name` shows in the inventory dump (static storage). Ops capture ids,
	/// never pointers, and re-resolve at apply time.
	void QueueEngineBoundaryApply(const char* name, std::function<void()>&& op);

	/// apply everything queued since the last barrier; called from
	/// CGame::SimDrawBarrier (the pause window). No-op when empty. Returns
	/// the number of applied ops: a nonzero count mutated sim state OUTSIDE
	/// any sim frame, so the caller must poke the snapshot's due-check
	/// (SimSnapshot::MarkMutatedOutsideFrame) or the rows serve stale values
	/// until the next sim frame (found by the windowed diff gate: queued
	/// SetUnitNoSelect vs the noSelect row).
	size_t DrainBoundaryApplies();

	/// log the per-callout trip inventory (sanctioned live reads, denials,
	/// queued pokes); `reason` tags the dump line. Used by the
	/// /splitcontractdump command and the end-of-game dump.
	void DumpInventory(const char* reason);

	/// game teardown: dump the inventory and reset all counters/queues
	void Clear();

	// the CGame::Draw bracket (see the header comment); depth-counted TLS
	struct ScopedDrawWindow {
		ScopedDrawWindow();
		~ScopedDrawWindow();
	};

	// sanctioned live-execution bracket: suspends enforcement on this thread
	// (used by LuaSnapshotServe::Route around the armed dual-run's live leg,
	// which deliberately runs live bodies from draw context to bit-compare)
	struct ScopedLiveException {
		ScopedLiveException();
		~ScopedLiveException();
	};

	// PR 38j: reverted PR 38h's ScopedContractReassert. Blanket-re-asserting
	// enforcement for the whole deferred event dispatch forced EVERY read the
	// handler makes onto the strict snapshot path, nil-ing the handler's OTHER
	// (neither-snapshot-served-nor-sanctioned-live) reads -- surfacing a 4th
	// widget error (cmd_stop_selfd) that 38h's own review predicted. The
	// event-time override is now consulted at the TOP of the specific affected
	// callouts instead (LuaSyncedRead GetUnitCommands/CommandCount/CurrentCommand
	// + GetUnitPosition/Direction), independent of the barrier's live exception,
	// so only the event-changed reads are presented event-time and every other
	// read the deferred handler makes stays on the working live path.
}
