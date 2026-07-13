/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

/**
 * @brief Sim|draw boundary size instrumentation (the "S0 measurement pass").
 *
 * Per-sim-frame counters sized to answer the questions in
 * doc/replay-seeking-architecture.md ("Sizes" section) and
 * doc/sim-draw-thread-decoupling-research.md (decision 8): how big is the
 * extracted-state boundary really, and how fast does it mutate?
 *
 *  - transformsMemStorage/modelUniformsStorage resident sizes + write rates
 *    (the in-tree proxy for snapshot/stream size)
 *  - per-piece transform churn (how many LocalModelPiece model-space
 *    transforms actually change per sim frame, on what fraction of objects)
 *  - command-queue mutation rate + queue-length distribution
 *  - projectile churn, split by re-derivability class (piece / hitscan /
 *    ballistic / guided / unsynced)
 *  - unit/feature create/destroy and LOS-transition event rates
 *
 * HARD CONSTRAINT (sync audit item 4): observation only. The counters are
 * plain relaxed atomics written from existing code paths; no synced state is
 * read-modified, no streflop/FPU-visible arithmetic is introduced, and no
 * synced allocation changes. The per-frame sampling walk (SampleFrame) only
 * runs while a dump is active and performs read-only traversals.
 *
 * The counter increments are cheap enough to be always-on (a handful of
 * relaxed fetch_adds per event); the dump samples per-frame deltas, so the
 * cumulative values never need resetting. Everything heavier (queue-length
 * walk, hot-field compare table) is gated on Active().
 *
 * Surfaced via /boundarydump <startFrame> <endFrame> [out.csv] — a sibling of
 * /profiledump that follows the same StartDump/DumpFrame pattern but keeps its
 * own (one fixed-width row per frame) buffer, so full-game dumps stay small
 * where /profiledump's per-name rows would not.
 */
namespace BoundaryStats {
	using Counter = std::atomic<uint64_t>;

	struct Counters {
		// TransformsMemStorage write traffic (GPU-extraction proxy; one
		// extraction pass over all objects per new sim frame — see
		// CModelDrawerDataBase::ExtractTransforms)
		Counter traChecked{0};   // UpdateIfChanged calls (compare performed)
		Counter traChanged{0};   // ... of which the value actually differed
		Counter traForced{0};    // UpdateForced calls (unconditional writes)

		// per-sim-frame piece-pose churn, measured sim-side in
		// CSolidObject::UpdatePrevFrameTransform (works headless, exact
		// per-sim-frame resolution regardless of draw rate)
		Counter pieceSampled{0};     // piece transforms examined
		Counter pieceChanged{0};     // ... of which changed since last sim frame
		Counter objSampled{0};       // objects examined (units + queued features)
		Counter objPieceChanged{0};  // ... with >= 1 changed piece transform
		Counter objMoved{0};         // ... whose root (unit-space) transform changed

		// CCommandQueue mutation rate (order-rate, the dirty-versioning bound)
		Counter cmdPushBack{0};
		Counter cmdPushFront{0};
		Counter cmdInsert{0};
		Counter cmdPopBack{0};
		Counter cmdPopFront{0};
		Counter cmdErase{0};      // erased elements (ranges count their size)
		Counter cmdClearCmds{0};  // elements dropped via clear()

		// sim|draw PR 30: command-queue/cmd-desc serving-cache copy cost (the
		// decision-4 acceptance number -- "one boundary copy tracks the order
		// rate"). Incremented in LuaSnapshotServe::RefreshCommandQueues each time
		// a dirty unit forces a fresh flattened copy of its queue / cmd-desc
		// surface; unchanged units keep their cached copy and add nothing. Bytes
		// are the flattened record + param/string sizes.
		Counter cmdBlocksCopied{0};      // fresh queue copies (commandQue + newUnitCommands)
		Counter cmdBlockBytes{0};        // their SnapCommand + param bytes
		Counter cmdDescBlocksCopied{0};  // fresh cmd-desc copies
		Counter cmdDescBlockBytes{0};    // their CmdDescRecord + string/param bytes

		// projectile churn by re-derivability class (spawn-params re-derivation
		// works for ballistic; guided/piece need per-frame updates)
		Counter projSpawnPiece{0};
		Counter projSpawnHitscan{0};
		Counter projSpawnGuided{0};    // weaponDef->tracks
		Counter projSpawnBallistic{0}; // weapon, not tracking, not hitscan
		Counter projSpawnSyncedOther{0};
		Counter projSpawnUnsynced{0};
		Counter projDespawnSynced{0};
		Counter projDespawnUnsynced{0};

		// unit/feature lifecycle + LOS transition event rates (the render-event
		// delta-queue load, and live-feature bookkeeping)
		Counter unitCreated{0};
		Counter unitDestroyed{0};
		Counter featCreated{0};
		Counter featDestroyed{0};
		Counter losEnterLos{0};
		Counter losLeaveLos{0};
		Counter losEnterRadar{0};
		Counter losLeaveRadar{0};
	};

	inline Counters ctr;
	inline std::atomic<bool> active{false};

	inline bool Active() { return active.load(std::memory_order_relaxed); }
	inline void Add(Counter& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

	// arm a dump over sim frames [startFrame, endFrame]; row per sampled frame,
	// CSV written when the range completes (or on FlushPartial)
	void StartDump(int startFrame, int endFrame, std::string path);
	// call once per sim frame (CGame::SimFrame); no-op unless a dump is active
	void SampleFrame(int frameNum);
	// write whatever was collected if the game ends before endFrame
	void FlushPartial();
}
