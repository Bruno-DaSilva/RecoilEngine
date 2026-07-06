/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <cstdio>
#include <cstring>

#include "LuaHandle.h"
#include "LuaHashString.h" // HSTR_PUSH_BOOL
#include "LuaInclude.h"

#include "Game/Camera.h"
#include "Game/Game.h" // the stats callouts' live `game` null-check
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

	// LuaSyncedRead's ParseTeam argument semantics (error text included). The
	// live helper's teamHandler.Team() result is never null, so the callers'
	// null-checks are inert and the twins skip them.
	inline int ParseTeamIDSynced(lua_State* L, const char* caller, int index, const SimSnapshot::TeamRows& rows)
	{
		const int teamID = luaL_checkint(L, index);

		if (!rows.ValidTeam(teamID))
			luaL_error(L, "Bad teamID in %s\n", caller);

		return teamID;
	}

	// LuaSyncedRead's IsPlayerUnsynced mirror (hostDemo/isFromDemo from the
	// player boundary copy); trivially false for the unsynced handles the
	// redirect serves, mirrored anyway for line-by-line fidelity
	inline bool IsPlayerUnsyncedMirror(lua_State* L, const SimSnapshot::PlayerRows& rows, int playerID)
	{
		const bool syncedHandle = CLuaHandle::GetHandleSynced(L);
		const bool onlyFromDemo = syncedHandle && (rows.hostDemo != 0);

		return (onlyFromDemo && rows.isFromDemo[playerID] == 0);
	}

	// the customOpts push loop GetTeamInfo/GetPlayerInfo share; table CONTENT
	// matches the live push regardless of map iteration order
	inline void PushOptsTable(lua_State* L, const spring::unordered_map<std::string, std::string>& opts)
	{
		lua_createtable(L, 0, opts.size());

		for (const auto& pair: opts) {
			lua_pushsstring(L, pair.first);
			lua_pushsstring(L, pair.second);
			lua_rawset(L, -3);
		}
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

	// The snapshot publishes at the first CGame::Draw boundary, but draw
	// callins run before that: LuaIntro's DrawLoadScreen (its __func__ starts
	// with "Draw", so the PR-2 bracket arms) fires while the game is still
	// loading. There the live tables already exist -- teams and players from
	// the start script, units spawning on the load thread -- so an empty
	// snapshot would serve nil where master serves values (first hit by the
	// PR-26 team/player family; load screens list players/teams). Pre-publish,
	// serve live: identical to master by construction, single-threaded still.
	if (simSnapshot.Generation() == 0)
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


/******************************************************************************
 * Team/player-table family (PR 26). Served from the TeamRows/PlayerRows
 * boundary copy (re-extracted every boundary, section E.3); allied-POV gates
 * via TeamRows::PovAlliedTeam, the LuaUtils::IsAlliedTeam mirror. UnitDef-free
 * family: the only live globals the twins touch are the handle POV and the
 * draw-owned `game` pointer null-check the stats callouts perform.
 ******************************************************************************/

// mirror of LuaSyncedRead::GetGaiaTeamID
int LuaSnapshotServe::GetGaiaTeamID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	if (rows.useLuaGaia == 0)
		return 0;

	lua_pushnumber(L, rows.gaiaTeamID);
	return 1;
}

// mirror of LuaSyncedRead::GetAllyTeamList
int LuaSnapshotServe::GetAllyTeamList(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	lua_createtable(L, rows.activeAllyTeams, 0);

	unsigned int allyCount = 1;

	for (int at = 0; at < rows.activeAllyTeams; at++) {
		lua_pushnumber(L, at);
		lua_rawseti(L, -2, allyCount++);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamList (team slots are never null, so the
// live null-skip is inert)
int LuaSnapshotServe::GetTeamList(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	int allyTeamID = -1;

	const int args = lua_gettop(L); // number of arguments

	// peek the first argument, but gracefully ignore the rest
	if (args >= 1) {
		allyTeamID = luaL_checkinteger(L, 1);
		// teamHandler.IsValidAllyTeam mirror
		if (!(allyTeamID >= 0 && allyTeamID < rows.activeAllyTeams))
			return 0;
	}

	lua_createtable(L, rows.activeTeams, 0);

	unsigned int teamCount = 1;

	for (int t = 0; t < rows.activeTeams; t++) {
		if ((allyTeamID >= 0) && (allyTeamID != rows.allyTeam[t]))
			continue;

		lua_pushnumber(L, t);
		lua_rawseti(L, -2, teamCount++);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetPlayerList (player slots are never null)
int LuaSnapshotServe::GetPlayerList(lua_State* L, const char* caller)
{
	const auto& trows = simSnapshot.ReadTeams();
	const auto& rows = simSnapshot.ReadPlayers();

	int teamID = -1;
	bool active = false;

	if (lua_isnumber(L, 1)) {
		teamID = lua_toint(L, 1);
		active = lua_isboolean(L, 2)? lua_toboolean(L, 2): active;
	}
	else if (lua_isboolean(L, 1)) {
		active = lua_toboolean(L, 1);
		teamID = lua_isnumber(L, 2)? lua_toint(L, 2): teamID;
	}

	if (teamID >= trows.activeTeams)
		return 0;

	lua_createtable(L, rows.activePlayers, 0);

	for (int p = 0, playerCount = 1; p < rows.activePlayers; p++) {
		if (IsPlayerUnsyncedMirror(L, rows, p))
			continue;

		if (active && rows.active[p] == 0)
			continue;

		if (teamID >= 0) {
			// exclude specs for normal team ID's
			if (rows.spectator[p] != 0)
				continue;
			if (rows.team[p] != teamID)
				continue;
		}

		lua_pushnumber(L, p);
		lua_rawseti(L, -2, playerCount++);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamInfo
int LuaSnapshotServe::GetTeamInfo(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	const int teamID = luaL_checkint(L, 1);
	if (!rows.ValidTeam(teamID))
		return 0;

	// read before modifying stack
	const bool getTeamOpts = luaL_optboolean(L, 2, true);

	lua_pushnumber(L,  teamID); // live pushes team->teamNum == teamID
	lua_pushnumber(L,  rows.leader[teamID]);
	lua_pushboolean(L, rows.isDead[teamID]);
	lua_pushboolean(L, rows.hasAIs[teamID]);
	lua_pushsstring(L, rows.sideName[teamID]);
	lua_pushnumber(L,  rows.allyTeam[teamID]);
	lua_pushnumber(L, rows.incomeMultiplier[teamID]);

	if (getTeamOpts)
		PushOptsTable(L, rows.customOpts[teamID]);

	return 7 + getTeamOpts;
}

// mirror of LuaSyncedRead::GetTeamAllyTeamID
int LuaSnapshotServe::GetTeamAllyTeamID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	const int teamID = luaL_checkint(L, 1);
	if (!rows.ValidTeam(teamID))
		return 0;

	lua_pushnumber(L, rows.allyTeam[teamID]);
	return 1;
}

// mirror of LuaSyncedRead::GetTeamResources
int LuaSnapshotServe::GetTeamResources(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);
	const Pov pov = HandlePov(L);

	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead))
		return 0;

	switch (luaL_checkstring(L, 2)[0]) {
		case 'm': {
			lua_pushnumber(L, rows.res[teamID].metal);
			lua_pushnumber(L, rows.resStorage[teamID].metal);
			lua_pushnumber(L, rows.resPrevPull[teamID].metal);
			lua_pushnumber(L, rows.resPrevIncome[teamID].metal);
			lua_pushnumber(L, rows.resPrevExpense[teamID].metal);
			lua_pushnumber(L, rows.resShare[teamID].metal);
			lua_pushnumber(L, rows.resPrevSent[teamID].metal);
			lua_pushnumber(L, rows.resPrevReceived[teamID].metal);
			lua_pushnumber(L, rows.resPrevExcess[teamID].metal);
			return 9;
		} break;
		case 'e': {
			lua_pushnumber(L, rows.res[teamID].energy);
			lua_pushnumber(L, rows.resStorage[teamID].energy);
			lua_pushnumber(L, rows.resPrevPull[teamID].energy);
			lua_pushnumber(L, rows.resPrevIncome[teamID].energy);
			lua_pushnumber(L, rows.resPrevExpense[teamID].energy);
			lua_pushnumber(L, rows.resShare[teamID].energy);
			lua_pushnumber(L, rows.resPrevSent[teamID].energy);
			lua_pushnumber(L, rows.resPrevReceived[teamID].energy);
			lua_pushnumber(L, rows.resPrevExcess[teamID].energy);
			return 9;
		} break;
		default: {
		} break;
	}

	return 0;
}

// mirror of LuaSyncedRead::GetTeamUnitStats
int LuaSnapshotServe::GetTeamUnitStats(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);

	if (game == nullptr)
		return 0;

	const Pov pov = HandlePov(L);

	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead) && rows.gameOver == 0)
		return 0;

	const TeamStatistics& stats = rows.currentStats[teamID];
	lua_pushnumber(L, stats.unitsKilled);
	lua_pushnumber(L, stats.unitsDied);
	lua_pushnumber(L, stats.unitsCaptured);
	lua_pushnumber(L, stats.unitsOutCaptured);
	lua_pushnumber(L, stats.unitsReceived);
	lua_pushnumber(L, stats.unitsSent);

	return 6;
}

// mirror of LuaSyncedRead::GetTeamResourceStats
int LuaSnapshotServe::GetTeamResourceStats(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);

	if (game == nullptr)
		return 0;

	const Pov pov = HandlePov(L);

	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead) && rows.gameOver == 0)
		return 0;

	const TeamStatistics& stats = rows.currentStats[teamID];

	switch (luaL_checkstring(L, 2)[0]) {
		case 'm': {
			lua_pushnumber(L, stats.metalUsed);
			lua_pushnumber(L, stats.metalProduced);
			lua_pushnumber(L, stats.metalExcess);
			lua_pushnumber(L, stats.metalReceived);
			lua_pushnumber(L, stats.metalSent);
			return 5;
		} break;
		case 'e': {
			lua_pushnumber(L, stats.energyUsed);
			lua_pushnumber(L, stats.energyProduced);
			lua_pushnumber(L, stats.energyExcess);
			lua_pushnumber(L, stats.energyReceived);
			lua_pushnumber(L, stats.energySent);
			return 5;
		} break;
		default: {
		} break;
	}

	return 0;
}

// mirror of LuaSyncedRead::GetTeamDamageStats
int LuaSnapshotServe::GetTeamDamageStats(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);

	if (game == nullptr)
		return 0;

	const Pov pov = HandlePov(L);

	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead) && rows.gameOver == 0)
		return 0;

	const TeamStatistics& stats = rows.currentStats[teamID];

	lua_pushnumber(L, stats.damageDealt);
	lua_pushnumber(L, stats.damageReceived);

	return 2;
}

// mirror of LuaSyncedRead::GetTeamUnitCount
int LuaSnapshotServe::GetTeamUnitCount(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	// parse the team
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);
	const Pov pov = HandlePov(L);

	// use the raw team count for allies
	if (rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead)) {
		lua_pushnumber(L, rows.numUnits[teamID]);
		return 1;
	}

	// loop through the units for enemies: the live path walks
	// unitHandler.GetUnitsByTeam counting LuaUtils::IsUnitVisible; the unit
	// snapshot covers the identical unit set at this boundary and
	// PovUnitVisible is the IsUnitVisible mirror (order-independent count)
	const auto& urows = simSnapshot.Read();

	unsigned int unitCount = 0;

	for (size_t id = 0; id < urows.MaxUnits(); ++id) {
		if (urows.valid[id] == 0 || urows.team[id] != static_cast<uint8_t>(teamID))
			continue;

		unitCount += int(urows.PovUnitVisible(static_cast<int>(id), pov.readAllyTeam, pov.fullRead));
	}

	lua_pushnumber(L, unitCount);
	return 1;
}

// mirror of LuaSyncedRead::GetPlayerInfo
int LuaSnapshotServe::GetPlayerInfo(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadPlayers();
	const auto& trows = simSnapshot.ReadTeams();

	const int playerID = luaL_checkint(L, 1);
	if (!rows.ValidPlayer(playerID))
		return 0;

	if (IsPlayerUnsyncedMirror(L, rows, playerID))
		return 0;

	// read before modifying stack
	const bool getPlayerOpts = luaL_optboolean(L, 2, true);

	// player->team is always a valid teamID in reachable states; the guards
	// mirror what an out-of-range live read would make unreachable anyway
	const int team = rows.team[playerID];

	lua_pushsstring(L, rows.name[playerID]);
	lua_pushboolean(L, rows.active[playerID]);
	lua_pushboolean(L, rows.spectator[playerID]);
	lua_pushnumber(L, team);
	lua_pushnumber(L, trows.ValidTeam(team) ? trows.allyTeam[team] : -1);
	lua_pushnumber(L, rows.ping[playerID] * 0.001f); // in seconds
	lua_pushnumber(L, rows.cpuUsage[playerID]);
	lua_pushsstring(L, rows.countryCode[playerID]);
	lua_pushnumber(L, rows.rank[playerID]);
	// same as select(4, GetTeamInfo(teamID=player->team))
	lua_pushboolean(L, trows.ValidTeam(team) && trows.hasAIs[team] != 0);

	if (getPlayerOpts) {
		PushOptsTable(L, rows.customOpts[playerID]);
	} else {
		lua_pushnil(L);
	}
	lua_pushboolean(L, rows.desynced[playerID]);

	return 12;
}

// mirror of LuaUnsyncedRead::GetTeamColor
int LuaSnapshotServe::GetTeamColor(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	const int teamID = luaL_checkint(L, 1);
	if ((teamID < 0) || (teamID >= rows.activeTeams))
		return 0;

	lua_pushnumber(L, rows.color[teamID][0] / 255.0f);
	lua_pushnumber(L, rows.color[teamID][1] / 255.0f);
	lua_pushnumber(L, rows.color[teamID][2] / 255.0f);
	lua_pushnumber(L, rows.color[teamID][3] / 255.0f);
	return 4;
}

// mirror of LuaUnsyncedRead::GetTeamOrigColor
int LuaSnapshotServe::GetTeamOrigColor(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	const int teamID = luaL_checkint(L, 1);
	if ((teamID < 0) || (teamID >= rows.activeTeams))
		return 0;

	lua_pushnumber(L, rows.origColor[teamID][0] / 255.0f);
	lua_pushnumber(L, rows.origColor[teamID][1] / 255.0f);
	lua_pushnumber(L, rows.origColor[teamID][2] / 255.0f);
	lua_pushnumber(L, rows.origColor[teamID][3] / 255.0f);
	return 4;
}
