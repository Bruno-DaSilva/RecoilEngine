/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimDrawSplit.h"

#include <cassert>

#include "System/Config/ConfigHandler.h"

CONFIG(int, SimDrawSplit)
	.defaultValue(0)
	.description("Run the simulation (net-message consumption + SimFrames + synced Lua) on a dedicated sim thread, meeting the draw side once per draw frame at the SimDrawBarrier (PR 27b). 0=off (single-threaded, bit-identical legacy behavior), 1=on. Default off; honored under headless when set explicitly (the DEBUG replay gates rely on that).");

namespace SimDrawSplit {

namespace {
	bool enabled = false;

	// see the header: keyed on execution context, not thread identity, so the
	// pre-thread-spawn commits are a faithful single-threaded dress rehearsal
	thread_local int tlSimPhaseDepth = 0;
}

bool Enabled() { return enabled; }

void UpdateConfig()
{
	enabled = (configHandler != nullptr && configHandler->GetInt("SimDrawSplit") != 0);
}

void Clear()
{
	enabled = false;
}

bool InSimPhase() { return (tlSimPhaseDepth > 0); }

bool DeferUnsyncedNow() { return (enabled && InSimPhase()); }

ScopedSimPhase::ScopedSimPhase() { ++tlSimPhaseDepth; }
ScopedSimPhase::~ScopedSimPhase() { assert(tlSimPhaseDepth > 0); --tlSimPhaseDepth; }

}
