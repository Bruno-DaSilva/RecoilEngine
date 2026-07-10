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
	bool valveServed = false;    // guarded by hsMtx

	std::atomic<int> lastBoundaryFrame = {-1};
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

bool ParkedAtValve()
{
	std::unique_lock<std::mutex> lock(hsMtx);
	return (parkKind == PARK_VALVE);
}

void ResumeFromValve()
{
	std::unique_lock<std::mutex> lock(hsMtx);

	assert(parkKind == PARK_VALVE);
	parkKind = PARK_NONE;
	valveServed = true;
	cvSim.notify_all();
	// pauseRequested is still set; wait for the frame-edge park (or a
	// repeat valve park, which the caller's loop services again)
	cvMain.wait(lock, []() { return (parkKind != PARK_NONE || !g_simThreadRunning.load()); });
}

void ResumeFromValveNoWait()
{
	// PR 44b: the Draw-top valve service runs with NO pause pending -- the
	// resumed sim will not park again (until the next valve/lifecycle
	// event), so there is nothing to wait for
	std::unique_lock<std::mutex> lock(hsMtx);

	assert(parkKind == PARK_VALVE);
	parkKind = PARK_NONE;
	valveServed = true;
	cvSim.notify_all();
}

void PublishBoundaryFrame(int boundaryFrame)
{
	lastBoundaryFrame.store(boundaryFrame);
}

void ReleasePause(int boundaryFrame)
{
	lastBoundaryFrame.store(boundaryFrame);

	{
		std::unique_lock<std::mutex> lock(hsMtx);
		pauseRequested.store(false);
		// PR 44b: a lazy/lifecycle park can catch (and release over) a
		// VALVE-parked sim -- the valve park state must survive the release
		// (the sim still waits for the valve service; ParkedAtValve() / the
		// Draw-top ServicePoolValve key on it). Edge parks resume as before.
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

void ParkAtValve()
{
	std::unique_lock<std::mutex> lock(hsMtx);

	parkKind = PARK_VALVE;
	valveServed = false;
	cvMain.notify_all();
	cvSim.wait(lock, []() { return (valveServed || simExit.load()); });
	valveServed = false;
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

int LastBoundaryFrame() { return lastBoundaryFrame.load(std::memory_order_relaxed); }

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
	lastBoundaryFrame.store(-1);
	pauseRequested.store(false);
}

}
