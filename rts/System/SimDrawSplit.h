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
	 * The pool-pressure valve (DeferredObjectDeleter, PR 44c §3.4): when the
	 * pools run out of headroom MID-frame, the sim thread SERVICES ITSELF --
	 * it publishes the mid-frame tail as a normal (pacing-gated) epoch and
	 * waits for EPOCH RETIREMENT in ValveParkWait() rounds, re-checking pool
	 * headroom after each round (the draw side's consume+retire makes the
	 * shells releasable; ServiceRetiredReleases returns the pages). While
	 * waiting it presents as a parked sim (PARK_VALVE), so a concurrent
	 * RequestPause is satisfied and gets genuine quiescence -- the wait
	 * holds parked while a pause is pending, and only the sim itself ever
	 * resumes from the valve. There is NO main-thread valve service anymore
	 * (the pre-44c in-place consume+produce is deleted).
	 *
	 * Deadlock argument (§3.4): the draw side never waits on the sim -- the
	 * epoch acquire is non-blocking and RequestPause is satisfied by an edge
	 * OR valve park. The sim's only waits are the epoch-ring backpressure
	 * gate (released by the draw side's acquire retiring an epoch), the
	 * valve wait (released by the draw side's consume+retire returning pool
	 * pages -- neither needs anything from the sim) and the pause park
	 * (released by ReleasePause).
	 *
	 * Backpressure (PR 44c §3.4): ClientReadNet consumes a NEWFRAME only
	 * while the epoch ring holds < N-1 unretired epochs
	 * (SimSnapshot::UnretiredEpochCount -- replaces the park-era
	 * LastBoundaryFrame()+1 gate), unless free-running (fast-forward /
	 * catch-up / skip / video capture): a free-running sim is extraction-
	 * SKIPPED by the producer's pacing gate, never ring-blocked (§5.13).
	 */

	// main-thread side
	void RequestPause();
	void ReleasePause();

	/// main-thread lock-free peek: the sim thread is currently parked (edge
	/// or valve) -- reads of sim state from the main thread are quiescent
	bool IsSimParked();

	// sim-thread side
	bool PauseRequested();
	void YieldIfPauseRequested();
	/// PR 44c (§3.4): one pool-valve wait round -- present as a parked sim
	/// (PARK_VALVE), nap ~1ms (woken early by ReleasePause / a pause request
	/// / exit), then hold parked while a pause is pending. Returns false when
	/// sim-thread exit was requested (the caller abandons the valve wait).
	/// The caller (DeferredObjectDeleter::WaitForEpochRetirementAtValve)
	/// re-checks pool headroom between rounds and resumes itself.
	bool ValveParkWait();
	void SimIdleWait();

	/// PR 44c telemetry: NEWFRAME consumptions denied by the epoch-ring
	/// backpressure gate (CGame::CanConsumeSimFrameNow); teardown-logged as
	/// [BackpressureStats]. Header-inline like the flags above.
	inline std::atomic<uint64_t> g_ringBlockCount = {0};

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

	// ---- PR 46: epoch-consistent draw interpolation (sim|draw §7.6 follow-up) ----
	// The sim frame the transforms-SSBO lerp pair CURRENTLY ON THE GPU was
	// extracted at (pair = [prev = preFrameTra(F), curr = pos(F)], produced by
	// the flip producer at frame edge F). Under the running flip the pair
	// reaches the GPU >= 1 draw frame AFTER the sim thread rebased the live
	// interpolation clock (CGame::SimFrame resets lastFrameTime at frame
	// start), so the raw globalRendering->timeOffset is momentarily paired
	// with the PREVIOUS frame's pair -- the rendered pose regresses ~0.8 of a
	// frame once per sim frame (the PR-46 30 Hz model/shadow jitter).
	// UniformConstants rebuilds timeInfo as {x = this frame, w = timeOffset +
	// (liveFrame - this frame)}: the shader's clamped Lerp then holds the
	// pair's edge pose until the fresh pair arrives (sub-draw-frame hold)
	// instead of jumping backward, and x+w (the continuous sim-time base Lua
	// shaders use) is unchanged. Stamped in CGame::UpdateUnsynced right after
	// the flip's transformsUploader.Update() consume; written and read on the
	// draw/main thread only -- plain int, -1 = nothing uploaded yet (pregame /
	// lockstep), consumers fall back to the master-identical raw values.
	inline int32_t g_uploadedTransformFrame = -1;

	inline void SetUploadedTransformFrame(int32_t frame) { g_uploadedTransformFrame = frame; }
	inline int32_t UploadedTransformFrame() { return g_uploadedTransformFrame; }
}
