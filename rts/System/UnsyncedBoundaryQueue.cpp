/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "UnsyncedBoundaryQueue.h"

#include <cassert>
#include <vector>

#include "System/EventClient.h"
#include "System/EventHandler.h"
#include "System/SimDrawSplit.h"

namespace UnsyncedBoundaryQueue {

namespace {
	struct Entry {
		// event-target tag: replay only while this client is still
		// registered (nullptr = untagged, closure self-validates). Never
		// dereferenced -- only compared against the live registry, so a
		// dangling tag is harmless (its entry just gets dropped, or in the
		// same-address-reload corner delivered to the successor handle).
		const CEventClient* tag;
		std::function<void()> fn;
	};

	// sim-phase-exclusive writer, barrier-exclusive reader (header comment)
	std::vector<Entry> entries;
	// drained batch kept separate so a closure firing new deferrable work
	// cannot invalidate the iteration
	std::vector<Entry> draining;
	// PR 44b: the sealed batch MOVES into the epoch's ring slot at seal time
	// (sim thread; `entries` keeps only the post-seal tail), and the consumer
	// dispatches its HELD slot's batch -- so the producer's appends and the
	// consumer's dispatch never touch the same container, fenced by the
	// epoch publish/acquire pair (no lock needed). Replaces the 44a
	// sealedEntries prefix index, which required the park to steal safely.
	// Slot count mirrors SimSnapshot::EPOCH_RING_SLOTS (static-asserted at
	// the Game.cpp seal site; the constant lives in the header).
	std::vector<Entry> slotBatches[MAX_EPOCH_BATCH_SLOTS];
}

bool ShouldDefer(const CEventClient* ec)
{
	return (SimDrawSplit::DeferUnsyncedNow() && !ec->GetSynced());
}

void Defer(std::function<void()>&& fn)
{
	assert(SimDrawSplit::DeferUnsyncedNow());
	entries.emplace_back(Entry{nullptr, std::move(fn)});
}

void DeferFor(const CEventClient* ec, std::function<void()>&& fn)
{
	assert(SimDrawSplit::DeferUnsyncedNow());
	entries.emplace_back(Entry{ec, std::move(fn)});
}

size_t Drain()
{
	assert(!SimDrawSplit::InSimPhase());

	size_t numDispatched = 0;

	while (!entries.empty()) {
		draining.clear();
		std::swap(draining, entries);

		for (auto& e: draining) {
			if (e.tag != nullptr && !eventHandler.HasClient(const_cast<CEventClient*>(e.tag)))
				continue;

			e.fn();
			numDispatched += 1;
		}
	}

	draining.clear();

	// nonzero: some handler may have poked sim state directly (the barrier's
	// live exception applies class-C ctrl writes immediately, and the drain
	// runs AFTER the snapshot publish) -- the caller must mark the snapshot
	// mutated-outside-frame or a no-new-frame boundary serves stale rows
	// (found by the armed field gate: noSelect / feat:blockingBits)
	return numDispatched;
}

void SealEpochBatch(int slot)
{
	// PR 44b: sim thread, at the produce edge -- move the pending closures
	// into the epoch's slot batch. The slot's previous batch was consumed
	// (cleared) when that epoch was dispatched; produce-after-consume pacing
	// guarantees no still-pending batch is overwritten.
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);
	assert(slotBatches[slot].empty());

	slotBatches[slot].swap(entries);
	entries.clear();
}

size_t DrainSealedBatch(int slot)
{
	// PR 44a/44b consumer half: replay exactly the producer-sealed batch (the
	// held epoch's slot batch) in fire order; the post-seal tail lives in
	// `entries` (sim-owned) and rides the next epoch. Unlike Drain() this
	// does NOT loop-to-empty: work a dispatched closure defers anew belongs
	// to the next epoch by construction.
	assert(!SimDrawSplit::InSimPhase());
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);

	if (slotBatches[slot].empty())
		return 0;

	size_t numDispatched = 0;

	draining.clear();
	std::swap(draining, slotBatches[slot]);

	for (auto& e: draining) {
		if (e.tag != nullptr && !eventHandler.HasClient(const_cast<CEventClient*>(e.tag)))
			continue;

		e.fn();
		numDispatched += 1;
	}

	draining.clear();

	// see Drain() -- same mutated-outside-frame contract for the caller
	return numDispatched;
}

bool Empty() { return entries.empty(); }

bool SlotBatchEmpty(int slot)
{
	assert(slot >= 0 && slot < MAX_EPOCH_BATCH_SLOTS);
	return slotBatches[slot].empty();
}

void Clear()
{
	entries.clear();
	draining.clear();
	for (auto& b: slotBatches)
		b.clear();
}

}
