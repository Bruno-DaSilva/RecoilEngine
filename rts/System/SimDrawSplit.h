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
}
