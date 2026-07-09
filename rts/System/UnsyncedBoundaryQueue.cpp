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
	// PR 44a: entries [0, sealedEntries) belong to the newest published epoch
	// (sealed by the producer at its frame edge, dispatched by the consumer
	// under the park; park-fenced like RenderEventQueue::sealedRecords)
	size_t sealedEntries = 0;
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
	// PR 44a: a full drain (lockstep barrier / valve service) consumed any
	// sealed-but-undispatched epoch batch along with the tail
	sealedEntries = 0;

	// nonzero: some handler may have poked sim state directly (the barrier's
	// live exception applies class-C ctrl writes immediately, and the drain
	// runs AFTER the snapshot publish) -- the caller must mark the snapshot
	// mutated-outside-frame or a no-new-frame boundary serves stale rows
	// (found by the armed field gate: noSelect / feat:blockingBits)
	return numDispatched;
}

void SealEpochBatch() { sealedEntries = entries.size(); }

size_t DrainSealedBatch()
{
	// PR 44a consumer half: replay exactly the producer-sealed batch in fire
	// order, keep the post-seal tail for the next epoch. Unlike Drain() this
	// does NOT loop-to-empty: work a dispatched closure defers anew belongs
	// to the next epoch by construction (the sim phase never ends under the
	// flip, so re-deferral is the steady state, not a corner).
	assert(!SimDrawSplit::InSimPhase());

	if (sealedEntries == 0)
		return 0;

	assert(sealedEntries <= entries.size());

	size_t numDispatched = 0;

	draining.clear();
	draining.insert(draining.end(),
		std::make_move_iterator(entries.begin()),
		std::make_move_iterator(entries.begin() + sealedEntries));
	entries.erase(entries.begin(), entries.begin() + sealedEntries);
	sealedEntries = 0;

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

void Clear()
{
	entries.clear();
	draining.clear();
	sealedEntries = 0;
}

}
