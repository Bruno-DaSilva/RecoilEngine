/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <algorithm>
#include <climits>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "System/TimeProfiler.h"
#include "System/GlobalRNG.h"
#include "System/StringHash.h"
#include "System/Log/ILog.h"
#include "System/Threading/SpringThreading.h"

#ifdef THREADPOOL
	#include "System/Threading/ThreadPool.h"
#endif

using ProfileMutexType = spring::mutex; //spring::spinlock
using HashNamMutexType = spring::mutex; //spring::spinlock

static ProfileMutexType profileMutex;
static HashNamMutexType hashToNameMutex;
static spring::unordered_map<unsigned, std::string> hashToName;
static spring::unordered_map<unsigned, int> refCounters;

// Per-thread stack of currently-open ScopedTimers, used to derive self-time:
// each open timer accumulates the inclusive span of its differently-named
// children, so self = inclusive - childTime. ScopedTimer is main-thread-only
// in practice (refCounters above is unlocked); the stack is thread_local so it
// stays correct even if a SCOPED_TIMER ever runs on another thread. Worker-pool
// zones use the separate ScopedMtTimer path and never touch this.
namespace {
	struct TimerStackFrame {
		unsigned nameHash;
		spring_time childTime;
		spring_time startTime;
		bool isZone; // pushed by the manual Lua Push/PopZone path (vs RAII ScopedTimer)
		spring_time calloutTime; // deep-mode callout subtree time, folded out as leaves
	};
	thread_local std::vector<TimerStackFrame> timerStack;

	// set on the main thread for the duration of CGame::SimFrame (see
	// ScopedSimFramePhase); read by AddTimeRaw to split sim vs draw budget
	thread_local bool tlInSimFrame = false;

	// folded-stack (flamegraph) accumulation, active only while a dump is running
	// (StartDump/StopDump toggle g_foldedActive). On each pop we charge the closing
	// timer's self-time to its full root->leaf path, keyed by the name-hash stack;
	// StopDump translates the hashes to names and writes a collapsed-stack file that
	// speedscope / flamegraph.pl read directly. Self-weighted so widths sum to wall.
	bool g_foldedActive = false;
	std::map<std::vector<unsigned>, int64_t> g_foldedMicros; // path of name-hashes -> summed self us
	thread_local std::vector<unsigned> g_foldedScratch;

	// open a timer on the self-time stack (shared by ScopedTimer and the manual
	// Push/PopZone path); mirrors the old ScopedTimer ctor's refCounters bump
	void timerStackPush(unsigned nameHash, spring_time startTime, bool isZone = false) {
		auto iter = refCounters.find(nameHash);
		if (iter == refCounters.end())
			iter = refCounters.insert(std::pair<unsigned, int>(nameHash, 0)).first;
		++(iter->second);

		timerStack.push_back({nameHash, spring_notime, startTime, isZone, spring_notime});
	}

	// close the top timer: charge its inclusive span to the parent (unless the
	// parent shares its name — same-name recursion gate), output its identity and
	// self/inclusive timing, and return whether refCounters hit zero (i.e. this is
	// the outermost same-name span, so the caller should record it via AddTime).
	bool timerStackPop(unsigned& nameHashOut, spring_time& startOut, spring_time& inclOut, spring_time& selfOut) {
		if (timerStack.empty())
			return false;

		const TimerStackFrame f = timerStack.back();
		timerStack.pop_back();

		const spring_time incl = spring_gettime() - f.startTime;
		if (!timerStack.empty() && timerStack.back().nameHash != f.nameHash)
			timerStack.back().childTime += incl;

		nameHashOut = f.nameHash;
		startOut = f.startTime;
		inclOut = incl;
		selfOut = incl - f.childTime;

		auto iter = refCounters.find(f.nameHash);
		assert(iter != refCounters.end());
		assert(iter->second > 0);
		const bool outermost = (--(iter->second) == 0);

		// charge this frame's self-time to its full call path for the flamegraph;
		// the remaining stack is the ancestor prefix, f is the leaf. Gate on
		// outermost (like AddTime) so same-name recursion folds into one path.
		// Subtract calloutTime: deep-mode callouts that ran in this zone are emitted
		// as their own leaves (FoldCalloutSample), so exclude them from the zone self.
		if (g_foldedActive && outermost) {
			int64_t us = (selfOut - f.calloutTime).toMicroSecsi();
			if (us < 0) us = 0;
			g_foldedScratch.clear();
			for (const TimerStackFrame& a: timerStack)
				g_foldedScratch.push_back(a.nameHash);
			g_foldedScratch.push_back(f.nameHash);
			g_foldedMicros[g_foldedScratch] += us;
		}

		return outermost;
	}
}

ScopedSimFramePhase::ScopedSimFramePhase(): prev(tlInSimFrame) { tlInSimFrame = true; }
ScopedSimFramePhase::~ScopedSimFramePhase() { tlInSimFrame = prev; }
bool ScopedSimFramePhase::InSimFrame() { return tlInSimFrame; }

CallinTimerNames::CallinTimerNames(const char* callin)
{
	const std::string s = std::string("Lua::Callins::Synced::") + callin;
	const std::string u = std::string("Lua::Callins::Unsynced::") + callin;
	syncedHash   = hashString(s.c_str());
	unsyncedHash = hashString(u.c_str());
	// RegisterTimer copies the name into hashToName, so the locals can go away
	CTimeProfiler::RegisterTimer(s.c_str());
	CTimeProfiler::RegisterTimer(u.c_str());
}


// Manual (non-RAII) self-time zones for the Lua Spring.Profiler primitive; they
// nest in the same stack as ScopedTimer, so a Lua-opened zone gets self-time, its
// triggered callouts as children, the phase tag, and the dump for free. Recorded
// as ordinary (non-special) timers: only when the profiler is enabled or a dump is
// active (which force-enables it), so an always-on handler costs ~nothing idle.
void CTimeProfiler::PushZone(unsigned nameHash)
{
	// only track zones while profiling/dumping; PopZone's isZone check then no-ops
	// for the unpushed counterpart, so an always-on handler costs ~nothing idle
	if (!IsEnabled())
		return;

	timerStackPush(nameHash, spring_gettime(), true);
}

void CTimeProfiler::PopZone()
{
	// refuse to pop a non-zone frame: a mismatched/over-eager PopZone must not
	// eat an enclosing RAII timer's frame (that would shift all later accounting)
	if (timerStack.empty() || !timerStack.back().isZone) {
		LOG_L(L_WARNING, "[TimeProfiler] Spring.ProfilerPopZone with no matching open zone");
		return;
	}

	unsigned h;
	spring_time st, incl, self;

	if (timerStackPop(h, st, incl, self))
		AddTime(h, st, incl, self, false, false, false);
}

bool CTimeProfiler::IsFoldedActive()
{
	return g_foldedActive;
}

// Deep-mode callout trampoline (LuaCalloutCounters) calls this on each callout exit
// while a dump is active, to weave callouts into the same flamegraph as the zones.
void CTimeProfiler::FoldCalloutSample(const std::vector<unsigned>& calloutPath, spring_time self, spring_time incl, bool topLevel)
{
	if (!g_foldedActive)
		return;

	// charge a top-level callout's full subtree to the open zone, so the zone's own
	// folded self (timerStackPop) excludes the callouts we emit as separate leaves
	if (topLevel && !timerStack.empty())
		timerStack.back().calloutTime += incl;

	// leaf path = open zones (timerStack) ++ open callouts (calloutPath, this one last)
	int64_t us = self.toMicroSecsi();
	if (us < 0) us = 0;
	g_foldedScratch.clear();
	for (const TimerStackFrame& a: timerStack)
		g_foldedScratch.push_back(a.nameHash);
	for (const unsigned h: calloutPath)
		g_foldedScratch.push_back(h);
	g_foldedMicros[g_foldedScratch] += us;
}

// Trims any Lua zones left open within a callin (a widget/gadget that pushed
// without popping, e.g. erroring outside a pcall), so the leak cannot corrupt the
// enclosing RAII timers' accounting. Constructed last in LUA_CALL_IN_CHECK so it
// unwinds before those timers pop.
ScopedZoneStackGuard::ScopedZoneStackGuard(): baseDepth(timerStack.size()) {}

ScopedZoneStackGuard::~ScopedZoneStackGuard()
{
	if (timerStack.size() <= baseDepth)
		return;

	const size_t leaked = timerStack.size() - baseDepth;

	while (timerStack.size() > baseDepth && timerStack.back().isZone) {
		const TimerStackFrame f = timerStack.back();
		timerStack.pop_back();

		const auto iter = refCounters.find(f.nameHash);
		if (iter != refCounters.end() && iter->second > 0)
			--(iter->second);
	}

	LOG_L(L_WARNING, "[TimeProfiler] %u unbalanced Spring.ProfilerPushZone(s) discarded at callin exit", unsigned(leaked));
}

static CGlobalUnsyncedRNG profileColorRNG;

const std::array<CTimeProfiler::ProfileSortFunc, CTimeProfiler::SortType::ST_COUNT> CTimeProfiler::SortingFunctions = {
	[](const TimeRecordPair& a, const TimeRecordPair& b) { return (a.first          < b.first         ); }, // ST_ALPHABETICAL = 0,
	[](const TimeRecordPair& a, const TimeRecordPair& b) { return (a.second.total   > b.second.total  ); }, // ST_TOTALTIME    = 1,
	[](const TimeRecordPair& a, const TimeRecordPair& b) { return (a.second.stats.y > b.second.stats.y); }, // ST_CURRENTTIME  = 2,
	[](const TimeRecordPair& a, const TimeRecordPair& b) { return (a.second.stats.z > b.second.stats.z); }, // ST_MAXTIME      = 3,
	[](const TimeRecordPair& a, const TimeRecordPair& b) { return (a.second.stats.x > b.second.stats.x); }, // ST_LAG          = 4,
};


spring_time BasicTimer::GetDuration() const
{
	return spring_difftime(spring_gettime(), startTime);
}

ScopedTimer::ScopedTimer(const unsigned _nameHash, bool _autoShowGraph, bool _specialTimer)
	: BasicTimer(_nameHash)

	// Game::SendClientProcUsage depends on "Sim" and "Draw" percentages, BenchMark on "Lua"
	// note that address-comparison is intended here, timer names are (and must be) literals
	, autoShowGraph(_autoShowGraph)
	, specialTimer(_specialTimer)
	// ordinary timers do nothing unless the profiler is enabled (a dump force-
	// enables it); special timers always report. Skips refCounters + stack + record
	// entirely during normal play.
	, tracked(_specialTimer || CTimeProfiler::GetInstance().IsEnabled())
{
	// charge our inclusive span to the parent zone on pop, so its self-time
	// excludes us (same-name recursion handled by the refCounters gate). BasicTimer
	// already stamped startTime.
	if (tracked)
		timerStackPush(nameHash, startTime);
}

ScopedTimer::~ScopedTimer()
{
	if (!tracked)
		return;

	unsigned h;
	spring_time st, incl, self;

	if (timerStackPop(h, st, incl, self))
		CTimeProfiler::GetInstance().AddTime(h, st, incl, self, autoShowGraph, specialTimer, false);
}



ScopedOnceTimer::ScopedOnceTimer(const char* timerName, const char* timerFrmt): startTime(spring_gettime())
{
	strncpy(name, timerName, sizeof(name));
	strncpy(frmt, timerFrmt, sizeof(frmt));

	name[sizeof(name) - 1] = 0;
	frmt[sizeof(frmt) - 1] = 0;
}

ScopedOnceTimer::ScopedOnceTimer(const std::string& timerName, const char* timerFrmt): startTime(spring_gettime())
{
	strncpy(name, timerName.c_str(), sizeof(name));
	strncpy(frmt, timerFrmt        , sizeof(frmt));

	name[sizeof(name) - 1] = 0;
	frmt[sizeof(frmt) - 1] = 0;
}

ScopedOnceTimer::~ScopedOnceTimer()
{
	LOG(frmt, __func__, name, int(GetDuration().toMilliSecsi()));
}

spring_time ScopedOnceTimer::GetDuration() const
{
	return spring_difftime(spring_gettime(), startTime);
}



ScopedMtTimer::ScopedMtTimer(unsigned _nameHash, bool _autoShowGraph)
	: BasicTimer(_nameHash)
	, autoShowGraph(_autoShowGraph)
{
}

ScopedMtTimer::~ScopedMtTimer()
{
	// worker-pool zones are not part of the main-thread self-time tree; self == inclusive
	const spring_time incl = GetDuration();
	CTimeProfiler::GetInstance().AddTime(nameHash, startTime, incl, incl, autoShowGraph, false, true);
}



//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CTimeProfiler::CTimeProfiler()
{
	// self
	RegisterTimer("Misc::Profiler::AddTime");
	// specials (conditional on LuaContextData)
	RegisterTimer("Lua::Callins::Synced");
	RegisterTimer("Lua::Callins::Unsynced");
	RegisterTimer("Lua::CollectGarbage::Synced");
	RegisterTimer("Lua::CollectGarbage::Unsynced");
	ResetState();
}

#if 1
CTimeProfiler::~CTimeProfiler() = default;
#else
CTimeProfiler::~CTimeProfiler()
{
	// should not be needed, destructor runs after main returns and all threads are gone
	std::lock_guard<ProfileMutexType> lock(profileMutex);
}
#endif


CTimeProfiler& CTimeProfiler::GetInstance()
{
	static CTimeProfiler tp;
	return tp;
}

bool CTimeProfiler::RegisterTimer(const char* timerName)
{
	const unsigned nameHash = hashString(timerName);

	std::lock_guard<HashNamMutexType> lock(hashToNameMutex);

	const auto iter = hashToName.find(nameHash);

	if (iter == hashToName.end()) {
		hashToName.emplace(nameHash, timerName);
		return true;
	}
	if (iter->second == timerName)
		return true;

	LOG_L(L_ERROR, "[%s] timer hash collision: %s <=> %s", __func__, timerName, iter->second.c_str());
	assert(false);
	return false;
}

bool CTimeProfiler::UnRegisterTimer(const char* timerName)
{
	const unsigned nameHash = hashString(timerName);

	std::lock_guard<HashNamMutexType> lock(hashToNameMutex);

	const auto iter = hashToName.find(nameHash);

	if (iter == hashToName.end())
		return false;

	hashToName.erase(iter);
	return true;
}


void CTimeProfiler::ResetState() {
	// grab lock; ThreadPool workers might already be running SCOPED_MT_TIMER
	std::lock_guard<ProfileMutexType> lock(profileMutex);

	profiles.clear();
	profiles.reserve(128);
	sortedProfiles.clear();
	#ifdef THREADPOOL
	threadProfiles.clear();
	threadProfiles.resize(ThreadPool::GetMaxThreads());
	#endif

	profileColorRNG.Seed(spring_tomsecs(lastBigUpdate = spring_gettime()));

	currentPosition = 0;
	resortProfiles = 0;

	enabled = false;
}

void CTimeProfiler::ToggleLock(bool lock)
{
	if (lock) {
		profileMutex.lock();
	} else {
		profileMutex.unlock();
	}
}


void CTimeProfiler::Update()
{
	if (!enabled) {
		UpdateRaw();
		ResortProfilesRaw();
		RefreshProfilesRaw();
		return;
	}

	// FIXME: non-locking threadsafe
	std::lock_guard<ProfileMutexType> lock(profileMutex);

	if (sortingType != ST_ALPHABETICAL)
		++resortProfiles;

	UpdateRaw();
	ResortProfilesRaw();
	RefreshProfilesRaw();
	// Now cleanup old thread profiles, no need to do it if
	// disabled since won't be accepting data.
	CleanupOldThreadProfiles();
}

void CTimeProfiler::UpdateRaw()
{
	currentPosition += 1;
	currentPosition &= (TimeRecord::numFrames - 1);

	for (auto& pi: profiles) {
		pi.second.frames[currentPosition] = spring_notime;
		pi.second.selfFrames[currentPosition] = spring_notime;
	}

	const spring_time curTime = spring_gettime();
	const float timeDiff = spring_diffmsecs(curTime, lastBigUpdate);

	if (timeDiff > 500.0f) {
		// update percentages and peaks twice every second
		for (auto& pi: profiles) {
			auto& p = pi.second;

			p.stats.y = spring_tomsecs(p.current) / timeDiff;
			p.current = spring_notime;

			p.selfPercent = spring_tomsecs(p.selfCurrent) / timeDiff;
			p.selfCurrent = spring_notime;

			p.newLagPeak = false;
			p.newPeak = (p.stats.y > p.stats.z);

			if (!p.newPeak)
				continue;

			p.stats.z = p.stats.y;

		}

		lastBigUpdate = curTime;
	}

	if (curTime.toSecsi() % 6 == 0) {
		for (auto& pi: profiles) {
			(pi.second).stats.x *= 0.5f;
		}
	}
}

void CTimeProfiler::ResortProfilesRaw()
{
	if (resortProfiles > 0) {
		resortProfiles = 0;

		sortedProfiles.clear();
		sortedProfiles.reserve(profiles.size());

		// either caller already has lock, or we are disabled and thread-safe
		{
			std::lock_guard<HashNamMutexType> lock(hashToNameMutex);

			for (const auto& profile: profiles) {
				const auto iter = hashToName.find(profile.first);

				if (iter == hashToName.end()) {
					LOG_L(L_ERROR, "[%s] timer with hash %u wasn't registered", __func__, profile.first);
					assert(false);
					hashToName.emplace(profile.first, "???");
					continue;
				}

				sortedProfiles.emplace_back(iter->second, profile.second);
			}
		}

		const auto& sortFunc = SortingFunctions[sortingType];
		std::sort(sortedProfiles.begin(), sortedProfiles.end(), sortFunc);
	}
}


void CTimeProfiler::RefreshProfiles()
{
	// ProfileDrawer calls this, and is only enabled when we are
	assert(enabled);

	// lock so nothing modifies *unsorted* profiles during the refresh
	std::lock_guard<ProfileMutexType> lock(profileMutex);

	RefreshProfilesRaw();
}

void CTimeProfiler::RefreshProfilesRaw()
{
	// either called from ProfileDrawer or from Update; the latter
	// makes the "/debuginfo profiling" command work when disabled
	for (auto& sortedProfile: sortedProfiles) {
		TimeRecord& rec = sortedProfile.second;

		const bool showGraph = rec.showGraph;

		rec = profiles[hashString(sortedProfile.first.c_str())];
		rec.showGraph = showGraph;
	}
}

void CTimeProfiler::CleanupOldThreadProfiles()
{
	#ifdef THREADPOOL
	const spring_time curTime = spring_gettime();
	const spring_time maxTime = spring_secs(MAX_THREAD_HIST_TIME);
	const size_t numThreads = std::min(threadProfiles.size(), (size_t)ThreadPool::GetNumThreads());
	size_t i = 0;

	for (auto& threadProf: threadProfiles) {
		if (i++ >= numThreads) break;
		while (!threadProf.empty() && (curTime - threadProf.front().second) > maxTime) {
			threadProf.pop_front();
		}
	}
	#endif
}

const CTimeProfiler::TimeRecord& CTimeProfiler::GetTimeRecord(const char* name) const
{
	// if disabled, only special timers can pass AddTime
	// all of those are non-threaded, so no need to lock
	if (!enabled)
		return (GetTimeRecordRaw(name));

	std::lock_guard<ProfileMutexType> lock(profileMutex);

	return (GetTimeRecordRaw(name));
}


void CTimeProfiler::AddTime(
	const unsigned nameHash,
	const spring_time startTime,
	const spring_time deltaTime,
	const spring_time selfTime,
	const bool showGraph,
	const bool specialTimer,
	const bool threadTimer
) {
	const spring_time t0 = spring_now();

	if (!enabled) {
		if (!specialTimer)
			return;

		assert(!threadTimer);
		AddTimeRaw(nameHash, startTime, deltaTime, selfTime, showGraph, threadTimer);
		AddTimeRaw(hashString("Misc::Profiler::AddTime"), t0, spring_now() - t0, spring_now() - t0, false, false);
		return;
	}

	// acquire lock at the start; one inserting thread could
	// cause a profile rehash and invalidate <pi> for another
	std::lock_guard<ProfileMutexType> lock(profileMutex);

	AddTimeRaw(nameHash, startTime, deltaTime, selfTime, showGraph, threadTimer);
	AddTimeRaw(hashString("Misc::Profiler::AddTime"), t0, spring_now() - t0, spring_now() - t0, false, false);
}

void CTimeProfiler::AddTimeRaw(
	const unsigned nameHash,
	const spring_time startTime,
	const spring_time deltaTime,
	const spring_time selfTime,
	const bool showGraph,
	const bool threadTimer
) {
#ifdef THREADPOOL
	if (threadTimer)
		threadProfiles[ThreadPool::GetThreadNum()].emplace_back(startTime, spring_gettime());
#endif

	auto pi = profiles.find(nameHash);
	auto& p = (pi != profiles.end()) ? pi->second: profiles[nameHash];

	// these are 0 if just created, works for both paths
	p.total   += deltaTime;
	p.current += deltaTime;

	p.selfTotal   += selfTime;
	p.selfCurrent += selfTime;

	if (tlInSimFrame) {
		p.totalSim     += deltaTime;
		p.selfTotalSim += selfTime;
	}

	p.newLagPeak = (p.stats.x > 0.0f && deltaTime.toMilliSecsf() > p.stats.x);
	p.stats.x    = std::max(p.stats.x, deltaTime.toMilliSecsf());

	if (pi != profiles.end()) {
		// profile already exists, add dt
		p.frames[currentPosition]     += deltaTime;
		p.selfFrames[currentPosition] += selfTime;
	} else {
		// new profile, new color
		p.color.x = profileColorRNG.NextFloat();
		p.color.y = profileColorRNG.NextFloat();
		p.color.z = profileColorRNG.NextFloat();
		p.showGraph = showGraph;

		resortProfiles += 1;
	}
}

CTimeProfiler::CalloutCountSnapshotFn CTimeProfiler::calloutCountSnapshotFn = nullptr;

void CTimeProfiler::StartDump(int f0, int f1, const std::string& path)
{
	if (dumpActive) {
		LOG_L(L_WARNING, "[TimeProfiler::StartDump] a dump is already active (-> %s)", dumpPath.c_str());
		return;
	}
	if (f1 < f0) {
		LOG_L(L_ERROR, "[TimeProfiler::StartDump] end frame %d precedes start frame %d", f1, f0);
		return;
	}

	dumpActive = true;
	dumpStartFrame = f0;
	dumpEndFrame = f1;
	dumpPath = path;
	dumpRows.clear();
	dumpPrevTimes.clear();
	dumpPrevCounts.clear();
	g_foldedMicros.clear();
	g_foldedActive = true;

	// record self/inclusive for every zone, not just the special Lua timers,
	// regardless of whether the on-screen profiler is up; restore on stop
	dumpSavedEnabled = enabled;
	SetEnabled(true);

	LOG("[TimeProfiler::StartDump] armed: sim frames %d..%d -> %s", f0, f1, path.c_str());
}

void CTimeProfiler::DumpFrame(int frameNum)
{
	if (!dumpActive)
		return;
	if (frameNum < dumpStartFrame)
		return;

	{
		std::lock_guard<ProfileMutexType> lock(profileMutex);

		// emit per-name deltas of the cumulative inclusive/self totals; a name's
		// first sampled frame only seeds its baseline (no row)
		for (const auto& pi: profiles) {
			const unsigned nameHash = pi.first;
			const TimeRecord& p = pi.second;

			auto it = dumpPrevTimes.find(nameHash);
			if (it == dumpPrevTimes.end()) {
				dumpPrevTimes.emplace(nameHash, DumpPrev{p.total, p.selfTotal, p.totalSim, p.selfTotalSim});
				continue;
			}

			const float inclMs    = (p.total        - it->second.total   ).toMilliSecsf();
			const float selfMs    = (p.selfTotal     - it->second.self    ).toMilliSecsf();
			const float inclSimMs = (p.totalSim      - it->second.totalSim).toMilliSecsf();
			const float selfSimMs = (p.selfTotalSim  - it->second.selfSim ).toMilliSecsf();
			it->second = DumpPrev{p.total, p.selfTotal, p.totalSim, p.selfTotalSim};

			if (inclMs <= 0.0f && selfMs <= 0.0f)
				continue;

			dumpRows.push_back({frameNum, nameHash, selfMs, inclMs, selfSimMs, inclSimMs, uint64_t(0)});
		}
	}

	// per-callout stats (REGISTER_LUA_CFUNC trampoline), if the Lua layer
	// registered a snapshot hook; same first-frame-seeds-baseline delta scheme.
	// a row with count>0 is a callout body, not an engine zone; (deep mode) its
	// self/incl columns carry self vs subtree body wall-time, just like a zone, so
	// summing self over callout rows is double-count-free.
	if (calloutCountSnapshotFn != nullptr) {
		static std::vector<CalloutStat> snap;
		snap.clear();
		calloutCountSnapshotFn(snap);

		for (const CalloutStat& s: snap) {
			auto it = dumpPrevCounts.find(s.nameHash);
			if (it == dumpPrevCounts.end()) {
				dumpPrevCounts.emplace(s.nameHash, DumpPrevCallout{s.count, s.bodySelf, s.bodyIncl, s.bodySelfSim, s.bodyInclSim});
				continue;
			}

			const uint64_t dCount  = s.count - it->second.count;
			const float selfMs     = (s.bodySelf    - it->second.bodySelf   ).toMilliSecsf();
			const float inclMs     = (s.bodyIncl    - it->second.bodyIncl   ).toMilliSecsf();
			const float selfSimMs  = (s.bodySelfSim - it->second.bodySelfSim).toMilliSecsf();
			const float inclSimMs  = (s.bodyInclSim - it->second.bodyInclSim).toMilliSecsf();
			it->second = DumpPrevCallout{s.count, s.bodySelf, s.bodyIncl, s.bodySelfSim, s.bodyInclSim};

			if (dCount == 0 && inclMs <= 0.0f)
				continue;

			dumpRows.push_back({frameNum, s.nameHash, selfMs, inclMs, selfSimMs, inclSimMs, dCount});
		}
	}

	if (frameNum >= dumpEndFrame)
		StopDump();
}

void CTimeProfiler::StopDump()
{
	if (!dumpActive)
		return;

	dumpActive = false;
	g_foldedActive = false;
	SetEnabled(dumpSavedEnabled);

	std::ofstream f(dumpPath);
	if (!f.good()) {
		LOG_L(L_ERROR, "[TimeProfiler::StopDump] cannot open %s for writing", dumpPath.c_str());
		dumpRows.clear();
		g_foldedMicros.clear();
		return;
	}

	f << "frame,name,self_ms,incl_ms,self_sim_ms,incl_sim_ms,count\n";
	{
		std::lock_guard<HashNamMutexType> lock(hashToNameMutex);
		for (const DumpRow& r: dumpRows) {
			const auto it = hashToName.find(r.nameHash);
			const char* nm = (it != hashToName.end()) ? it->second.c_str() : "???";
			f << r.frame << ',' << nm << ',' << r.selfMs << ',' << r.inclMs
			  << ',' << r.selfSimMs << ',' << r.inclSimMs << ',' << r.count << '\n';
		}
	}

	LOG("[TimeProfiler::StopDump] wrote %u rows (frames %d..%d) to %s",
		unsigned(dumpRows.size()), dumpStartFrame, dumpEndFrame, dumpPath.c_str());

	// sibling collapsed-stack file (Brendan Gregg "folded" format) for flamegraph
	// tools: each line is "root;child;...;leaf <self_microsecs>" over the whole dump
	if (!g_foldedMicros.empty()) {
		const std::string foldedPath = dumpPath + ".folded";
		std::ofstream ff(foldedPath);
		if (ff.good()) {
			std::lock_guard<HashNamMutexType> lock(hashToNameMutex);
			for (const auto& kv: g_foldedMicros) {
				const std::vector<unsigned>& path = kv.first;
				for (size_t i = 0; i < path.size(); ++i) {
					const auto it = hashToName.find(path[i]);
					ff << (i ? ";" : "") << ((it != hashToName.end()) ? it->second.c_str() : "???");
				}
				ff << ' ' << kv.second << '\n';
			}
			LOG("[TimeProfiler::StopDump] wrote %u folded stacks to %s",
				unsigned(g_foldedMicros.size()), foldedPath.c_str());
		} else {
			LOG_L(L_ERROR, "[TimeProfiler::StopDump] cannot open %s for writing", foldedPath.c_str());
		}
	}

	dumpRows.clear();
	dumpPrevTimes.clear();
	dumpPrevCounts.clear();
	g_foldedMicros.clear();
}

void CTimeProfiler::PrintProfilingInfo() const
{
	if (sortedProfiles.empty())
		return;

	LOG("%35s|%18s|%s", "Part", "Total Time", "Time of the last 0.5s");

	for (const auto& sortedProfile: sortedProfiles) {
		const std::string& name = sortedProfile.first;
		const TimeRecord& tr = sortedProfile.second;

		LOG("%35s %16.2fms %5.2f%%", name.c_str(), tr.total.toMilliSecsf(), tr.stats.y * 100);
	}
}

