/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimDrawSplit.h"

#include "Rendering/Common/RenderEventQueue.h" // drain-window dead/pending shell maps
#include "Rendering/Units/UnitDrawer.h"        // drawer render record (drain-time liveness)

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "System/Config/ConfigHandler.h"

CONFIG(int, SimDrawSplit)
	.defaultValue(0)
	.description("Run the simulation (net-message consumption + SimFrames + synced Lua) on a dedicated sim thread, meeting the draw side once per draw frame at the SimDrawBarrier (PR 27b). 0=off (single-threaded, bit-identical legacy behavior), 1=on. Default off; honored under headless when set explicitly (the DEBUG replay gates rely on that).");

namespace SimDrawSplit {

namespace {
	// see the header: keyed on execution context, not thread identity, so the
	// pre-thread-spawn commits are a faithful single-threaded dress rehearsal
	thread_local int tlSimPhaseDepth = 0;

}

void UpdateConfig()
{
	g_splitEnabled = (configHandler != nullptr && configHandler->GetInt("SimDrawSplit") != 0);
}

void Clear()
{
	g_splitEnabled = false;
	g_boundaryShellWindow = false;
	g_dispatchingEpochId = 0;
	g_uploadedTransformFrame = -1;
}

// SetBoundaryShellWindow / BoundaryShellWindowActive are header-inline (see
// SimDrawSplit.h): the flag gates SimSnapshot's DEAD_THIS_BATCH validity,
// which is read inline from the row accessors.

// PR 44b §3.8 (see the header): drain-time liveness for deferred closures.
// The pre-44b form was `unitHandler.GetUnit(id) == expected`, legal only with
// the sim parked; this consults draw-owned / dispatch-populated state only.
bool BoundaryUnitAliveAtDrain(int unitID, const CUnit* expected)
{
	if (expected == nullptr)
		return false;

	// died in the dispatching batch (its destroy record just dispatched)
	if (renderEventQueue.ResolveBoundaryDeadUnit(unitID) != nullptr)
		return false;

	// died after the epoch's edge (destroy record pending in the next epoch)
	if (renderEventQueue.FindPendingDestroyUnit(unitID) != nullptr)
		return false;

	// identity: the drawer record's deferred-safe handle is the current
	// occupant of the id (a reused id holds the NEW object here)
	return (CUnitDrawer::GetRenderRecord(unitID).obj == expected);
}

CUnit* BoundaryLiveUnit(int unitID)
{
	if (renderEventQueue.ResolveBoundaryDeadUnit(unitID) != nullptr)
		return nullptr;

	if (renderEventQueue.FindPendingDestroyUnit(unitID) != nullptr)
		return nullptr;

	// read-only handle by contract; the two draw-owned poke consumers
	// (UI-group inherit) mutate draw-owned unit state only
	return const_cast<CUnit*>(CUnitDrawer::GetRenderRecord(unitID).obj);
}

bool InSimPhase() { return (tlSimPhaseDepth > 0); }

bool DeferUnsyncedNow() { return (g_splitEnabled && InSimPhase()); }

ScopedSimPhase::ScopedSimPhase() { ++tlSimPhaseDepth; }
ScopedSimPhase::~ScopedSimPhase() { assert(tlSimPhaseDepth > 0); --tlSimPhaseDepth; }


// ---------------------------------------------------------------------------
// the handshake (see the header comment for the protocol + deadlock argument)
// ---------------------------------------------------------------------------

namespace {
	enum ParkKind { PARK_NONE = 0, PARK_EDGE = 1, PARK_VALVE = 2 };

	std::mutex hsMtx;
	std::condition_variable cvSim;   // sim thread waits on this
	std::condition_variable cvMain;  // main thread waits on this

	// fast-path peek for the sim loop; all transitions happen under hsMtx
	std::atomic<bool> pauseRequested = {false};

	// transitions under hsMtx; atomic so IsSimParked can peek lock-free
	std::atomic<int> parkKind = {PARK_NONE};

	std::atomic<bool> simExit = {false};
}


void RequestPause()
{
	if (!g_simThreadRunning.load())
		return;

	std::unique_lock<std::mutex> lock(hsMtx);

	pauseRequested.store(true);
	cvSim.notify_all();
	cvMain.wait(lock, []() { return (parkKind != PARK_NONE || !g_simThreadRunning.load()); });
}

void ReleasePause()
{
	{
		std::unique_lock<std::mutex> lock(hsMtx);
		pauseRequested.store(false);
		// PR 44c: a lazy/lifecycle park can catch (and release over) a
		// VALVE-parked sim -- the valve park state must survive the release
		// (only the sim itself resumes from the valve, in ValveParkWait,
		// once pool headroom returns). Edge parks resume as before.
		if (parkKind == PARK_EDGE)
			parkKind = PARK_NONE;
	}

	cvSim.notify_all();
}


bool PauseRequested() { return pauseRequested.load(std::memory_order_relaxed); }

void YieldIfPauseRequested()
{
	if (!PauseRequested())
		return;

	std::unique_lock<std::mutex> lock(hsMtx);

	while (pauseRequested.load() && !simExit.load()) {
		parkKind = PARK_EDGE;
		cvMain.notify_all();
		cvSim.wait(lock);
	}

	parkKind = PARK_NONE;
}

bool ValveParkWait()
{
	// PR 44c (§3.4): one self-servicing pool-valve wait round, sim thread,
	// mid-frame. Present as a parked sim so a concurrent RequestPause is
	// satisfied (a valve-parked sim IS quiescent), nap briefly to give the
	// draw side time to consume+retire, then -- crucially -- hold parked for
	// as long as a pause is pending: the pause holder's park-time reads need
	// quiescence until ReleasePause. parkKind returns to PARK_NONE before the
	// caller goes active again (ServiceRetiredReleases / the forced tail
	// publish), so a RequestPause landing mid-round simply waits for the next
	// round's re-park (~ms). Only the sim itself resumes from the valve.
	std::unique_lock<std::mutex> lock(hsMtx);

	parkKind = PARK_VALVE;
	cvMain.notify_all();

	// bounded nap; woken early by ReleasePause, a new pause request, or exit
	cvSim.wait_for(lock, std::chrono::milliseconds(1));

	// a pause holder observed PARK_VALVE and is reading sim state -- stay
	// parked (fully quiescent) until it releases
	while (pauseRequested.load() && !simExit.load())
		cvSim.wait(lock);

	parkKind = PARK_NONE;
	return !simExit.load();
}

void SimIdleWait()
{
	std::unique_lock<std::mutex> lock(hsMtx);

	if (pauseRequested.load() || simExit.load())
		return;

	// bounded nap between net-consumption passes: woken early by a pause
	// request or exit; new net packets are simply picked up next pass
	cvSim.wait_for(lock, std::chrono::milliseconds(1));
}

bool IsSimParked() { return (parkKind.load(std::memory_order_relaxed) != PARK_NONE); }


void SetSimThreadRunning(bool b)
{
	{
		std::unique_lock<std::mutex> lock(hsMtx);
		g_simThreadRunning.store(b);

		if (!b)
			parkKind = PARK_NONE;
	}

	// a dying sim thread must unblock a main thread waiting for a park
	cvMain.notify_all();
}

void RequestSimThreadExit()
{
	{
		std::unique_lock<std::mutex> lock(hsMtx);
		simExit.store(true);
	}

	cvSim.notify_all();
}

bool SimThreadExitRequested() { return simExit.load(std::memory_order_relaxed); }

void ResetSimThreadExit()
{
	simExit.store(false);
	pauseRequested.store(false);
}

}
