/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <cstdio>
#include <cstring>

#include "LuaHandle.h"
#include "LuaInclude.h"

#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Misc/CollisionVolume.h" // WORLD_TO_OBJECT_SPACE
#include "Sim/Units/Unit.h" // LOS_INLOS / LOS_INRADAR bits
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "System/TimeProfiler.h" // ScopedDrawCallinContext

namespace {
	struct Pov {
		int readAllyTeam;
		bool fullRead;
	};

	inline Pov HandlePov(lua_State* L)
	{
		return { CLuaHandle::GetHandleReadAllyTeam(L), CLuaHandle::GetHandleFullRead(L) };
	}

	// LuaSyncedRead::ParseRawUnit argument semantics (error text included)
	inline int ParseUnitIDSynced(lua_State* L, const char* caller, int index)
	{
		if (!lua_isnumber(L, index))
			luaL_error(L, "[%s] unitID (arg #%d) not a number\n", caller, index);

		return lua_toint(L, index);
	}

	// LuaUnsyncedRead::ParseUnit argument semantics (error text included)
	inline int ParseUnitIDUnsynced(lua_State* L, const char* caller, int index)
	{
		if (!lua_isnumber(L, index))
			luaL_error(L, "%s(): unitID not a number", caller);

		return lua_toint(L, index);
	}

	// dual-run slot compare: both paths push plain values (numbers, booleans,
	// nils), compared bit-exactly; lua_tostring is avoided since it would
	// convert number slots in place
	bool SlotsEqual(lua_State* L, int a, int b)
	{
		const int ta = lua_type(L, a);
		const int tb = lua_type(L, b);

		if (ta != tb)
			return false;

		switch (ta) {
			case LUA_TNUMBER: {
				const double da = lua_tonumber(L, a);
				const double db = lua_tonumber(L, b);
				return (std::memcmp(&da, &db, sizeof(double)) == 0);
			}
			case LUA_TBOOLEAN:
				return (lua_toboolean(L, a) == lua_toboolean(L, b));
			case LUA_TNIL:
				return true;
			default:
				return (lua_rawequal(L, a, b) != 0);
		}
	}

	void DescribeSlot(lua_State* L, int idx, char* buf, size_t n)
	{
		switch (lua_type(L, idx)) {
			case LUA_TNUMBER:  snprintf(buf, n, "%.17g", lua_tonumber(L, idx)); break;
			case LUA_TBOOLEAN: snprintf(buf, n, "%s", lua_toboolean(L, idx) ? "true" : "false"); break;
			case LUA_TNIL:     snprintf(buf, n, "nil"); break;
			default:           snprintf(buf, n, "<%s>", lua_typename(L, lua_type(L, idx))); break;
		}
	}
}


bool LuaSnapshotServe::ShouldServe(lua_State* L)
{
	return (!CLuaHandle::GetHandleSynced(L) && ScopedDrawCallinContext::InDrawCallin());
}


int LuaSnapshotServe::Route(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn)
{
	if (!ShouldServe(L))
		return liveFn(L, caller);

	if (!snapshotDiffGate.Armed())
		return snapFn(L, caller);

	// armed: run BOTH real paths, bit-compare their actual return slots
	// (masking and gating included by construction), serve the snapshot values.
	// The live returns are stashed in the registry and the stack is reset to
	// the original arguments before the twin runs: optional-arg reads
	// (luaL_optboolean at index nargs+1..) would otherwise see the live
	// returns instead of "none" (found live: a 1-arg GetUnitPosition call made
	// the twin read the live path's x as its midPos flag).
	const int base = lua_gettop(L);
	const int liveN = liveFn(L, caller);

	lua_createtable(L, liveN, 0);
	for (int i = 1; i <= liveN; ++i) {
		lua_pushvalue(L, base + i);
		lua_rawseti(L, -2, i);
	}
	const int liveRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_settop(L, base); // args only, exactly as liveFn saw them

	const int snapN = snapFn(L, caller);

	lua_rawgeti(L, LUA_REGISTRYINDEX, liveRef); // live-returns table at base+snapN+1
	const int tbl = base + snapN + 1;

	bool equal = (liveN == snapN);
	char detail[160] = "";

	if (!equal) {
		snprintf(detail, sizeof(detail), "return counts differ: live=%d snap=%d", liveN, snapN);
	} else {
		for (int i = 1; i <= liveN; ++i) {
			lua_rawgeti(L, tbl, i); // live value i at tbl+1
			const bool slotEqual = SlotsEqual(L, tbl + 1, base + i);

			if (!slotEqual && equal) {
				equal = false;
				char liveDesc[48];
				char snapDesc[48];
				DescribeSlot(L, tbl + 1, liveDesc, sizeof(liveDesc));
				DescribeSlot(L, base + i, snapDesc, sizeof(snapDesc));
				snprintf(detail, sizeof(detail), "return %d differs: live=%s snap=%s", i, liveDesc, snapDesc);
			}
			lua_pop(L, 1);
		}
	}

	snapshotDiffGate.CountCallout(caller, equal, detail);

	lua_pop(L, 1); // live-returns table
	luaL_unref(L, LUA_REGISTRYINDEX, liveRef);

	return snapN;
}


/******************************************************************************
 * Serving twins. Each is a line-by-line mirror of the corresponding live body
 * (LuaSyncedRead.cpp / LuaUnsyncedRead.cpp) with sim-object reads replaced by
 * snapshot row reads; float expression order is kept identical so the results
 * are bit-equal. The armed Route() dual-run is what keeps them honest.
 ******************************************************************************/

// mirror of GetSolidObjectPosition(L, ParseUnit(...), false)
int LuaSnapshotServe::GetUnitPosition(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror: no such unit / not visible => nil (stale/nil contract)
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	float3 errorVec;

	if (!rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		errorVec = rows.LuaErrorVector(unitID, pov.readAllyTeam, pov.fullRead);

	// NOTE: must be read before any pushing (same caveat as the live body)
	const bool returnMidPos = luaL_optboolean(L, 2, false);
	const bool returnAimPos = luaL_optboolean(L, 3, false);

	lua_pushnumber(L, rows.pos[unitID].x + errorVec.x);
	lua_pushnumber(L, rows.pos[unitID].y + errorVec.y);
	lua_pushnumber(L, rows.pos[unitID].z + errorVec.z);

	if (returnMidPos) {
		lua_pushnumber(L, rows.midPos[unitID].x + errorVec.x);
		lua_pushnumber(L, rows.midPos[unitID].y + errorVec.y);
		lua_pushnumber(L, rows.midPos[unitID].z + errorVec.z);
	}
	if (returnAimPos) {
		lua_pushnumber(L, rows.aimPos[unitID].x + errorVec.x);
		lua_pushnumber(L, rows.aimPos[unitID].y + errorVec.y);
		lua_pushnumber(L, rows.aimPos[unitID].z + errorVec.z);
	}

	return (3 + (3 * returnMidPos) + (3 * returnAimPos));
}

// mirror of LuaSyncedRead::GetUnitHealth
int LuaSnapshotServe::GetUnitHealth(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// immutable def-table data, not sim state: direct deref is allowed
	const UnitDef* ud = unitDefHandler->GetUnitDefByID(rows.defID[unitID]);
	const bool enemyUnit = !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead);

	if (ud->hideDamage && enemyUnit) {
		lua_pushnil(L);
		lua_pushnil(L);
		lua_pushnil(L);
	} else if (!enemyUnit || (ud->decoyDef == nullptr)) {
		lua_pushnumber(L, rows.health[unitID]);
		lua_pushnumber(L, rows.maxHealth[unitID]);
		lua_pushnumber(L, rows.paralyzeDamage[unitID]);
	} else {
		const float scale = (ud->decoyDef->health / ud->health);
		lua_pushnumber(L, scale * rows.health[unitID]);
		lua_pushnumber(L, scale * rows.maxHealth[unitID]);
		lua_pushnumber(L, scale * rows.paralyzeDamage[unitID]);
	}
	lua_pushnumber(L, rows.captureProgress[unitID]);
	lua_pushnumber(L, rows.buildProgress[unitID]);
	return 5;
}

// mirror of LuaSyncedRead::GetUnitIsStunned
int LuaSnapshotServe::GetUnitIsStunned(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const bool stunned = (rows.stunned[unitID] != 0);
	const bool beingBuilt = (rows.beingBuilt[unitID] != 0);

	lua_pushboolean(L, stunned || beingBuilt);
	lua_pushboolean(L, stunned);
	lua_pushboolean(L, beingBuilt);
	return 3;
}

// mirror of LuaUnsyncedRead::GetUnitViewPosition; the draw position itself is
// drawer-owned (id-keyed) state, only the gate/mask reads were live sim state
int LuaSnapshotServe::GetUnitViewPosition(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseUnit gate mirror (no ally bypass: reads the
	// readAllyTeam losStatus row directly, like the live helper)
	if (!rows.Valid(unitID))
		return 0;
	if (pov.readAllyTeam < 0) {
		if (!pov.fullRead)
			return 0;
	} else if ((rows.LosStatus(unitID, pov.readAllyTeam) & (LOS_INLOS | LOS_INRADAR)) == 0) {
		return 0;
	}

	float3 unitPos;

	if (luaL_optboolean(L, 2, false)) {
		// CUnitDrawer::GetObjDrawMidPos mirror: drawPos + object-space relMidPos
		unitPos = CUnitDrawer::GetDrawPos(unitID) + rows.ObjectSpaceVec(unitID, WORLD_TO_OBJECT_SPACE * rows.relMidPos[unitID]);
	} else {
		unitPos = CUnitDrawer::GetDrawPos(unitID);
	}

	const float3 errorVec = rows.LuaErrorVector(unitID, pov.readAllyTeam, pov.fullRead);

	lua_pushnumber(L, unitPos.x + errorVec.x);
	lua_pushnumber(L, unitPos.y + errorVec.y);
	lua_pushnumber(L, unitPos.z + errorVec.z);
	return 3;
}
