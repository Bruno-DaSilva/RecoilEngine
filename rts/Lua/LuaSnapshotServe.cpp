/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <algorithm>
#include <cassert> // the rotation twins' IsOrthoNormal assert
#include <cstdio>
#include <cstring>
#include <vector>

#include "LuaHandle.h"
#include "LuaHashString.h" // HSTR_PUSH_BOOL
#include "LuaInclude.h"
#include "LuaSplitContract.h"
#include "LuaUtils.h" // allegiance constants (spatial-list twins)

#include "Game/Camera.h"
#include "Game/Game.h" // the stats callouts' live `game` null-check
#include "Game/GlobalUnsynced.h" // gu->myAllyTeam (IsUnitAllied's fullRead answer)
#include "Game/SelectedUnitsHandler.h" // IsUnitSelected's id-set payload
#include "Game/UI/Groups/Group.h" // CGroup::id (GetUnitGroup)
#include "Game/UI/Groups/GroupHandler.h" // uiGroupHandlers (GetUnitGroup)
#include "Rendering/Common/DrawMapMirrors.h" // PR 28: map-layer mirror serving
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/Common/SnapshotPickGrid.h"
#include "Rendering/Features/FeatureDrawer.h" // CFeatureDrawer::GetDrawFlag / GetUnsyncedTransformMatrix
#include "Rendering/GlobalRendering.h" // timeOffset (draw-owned)
#include "Rendering/IconHandler.h" // icon data pushes (GetUnitIcon/GetUnitIconData)
#include "Rendering/Models/3DModelPiece.hpp" // PR 33 S3DModel / S3DModelPiece (piece metadata)
#include "Rendering/Models/LocalModel.hpp" // PR 33 barrier piece capture
#include "Rendering/Models/LocalModelPiece.hpp" // PR 33 barrier piece capture
#include "Rendering/Units/UnitDrawer.h"
#include "Sim/Features/Feature.h" // CFeature payload reads (luaDraw/noDraw/engineDrawMask/alwaysUpdateMat/selectionVolume); PR 33 feature colvol / hitModelPieces / allyteam
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Features/FeatureHandler.h" // PR 33 feature-piece refresh walk
#include "Sim/Misc/CollisionVolume.h" // WORLD_TO_OBJECT_SPACE
#include "Sim/Misc/GlobalConstants.h" // GAME_SPEED
#include "Sim/Misc/GlobalSynced.h" // GODMODE_*_BIT
#include "Sim/MoveTypes/MoveDefHandler.h" // immutable MoveDef name (GetUnitMoveDefID)
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name twins
#include "Sim/Units/CommandAI/CommandAI.h" // command-queue family boundary copies
#include "Sim/Units/CommandAI/FactoryCAI.h"
#include "Sim/Units/Unit.h" // LOS_* bits
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h" // RefreshCommandQueues' barrier walk; PR 33 unit-piece refresh walk
#include "Sim/Units/Scripts/UnitScript.h" // PR 33 GetUnitScriptPiece/Names
#include "Sim/Units/UnitTypes/Builder.h" // sim|draw PR 30: GetUnitWorkerTask builder decode
#include "Sim/Units/UnitTypes/Factory.h" // CFactory bugger-off scalars
#include "Game/BoundaryStats.h" // sim|draw PR 30: block-copy telemetry
#include "System/Matrix44f.h" // PR 33 GetUnitPieceMatrix (captured model-space matrix)
#include "System/AABB.hpp" // GetUnitsInBox's boxCheck
#include "System/ContainerUtil.h" // spring::VectorSortUnique (GetTeamUnitsByDefs)
#include "System/MainDefines.h" // STRCASECMP (PackBuildQueueSnap)
#include "System/Cpp11Compat.hpp" // spring::random_shuffle (GetTeamUnitsByDefs)
#include "System/Log/ILog.h"
#include "System/SimDrawSplit.h"
#include "System/EventClient.h" // CEventClient special-team constants
#include "System/SpringMath.h" // ClampRadPi (GetUnitHeading)
#include "System/StringHash.h" // hashString (GetUnitSensorRadius)
#include "System/TimeProfiler.h" // ScopedDrawCallinContext
#include "System/UnorderedSet.hpp"

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

	// LuaUnsyncedRead's ParseFeature argument semantics (error text included)
	inline int ParseFeatureIDUnsynced(lua_State* L, const char* caller, int index)
	{
		if (!lua_isnumber(L, index))
			luaL_error(L, "%s(): Bad featureID", caller);

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

	// sanctioned draw-side id->pointer resolution for drawer payload reads that
	// only exist pointer-keyed (GetDrawFlag / the icon-state accessors): the
	// drawer's boundary resolve cache first, the died-in-burst shell second,
	// NEVER the sim-owned handler tables (dogfood invariant; same pattern as
	// the GetUnitDrawFlag live fix, commit 6910e82315). nullptr = dead per
	// drawer; callers answer the "no such unit" nil shape.
	inline const CUnit* ResolveDrawUnit(int unitID)
	{
		const CUnit* unit = DrawerGetObjectByID<CUnit>(unitID);

		if (unit == nullptr)
			unit = SimDrawSplit::ShellFallbackUnit(unitID);

		return unit;
	}

	// draw-side feature resolver for the unsynced-owned payload reads (the
	// luaDraw/noDraw/drawFlag/selection-volume family): the drawer's boundary
	// cache first, the died-in-burst shell second -- NEVER the sim-owned
	// featureHandler (dogfood invariant, doc/pr27b-implementation-notes.md)
	inline const CFeature* ResolveDrawFeature(int featureID)
	{
		const CFeature* feature = DrawerGetObjectByID<CFeature>(featureID);

		if (feature == nullptr)
			feature = SimDrawSplit::ShellFallbackFeature(featureID);

		return feature;
	}

	// unit-flags family (PR 27b serving batch 2): the payloads are unsynced-
	// owned flags on the live CUnit (LuaUnsyncedCtrl / UnitRendering writers);
	// only the ParseUnit visibility gate is a live sim read. Mirror the gate
	// from the snapshot rows (LuaUnsyncedRead::ParseUnit branch order, no ally
	// bypass), then resolve the payload pointer through the boundary-consistent
	// draw-side path (drawer resolve cache + died-in-burst shell fallback,
	// LuaUtils::IdToObject) -- NEVER the sim-owned handler tables (dogfood
	// invariant, doc/pr27b-implementation-notes.md). A resolver miss after a
	// passing gate returns nullptr = the live path's "no such unit" nil shape.
	inline const CUnit* PovResolveUnitUnsynced(lua_State* L, const char* caller)
	{
		const auto& rows = simSnapshot.Read();
		const int unitID = ParseUnitIDUnsynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		// LuaUnsyncedRead::ParseUnit gate mirror
		if (!rows.Valid(unitID))
			return nullptr;
		if (pov.readAllyTeam < 0) {
			if (!pov.fullRead)
				return nullptr;
		} else if ((rows.LosStatus(unitID, pov.readAllyTeam) & (LOS_INLOS | LOS_INRADAR)) == 0) {
			return nullptr;
		}

		return LuaUtils::IdToObject<CUnit>(unitID, caller);
	}

	// mirrors of LuaUnsyncedRead.cpp's file-local Impl::PushIconData /
	// Impl::GetIconDataImpl (keep in lockstep with the live file); the icon
	// tables are load-time data mutated only by main-thread Lua ctrl
	template<bool full>
	void PushIconDataMirror(lua_State* L, const icon::IconData& iconData)
	{
		lua_createtable(L, 0, 2 + 5 * !full);

		LuaPushNamedString(L, "name", iconData.GetName());
		if constexpr (full) {
			LuaPushNamedString(L, "fileName", iconData.GetFileName());
			LuaPushNamedNumber(L, "size", iconData.GetSize());
			LuaPushNamedNumber(L, "distance", iconData.GetDistance());
			LuaPushNamedBool(L, "radiusAdjust", iconData.GetRadiusAdjust());

			{
				const auto& stc = iconData.GetSrcTexCoords();
				lua_pushliteral(L, "srcTexCoords");
				lua_createtable(L, 0, 4);

				LuaPushNamedNumber(L, "x0", stc.x1);
				LuaPushNamedNumber(L, "y0", stc.y1);
				LuaPushNamedNumber(L, "x1", stc.x2);
				LuaPushNamedNumber(L, "y1", stc.y2);

				lua_rawset(L, -3);
			}
		}

		const auto& atc = iconData.GetTexCoords();
		{
			lua_pushliteral(L, "atlasTexCoords");
			lua_createtable(L, 0, 5);

			LuaPushNamedNumber(L, "x0", atc.x1);
			LuaPushNamedNumber(L, "y0", atc.y1);
			LuaPushNamedNumber(L, "x1", atc.x2);
			LuaPushNamedNumber(L, "y1", atc.y2);
			LuaPushNamedNumber(L, "atlasIndex", atc.pageNum);

			lua_rawset(L, -3);
		}
	}

	template<bool full>
	int GetIconDataImplMirror(lua_State* L, size_t iconIdx)
	{
		if (iconIdx == icon::INVALID_ICON_INDEX)
			return 0;

		const auto& iconData = icon::iconHandler.GetIconData(iconIdx);

		PushIconDataMirror<full>(L, iconData);
		return 1;
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

	// PR 27b: the list-returning spatial/team twins deliberately serve
	// ascending-id order instead of master's quadfield-walk / creation order
	// (and GetTeamUnitsByDefs re-shuffles its groups live), so an
	// element-for-element table compare would always "fail" on order. These
	// callouts opt in to a set-mode compare: (numeric key -> numeric value)
	// entries -- the id array slots -- match as multisets ignoring their
	// positions, everything else ("n"/"unknown"/defID keys) matches by key,
	// sub-tables recursing in set mode.
	bool CompareTablesAsIdSet(const char* caller)
	{
		static const spring::unordered_set<std::string> idSetCallouts = {
			"GetAllUnits", "GetTeamUnits", "GetTeamUnitsSorted",
			"GetTeamUnitsByDefs",
			"GetUnitsInRectangle", "GetUnitsInBox",
			"GetUnitsInCylinder", "GetUnitsInSphere",
			"GetFeaturesInRectangle", "GetFeaturesInSphere",
			"GetFeaturesInCylinder", "GetProjectilesInRectangle",
		};
		return (idSetCallouts.find(caller) != idSetCallouts.end());
	}

	bool TablesEqualIdSet(lua_State* L, int a, int b)
	{
		a = luaS_absIndex(L, a);
		b = luaS_absIndex(L, b);

		std::vector<double> aNums;
		std::vector<double> bNums;

		// a's entries: array slots into the multiset, the rest matched in b
		lua_pushnil(L);
		while (lua_next(L, a) != 0) {
			// stack: ... key value
			if (lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER) {
				aNums.push_back(lua_tonumber(L, -1));
				lua_pop(L, 1);
				continue;
			}

			lua_pushvalue(L, -2);
			lua_rawget(L, b); // ... key aValue bValue
			const bool eq = (lua_type(L, -2) == LUA_TTABLE && lua_type(L, -1) == LUA_TTABLE) ?
				TablesEqualIdSet(L, -2, -1) : SlotsEqual(L, -2, -1);
			lua_pop(L, 2); // ... key
			if (!eq) {
				lua_pop(L, 1);
				return false;
			}
		}

		// b's entries: gather its multiset, catch keys a lacks
		lua_pushnil(L);
		while (lua_next(L, b) != 0) {
			if (lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER) {
				bNums.push_back(lua_tonumber(L, -1));
				lua_pop(L, 1);
				continue;
			}

			lua_pushvalue(L, -2);
			lua_rawget(L, a);
			const bool present = !lua_isnil(L, -1);
			lua_pop(L, 2);
			if (!present) {
				lua_pop(L, 1);
				return false;
			}
		}

		std::sort(aNums.begin(), aNums.end());
		std::sort(bNums.begin(), bNums.end());
		return (aNums == bNums);
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

	// PR 27b: the dual-run's premise -- live and snapshot reads observing the
	// same quiescent sim -- is void while the sim thread runs (the live leg
	// would race it and false-flag besides); serve the snapshot directly
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning() && !SimDrawSplit::IsSimParked()) {
		static bool warned = false;
		if (!warned) {
			LOG_L(L_WARNING, "[LuaSnapshotServe::Route] armed dual-run disabled: the sim thread is running (SimDrawSplit=1); field passes at the barrier remain active");
			warned = true;
		}
		return snapFn(L, caller);
	}

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

	// list callouts with the documented ascending-id order deviation compare
	// their result tables as ID sets (see CompareTablesAsIdSet)
	const bool idSetMode = CompareTablesAsIdSet(caller);

	if (!equal) {
		snprintf(detail, sizeof(detail), "return counts differ: live=%d snap=%d", liveN, snapN);
	} else {
		for (int i = 1; i <= liveN; ++i) {
			lua_rawgeti(L, tbl, i); // live value i at tbl+1
			const bool slotEqual =
				(idSetMode && lua_type(L, tbl + 1) == LUA_TTABLE && lua_type(L, base + i) == LUA_TTABLE) ?
				TablesEqualIdSet(L, tbl + 1, base + i) :
				SlotsEqual(L, tbl + 1, base + i);

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


/******************************************************************************
 * Spatial-query + team-unit-list families (PR 27b). The spatial twins take
 * coarse candidates from the SnapshotPickGrid (rebuilt per boundary from the
 * same front buffers they read) and re-apply the live bodies' exact filters:
 * first the quadfield coarse bounds/distance math on raw pos (that check is
 * part of the live result set, not just its broadphase), then the allegiance/
 * visibility/region filters via the Pov mirrors. The list twins serve from a
 * per-boundary team-unit index derived lazily from the front UnitRows.
 *
 * DOCUMENTED DEVIATION (decided): result order is ascending-id, not master's
 * quadfield-walk / active-list / creation order (unreproducible without the
 * live containers). Sets and counts are identical; the armed dual-run
 * compares these callouts as ID sets (CompareTablesAsIdSet).
 ******************************************************************************/

namespace {
	// scratch reused across calls (single-threaded draw context; the twins run
	// no Lua, so no reentrancy -- same pattern as the live gtuObjectIDs)
	std::vector<int> sqCandidateIDs;
	std::vector<int> sqObjectIDs;
	std::vector<int> snapGtuObjectIDs;
	std::vector< std::pair<int, int> > snapGtuDefCounts;

	/**
	 * Per-boundary team-unit index: the snapshot-side equivalent of
	 * unitHandler's per-team / per-(team, def) unit lists the team-list family
	 * walks. Derived lazily on first use per generation by one pass over the
	 * front UnitRows (validity + team + defID) -- no extraction-time work.
	 * Id vectors ascend by construction (the pass ascends).
	 */
	struct TeamUnitIndex {
		uint32_t generation = 0;
		bool built = false;

		std::vector<int> allIDs;                                             // every valid id
		std::vector<std::vector<int>> idsByTeam;                             // [teamID]
		std::vector<spring::unordered_map<int, std::vector<int>>> idsByTeamAndDef; // [teamID][defID]
	};
	TeamUnitIndex teamUnitIndex;

	const TeamUnitIndex& GetTeamUnitIndex()
	{
		TeamUnitIndex& idx = teamUnitIndex;
		const uint32_t gen = simSnapshot.Generation();

		if (idx.built && idx.generation == gen)
			return idx;

		const auto& urows = simSnapshot.Read();
		const auto& trows = simSnapshot.ReadTeams();

		idx.allIDs.clear();
		idx.idsByTeam.resize(trows.activeTeams);
		idx.idsByTeamAndDef.resize(trows.activeTeams);
		for (auto& v: idx.idsByTeam)
			v.clear();
		for (auto& m: idx.idsByTeamAndDef)
			m.clear();

		for (size_t id = 0; id < urows.MaxUnits(); ++id) {
			if (urows.valid[id] == 0)
				continue;

			const int unitID = static_cast<int>(id);
			const int teamID = urows.team[id];

			idx.allIDs.push_back(unitID);

			if (teamID >= 0 && teamID < trows.activeTeams) {
				idx.idsByTeam[teamID].push_back(unitID);
				idx.idsByTeamAndDef[teamID][urows.defID[id]].push_back(unitID);
			}
		}

		idx.built = true;
		idx.generation = gen;
		return idx;
	}

	// unitHandler.GetUnitsByTeamAndDef mirror shape: empty vector for defs the
	// team has no units of
	const std::vector<int>& IdsByTeamAndDef(const TeamUnitIndex& idx, int teamID, int unitDefID)
	{
		static const std::vector<int> empty;

		const auto& defMap = idx.idsByTeamAndDef[teamID];
		const auto it = defMap.find(unitDefID);

		return (it == defMap.end()) ? empty : it->second;
	}

	// LuaUtils::ParseAllegiance mirror (identical error text); the only live
	// read is teamHandler.ActiveTeams() -> the team boundary copy
	inline int ParseAllegianceMirror(lua_State* L, const char* caller, int index)
	{
		if (!lua_isnumber(L, index))
			return LuaUtils::AllUnits;

		const int teamID = lua_toint(L, index);

		// MyUnits, AllyUnits, and EnemyUnits do not apply to fullRead
		if (CLuaHandle::GetHandleFullRead(L) && (teamID < 0))
			return LuaUtils::AllUnits;

		if (teamID < LuaUtils::EnemyUnits) {
			luaL_error(L, "Bad teamID in %s (%d)", caller, teamID);
		}
		else if (teamID >= simSnapshot.ReadTeams().activeTeams) {
			luaL_error(L, "Bad teamID in %s (%d)", caller, teamID);
		}

		return teamID;
	}

	// LuaSyncedRead's ApplyPlanarTeamError mirror; the live reads are
	// teamHandler.AllyTeam (TeamRows) and losHandler->GetAllyTeamRadarErrorSize
	// (the UnitRows radar-error scalars). Only reached when !fullRead; a
	// negative readAllyTeam cannot arise for a real non-fullRead handle (the
	// live path would index radarErrorSizes out of bounds there), so it reads
	// as no expansion -- every unit fails the visibility filters anyway
	inline void ApplyPlanarTeamErrorMirror(lua_State* L, int allegiance, float3& mins, float3& maxs)
	{
		const Pov pov = HandlePov(L);
		const auto& trows = simSnapshot.ReadTeams();

		if ((allegiance >= 0 && !trows.PovAlliedTeam(allegiance, pov.readAllyTeam, pov.fullRead)) ||
		   !(allegiance == LuaUtils::MyUnits || allegiance == LuaUtils::AllyUnits)) {
			const auto& urows = simSnapshot.Read();

			if (pov.readAllyTeam < 0 || pov.readAllyTeam >= urows.numAllyTeams)
				return;

			const float allyTeamError = urows.radarErrorSizes[pov.readAllyTeam];
			const float3 allyTeamError3(allyTeamError, 0.0f, allyTeamError);
			mins -= allyTeamError3;
			maxs += allyTeamError3;
		}
	}

	// CQuadField::GetUnitsExact(mins, maxs) mirror over the pick grid: coarse
	// superset from the grid cells, then the exact raw-pos bounds the live
	// query applies -- the candidate SET matches live, only its order deviates
	const std::vector<int>& QuadfieldUnitsExactMirror(const SimSnapshot::UnitRows& rows, const float3& mins, const float3& maxs)
	{
		snapshotPickGrid.QueryUnitsInRect(mins, maxs, sqCandidateIDs);

		sqObjectIDs.clear();

		for (const int unitID: sqCandidateIDs) {
			const float3& pos = rows.pos[unitID];
			if (pos.x < mins.x || pos.x > maxs.x)
				continue;
			if (pos.z < mins.z || pos.z > maxs.z)
				continue;

			sqObjectIDs.push_back(unitID);
		}

		return sqObjectIDs;
	}

	// LuaSyncedRead's GetFilteredUnits mirror: identical allegiance
	// disqualifier branches and the same midPos + GetLuaErrorVector position
	// input; candidates arrive pre-filtered by QuadfieldUnitsExactMirror
	template<typename InRegion>
	void GetFilteredUnitsSnap(lua_State* L, const SimSnapshot::UnitRows& rows,
		int allegiance, const std::vector<int>& unitIDs, InRegion inRegion)
	{
		const int readTeam = CLuaHandle::GetHandleReadTeam(L);
		const int readAllyTeam = CLuaHandle::GetHandleReadAllyTeam(L);
		const bool fullRead = CLuaHandle::GetHandleFullRead(L);

		auto runLoop = [&](auto disqualifier) {
			unsigned int count = 0;
			for (const int unitID : unitIDs) {
				if (disqualifier(unitID))
					continue;

				float3 pos = rows.midPos[unitID] + rows.LuaErrorVector(unitID, readAllyTeam, fullRead);
				if (!inRegion(unitID, pos))
					continue;

				lua_pushnumber(L, unitID);
				lua_rawseti(L, -2, ++count);
			}
		};

		switch (allegiance) {
			case LuaUtils::AllUnits:
				runLoop([&](int u) { return !rows.PovUnitVisible(u, readAllyTeam, fullRead); });
				break;
			case LuaUtils::MyUnits:
				runLoop([&](int u) { return rows.team[u] != readTeam || !rows.PovUnitVisible(u, readAllyTeam, fullRead); });
				break;
			case LuaUtils::AllyUnits:
				runLoop([&](int u) { return rows.allyTeam[u] != readAllyTeam || !rows.PovUnitVisible(u, readAllyTeam, fullRead); });
				break;
			case LuaUtils::EnemyUnits:
				runLoop([&](int u) { return rows.allyTeam[u] == readAllyTeam || !rows.PovUnitVisible(u, readAllyTeam, fullRead); });
				break;
			default:
				runLoop([&](int u) { return rows.team[u] != allegiance || !rows.PovUnitVisible(u, readAllyTeam, fullRead); });
				break;
		}
	}

	// LuaSyncedRead's ProcessFeatures mirror: same branch structure, the
	// IsFeatureVisible gate via PovFeatureVisible
	void ProcessFeaturesSnap(lua_State* L, const SimSnapshot::FeatureRows& rows, const std::vector<int>& featureIDs)
	{
		const unsigned int featureCount = featureIDs.size();
		unsigned int arrayIndex = 1;

		lua_createtable(L, featureCount, 0);

		if (CLuaHandle::GetHandleReadAllyTeam(L) < 0) {
			if (CLuaHandle::GetHandleFullRead(L)) {
				for (unsigned int i = 0; i < featureCount; i++) {
					lua_pushnumber(L, featureIDs[i]);
					lua_rawseti(L, -2, arrayIndex++);
				}
			}
		} else {
			const Pov pov = HandlePov(L);

			for (unsigned int i = 0; i < featureCount; i++) {
				const int featureID = featureIDs[i];

				if (!PovFeatureVisible(rows, featureID, pov)) {
					continue;
				}

				lua_pushnumber(L, featureID);
				lua_rawseti(L, -2, arrayIndex++);
			}
		}
	}

	// LuaSyncedRead's GetProjectilesLuaTable mirror; candidates are synced by
	// construction (only synced projectiles have rows -- the !pro->synced skip)
	void GetProjectilesLuaTableSnap(lua_State* L, const SimSnapshot::ProjectileRows& rows,
		const std::vector<int>& projectileIDs, bool excludeWeaponProjectiles, bool excludePieceProjectiles)
	{
		int arrayIndex = 1;

		lua_createtable(L, static_cast<int>(projectileIDs.size()), 0);

		if (CLuaHandle::GetHandleReadAllyTeam(L) < 0) {
			if (CLuaHandle::GetHandleFullRead(L)) {
				for (const int projID : projectileIDs) {
					if (rows.isWeapon[projID] && excludeWeaponProjectiles)
						continue;
					if (rows.isPiece[projID] && excludePieceProjectiles)
						continue;

					lua_pushinteger(L, projID);
					lua_rawseti(L, -2, arrayIndex++);
				}
			}
		} else {
			const Pov pov = HandlePov(L);

			for (const int projID : projectileIDs) {
				if (rows.isWeapon[projID] && excludeWeaponProjectiles)
					continue;
				if (rows.isPiece[projID] && excludePieceProjectiles)
					continue;

				if (!rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
					continue;

				lua_pushinteger(L, projID);
				lua_rawseti(L, -2, arrayIndex++);
			}
		}
	}

	// LuaSyncedRead's PushVisibleUnits mirror (GetTeamUnitsSorted's enemy
	// tally); unknown-typed ids collect in snapGtuObjectIDs like gtuObjectIDs
	bool PushVisibleUnitsSnap(
		lua_State* L,
		const SimSnapshot::UnitRows& rows,
		const Pov& pov,
		const std::vector<int>& defUnitIDs,
		int unitDefID,
		unsigned int* unitCount,
		unsigned int* defCount
	) {
		bool createdTable = false;

		for (const int unitID: defUnitIDs) {
			if (!rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
				continue;

			if (!rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead)) {
				snapGtuObjectIDs.push_back(unitID);
				continue;
			}

			// push new table for first unit of type <unitDefID> to be visible
			if (!createdTable) {
				createdTable = true;

				lua_pushnumber(L, unitDefID);
				lua_createtable(L, defUnitIDs.size(), 0);

				(*defCount)++;
			}

			// add count-th unitID to table
			lua_pushnumber(L, unitID);
			lua_rawseti(L, -2, (*unitCount)++);
		}

		return createdTable;
	}

	// LuaSyncedRead's InsertSearchUnitDefs mirror (immutable def data)
	inline void InsertSearchUnitDefsSnap(const UnitDef* ud, bool allied)
	{
		if (ud == nullptr)
			return;

		if (!allied && ud->decoyDef)
			return;

		snapGtuObjectIDs.push_back(ud->id);
	}
}


// mirror of LuaSyncedRead::GetAllUnits
int LuaSnapshotServe::GetAllUnits(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();
	const Pov pov = HandlePov(L);

	// DEVIATION: ascending ids (master lists unitHandler's active-unit order)
	lua_createtable(L, idx.allIDs.size(), 0);

	unsigned int unitCount = 1;
	if (pov.fullRead) {
		for (const int unitID: idx.allIDs) {
			lua_pushnumber(L, unitID);
			lua_rawseti(L, -2, unitCount++);
		}
	} else {
		for (const int unitID: idx.allIDs) {
			if (!rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
				continue;

			lua_pushnumber(L, unitID);
			lua_rawseti(L, -2, unitCount++);
		}
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamUnits
int LuaSnapshotServe::GetTeamUnits(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	// parse the team
	const auto& trows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, trows);
	const Pov pov = HandlePov(L);

	const auto& urows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();
	const std::vector<int>& teamUnitIDs = idx.idsByTeam[teamID];

	unsigned int unitCount = 1;

	// raw push for allies -- DEVIATION: ascending ids, not creation order
	if (trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead)) {
		lua_createtable(L, teamUnitIDs.size(), 0);

		for (const int unitID: teamUnitIDs) {
			lua_pushnumber(L, unitID);
			lua_rawseti(L, -2, unitCount++);
		}

		return 1;
	}

	// check visibility for enemies
	lua_createtable(L, teamUnitIDs.size(), 0);

	for (const int unitID: teamUnitIDs) {
		if (!urows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
			continue;
		lua_pushnumber(L, unitID);
		lua_rawseti(L, -2, unitCount++);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamUnitsSorted (NB the live quirk: unitCount is
// cumulative across the allied branch's def tables, so their array keys are
// not 1-based per table -- mirrored, the keys must match; only the within-def
// id order deviates)
int LuaSnapshotServe::GetTeamUnitsSorted(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	// parse the team
	const auto& trows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, trows);
	const Pov pov = HandlePov(L);

	const auto& urows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();

	unsigned int defCount = 0;
	unsigned int unitCount = 1;

	// table = {[unitDefID] = {[1] = unitID, [2] = unitID, ...}}
	lua_createtable(L, unitDefHandler->NumUnitDefs(), 0);

	if (trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead)) {
		// tally for allies -- DEVIATION: ascending ids inside each def table
		for (unsigned int i = 0, n = unitDefHandler->NumUnitDefs(); i < n; i++) {
			const std::vector<int>& unitsByDef = IdsByTeamAndDef(idx, teamID, i + 1);

			if (unitsByDef.empty())
				continue;

			lua_pushnumber(L, i + 1);
			lua_createtable(L, unitsByDef.size(), 0);
			defCount++;

			for (const int unitID: unitsByDef) {
				lua_pushnumber(L, unitID);
				lua_rawseti(L, -2, unitCount++);
			}
			lua_rawset(L, -3);
		}
	} else {
		// tally for enemies
		snapGtuObjectIDs.clear();
		snapGtuObjectIDs.reserve(16);

		for (unsigned int i = 0, n = unitDefHandler->NumUnitDefs(); i < n; i++) {
			const unsigned int unitDefID = i + 1;

			const UnitDef* ud = unitDefHandler->GetUnitDefByID(unitDefID);

			// we deal with decoys later
			if (ud->decoyDef != nullptr)
				continue;

			bool createdTable = PushVisibleUnitsSnap(L, urows, pov, IdsByTeamAndDef(idx, teamID, unitDefID), unitDefID, &unitCount, &defCount);

			// for all decoy-defs of unitDefID, add decoy units under the same ID
			const auto& decoyMap = unitDefHandler->GetDecoyDefIDs();
			const auto decoyMapIt = decoyMap.find(unitDefID);

			if (decoyMapIt != decoyMap.end()) {
				for (int decoyDefID: decoyMapIt->second) {
					createdTable |= PushVisibleUnitsSnap(L, urows, pov, IdsByTeamAndDef(idx, teamID, decoyDefID), unitDefID, &unitCount, &defCount);
				}
			}

			if (createdTable)
				lua_rawset(L, -3);

		}

		if (!snapGtuObjectIDs.empty()) {
			HSTR_PUSH(L, "unknown");

			defCount += 1;
			unitCount = 1;

			lua_createtable(L, snapGtuObjectIDs.size(), 0);

			for (int unitID: snapGtuObjectIDs) {
				lua_pushnumber(L, unitID);
				lua_rawseti(L, -2, unitCount++);
			}
			lua_rawset(L, -3);
		}
	}

	// UnitDef ID keys are not consecutive, so add the "n"
	HSTR_PUSH_NUMBER(L, "n", defCount);
	return 1;
}

// mirror of LuaSyncedRead::GetTeamUnitsCounts
int LuaSnapshotServe::GetTeamUnitsCounts(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	// parse the team
	const auto& trows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, trows);
	const Pov pov = HandlePov(L);

	const auto& urows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();

	unsigned int unknownCount = 0;
	unsigned int defCount = 0;

	// send the raw unitsByDefs counts for allies
	if (trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead)) {
		lua_createtable(L, unitDefHandler->NumUnitDefs(), 0);

		for (unsigned int i = 0, n = unitDefHandler->NumUnitDefs(); i < n; i++) {
			const unsigned int unitDefID = i + 1;
			const unsigned int unitCount = IdsByTeamAndDef(idx, teamID, unitDefID).size();

			if (unitCount == 0)
				continue;

			lua_pushnumber(L, unitCount);
			lua_rawseti(L, -2, unitDefID);
			defCount++;
		}

		// keys are not necessarily consecutive here due to
		// the unitCount check, so add the "n" key manually
		HSTR_PUSH_NUMBER(L, "n", defCount);
		return 1;
	}

	// tally the counts for enemies
	snapGtuDefCounts.clear();
	snapGtuDefCounts.resize(unitDefHandler->NumUnitDefs() + 1, {0, 0});

	for (const int unitID: idx.idsByTeam[teamID]) {
		if (!urows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
			continue;

		if (!urows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead)) {
			unknownCount++;
		} else {
			// LuaUtils::EffectiveUnitDef mirror (immutable def data)
			const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.defID[unitID]);
			const UnitDef* unitDef = (urows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead) || ud->decoyDef == nullptr) ? ud : ud->decoyDef;

			snapGtuDefCounts[unitDef->id].first = unitDef->id;
			snapGtuDefCounts[unitDef->id].second += 1;
		}
	}

	// push the counts
	lua_createtable(L, 0, snapGtuDefCounts.size());

	for (const auto& gtuDefCount: snapGtuDefCounts) {
		if (gtuDefCount.second == 0)
			continue;
		lua_pushnumber(L, gtuDefCount.second);
		lua_rawseti(L, -2, gtuDefCount.first);
		defCount++;
	}
	if (unknownCount > 0) {
		HSTR_PUSH_NUMBER(L, "unknown", unknownCount);
		defCount++;
	}

	// unitDef->id is used for ordering, so not consecutive
	HSTR_PUSH_NUMBER(L, "n", defCount);
	return 1;
}

// mirror of LuaSyncedRead::GetTeamUnitsByDefs
int LuaSnapshotServe::GetTeamUnitsByDefs(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	const auto& trows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, trows);
	const Pov pov = HandlePov(L);

	const bool allied = trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead);

	// parse the unitDefs
	snapGtuObjectIDs.clear();
	snapGtuObjectIDs.reserve(16);

	if (lua_isnumber(L, 2)) {
		InsertSearchUnitDefsSnap(unitDefHandler->GetUnitDefByID(lua_toint(L, 2)), allied);
	} else if (lua_istable(L, 2)) {
		const int tableIdx = 2;

		for (lua_pushnil(L); lua_next(L, tableIdx) != 0; lua_pop(L, 1)) {
			if (!lua_isnumber(L, LUA_TABLE_VALUE_INDEX))
				continue;

			InsertSearchUnitDefsSnap(unitDefHandler->GetUnitDefByID(lua_toint(L, LUA_TABLE_VALUE_INDEX)), allied);
		}
	} else {
		luaL_error(L, "Incorrect arguments to GetTeamUnitsByDefs()");
	}

	// sort the ID's so duplicates can be skipped
	spring::VectorSortUnique(snapGtuObjectIDs);

	const auto& urows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();
	const std::vector<int>& teamUnitIDs = idx.idsByTeam[teamID];

	std::vector<int> unitIDs;
	size_t lastOfsset = 0;
	bool isCalledFromSynced = CLuaHandle::GetHandleSynced(L);

	for (const int unitDefID: snapGtuObjectIDs) {
		for (const int unitID: teamUnitIDs) {
			if (!allied && !urows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
				continue;

			// immutable def data (the unit->unitDef deref)
			const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.defID[unitID]);

			if (ud->id == unitDefID || (!allied && ud->decoyDef && ud->decoyDef->id == unitDefID)) {
				unitIDs.emplace_back(unitID);
			}
		}

		if (isCalledFromSynced)
			continue;

		/* kept from the live body: the per-def groups are shuffled so the
		 * result order reveals nothing (ascending id would leak creation
		 * order); the armed dual-run compares this callout as an ID set */
		spring::random_shuffle(unitIDs.begin() + lastOfsset, unitIDs.end(), guRNG);
		lastOfsset = unitIDs.size();
	}

	lua_createtable(L, unitIDs.size(), 0);

	for (int i = 0; i < unitIDs.size(); ++i) {
		lua_pushnumber(L, unitIDs[i]);
		lua_rawseti(L, -2, i + 1);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamUnitDefCount
int LuaSnapshotServe::GetTeamUnitDefCount(lua_State* L, const char* caller)
{
	if (CLuaHandle::GetHandleReadAllyTeam(L) == CEventClient::NoAccessTeam)
		return 0;

	// parse the team
	const auto& trows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, trows);
	const Pov pov = HandlePov(L);

	const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(luaL_checkint(L, 2));

	if (unitDef == nullptr)
		luaL_error(L, "Bad unitDefID in GetTeamUnitDefCount()");

	const auto& urows = simSnapshot.Read();
	const TeamUnitIndex& idx = GetTeamUnitIndex();

	// use the unitsByDefs count for allies
	if (trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead)) {
		lua_pushnumber(L, IdsByTeamAndDef(idx, teamID, unitDef->id).size());
		return 1;
	}

	// you can never count enemy decoys
	if (unitDef->decoyDef != nullptr) {
		lua_pushnumber(L, 0);
		return 1;
	}

	unsigned int unitCount = 0;

	// tally the given unitDef units
	for (const int unitID: IdsByTeamAndDef(idx, teamID, unitDef->id)) {
		unitCount += (urows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead));
	}

	// tally the decoy units for the given unitDef
	const auto& decoyMap = unitDefHandler->GetDecoyDefIDs();
	const auto decoyMapIt = decoyMap.find(unitDef->id);

	if (decoyMapIt != decoyMap.end()) {
		for (const int udID: decoyMapIt->second) {
			for (const int unitID: IdsByTeamAndDef(idx, teamID, udID)) {
				unitCount += (urows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead));
			}
		}
	}

	lua_pushnumber(L, unitCount);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitsInRectangle
int LuaSnapshotServe::GetUnitsInRectangle(lua_State* L, const char* caller)
{
	const float xmin = luaL_checkfloat(L, 1);
	const float zmin = luaL_checkfloat(L, 2);
	const float xmax = luaL_checkfloat(L, 3);
	const float zmax = luaL_checkfloat(L, 4);

	float3 mins(xmin, 0.0f, zmin);
	float3 maxs(xmax, 0.0f, zmax);

	const int allegiance = ParseAllegianceMirror(L, caller, 5);

	const auto rectangleCheck = [&](int unitID, const float3 &pos) {
		if((pos.x < xmin) || (pos.x > xmax))
			return false;
		if((pos.z < zmin) || (pos.z > zmax))
			return false;
		return true;
	};

	const bool fullRead = CLuaHandle::GetHandleFullRead(L);
	if (!fullRead)
		ApplyPlanarTeamErrorMirror(L, allegiance, mins, maxs);

	const auto& rows = simSnapshot.Read();
	const std::vector<int>& units = QuadfieldUnitsExactMirror(rows, mins, maxs);

	lua_createtable(L, units.size(), 0);

	GetFilteredUnitsSnap(L, rows, allegiance, units, rectangleCheck);

	return 1;
}

// mirror of LuaSyncedRead::GetUnitsInBox
int LuaSnapshotServe::GetUnitsInBox(lua_State* L, const char* caller)
{
	const float xmin = luaL_checkfloat(L, 1);
	const float ymin = luaL_checkfloat(L, 2);
	const float zmin = luaL_checkfloat(L, 3);
	const float xmax = luaL_checkfloat(L, 4);
	const float ymax = luaL_checkfloat(L, 5);
	const float zmax = luaL_checkfloat(L, 6);

	float3 mins(xmin, 0.0f, zmin);
	float3 maxs(xmax, 0.0f, zmax);

	const int allegiance = ParseAllegianceMirror(L, caller, 7);

	const auto boxCheck = [&](int unitID, float3 pos) {
		return AABB(float3(xmin, ymin, zmin), float3(xmax, ymax, zmax)).Contains(pos);
	};

	const bool fullRead = CLuaHandle::GetHandleFullRead(L);
	if (!fullRead)
		ApplyPlanarTeamErrorMirror(L, allegiance, mins, maxs);

	const auto& rows = simSnapshot.Read();
	const std::vector<int>& units = QuadfieldUnitsExactMirror(rows, mins, maxs);

	lua_createtable(L, units.size(), 0);

	GetFilteredUnitsSnap(L, rows, allegiance, units, boxCheck);

	return 1;
}

// mirror of LuaSyncedRead::GetUnitsInCylinder
int LuaSnapshotServe::GetUnitsInCylinder(lua_State* L, const char* caller)
{
	const float x      = luaL_checkfloat(L, 1);
	const float z      = luaL_checkfloat(L, 2);
	const float radius = luaL_checkfloat(L, 3);
	const float radSqr = (radius * radius);

	float3 mins(x - radius, 0.0f, z - radius);
	float3 maxs(x + radius, 0.0f, z + radius);

	const int allegiance = ParseAllegianceMirror(L, caller, 4);

	const auto cylinderCheck = [&](int unitID, const float3 &p) {
		return p.SqDistance2D(float3{x, 0.0, z}) <= radSqr;
	};

	const bool fullRead = CLuaHandle::GetHandleFullRead(L);
	if (!fullRead)
		ApplyPlanarTeamErrorMirror(L, allegiance, mins, maxs);

	const auto& rows = simSnapshot.Read();
	const std::vector<int>& units = QuadfieldUnitsExactMirror(rows, mins, maxs);

	lua_createtable(L, units.size(), 0);

	GetFilteredUnitsSnap(L, rows, allegiance, units, cylinderCheck);

	return 1;
}

// mirror of LuaSyncedRead::GetUnitsInSphere
int LuaSnapshotServe::GetUnitsInSphere(lua_State* L, const char* caller)
{
	const float x      = luaL_checkfloat(L, 1);
	const float y      = luaL_checkfloat(L, 2);
	const float z      = luaL_checkfloat(L, 3);
	const float radius = luaL_checkfloat(L, 4);
	const float radSqr = (radius * radius);

	float3 mins(x - radius, 0.0f, z - radius);
	float3 maxs(x + radius, 0.0f, z + radius);

	const int allegiance = ParseAllegianceMirror(L, caller, 5);

	const auto sphereCheck = [&](int unitID, const float3 &p) {
		return p.SqDistance(float3(x, y, z)) <= radSqr;
	};

	const bool fullRead = CLuaHandle::GetHandleFullRead(L);
	if (!fullRead)
		ApplyPlanarTeamErrorMirror(L, allegiance, mins, maxs);

	const auto& rows = simSnapshot.Read();
	const std::vector<int>& units = QuadfieldUnitsExactMirror(rows, mins, maxs);

	lua_createtable(L, units.size(), 0);

	GetFilteredUnitsSnap(L, rows, allegiance, units, sphereCheck);

	return 1;
}

// mirror of LuaSyncedRead::GetFeaturesInRectangle
int LuaSnapshotServe::GetFeaturesInRectangle(lua_State* L, const char* caller)
{
	const float xmin = luaL_checkfloat(L, 1);
	const float zmin = luaL_checkfloat(L, 2);
	const float xmax = luaL_checkfloat(L, 3);
	const float zmax = luaL_checkfloat(L, 4);

	const float3 mins(xmin, 0.0f, zmin);
	const float3 maxs(xmax, 0.0f, zmax);

	const auto& rows = simSnapshot.ReadFeatures();

	// CQuadField::GetFeaturesExact(mins, maxs) mirror: grid superset + the
	// exact raw-pos bounds
	snapshotPickGrid.QueryFeaturesInRect(mins, maxs, sqCandidateIDs);

	sqObjectIDs.clear();

	for (const int featureID: sqCandidateIDs) {
		const float3& pos = rows.pos[featureID];
		if (pos.x < mins.x || pos.x > maxs.x)
			continue;
		if (pos.z < mins.z || pos.z > maxs.z)
			continue;

		sqObjectIDs.push_back(featureID);
	}

	ProcessFeaturesSnap(L, rows, sqObjectIDs);
	return 1;
}

// mirror of LuaSyncedRead::GetFeaturesInSphere
int LuaSnapshotServe::GetFeaturesInSphere(lua_State* L, const char* caller)
{
	const float x = luaL_checkfloat(L, 1);
	const float y = luaL_checkfloat(L, 2);
	const float z = luaL_checkfloat(L, 3);
	const float rad = luaL_checkfloat(L, 4);

	const float3 pos(x, y, z);

	const auto& rows = simSnapshot.ReadFeatures();

	// CQuadField::GetFeaturesExact(pos, rad, true) mirror: the live gather
	// clamps its centre in-bounds (GetQuads) but tests distance against the
	// raw pos, with the search radius widened per candidate by its own radius
	snapshotPickGrid.QueryFeaturesInRadius(pos.cClampInBounds(), rad, sqCandidateIDs);

	sqObjectIDs.clear();

	for (const int featureID: sqCandidateIDs) {
		const float totRad   = rad + rows.radius[featureID];
		const float totRadSq = totRad * totRad;
		const float posDstSq = pos.SqDistance(rows.pos[featureID]);

		if (posDstSq >= totRadSq)
			continue;

		sqObjectIDs.push_back(featureID);
	}

	ProcessFeaturesSnap(L, rows, sqObjectIDs);
	return 1;
}

// mirror of LuaSyncedRead::GetFeaturesInCylinder
int LuaSnapshotServe::GetFeaturesInCylinder(lua_State* L, const char* caller)
{
	const float x = luaL_checkfloat(L, 1);
	const float z = luaL_checkfloat(L, 2);
	const float rad = luaL_checkfloat(L, 3);

	const float3 pos(x, 0, z);

	const auto& rows = simSnapshot.ReadFeatures();

	// CQuadField::GetFeaturesExact(pos, rad, false) mirror (2D distance)
	snapshotPickGrid.QueryFeaturesInRadius(pos.cClampInBounds(), rad, sqCandidateIDs);

	sqObjectIDs.clear();

	for (const int featureID: sqCandidateIDs) {
		const float totRad   = rad + rows.radius[featureID];
		const float totRadSq = totRad * totRad;
		const float posDstSq = pos.SqDistance2D(rows.pos[featureID]);

		if (posDstSq >= totRadSq)
			continue;

		sqObjectIDs.push_back(featureID);
	}

	ProcessFeaturesSnap(L, rows, sqObjectIDs);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectilesInRectangle
int LuaSnapshotServe::GetProjectilesInRectangle(lua_State* L, const char* caller)
{
	const float xmin = luaL_checkfloat(L, 1);
	const float zmin = luaL_checkfloat(L, 2);
	const float xmax = luaL_checkfloat(L, 3);
	const float zmax = luaL_checkfloat(L, 4);

	const bool excludeWeaponProjectiles = luaL_optboolean(L, 5, false);
	const bool excludePieceProjectiles = luaL_optboolean(L, 6, false);

	const float3 mins(xmin, 0.0f, zmin);
	const float3 maxs(xmax, 0.0f, zmax);

	const auto& rows = simSnapshot.ReadProjectiles();

	// CQuadField::GetProjectilesExact(mins, maxs) mirror by linear scan (no
	// projectile grid rows): validity + the exact raw-pos bounds
	sqObjectIDs.clear();

	for (size_t projID = 0; projID < rows.MaxSlots(); ++projID) {
		if (rows.valid[projID] == 0)
			continue;

		const float3& pos = rows.pos[projID];
		if (pos.x < mins.x || pos.x > maxs.x)
			continue;
		if (pos.z < mins.z || pos.z > maxs.z)
			continue;

		sqObjectIDs.push_back(static_cast<int>(projID));
	}

	GetProjectilesLuaTableSnap(L, rows, sqObjectIDs, excludeWeaponProjectiles, excludePieceProjectiles);
	return 1;
}


/******************************************************************************
 * Unsynced flag/drawer parse-gate family (PR 27b serving batch 2, family 2).
 * The payloads are draw-owned (unsynced object flags, drawer icon/draw-flag
 * state, drawer transforms, UI selection/group tables) -- only the ParseUnit/
 * ParseFeature visibility gate was a live sim read. Gates mirror the live
 * parse helpers from the snapshot rows; payload pointers, where needed at
 * all, resolve through the sanctioned draw-side path (drawer boundary cache
 * + died-in-burst shell), never the sim-owned handler tables.
 ******************************************************************************/

// mirror of LuaUnsyncedRead::GetUnitLuaDraw (the luaDraw flag is unsynced-
// owned object state, written only by Spring.UnitRendering.SetUnitLuaDraw on
// the draw thread; only the ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitLuaDraw(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->luaDraw);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitNoDraw (the noDraw flag is unsynced-owned
// object state, boundary-applied by SetUnitNoDraw under the split; only the
// ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitNoDraw(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->noDraw);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitNoMinimap (the noMinimap flag is
// unsynced-owned, boundary-applied by SetUnitNoMinimap under the split; only
// the ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitNoMinimap(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->noMinimap);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitNoGroup (the noGroup flag is unsynced-
// owned, written synchronously on the draw thread by SetUnitNoGroup; only the
// ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitNoGroup(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->noGroup);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitNoSelect; the flag is read live off the
// draw-resolved object rather than the rows.noSelect boundary row so a
// draw-context SetUnitNoSelect (synchronous poke under the split) keeps its
// read-your-own-write semantics; the residual is sim's UpdateVoidState
// two-writer race, a torn single-byte bool -- the tolerated section-C class
int LuaSnapshotServe::GetUnitNoSelect(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->noSelect);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitEngineDrawMask (the mask is unsynced-
// owned, boundary-applied by SetUnitEngineDrawMask under the split; only the
// ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitEngineDrawMask(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushinteger(L, unit->engineDrawMask);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitAlwaysUpdateMatrix (the flag is unsynced-
// owned, boundary-applied by SetUnitAlwaysUpdateMatrix under the split; only
// the ParseUnit gate was a sim read)
int LuaSnapshotServe::GetUnitAlwaysUpdateMatrix(lua_State* L, const char* caller)
{
	const CUnit* unit = PovResolveUnitUnsynced(L, caller);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, unit->alwaysUpdateMat);
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitDrawFlag; the flag itself is drawer-owned
// (id-keyed DrawFlagState storage), only the ParseUnit gate was a live sim
// read. Folds the dogfood fix (commit 6910e82315): the payload pointer comes
// from the drawer's boundary resolve cache with the died-in-burst shell as
// fallback, never the sim-owned handler tables; a double miss answers the
// same "no such unit" nil shape.
int LuaSnapshotServe::GetUnitDrawFlag(lua_State* L, const char* caller)
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

	const CUnit* unit = ResolveDrawUnit(unitID);

	if (unit == nullptr)
		return 0;

	lua_pushinteger(L, CUnitDrawer::GetDrawFlag(unit));
	return 1;
}

// mirror of LuaUnsyncedRead::UnitIconGetDraw; drawIcon is drawer-owned
// per-unit icon state (id-keyed UnitIconState, PR 5 eviction), written by
// main-thread Lua ctrl (Spring.SetUnitIconDraw) only -- the ParseUnit gate
// was the sole live sim read. Pointer-keyed accessor, so resolve through
// the sanctioned draw-side path.
int LuaSnapshotServe::UnitIconGetDraw(lua_State* L, const char* caller)
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

	const CUnit* unit = ResolveDrawUnit(unitID);

	if (unit == nullptr)
		return 0;

	lua_pushboolean(L, CUnitDrawer::GetUnitDrawIcon(unit));
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitIcon; currentIconIndex is drawer-owned
// per-unit icon state (PR 5 eviction, authored by the draw-side icon pass)
// and the iconHandler tables are load-time data mutated only by main-thread
// Lua ctrl -- the ParseUnit gate was the sole live sim read
int LuaSnapshotServe::GetUnitIcon(lua_State* L, const char* caller)
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

	const CUnit* unit = ResolveDrawUnit(unitID);

	if (unit == nullptr)
		return 0;

	const auto iconIdx = CUnitDrawer::GetUnitIconIndex(unit);

	if (iconIdx == icon::INVALID_ICON_INDEX) {
		lua_pushstring(L, "");
	}
	else {
		const auto& iconData = icon::iconHandler.GetIconData(iconIdx);
		lua_pushstring(L, iconData.GetName().c_str());
	}

	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitIconData; same ownership story as the
// GetUnitIcon twin. The optional fullData arg is read BEFORE the gate, like
// the live body reads it before its nullptr check (stack-order fidelity:
// a bad arg #2 must raise the same error even for invisible/invalid ids)
int LuaSnapshotServe::GetUnitIconData(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDUnsynced(L, caller, 1);
	const auto fullData = luaL_optboolean(L, 2, false);
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

	const CUnit* unit = ResolveDrawUnit(unitID);

	if (unit == nullptr)
		return 0;

	if (fullData)
		return GetIconDataImplMirror<true >(L, CUnitDrawer::GetUnitIconIndex(unit));
	else
		return GetIconDataImplMirror<false>(L, CUnitDrawer::GetUnitIconIndex(unit));
}

// mirror of LuaUnsyncedRead::IsUnitSelected; selectedUnits is an id set
// owned by main-thread UI code (deaths arrive via DeliverBoundaryDeaths
// under the split), so the payload read stays live and needs no object
// pointer at all -- only the ParseUnit gate was a sim read
int LuaSnapshotServe::IsUnitSelected(lua_State* L, const char* caller)
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

	const auto& selUnits = selectedUnitsHandler.selectedUnits;
	lua_pushboolean(L, selUnits.find(unitID) != selUnits.end());
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitGroup; the group tables are unsynced
// UI state (uiGroupHandlers, id-keyed) and CUnit::GetGroup() is just
// uiGroupHandlers[team].GetUnitGroup(id) -- the live sim reads were the
// ParseUnit gate and unit->team, both served from the rows (the team row
// is the boundary answer, consistent with the gate). team == gu->myTeam
// after the check, so the handler index needs no live team read either.
int LuaSnapshotServe::GetUnitGroup(lua_State* L, const char* caller)
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

	if (rows.Team(unitID) != gu->myTeam)
		return 0;

	const CGroup* group = uiGroupHandlers[gu->myTeam].GetUnitGroup(unitID);

	if (group == nullptr)
		return 0;

	lua_pushnumber(L, group->id);
	return 1;
}

// mirror of LuaUnsyncedRead::IsUnitInView (the camera test is draw-owned;
// the ParseUnit gate and the midPos/radius payload were live sim reads)
int LuaSnapshotServe::IsUnitInView(lua_State* L, const char* caller)
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

	lua_pushboolean(L, camera->InView(rows.midPos[unitID], rows.radius[unitID]));
	return 1;
}

// mirror of LuaUnsyncedRead::GetUnitTransformMatrix; the live payload is
// CUnitDrawer::GetUnsyncedTransformMatrix: drawPos is drawer-owned (id-keyed),
// but the error offset and the ComposeMatrix basis (frontdir/updir/rightdir)
// are sim-written every frame -- mirrored from the rows
int LuaSnapshotServe::GetUnitTransformMatrix(lua_State* L, const char* caller)
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

	// CUnitDrawerData::GetUnsyncedTransformMatrix mirror (the live call site
	// passes fullread = false; gu->* are draw-owned unsynced globals and the
	// live formula reads gu->myAllyTeam, NOT the handle POV)
	float3 interPos = CUnitDrawer::GetDrawPos(unitID);

	if (!gu->spectatingFullView)
		interPos += rows.ErrorVector(unitID, gu->myAllyTeam);

	// CSolidObject::ComposeMatrix mirror: CMatrix44f(pos, -rightdir, updir, frontdir)
	CMatrix44f m(interPos, -rows.rightdir[unitID], rows.updir[unitID], rows.frontdir[unitID]);

	if (luaL_optboolean(L, 2, false))
		m = m.InvertAffine();

	for (int i = 0; i < 16; i += 4) {
		lua_pushnumber(L, m[i + 0]);
		lua_pushnumber(L, m[i + 1]);
		lua_pushnumber(L, m[i + 2]);
		lua_pushnumber(L, m[i + 3]);
	}

	return 16;
}

// mirror of LuaUnsyncedRead::GetUnitSelectionVolumeData; only the ParseUnit
// gate was a live losStatus read. The volume itself is unsynced-owned after
// creation (LuaUnsyncedCtrl::SetUnitSelectionVolumeData is the only
// post-PreInit writer), so it stays a live read -- through the sanctioned
// draw-side resolver, which also keeps same-frame set-then-read fresh (the
// boundary selVol row copy would lag a widget's own mutation)
int LuaSnapshotServe::GetUnitSelectionVolumeData(lua_State* L, const char* caller)
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

	// boundary-consistent pointer resolution (drawer cache + died-in-burst
	// shell); a miss is the same "no such unit" nil shape, never a
	// unitHandler fallback
	const CUnit* unit = LuaUtils::IdToObject<CUnit>(unitID, caller);

	if (unit == nullptr)
		return 0;

	return LuaUtils::PushColVolData(L, &unit->selectionVolume);
}

// mirror of LuaSyncedRead::GetUnitRotation. GetSolidObjectRotation branches
// on GetHandleSynced, but ShouldServe rejects synced handles, so only the
// unsynced branch (CUnitDrawer::GetUnsyncedTransformMatrix) is reachable
// here -- same drawPos + rows-basis composition as the GetUnitTransformMatrix
// twin, angles extracted the same way as live
int LuaSnapshotServe::GetUnitRotation(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// CUnitDrawerData::GetUnsyncedTransformMatrix mirror (fullread = false at
	// the live call site; gu->* are draw-owned, the live formula reads
	// gu->myAllyTeam, not the handle POV) + CSolidObject::ComposeMatrix mirror
	float3 interPos = CUnitDrawer::GetDrawPos(unitID);

	if (!gu->spectatingFullView)
		interPos += rows.ErrorVector(unitID, gu->myAllyTeam);

	const CMatrix44f matrix(interPos, -rows.rightdir[unitID], rows.updir[unitID], rows.frontdir[unitID]);
	const float3 angles = matrix.GetEulerAnglesLftHand();

	assert(matrix.IsOrthoNormal());

	lua_pushnumber(L, angles[CMatrix44f::ANGLE_P]);
	lua_pushnumber(L, angles[CMatrix44f::ANGLE_Y]);
	lua_pushnumber(L, angles[CMatrix44f::ANGLE_R]);
	return 3;
}

// mirror of LuaUnsyncedRead::GetFeatureLuaDraw (GetSolidObjectLuaDraw with the
// unsynced ParseFeature gate; luaDraw is unsynced-owned -- FeatureRendering.
// SetFeatureLuaDraw only exists in LuaRules' unsynced env -- so the payload
// stays a live read through the draw-side resolver)
int LuaSnapshotServe::GetFeatureLuaDraw(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror (fullRead bypass, then the
	// readAllyTeam<0 deny, then feature LOS -- same observable order)
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	lua_pushboolean(L, feature->luaDraw);
	return 1;
}

// mirror of LuaUnsyncedRead::GetFeatureNoDraw (GetSolidObjectNoDraw with the
// unsynced ParseFeature gate; noDraw is unsynced-owned -- LuaUnsyncedCtrl::
// SetFeatureNoDraw is the only post-creation writer)
int LuaSnapshotServe::GetFeatureNoDraw(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	lua_pushboolean(L, feature->noDraw);
	return 1;
}

// mirror of LuaUnsyncedRead::GetFeatureEngineDrawMask (GetSolidObjectEngineDrawMask
// with the unsynced ParseFeature gate; engineDrawMask is unsynced-owned --
// LuaUnsyncedCtrl::SetFeatureEngineDrawMask is the only post-creation writer)
int LuaSnapshotServe::GetFeatureEngineDrawMask(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	lua_pushinteger(L, feature->engineDrawMask);
	return 1;
}

// mirror of LuaUnsyncedRead::GetFeatureAlwaysUpdateMatrix (alwaysUpdateMat is
// unsynced-owned -- LuaUnsyncedCtrl::SetFeatureAlwaysUpdateMatrix is the only
// post-creation writer)
int LuaSnapshotServe::GetFeatureAlwaysUpdateMatrix(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	lua_pushboolean(L, feature->alwaysUpdateMat);
	return 1;
}

// mirror of LuaUnsyncedRead::GetFeatureDrawFlag; folds the dogfood-round
// drawer-cache/shell fix (commits 6910e82315 / f5e064c252): the payload is
// DRAWER-owned (CFeatureDrawer::GetDrawFlag), the pointer comes from the
// drawer's boundary cache + died-in-burst shell, and the visibility gate --
// the fix's one remaining live sim read (LuaUtils::IsFeatureVisible) -- now
// answers from the snapshot Pov mirror per the 27b plan of record
int LuaSnapshotServe::GetFeatureDrawFlag(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	lua_pushinteger(L, CFeatureDrawer::GetDrawFlag(feature));
	return 1;
}

// mirror of LuaUnsyncedRead::GetFeatureSelectionVolumeData
// (GetSolidObjectSelectionVolume with the unsynced ParseFeature gate; the
// selection volume is unsynced-owned -- ctor-init happens-before the boundary
// publish, LuaUnsyncedCtrl::SetFeatureSelectionVolumeData is the only later
// writer -- so the payload stays a live read through the draw-side resolver,
// which also keeps same-draw-frame set-then-get read-your-write consistent
// where the boundary selVol row copy would be one frame stale)
int LuaSnapshotServe::GetFeatureSelectionVolumeData(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CFeature* feature = ResolveDrawFeature(featureID);

	if (feature == nullptr)
		return 0;

	return LuaUtils::PushColVolData(L, &feature->selectionVolume);
}

// mirror of LuaUnsyncedRead::GetFeatureTransformMatrix (GetObjectTransformMatrix
// over the drawer-owned unsynced transform -- id-keyed accessor, identity on a
// stale id, exactly the live pointer path's f->id lookup; only the ParseFeature
// gate was a sim read)
int LuaSnapshotServe::GetFeatureTransformMatrix(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDUnsynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// LuaUnsyncedRead::ParseFeature gate mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	CMatrix44f m = CFeatureDrawer::GetUnsyncedTransformMatrix(featureID);

	// NOTE: read before any pushing (same order as the live helper)
	if (luaL_optboolean(L, 2, false))
		m = m.InvertAffine();

	for (int i = 0; i < 16; i += 4) {
		lua_pushnumber(L, m[i + 0]);
		lua_pushnumber(L, m[i + 1]);
		lua_pushnumber(L, m[i + 2]);
		lua_pushnumber(L, m[i + 3]);
	}

	return 16;
}

// mirror of LuaSyncedRead::GetFeatureRotation. GetSolidObjectRotation branches
// on GetHandleSynced, and ShouldServe() excludes synced handles, so the served
// leg is always the drawer-owned unsynced transform (draw-safe since PR 3);
// only the ParseFeature/IsFeatureVisible gate was a live sim read
int LuaSnapshotServe::GetFeatureRotation(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror (the live body's IsFeatureVisible re-check is the
	// same predicate ParseFeature already applied)
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	const CMatrix44f matrix = CFeatureDrawer::GetUnsyncedTransformMatrix(featureID);
	const float3 angles = matrix.GetEulerAnglesLftHand();

	assert(matrix.IsOrthoNormal());

	lua_pushnumber(L, angles[CMatrix44f::ANGLE_P]);
	lua_pushnumber(L, angles[CMatrix44f::ANGLE_Y]);
	lua_pushnumber(L, angles[CMatrix44f::ANGLE_R]);
	return 3;
}


/******************************************************************************
 * Command-queue family (PR 27b serving batch 2, first family).
 *
 * Unlike the row-backed families above, queue contents cannot be read lazily
 * at callout time: the twins run post-release while the sim thread mutates
 * the deques. RefreshCommandQueues() below runs AT THE BARRIER (sim parked,
 * right after the snapshot publish) and re-copies only the queues whose
 * CCommandQueue version changed since the last boundary (versions are bumped
 * at every queue mutation, structural or in-place, and are globally unique
 * across queue instances -- see CommandQueue.h).
 *
 * Copies are flattened into SnapCommand + a flat float param buffer instead
 * of std::vector<Command>: copying/destroying a raw Command with more than
 * MAX_COMMAND_PARAMS params acquires/releases pages of the sim-owned global
 * cmdParamsPool, and GetParam on such a copy reads pool storage the sim
 * thread resizes (AcquirePage grows the page table) -- both races under the
 * running split. The flatten happens element-wise under the parked sim, the
 * same per-element copy the WaitCommandsAI deferral does.
 ******************************************************************************/

namespace {
	// the Command fields the family's twins serve (params live in the slot's
	// flat buffer at [paramOffset, paramOffset + numParams))
	struct SnapCommand {
		int id;
		unsigned int tag;
		unsigned char options;
		unsigned int paramOffset;
		unsigned int numParams;
	};

	// sim|draw PR 30: the SCommandDescription fields the cmd-desc twins serve
	// (LuaUtils::PushCommandDesc surface, 12 keys + params); refCount excluded
	// (unsynced cache bookkeeping, not part of the Lua shape)
	struct CmdDescRecord {
		int id;
		int type;
		unsigned char queueing;
		unsigned char hidden;
		unsigned char disabled;
		unsigned char showUnique;
		unsigned char onlyTexture;
		std::string name;
		std::string action;
		std::string iconname;
		std::string mouseicon;
		std::string tooltip;
		std::vector<std::string> params;
	};

	struct UnitCmdQueueSlot {
		// 0 = never copied (real versions are a pre-incremented global counter)
		uint64_t cmdQueVersion = 0;
		uint64_t newUnitCmdsVersion = 0;

		bool present = false;
		bool isFactoryCAI = false;  // CFactoryCAI: unit callouts serve newUnitCommands instead
		bool isFactoryUnit = false; // CFactory: GetFactoryBuggerOff's dynamic_cast gate

		// GetFactoryBuggerOff payload (CFactory bo*): no version to key on, but
		// factories are few -- re-copied unconditionally every refresh
		bool boPerform = false;
		bool boSherical = false;
		bool boForced = false;
		float boOffset = 0.0f;
		float boRadius = 0.0f;
		int boRelHeading = 0;

		std::vector<SnapCommand> commandQue;
		std::vector<float> commandQueParams;
		std::vector<SnapCommand> newUnitCommands;
		std::vector<float> newUnitCommandsParams;

		// sim|draw PR 30: cmd-desc surface (possibleCommands), version-keyed on
		// CCommandAI::GetCmdDescVersion() -- a separate version so a queue pop
		// does not force a desc recopy (descs change far less often)
		uint64_t cmdDescVersion = 0;
		std::vector<CmdDescRecord> descs;

		// sim|draw PR 30: GetUnitWorkerTask's resolved (numRet, cmd, target)
		// answer, decoded at extraction (ResolveWorkerTask) since curBuild/
		// curCapture/curResurrect/curReclaim/terraforming are not in the queue
		unsigned char workerTaskNumRet = 0;
		int workerTaskCmd = 0;
		int workerTaskTarget = 0;
	};

	// indexed by unitID, sized unitHandler.MaxUnits() at first refresh
	std::vector<UnitCmdQueueSlot> cmdQueueCache;
	uint32_t cmdQueueCacheGeneration = 0;

	void ClearCmdQueueSlot(UnitCmdQueueSlot& slot)
	{
		slot.cmdQueVersion = 0;
		slot.newUnitCmdsVersion = 0;
		slot.present = false;
		slot.isFactoryCAI = false;
		slot.isFactoryUnit = false;
		slot.commandQue.clear();
		slot.commandQueParams.clear();
		slot.newUnitCommands.clear();
		slot.newUnitCommandsParams.clear();
		slot.cmdDescVersion = 0;
		slot.descs.clear();
		slot.workerTaskNumRet = 0;
		slot.workerTaskCmd = 0;
		slot.workerTaskTarget = 0;
	}

	void CopyQueueSnap(const CCommandQueue& q, std::vector<SnapCommand>& cmds, std::vector<float>& params)
	{
		cmds.clear();
		params.clear();
		cmds.reserve(q.size());

		for (const Command& c: q) {
			const unsigned int numParams = c.GetNumParams();

			cmds.push_back({c.GetID(), c.GetTag(), c.GetOpts(), static_cast<unsigned int>(params.size()), numParams});

			for (unsigned int i = 0; i < numParams; ++i)
				params.push_back(c.GetParam(i));
		}

		// sim|draw PR 30: dirty-versioned copy-cost telemetry (decision-4 "one
		// boundary copy tracks the order rate")
		BoundaryStats::Add(BoundaryStats::ctr.cmdBlocksCopied);
		BoundaryStats::Add(BoundaryStats::ctr.cmdBlockBytes,
			cmds.size() * sizeof(SnapCommand) + params.size() * sizeof(float));
	}

	// sim|draw PR 30: flatten possibleCommands into the slot's desc records
	// (mirror of BuildDescBlock; the fields LuaUtils::PushCommandDesc serves)
	void CopyDescsSnap(const std::vector<const SCommandDescription*>& live, std::vector<CmdDescRecord>& out)
	{
		out.clear();
		out.reserve(live.size());

		uint64_t bytes = 0;
		for (const SCommandDescription* cd: live) {
			CmdDescRecord r;
			r.id          = cd->id;
			r.type        = cd->type;
			r.queueing    = cd->queueing;
			r.hidden      = cd->hidden;
			r.disabled    = cd->disabled;
			r.showUnique  = cd->showUnique;
			r.onlyTexture = cd->onlyTexture;
			r.name        = cd->name;
			r.action      = cd->action;
			r.iconname    = cd->iconname;
			r.mouseicon   = cd->mouseicon;
			r.tooltip     = cd->tooltip;
			r.params      = cd->params;

			bytes += sizeof(CmdDescRecord) + r.name.size() + r.action.size() +
				r.iconname.size() + r.mouseicon.size() + r.tooltip.size();
			for (const std::string& p: r.params)
				bytes += p.size();

			out.push_back(std::move(r));
		}

		BoundaryStats::Add(BoundaryStats::ctr.cmdDescBlocksCopied);
		BoundaryStats::Add(BoundaryStats::ctr.cmdDescBlockBytes, bytes);
	}

	// sim|draw PR 30: mirror of GetBuilderWorkerTask / GetFactoryWorkerTask,
	// decoded at extraction into the slot (the fields it reads -- curBuild/
	// curCapture/curResurrect/curReclaim/terraforming -- are not derivable from
	// the queue copy, so the resolved answer is stored instead of deep rows)
	void ResolveWorkerTask(const CUnit* unit, UnitCmdQueueSlot& slot)
	{
		slot.workerTaskNumRet = 0;
		slot.workerTaskCmd = 0;
		slot.workerTaskTarget = 0;

		if (const CBuilder* builder = dynamic_cast<const CBuilder*>(unit)) {
			if (builder->curBuild) {
				slot.workerTaskCmd = builder->curBuild->beingBuilt ? -builder->curBuild->unitDef->id : CMD_REPAIR;
				slot.workerTaskTarget = builder->curBuild->id;
				slot.workerTaskNumRet = 2;
			} else if (builder->curCapture) {
				slot.workerTaskCmd = CMD_CAPTURE;
				slot.workerTaskTarget = builder->curCapture->id;
				slot.workerTaskNumRet = 2;
			} else if (builder->curResurrect) {
				slot.workerTaskCmd = CMD_RESURRECT;
				slot.workerTaskTarget = builder->curResurrect->id + unitHandler.MaxUnits();
				slot.workerTaskNumRet = 2;
			} else if (builder->curReclaim) {
				slot.workerTaskCmd = CMD_RECLAIM;
				if (builder->reclaimingUnit) {
					const CUnit* reclaimee = dynamic_cast<const CUnit*>(builder->curReclaim);
					slot.workerTaskTarget = (reclaimee != nullptr) ? reclaimee->id : 0;
				} else {
					const CFeature* reclaimee = dynamic_cast<const CFeature*>(builder->curReclaim);
					slot.workerTaskTarget = (reclaimee != nullptr) ? (reclaimee->id + unitHandler.MaxUnits()) : 0;
				}
				slot.workerTaskNumRet = 2;
			} else if (builder->helpTerraform || builder->terraforming) {
				slot.workerTaskCmd = CMD_RESTORE;
				slot.workerTaskNumRet = 1;
			}
			return;
		}

		if (const CFactory* factory = dynamic_cast<const CFactory*>(unit)) {
			if (factory->curBuild) {
				slot.workerTaskCmd = factory->curBuild->beingBuilt ? -factory->curBuild->unitDef->id : CMD_REPAIR;
				slot.workerTaskTarget = factory->curBuild->id;
				slot.workerTaskNumRet = 2;
			}
			return;
		}
	}

	// sim|draw PR 30: LuaUtils::PushCommandDesc mirror over a CmdDescRecord
	// (same 12 keys, same order, same params sub-table)
	void PushCommandDescSnap(lua_State* L, const CmdDescRecord& cd)
	{
		const int numParams = cd.params.size();
		const int numTblKeys = 12;

		lua_checkstack(L, 1 + 1 + 1 + 1);
		lua_createtable(L, 0, numTblKeys);

		HSTR_PUSH_NUMBER(L, "id",          cd.id);
		HSTR_PUSH_NUMBER(L, "type",        cd.type);
		HSTR_PUSH_STRING(L, "name",        cd.name);
		HSTR_PUSH_STRING(L, "action",      cd.action);
		HSTR_PUSH_STRING(L, "tooltip",     cd.tooltip);
		HSTR_PUSH_STRING(L, "texture",     cd.iconname);
		HSTR_PUSH_STRING(L, "cursor",      cd.mouseicon);
		HSTR_PUSH_BOOL(L,   "queueing",    cd.queueing);
		HSTR_PUSH_BOOL(L,   "hidden",      cd.hidden);
		HSTR_PUSH_BOOL(L,   "disabled",    cd.disabled);
		HSTR_PUSH_BOOL(L,   "showUnique",  cd.showUnique);
		HSTR_PUSH_BOOL(L,   "onlyTexture", cd.onlyTexture);

		HSTR_PUSH(L, "params");
		lua_createtable(L, 0, numParams);
		for (int p = 0; p < numParams; p++) {
			lua_pushsstring(L, cd.params[p]);
			lua_rawseti(L, -2, p + 1);
		}
		lua_settable(L, -3);
	}

	const UnitCmdQueueSlot* GetCmdQueueSlot(int unitID)
	{
		if (unitID < 0 || static_cast<size_t>(unitID) >= cmdQueueCache.size())
			return nullptr;

		const UnitCmdQueueSlot& slot = cmdQueueCache[unitID];

		return slot.present ? &slot : nullptr;
	}

	// the family's shared gate: ParseAllyUnit mirror (rows.Valid + PovAlliedUnit,
	// like GetUnitExperience) followed by the boundary-copy lookup; nullptr IS
	// the live path's "no such unit" nil shape. A rows-valid id without a slot
	// cannot happen (refresh and publish share the barrier) -- nil defensively.
	const UnitCmdQueueSlot* ParseAllyUnitCmdSlot(lua_State* L, const char* caller, int* outUnitID = nullptr)
	{
		const auto& rows = simSnapshot.Read();
		const int unitID = ParseUnitIDSynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		if (outUnitID != nullptr)
			*outUnitID = unitID;

		if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
			return nullptr;

		return GetCmdQueueSlot(unitID);
	}

	// LuaUtils::PushCommandParamsTable mirror over the flat param buffer
	void PushSnapCommandParamsTable(lua_State* L, const SnapCommand& cmd, const std::vector<float>& params, bool subtable)
	{
		if (subtable)
			HSTR_PUSH(L, "params");

		lua_createtable(L, cmd.numParams, 0);

		for (unsigned int p = 0; p < cmd.numParams; p++) {
			lua_pushnumber(L, params[cmd.paramOffset + p]);
			lua_rawseti(L, -2, p + 1);
		}

		if (subtable)
			lua_rawset(L, -3);
	}

	// LuaUtils::PushCommandOptionsTable mirror
	void PushSnapCommandOptionsTable(lua_State* L, const SnapCommand& cmd, bool subtable)
	{
		if (subtable)
			HSTR_PUSH(L, "options");

		lua_createtable(L, 0, 7);
		HSTR_PUSH_NUMBER(L, "coded", cmd.options);
		HSTR_PUSH_BOOL(L, "alt",      !!(cmd.options & ALT_KEY        ));
		HSTR_PUSH_BOOL(L, "ctrl",     !!(cmd.options & CONTROL_KEY    ));
		HSTR_PUSH_BOOL(L, "shift",    !!(cmd.options & SHIFT_KEY      ));
		HSTR_PUSH_BOOL(L, "right",    !!(cmd.options & RIGHT_MOUSE_KEY));
		HSTR_PUSH_BOOL(L, "meta",     !!(cmd.options & META_KEY       ));
		HSTR_PUSH_BOOL(L, "internal", !!(cmd.options & INTERNAL_ORDER ));

		if (subtable)
			lua_rawset(L, -3);
	}

	// LuaSyncedRead's PackCommand mirror
	void PackCommandSnap(lua_State* L, const SnapCommand& cmd, const std::vector<float>& params)
	{
		lua_createtable(L, 0, 4);

		HSTR_PUSH_NUMBER(L, "id", cmd.id);

		PushSnapCommandParamsTable(L, cmd, params, true);
		PushSnapCommandOptionsTable(L, cmd, true);

		HSTR_PUSH_NUMBER(L, "tag", cmd.tag);
	}

	// LuaSyncedRead's PackCommandQueue mirror (identical types: the callers'
	// int numCmds converts to size_t exactly like the live call, so the inert
	// `count == -1u` branch and the min() clamp behave bit-identically)
	void PackCommandQueueSnap(lua_State* L, const std::vector<SnapCommand>& commands, const std::vector<float>& params, size_t count)
	{
		size_t c = 0;

		if (count == -1u)
			count = commands.size();

		lua_createtable(L, std::min(count, commands.size()), 0);

		for (const SnapCommand& command: commands) {
			if (c >= count)
				break;

			PackCommandSnap(L, command, params);
			lua_rawseti(L, -2, ++c);
		}
	}

	// LuaSyncedRead's PackFactoryCounts mirror (only reads command ids)
	void PackFactoryCountsSnap(lua_State* L, const std::vector<SnapCommand>& q, int count, bool noCmds)
	{
		lua_createtable(L, count + 1, 0);

		int entry = 0;
		int currentCmd = 0;
		int currentCount = 0;

		for (const SnapCommand& sc: q) {
			if (entry >= count) {
				currentCount = 0;
				break;
			}
			const int cmdID = sc.id;
			if (noCmds && (cmdID >= 0))
				continue;

			if (entry == 0) {
				currentCmd = cmdID;
				currentCount = 1;
				entry = 1;
			}
			else if (cmdID == currentCmd) {
				currentCount++;
			}
			else {
				entry++;
				// negative integer keys live in the hash part, hence nrec=1
				// (same note as the live body)
				lua_createtable(L, 0, 1); {
					lua_pushnumber(L, currentCount);
					lua_rawseti(L, -2, -currentCmd);
				}
				lua_rawseti(L, -2, entry);
				currentCmd = cmdID;
				currentCount = 1;
			}
		}
		if (currentCount > 0) {
			entry++;
			lua_createtable(L, 0, 1); {
				lua_pushnumber(L, currentCount);
				lua_rawseti(L, -2, -currentCmd);
			}
			lua_rawseti(L, -2, entry);
		}

		HSTR_PUSH_NUMBER(L, "n", entry);
	}

	// LuaSyncedRead's PackBuildQueue mirror; builderDef comes from the snapshot
	// defID row (the true def -- the allied gate passed, so no decoy applies),
	// buildee/builder def derefs are immutable game data
	int PackBuildQueueSnap(lua_State* L, bool canBuild, const char* caller)
	{
		int unitID = -1;
		const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller, &unitID);

		if (slot == nullptr)
			return 0;

		const auto& commandQue = slot->commandQue;

		lua_createtable(L, commandQue.size(), 0);

		int entry = 0;
		int currentType = -1;
		int currentCount = 0;

		for (const SnapCommand& cmd: commandQue) {
			// not a build command
			if (cmd.id >= 0)
				continue;

			const int unitDefID = -cmd.id;

			if (canBuild) {
				// skip build orders that this unit can not start
				const UnitDef* buildeeDef = unitDefHandler->GetUnitDefByID(unitDefID);
				const UnitDef* builderDef = unitDefHandler->GetUnitDefByID(simSnapshot.Read().defID[unitID]);

				// if something is wrong, bail
				if ((buildeeDef == nullptr) || (builderDef == nullptr))
					continue;

				using P = decltype(UnitDef::buildOptions)::value_type;

				const auto& buildOptCmp = [&](const P& e) { return (STRCASECMP(e.second.c_str(), buildeeDef->name.c_str()) == 0); };
				const auto& buildOpts = builderDef->buildOptions;
				const auto  buildOptIt = std::find_if(buildOpts.cbegin(), buildOpts.cend(), buildOptCmp);

				// didn't find a matching entry
				if (buildOptIt == buildOpts.end())
					continue;
			}

			if (currentType == unitDefID) {
				currentCount++;
			} else if (currentType == -1) {
				currentType = unitDefID;
				currentCount = 1;
			} else {
				entry++;
				lua_newtable(L);
				lua_pushnumber(L, currentCount);
				lua_rawseti(L, -2, currentType);
				lua_rawseti(L, -2, entry);
				currentType = unitDefID;
				currentCount = 1;
			}
		}

		if (currentCount > 0) {
			entry++;
			lua_newtable(L);
			lua_pushnumber(L, currentCount);
			lua_rawseti(L, -2, currentType);
			lua_rawseti(L, -2, entry);
		}

		lua_pushnumber(L, entry);

		return 2;
	}
}


void LuaSnapshotServe::RefreshCommandQueues()
{
	// barrier-only (sim parked or single-threaded): walks unitHandler and
	// reads live queues. Generation-gated so the copies always describe the
	// same boundary as the published rows -- and so the walk is free when the
	// publish above didn't swap (no sim frame, no boundary mutation).
	const uint32_t gen = simSnapshot.Generation();

	if (gen == 0 || gen == cmdQueueCacheGeneration)
		return;

	cmdQueueCacheGeneration = gen;

	const size_t maxUnits = unitHandler.MaxUnits();

	if (cmdQueueCache.size() != maxUnits)
		cmdQueueCache.resize(maxUnits);

	for (size_t id = 0; id < maxUnits; ++id) {
		UnitCmdQueueSlot& slot = cmdQueueCache[id];
		const CUnit* unit = unitHandler.GetUnit(id);

		// dead ids must serve the "no such unit" nil shape, never stale copies
		if (unit == nullptr) {
			if (slot.present)
				ClearCmdQueueSlot(slot);
			continue;
		}

		const CCommandAI* cai = unit->commandAI; // never null
		const uint64_t cmdQueVersion = cai->commandQue.GetVersion();

		if (!slot.present || slot.cmdQueVersion != cmdQueVersion) {
			// (re)classify here too: a died-and-respawned id always lands in
			// this branch (queue versions are globally unique), so the flags
			// can never go stale across id reuse
			slot.present = true;
			slot.isFactoryCAI = (dynamic_cast<const CFactoryCAI*>(cai) != nullptr);
			slot.isFactoryUnit = (dynamic_cast<const CFactory*>(unit) != nullptr);

			CopyQueueSnap(cai->commandQue, slot.commandQue, slot.commandQueParams);
			slot.cmdQueVersion = cmdQueVersion;

			if (!slot.isFactoryCAI && !slot.newUnitCommands.empty()) {
				slot.newUnitCommands.clear();
				slot.newUnitCommandsParams.clear();
				slot.newUnitCmdsVersion = 0;
			}
		}

		// sim|draw PR 30: cmd-desc surface, keyed on its OWN version (a queue pop
		// does not recopy descs). A fresh/respawned slot has cmdDescVersion 0 and
		// GetCmdDescVersion() is a globally-unique >=1 value, so it rebuilds; a
		// reused id can never alias a prior owner's descs (see GetCmdDescVersion).
		const uint64_t descVersion = cai->GetCmdDescVersion();
		if (slot.cmdDescVersion != descVersion) {
			CopyDescsSnap(cai->GetPossibleCommands(), slot.descs);
			slot.cmdDescVersion = descVersion;
		}

		if (slot.isFactoryCAI) {
			const CFactoryCAI* fcai = static_cast<const CFactoryCAI*>(cai);
			const uint64_t newUnitCmdsVersion = fcai->newUnitCommands.GetVersion();

			if (slot.newUnitCmdsVersion != newUnitCmdsVersion) {
				CopyQueueSnap(fcai->newUnitCommands, slot.newUnitCommands, slot.newUnitCommandsParams);
				slot.newUnitCmdsVersion = newUnitCmdsVersion;
			}
		}

		if (slot.isFactoryUnit) {
			const CFactory* fac = static_cast<const CFactory*>(unit);

			slot.boPerform    = fac->boPerform;
			slot.boOffset     = fac->boOffset;
			slot.boRadius     = fac->boRadius;
			slot.boRelHeading = fac->boRelHeading;
			slot.boSherical   = fac->boSherical;
			slot.boForced     = fac->boForced;
		}

		// sim|draw PR 30: decode GetUnitWorkerTask's answer here (not queue-
		// derivable); re-resolved every refresh (few builders/factories)
		ResolveWorkerTask(unit, slot);
	}
}


// mirror of LuaSyncedRead::GetUnitCommands (also reached via GetCommandQueue,
// whose live body forwards here exactly like on master)
int LuaSnapshotServe::GetUnitCommands(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	// send the new unit commands for factories, otherwise the normal commands
	const auto& queue  = slot->isFactoryCAI ? slot->newUnitCommands : slot->commandQue;
	const auto& params = slot->isFactoryCAI ? slot->newUnitCommandsParams : slot->commandQueParams;

	const int  numCmds   = luaL_checkint(L, 2); // must always be given, -1 is a performance pitfall
	const bool cmdsTable = luaL_optboolean(L, 3, true); // deprecated, prefer to set 2nd arg to 0

	if (cmdsTable && (numCmds != 0)) {
		// *get wants the actual commands
		PackCommandQueueSnap(L, queue, params, numCmds);
	} else {
		LOG_DEPRECATED("This game is issuing `Spring.GetUnitCommands(unitId, 0)`, `Spring.GetCommandQueue(unitId, 0)` or passing a third argument to these functions. This usage is deprecated, please use `Spring.GetUnitCommandCount(unitId)` instead or fix some underlying bug.");
		// *get just wants the queue's size
		lua_pushnumber(L, queue.size());
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitCommandCount
int LuaSnapshotServe::GetUnitCommandCount(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	const auto& queue = slot->isFactoryCAI ? slot->newUnitCommands : slot->commandQue;

	lua_pushnumber(L, queue.size());

	return 1;
}

// mirror of LuaSyncedRead::GetUnitCurrentCommand (1-based cmdIndex, negative
// counts from the queue end, beyond-queue serves nil -- identical arithmetic,
// including the int/size_t mixed compares)
int LuaSnapshotServe::GetUnitCurrentCommand(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	const auto& queue  = slot->isFactoryCAI ? slot->newUnitCommands : slot->commandQue;
	const auto& params = slot->isFactoryCAI ? slot->newUnitCommandsParams : slot->commandQueParams;

	int cmdIndex = luaL_optint(L, 2, 1);
	if (cmdIndex > 0) {
		// - 1 to convert from lua index to C index
		cmdIndex -= 1;
	} else {
		cmdIndex = queue.size() + cmdIndex;
	}

	if (cmdIndex >= queue.size() || cmdIndex < 0)
		return 0;

	const SnapCommand& cmd = queue[cmdIndex];
	lua_pushnumber(L, cmd.id);
	lua_pushnumber(L, cmd.options);
	lua_pushnumber(L, cmd.tag);

	const unsigned int numParams = cmd.numParams;
	for (unsigned int i = 0; i < numParams; ++i)
		lua_pushnumber(L, params[cmd.paramOffset + i]);

	return 3 + numParams;
}

// mirror of LuaSyncedRead::GetFactoryCommands (the factory's own commandQue,
// not newUnitCommands)
int LuaSnapshotServe::GetFactoryCommands(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	// bail if not a factory
	if (!slot->isFactoryCAI)
		return 0;

	const int  numCmds   = luaL_checkint(L, 2);
	const bool cmdsTable = luaL_optboolean(L, 3, true); // deprecated, prefer to set 2nd arg to 0

	if (cmdsTable && (numCmds != 0)) {
		PackCommandQueueSnap(L, slot->commandQue, slot->commandQueParams, numCmds);
	} else {
		LOG_DEPRECATED("This game is issuing `Spring.GetFactoryCommands(unitId, 0)`, or passing a third argument. This usage is deprecated, please use `Spring.GetFactoryCommandCount(unitId)` instead or fix some underlying bug.");
		lua_pushnumber(L, slot->commandQue.size());
	}

	return 1;
}

// mirror of LuaSyncedRead::GetFactoryCommandCount
int LuaSnapshotServe::GetFactoryCommandCount(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	// bail if not a factory
	if (!slot->isFactoryCAI)
		return 0;

	lua_pushnumber(L, slot->commandQue.size());

	return 1;
}

// mirror of LuaSyncedRead::GetFactoryCounts
int LuaSnapshotServe::GetFactoryCounts(lua_State* L, const char* caller)
{
	const UnitCmdQueueSlot* slot = ParseAllyUnitCmdSlot(L, caller);

	if (slot == nullptr)
		return 0;

	if (!slot->isFactoryCAI)
		return 0; // not a factory, bail

	// get the desired number of commands to return
	int count = luaL_optint(L, 2, -1);
	if (count < 0)
		count = (int)slot->commandQue.size();

	const bool noCmds = !luaL_optboolean(L, 3, false);

	PackFactoryCountsSnap(L, slot->commandQue, count, noCmds);

	return 1;
}

// mirror of LuaSyncedRead::GetFactoryBuggerOff (ParseUnit gate, then the
// CFactory dynamic_cast gate; the payload scalars are re-copied every refresh)
int LuaSnapshotServe::GetFactoryBuggerOff(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const UnitCmdQueueSlot* slot = GetCmdQueueSlot(unitID);

	if (slot == nullptr || !slot->isFactoryUnit)
		return 0;

	lua_pushboolean(L, slot->boPerform    );
	lua_pushnumber (L, slot->boOffset     );
	lua_pushnumber (L, slot->boRadius     );
	lua_pushnumber (L, slot->boRelHeading );
	lua_pushboolean(L, slot->boSherical   );
	lua_pushboolean(L, slot->boForced     );

	return 6;
}

// mirror of LuaSyncedRead::GetFullBuildQueue
int LuaSnapshotServe::GetFullBuildQueue(lua_State* L, const char* caller)
{
	return PackBuildQueueSnap(L, false, caller);
}

// mirror of LuaSyncedRead::GetRealBuildQueue
int LuaSnapshotServe::GetRealBuildQueue(lua_State* L, const char* caller)
{
	return PackBuildQueueSnap(L, true, caller);
}

// mirror of LuaSyncedRead::GetUnitCmdDescs (ParseTypedUnit gate, 1-based slicing)
int LuaSnapshotServe::GetUnitCmdDescs(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const UnitCmdQueueSlot* slot = GetCmdQueueSlot(unitID);
	if (slot == nullptr)
		return 0;

	const std::vector<CmdDescRecord>& cmdDescs = slot->descs;
	const int lastDesc = (int)cmdDescs.size() - 1;

	const int args = lua_gettop(L); // number of arguments
	int startIndex = 0;
	int endIndex = lastDesc;
	if ((args >= 2) && lua_isnumber(L, 2)) {
		startIndex = lua_toint(L, 2) - 1;
		if ((args >= 3) && lua_isnumber(L, 3)) {
			endIndex = lua_toint(L, 3) - 1;
		} else {
			endIndex = startIndex;
		}
	}
	startIndex = std::clamp(startIndex, 0, lastDesc);
	endIndex   = std::clamp(endIndex  , 0, lastDesc);

	lua_createtable(L, endIndex - startIndex, 0);
	int count = 1;
	for (int i = startIndex; i <= endIndex; i++) {
		PushCommandDescSnap(L, cmdDescs[i]);
		lua_rawseti(L, -2, count++);
	}

	return 1;
}

// mirror of LuaSyncedRead::FindUnitCmdDesc (ParseTypedUnit gate)
int LuaSnapshotServe::FindUnitCmdDesc(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const UnitCmdQueueSlot* slot = GetCmdQueueSlot(unitID);
	if (slot == nullptr)
		return 0;

	const int cmdID = luaL_checkint(L, 2);

	const std::vector<CmdDescRecord>& cmdDescs = slot->descs;
	for (int i = 0; i < (int)cmdDescs.size(); i++) {
		if (cmdDescs[i].id == cmdID) {
			lua_pushnumber(L, i + 1);
			return 1;
		}
	}
	return 0;
}

// mirror of LuaSyncedRead::GetUnitWorkerTask (ParseInLosUnit gate; the resolved
// build/repair/reclaim/... answer was decoded at extraction, see ResolveWorkerTask)
int LuaSnapshotServe::GetUnitWorkerTask(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const UnitCmdQueueSlot* slot = GetCmdQueueSlot(unitID);
	if (slot == nullptr || slot->workerTaskNumRet == 0)
		return 0;

	lua_pushnumber(L, slot->workerTaskCmd);
	if (slot->workerTaskNumRet == 2)
		lua_pushnumber(L, slot->workerTaskTarget);

	return slot->workerTaskNumRet;
}

// sim|draw PR 30 mirror-verification hook for SnapshotDiffGate::CheckCmdQueueRows.
// Compares the cached slot for unitID against the live sim state at the armed
// boundary (the cache was just rebuilt by RefreshCommandQueues, so every field
// must trivially match -- a missed dirty-mark becomes a deterministic failure).
// Binds live queues through const refs so the mutable (version-bumping) queue
// accessors are never invoked. Lives here (not in SnapshotDiffGate) so the file-
// static cache stays encapsulated and the sim includes are already present.
LuaSnapshotServe::CmdQueueCompareResult LuaSnapshotServe::CompareCmdQueueSlot(int unitID, const CUnit* liveUnit)
{
	CmdQueueCompareResult res{};

	const UnitCmdQueueSlot* slot =
		(unitID >= 0 && static_cast<size_t>(unitID) < cmdQueueCache.size() && cmdQueueCache[unitID].present)
			? &cmdQueueCache[unitID] : nullptr;

	res.present = (slot != nullptr);
	if (slot == nullptr || liveUnit == nullptr)
		return res;

	// bitwise float compare (mirror of SnapshotDiffGate::BitEqual)
	const auto bitEq = [](float a, float b) {
		return std::memcmp(&a, &b, sizeof(float)) == 0;
	};
	const auto queueEqual = [&](const std::vector<SnapCommand>& cmds, const std::vector<float>& params, const CCommandQueue& live) -> bool {
		if (cmds.size() != live.size())
			return false;
		size_t i = 0;
		for (const Command& c: live) {
			const SnapCommand& r = cmds[i++];
			if (r.id != c.GetID() || r.tag != c.GetTag() || r.options != c.GetOpts() || r.numParams != c.GetNumParams())
				return false;
			for (unsigned int p = 0; p < r.numParams; ++p) {
				if (!bitEq(params[r.paramOffset + p], c.GetParam(p)))
					return false;
			}
		}
		return true;
	};

	const CCommandAI* cai = liveUnit->commandAI;
	const bool isFactoryCAI = (dynamic_cast<const CFactoryCAI*>(cai) != nullptr);

	// queue block(s): commandQue always, newUnitCommands for factory CAIs
	bool queueOk = queueEqual(slot->commandQue, slot->commandQueParams, cai->commandQue);
	if (isFactoryCAI) {
		const CFactoryCAI* fcai = static_cast<const CFactoryCAI*>(cai);
		queueOk = queueOk && queueEqual(slot->newUnitCommands, slot->newUnitCommandsParams, fcai->newUnitCommands);
	}
	res.queueOk = queueOk;

	// cmd-desc block vs live possibleCommands
	const std::vector<const SCommandDescription*>& liveDescs = cai->GetPossibleCommands();
	bool descsOk = (slot->descs.size() == liveDescs.size());
	for (size_t i = 0; descsOk && i < liveDescs.size(); ++i) {
		const CmdDescRecord& r = slot->descs[i];
		const SCommandDescription& d = *liveDescs[i];
		descsOk = (r.id == d.id && r.type == d.type &&
			bool(r.queueing) == d.queueing && bool(r.hidden) == d.hidden && bool(r.disabled) == d.disabled &&
			bool(r.showUnique) == d.showUnique && bool(r.onlyTexture) == d.onlyTexture &&
			r.name == d.name && r.action == d.action && r.iconname == d.iconname &&
			r.mouseicon == d.mouseicon && r.tooltip == d.tooltip && r.params == d.params);
	}
	res.descsOk = descsOk;

	// worker-task decode: re-resolve into a scratch slot and compare
	UnitCmdQueueSlot scratch;
	ResolveWorkerTask(liveUnit, scratch);
	res.workerOk = (slot->workerTaskNumRet == scratch.workerTaskNumRet) &&
		(scratch.workerTaskNumRet < 1 || slot->workerTaskCmd == scratch.workerTaskCmd) &&
		(scratch.workerTaskNumRet != 2 || slot->workerTaskTarget == scratch.workerTaskTarget);

	// classification flags + factory bugger-off scalars
	const CFactory* fac = dynamic_cast<const CFactory*>(liveUnit);
	bool factoryOk =
		(bool(slot->isFactoryCAI) == isFactoryCAI) &&
		(bool(slot->isFactoryUnit) == (fac != nullptr));
	if (fac != nullptr) {
		factoryOk = factoryOk &&
			(bool(slot->boPerform) == fac->boPerform) &&
			(bool(slot->boSherical) == fac->boSherical) &&
			(bool(slot->boForced) == fac->boForced) &&
			bitEq(slot->boOffset, fac->boOffset) &&
			bitEq(slot->boRadius, fac->boRadius) &&
			(slot->boRelHeading == fac->boRelHeading);
	}
	res.factoryOk = factoryOk;

	return res;
}


/******************************************************************************
 * BEGIN PR 33 (pieces/scripts family) -- serving section
 *
 * Serves the 23 piece/script sanctioned callouts (10 unit piece/script, 11
 * feature piece/colvol/last-hit, 2 piece-projectile). Two mechanisms:
 *
 *  - unit/feature piece + script + feature colvol/last-hit: a BARRIER-REFRESHED
 *    cache (RefreshPieces, same shape as the command-queue serving pattern).
 *    Dynamic piece transforms cannot be read lazily post-release under the
 *    running split -- LocalModelPiece::GetModelSpaceMatrix recomputes a dirty
 *    piece, mutating sim-side caches mid-draw (exactly what PR 8's
 *    ExtractTransforms forbids). RefreshPieces() runs AT THE BARRIER (sim
 *    parked / single-threaded, right after simSnapshot.Update()) and captures
 *    the FINAL computed values via the live LocalModelPiece/CSolidObject
 *    accessors, so served values are bit-identical to the live callouts by
 *    construction. Static piece metadata (names/hierarchy/geometry/offsets/
 *    emit-dir) is model-immutable: cached per-model once (keyed by the shared
 *    root S3DModelPiece*), served directly, never recopied per boundary.
 *
 *  - piece-projectile params/name: SimSnapshot::ProjectileRows extension (the
 *    add-a-field recipe); CPieceProjectile is a synced projectile so its ids
 *    resolve like every other projectile row, verified by SnapshotDiffGate's
 *    P_PIECEPARAMS field pass rather than the Route() dual-run.
 *
 * POV: unit piece callouts mirror ParseTypedUnit (rows.Valid + PovUnitTyped);
 * feature callouts mirror ParseFeature (featRows.Valid + PovFeatureVisible);
 * piece-projectile callouts mirror ParseProjectile (ProjectileRows::PovVisible).
 * Enumerated deviations (see the commit message): (a) piece dynamic values are
 * captured at the barrier, memory cost is one boundary of transforms/matrices
 * per object (bounded, generation-gated, documented follow-up: expose PR 8's
 * transformsMemStorage by id); (b) the unreachable GetPieceProjectileName
 * omp==null defensive branch is not reproduced; (c) draw-side interpolation
 * side-effect (integration review): RefreshPieces walks GetModelSpaceMatrix/
 * GetAbsolutePos/GetEmitDirPos over ALL pieces of ALL units and features,
 * including out-of-LOS objects. For a dirty piece GetModelSpaceMatrix runs
 * UpdateParentMatricesRec, which clears `dirty`, sets wasUpdated[0]=true and
 * rewrites modelSpaceMat. PR 8's ExtractTransforms (later the same frame in
 * worldDrawer.Update) is LOS-gated and leaves hidden pieces lazy, so under an
 * armed gate / SimDrawSplit=1 RefreshPieces pre-touches the unsynced
 * interpolation bookkeeping (wasUpdated / prevModelSpaceTra path) of hidden
 * pieces that master/PR 8 would not recompute at that point. This is NOT a
 * sync deviation (wasUpdated is CR_IGNORED, modelSpaceMat is unsynced), does
 * NOT affect flag-off (RefreshPieces early-outs) or served Lua values (served
 * from the separate cache): it only advances the same recompute PR 8 would do
 * later to earlier in the frame for hidden pieces. Verified no LOS-entry
 * interpolation artifact under the split's resim gate (checksum-clean).
 ******************************************************************************/

namespace {
	// per-model immutable metadata (names/hierarchy/geometry/offset/emit-dir),
	// keyed by the shared root S3DModelPiece* -- all instances of a model share
	// the same original piece tree, so this is built once and reused
	struct ModelPieceMeta {
		struct PieceStatic {
			std::string name;
			std::string parentName;             // parent ? parent->name : "[null]"
			std::vector<std::string> children;  // child piece names, in order
			bool hasGeometry = false;
			float3 mins;
			float3 maxs;
			float3 offset;
			float3 emitDir;                     // LocalModelPiece::GetDirection() == original->GetEmitDir()
		};
		int32_t rootPieceIndex = 0;
		std::vector<PieceStatic> pieces;
	};

	spring::unordered_map<const void*, ModelPieceMeta> modelMetaCache;

	// captured dynamic (per-boundary) piece state, final computed values
	struct PieceDynamic {
		float3 absPos;               // LocalModelPiece::GetAbsolutePos()
		CMatrix44f modelSpaceMat;    // LocalModelPiece::GetModelSpaceMatrix()
		float3 posDirPos;            // GetSolidObjectPiecePosDir's pos (object space)
		float3 posDirDir;            // GetSolidObjectPiecePosDir's dir (object space)
		CollisionVolume pieceColVol; // LocalModelPiece::GetCollisionVolume() (features)
	};

	struct ObjectPieceSlot {
		bool present = false;
		const void* metaKey = nullptr;   // ModelPieceMeta key (root original piece ptr)
		int32_t rootPieceIndex = 0;
		int32_t numPieces = 0;
		std::vector<PieceDynamic> pieces;
		// unit script mapping (fixed after script init; empty for features).
		// scriptToModel[sp] == CUnitScript::ScriptToModel(sp) (-1 = none)
		std::vector<int32_t> scriptToModel;
		// feature-only extras
		bool hasColVol = false;
		CollisionVolume colVol;          // CFeature::collisionVolume
		int32_t lastHitPieceIndex = -1;  // hitModelPieces[true] lmodel index, -1 = none
		int32_t lastHitFrame = -1;       // pieceHitFrames[true]
	};

	// indexed by unitID / featureID, sized at first refresh
	std::vector<ObjectPieceSlot> unitPieceCache;
	std::vector<ObjectPieceSlot> featurePieceCache;
	uint32_t pieceCacheGeneration = 0;

	// build (once) the immutable metadata for o's model from a live LocalModel
	const ModelPieceMeta& GetOrBuildModelMeta(const void* key, const LocalModel& lm)
	{
		auto it = modelMetaCache.find(key);
		if (it != modelMetaCache.end())
			return it->second;

		ModelPieceMeta meta;
		meta.rootPieceIndex = lm.GetRoot()->GetLModelPieceIndex();
		meta.pieces.resize(lm.pieces.size());

		for (size_t i = 0; i < lm.pieces.size(); ++i) {
			const S3DModelPiece& op = *(lm.pieces[i].original);
			ModelPieceMeta::PieceStatic& ps = meta.pieces[i];
			ps.name = op.name;
			ps.parentName = (op.parent != nullptr) ? op.parent->name : "[null]";
			ps.children.resize(op.children.size());
			for (size_t c = 0; c < op.children.size(); ++c)
				ps.children[c] = op.children[c]->name;
			ps.hasGeometry = op.HasGeometryData();
			ps.mins = op.mins;
			ps.maxs = op.maxs;
			ps.offset = op.offset;
			ps.emitDir = lm.pieces[i].GetDirection();
		}

		return modelMetaCache.emplace(key, std::move(meta)).first->second;
	}

	// capture o's dynamic piece state + static-metadata key at the barrier
	void RefreshObjectPieceSlot(ObjectPieceSlot& slot, const CSolidObject* o,
	                            bool captureScript, bool captureColVol, bool captureLastHit)
	{
		const LocalModel& lm = o->localModel;
		const size_t numPieces = lm.pieces.size();

		slot.present = true;
		// GetRoot() == GetPiece(0) asserts HasPiece(0) then derefs pieces[0];
		// under NDEBUG the assert is gone, so an empty LocalModel (pieceless
		// model, e.g. some map features / not-yet-initialized) would OOB-deref.
		// RefreshPieces walks ALL active objects unconditionally (the live
		// callouts only ever run per-queried-object), so guard the root read
		// with the same (numPieces > 0) test the metaKey line below already uses.
		slot.numPieces = static_cast<int32_t>(numPieces);
		slot.rootPieceIndex = (numPieces > 0) ? lm.GetRoot()->GetLModelPieceIndex() : -1;
		slot.metaKey = (numPieces > 0) ? static_cast<const void*>(lm.pieces[0].original) : nullptr;

		if (slot.metaKey != nullptr)
			GetOrBuildModelMeta(slot.metaKey, lm); // ensure metadata exists

		slot.pieces.resize(numPieces);
		for (size_t i = 0; i < numPieces; ++i) {
			const LocalModelPiece& lmp = lm.pieces[i];
			PieceDynamic& pd = slot.pieces[i];

			// final values via the exact live accessors (bit-identical serving)
			pd.absPos = lmp.GetAbsolutePos();
			pd.modelSpaceMat = lmp.GetModelSpaceMatrix();

			float3 emitPos;
			float3 emitDir;
			lmp.GetEmitDirPos(emitPos, emitDir);
			pd.posDirPos = o->GetObjectSpacePos(emitPos);
			pd.posDirDir = o->GetObjectSpaceVec(emitDir);

			if (captureColVol)
				pd.pieceColVol = *(lmp.GetCollisionVolume());
		}

		slot.scriptToModel.clear();
		if (captureScript) {
			const CUnit* u = static_cast<const CUnit*>(o);
			const CUnitScript* script = u->script; // never null for a live unit
			slot.scriptToModel.resize(script->pieces.size());
			for (size_t sp = 0; sp < script->pieces.size(); ++sp)
				slot.scriptToModel[sp] = script->ScriptToModel(static_cast<int>(sp));
		}

		slot.hasColVol = captureColVol;
		if (captureColVol)
			slot.colVol = o->collisionVolume;

		slot.lastHitPieceIndex = -1;
		slot.lastHitFrame = -1;
		if (captureLastHit && o->hitModelPieces[true] != nullptr) {
			slot.lastHitPieceIndex = static_cast<int32_t>(o->hitModelPieces[true]->GetLModelPieceIndex());
			slot.lastHitFrame = o->pieceHitFrames[true];
		}
	}

	const ObjectPieceSlot* GetUnitPieceSlot(int unitID)
	{
		if (unitID < 0 || static_cast<size_t>(unitID) >= unitPieceCache.size())
			return nullptr;
		const ObjectPieceSlot& s = unitPieceCache[unitID];
		return s.present ? &s : nullptr;
	}

	const ObjectPieceSlot* GetFeaturePieceSlot(int featureID)
	{
		if (featureID < 0 || static_cast<size_t>(featureID) >= featurePieceCache.size())
			return nullptr;
		const ObjectPieceSlot& s = featurePieceCache[featureID];
		return s.present ? &s : nullptr;
	}

	// ParseTypedUnit mirror (rows.Valid + PovUnitTyped) + piece-slot lookup
	const ObjectPieceSlot* ParseTypedUnitPieceSlot(lua_State* L, const char* caller)
	{
		const auto& rows = simSnapshot.Read();
		const int unitID = ParseUnitIDSynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
			return nullptr;

		return GetUnitPieceSlot(unitID);
	}

	// ParseFeature mirror (featRows.Valid + PovFeatureVisible) + piece-slot lookup
	const ObjectPieceSlot* ParseFeaturePieceSlot(lua_State* L, const char* caller)
	{
		const auto& rows = simSnapshot.ReadFeatures();
		const int featureID = ParseFeatureIDSynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
			return nullptr;

		return GetFeaturePieceSlot(featureID);
	}

	// the piece-index arg (#2) mirror of ParseObjectConstLocalModelPiece:
	// luaL_checkint - 1, then HasPiece bounds check. returns -1 on miss.
	int ParsePieceIndex(lua_State* L, const ObjectPieceSlot* slot)
	{
		const int pieceIdx = luaL_checkint(L, 2) - 1;
		if (pieceIdx < 0 || pieceIdx >= slot->numPieces)
			return -1;
		return pieceIdx;
	}

	const ModelPieceMeta* GetSlotMeta(const ObjectPieceSlot* slot)
	{
		if (slot->metaKey == nullptr)
			return nullptr;
		auto it = modelMetaCache.find(slot->metaKey);
		return (it != modelMetaCache.end()) ? &it->second : nullptr;
	}

	// GetSolidObjectPieceInfoHelper mirror over cached metadata
	int PushPieceInfoSnap(lua_State* L, const ModelPieceMeta::PieceStatic& ps)
	{
		lua_createtable(L, 0, 7);
		HSTR_PUSH_STRING(L, "name", ps.name);
		HSTR_PUSH_STRING(L, "parent", ps.parentName);

		HSTR_PUSH(L, "children");
		lua_createtable(L, ps.children.size(), 0);
		for (size_t c = 0; c < ps.children.size(); c++) {
			lua_pushsstring(L, ps.children[c]);
			lua_rawseti(L, -2, c + 1);
		}
		lua_rawset(L, -3);

		HSTR_PUSH(L, "isEmpty");
		lua_pushboolean(L, !ps.hasGeometry);
		lua_rawset(L, -3);

		HSTR_PUSH(L, "min");
		lua_createtable(L, 3, 0); {
			lua_pushnumber(L, ps.mins.x); lua_rawseti(L, -2, 1);
			lua_pushnumber(L, ps.mins.y); lua_rawseti(L, -2, 2);
			lua_pushnumber(L, ps.mins.z); lua_rawseti(L, -2, 3);
		}
		lua_rawset(L, -3);

		HSTR_PUSH(L, "max");
		lua_createtable(L, 3, 0); {
			lua_pushnumber(L, ps.maxs.x); lua_rawseti(L, -2, 1);
			lua_pushnumber(L, ps.maxs.y); lua_rawseti(L, -2, 2);
			lua_pushnumber(L, ps.maxs.z); lua_rawseti(L, -2, 3);
		}
		lua_rawset(L, -3);

		HSTR_PUSH(L, "offset");
		lua_createtable(L, 3, 0); {
			lua_pushnumber(L, ps.offset.x); lua_rawseti(L, -2, 1);
			lua_pushnumber(L, ps.offset.y); lua_rawseti(L, -2, 2);
			lua_pushnumber(L, ps.offset.z); lua_rawseti(L, -2, 3);
		}
		lua_rawset(L, -3);
		return 1;
	}

	// ---- shared serving bodies over an already-resolved ObjectPieceSlot ----

	int ServeRootPiece(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		lua_pushnumber(L, slot->rootPieceIndex + 1);
		return 1;
	}

	int ServePieceMap(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const ModelPieceMeta* meta = GetSlotMeta(slot);
		lua_createtable(L, 0, slot->numPieces);
		if (meta != nullptr) {
			for (int i = 0; i < slot->numPieces; i++) {
				lua_pushsstring(L, meta->pieces[i].name);
				lua_pushnumber(L, i + 1);
				lua_rawset(L, -3);
			}
		}
		return 1;
	}

	int ServePieceList(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const ModelPieceMeta* meta = GetSlotMeta(slot);
		lua_createtable(L, slot->numPieces, 0);
		if (meta != nullptr) {
			for (int i = 0; i < slot->numPieces; i++) {
				lua_pushsstring(L, meta->pieces[i].name);
				lua_rawseti(L, -2, i + 1);
			}
		}
		return 1;
	}

	int ServePieceInfo(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const int pieceIdx = ParsePieceIndex(L, slot);
		if (pieceIdx < 0)
			return 0;
		const ModelPieceMeta* meta = GetSlotMeta(slot);
		if (meta == nullptr)
			return 0;
		return PushPieceInfoSnap(L, meta->pieces[pieceIdx]);
	}

	int ServePiecePosition(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const int pieceIdx = ParsePieceIndex(L, slot);
		if (pieceIdx < 0)
			return 0;
		const float3& pos = slot->pieces[pieceIdx].absPos;
		lua_pushnumber(L, pos.x);
		lua_pushnumber(L, pos.y);
		lua_pushnumber(L, pos.z);
		return 3;
	}

	int ServePieceDirection(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const int pieceIdx = ParsePieceIndex(L, slot);
		if (pieceIdx < 0)
			return 0;
		const ModelPieceMeta* meta = GetSlotMeta(slot);
		if (meta == nullptr)
			return 0;
		const float3& dir = meta->pieces[pieceIdx].emitDir;
		lua_pushnumber(L, dir.x);
		lua_pushnumber(L, dir.y);
		lua_pushnumber(L, dir.z);
		return 3;
	}

	int ServePiecePosDir(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const int pieceIdx = ParsePieceIndex(L, slot);
		if (pieceIdx < 0)
			return 0;
		const PieceDynamic& pd = slot->pieces[pieceIdx];
		lua_pushnumber(L, pd.posDirPos.x);
		lua_pushnumber(L, pd.posDirPos.y);
		lua_pushnumber(L, pd.posDirPos.z);
		lua_pushnumber(L, pd.posDirDir.x);
		lua_pushnumber(L, pd.posDirDir.y);
		lua_pushnumber(L, pd.posDirDir.z);
		return 6;
	}

	int ServePieceMatrix(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		const int pieceIdx = ParsePieceIndex(L, slot);
		if (pieceIdx < 0)
			return 0;
		for (float mi: slot->pieces[pieceIdx].modelSpaceMat.m)
			lua_pushnumber(L, mi);
		return 16;
	}

	// GetSolidObjectLastHitPiece mirror (feature only in PR 33)
	int ServeLastHitPiece(lua_State* L, const ObjectPieceSlot* slot)
	{
		if (slot == nullptr)
			return 0;
		if (slot->lastHitPieceIndex < 0)
			return 0;
		const ModelPieceMeta* meta = GetSlotMeta(slot);

		if (lua_isboolean(L, 1) && lua_toboolean(L, 1)) {
			lua_pushnumber(L, slot->lastHitPieceIndex + 1);
		} else {
			if (meta == nullptr)
				return 0;
			lua_pushsstring(L, meta->pieces[slot->lastHitPieceIndex].name);
		}
		lua_pushnumber(L, slot->lastHitFrame);
		return 2;
	}
}


void LuaSnapshotServe::RefreshPieces()
{
	// barrier-only (sim parked / single-threaded), right after simSnapshot.Update().
	// generation-gated so the copies always describe the same boundary as the
	// published rows, and so the walk is free when the publish did not swap.
	const uint32_t gen = simSnapshot.Generation();

	if (gen == 0 || gen == pieceCacheGeneration)
		return;

	// flag-off (no contract, gate unarmed) never serves these twins -- skip the
	// whole capture so flag-off pays only the generation compare above
	if (!LuaSplitContract::Enabled() && !snapshotDiffGate.Armed())
		return;

	pieceCacheGeneration = gen;

	const size_t maxUnits = unitHandler.MaxUnits();
	if (unitPieceCache.size() != maxUnits)
		unitPieceCache.resize(maxUnits);
	for (ObjectPieceSlot& s: unitPieceCache)
		s.present = false;

	for (const CUnit* u: unitHandler.GetActiveUnits())
		RefreshObjectPieceSlot(unitPieceCache[u->id], u, /*script*/true, /*colVol*/false, /*lastHit*/false);

	const auto& activeFeatureIDs = featureHandler.GetActiveFeatureIDs();
	int maxFeatureID = -1;
	for (const int id: activeFeatureIDs)
		maxFeatureID = std::max(maxFeatureID, id);
	const size_t wantFeatSlots = static_cast<size_t>(maxFeatureID + 1);
	if (featurePieceCache.size() < wantFeatSlots)
		featurePieceCache.resize(std::max(wantFeatSlots, featurePieceCache.size()));
	for (ObjectPieceSlot& s: featurePieceCache)
		s.present = false;

	for (const int id: activeFeatureIDs) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f != nullptr)
			RefreshObjectPieceSlot(featurePieceCache[id], f, /*script*/false, /*colVol*/true, /*lastHit*/true);
	}
}


// ---- unit piece/script twins (mirror LuaSyncedRead, ParseTypedUnit gate) ----
int LuaSnapshotServe::GetUnitRootPiece(lua_State* L, const char* caller)   { return ServeRootPiece(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPieceMap(lua_State* L, const char* caller)    { return ServePieceMap(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPieceList(lua_State* L, const char* caller)   { return ServePieceList(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPieceInfo(lua_State* L, const char* caller)   { return ServePieceInfo(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPiecePosition(lua_State* L, const char* caller){ return ServePiecePosition(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPieceDirection(lua_State* L, const char* caller){ return ServePieceDirection(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPiecePosDir(lua_State* L, const char* caller) { return ServePiecePosDir(L, ParseTypedUnitPieceSlot(L, caller)); }
int LuaSnapshotServe::GetUnitPieceMatrix(lua_State* L, const char* caller) { return ServePieceMatrix(L, ParseTypedUnitPieceSlot(L, caller)); }

// mirror of LuaSyncedRead::GetUnitScriptPiece
int LuaSnapshotServe::GetUnitScriptPiece(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseTypedUnitPieceSlot(L, caller);
	if (slot == nullptr)
		return 0;

	const std::vector<int32_t>& scriptToModel = slot->scriptToModel;

	if (!lua_isnumber(L, 2)) {
		// whole script->piece map
		lua_newtable(L);
		for (size_t sp = 0; sp < scriptToModel.size(); sp++) {
			const int piece = scriptToModel[sp];
			if (piece != -1) {
				lua_pushnumber(L, piece + 1);
				lua_rawseti(L, -2, sp);
			}
		}
		return 1;
	}

	const int scriptPiece = lua_toint(L, 2);
	const int piece = (scriptPiece >= 0 && static_cast<size_t>(scriptPiece) < scriptToModel.size())
		? scriptToModel[scriptPiece] : -1;
	if (piece < 0)
		return 0;

	lua_pushnumber(L, piece + 1);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitScriptNames
int LuaSnapshotServe::GetUnitScriptNames(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseTypedUnitPieceSlot(L, caller);
	if (slot == nullptr)
		return 0;

	const ModelPieceMeta* meta = GetSlotMeta(slot);
	const std::vector<int32_t>& scriptToModel = slot->scriptToModel;

	lua_createtable(L, scriptToModel.size(), 0);
	if (meta != nullptr) {
		for (size_t sp = 0; sp < scriptToModel.size(); sp++) {
			const int piece = scriptToModel[sp];
			if (piece < 0)
				continue; // live derefs pieces[sp]->original directly (non-null in practice)
			lua_pushsstring(L, meta->pieces[piece].name);
			lua_pushnumber(L, sp);
			lua_rawset(L, -3);
		}
	}
	return 1;
}


// ---- feature piece / colvol / last-hit twins (ParseFeature gate) ----
int LuaSnapshotServe::GetFeatureRootPiece(lua_State* L, const char* caller)   { return ServeRootPiece(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePieceMap(lua_State* L, const char* caller)    { return ServePieceMap(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePieceList(lua_State* L, const char* caller)   { return ServePieceList(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePieceInfo(lua_State* L, const char* caller)   { return ServePieceInfo(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePiecePosition(lua_State* L, const char* caller){ return ServePiecePosition(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePieceDirection(lua_State* L, const char* caller){ return ServePieceDirection(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePiecePosDir(lua_State* L, const char* caller) { return ServePiecePosDir(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeaturePieceMatrix(lua_State* L, const char* caller) { return ServePieceMatrix(L, ParseFeaturePieceSlot(L, caller)); }
int LuaSnapshotServe::GetFeatureLastAttackedPiece(lua_State* L, const char* caller) { return ServeLastHitPiece(L, ParseFeaturePieceSlot(L, caller)); }

// mirror of LuaSyncedRead::GetFeatureCollisionVolumeData
int LuaSnapshotServe::GetFeatureCollisionVolumeData(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseFeaturePieceSlot(L, caller);
	if (slot == nullptr)
		return 0;
	return LuaUtils::PushColVolData(L, &slot->colVol);
}

// mirror of LuaSyncedRead::GetFeaturePieceCollisionVolumeData (PushPieceCollisionVolumeData)
int LuaSnapshotServe::GetFeaturePieceCollisionVolumeData(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseFeaturePieceSlot(L, caller);
	if (slot == nullptr)
		return 0;
	const int pieceIdx = ParsePieceIndex(L, slot);
	if (pieceIdx < 0)
		return 0;
	return LuaUtils::PushColVolData(L, &slot->pieces[pieceIdx].pieceColVol);
}


// ---- piece-projectile twins (ProjectileRows extension, ParseProjectile gate) ----
// mirror of LuaSyncedRead::GetPieceProjectileParams
int LuaSnapshotServe::GetPieceProjectileParams(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (rows.isPiece[projID] == 0)
		return 0;

	lua_pushnumber(L, rows.pieceExplFlags[projID]);
	lua_pushnumber(L, rows.pieceSpinAngle[projID]);
	lua_pushnumber(L, rows.pieceSpinSpeed[projID]);
	lua_pushnumber(L, rows.pieceSpinVec[projID].x);
	lua_pushnumber(L, rows.pieceSpinVec[projID].y);
	lua_pushnumber(L, rows.pieceSpinVec[projID].z);
	return (1 + 1 + 1 + 3);
}

// mirror of LuaSyncedRead::GetPieceProjectileName
int LuaSnapshotServe::GetPieceProjectileName(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (rows.isPiece[projID] == 0)
		return 0;
	// deviation: the live path's unreachable omp==null guard maps here to an
	// empty pieceName -- a piece projectile always has a non-null omp
	lua_pushsstring(L, rows.pieceName[projID]);
	return 1;
}

/* END PR 33 (pieces/scripts family) -- serving section */


void LuaSnapshotServe::ClearCaches()
{
	teamUnitIndex.built = false;
	teamUnitIndex.generation = 0;
	teamUnitIndex.allIDs.clear();
	for (auto& v: teamUnitIndex.idsByTeam)
		v.clear();
	for (auto& m: teamUnitIndex.idsByTeamAndDef)
		m.clear();

	// command-queue serving cache: unit ids and queue versions restart with
	// the next game, so a surviving entry could alias fresh ones
	cmdQueueCache.clear();
	cmdQueueCache.shrink_to_fit();
	cmdQueueCacheGeneration = 0;

	// PR 33 piece caches: unit/feature ids and model pointers restart with the
	// next game, so a surviving entry could alias fresh ones
	unitPieceCache.clear();
	unitPieceCache.shrink_to_fit();
	featurePieceCache.clear();
	featurePieceCache.shrink_to_fit();
	modelMetaCache.clear();
	pieceCacheGeneration = 0;
}


// ===========================================================================
// PR 28: map-layer mirror family (positional LOS + map info)
// Served from DrawMapMirrors, not SimSnapshot rows. Each twin is a line-by-line
// mirror of the live LuaSyncedRead body with the losHandler/mapInfo/smoothGround
// /orig-heightmap dereference replaced by the corresponding drawMapMirrors query
// (which itself reproduces the live formula over the mirror). Argument parsing,
// error text and return shapes are identical by construction.
// ===========================================================================

namespace {
	// LuaSyncedRead::GetEffectiveLosAllyTeam mirror. Reads only the handle POV
	// (CLuaHandle) and the mirror's ally-team count (== teamHandler.
	// ActiveAllyTeams(), captured at the drain) -- no live teamHandler read.
	inline int ServeEffectiveLosAllyTeam(lua_State* L, int arg)
	{
		if (lua_isnoneornil(L, arg))
			return (CLuaHandle::GetHandleReadAllyTeam(L));

		const int aat = luaL_optint(L, arg, CEventClient::MinSpecialTeam - 1);

		if (aat == CEventClient::NoAccessTeam)
			return aat;

		if (CLuaHandle::GetHandleFullRead(L)) {
			if (aat >= 0 && aat < drawMapMirrors.NumAllyTeams())
				return aat;

			if (aat == CEventClient::AllAccessTeam)
				return aat;
		} else {
			if (aat == CLuaHandle::GetHandleReadAllyTeam(L))
				return aat;
		}

		// never returns
		return (luaL_argerror(L, arg, "Invalid allyTeam"));
	}
}

int LuaSnapshotServe::IsPosInLos(lua_State* L, const char* caller)
{
	const float3 pos(luaL_checkfloat(L, 1),
	                 luaL_checkfloat(L, 2),
	                 luaL_checkfloat(L, 3));

	const int allyTeamID = ServeEffectiveLosAllyTeam(L, 4);
	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, drawMapMirrors.PosInLos(pos, allyTeamID));
	return 1;
}

int LuaSnapshotServe::IsPosInRadar(lua_State* L, const char* caller)
{
	const float3 pos(luaL_checkfloat(L, 1),
	                 luaL_checkfloat(L, 2),
	                 luaL_checkfloat(L, 3));

	const int allyTeamID = ServeEffectiveLosAllyTeam(L, 4);
	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, drawMapMirrors.PosInRadar(pos, allyTeamID));
	return 1;
}

int LuaSnapshotServe::IsPosInAirLos(lua_State* L, const char* caller)
{
	const float3 pos(luaL_checkfloat(L, 1),
	                 luaL_checkfloat(L, 2),
	                 luaL_checkfloat(L, 3));

	const int allyTeamID = ServeEffectiveLosAllyTeam(L, 4);
	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, drawMapMirrors.PosInAirLos(pos, allyTeamID));
	return 1;
}

int LuaSnapshotServe::GetPositionLosState(lua_State* L, const char* caller)
{
	const float3 pos(luaL_checkfloat(L, 1),
	                 luaL_checkfloat(L, 2),
	                 luaL_checkfloat(L, 3));

	const int allyTeamID = ServeEffectiveLosAllyTeam(L, 4);
	if (allyTeamID < 0) {
		const bool fullView = (allyTeamID == CEventClient::AllAccessTeam);
		lua_pushboolean(L, fullView);
		lua_pushboolean(L, fullView);
		lua_pushboolean(L, fullView);
		lua_pushboolean(L, fullView);
		return 4;
	}

	const bool inLos    = drawMapMirrors.PosInLos(pos, allyTeamID);
	const bool inRadar  = drawMapMirrors.PosInRadar(pos, allyTeamID);
	const bool inJammer = drawMapMirrors.PosInJammer(pos, allyTeamID);

	lua_pushboolean(L, inLos || inRadar);
	lua_pushboolean(L, inLos);
	lua_pushboolean(L, inRadar);
	lua_pushboolean(L, inJammer);
	return 4;
}

int LuaSnapshotServe::GetRadarErrorParams(lua_State* L, const char* caller)
{
	const int allyTeamID = lua_tonumber(L, 1);

	if (!(allyTeamID >= 0 && allyTeamID < drawMapMirrors.NumAllyTeams()))
		return 0;

	// LuaUtils::IsAlliedAllyTeam(L, allyTeamID) mirror
	const int readAllyTeam = CLuaHandle::GetHandleReadAllyTeam(L);
	const bool allied = (readAllyTeam < 0) ? CLuaHandle::GetHandleFullRead(L) : (allyTeamID == readAllyTeam);

	if (allied) {
		lua_pushnumber(L, drawMapMirrors.AllyTeamRadarErrorSize(allyTeamID));
	} else {
		lua_pushnumber(L, drawMapMirrors.BaseRadarErrorSize());
	}
	lua_pushnumber(L, drawMapMirrors.BaseRadarErrorSize());
	lua_pushnumber(L, drawMapMirrors.BaseRadarErrorMult());
	return 3;
}

int LuaSnapshotServe::GetTerrainTypeData(lua_State* L, const char* caller)
{
	const int tti = luaL_checkint(L, 1);

	if (tti < 0 || tti >= drawMapMirrors.TerrainTypeCount())
		return 0;

	// PushTerrainTypeData(L, tt, false) mirror (8 returns: index + name + the
	// 5 speed/hardness floats + receiveTracks)
	const DrawMapMirrors::TerrainType& tt = drawMapMirrors.TerrainTypeAt(tti);
	lua_pushinteger(L, tti);
	lua_pushsstring(L, tt.name);
	lua_pushnumber(L, tt.hardness);
	lua_pushnumber(L, tt.tankSpeed);
	lua_pushnumber(L, tt.kbotSpeed);
	lua_pushnumber(L, tt.hoverSpeed);
	lua_pushnumber(L, tt.shipSpeed);
	lua_pushboolean(L, tt.receiveTracks);
	return 8;
}

int LuaSnapshotServe::GetSmoothMeshHeight(lua_State* L, const char* caller)
{
	const float x = luaL_checkfloat(L, 1);
	const float z = luaL_checkfloat(L, 2);

	lua_pushnumber(L, drawMapMirrors.SmoothMeshHeight(x, z));
	return 1;
}

int LuaSnapshotServe::GetGroundOrigHeight(lua_State* L, const char* caller)
{
	const float x = luaL_checkfloat(L, 1);
	const float z = luaL_checkfloat(L, 2);

	lua_pushnumber(L, drawMapMirrors.OrigHeight(x, z));
	return 1;
}
