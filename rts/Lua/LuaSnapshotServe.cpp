/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <cstdio>
#include <cstring>

#include "LuaHandle.h"
#include "LuaHashString.h" // HSTR_PUSH_BOOL
#include "LuaInclude.h"

#include "Game/Camera.h"
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Misc/CollisionVolume.h" // WORLD_TO_OBJECT_SPACE
#include "Sim/Units/Unit.h" // LOS_* bits
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "System/EventClient.h" // CEventClient special-team constants
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

	bool SlotsEqual(lua_State* L, int a, int b);

	// structural compare for the plain result tables some callouts return
	// ({x,y,z} arrays, {los=,radar=,typed=} maps): lua_rawequal is identity,
	// so two structurally-identical fresh tables would always "differ".
	// Shallow key sweep both ways (values recurse through SlotsEqual; nesting
	// here is scalar-only in practice).
	bool TablesEqual(lua_State* L, int a, int b)
	{
		a = luaS_absIndex(L, a);
		b = luaS_absIndex(L, b);

		// every key of a maps to an equal value in b
		lua_pushnil(L);
		while (lua_next(L, a) != 0) {
			// stack: ... key value
			lua_pushvalue(L, -2);
			lua_rawget(L, b); // ... key aValue bValue
			const bool eq = SlotsEqual(L, -2, -1);
			lua_pop(L, 2); // ... key
			if (!eq) {
				lua_pop(L, 1);
				return false;
			}
		}

		// and b has no keys a lacks
		lua_pushnil(L);
		while (lua_next(L, b) != 0) {
			lua_pushvalue(L, -2);
			lua_rawget(L, a); // ... key bValue aValue
			const bool present = !lua_isnil(L, -1);
			lua_pop(L, 2);
			if (!present) {
				lua_pop(L, 1);
				return false;
			}
		}

		return true;
	}

	// dual-run slot compare: both paths push plain values (numbers, booleans,
	// nils, small result tables), compared bit-exactly; lua_tostring is
	// avoided since it would convert number slots in place
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
			case LUA_TTABLE:
				return TablesEqual(L, a, b);
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

// mirror of LuaSyncedRead::GetUnitLosState (incl. its GetEffectiveLosAllyTeam
// helper, which can raise "Invalid allyTeam" exactly like the live path)
int LuaSnapshotServe::GetUnitLosState(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// GetEffectiveLosAllyTeam mirror (teamHandler.IsValidAllyTeam == in [0, numAllyTeams))
	int allyTeamID = 0;
	if (lua_isnoneornil(L, 2)) {
		allyTeamID = pov.readAllyTeam;
	} else {
		const int aat = luaL_optint(L, 2, CEventClient::MinSpecialTeam - 1);

		if (aat == CEventClient::NoAccessTeam) {
			allyTeamID = aat;
		} else if (pov.fullRead && (aat >= 0 && aat < rows.numAllyTeams)) {
			allyTeamID = aat;
		} else if (pov.fullRead && aat == CEventClient::AllAccessTeam) {
			allyTeamID = aat;
		} else if (!pov.fullRead && aat == pov.readAllyTeam) {
			allyTeamID = aat;
		} else {
			return luaL_argerror(L, 2, "Invalid allyTeam");
		}
	}

	unsigned short losStatus;
	if (allyTeamID < 0) {
		losStatus = (allyTeamID == CEventClient::AllAccessTeam) ? (LOS_ALL_MASK_BITS | LOS_ALL_BITS) : 0;
	} else {
		losStatus = rows.losStatusAll[allyTeamID * rows.MaxUnits() + unitID];
	}

	constexpr int currMask = LOS_INLOS   | LOS_INRADAR;
	constexpr int prevMask = LOS_PREVLOS | LOS_CONTRADAR;

	const bool isTyped = ((losStatus & prevMask) == prevMask);

	if (luaL_optboolean(L, 3, false)) {
		// return a numeric value
		if (!pov.fullRead)
			losStatus &= ((prevMask * isTyped) | currMask);

		lua_pushnumber(L, losStatus);
		return 1;
	}

	lua_createtable(L, 0, 3);
	if (losStatus & LOS_INLOS) {
		HSTR_PUSH_BOOL(L, "los", true);
	}
	if (losStatus & LOS_INRADAR) {
		HSTR_PUSH_BOOL(L, "radar", true);
	}
	if ((losStatus & LOS_INLOS) || isTyped) {
		HSTR_PUSH_BOOL(L, "typed", true);
	}
	return 1;
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

// mirror of LuaUnsyncedRead::IsUnitVisible (camera and icon state are
// draw-side already; the losStatus gates and radius default were sim reads)
int LuaSnapshotServe::IsUnitVisible(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseUnit gate mirror
	if (!rows.Valid(unitID))
		return 0;
	if (pov.readAllyTeam < 0) {
		if (!pov.fullRead)
			return 0;
	} else if ((rows.LosStatus(unitID, pov.readAllyTeam) & (LOS_INLOS | LOS_INRADAR)) == 0) {
		return 0;
	}

	const float radius = luaL_optnumber(L, 2, rows.radius[unitID]);
	const bool checkIcon = lua_toboolean(L, 3);

	if (pov.readAllyTeam < 0) {
		// the gate above guarantees fullRead here
		lua_pushboolean(L,
			(!checkIcon || !CUnitDrawer::GetIsIcon(unitID)) &&
			camera->InView(rows.midPos[unitID], radius));
	} else {
		if ((rows.LosStatus(unitID, pov.readAllyTeam) & LOS_INLOS) == 0) {
			lua_pushboolean(L, false);
		} else {
			lua_pushboolean(L,
				(!checkIcon || !CUnitDrawer::GetIsIcon(unitID)) &&
				camera->InView(rows.midPos[unitID], radius));
		}
	}
	return 1;
}

// mirror of LuaUnsyncedRead::IsUnitIcon (drawer-owned state; only the
// ParseUnit gate was a sim read)
int LuaSnapshotServe::IsUnitIcon(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID))
		return 0;
	if (pov.readAllyTeam < 0) {
		if (!pov.fullRead)
			return 0;
	} else if ((rows.LosStatus(unitID, pov.readAllyTeam) & (LOS_INLOS | LOS_INRADAR)) == 0) {
		return 0;
	}

	lua_pushboolean(L, CUnitDrawer::GetIsIcon(unitID));
	return 1;
}


/******************************************************************************
 * Projectile family (E.1b second family). ParseProjectile arg semantics are
 * luaL_checkint; visibility is ProjectileRows::PovVisible (the extraction-time
 * positional-LOS answer + own-allyteam bypass, mirroring IsProjectileVisible).
 ******************************************************************************/

// mirror of LuaSyncedRead::GetProjectilePosition
int LuaSnapshotServe::GetProjectilePosition(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.pos[projID].x);
	lua_pushnumber(L, rows.pos[projID].y);
	lua_pushnumber(L, rows.pos[projID].z);
	return 3;
}

// mirror of LuaSyncedRead::GetProjectileVelocity (GetWorldObjectVelocity shape)
int LuaSnapshotServe::GetProjectileVelocity(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.speed[projID].x);
	lua_pushnumber(L, rows.speed[projID].y);
	lua_pushnumber(L, rows.speed[projID].z);
	lua_pushnumber(L, rows.speed[projID].w);
	return 4;
}

// mirror of LuaSyncedRead::GetProjectileDefID
int LuaSnapshotServe::GetProjectileDefID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.isWeapon[projID])
		return 0;
	if (rows.weaponDefID[projID] < 0) // no WeaponDef
		return 0;

	lua_pushnumber(L, rows.weaponDefID[projID]);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileTarget
int LuaSnapshotServe::GetProjectileTarget(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.isWeapon[projID])
		return 0;

	const uint8_t targetType = rows.targetType[projID];

	if (targetType == 'g') {
		lua_pushnumber(L, int('g'));
		lua_createtable(L, 3, 0);
		lua_pushnumber(L, rows.targetPos[projID].x); lua_rawseti(L, -2, 1);
		lua_pushnumber(L, rows.targetPos[projID].y); lua_rawseti(L, -2, 2);
		lua_pushnumber(L, rows.targetPos[projID].z); lua_rawseti(L, -2, 3);
		return 2;
	}
	if (targetType == 'u' || targetType == 'f' || targetType == 'p') {
		lua_pushnumber(L, int(targetType));
		lua_pushnumber(L, rows.targetID[projID]);
		return 2;
	}

	// live path asserts unreachable here; deterministic nil mirrors its return 0
	return 0;
}

// mirror of LuaSyncedRead::GetProjectileOwnerID
int LuaSnapshotServe::GetProjectileOwnerID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// unitHandler.MaxUnits() mirror: the unit rows are sized to exactly that
	const int unitID = rows.ownerID[projID];
	if ((unitID < 0) || (static_cast<size_t>(unitID) >= simSnapshot.Read().MaxUnits()))
		return 0;

	lua_pushnumber(L, unitID);
	return 1;
}
