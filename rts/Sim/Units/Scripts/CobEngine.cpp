/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "CobEngine.h"

#include "CobDeferredCallin.h"
#include "CobThread.h"
#include "CobFile.h"

#include <cstddef>
#include <cstdint>
#include "System/Misc/TracyDefs.h"
#include "Lua/LuaUI.h"

// For slot pool generation handling.
// We're using 13 generation bits / 18 slot bits so that:
// * at 32k MAX_UNITS, every unit could have 8 cob scripts
// * there's room for 8,192 generations
// both of which are WELL beyond the supported use case of the engine.
static constexpr uint32_t THREAD_ID_GEN_BITS  = 13;
static constexpr uint32_t THREAD_ID_SLOT_BITS = 18;
constexpr static uint32_t GENERATION_MAX = (1 << THREAD_ID_GEN_BITS) - 1;
constexpr static uint32_t SLOT_MAX = (1 << THREAD_ID_SLOT_BITS) - 1;
static_assert(THREAD_ID_GEN_BITS + THREAD_ID_SLOT_BITS <= 31,                                  
			"thread id must fit in non-negative int");

CR_BIND(CCobEngine, )

CR_REG_METADATA(CCobEngine, (
	CR_MEMBER(threadSlots),
	CR_MEMBER(recycledThreadSlots),
	CR_MEMBER(tickAddedThreads),
	CR_MEMBER(tickRemovedThreads),
	CR_MEMBER(runningThreadIDs),
	CR_MEMBER(sleepingThreadIDs),
	// always null/empty when saving
	CR_IGNORED(waitingThreadIDs),

	CR_IGNORED(curThread),
	CR_IGNORED(deferredCallins),

	CR_MEMBER(currentTime)
))

CR_BIND(CCobEngine::SleepingThread, )
CR_REG_METADATA(CCobEngine::SleepingThread, (
	CR_MEMBER(id),
	CR_MEMBER(wt)
))
CR_BIND(CCobEngine::Slot, )
CR_REG_METADATA(CCobEngine::Slot, (
	CR_MEMBER(generation),
	CR_MEMBER(isOccupied),
	CR_MEMBER(thread)
))

static const char* const numCobThreadsPlot = "CobThreads";

int CCobEngine::AllocateThreadID()
{
	size_t slotIndex;
    
	if (recycledThreadSlots.empty()) {
		slotIndex = threadSlots.size();
		threadSlots.emplace_back();
	} else {
		slotIndex = recycledThreadSlots.back();
		recycledThreadSlots.pop_back();
	}
	
	Slot* slot = &threadSlots[slotIndex];
	slot->generation = (slot->generation + 1) & GENERATION_MAX;
	slot->isOccupied = true;
	return PackThreadID(slot->generation, slotIndex);
}

int CCobEngine::AddThread(CCobThread&& thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (thread.GetID() == -1) {
		thread.SetID(AllocateThreadID());
	}
	assert(thread.GetID() >= 0);

	uint32_t generation;
    size_t slotIndex;
    UnpackThreadID(thread.GetID(), generation, slotIndex);
	
	Slot* slot = &threadSlots[slotIndex];
	slot->thread = std::move(thread);
	slot->thread.cobInst->AddThreadID(slot->thread.GetID());

	TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadSlots.size() - recycledThreadSlots.size()));
	return slot->thread.GetID();
}

bool CCobEngine::RemoveThread(int threadID) {
	RECOIL_DETAILED_TRACY_ZONE;
	assert(threadID >= 0);

    size_t slotIndex;
	uint32_t generation;
	UnpackThreadID(threadID,  generation, slotIndex);
	if unlikely(slotIndex >= threadSlots.size()) {
    	return false;
	}

	Slot* matchingSlot = &threadSlots[slotIndex];
	if (!matchingSlot->isOccupied || matchingSlot->generation != generation) {
		return false;
	}

	std::destroy_at(&matchingSlot->thread);
	std::construct_at(&matchingSlot->thread); // avoids double destruct when the deque tears down at engine shutdown

	matchingSlot->isOccupied = false;
	recycledThreadSlots.push_back(slotIndex);

	TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadSlots.size() - recycledThreadSlots.size()));
	return true;
}

void CCobEngine::ProcessQueuedThreads() {
	ZoneScoped;

	// Remove threads killed during Tick by other thread (SIGNAL), we do it
	// here as nothing is actively referencing any thread's memory here.
	for (int threadID: tickRemovedThreads) {
		RemoveThread(threadID);
	}
	tickRemovedThreads.clear();

	// move new threads spawned by START into threadSlots;
	// their ID's will already have been scheduled into either
	// waitingThreadIDs or sleepingThreadIDs
	for (CCobThread& t: tickAddedThreads) {
		AddThread(std::move(t));
	}

	tickAddedThreads.clear();
}

// a thread wants to continue running at a later time, and adds itself to the scheduler
void CCobEngine::ScheduleThread(const CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (thread->GetState()) {
		case CCobThread::Run: {
			waitingThreadIDs.push_back(thread->GetID());
		} break;
		case CCobThread::Sleep: {
			sleepingThreadIDs.push(SleepingThread{thread->GetID(), thread->GetWakeTime()});
		} break;
		default: {
			LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %d", __func__, thread->GetState(), thread->GetID());
		} break;
	}
}

void CCobEngine::SanityCheckThreads(const CCobInstance* owner)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (false) {
		// no threads belonging to owner should be left
		for (const auto& p: threadSlots) {
			assert(p.thread.cobInst != owner || p.isOccupied == false);
		}
		for (const CCobThread& t: tickAddedThreads) {
			assert(t.cobInst != owner);
		}
	}
}


void CCobEngine::TickThread(CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// for error messages originating in CUnitScript
	curThread = thread;

	// NB: threadID is still in <runningThreadIDs> here, TickRunningThreads clears it
	if (thread != nullptr && !thread->Tick())
		RemoveThread(thread->GetID());

	curThread = nullptr;
}

void CCobEngine::WakeSleepingThreads()
{
	ZoneScoped;
	// check on the sleeping threads, remove any whose owner died
	while (!sleepingThreadIDs.empty()) {
		CCobThread* zzzThread = GetThread((sleepingThreadIDs.top()).id);

		if (zzzThread == nullptr) {
			sleepingThreadIDs.pop();
			continue;
		}

		// not yet time to execute this thread or any subsequent sleepers
		if (zzzThread->GetWakeTime() >= currentTime)
			break;

		// remove executing thread from the queue
		sleepingThreadIDs.pop();

		// wake up the thread and tick it (if not dead)
		// this can quite possibly re-add the thread to <sleepingThreadIDs>
		// again, but any thread is guaranteed to sleep for at least 1 tick
		switch (zzzThread->GetState()) {
			case CCobThread::Sleep: {
				zzzThread->SetState(CCobThread::Run);
				TickThread(zzzThread);
			} break;
			case CCobThread::Dead: {
				RemoveThread(zzzThread->GetID());
			} break;
			default: {
				LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %d", __func__, zzzThread->GetState(), zzzThread->GetID());
			} break;
		}
	}
}

void CCobEngine::TickRunningThreads()
{
	ZoneScoped;
	// advance all currently running threads
	for (const int threadID: runningThreadIDs) {
		TickThread(GetThread(threadID));
	}

	// a thread can never go from running->running, so clear the list
	// note: if preemption was to be added, this would no longer hold
	// however, TA scripts can not run preemptively anyway since there
	// aren't any synchronization methods available
	runningThreadIDs.clear();

	// prepare threads that will run next frame
	std::swap(runningThreadIDs, waitingThreadIDs);
}

void CCobEngine::Tick(int deltaTime)
{
	ZoneScoped;
	currentTime += deltaTime;

	TickRunningThreads();
	ProcessQueuedThreads();

	WakeSleepingThreads();
	ProcessQueuedThreads();
}


void CCobEngine::ShowScriptError(const std::string& msg)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (curThread != nullptr) {
		curThread->ShowError(msg.c_str());
		return;
	}

	LOG_L(L_ERROR, "[COBEngine::%s] \"%s\" outside script execution", __func__, msg.c_str());
}


void CCobEngine::AddDeferredCallin(CCobDeferredCallin&& deferredCallin)
{
	deferredCallins[deferredCallin.funcHash].push_back(deferredCallin);
}


void CCobEngine::RunDeferredCallins()
{
	std::vector<int> funcHashes;
	funcHashes.reserve(deferredCallins.size());
	for(auto& it: deferredCallins)
		funcHashes.push_back(it.first);

	for(auto funcHash: funcHashes) {
		auto it = deferredCallins.find(funcHash); // 'it' has to necessarily be present at this point

		auto callins = std::move(it->second);
		deferredCallins.erase(it);

		const LuaHashString cmdStr = LuaHashString(callins[0].funcName.c_str());
		luaRules->unsyncedLuaHandle.Cob2LuaBatch(cmdStr, callins);
		if (luaUI)
			luaUI->Cob2LuaBatch(cmdStr, callins);
	}
}

int CCobEngine::PackThreadID(const uint32_t generation, const size_t slotIndex) {
	assert(slotIndex <= SLOT_MAX);
	assert(generation <= GENERATION_MAX);
	if unlikely (slotIndex > SLOT_MAX || generation > GENERATION_MAX) {
		LOG_L(L_ERROR, "[CCobEngine::%s] cannot pack id (generation=%u, slotIndex=%zu) — exceeds GENERATION_MAX=%u or SLOT_MAX=%u",
			__func__, generation, slotIndex, GENERATION_MAX, SLOT_MAX);
		return -1;
	}
	return static_cast<int>((generation << THREAD_ID_SLOT_BITS) | slotIndex);
}

void CCobEngine::UnpackThreadID(const int threadID, uint32_t& generation, size_t& slotIndex) {
	const uint32_t bits = static_cast<uint32_t>(threadID);
	generation = (bits >> THREAD_ID_SLOT_BITS) & GENERATION_MAX;
	slotIndex = bits & SLOT_MAX;
}
