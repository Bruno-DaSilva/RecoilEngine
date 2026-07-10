/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <functional>

class CEventClient;

/**
 * @brief UnsyncedBoundaryQueue -- boundary deferral for sim-fired unsynced work
 *
 * PR 27b (doc/sim-draw-thread-decoupling-research.md Wave 4, and the
 * "enumerated deviation (a)" of the compatibility-invariants section:
 * unsynced event handlers defer from mid-sim-frame to the boundary).
 *
 * Under the split, the sim phase executes on the sim thread while the main
 * thread owns every unsynced lua_State and all draw/UI state. Anything the
 * sim fires that would synchronously run unsynced code -- eventHandler
 * dispatches to unsynced clients (LuaUI UnitCreated/GameFrame/...), Cob2Lua
 * into unsynced handles, SendToUnsynced, wordCompletion writes, the
 * net-message draw pokes -- must instead be captured here and replayed on the
 * main thread inside the next SimDrawBarrier, in exact fire order.
 *
 * Semantics:
 *  - ONE FIFO of closures; order across all sources is the fire order, which
 *    is what master's synchronous dispatch delivered.
 *  - Closures capture arguments BY VALUE plus sim-object pointers. Object
 *    pointers stay dereferenceable until the barrier by the PR-13 deferred-
 *    deletion epoch (a destroyed object is a readable shell until the drain
 *    acks it); args that master reads at fire time are captured at fire time.
 *    Deep sub-object pointers (e.g. CWeapon*) are NOT lifetime-protected --
 *    such events re-validate or drop at drain (see the StockpileChanged
 *    dispatcher).
 *  - Event-client dispatches use DeferFor(ec, fn): the entry is tagged with
 *    the client and only replayed if that client is still registered with
 *    the eventHandler at drain time -- a handle disabled between defer and
 *    boundary (e.g. /luaui disable) silently drops its pending events, the
 *    same set it would have missed had it died a frame earlier. Closures
 *    referencing handle globals (luaUI, luaRules) re-null-check at drain.
 *  - Deferral triggers ONLY when SimDrawSplit::DeferUnsyncedNow() -- flag on
 *    AND inside the sim phase. Flag off: one cached-bool load, immediate
 *    dispatch, bit-identical legacy behavior. Main-thread dispatch outside
 *    the sim phase (input events, draw callins, the barrier's own sanctioned
 *    dispatches) is never deferred.
 *
 * THREADING: writers are sim-phase context only; Drain() runs on the main
 * thread inside SimDrawBarrier while the sim thread is parked (or, pre-spawn,
 * on the single thread). The container therefore never sees concurrent
 * access and takes no lock; the asserts in the .cpp enforce the discipline.
 */
namespace UnsyncedBoundaryQueue {
	/// PR 44b: mirrors SimSnapshot::EPOCH_RING_SLOTS (static-asserted at the
	/// Game.cpp seal site)
	inline constexpr int MAX_EPOCH_BATCH_SLOTS = 3;

	/// the per-client deferral predicate for event dispatch loops:
	/// SimDrawSplit::DeferUnsyncedNow() && client is unsynced. Fire-time
	/// capture clients (CEventClient::IsSimPhaseCaptureClient) are exempted
	/// at their dispatch sites (the LOS-transition macro), not here -- their
	/// OTHER events (e.g. CUnitDrawerData::PlayerChanged) do defer.
	bool ShouldDefer(const CEventClient* ec);

	/// append an untagged closure (non-event work: Cob2Lua, SendToUnsynced,
	/// net-message draw pokes; the closure re-validates its own targets);
	/// asserts sim-phase context
	void Defer(std::function<void()>&& fn);

	/// append an event dispatch for client `ec` (drain-time registry check)
	void DeferFor(const CEventClient* ec, std::function<void()>&& fn);

	/// replay everything in fire order; SimDrawBarrier step (sim parked)
	/// returns the number of dispatched entries; nonzero means a handler may
	/// have mutated sim state post-snapshot (see the .cpp comment) -- the
	/// caller marks the snapshot mutated-outside-frame
	size_t Drain();

	// ---- PR 44a/44b (producer flip): per-epoch closure batches ----
	/// producer (sim thread, frame edge): MOVE the pending closures into the
	/// epoch ring slot about to publish (PR 44b: physically per-slot, so the
	/// consumer's dispatch and the sim's appends never share a container --
	/// fenced by the epoch publish/acquire, no lock)
	void SealEpochBatch(int slot);
	/// consumer (barrier): replay exactly the held epoch's sealed batch in
	/// fire order; same return-count contract as Drain()
	size_t DrainSealedBatch(int slot);

	bool Empty();
	/// true when the slot holds no sealed-undispatched batch
	bool SlotBatchEmpty(int slot);

	/// teardown: drop anything queued (game is going away; the callin
	/// targets are being destroyed)
	void Clear();
}
