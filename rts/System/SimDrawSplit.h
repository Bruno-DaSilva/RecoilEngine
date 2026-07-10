/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * @brief SimDrawSplit -- the sim|draw thread-split feature flag + sim-phase bracket
 *
 * PR 27b of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md
 * Wave 4). One config var, `SimDrawSplit` (default 0), gates the whole split:
 *
 *   0 (default) -- single-threaded, bit-identical legacy behavior. Every
 *     predicate below is a cached-bool load returning false; no new locks are
 *     taken anywhere (the split's lock additions are WrappedSync-style
 *     toggles keyed on this flag).
 *   1 -- the split is active: the sim thread runs ClientReadNet -> SimFrames
 *     (once the thread-spawn commit is in; with only the deferral commits
 *     landed, flag-on is the single-threaded dress rehearsal of the boundary
 *     deferral semantics), and all sim-fired unsynced work defers to the
 *     SimDrawBarrier (see UnsyncedBoundaryQueue.h).
 *
 * Headless note: the default keeps headless single-threaded, but an explicit
 * SimDrawSplit=1 is honored there too -- the DEBUG-headless replay gate and
 * the automated flag-ON resim gates depend on that.
 *
 * THE SIM PHASE (InSimPhase): a thread-local bracket around the net-message
 * consumption that issues SimFrames (ClientReadNet). Pre-split, CGame::Update
 * opens it on the main thread; post-split, the sim thread's loop opens it --
 * so "InSimPhase()" is exactly "executing on behalf of the simulation right
 * now", regardless of which commit landed. Deferral predicates key on it, NOT
 * on thread identity, precisely so the semantics are testable single-threaded.
 */
#include <atomic>
#include <cstdint>

class CUnit;
class CFeature;

namespace SimDrawSplit {
	/// cached `SimDrawSplit` config var; stable for the whole game session
	/// (read once at CGame construction -- changing it mid-game is unsupported).
	/// Header-inline (C++17 inline variable) so TUs like TimeProfiler.cpp that
	/// only gate on the flag link into test executables without dragging
	/// SimDrawSplit.cpp's config/rendering dependencies along.
	inline bool g_splitEnabled = false;

	inline bool Enabled() { return g_splitEnabled; }

	/// (re)read the config var; called from the CGame ctor
	void UpdateConfig();

	/// game teardown reset (thread-locals are per-thread and die with them)
	void Clear();

	// ---- boundary dispatch-drain window (PR 27b -> PR 43 -> PR 44b) ----
	// True while the barrier (or the valve service) replays deferred
	// dispatches whose objects may have died later in the same sim burst:
	// the deferred-deletion shells are still readable (the ack comes after),
	// so record dispatch resolves them instead of crashing -- master ran
	// those handlers mid-frame with the object alive. PR 43 additionally
	// keys the snapshot's DEAD_THIS_BATCH row validity and the drawers'
	// dead-retained render records on this window. PR 44b (§3.6c) scopes the
	// window PER EPOCH: the flip consumer opens it with the epoch id whose
	// sealed records/closures are dispatching (the lockstep barrier and the
	// valve service open it with their held/served epoch likewise), and that
	// epoch cannot retire until the bracket closes (retirement only happens
	// at the next barrier's acquire, after the close -- asserted there).
	// Main thread only; MUST be false again before the ack poisons the
	// shells. Header-inline (like g_splitEnabled) so SimSnapshot's Valid()
	// accessors can consult it without pulling SimDrawSplit.cpp into every
	// TU (test executables).
	inline bool g_boundaryShellWindow = false;
	inline uint64_t g_dispatchingEpochId = 0;

	inline void SetBoundaryShellWindow(bool active) { g_boundaryShellWindow = active; }
	inline bool BoundaryShellWindowActive() { return g_boundaryShellWindow; }

	// PR 44b §3.6c: the per-epoch dispatch bracket (flip consumer + valve)
	inline void OpenEpochDispatchWindow(uint64_t epochId) {
		g_boundaryShellWindow = true;
		g_dispatchingEpochId = epochId;
	}
	inline void CloseEpochDispatchWindow() {
		g_boundaryShellWindow = false;
		g_dispatchingEpochId = 0;
	}
	inline uint64_t DispatchingEpochId() { return g_dispatchingEpochId; }

	// PR 44b §3.8: drain-time liveness checks for the deferred closures that
	// used to re-resolve ids through the sim-owned handler tables (a
	// concurrent-rehash/generation hazard once the sim runs during the
	// drain). Both consult ONLY draw-owned / dispatch-populated state: the
	// drawer render record's deferred-safe handle, the batch's dead-shell
	// map and the pending-destroy ledger (mutex-guarded). "Alive at drain"
	// reproduces the old `unitHandler.GetUnit(id) == expected` semantics:
	// false when the id died in the dispatching batch, died after the
	// epoch's edge (destroy record pending in the next epoch), or was reused
	// by a new object.
	bool BoundaryUnitAliveAtDrain(int unitID, const CUnit* expected);
	/// current live occupant of unitID via the drawer render record;
	/// nullptr when dead-in-batch / pending-dead / unregistered
	CUnit* BoundaryLiveUnit(int unitID);

	/// true while this thread executes the sim phase (ClientReadNet/SimFrame)
	bool InSimPhase();

	/// Enabled() && InSimPhase() -- "sim-fired unsynced work must defer now"
	bool DeferUnsyncedNow();

	// the sim-phase bracket; opened unconditionally (a TLS bool store) so the
	// flag-off path stays byte-identical in behavior without extra branches
	struct ScopedSimPhase {
		ScopedSimPhase();
		~ScopedSimPhase();
	};

	/**
	 * THE HANDSHAKE (the split's barrier synchronization, PR 27b commit a).
	 *
	 * Protocol: once per draw frame the main thread calls RequestPause(); the
	 * sim thread parks at its next frame edge (never mid-SimFrame -- the
	 * yield points are the sim loop top and the inter-packet check inside
	 * ClientReadNet) and the main thread runs the SimDrawBarrier + the drawer
	 * extraction with the sim quiescent, then ReleasePause(boundaryFrame).
	 *
	 * The pool-pressure valve (DeferredObjectDeleter) may instead park the
	 * sim MID-frame via ParkAtValve() when the object pools run out of
	 * headroom; RequestPause() then returns with ParkedAtValve() true and the
	 * caller must service it (flush records + drain deferred dispatches +
	 * ack + release the slots) and call ResumeFromValve(), which waits for
	 * the eventual frame-edge park. A valve park with no pause pending is
	 * serviced by the next Draw's RequestPause().
	 *
	 * Deadlock argument: the main thread only blocks waiting for `parked !=
	 * NONE || !simRunning`; the sim thread only blocks parked (released by
	 * ReleasePause / ResumeFromValve / exit). A valve-parked sim satisfies
	 * the main thread's wait immediately, and the valve service frees the
	 * pages the sim needs to reach its frame edge.
	 *
	 * Backpressure: ReleasePause records the boundary's sim frame;
	 * ClientReadNet consumes a NEWFRAME only while `frameNum <
	 * LastBoundaryFrame()+1` unless free-running (fast-forward / catch-up /
	 * skip / video capture) -- interpolation stays within one frame of the
	 * published boundary at 1x.
	 */

	// main-thread side
	void RequestPause();
	bool ParkedAtValve();
	void ResumeFromValve();
	/// PR 44b (no-park): resume a valve-parked sim WITHOUT waiting for a
	/// follow-up park (no pause is pending -- the Draw-top valve service)
	void ResumeFromValveNoWait();
	void ReleasePause(int boundaryFrame);
	/// PR 44b (no-park): the consumer publishes the backpressure boundary
	/// frame explicitly (ReleasePause's side effect, park-free) -- the same
	/// value at the same per-frame point (the consume-complete signal), so
	/// the 44a pacing behavior is preserved verbatim
	void PublishBoundaryFrame(int boundaryFrame);

	/// main-thread lock-free peek: the sim thread is currently parked (edge
	/// or valve) -- reads of sim state from the main thread are quiescent
	bool IsSimParked();

	// sim-thread side
	bool PauseRequested();
	void YieldIfPauseRequested();
	void ParkAtValve();
	void SimIdleWait();
	int  LastBoundaryFrame();

	// sim-thread lifecycle (main sets running BEFORE spawning -- the pause
	// handshake must see the thread from the very first Draw -- and exit+join
	// at teardown; the thread proc clears running on exit).
	// Header-inline like Enabled() so TUs that only gate on it (QuadField's
	// scratch-slot owner) link into test executables without SimDrawSplit.cpp.
	inline std::atomic<bool> g_simThreadRunning = {false};

	inline bool SimThreadRunning() { return g_simThreadRunning.load(); }

	void SetSimThreadRunning(bool b);
	void RequestSimThreadExit();
	bool SimThreadExitRequested();
	void ResetSimThreadExit();
}
