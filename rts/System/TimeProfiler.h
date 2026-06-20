/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef TIME_PROFILER_H
#define TIME_PROFILER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <deque>
#include <vector>
#include <array>
#include <utility>

#include "System/Misc/SpringTime.h"
#include "System/Misc/NonCopyable.h"
#include "System/float3.h"
#include "System/StringHash.h"
#include "System/UnorderedMap.hpp"

#include "System/Misc/TracyDefs.h"

// disable these for minimal profiling; all special
// timers contribute even when profiler is disabled
// NB: names are assumed to be compile-time literals
#define SCOPED_TIMER(      name)  ZoneScopedNC(name, tracy::Color::Goldenrod); static TimerNameRegistrar __tnr(name); ScopedTimer __scopedTimer(hashString(name));
#define SCOPED_TIMER_NOREG(name)  ZoneScopedNC(name, tracy::Color::Goldenrod);                                        ScopedTimer __scopedTimer(hashString(name));

#define SCOPED_SPECIAL_TIMER(      name)  static TimerNameRegistrar __stnr(name); ScopedTimer __scopedTimer(hashString(name), false, true);
#define SCOPED_SPECIAL_TIMER_NOREG(name)                                          ScopedTimer __scopedTimer(hashString(name), false, true);

#define SCOPED_MT_TIMER(name)  ScopedMtTimer __scopedTimer(hashString(name));

#define SCOPED_ONCE_TIMER(name) ZoneScopedNC(name, tracy::Color::Purple); ScopedOnceTimer __timer(name);

static constexpr float MAX_THREAD_HIST_TIME = 0.5f; // secs

class BasicTimer : public spring::noncopyable
{
public:
	//BasicTimer(const spring_time time): nameHash(0), startTime(time) {}
	BasicTimer(unsigned _nameHash) : nameHash(_nameHash), startTime(spring_gettime()) { }

	spring_time GetDuration() const;

protected:
	const unsigned nameHash;
	const spring_time startTime;
};


/**
 * @brief Time profiling helper class
 *
 * Construct an instance of this class where you want to begin time measuring,
 * and destruct it at the end (or let it be autodestructed).
 */
class ScopedTimer : public BasicTimer
{
public:
	ScopedTimer(const unsigned _nameHash, bool _autoShowGraph = false, bool _specialTimer = false);
	~ScopedTimer();

private:
	const bool autoShowGraph;
	const bool specialTimer;
	// whether this timer participates in the self-time stack / records at all:
	// special timers always do (they report even when the profiler is disabled);
	// ordinary timers only when the profiler is enabled, so the whole attribution
	// machinery is a no-op (cheaper than the old refCounters path) during normal
	// play. Captured at construction so push/pop stay balanced across a toggle.
	const bool tracked;
};


class ScopedMtTimer : public BasicTimer
{
public:
	ScopedMtTimer(const unsigned _nameHash, bool _autoShowGraph = false);
	~ScopedMtTimer();

private:
	const bool autoShowGraph;
};



/**
 * @brief print passed time to infolog
 */
class ScopedOnceTimer
{
public:
	ScopedOnceTimer(const std::string& name, const char* frmt = "[%s][%s] %ims");
	ScopedOnceTimer(const char* name, const char* frmt = "[%s][%s] %ims");
	~ScopedOnceTimer();

	spring_time GetDuration() const;

protected:
	const spring_time startTime;

	char name[128];
	char frmt[128];
};



class CTimeProfiler
{
public:
	CTimeProfiler();
	~CTimeProfiler();

	static CTimeProfiler& GetInstance();

	static bool RegisterTimer(const char* name);
	static bool UnRegisterTimer(const char* name);

	struct TimeRecord {
		TimeRecord() {
			frames.fill(spring_time(0));
			selfFrames.fill(spring_time(0));
		}

		static constexpr unsigned numFrames = 128;

		spring_time total = spring_notime;
		spring_time current = spring_notime;
		std::array<spring_time, numFrames> frames;

		// self-time (inclusive span minus time spent in differently-named
		// child zones); parallels total/current/frames above
		spring_time selfTotal = spring_notime;
		spring_time selfCurrent = spring_notime;
		std::array<spring_time, numFrames> selfFrames;

		// per-0.5s self-time percentage (parallels stats.y)
		float selfPercent = 0.0f;

		// sim-phase portion of the cumulative totals (the rest is draw/update);
		// charged when the zone ran inside CGame::SimFrame. total/selfTotal are
		// the sim+draw sums, so draw = total - totalSim. Used by the dump to
		// answer "is this on the ~33ms sim critical path".
		spring_time totalSim = spring_notime;
		spring_time selfTotalSim = spring_notime;

		// .x := maximum dt, .y := time-percentage, .z := peak-percentage
		float3 stats;
		float3 color;

		bool newPeak = false;
		bool newLagPeak = false;
		bool showGraph = false;
	};

	enum SortType {
		ST_ALPHABETICAL = 0,
		ST_TOTALTIME    = 1,
		ST_CURRENTTIME  = 2,
		ST_MAXTIME      = 3,
		ST_LAG          = 4,
		ST_COUNT        = 5
	};

	using TimeRecordPair = std::pair<std::string, TimeRecord>;
	using ProfileSortFunc = bool(*)(const TimeRecordPair&, const TimeRecordPair&);

	static const std::array<ProfileSortFunc, SortType::ST_COUNT> SortingFunctions;
public:
	std::vector< std::pair<std::string, TimeRecord> >& GetSortedProfiles() { return sortedProfiles; }
	std::vector< std::deque< std::pair<spring_time, spring_time> > >& GetThreadProfiles() { return threadProfiles; }

	size_t GetNumSortedProfiles() const { return (sortedProfiles.size()); }
	size_t GetNumThreadProfiles() const { return (threadProfiles.size()); }

	float GetTimePercentage(const char* name) const { return (GetTimeRecord(name).stats.y); }
	float GetTimePercentageRaw(const char* name) const { return (GetTimeRecordRaw(name).stats.y); }

	const TimeRecord& GetTimeRecord(const char* name) const;
	const TimeRecord& GetTimeRecordRaw(const char* name) const {
		// do not default-create keys, breaks resorting
		const auto it = profiles.find(hashString(name));
		const static TimeRecord tr;

		if (it == profiles.end())
			return tr;

		return (it->second);
	}

	void ToggleLock(bool lock);
	void ResetState();
	void ResetPeaks() {
		ToggleLock(true);

		for (auto& p: profiles)
			p.second.stats.z = 0.0f;

		ToggleLock(false);
	}

	void SetSortingType(SortType st) {
		sortingType = st;
		++resortProfiles;
	}

	void Update();
	void UpdateRaw();

	void ResortProfilesRaw();
	void RefreshProfiles();
	void RefreshProfilesRaw();
	void CleanupOldThreadProfiles();

	void SetEnabled(bool b) { enabled = b; }
	bool IsEnabled() const { return enabled.load(std::memory_order_relaxed); }
	void PrintProfilingInfo() const;

	// Manual self-time zone open/close for the Lua Spring.Profiler primitive;
	// nests in the same per-thread stack as ScopedTimer. PopZone closes the
	// most-recently-pushed zone. Main-thread only.
	void PushZone(unsigned nameHash);
	void PopZone();

	// Unified flamegraph: while a dump is active, the deep-mode callout trampoline folds
	// each callout's self-time as a leaf under the currently-open zone path, and charges
	// a top-level callout's full inclusive to that zone so its own folded self excludes
	// the callouts (no double count). calloutPath is the stack of currently-open callout
	// name-hashes (leaf last); topLevel means this callout is directly under a zone.
	static bool IsFoldedActive();
	static void FoldCalloutSample(const std::vector<unsigned>& calloutPath, spring_time self, spring_time incl, bool topLevel);

	// Frame-range dump: force-enable the profiler over [f0,f1] and append each
	// sim frame's per-name {self, inclusive, count} to a buffer, serialized to
	// <path> (CSV) when the range completes. Engine-side so it works headless
	// and while the on-screen profiler is disabled. DumpFrame() is a no-op
	// unless a dump is active. See TimeProfiler.cpp for the format.
	void StartDump(int f0, int f1, const std::string& path);
	void DumpFrame(int frameNum);
	bool IsDumpActive() const { return dumpActive; }

	// Optional hook the Lua layer registers so per-callout stats (the
	// REGISTER_LUA_CFUNC trampoline counters, and body-time in deep mode) land in
	// the dump. Fills <out> with cumulative per-callout-name values. nullptr =>
	// no callout rows. body* are zero unless deep mode (level 2) is on.
	//
	// bodySelf excludes time spent in nested callouts (a higher-order callout like
	// gl.RenderToTexture pcalls a Lua fn full of other callouts; those are timed in
	// their own buckets, so charging them here too would double-count). bodyIncl is
	// the full subtree. For leaf callouts the two are equal; summing bodySelf over
	// all names is the honest, non-double-counted total callout time.
	struct CalloutStat {
		unsigned nameHash;
		uint64_t count;
		spring_time bodySelf;    // cumulative body wall-time, nested callouts excluded
		spring_time bodyIncl;    // cumulative body wall-time, nested callouts included
		spring_time bodySelfSim; // sim-phase portion of bodySelf
		spring_time bodyInclSim; // sim-phase portion of bodyIncl
	};
	using CalloutCountSnapshotFn = void (*)(std::vector<CalloutStat>& out);
	static void SetCalloutCountSnapshotFn(CalloutCountSnapshotFn fn) { calloutCountSnapshotFn = fn; }

	void AddTime(
		unsigned nameHash,
		const spring_time startTime,
		const spring_time deltaTime,
		const spring_time selfTime,
		const bool showGraph = false,
		const bool specialTimer = false,
		const bool threadTimer = false
	);
	void AddTimeRaw(
		unsigned nameHash,
		const spring_time startTime,
		const spring_time deltaTime,
		const spring_time selfTime,
		const bool showGraph,
		const bool threadTimer
	);

private:
	void StopDump();

	SortType sortingType = SortType::ST_ALPHABETICAL;
	spring::unordered_map<unsigned, TimeRecord> profiles;

	std::vector< std::pair<std::string, TimeRecord> > sortedProfiles;
	std::vector< std::deque< std::pair<spring_time, spring_time> > > threadProfiles;

	spring_time lastBigUpdate;

	/// increases each update, from 0 to (numFrames-1)
	unsigned currentPosition;
	unsigned resortProfiles;

	// if false, AddTime is a no-op for (almost) all timers
	std::atomic<bool> enabled;

	// frame-range dump state (main-thread only; see StartDump/DumpFrame)
	struct DumpRow {
		int frame;
		unsigned nameHash;
		float selfMs;
		float inclMs;
		float selfSimMs; // sim-phase portion of selfMs (rest is draw/update)
		float inclSimMs; // sim-phase portion of inclMs
		uint64_t count;
	};
	struct DumpPrev {
		spring_time total, self, totalSim, selfSim;
	};
	bool dumpActive = false;
	bool dumpSavedEnabled = false;
	int dumpStartFrame = 0;
	int dumpEndFrame = 0;
	std::string dumpPath;
	std::vector<DumpRow> dumpRows;
	// per-name cumulative totals at the previous sampled frame, to delta
	spring::unordered_map<unsigned, DumpPrev> dumpPrevTimes;
	// per-callout-name cumulative stats at the previous sampled frame, to delta
	struct DumpPrevCallout {
		uint64_t count;
		spring_time bodySelf, bodyIncl, bodySelfSim, bodyInclSim;
	};
	spring::unordered_map<unsigned, DumpPrevCallout> dumpPrevCounts;

	static CalloutCountSnapshotFn calloutCountSnapshotFn;
};


class TimerNameRegistrar : public spring::noncopyable
{
public:
	TimerNameRegistrar(const char* timerName) {
		CTimeProfiler::RegisterTimer(timerName);
	}
};


/**
 * Registers (once) and caches the hashes for a callin's per-callin profiling
 * timers, "Lua::Callins::Synced::<callin>" and "...Unsynced::<callin>". Used by
 * LUA_CALL_IN_CHECK to break the two synced/unsynced lumps down by callin name.
 */
class CallinTimerNames : public spring::noncopyable
{
public:
	explicit CallinTimerNames(const char* callin);
	unsigned syncedHash;
	unsigned unsyncedHash;
};


/**
 * RAII bracket marking the calling (main) thread as running inside CGame::SimFrame,
 * so AddTime can split each zone's time into the sim vs draw/update budget. Nesting
 * is supported (restores the previous value). Worker-pool threads never set it, so
 * their ScopedMtTimer time is counted as non-sim.
 */
class ScopedSimFramePhase
{
public:
	ScopedSimFramePhase();
	~ScopedSimFramePhase();

	static bool InSimFrame();
private:
	const bool prev;
};


/**
 * RAII guard for a callin: records the self-time stack depth on entry and, on
 * exit, discards any Lua zones (Spring.ProfilerPushZone) left open within the
 * callin so an unbalanced widget/gadget cannot corrupt enclosing timers. Must be
 * the last profiler object constructed in the callin (see LUA_CALL_IN_CHECK).
 */
class ScopedZoneStackGuard : public spring::noncopyable
{
public:
	ScopedZoneStackGuard();
	~ScopedZoneStackGuard();
private:
	const size_t baseDepth;
};

#endif // TIME_PROFILER_H
