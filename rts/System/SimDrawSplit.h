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
namespace SimDrawSplit {
	/// cached `SimDrawSplit` config var; stable for the whole game session
	/// (read once at CGame construction -- changing it mid-game is unsupported)
	bool Enabled();

	/// (re)read the config var; called from the CGame ctor
	void UpdateConfig();

	/// game teardown reset (thread-locals are per-thread and die with them)
	void Clear();

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
	void ReleasePause(int boundaryFrame);

	/// main-thread lock-free peek: the sim thread is currently parked (edge
	/// or valve) -- reads of sim state from the main thread are quiescent
	bool IsSimParked();

	// sim-thread side
	bool PauseRequested();
	void YieldIfPauseRequested();
	void ParkAtValve();
	void SimIdleWait();
	int  LastBoundaryFrame();

	// sim-thread lifecycle (main sets exit + joins; the thread proc brackets
	// itself with SetSimThreadRunning)
	void SetSimThreadRunning(bool b);
	bool SimThreadRunning();
	void RequestSimThreadExit();
	bool SimThreadExitRequested();
	void ResetSimThreadExit();
}
