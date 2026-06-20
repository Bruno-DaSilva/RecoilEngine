/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_CALL_IN_CHECK_H
#define LUA_CALL_IN_CHECK_H

#include "System/TimeProfiler.h"
#include "LuaUtils.h"

#if DEBUG_LUA
#  define LUA_CALL_IN_CHECK_NAMED(L, name, ...) SCOPED_SPECIAL_TIMER_NOREG(name); LuaUtils::ScopedStackChecker ciCheck((L));
#else
#  define LUA_CALL_IN_CHECK_NAMED(L, name, ...) SCOPED_SPECIAL_TIMER_NOREG(name);
#endif

// nested per-callin timer (child of the synced/unsynced aggregate above), named
// "Lua::Callins::{Synced,Unsynced}::<callin>" via __func__ at the call site. Non-
// special, so it only does work when the profiler is enabled (or a dump is active)
// — no per-callin overhead during normal play.
#define SCOPED_CALLIN_TIMER(L) \
	static CallinTimerNames __citn(__func__); \
	ScopedTimer __callinScopedTimer((GetLuaContextData(L)->synced)? __citn.syncedHash: __citn.unsyncedHash, false, false)

#define LUA_CALL_IN_CHECK(L, ...) \
	LUA_CALL_IN_CHECK_NAMED(L, (GetLuaContextData(L)->synced)? "Lua::Callins::Synced": "Lua::Callins::Unsynced", __VA_ARGS__); \
	SCOPED_CALLIN_TIMER(L); \
	ScopedZoneStackGuard __zoneStackGuard;
#endif /* LUA_CALL_IN_CHECK_H */
