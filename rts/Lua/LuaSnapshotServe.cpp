/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <cstdio>
#include <cstring>

#include "LuaHandle.h"
#include "LuaHashString.h" // HSTR_PUSH_BOOL
#include "LuaInclude.h"
#include "LuaSplitContract.h"

#include "Game/Camera.h"
#include "Game/Game.h" // the stats callouts' live `game` null-check
#include "Game/GlobalUnsynced.h" // gu->myAllyTeam (IsUnitAllied's fullRead answer)
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/GlobalRendering.h" // timeOffset (draw-owned)
#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Misc/CollisionVolume.h" // WORLD_TO_OBJECT_SPACE
#include "Sim/Misc/GlobalConstants.h" // GAME_SPEED
#include "Sim/Misc/GlobalSynced.h" // GODMODE_*_BIT
#include "Sim/MoveTypes/MoveDefHandler.h" // immutable MoveDef name (GetUnitMoveDefID)
#include "Sim/Units/Unit.h" // LOS_* bits
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "System/EventClient.h" // CEventClient special-team constants
#include "System/SpringMath.h" // ClampRadPi (GetUnitHeading)
#include "System/StringHash.h" // hashString (GetUnitSensorRadius)
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

	// LuaSyncedRead's ParseFeature argument semantics (error text included)
	inline int ParseFeatureIDSynced(lua_State* L, const char* caller, int index)
	{
		if (!lua_isnumber(L, index))
			luaL_error(L, "[%s] featureID (arg #%d) not a number\n", caller, index);

		return lua_toint(L, index);
	}

	// LuaUtils::IsFeatureVisible mirror (exact branch order: fullRead bypass,
	// then the no-access gate, then CFeature::IsInLosForAllyTeam); caller must
	// have checked rows.Valid(featureID)
	inline bool PovFeatureVisible(const SimSnapshot::FeatureRows& rows, int featureID, const Pov& pov)
	{
		if (pov.fullRead)
			return true;
		if (pov.readAllyTeam < 0)
			return false;

		return rows.IsInLosForAllyTeam(featureID, pov.readAllyTeam);
	}

	// GetSolidObjectBlocking's seven pushes from the packed row byte
	// (bit i = push slot i, see the UnitRows::blockingBits layout comment)
	inline int PushBlockingBits(lua_State* L, uint8_t bits)
	{
		lua_pushboolean(L, (bits >> 0) & 1);
		lua_pushboolean(L, (bits >> 1) & 1);
		lua_pushboolean(L, (bits >> 2) & 1);
		lua_pushboolean(L, (bits >> 3) & 1);

		lua_pushboolean(L, (bits >> 4) & 1);
		lua_pushboolean(L, (bits >> 5) & 1);
		lua_pushboolean(L, (bits >> 6) & 1);

		return 7;
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
	if (CLuaHandle::GetHandleSynced(L))
		return false;

	if (ScopedDrawCallinContext::InDrawCallin())
		return true;

	// under the split contract (PR 27a) the serving window widens from Draw*
	// callins to the whole draw phase: at 27b ALL unsynced execution inside
	// CGame::Draw (widget Update, GUI handlers, ...) runs on the draw thread,
	// so it must read the snapshot there too. Pre-split the snapshot equals
	// live at every point inside the draw window (no sim frame can intervene),
	// so this widening is value-identical -- it rehearses the plumbing only.
	return LuaSplitContract::Enforced(L);
}


int LuaSnapshotServe::Route(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn)
{
	if (!ShouldServe(L))
		return liveFn(L, caller);

	// The snapshot publishes its first unit extraction at frame 0, but draw
	// callins run before that: LuaIntro's DrawLoadScreen (its __func__ starts
	// with "Draw", so the PR-2 bracket arms) fires while the game is still
	// loading, and CGame::Draw runs through the whole pregame wait with the
	// unit due-check never firing (generation stays 0). There the live tables
	// already exist -- teams and players from the start script, units spawning
	// on the load thread -- so an empty snapshot would serve nil where master
	// serves values (first hit by the PR-26 team/player family; load screens
	// list players/teams). Pre-publish, serve live: identical to master by
	// construction, single-threaded still. The live-exception bracket keeps
	// the split contract's parse gates out of this sanctioned fallback (found
	// by the PR-27a count-mode gate: pregame draws tripped -- and strict mode
	// would have denied -- the SERVED team callouts' live legs).
	if (simSnapshot.Generation() == 0) {
		LuaSplitContract::ScopedLiveException prePublishFallback;
		return liveFn(L, caller);
	}

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
	// the live leg deliberately reads live sim state from draw context (the
	// whole point of the dual-run); suspend the split contract's gates so
	// the parse helpers don't nil it out in strict mode
	int liveN = 0;
	{
		LuaSplitContract::ScopedLiveException liveLeg;
		liveN = liveFn(L, caller);
	}

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
 * PR 27a row-backed tail: the remaining per-object callouts whose live reads
 * are fully covered by the PR-27a SimSnapshot rows. Unit twins mirror the
 * live body's parse helper exactly (ParseUnit/ParseAllyUnit/ParseInLosUnit/
 * ParseTypedUnit -> the matching Pov predicate); feature twins mirror
 * ParseFeature (ParseFeatureIDSynced + PovFeatureVisible); projectile twins
 * keep the luaL_checkint + PovVisible shape of the family above.
 ******************************************************************************/

// mirror of LuaSyncedRead::ValidUnitID (no luaL error: the live body
// short-circuits lua_isnumber before ParseUnit)
int LuaSnapshotServe::ValidUnitID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const Pov pov = HandlePov(L);

	bool valid = false;

	if (lua_isnumber(L, 1)) {
		const int unitID = lua_toint(L, 1);
		// ParseUnit mirror
		valid = rows.Valid(unitID) && rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead);
	}

	lua_pushboolean(L, valid);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitDefID (incl. the LuaUtils::EffectiveUnitDef
// decoy substitution; UnitDef derefs are immutable game data)
int LuaSnapshotServe::GetUnitDefID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead)) {
		lua_pushnumber(L, rows.defID[unitID]);
		return 1;
	}

	if (!rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// EffectiveUnitDef mirror: not allied here, so the decoy def if one exists
	const UnitDef* ud = unitDefHandler->GetUnitDefByID(rows.defID[unitID]);

	lua_pushnumber(L, (ud->decoyDef != nullptr) ? ud->decoyDef->id : ud->id);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitTeam
int LuaSnapshotServe::GetUnitTeam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.team[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitAllyTeam
int LuaSnapshotServe::GetUnitAllyTeam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.allyTeam[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitNeutral
int LuaSnapshotServe::GetUnitNeutral(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.neutral[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitIsDead
int LuaSnapshotServe::GetUnitIsDead(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.isDead[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitIsBeingBuilt
int LuaSnapshotServe::GetUnitIsBeingBuilt(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.beingBuilt[unitID]);
	lua_pushnumber(L, rows.buildProgress[unitID]);
	return 2;
}

// mirror of GetWorldObjectVelocity(L, ParseInLosUnit(...))
int LuaSnapshotServe::GetUnitVelocity(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.speed[unitID].x);
	lua_pushnumber(L, rows.speed[unitID].y);
	lua_pushnumber(L, rows.speed[unitID].z);
	lua_pushnumber(L, rows.speed[unitID].w);
	return 4;
}

// mirror of LuaSyncedRead::GetUnitDirection
int LuaSnapshotServe::GetUnitDirection(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.frontdir[unitID].x);
	lua_pushnumber(L, rows.frontdir[unitID].y);
	lua_pushnumber(L, rows.frontdir[unitID].z);

	lua_pushnumber(L, rows.rightdir[unitID].x);
	lua_pushnumber(L, rows.rightdir[unitID].y);
	lua_pushnumber(L, rows.rightdir[unitID].z);

	lua_pushnumber(L, rows.updir[unitID].x);
	lua_pushnumber(L, rows.updir[unitID].y);
	lua_pushnumber(L, rows.updir[unitID].z);

	return 9;
}

// mirror of LuaSyncedRead::GetUnitHeading (heading rows are int16, same
// int16 -> float conversion as the live SyncedSshort read)
int LuaSnapshotServe::GetUnitHeading(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	float heading = rows.heading[unitID];
	if (luaL_optboolean(L, 2, false)) {
		heading = ClampRadPi(math::PI / 32768.0f * heading);
	}

	lua_pushnumber(L, heading);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitVectors
int LuaSnapshotServe::GetUnitVectors(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

#define PACK_VECTOR(n) \
	lua_createtable(L, 3, 0);            \
	lua_pushnumber(L, rows. n [unitID].x); lua_rawseti(L, -2, 1); \
	lua_pushnumber(L, rows. n [unitID].y); lua_rawseti(L, -2, 2); \
	lua_pushnumber(L, rows. n [unitID].z); lua_rawseti(L, -2, 3)

	PACK_VECTOR(frontdir);
	PACK_VECTOR(updir);
	PACK_VECTOR(rightdir);

#undef PACK_VECTOR

	return 3;
}

// mirror of LuaSyncedRead::GetUnitRadius
int LuaSnapshotServe::GetUnitRadius(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.radius[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitHeight
int LuaSnapshotServe::GetUnitHeight(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.height[unitID]);
	return 1;
}

// mirror of GetSolidObjectMass(L, ParseInLosUnit(...))
int LuaSnapshotServe::GetUnitMass(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.mass[unitID]);

	return 1;
}

// mirror of LuaSyncedRead::GetUnitExperience
int LuaSnapshotServe::GetUnitExperience(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.experience[unitID]);
	lua_pushnumber(L, rows.limExperience[unitID]);
	return 2;
}

// mirror of LuaSyncedRead::GetUnitIsActive
int LuaSnapshotServe::GetUnitIsActive(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.activated[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitIsCloaked
int LuaSnapshotServe::GetUnitIsCloaked(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.isCloaked[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitMaxRange
int LuaSnapshotServe::GetUnitMaxRange(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.maxRange[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitBuildFacing
int LuaSnapshotServe::GetUnitBuildFacing(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.buildFacing[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitSensorRadius (incl. its unknown-type error)
int LuaSnapshotServe::GetUnitSensorRadius(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	switch (hashString(luaL_checkstring(L, 2))) {
		case hashString("los"): {
			lua_pushnumber(L, rows.losRadius[unitID]);
		} break;
		case hashString("airLos"): {
			lua_pushnumber(L, rows.airLosRadius[unitID]);
		} break;
		case hashString("radar"): {
			lua_pushnumber(L, rows.radarRadius[unitID]);
		} break;
		case hashString("sonar"): {
			lua_pushnumber(L, rows.sonarRadius[unitID]);
		} break;
		case hashString("seismic"): {
			lua_pushnumber(L, rows.seismicRadius[unitID]);
		} break;
		case hashString("radarJammer"): {
			lua_pushnumber(L, rows.jammerRadius[unitID]);
		} break;
		case hashString("sonarJammer"): {
			lua_pushnumber(L, rows.sonarJamRadius[unitID]);
		} break;
		default: {
			luaL_error(L, "[%s] unknown sensor type \"%s\"", caller, luaL_checkstring(L, 2));
		} break;
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitSeismicSignature
int LuaSnapshotServe::GetUnitSeismicSignature(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.seismicSignature[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitSelfDTime
int LuaSnapshotServe::GetUnitSelfDTime(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.selfDCountdown[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitArmored
int LuaSnapshotServe::GetUnitArmored(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.armoredState[unitID]);
	lua_pushnumber(L, rows.armoredMultiple[unitID]);
	return 2;
}

// mirror of LuaSyncedRead::GetUnitResources
int LuaSnapshotServe::GetUnitResources(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.resourcesMake[unitID].metal);
	lua_pushnumber(L, rows.resourcesUse[unitID].metal);
	lua_pushnumber(L, rows.resourcesMake[unitID].energy);
	lua_pushnumber(L, rows.resourcesUse[unitID].energy);
	return 4;
}

// mirror of LuaSyncedRead::GetUnitHarvestStorage
int LuaSnapshotServe::GetUnitHarvestStorage(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	for (int i = 0; i < SResourcePack::MAX_RESOURCES; ++i) {
		lua_pushnumber(L, rows.harvested[unitID][i]);
		lua_pushnumber(L, rows.harvestStorage[unitID][i]);
	}
	return 2 * SResourcePack::MAX_RESOURCES;
}

// mirror of LuaSyncedRead::GetUnitCosts
int LuaSnapshotServe::GetUnitCosts(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.buildTime[unitID]);
	lua_pushnumber(L, rows.cost[unitID].metal);
	lua_pushnumber(L, rows.cost[unitID].energy);
	return 3;
}

// mirror of LuaSyncedRead::GetUnitCostTable
int LuaSnapshotServe::GetUnitCostTable(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;
	lua_createtable(L, 0, 2);
	lua_pushstring(L, "metal");
	lua_pushnumber(L, rows.cost[unitID].metal);
	lua_rawset(L, -3);
	lua_pushstring(L, "energy");
	lua_pushnumber(L, rows.cost[unitID].energy);
	lua_rawset(L, -3);
	lua_pushnumber(L, rows.buildTime[unitID]);
	return 2;
}

// mirror of LuaSyncedRead::GetUnitMoveDefID (the -1 row encoding is the live
// "no moveDef" branch; the name is immutable MoveDef data, deref'd directly)
int LuaSnapshotServe::GetUnitMoveDefID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int moveDefID = rows.moveDefID[unitID];
	if (moveDefID < 0) {
		lua_pushboolean(L, false);
		return 1;
	}

	lua_pushnumber(L, moveDefID);
	lua_pushsstring(L, moveDefHandler.GetMoveDefByPathType(moveDefID)->name);
	return 2;
}

// mirror of GetSolidObjectBlocking(L, ParseTypedUnit(...))
int LuaSnapshotServe::GetUnitBlocking(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	return PushBlockingBits(L, rows.blockingBits[unitID]);
}

// mirror of LuaSyncedRead::GetUnitLeavesGhost
int LuaSnapshotServe::GetUnitLeavesGhost(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.leavesGhost[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitSeparation (both parses run before the
// null-checks, like the live body; GetLuaErrorPos == midPos + LuaErrorVector)
int LuaSnapshotServe::GetUnitSeparation(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID1 = ParseUnitIDSynced(L, caller, 1);
	const int unitID2 = ParseUnitIDSynced(L, caller, 2);
	const Pov pov = HandlePov(L);

	// ParseUnit mirrors
	if (!rows.Valid(unitID1) || !rows.PovUnitVisible(unitID1, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.Valid(unitID2) || !rows.PovUnitVisible(unitID2, pov.readAllyTeam, pov.fullRead))
		return 0;

	float3 pos1 = rows.midPos[unitID1];
	float3 pos2 = rows.midPos[unitID2];

	if (!rows.PovAlliedUnit(unitID1, pov.readAllyTeam, pov.fullRead))
		pos1 = rows.midPos[unitID1] + rows.LuaErrorVector(unitID1, pov.readAllyTeam, pov.fullRead);
	if (!rows.PovAlliedUnit(unitID2, pov.readAllyTeam, pov.fullRead))
		pos2 = rows.midPos[unitID2] + rows.LuaErrorVector(unitID2, pov.readAllyTeam, pov.fullRead);

	const float dist = (luaL_optboolean(L, 3, false))? pos1.distance2D(pos2): pos1.distance(pos2);

	if (luaL_optboolean(L, 4, false)) {
		lua_pushnumber(L, std::max(0.0f, dist - rows.radius[unitID1] - rows.radius[unitID2]));
	} else {
		lua_pushnumber(L, dist);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitFeatureSeparation (feature arg is only
// parsed once the unit gate passed, like the live body)
int LuaSnapshotServe::GetUnitFeatureSeparation(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const auto& frows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 2);

	// ParseFeature mirror (the live body's extra IsFeatureVisible re-check is
	// the same predicate twice)
	if (!frows.Valid(featureID) || !PovFeatureVisible(frows, featureID, pov))
		return 0;

	float3 pos1 = rows.midPos[unitID];
	float3 pos2 = frows.midPos[featureID];

	if (!rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		pos1 = rows.midPos[unitID] + rows.LuaErrorVector(unitID, pov.readAllyTeam, pov.fullRead);

	lua_pushnumber(L, (luaL_optboolean(L, 3, false))? pos1.distance2D(pos2): pos1.distance(pos2));
	return 1;
}

// mirror of LuaSyncedRead::IsUnitInRadar (incl. its GetEffectiveLosAllyTeam
// helper, which can raise "Invalid allyTeam" exactly like the live path); the
// inRadarAll row IS the extracted losHandler->InRadar(unit, allyTeam) answer
int LuaSnapshotServe::IsUnitInRadar(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
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

	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, rows.InRadar(unitID, allyTeamID));
	return 1;
}

// mirror of LuaUnsyncedRead::IsUnitAllied (the readAllyTeam < 0 branch
// answers against gu->myAllyTeam, which is draw-owned state)
int LuaSnapshotServe::IsUnitAllied(lua_State* L, const char* caller)
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

	if (pov.readAllyTeam < 0) {
		// in this case handle has full-read access since the unit passed the gate
		lua_pushboolean(L, rows.allyTeam[unitID] == gu->myAllyTeam);
	} else {
		lua_pushboolean(L, rows.allyTeam[unitID] == pov.readAllyTeam);
	}

	return 1;
}

// mirror of LuaSyncedRead::ValidFeatureID (no luaL error: the live body
// short-circuits lua_isnumber before ParseFeature)
int LuaSnapshotServe::ValidFeatureID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const Pov pov = HandlePov(L);

	bool valid = false;

	if (lua_isnumber(L, 1)) {
		const int featureID = lua_toint(L, 1);
		// ParseFeature mirror
		valid = rows.Valid(featureID) && PovFeatureVisible(rows, featureID, pov);
	}

	lua_pushboolean(L, valid);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureDefID
int LuaSnapshotServe::GetFeatureDefID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror (the live extra IsFeatureVisible re-check is the
	// same predicate twice)
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.defID[featureID]);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureTeam
int LuaSnapshotServe::GetFeatureTeam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	if (rows.allyTeam[featureID] < 0) {
		lua_pushnumber(L, -1);
	} else {
		lua_pushnumber(L, rows.team[featureID]);
	}
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureAllyTeam
int LuaSnapshotServe::GetFeatureAllyTeam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.allyTeam[featureID]);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureHealth (def->health is immutable
// FeatureDef data, deref'd directly)
int LuaSnapshotServe::GetFeatureHealth(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.health[featureID]);
	lua_pushnumber(L, featureDefHandler->GetFeatureDefByID(rows.defID[featureID])->health);
	lua_pushnumber(L, rows.resurrectProgress[featureID]);
	return 3;
}

// mirror of LuaSyncedRead::GetFeatureHeight
int LuaSnapshotServe::GetFeatureHeight(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.height[featureID]);
	return 1;
}

// mirror of LuaSyncedRead::GetFeaturePosition (GetSolidObjectPosition with
// isFeature=true: no error vector; the optional mid/aim returns read the
// midPos/aimPos rows)
int LuaSnapshotServe::GetFeaturePosition(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	// NOTE: read before any pushing (same caveat as the live helper)
	const bool returnMidPos = luaL_optboolean(L, 2, false);
	const bool returnAimPos = luaL_optboolean(L, 3, false);

	lua_pushnumber(L, rows.pos[featureID].x);
	lua_pushnumber(L, rows.pos[featureID].y);
	lua_pushnumber(L, rows.pos[featureID].z);

	if (returnMidPos) {
		lua_pushnumber(L, rows.midPos[featureID].x);
		lua_pushnumber(L, rows.midPos[featureID].y);
		lua_pushnumber(L, rows.midPos[featureID].z);
	}
	if (returnAimPos) {
		lua_pushnumber(L, rows.aimPos[featureID].x);
		lua_pushnumber(L, rows.aimPos[featureID].y);
		lua_pushnumber(L, rows.aimPos[featureID].z);
	}

	return (3 + (3 * returnMidPos) + (3 * returnAimPos));
}

// mirror of LuaSyncedRead::GetFeatureRadius
int LuaSnapshotServe::GetFeatureRadius(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.radius[featureID]);
	return 1;
}

// mirror of GetSolidObjectMass(L, ParseFeature(...))
int LuaSnapshotServe::GetFeatureMass(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.mass[featureID]);

	return 1;
}

// mirror of LuaSyncedRead::GetFeatureDirection (front = the transMatrix Z
// column, right = X, up = Y, exactly as the live body reads them)
int LuaSnapshotServe::GetFeatureDirection(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.matZdir[featureID].x);
	lua_pushnumber(L, rows.matZdir[featureID].y);
	lua_pushnumber(L, rows.matZdir[featureID].z);

	lua_pushnumber(L, rows.matXdir[featureID].x);
	lua_pushnumber(L, rows.matXdir[featureID].y);
	lua_pushnumber(L, rows.matXdir[featureID].z);

	lua_pushnumber(L, rows.matYdir[featureID].x);
	lua_pushnumber(L, rows.matYdir[featureID].y);
	lua_pushnumber(L, rows.matYdir[featureID].z);

	return 9;
}

// mirror of GetWorldObjectVelocity(L, ParseFeature(...))
int LuaSnapshotServe::GetFeatureVelocity(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.speed[featureID].x);
	lua_pushnumber(L, rows.speed[featureID].y);
	lua_pushnumber(L, rows.speed[featureID].z);
	lua_pushnumber(L, rows.speed[featureID].w);
	return 4;
}

// mirror of LuaSyncedRead::GetFeatureHeading
int LuaSnapshotServe::GetFeatureHeading(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.heading[featureID]);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureResources
int LuaSnapshotServe::GetFeatureResources(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L,  rows.resources[featureID].metal);
	lua_pushnumber(L,  rows.defResources[featureID].metal);
	lua_pushnumber(L,  rows.resources[featureID].energy);
	lua_pushnumber(L,  rows.defResources[featureID].energy);
	lua_pushnumber(L,  rows.reclaimLeft[featureID]);
	lua_pushnumber(L,  rows.reclaimTime[featureID]);
	return 6;
}

// mirror of GetSolidObjectBlocking(L, ParseFeature(...))
int LuaSnapshotServe::GetFeatureBlocking(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	return PushBlockingBits(L, rows.blockingBits[featureID]);
}

// mirror of LuaSyncedRead::GetFeatureNoSelect
int LuaSnapshotServe::GetFeatureNoSelect(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushboolean(L, rows.noSelect[featureID]);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureResurrect (the -1 row encoding is the
// live "no udef" branch; the name is immutable UnitDef data, deref'd directly)
int LuaSnapshotServe::GetFeatureResurrect(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const int resurrectDefID = rows.resurrectDefID[featureID];

	if (resurrectDefID < 0) {
		lua_pushliteral(L, "");
	} else {
		lua_pushsstring(L, unitDefHandler->GetUnitDefByID(resurrectDefID)->name);
	}

	lua_pushnumber(L, rows.buildFacing[featureID]);
	return 2;
}

// mirror of LuaSyncedRead::GetFeatureSeparation (second parse only runs once
// the first gate passed, like the live body)
int LuaSnapshotServe::GetFeatureSeparation(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID1 = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID1) || !PovFeatureVisible(rows, featureID1, pov))
		return 0;

	const int featureID2 = ParseFeatureIDSynced(L, caller, 2);

	if (!rows.Valid(featureID2) || !PovFeatureVisible(rows, featureID2, pov))
		return 0;

	float3 pos1 = rows.pos[featureID1];
	float3 pos2 = rows.pos[featureID2];

	float dist;
	if (lua_isboolean(L, 3) && lua_toboolean(L, 3)) {
		dist = pos1.distance2D(pos2);
	} else {
		dist = pos1.distance(pos2);
	}

	lua_pushnumber(L, dist);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileDirection
int LuaSnapshotServe::GetProjectileDirection(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.dir[projID].x);
	lua_pushnumber(L, rows.dir[projID].y);
	lua_pushnumber(L, rows.dir[projID].z);
	return 3;
}

// mirror of LuaSyncedRead::GetProjectileGravity
int LuaSnapshotServe::GetProjectileGravity(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.mygravity[projID]);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileTeamID (teamHandler.IsValidTeam via
// the TeamRows mirror)
int LuaSnapshotServe::GetProjectileTeamID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (!simSnapshot.ReadTeams().ValidTeam(rows.teamID[projID]))
		return 0;

	lua_pushnumber(L, rows.teamID[projID]);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileAllyTeamID
int LuaSnapshotServe::GetProjectileAllyTeamID(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int allyTeamID = rows.allyTeam[projID];
	// teamHandler.IsValidAllyTeam mirror
	if (!(allyTeamID >= 0 && allyTeamID < rows.numAllyTeams))
		return 0;

	lua_pushnumber(L, allyTeamID);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileType
int LuaSnapshotServe::GetProjectileType(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushboolean(L, rows.isWeapon[projID]);
	lua_pushboolean(L, rows.isPiece[projID]);
	return 2;
}

// mirror of LuaSyncedRead::GetProjectileTimeToLive
int LuaSnapshotServe::GetProjectileTimeToLive(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.isWeapon[projID])
		return 0;

	lua_pushnumber(L, rows.ttl[projID]);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileIsIntercepted
int LuaSnapshotServe::GetProjectileIsIntercepted(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.isWeapon[projID])
		return 0;

	lua_pushboolean(L, rows.intercepted[projID]);
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


/******************************************************************************/
//
//  global-scalar family (PR 27a; SimSnapshot::GlobalRows)
//

// mirror of LuaSyncedRead::GetGameFrame
int LuaSnapshotServe::GetGameFrame(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadGlobals();

	const int simFrames = rows.luaSimFrame;
	const int dayFrames = GAME_SPEED * (24 * 60 * 60);

	lua_pushnumber(L, simFrames % dayFrames);
	lua_pushnumber(L, simFrames / dayFrames);
	return 2;
}

// mirror of LuaSyncedRead::GetGameSeconds
int LuaSnapshotServe::GetGameSeconds(lua_State* L, const char* caller)
{
	lua_pushnumber(L, simSnapshot.ReadGlobals().luaSimFrame * INV_GAME_SPEED);
	return 1;
}

// mirror of LuaUnsyncedRead::GetGameSecondsInterpolated (timeOffset is
// draw-owned state, read directly)
int LuaSnapshotServe::GetGameSecondsInterpolated(lua_State* L, const char* caller)
{
	lua_pushnumber(L, (simSnapshot.ReadGlobals().luaSimFrame + globalRendering->timeOffset) / GAME_SPEED);
	return 1;
}

// mirror of LuaUnsyncedRead::GetGameSpeed
int LuaSnapshotServe::GetGameSpeed(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadGlobals();

	lua_pushnumber(L, rows.wantedSpeedFactor);
	lua_pushnumber(L, rows.speedFactor);
	lua_pushboolean(L, rows.paused);
	return 3;
}

// mirror of LuaSyncedRead::GetWind
int LuaSnapshotServe::GetWind(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadGlobals();

	lua_pushnumber(L, rows.windVec.x);
	lua_pushnumber(L, rows.windVec.y);
	lua_pushnumber(L, rows.windVec.z);
	lua_pushnumber(L, rows.windStrength);
	lua_pushnumber(L, rows.windDir.x);
	lua_pushnumber(L, rows.windDir.y);
	lua_pushnumber(L, rows.windDir.z);
	return 7;
}

// mirror of LuaSyncedRead::IsCheatingEnabled
int LuaSnapshotServe::IsCheatingEnabled(lua_State* L, const char* caller)
{
	lua_pushboolean(L, simSnapshot.ReadGlobals().cheatEnabled);
	return 1;
}

// mirror of LuaSyncedRead::IsGodModeEnabled
int LuaSnapshotServe::IsGodModeEnabled(lua_State* L, const char* caller)
{
	const int godMode = simSnapshot.ReadGlobals().godMode;

	lua_pushboolean(L, godMode != 0);
	lua_pushboolean(L, (godMode & GODMODE_ATC_BIT) != 0);
	lua_pushboolean(L, (godMode & GODMODE_ETC_BIT) != 0);
	return 3;
}

// mirror of LuaSyncedRead::IsEditDefsEnabled
int LuaSnapshotServe::IsEditDefsEnabled(lua_State* L, const char* caller)
{
	lua_pushboolean(L, simSnapshot.ReadGlobals().editDefsEnabled);
	return 1;
}

// mirror of LuaSyncedRead::AreHelperAIsEnabled
int LuaSnapshotServe::AreHelperAIsEnabled(lua_State* L, const char* caller)
{
	lua_pushboolean(L, !simSnapshot.ReadGlobals().noHelperAIs);
	return 1;
}

// mirror of LuaSyncedRead::IsNoCostEnabled
int LuaSnapshotServe::IsNoCostEnabled(lua_State* L, const char* caller)
{
	lua_pushboolean(L, simSnapshot.ReadGlobals().defsNoCost);
	return 1;
}

// mirror of LuaSyncedRead::IsGameOver (gameOver is captured in the team rows'
// global block since PR 26)
int LuaSnapshotServe::IsGameOver(lua_State* L, const char* caller)
{
	if (game == nullptr)
		return 0;

	lua_pushboolean(L, simSnapshot.ReadTeams().gameOver);
	return 1;
}

// mirror of LuaSyncedRead::GetGroundExtremes
int LuaSnapshotServe::GetGroundExtremes(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadGlobals();

	lua_pushnumber(L, rows.initMinHeight);
	lua_pushnumber(L, rows.initMaxHeight);
	lua_pushnumber(L, rows.currMinHeight);
	lua_pushnumber(L, rows.currMaxHeight);
	return 4;
}

// mirror of LuaSyncedRead::GetGlobalLos
int LuaSnapshotServe::GetGlobalLos(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadGlobals();

	const int allyTeam = luaL_optint(L, 1, CLuaHandle::GetHandleReadAllyTeam(L));
	if (!rows.ValidAllyTeam(allyTeam))
		return 0;

	lua_pushboolean(L, rows.globalLos[allyTeam]);
	return 1;
}
