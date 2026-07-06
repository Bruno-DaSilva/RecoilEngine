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

void Drain()
{
	assert(!SimDrawSplit::InSimPhase());

	while (!entries.empty()) {
		draining.clear();
		std::swap(draining, entries);

		for (auto& e: draining) {
			if (e.tag != nullptr && !eventHandler.HasClient(const_cast<CEventClient*>(e.tag)))
				continue;

			e.fn();
		}
	}

	draining.clear();
}

bool Empty() { return entries.empty(); }

void Clear()
{
	entries.clear();
	draining.clear();
}

}
