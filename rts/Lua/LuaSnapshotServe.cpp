/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSnapshotServe.h"

#include <algorithm>
#include <array>  // PR 44a: per-ring-slot cache arrays
#include <cassert> // the rotation twins' IsOrthoNormal assert
#include <cstdio>
#include <cstring>
#include <map>    // PR 34: PushUnitListSortedByDefSnap's ordered def buckets
#include <memory> // PR 44a: pointer-stable model-meta values
#include <mutex>  // PR 44a: cross-thread query channel + model-meta cache
#include <unordered_map> // PR 35: trace-query reply map (main-thread-only channel)
#include <vector>

#include "LuaConfig.h" // PR 31: LUA_WEAPON_BASE_INDEX (weapon/shield family)
#include "LuaHandle.h"
#include "LuaHashString.h" // HSTR_PUSH_BOOL
#include "LuaInclude.h"
#include "LuaRulesParams.h" // PR 38: game+team rules-params twins (Params/Param)
#include "LuaSplitContract.h"
#include "LuaUtils.h" // allegiance constants (spatial-list twins)

#include "Game/Camera.h"
#include "Game/Game.h" // the stats callouts' live `game` null-check
#include "Game/GameHelper.h" // sim|draw PR 29: Pos2BuildPos build-grid snap (draw-safe unsynced heightmap)
#include "Game/GlobalUnsynced.h" // gu->myAllyTeam (IsUnitAllied's fullRead answer)
#include "Game/SelectedUnitsHandler.h" // IsUnitSelected's id-set payload; PR 34 GetSelectedUnits* draw-owned id set
#include "Game/UI/Groups/Group.h" // CGroup::id (GetUnitGroup); PR 34 CGroup::units (GetGroupUnits*)
#include "Game/UI/Groups/GroupHandler.h" // uiGroupHandlers (GetUnitGroup / PR 34 GetGroupUnits*)
#include "Map/MapDimensions.h" // sim|draw PR 29: GetGroundBlocked map-coord clamp
#include "Map/ReadMap.h" // sim|draw PR 40: readMap->GridVisibility (frustum twins)
#include "Rendering/Common/DrawMapMirrors.h" // PR 28: map-layer mirror serving
#include "Rendering/Common/PlacementEpochView.h" // PLACEMENT REHOST (stage 3): EpochView
#include "Game/PlacementPredicates.h"          // PLACEMENT REHOST (stage 3): templated predicates
#include "Rendering/Common/TraceEpochView.h"  // TRACE REHOST (stage 4): trace::EpochView
#include "Sim/Weapons/WeaponPredicates.h"      // TRACE REHOST (stage 4): templated trace predicates
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/Common/SnapshotPickGrid.h"
#include "Rendering/Features/FeatureDrawer.h" // CFeatureDrawer::GetDrawFlag / GetUnsyncedTransformMatrix
#include "Rendering/GlobalRendering.h" // timeOffset (draw-owned)
#include "Rendering/IconHandler.h" // icon data pushes (GetUnitIcon/GetUnitIconData)
#include "Rendering/Models/3DModel.hpp" // PR 32 GetUnitEffectiveBuildRange: S3DModel::radius
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
#include "Sim/Misc/LosHandler.h" // sim|draw PR 38e: TestMoveOrder barrier los gate (losHandler->InLos)
#include "Sim/Misc/QuadField.h" // sim|draw PR 40: quadField geometry + GetQuads (frustum membership mirror)
#include "Sim/MoveTypes/MoveDefHandler.h" // immutable MoveDef name (GetUnitMoveDefID)
#include "Sim/MoveTypes/MoveMath/MoveMath.h" // sim|draw PR 38e: TestMoveOrder barrier CheckCollisionQuery ctor
#include "Sim/MoveTypes/AAirMoveType.h" // PR 32 GetUnitMoveTypeData aircraftState enum
#include "Sim/MoveTypes/HoverAirMoveType.h" // PR 32 GetUnitMoveTypeData flyState enum
#include "Sim/MoveTypes/GroundMoveType.h" // WS-6 GetUnitEstimatedPath first-touch live read
#include "Sim/Path/IPathManager.h" // WS-6 GetUnitEstimatedPath first-touch GetPathWayPoints
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name twins
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectileTypes.h" // PR 31 WEAPON_*_PROJECTILE (GetUnitWeaponVectors)
#include "Sim/Weapons/WeaponTarget.h" // PR 31 Target_* (GetUnitWeaponTarget/CanFire)
#include "Sim/Weapons/Weapon.h" // PR 35 CWeapon trace predicates (barrier evaluation)
#include "Sim/Units/BuildInfo.h" // sim|draw PR 29: Pos2BuildPos
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
#include "System/Config/ConfigHandler.h" // WS-1 PieceSkipOracle knob
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

// WS-1 §6.2 DS oracle: every N produced epochs the producer re-captures every
// piece slot the version-skip skipped and error-logs the first differing field
// (the missed-mutation-choke detector). 0 disables; enable for at least one
// full-length replay per gate battery and when investigating piece staleness.
CONFIG(int, PieceSkipOracle)
	.defaultValue(0)
	.minimumValue(0)
	.description("Every N produced epochs, re-capture skipped piece-cache slots and log mismatches (sim|draw WS-1 skip oracle); 0 = off.");

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
	// drawer's render record, NEVER the sim-owned handler tables (dogfood
	// invariant; same pattern as the GetUnitDrawFlag live fix, commit
	// 6910e82315). nullptr = dead per drawer; callers answer the "no such
	// unit" nil shape. PR 43 (3b): the died-in-burst shell fallback is
	// retired -- the record itself RETAINS a died-in-batch id (obj = shell)
	// through the deferred-dispatch window (ClearDeadRetainedRecords).
	inline const CUnit* ResolveDrawUnit(int unitID)
	{
		return DrawerGetObjectByID<CUnit>(unitID);
	}

	// draw-side feature resolver for the unsynced-owned payload reads (the
	// luaDraw/noDraw/drawFlag/selection-volume family): the drawer's render
	// record, NEVER the sim-owned featureHandler (dogfood invariant). PR 43
	// (3b): shell fallback retired, see ResolveDrawUnit.
	inline const CFeature* ResolveDrawFeature(int featureID)
	{
		return DrawerGetObjectByID<CFeature>(featureID);
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
			// PR 34 (spatial/list remainder): whole-list twins with the same
			// ascending-id order deviation. The selection/group aggregates and
			// the centroids are deliberately NOT here: they iterate the same Lua
			// table / draw-owned container as the live path (order-exact) and the
			// counts tables are keyed (multiset compare would be wrong for them).
			"GetAllProjectiles", "GetProjectilesInSphere", "GetAllFeatures",
			// PR 39: GetUnitsInPlanes serves ascending-id per team and
			// reproduces master's per-team counter-reset overwrite. Single-team
			// queries are set-identical (order-only deviation, caught by set
			// mode); multi-team queries can differ in the overwrite tail (the
			// binding Batch-1 ruling's documented deviation) -- set mode is the
			// best comparator available from the two result tables alone.
			"GetUnitsInPlanes",
			// PR 40 (Wave 6): the frustum/screen-rect family served via the
			// per-quad membership mirror. The visible set is identical; only the
			// result table's within-quad order deviates (ascending id vs live
			// quadfield insertion order), so set mode is the correct comparator.
			"GetVisibleUnits", "GetVisibleFeatures",
			"GetUnitsInScreenRectangle", "GetFeaturesInScreenRectangle",
			// PR 41 (Wave 6): GetVisibleProjectiles served over the per-quad SYNCED-
			// projectile membership mirror (hitscan ray / non-hitscan single cell).
			// Same within-quad order deviation (ascending id vs live quadfield
			// insertion order); the visible set is identical, so set mode is correct.
			"GetVisibleProjectiles",
			// PR 40: the two nearest-unit scalars. These return a single id (or
			// nil), not a table, so set mode is a no-op for them (the comparator
			// only id-set-compares TABLE slots) -- listed for completeness/intent.
			// Their BLESSED tie-break deviation (ascending-snapshot-id vs live
			// quadfield-visitation order on exactly-equal distances, Batch-4
			// ruling) may surface as an exact-compare mismatch on those ties.
			"GetUnitNearestAlly", "GetUnitNearestEnemy",
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


int LuaSnapshotServe::RoutePieceCache(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn)
{
	// PR 47 (see the .h comment): the piece caches are captured under exactly
	// this predicate (EnsurePieceCacheCaptured's flag-off skip); when capture
	// is skipped the twins MUST NOT be picked -- the flag-off draw-callin
	// rehearsal would serve NIL from the empty cache where the base branch
	// serves real values. Flag-off single-threaded draw context makes the
	// live leg trivially safe (the sim cannot advance mid-callin).
	if (!LuaSplitContract::Enabled() && !snapshotDiffGate.Armed())
		return liveFn(L, caller);

	return Route(L, caller, liveFn, snapFn);
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
	if (simSnapshot.HeldEpochId() == 0) {
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

	// EVENT-TIME NOTE (PR 38f/38g/38j, sim|draw): three sim-fired deferred EVENT
	// handlers -- gui_selfd_icons (UnitCommand -> cmd queue), unit_idle_guard
	// (UnitCmdDone -> cmd queue/count) and unit_ghostradar_gl4 (UnitLeftLos ->
	// position) -- read state that changed AT the event, which the parked
	// end-of-frame the live leg observes no longer reflects. They carry an
	// event-time override (ScopedCmdQueueEventOverride / SimSnapshotLosEvent)
	// installed around the deferred dispatch. PR 38h briefly re-asserted
	// Enforced() for the WHOLE dispatch to route these reads to the twin, but
	// that forced every OTHER read the handler makes onto the strict path and
	// nil'd the ones that are neither snapshot-served nor sanctioned-live (a 4th
	// widget error); 38j reverted it. Under 38j the override is consulted at the
	// TOP of only the specific affected callouts (GetUnitCommands/CommandCount/
	// CurrentCommand, GetUnitPosition/Direction): when arg#1's unit has an
	// override those callouts serve its snapshot twin DIRECTLY (short-circuit
	// above the dual-run), and every other read the deferred handler makes stays
	// on the live leg via ShouldServe()==false. So these handlers no longer reach
	// the armed dual-run below with an override in play: the diff gate never
	// dispatches deferred (immediate at fire time -> no override installed), so
	// the override globals are inert during any dual-run and there is no
	// event-time divergence for the gate to flag. Flag-off is unaffected (no
	// deferral -> handlers run live as before).
	//
	// armed: run BOTH real paths, bit-compare their actual return slots
	// (masking and gating included by construction), serve the snapshot values.
	// The live returns are stashed in the registry and the stack is reset to
	// the original arguments before the twin runs: optional-arg reads
	// (luaL_optboolean at index nargs+1..) would otherwise see the live
	// returns instead of "none" (found live: a 1-arg GetUnitPosition call made
	// the twin read the live path's x as its midPos flag).
	const int base = lua_gettop(L);

	// Snapshot the ORIGINAL argument values before the live leg. Most live
	// bodies leave their arg slots untouched (they read args, push returns on
	// top), so lua_settop(base) below would restore them -- but a few overwrite
	// their arg slots in place: GetGroundInfoLive pops (x,z) and pushes the
	// quantized (ix,iz) so LuaMetalMap's absolute-index read sees them. For
	// those, lua_settop alone leaves the twin reading the mutated (ix,iz) as its
	// (x,z) args -> a different map square -> a SPURIOUS dual-run mismatch (the
	// twin is correct in real serving, where it runs alone on the true args).
	// Restore the args verbatim instead. (Only reached when the gate is armed.)
	lua_createtable(L, base, 0);
	for (int i = 1; i <= base; ++i) {
		lua_pushvalue(L, i);
		lua_rawseti(L, -2, i);
	}
	const int argRef = luaL_ref(L, LUA_REGISTRYINDEX);

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

	// restore the original args exactly as liveFn first saw them (slots 1..base),
	// undoing any in-place arg mutation the live leg did
	lua_settop(L, 0);
	lua_rawgeti(L, LUA_REGISTRYINDEX, argRef); // args table at slot 1
	for (int i = 1; i <= base; ++i)
		lua_rawgeti(L, 1, i);                  // args -> slots 2..base+1
	lua_remove(L, 1);                          // drop the table -> args at 1..base
	luaL_unref(L, LUA_REGISTRYINDEX, argRef);

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

	// ParseUnit mirror: no such unit / not visible => nil (stale/nil contract).
	//
	// DEAD_THIS_BATCH (mirrors PR 38k's command-queue fix): a deferred UnitLeftLos
	// handler for a unit that died THIS batch finds its snapshot row already
	// invalidated -- the boundary re-extraction cleared valid[] for the now-gone
	// unit. But Extract() overwrites pos[] only for active units, so pos[unitID]
	// still holds the unit's last-boundary (~at-death) position, which is what
	// master's synchronous mid-sim UnitLeftLos handler read (the CUnit is not
	// destroyed until after the event fires). When the LOS-exit override is
	// installed for this one unit, bypass the !Valid nil and serve that retained
	// position -- still gated on PovUnitVisible, which honors the override's
	// captured radar losStatus (INRADAR) so it opens/nils exactly as master's
	// synchronous handler did. Inert flag-off and during the diff-gate dual-run
	// (no deferral -> no override installed -> reduces to the original
	// !Valid || !PovUnitVisible gate -> byte-identical).
	const bool losExitOverride = SimSnapshotLosEvent::ActiveForUnit(unitID);
	if ((!rows.Valid(unitID) && !losExitOverride) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
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
		losStatus = rows.losStatusAll[unitID * rows.numAllyTeams + allyTeamID];
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
		if (urows.valid[id] != SimSnapshotValid::ACTIVE || urows.team[id] != static_cast<uint8_t>(teamID))
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


/******************************************************************************
 * Team/player misc family (PR 36). Served from the PR-36 TeamRows/PlayerRows
 * extensions + the SimSnapshot map-start cache. Allied-POV gates via
 * TeamRows::PovAlliedTeam; the two-arg allied checks read the UnitRows alliance
 * matrix (SimSnapshot::Read().Allied, the only place the matrix is stored, as
 * GetTeamUnitCount already does). The GetAIInfo synced-handle branch (SYNCED_*)
 * is unreachable -- ShouldServe rejects synced handles.
 ******************************************************************************/

// mirror of LuaSyncedRead::GetTeamStartPosition
int LuaSnapshotServe::GetTeamStartPosition(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);
	const Pov pov = HandlePov(L);

	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const float3& pos = rows.startPos[teamID];
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	lua_pushnumber(L, pos.z);
	lua_pushboolean(L, rows.hasValidStartPos[teamID]);
	return 4;
}

// mirror of LuaSyncedRead::GetAllyTeamStartBox (corners pre-computed at
// extraction in the live float-expression order, so the push is bit-identical)
int LuaSnapshotServe::GetAllyTeamStartBox(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const unsigned int allyTeamID = luaL_checkint(L, 1);

	if (!rows.ValidAllyTeam(static_cast<int>(allyTeamID)))
		return 0;

	const float4& box = rows.allyStartBox[allyTeamID];
	lua_pushnumber(L, box.x);
	lua_pushnumber(L, box.y);
	lua_pushnumber(L, box.z);
	lua_pushnumber(L, box.w);
	return 4;
}

// mirror of LuaSyncedRead::GetMapStartPositions (immutable map data, cached)
int LuaSnapshotServe::GetMapStartPositions(lua_State* L, const char* caller)
{
	const int n = simSnapshot.MapStartPosCount();

	lua_createtable(L, n, 0);
	for (int teamNum = 0; teamNum < n; ++teamNum) {
		if (!simSnapshot.MapStartPosValid(teamNum))
			continue;

		const float3 pos = simSnapshot.MapStartPos(teamNum);
		lua_createtable(L, 3, 0);
		lua_pushnumber(L, pos.x); lua_rawseti(L, -2, 1);
		lua_pushnumber(L, pos.y); lua_rawseti(L, -2, 2);
		lua_pushnumber(L, pos.z); lua_rawseti(L, -2, 3);
		lua_rawseti(L, -2, teamNum); // [i] = {x,y,z}
	}

	return 1;
}

// mirror of LuaSyncedRead::GetTeamMaxUnits (2nd value's GetNumUnits() ==
// unitHandler.NumUnitsByTeam == the numUnits row, tracked in lockstep)
int LuaSnapshotServe::GetTeamMaxUnits(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);
	const Pov pov = HandlePov(L);

	lua_pushnumber(L, rows.maxUnits[teamID]);

	if (rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead))
		lua_pushnumber(L, rows.numUnits[teamID]);
	else
		lua_pushnil(L);

	return 2;
}

// mirror of LuaSyncedRead::GetTeamLuaAI (no alliance gate)
int LuaSnapshotServe::GetTeamLuaAI(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);

	if (rows.hasLuaAI[teamID] == 0)
		return 0;

	lua_pushsstring(L, rows.luaAIName[teamID]);
	return 1;
}

// mirror of LuaSyncedRead::GetAIInfo (returns 0 on invalid team, no error;
// the synced-handle SYNCED_* branch is unreachable in a served twin)
int LuaSnapshotServe::GetAIInfo(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();

	int numVals = 0;

	const int teamId = luaL_checkint(L, 1);
	if (!rows.ValidTeam(teamId))
		return numVals;

	if (rows.aiHasAI[teamId] == 0)
		return numVals;

	// synced AI info
	lua_pushnumber(L, rows.aiID[teamId]);
	lua_pushsstring(L, rows.aiName[teamId]);
	lua_pushnumber(L, rows.aiHostPlayer[teamId]);
	numVals += 3;

	if (rows.aiIsLocal[teamId] != 0) {
		lua_pushsstring(L, rows.aiShortName[teamId]);
		lua_pushsstring(L, rows.aiVersion[teamId]);
		PushOptsTable(L, rows.aiOptions[teamId]);
	} else {
		HSTR_PUSH(L, "UNKNOWN");
		HSTR_PUSH(L, "UNKNOWN");
		lua_newtable(L);
	}
	numVals += 3;

	return numVals;
}

// mirror of LuaSyncedRead::GetAllyTeamInfo
int LuaSnapshotServe::GetAllyTeamInfo(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const size_t allyteam = (size_t)luaL_checkint(L, -1);

	if (!rows.ValidAllyTeam(static_cast<int>(allyteam)))
		return 0;

	PushOptsTable(L, rows.allyTeamOpts[allyteam]);
	return 1;
}

// mirror of LuaSyncedRead::AreTeamsAllied (arg order quirk preserved: teamId1
// reads slot -1, teamId2 slot -2). AlliedTeams(a,b) == Ally(AllyTeam a, AllyTeam b)
int LuaSnapshotServe::AreTeamsAllied(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamId1 = (int)luaL_checkint(L, -1);
	const int teamId2 = (int)luaL_checkint(L, -2);

	if (!rows.ValidTeam(teamId1) || !rows.ValidTeam(teamId2))
		return 0;

	lua_pushboolean(L, simSnapshot.Read().Allied(rows.allyTeam[teamId1], rows.allyTeam[teamId2]));
	return 1;
}

// mirror of LuaSyncedRead::ArePlayersAllied (same arg-order quirk)
int LuaSnapshotServe::ArePlayersAllied(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadPlayers();
	const auto& trows = simSnapshot.ReadTeams();
	const int player1 = luaL_checkint(L, -1);
	const int player2 = luaL_checkint(L, -2);

	if (!rows.ValidPlayer(player1) || !rows.ValidPlayer(player2))
		return 0;

	if (IsPlayerUnsyncedMirror(L, rows, player1) || IsPlayerUnsyncedMirror(L, rows, player2))
		return 0;

	const int t1 = rows.team[player1];
	const int t2 = rows.team[player2];
	const int at1 = trows.ValidTeam(t1) ? trows.allyTeam[t1] : -1;
	const int at2 = trows.ValidTeam(t2) ? trows.allyTeam[t2] : -1;

	lua_pushboolean(L, simSnapshot.Read().Allied(at1, at2));
	return 1;
}

// mirror of LuaSyncedRead::GetPlayerControlledUnit
int LuaSnapshotServe::GetPlayerControlledUnit(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadPlayers();
	const int playerID = luaL_checkint(L, 1);

	if (!rows.ValidPlayer(playerID))
		return 0;

	if (IsPlayerUnsyncedMirror(L, rows, playerID))
		return 0;

	const int controlleeID = rows.controlleeID[playerID];
	if (controlleeID < 0) // no controllee (GetControllee() == nullptr)
		return 0;

	const int readAllyTeam = CLuaHandle::GetHandleReadAllyTeam(L);
	if ((readAllyTeam == CEventClient::NoAccessTeam) ||
	    ((readAllyTeam >= 0) && !simSnapshot.Read().Allied(rows.controlleeAllyTeam[playerID], readAllyTeam))) {
		return 0;
	}

	lua_pushnumber(L, controlleeID);
	return 1;
}

// mirror of LuaSyncedRead::GetTeamStatsHistory (full statHistory copy; the last
// entry's frame/time override reads the snapshot's luaSimFrame)
int LuaSnapshotServe::GetTeamStatsHistory(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadTeams();
	const int teamID = ParseTeamIDSynced(L, caller, 1, rows);

	if (game == nullptr)
		return 0;

	const Pov pov = HandlePov(L);
	if (!rows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead) && rows.gameOver == 0)
		return 0;

	const int args = lua_gettop(L);
	const std::vector<TeamStatistics>& teamStats = rows.statHistory[teamID];

	if (args == 1) {
		lua_pushnumber(L, teamStats.size());
		return 1;
	}

	const int statCount = static_cast<int>(teamStats.size());

	int start = 0;
	if ((args >= 2) && lua_isnumber(L, 2)) {
		start = lua_toint(L, 2) - 1;
		start = std::max(0, std::min(statCount - 1, start));
	}

	int end = start;
	if ((args >= 3) && lua_isnumber(L, 3)) {
		end = lua_toint(L, 3) - 1;
		end = std::max(0, std::min(statCount - 1, end));
	}

	const int luaSimFrame = simSnapshot.ReadGlobals().luaSimFrame;

	lua_createtable(L, std::max(0, end - start), 0);
	if (statCount > 0) {
		int count = 1;
		for (int i = start; i <= end; ++i) {
			const TeamStatistics& stats = teamStats[i];
			lua_createtable(L, 0, 21); {
				if (i + 1 == statCount) {
					// the most recent entry's frame lies in the future; match the
					// live path by reporting the current (snapshot) frame instead
					HSTR_PUSH_NUMBER(L, "time",         luaSimFrame / GAME_SPEED);
					HSTR_PUSH_NUMBER(L, "frame",        luaSimFrame);
				} else {
					HSTR_PUSH_NUMBER(L, "time",         stats.frame / GAME_SPEED);
					HSTR_PUSH_NUMBER(L, "frame",        stats.frame);
				}

				HSTR_PUSH_NUMBER(L, "metalUsed",        stats.metalUsed);
				HSTR_PUSH_NUMBER(L, "metalProduced",    stats.metalProduced);
				HSTR_PUSH_NUMBER(L, "metalExcess",      stats.metalExcess);
				HSTR_PUSH_NUMBER(L, "metalReceived",    stats.metalReceived);
				HSTR_PUSH_NUMBER(L, "metalSent",        stats.metalSent);

				HSTR_PUSH_NUMBER(L, "energyUsed",       stats.energyUsed);
				HSTR_PUSH_NUMBER(L, "energyProduced",   stats.energyProduced);
				HSTR_PUSH_NUMBER(L, "energyExcess",     stats.energyExcess);
				HSTR_PUSH_NUMBER(L, "energyReceived",   stats.energyReceived);
				HSTR_PUSH_NUMBER(L, "energySent",       stats.energySent);

				HSTR_PUSH_NUMBER(L, "damageDealt",      stats.damageDealt);
				HSTR_PUSH_NUMBER(L, "damageReceived",   stats.damageReceived);

				HSTR_PUSH_NUMBER(L, "unitsProduced",    stats.unitsProduced);
				HSTR_PUSH_NUMBER(L, "unitsDied",        stats.unitsDied);
				HSTR_PUSH_NUMBER(L, "unitsReceived",    stats.unitsReceived);
				HSTR_PUSH_NUMBER(L, "unitsSent",        stats.unitsSent);
				HSTR_PUSH_NUMBER(L, "unitsCaptured",    stats.unitsCaptured);
				HSTR_PUSH_NUMBER(L, "unitsOutCaptured", stats.unitsOutCaptured);
				HSTR_PUSH_NUMBER(L, "unitsKilled",      stats.unitsKilled);
			}
			lua_rawseti(L, -2, count++);
		}
	}

	return 1;
}

// mirror of LuaUnsyncedRead::GetPlayerStatistics
int LuaSnapshotServe::GetPlayerStatistics(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadPlayers();
	const int playerID = luaL_checkint(L, 1);

	if (!rows.ValidPlayer(playerID))
		return 0;

	const PlayerStatistics& pStats = rows.currentStats[playerID];

	lua_pushnumber(L, pStats.mousePixels);
	lua_pushnumber(L, pStats.mouseClicks);
	lua_pushnumber(L, pStats.keyPresses);
	lua_pushnumber(L, pStats.numCommands);
	lua_pushnumber(L, pStats.unitCommands);

	return 5;
}


/******************************************************************************/
//
//  PR 38 (zero-sanction flip): game+team rules-params serving
//
//  Served from SimSnapshot::GlobalRows::gameRulesParams (singleton) and
//  TeamRows::teamRulesParams (per-team). The two mirror helpers below are
//  byte-equivalent to LuaSyncedRead.cpp's file-static PushRulesParams /
//  GetRulesParam (which are not visible outside that TU) -- keep them in
//  lockstep with the live helpers. The game twins have no POV (always
//  PRIVATE_MASK); the team twins reproduce the live losMask decision over the
//  snapshot alliance rows (see the per-twin comments).
//

namespace {
	// byte-equivalent mirror of LuaSyncedRead.cpp::PushRulesParams
	int PushRulesParamsMirror(lua_State* L, const LuaRulesParams::Params& params, const int losStatus)
	{
		lua_createtable(L, 0, params.size());

		for (const auto& it: params) {
			const std::string& name = it.first;
			const LuaRulesParams::Param& param = it.second;
			if (!(param.los & losStatus))
				continue;

			std::visit ([L, &name](auto&& value) {
				using T = std::decay_t <decltype(value)>;
				if constexpr (std::is_same_v <T, float>)
					LuaPushNamedNumber(L, name, value);
				else if constexpr (std::is_same_v <T, bool>)
					LuaPushNamedBool(L, name, value);
				else if constexpr (std::is_same_v <T, std::string>)
					LuaPushNamedString(L, name, value);
			}, param.value);
		}

		return 1;
	}

	// byte-equivalent mirror of LuaSyncedRead.cpp::GetRulesParam
	int GetRulesParamMirror(lua_State* L, int index, const LuaRulesParams::Params& params, const int losStatus)
	{
		const std::string& key = luaL_checkstring(L, index);
		const auto it = params.find(key);
		if (it == params.end())
			return 0;

		const LuaRulesParams::Param& param = it->second;
		if (!(param.los & losStatus))
			return 0;

		std::visit ([L](auto&& value) {
			using T = std::decay_t <decltype(value)>;
			if constexpr (std::is_same_v <T, float>)
				lua_pushnumber(L, value);
			else if constexpr (std::is_same_v <T, bool>)
				lua_pushboolean(L, value);
			else if constexpr (std::is_same_v <T, std::string>)
				lua_pushsstring(L, value);
		}, param.value);

		return 1;
	}

	// mirror of LuaSyncedRead::GetTeamRulesParam(s) losMask computation:
	//   PUBLIC, |= PRIVATE_MASK if IsAlliedTeam || gameOver,
	//   else |= ALLIED_MASK if teamHandler.AlliedTeams(teamNum, readTeam).
	// PovAlliedTeam mirrors LuaUtils::IsAlliedTeam; the AlliedTeams leg is
	// reproduced over the snapshot alliance matrix (UnitRows::Allied) + the
	// per-team allyTeam rows. The AlliedTeams branch is only reached when
	// PovAlliedTeam is false, which for a served (unsynced draw-context) handle
	// implies readAllyTeam >= 0 and thus a valid readTeam -- the ValidTeam guard's
	// false fallback is defensive (a degenerate/negative readTeam that master would
	// index out of the fixed teams[] array); enumerated as an equivalence
	// assumption in the commit message.
	int TeamRulesLosMask(lua_State* L, const SimSnapshot::TeamRows& trows, int teamID)
	{
		const Pov pov = HandlePov(L);
		int losMask = LuaRulesParams::RULESPARAMLOS_PUBLIC;

		if (trows.PovAlliedTeam(teamID, pov.readAllyTeam, pov.fullRead) || trows.gameOver != 0) {
			losMask |= LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK;
		} else {
			const int readTeam = CLuaHandle::GetHandleReadTeam(L);
			if (trows.ValidTeam(readTeam) &&
			    simSnapshot.Read().Allied(trows.allyTeam[teamID], trows.allyTeam[readTeam])) {
				losMask |= LuaRulesParams::RULESPARAMLOS_ALLIED_MASK;
			}
		}

		return losMask;
	}
}

// mirror of LuaSyncedRead::GetGameRulesParams (always readable for all)
int LuaSnapshotServe::GetGameRulesParams(lua_State* L, const char* caller)
{
	return PushRulesParamsMirror(L, simSnapshot.ReadGlobals().gameRulesParams,
		LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK);
}

// mirror of LuaSyncedRead::GetGameRulesParam
int LuaSnapshotServe::GetGameRulesParam(lua_State* L, const char* caller)
{
	return GetRulesParamMirror(L, 1, simSnapshot.ReadGlobals().gameRulesParams,
		LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK);
}

// mirror of LuaSyncedRead::GetTeamRulesParams
int LuaSnapshotServe::GetTeamRulesParams(lua_State* L, const char* caller)
{
	const auto& trows = simSnapshot.ReadTeams();

	// live: ParseTeam(1) -> null-check + game null-check. ParseTeam returns null
	// ONLY on its DenyLiveRead branch; for an out-of-range teamID (split off) it
	// raises luaL_error("Bad teamID"). Mirror that: an invalid teamID raises here
	// too (not a silent nil), else a widget calling GetTeamRulesParams(badID)
	// would get nil under flag-ON where master raises. The game==null case stays a
	// live "return 0".
	const int teamID = luaL_checkint(L, 1);
	if (!trows.ValidTeam(teamID))
		luaL_error(L, "Bad teamID in %s\n", caller);
	if (game == nullptr)
		return 0;

	return PushRulesParamsMirror(L, trows.teamRulesParams[teamID], TeamRulesLosMask(L, trows, teamID));
}

// mirror of LuaSyncedRead::GetTeamRulesParam
int LuaSnapshotServe::GetTeamRulesParam(lua_State* L, const char* caller)
{
	const auto& trows = simSnapshot.ReadTeams();

	// See GetTeamRulesParams: mirror live ParseTeam's luaL_error on an invalid
	// teamID rather than returning a silent nil; game==null stays "return 0".
	const int teamID = luaL_checkint(L, 1);
	if (!trows.ValidTeam(teamID))
		luaL_error(L, "Bad teamID in %s\n", caller);
	if (game == nullptr)
		return 0;

	return GetRulesParamMirror(L, 2, trows.teamRulesParams[teamID], TeamRulesLosMask(L, trows, teamID));
}


/******************************************************************************/
//
//  PR 38c (zero-sanction flip): player/unit/feature rules-params serving
//
//  Extends PR 38 part 1's game+team mechanism to the per-object namespaces.
//  Served from SimSnapshot::PlayerRows::playerRulesParams / UnitRows::
//  unitRulesParams / FeatureRows::featureRulesParams (plain per-boundary full
//  copies of CPlayer/CUnit/CFeature::modParams). Each twin reproduces its live
//  callout's exact POV/losMask decision AND its Parse* visibility gate over the
//  snapshot rows; the shared Push/GetRulesParamMirror helpers (above) do the
//  byte-equivalent table/value push. Keep in lockstep with LuaSyncedRead.cpp's
//  GetPlayerRulesParam(s) / GetUnitRulesParam(s) / GetFeatureRulesParam(s).
//

namespace {
	// mirror of LuaSyncedRead::GetPlayerRulesParam(s) losMask computation:
	//   synced handle -> PRIVATE; else own-player / fullRead / gameOver ->
	//   PRIVATE; else PUBLIC. (Player private params are readable only by that
	//   player, so there is no ALLIED leg -- unlike team/unit/feature.)
	int PlayerRulesLosMask(lua_State* L, int playerID)
	{
		if (CLuaHandle::GetHandleSynced(L))
			return LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK;
		// gu->myPlayerNum is unsynced/draw-owned; gameOver read from the mirror
		if (playerID == gu->myPlayerNum || CLuaHandle::GetHandleFullRead(L) ||
		    simSnapshot.ReadTeams().gameOver != 0)
			return LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK;
		return LuaRulesParams::RULESPARAMLOS_PUBLIC_MASK;
	}

	// mirror of LuaSyncedRead::GetUnitRulesParamLosMask:
	//   IsAllyUnit || gameOver -> PRIVATE; AlliedTeams(unit->team, readTeam) ->
	//   ALLIED; readAllyTeam < 0 -> PUBLIC; else the losStatus ladder
	//   (INLOS -> INLOS, PREVLOS|CONTRADAR -> TYPED, INRADAR -> INRADAR, else
	//   PUBLIC). IsAllyUnit is PovAlliedUnit; AlliedTeams(unit->team, readTeam)
	//   == Allied(unit->allyteam, teams.allyTeam[readTeam]) over the snapshot
	//   alliance matrix + per-team allyTeam rows. The ValidTeam(readTeam) guard's
	//   false fallback is defensive (a degenerate/negative readTeam that master
	//   would index out of the fixed teams[] array); enumerated as an equivalence
	//   assumption. Caller has already gated Valid(unitID) + PovUnitVisible.
	int UnitRulesLosMask(lua_State* L, const SimSnapshot::UnitRows& rows, int unitID, const Pov& pov)
	{
		const auto& trows = simSnapshot.ReadTeams();

		if (rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead) || trows.gameOver != 0)
			return LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK;

		const int readTeam = CLuaHandle::GetHandleReadTeam(L);
		if (trows.ValidTeam(readTeam) &&
		    rows.Allied(rows.AllyTeam(unitID), trows.allyTeam[readTeam]))
			return LuaRulesParams::RULESPARAMLOS_ALLIED_MASK;

		if (pov.readAllyTeam < 0)
			return LuaRulesParams::RULESPARAMLOS_PUBLIC_MASK;

		const uint8_t losStatus = rows.LosStatus(unitID, pov.readAllyTeam);
		if (losStatus & LOS_INLOS)
			return LuaRulesParams::RULESPARAMLOS_INLOS_MASK;
		if (losStatus & (LOS_PREVLOS | LOS_CONTRADAR))
			return LuaRulesParams::RULESPARAMLOS_TYPED_MASK;
		if (losStatus & LOS_INRADAR)
			return LuaRulesParams::RULESPARAMLOS_INRADAR_MASK;

		return LuaRulesParams::RULESPARAMLOS_PUBLIC_MASK;
	}

	// mirror of LuaSyncedRead::GetFeatureRulesParam(s) losMask chain:
	//   PUBLIC, then |= PRIVATE if IsAlliedAllyTeam(feature->allyteam) || gameOver;
	//   else if AlliedTeams(feature->team, readTeam) |= ALLIED; else if
	//   readAllyTeam < 0 no-access; else if IsFeatureVisible |= INLOS. The
	//   AlliedTeams inputs (featTeam/readTeam) are pure reads, computed up-front;
	//   the ValidTeam guards are the same defensive equivalence assumption as the
	//   unit/team mask (a feature with team/allyteam outside the fixed range that
	//   master would index out of teams[]). Caller has already gated Valid +
	//   PovFeatureVisible.
	int FeatureRulesLosMask(lua_State* L, const SimSnapshot::FeatureRows& rows, int featureID, const Pov& pov)
	{
		const auto& trows = simSnapshot.ReadTeams();
		int losMask = LuaRulesParams::RULESPARAMLOS_PUBLIC_MASK;

		const int featAllyTeam = rows.AllyTeam(featureID);
		const int featTeam = rows.Team(featureID);
		const int readTeam = CLuaHandle::GetHandleReadTeam(L);
		// IsAlliedAllyTeam(L, feature->allyteam)
		const bool alliedAlly = (pov.readAllyTeam < 0) ? pov.fullRead : (featAllyTeam == pov.readAllyTeam);

		if (alliedAlly || trows.gameOver != 0) {
			losMask |= LuaRulesParams::RULESPARAMLOS_PRIVATE_MASK;
		} else if (trows.ValidTeam(featTeam) && trows.ValidTeam(readTeam) &&
		           simSnapshot.Read().Allied(trows.allyTeam[featTeam], trows.allyTeam[readTeam])) {
			losMask |= LuaRulesParams::RULESPARAMLOS_ALLIED_MASK;
		} else if (pov.readAllyTeam < 0) {
			//! NoAccessTeam
		} else if (PovFeatureVisible(rows, featureID, pov)) {
			losMask |= LuaRulesParams::RULESPARAMLOS_INLOS_MASK;
		}

		return losMask;
	}
}

// mirror of LuaSyncedRead::GetPlayerRulesParams
int LuaSnapshotServe::GetPlayerRulesParams(lua_State* L, const char* caller)
{
	const auto& prows = simSnapshot.ReadPlayers();

	// live: luaL_checkint(1) -> IsValidPlayer -> Player(id) null-check ->
	// IsPlayerUnsynced. Player(validID) is never null, so the null-check is inert.
	const int playerID = luaL_checkint(L, 1);
	if (!prows.ValidPlayer(playerID) || IsPlayerUnsyncedMirror(L, prows, playerID))
		return 0;

	return PushRulesParamsMirror(L, prows.playerRulesParams[playerID], PlayerRulesLosMask(L, playerID));
}

// mirror of LuaSyncedRead::GetPlayerRulesParam
int LuaSnapshotServe::GetPlayerRulesParam(lua_State* L, const char* caller)
{
	const auto& prows = simSnapshot.ReadPlayers();

	const int playerID = luaL_checkint(L, 1);
	if (!prows.ValidPlayer(playerID) || IsPlayerUnsyncedMirror(L, prows, playerID))
		return 0;

	return GetRulesParamMirror(L, 2, prows.playerRulesParams[playerID], PlayerRulesLosMask(L, playerID));
}

// mirror of LuaSyncedRead::GetUnitRulesParams
int LuaSnapshotServe::GetUnitRulesParams(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();

	// live: ParseUnit(1) = ParseRawUnit (number check + id resolve) + the
	// IsUnitVisible vistest; then a game==nullptr guard. The number check +
	// error text is ParseUnitIDSynced; validity/vistest from the rows.
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead) || game == nullptr)
		return 0;

	return PushRulesParamsMirror(L, rows.unitRulesParams[unitID], UnitRulesLosMask(L, rows, unitID, pov));
}

// mirror of LuaSyncedRead::GetUnitRulesParam
int LuaSnapshotServe::GetUnitRulesParam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();

	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead) || game == nullptr)
		return 0;

	return GetRulesParamMirror(L, 2, rows.unitRulesParams[unitID], UnitRulesLosMask(L, rows, unitID, pov));
}

// mirror of LuaSyncedRead::GetFeatureRulesParams
int LuaSnapshotServe::GetFeatureRulesParams(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();

	// live: ParseFeature(1) = number check + id resolve + IsFeatureVisible
	// vistest (no game null-check, unlike units/teams).
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	return PushRulesParamsMirror(L, rows.featureRulesParams[featureID], FeatureRulesLosMask(L, rows, featureID, pov));
}

// mirror of LuaSyncedRead::GetFeatureRulesParam
int LuaSnapshotServe::GetFeatureRulesParam(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();

	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	return GetRulesParamMirror(L, 2, rows.featureRulesParams[featureID], FeatureRulesLosMask(L, rows, featureID, pov));
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
		uint64_t generation = 0; // PR 43 §2.5: EpochId key (u64, monotonic per game)
		bool built = false;

		std::vector<int> allIDs;                                             // every valid id
		std::vector<std::vector<int>> idsByTeam;                             // [teamID]
		std::vector<spring::unordered_map<int, std::vector<int>>> idsByTeamAndDef; // [teamID][defID]
	};
	TeamUnitIndex teamUnitIndex;

	const TeamUnitIndex& GetTeamUnitIndex()
	{
		TeamUnitIndex& idx = teamUnitIndex;
		const uint64_t gen = simSnapshot.HeldEpochId();

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
			if (urows.valid[id] != SimSnapshotValid::ACTIVE)
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

	// ---- PR 34 (spatial/list remainder) helpers ----

	// mirror of LuaSyncedRead's file-local GetUnitTableCentroid: iterate the arg
	// table, ParseUnit each entry (ParseRawUnit number-check + error, then the
	// visibility gate), accumulate the raw midPos row. indexWithinTable is -1
	// (array value) or -2 (map key), exactly as the live callers pass it.
	int GetUnitTableCentroidSnap(lua_State* L, int indexWithinTable, const char* caller)
	{
		if (!lua_istable(L, 1))
			luaL_error(L, "[%s] argument must be a table", caller);

		const auto& rows = simSnapshot.Read();
		const Pov pov = HandlePov(L);

		float3 center {0.0f, 0.0f, 0.0f};
		size_t count = 0;
		for (lua_pushnil(L); lua_next(L, 1); lua_pop(L, 1)) {
			// ParseUnit mirror at indexWithinTable: ParseRawUnit's number check
			// (same error text) then the ParseUnit visibility gate
			const int unitID = ParseUnitIDSynced(L, caller, indexWithinTable);

			if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
				continue;

			center += rows.midPos[unitID];
			++count;
		}

		if (!count)
			return 0;

		center /= static_cast<float>(count);

		lua_pushnumber(L, center.x);
		lua_pushnumber(L, center.y);
		lua_pushnumber(L, center.z);

		return 3;
	}

	// mirror of LuaUnsyncedRead.cpp's file-local PushUnitListSortedByDef: the
	// only live read is unitHandler.GetUnit(id)->unitDef->id, replaced by the
	// UnitRows defID row. The id container is draw-owned (selection / group),
	// iterated in the same order as the live path, so the result table is
	// order-exact (not an id-set deviation).
	//
	// DEVIATION sub-case (defID-0 staleness, review-appended at integration):
	// the draw-owned selection/group id set and the published snapshot skew by
	// up to one boundary under the running split. If the set names a unitID
	// whose snapshot row is stale/absent (a unit selected-then-died, or one
	// spawned+selected after the last boundary), rows.DefID(unitID) returns the
	// stale/nil default 0 instead of the true unitDef->id, so that id lands in
	// def bucket 0 rather than its real def bucket (and the sparse tally counts
	// it under key 0). Pre-split this cannot occur (the draw window sees no
	// intervening sim frame, so the set and the snapshot agree); the armed
	// dual-run compares against the live path at the same boundary, so it is
	// not flagged. The live path derefs GetUnit(id)->unitDef directly, which for
	// a live-but-uncaptured id would read the true def -- hence the documented
	// skew. No sync/masking impact: def-0 grouping is a draw-side ordering
	// artifact of the boundary skew, not a synced-state divergence.
	template <typename T>
	size_t PushUnitListSortedByDefSnap(lua_State* L, const SimSnapshot::UnitRows& rows, const T& units)
	{
		std::map<int, std::vector<int>> unitsByDef;

		for (const auto unitID: units)
			unitsByDef[rows.DefID(unitID)].push_back(unitID);

		lua_createtable(L, 0, unitsByDef.size());

		for (const auto& [unitDefID, unitIDs]: unitsByDef) {
			lua_createtable(L, unitIDs.size(), 0);
			for (size_t i = 0; i < unitIDs.size(); ++i) {
				lua_pushnumber(L, unitIDs[i]);
				lua_rawseti(L, -2, i + 1);
			}
			lua_rawseti(L, -2, unitDefID);
		}

		return unitsByDef.size();
	}

	// mirror of LuaUnsyncedRead.cpp's file-local PushSparseUnitTallyByDef
	// (unitDef->id -> the UnitRows defID row)
	template <typename T>
	size_t PushSparseUnitTallyByDefSnap(lua_State* L, const SimSnapshot::UnitRows& rows, const T& v)
	{
		std::vector<size_t> counts(unitDefHandler->NumUnitDefs() + 1, 0);
		size_t numDefKeys = 0;
		for (const int unitID: v)
			if (!counts[rows.DefID(unitID)]++)
				numDefKeys++;

		lua_createtable(L, 0, numDefKeys);
		for (size_t i = 0; i < counts.size(); ++i) {
			if (counts[i] == 0)
				continue;

			lua_pushnumber(L, counts[i]);
			lua_rawseti(L, -2, i);
		}

		return numDefKeys;
	}

	// mirror of LuaUnsyncedRead.cpp's file-local GetGroupFromArg: uiGroupHandlers
	// and CGroup are draw-owned UI state, so this is a pointer-free read of
	// draw-side data (no sim-owned table touched)
	inline const CGroup* GetGroupFromArgSnap(lua_State* L, int arg)
	{
		const int groupID = luaL_checkint(L, arg);
		const auto& groupHandler = uiGroupHandlers[gu->myTeam];

		if (!groupHandler.HasGroup(groupID))
			return nullptr;

		return groupHandler.GetGroup(groupID);
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
		 * order); the armed dual-run compares this callout as an ID set.
		 *
		 * §4.8 (Race 2): this served twin runs on the DRAW thread under the
		 * split; the global unsynced guRNG is concurrently RMW'd by sim-thread
		 * particle/CEG spawns, a data race on one PCG32 state (UB, though the
		 * order is only cosmetic anti-leak). Use a draw-owned unsynced stream
		 * instead. Inert flag-off: this twin never runs (the Live
		 * GetTeamUnitsByDefs on the main thread is used); the default PCG32
		 * state is a valid stream (no seed needed), and the callout is compared
		 * order-insensitively. */
		static CGlobalUnsyncedRNG drawUnsyncedRNG;
		spring::random_shuffle(unitIDs.begin() + lastOfsset, unitIDs.end(), drawUnsyncedRNG);
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
		if (rows.valid[projID] != SimSnapshotValid::ACTIVE)
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
 * Spatial remainder (PR 39, Wave 6).
 ******************************************************************************/

namespace {
	// LuaSyncedRead's file-local Plane / UnitInPlanes, copied verbatim (immutable
	// geometry; the float expression order is the bit-equality gate). Kept local
	// so the twin is a line-by-line mirror of the live body.
	struct SnapPlane {
		float x, y, z, d;  // ax + by + cz + d = 0
	};

	static inline bool SnapUnitInPlanes(const float3& pos, const float radius, const std::vector<SnapPlane>& planes)
	{
		for (const SnapPlane& p: planes) {
			const float dist = (pos.x * p.x) + (pos.y * p.y) + (pos.z * p.z) + p.d;
			if ((dist - radius) > 0.0f) {
				return false; // outside
			}
		}
		return true;
	}
}

// mirror of LuaSyncedRead::GetUnitsInPlanes.
//
// The live body loops teams [startTeam, endTeam] and, per team, calls
// GetFilteredUnits over unitHandler.GetUnitsByTeam(team) writing into ONE shared
// result table with a per-team counter that RESETS to 0 each team -- so a later
// team's ids overwrite the low array slots of an earlier team's (master's known
// counter-reset overwrite quirk). This twin reproduces that structure exactly:
// GetFilteredUnitsSnap also resets its count per call and writes into the shared
// table. Per the binding Batch-1 amendment ruling, per-team iteration is
// ascending snapshot-id (idx.idsByTeam), NOT unitHandler.GetUnitsByTeam order.
//
// DEVIATION (ruled): single-team queries (allegiance >= 0 / MyUnits) are
// set-identical to master (only within-team order differs). Multi-team queries
// (AllUnits / AllyUnits / EnemyUnits over >1 team) can differ in the overwrite
// tail -- which units survive the overwrite depends on within-team order, so the
// final multiset is not order-invariant. This is the documented deviation the
// ruling accepts; the armed dual-run set-compares (CompareTablesAsIdSet), which
// passes single-team and may flag the multi-team overwrite tail.
int LuaSnapshotServe::GetUnitsInPlanes(lua_State* L, const char* caller)
{
	if (!lua_istable(L, 1)) {
		luaL_error(L, "Incorrect arguments to GetUnitsInPlanes()");
	}

	// parse the planes (identical to the live body, incl. the lua_gettop(L)
	// table index quirk it uses)
	std::vector<SnapPlane> planes;
	const int table = lua_gettop(L);
	for (lua_pushnil(L); lua_next(L, table) != 0; lua_pop(L, 1)) {
		if (lua_istable(L, -1)) {
			float values[4];
			const int v = LuaUtils::ParseFloatArray(L, -1, values, 4);
			if (v == 4) {
				SnapPlane plane = { values[0], values[1], values[2], values[3] };
				planes.push_back(plane);
			}
		}
	}

	const auto& rows = simSnapshot.Read();
	const auto& trows = simSnapshot.ReadTeams();
	const TeamUnitIndex& idx = GetTeamUnitIndex();
	const Pov pov = HandlePov(L);

	int startTeam, endTeam;

	const int allegiance = ParseAllegianceMirror(L, caller, 2);
	if (allegiance >= 0) {
		startTeam = allegiance;
		endTeam = allegiance;
	}
	else if (allegiance == LuaUtils::MyUnits) {
		const int readTeam = CLuaHandle::GetHandleReadTeam(L);
		startTeam = readTeam;
		endTeam = readTeam;
	}
	else {
		startTeam = 0;
		endTeam = trows.activeTeams - 1;
	}

	const auto planesTest = [&](int unitID, const float3 &pos) {
		return SnapUnitInPlanes(pos, rows.radius[unitID], planes);
	};

	static const std::vector<int> emptyIDs;

	lua_newtable(L);

	for (int team = startTeam; team <= endTeam; team++) {
		// LuaUtils::IsAlliedTeam mirror (TeamRows::PovAlliedTeam)
		if (allegiance == LuaUtils::AllyUnits && !trows.PovAlliedTeam(team, pov.readAllyTeam, pov.fullRead))
			continue;
		if (allegiance == LuaUtils::EnemyUnits && trows.PovAlliedTeam(team, pov.readAllyTeam, pov.fullRead))
			continue;

		// unitHandler.GetUnitsByTeam(team) mirror; empty for out-of-range teams
		const std::vector<int>& units =
			(team >= 0 && team < trows.activeTeams) ? idx.idsByTeam[team] : emptyIDs;

		GetFilteredUnitsSnap(L, rows, allegiance, units, planesTest);
	}

	return 1;
}


/******************************************************************************
 * Frustum / screen-rect / nearest spatial family (PR 40, Wave 6).
 *
 * The five frustum/screen-rect twins (GetVisibleUnits/Features, Get{Units,
 * Features}InScreenRectangle) are served through a draw-side per-quad
 * object-membership MIRROR keyed IDENTICALLY to CQuadField -- same
 * numQuadsX/numQuadsZ geometry, same BASE_QUAD_SIZE cells, same
 * GetQuads(pos,radius) DISC membership (NOT SnapshotPickGrid, which inserts by
 * selVol extent -> a conservative superset, correct for cursor-picking but
 * WRONG for exact visible-set membership). The mirror is rebuilt lazily per
 * boundary (per snapshot generation) from the gate-verified pos/radius rows:
 * unit/feature id -> quadField.GetQuads(pos,radius) == the object's live
 * unit->quads / feature quad-set (MovedUnit / AddFeature use exactly those
 * inputs), so the mirror reproduces the live quadfield membership bit-for-bit
 * (faithful by construction). Each twin then walks the mirror with the SAME
 * readMap->GridVisibility a snapshot-backed IQuadDrawer, re-applying the live
 * body's exact filters over the snapshot rows + the id-keyed drawer draw-pos +
 * the camera.
 *
 * The two nearest scalars (GetUnitNearestAlly/Enemy) reproduce CGameHelper's
 * closest-unit search over the SnapshotPickGrid radius query + the snapshot
 * rows (PR 25's GetClosestFriendlyUnit(synced=false) is the direct precedent
 * for the ally variant; the enemy variant applies the Enemy / Enemy_InLos
 * filters + the InLos/Cylinder distance tests over the same rows). RULED
 * (Batch-4): the ascending-snapshot-id tie-break on exactly-equal distances is
 * an accepted advisory-UI deviation.
 *
 * DOCUMENTED DEVIATION: within-quad result order is ascending id, not the live
 * quadfield insertion order; the sets are identical (CompareTablesAsIdSet).
 ******************************************************************************/

namespace {
	// per-quad object-membership mirror (see the section header). Rebuilt lazily
	// on generation change from the front UnitRows/FeatureRows.
	struct QuadMembership {
		uint64_t generation = 0; // PR 43 §2.5: EpochId key (u64, monotonic per game)
		bool built = false;
		int numQuadsX = 0;
		int numQuadsZ = 0;
		std::vector<std::vector<int>> unitCells;    // [z * numQuadsX + x], == CQuadField cell index
		std::vector<std::vector<int>> featureCells; // same indexing
	};
	QuadMembership quadMembership;

	const QuadMembership& GetQuadMembership()
	{
		QuadMembership& m = quadMembership;
		const uint64_t gen = simSnapshot.HeldEpochId();
		if (m.built && m.generation == gen)
			return m;

		// PR 43: enumeration/spatial twins include only ACTIVE rows, NOT
		// DEAD_THIS_BATCH -- that state is a POINT-read mechanism (rows.Valid()
		// serves a held id's at-death state during the dispatch window), while a
		// dead unit is gone from master's spatial containers. These indices are
		// also cached per epoch, so admitting a DEAD_THIS_BATCH row (which only
		// exists during the window) would keep a corpse in the mirror past the
		// window close. Same rule at every `valid[id] != ACTIVE` scan here.

		// quadField geometry is immutable after map load (reads are const, no
		// mutable-membership touch); numQuadsX/numQuadsZ match GetQuadAt(x, y)
		m.numQuadsX = quadField.GetNumQuadsX();
		m.numQuadsZ = quadField.GetNumQuadsZ();
		const size_t numCells = static_cast<size_t>(m.numQuadsX) * m.numQuadsZ;

		m.unitCells.resize(numCells);
		m.featureCells.resize(numCells);
		for (auto& c: m.unitCells) c.clear();
		for (auto& c: m.featureCells) c.clear();

		{
			const auto& urows = simSnapshot.Read();
			for (size_t id = 0; id < urows.MaxUnits(); ++id) {
				if (urows.valid[id] != SimSnapshotValid::ACTIVE)
					continue;
				// membership == CQuadField::MovedUnit's GetQuads(unit->pos,
				// unit->radius); the scratch slot is the main-split slot under the
				// split (DefaultQuadFieldQueryOwner), so it cannot collide with the
				// sim thread's slot-0 queries
				QuadFieldQuery qfq;
				quadField.GetQuads(qfq, urows.pos[id], urows.radius[id]);
				for (const int qi: *qfq.quads)
					m.unitCells[qi].push_back(static_cast<int>(id));
			}
		}
		{
			const auto& frows = simSnapshot.ReadFeatures();
			for (size_t id = 0; id < frows.MaxSlots(); ++id) {
				if (frows.valid[id] != SimSnapshotValid::ACTIVE)
					continue;
				// membership == CQuadField::AddFeature's GetQuads(feature->pos,
				// feature->radius)
				QuadFieldQuery qfq;
				quadField.GetQuads(qfq, frows.pos[id], frows.radius[id]);
				for (const int qi: *qfq.quads)
					m.featureCells[qi].push_back(static_cast<int>(id));
			}
		}

		m.built = true;
		m.generation = gen;
		return m;
	}

	// snapshot-backed IQuadDrawer: mirrors CVisUnitQuadDrawer/CVisFeatureQuadDrawer
	// but collects the membership mirror's per-quad id lists instead of the live
	// quadField's per-quad object lists (identical numQuadsX*y + x cell index).
	struct SnapVisQuadDrawer: public CReadMap::IQuadDrawer {
		const std::vector<std::vector<int>>* cells = nullptr;
		int numQuadsX = 0;
		std::vector<const std::vector<int>*> lists;

		void ResetState() override { lists.clear(); lists.reserve(64); }
		void DrawQuad(int x, int y) override {
			const std::vector<int>& l = (*cells)[static_cast<size_t>(numQuadsX) * y + x];
			if (!l.empty())
				lists.push_back(&l);
		}
	};

	// walk the GridVisibility-visited quads of `cells` and return the deduped set
	// of reachable ids (each unique id once), mirroring the live GridVisibility +
	// unsyncedTempNum dedup. Reused scratch (single-threaded draw context; the
	// twins call no Lua between collecting and iterating, so no reentrancy).
	const std::vector<int>& CollectVisibleMembership(
		const std::vector<std::vector<int>>& cells, int numQuadsX, size_t maxIDs)
	{
		static SnapVisQuadDrawer drawer;
		static std::vector<int> out;
		static std::vector<uint32_t> seenStamp;
		static uint32_t seenQuery = 0;

		drawer.cells = &cells;
		drawer.numQuadsX = numQuadsX;
		drawer.ResetState();
		readMap->GridVisibility(nullptr, &drawer, 1e9, CQuadField::BASE_QUAD_SIZE / SQUARE_SIZE);

		if (seenStamp.size() < maxIDs)
			seenStamp.resize(maxIDs, 0);
		if (++seenQuery == 0) { // stamp wrap (astronomically rare)
			std::fill(seenStamp.begin(), seenStamp.end(), 0);
			seenQuery = 1;
		}

		out.clear();
		for (const std::vector<int>* list: drawer.lists) {
			for (const int id: *list) {
				if (static_cast<size_t>(id) >= seenStamp.size())
					continue;
				if (seenStamp[id] == seenQuery)
					continue;
				seenStamp[id] = seenQuery;
				out.push_back(id);
			}
		}
		return out;
	}

	// PR 41: per-quad SYNCED-projectile membership mirror, keyed IDENTICALLY to
	// CQuadField. Per projectile the cell set mirrors CQuadField::AddProjectile
	// EXACTLY -- hitscan -> GetQuadsOnRay(pos, dir, speed.w) (a RAY); non-hitscan ->
	// the SINGLE cell WorldPosToQuadFieldIdx(pos) -- NOT the GetQuads(pos,radius)
	// disc the unit/feature mirror uses. This reproduces the live p->quads
	// bit-for-bit (MovedProjectile resyncs non-hitscan membership to the current pos
	// every sim frame; hitscan pos/dir/speed.w are immutable after the ctor's
	// AddProjectile), so walking it yields the same set the live
	// CVisProjectileQuadDrawer collects from baseQuads[].projectiles. Built lazily
	// per boundary (on snapshot-generation change), separate from the unit/feature
	// mirror so the GetVisibleUnits/Features twins pay nothing for it.
	struct ProjQuadMembership {
		uint64_t generation = 0; // PR 43 §2.5: EpochId key (u64, monotonic per game)
		bool built = false;
		int numQuadsX = 0;
		int numQuadsZ = 0;
		std::vector<std::vector<int>> cells; // [z * numQuadsX + x], == CQuadField cell index
	};
	ProjQuadMembership projQuadMembership;

	// CQuadField::WorldPosToQuadFieldIdx is private; reproduce it EXACTLY from the
	// public geometry getters (same int/float promotion, clamp, row-major index) so
	// the non-hitscan single-cell membership is bit-identical to AddProjectile's.
	static inline int SnapWorldPosToQuadFieldIdx(const float3& p)
	{
		const int qsx = quadField.GetQuadSizeX();
		const int qsz = quadField.GetQuadSizeZ();
		const int nqx = quadField.GetNumQuadsX();
		const int nqz = quadField.GetNumQuadsZ();
		return std::clamp(int(p.z / qsz), 0, nqz - 1) * nqx + std::clamp(int(p.x / qsx), 0, nqx - 1);
	}

	const ProjQuadMembership& GetProjQuadMembership()
	{
		ProjQuadMembership& m = projQuadMembership;
		const uint64_t gen = simSnapshot.HeldEpochId();
		if (m.built && m.generation == gen)
			return m;

		// quadField geometry is immutable after map load; numQuadsX/numQuadsZ match
		// GetQuadAt(x, y) (the same indexing the live projQuadIter walks)
		m.numQuadsX = quadField.GetNumQuadsX();
		m.numQuadsZ = quadField.GetNumQuadsZ();
		const size_t numCells = static_cast<size_t>(m.numQuadsX) * m.numQuadsZ;

		m.cells.resize(numCells);
		for (auto& c: m.cells) c.clear();

		const auto& prows = simSnapshot.ReadProjectiles();
		for (size_t id = 0; id < prows.MaxSlots(); ++id) {
			if (prows.valid[id] != SimSnapshotValid::ACTIVE)
				continue;

			// == CQuadField::AddProjectile's membership rule. The GetQuadsOnRay
			// scratch slot is the main-split slot (DefaultQuadFieldQueryOwner), so it
			// cannot collide with the sim thread's slot-0 queries (same argument as
			// the PR-40 GetQuads mirror).
			if (prows.hitscan[id]) {
				QuadFieldQuery qfq;
				quadField.GetQuadsOnRay(qfq, prows.pos[id], prows.dir[id], prows.speed[id].w);
				for (const int qi: *qfq.quads)
					m.cells[qi].push_back(static_cast<int>(id));
			} else {
				const int qi = SnapWorldPosToQuadFieldIdx(prows.pos[id]);
				m.cells[qi].push_back(static_cast<int>(id));
			}
		}

		m.built = true;
		m.generation = gen;
		return m;
	}
}


// mirror of LuaUnsyncedRead::GetVisibleUnits. Walks the per-quad membership
// mirror; noDraw is read through the sanctioned draw-side resolver (unsynced
// object flag, same as GetUnitNoDraw), losStatus/team/allyteam from the rows,
// icon/draw-midpos/draw-radius from the id-keyed drawer accessors.
int LuaSnapshotServe::GetVisibleUnits(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const auto& trows = simSnapshot.ReadTeams();

	// arg 1 - teamID
	int teamID = luaL_optint(L, 1, -1);
	int allyTeamID = CLuaHandle::GetHandleReadAllyTeam(L);

	if (teamID == LuaUtils::MyUnits) {
		const int scriptTeamID = CLuaHandle::GetHandleReadTeam(L);

		if (scriptTeamID >= 0) {
			teamID = scriptTeamID;
		} else {
			teamID = LuaUtils::AllUnits;
		}
	}

	if (teamID >= 0) {
		if (!trows.ValidTeam(teamID)) // teamHandler.IsValidTeam mirror
			return 0;

		allyTeamID = trows.allyTeam[teamID]; // teamHandler.AllyTeam mirror
	}
	if (allyTeamID < 0) {
		if (!CLuaHandle::GetHandleFullRead(L)) {
			return 0;
		}
	}

	// arg 3 - noIcons
	const bool noIcons = !luaL_optboolean(L, 3, true);

	float radiusMult = 1.0f;
	float testRadius = 0.0f;

	// arg 2 - use fixed test-value or add unit radii to it
	if (lua_israwnumber(L, 2)) {
		radiusMult = float((testRadius = lua_tofloat(L, 2)) >= 0.0f);
		testRadius = std::max(testRadius, -testRadius);
	}

	const QuadMembership& mem = GetQuadMembership();
	const std::vector<int>& ids = CollectVisibleMembership(mem.unitCells, mem.numQuadsX, rows.MaxUnits());

	lua_createtable(L, ids.size(), 0);

	unsigned int count = 0;
	for (const int unitID: ids) {
		// u->noDraw: unsynced draw-owned flag, resolved through the drawer
		// boundary cache / died-in-burst shell (never the sim handler)
		const CUnit* unit = ResolveDrawUnit(unitID);
		if (unit == nullptr)
			continue;

		if (unit->noDraw)
			continue;

		if (allyTeamID >= 0 && !(rows.LosStatus(unitID, allyTeamID) & LOS_INLOS))
			continue;

		if (noIcons && CUnitDrawer::GetIsIcon(unitID))
			continue;

		if ((teamID == LuaUtils::AllyUnits)  && (allyTeamID != rows.allyTeam[unitID]))
			continue;

		if ((teamID == LuaUtils::EnemyUnits) && (allyTeamID == rows.allyTeam[unitID]))
			continue;

		if ((teamID >= 0) && (teamID != rows.team[unitID]))
			continue;

		if (!camera->InView(CUnitDrawer::GetDrawMidPos(unitID), testRadius + (CUnitDrawer::GetDrawRadius(unitID) * radiusMult)))
			continue;

		lua_pushnumber(L, unitID);
		lua_rawseti(L, -2, ++count);
	}

	return 1;
}


// mirror of LuaUnsyncedRead::GetVisibleFeatures. noDraw + def->geoThermal from
// the sanctioned feature resolver; draw-flag/draw-midpos/draw-radius id-keyed;
// visibility from the FeatureRows IsInLosForAllyTeam mirror.
int LuaSnapshotServe::GetVisibleFeatures(lua_State* L, const char* caller)
{
	const auto& trows = simSnapshot.ReadTeams();
	const auto& frows = simSnapshot.ReadFeatures();

	// arg 1 - allyTeamID
	int allyTeamID = luaL_optint(L, 1, -1);

	if (allyTeamID >= 0) {
		if (!trows.ValidAllyTeam(allyTeamID)) { // teamHandler.ValidAllyTeam mirror
			return 0;
		}
	} else {
		allyTeamID = -1;

		if (!CLuaHandle::GetHandleFullRead(L)) {
			allyTeamID = CLuaHandle::GetHandleReadAllyTeam(L);
		}
	}

	const bool noIcons = !luaL_optboolean(L, 3, true);
	const bool noGeos = !luaL_optboolean(L, 4, true);

	float radiusMult = 0.0f; // 0 or 1
	float testRadius = 0.0f;

	// arg 2 - use fixed test-value or add feature radii to it
	if (lua_israwnumber(L, 2)) {
		radiusMult = float((testRadius = lua_tofloat(L, 2)) >= 0.0f);
		testRadius = std::max(testRadius, -testRadius);
	}

	const QuadMembership& mem = GetQuadMembership();
	const std::vector<int>& ids = CollectVisibleMembership(mem.featureCells, mem.numQuadsX, frows.MaxSlots());

	lua_createtable(L, ids.size(), 0);

	unsigned int count = 0;
	for (const int featureID: ids) {
		// f->noDraw + f->def->geoThermal: unsynced flag + immutable def, read
		// through the sanctioned feature resolver (drawer cache / shell)
		const CFeature* feature = ResolveDrawFeature(featureID);
		if (feature == nullptr)
			continue;

		if (feature->noDraw)
			continue;

		if (noIcons && CFeatureDrawer::GetDrawFlag(featureID) == DrawFlags::SO_DRICON_FLAG)
			continue;

		if (noGeos && feature->def->geoThermal)
			continue;

		if (!gu->spectatingFullView && !frows.IsInLosForAllyTeam(featureID, allyTeamID))
			continue;

		if (!camera->InView(CFeatureDrawer::GetDrawMidPos(featureID), testRadius + (CFeatureDrawer::GetDrawRadius(featureID) * radiusMult)))
			continue;

		lua_pushnumber(L, featureID);
		lua_rawseti(L, -2, ++count);
	}

	return 1;
}


// mirror of LuaUnsyncedRead::GetVisibleProjectiles (PR 41). Walks the per-quad
// SYNCED-projectile membership mirror (hitscan ray / non-hitscan single cell ==
// the live CQuadField::AddProjectile membership), re-applying the live body's
// filters over the ProjectileRows: the CWorldObject-overload LOS answer
// (visInLosAll, distinct from the positional inLosAll), the draw-cull
// camera->InView(pos, drawRadius), and the weapon/piece toggles. The live
// `!p->synced` filter is a no-op here -- ProjectileRows holds ONLY synced
// projectiles (CQuadField::AddProjectile asserts p->synced), which is exactly the
// set the live quadfield walk yields -- so it is omitted, not approximated.
int LuaSnapshotServe::GetVisibleProjectiles(lua_State* L, const char* caller)
{
	const auto& trows = simSnapshot.ReadTeams();
	const auto& prows = simSnapshot.ReadProjectiles();

	int allyTeamID = luaL_optint(L, 1, -1);

	if (allyTeamID >= 0) {
		if (!trows.ValidAllyTeam(allyTeamID)) { // teamHandler.ValidAllyTeam mirror
			return 0;
		}
	} else {
		allyTeamID = -1;

		if (!CLuaHandle::GetHandleFullRead(L)) {
			allyTeamID = CLuaHandle::GetHandleReadAllyTeam(L);
		}
	}

	/*const bool addSyncedProjectiles =*/ luaL_optboolean(L, 2, true);
	const bool addWeaponProjectiles = luaL_optboolean(L, 3, true);
	const bool addPieceProjectiles = luaL_optboolean(L, 4, true);

	const ProjQuadMembership& mem = GetProjQuadMembership();
	const std::vector<int>& ids = CollectVisibleMembership(mem.cells, mem.numQuadsX, prows.MaxSlots());

	lua_createtable(L, ids.size(), 0);

	unsigned int count = 0;
	for (const int projID: ids) {
		if (allyTeamID >= 0 && !prows.VisInLos(projID, allyTeamID))
			continue;

		if (!camera->InView(prows.pos[projID], prows.drawRadius[projID]))
			continue;

		// live `if (!p->synced) continue;` -- always false here (see the header note)

		if (!addWeaponProjectiles && prows.isWeapon[projID])
			continue;

		if (!addPieceProjectiles && prows.isPiece[projID])
			continue;

		lua_pushnumber(L, projID);
		lua_rawseti(L, -2, ++count);
	}

	return 1;
}


// mirror of LuaUnsyncedRead::GetUnitsInScreenRectangle. Same membership mirror +
// camera screen-projection (CalcViewPortCoordinates over the id-keyed draw-pos);
// the LuaUtils::IsUnitVisible filter maps to UnitRows::PovUnitVisible.
int LuaSnapshotServe::GetUnitsInScreenRectangle(lua_State* L, const char* caller)
{
	float l = luaL_checkfloat(L, 1);
	float t = luaL_checkfloat(L, 2);
	float r = luaL_checkfloat(L, 3);
	float b = luaL_checkfloat(L, 4);

	if (l > r) std::swap(l, r);
	if (t > b) std::swap(t, b);

	const auto& rows = simSnapshot.Read();
	const Pov pov = HandlePov(L);

	const int readTeam = CLuaHandle::GetHandleReadTeam(L);
	const int readATeam = CLuaHandle::GetHandleReadAllyTeam(L);

	const int allegiance = ParseAllegianceMirror(L, caller, 5);

	const QuadMembership& mem = GetQuadMembership();
	const std::vector<int>& ids = CollectVisibleMembership(mem.unitCells, mem.numQuadsX, rows.MaxUnits());

	lua_createtable(L, ids.size(), 0);

	// LuaUtils::IsUnitVisible mirror (== UnitRows::PovUnitVisible); ids come from
	// valid membership cells so Valid() holds
	const auto unitVisible = [&](int unitID) {
		return rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead);
	};

	const auto runLoop = [&](auto disqualifier) {
		uint32_t count = 0;
		for (const int unitID: ids) {
			if (disqualifier(unitID))
				continue;

			const float3 vpPos = camera->CalcViewPortCoordinates(CUnitDrawer::GetDrawPos(unitID));

			if (vpPos.x > r || vpPos.x < l)
				continue;

			if (vpPos.y > b || vpPos.y < t)
				continue;

			if (vpPos.z > 1.0f || vpPos.z < 0.0f)
				continue;

			lua_pushnumber(L, unitID);
			lua_rawseti(L, -2, ++count);
		}
	};

	switch (allegiance) {
		case LuaUtils::AllUnits:
			runLoop([&](int uid) { return !unitVisible(uid); });
			break;
		case LuaUtils::MyUnits:
			runLoop([&](int uid) { return rows.team[uid] != readTeam || !unitVisible(uid); });
			break;
		case LuaUtils::AllyUnits:
			runLoop([&](int uid) { return rows.allyTeam[uid] != readATeam || !unitVisible(uid); });
			break;
		case LuaUtils::EnemyUnits:
			runLoop([&](int uid) { return rows.allyTeam[uid] == readATeam || !unitVisible(uid); });
			break;
		default:
			runLoop([&](int uid) { return rows.team[uid] != allegiance || !unitVisible(uid); });
			break;
	}

	return 1;
}


// mirror of LuaUnsyncedRead::GetFeaturesInScreenRectangle (no visibility filter;
// just the membership mirror + camera screen-projection over the id-keyed
// feature draw-pos).
int LuaSnapshotServe::GetFeaturesInScreenRectangle(lua_State* L, const char* caller)
{
	float l = luaL_checkfloat(L, 1);
	float t = luaL_checkfloat(L, 2);
	float r = luaL_checkfloat(L, 3);
	float b = luaL_checkfloat(L, 4);

	if (l > r) std::swap(l, r);
	if (t > b) std::swap(t, b);

	const auto& frows = simSnapshot.ReadFeatures();

	const QuadMembership& mem = GetQuadMembership();
	const std::vector<int>& ids = CollectVisibleMembership(mem.featureCells, mem.numQuadsX, frows.MaxSlots());

	lua_createtable(L, ids.size(), 0);

	uint32_t count = 0;
	for (const int featureID: ids) {
		const float3 vpPos = camera->CalcViewPortCoordinates(CFeatureDrawer::GetDrawPos(featureID));

		if (vpPos.x > r || vpPos.x < l)
			continue;

		if (vpPos.y > b || vpPos.y < t)
			continue;

		if (vpPos.z > 1.0f || vpPos.z < 0.0f)
			continue;

		lua_pushnumber(L, featureID);
		lua_rawseti(L, -2, ++count);
	}

	return 1;
}


// mirror of LuaSyncedRead::GetUnitNearestAlly. Reproduces
// CGameHelper::GetClosestFriendlyUnit(synced=false) EXACTLY (PR 25's landed
// snapshot search): the SnapshotPickGrid radius query + Filter::Friendly
// (allied-to-searchAllyteam, excludeUnit==self) + Query::ClosestUnit (raw
// midPos 2D distance). The ascending-snapshot-id tie-break is the blessed
// Batch-4 advisory-UI deviation.
int LuaSnapshotServe::GetUnitNearestAlly(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit gate mirror (ParseRawUnit validity + LuaUtils::IsAllyUnit)
	if (!rows.Valid(unitID))
		return 0;
	if (!rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const float range = luaL_optnumber(L, 2, 1.0e9f);
	const float3 pos = rows.pos[unitID];
	const int searchAllyteam = rows.allyTeam[unitID];

	static std::vector<int> cand;
	snapshotPickGrid.QueryUnitsInRadius(pos, range, cand);

	float closeSqDist = range * range;
	int closeID = -1;

	for (const int id: cand) {
		if (id == unitID) // excludeUnit
			continue;
		if (!rows.Allied(searchAllyteam, rows.AllyTeam(id))) // Filter::Friendly::Team
			continue;

		const float sqDist = (pos - rows.MidPos(id)).SqLength2D();
		if (sqDist <= closeSqDist) {
			closeSqDist = sqDist;
			closeID = id;
		}
	}

	if (closeID >= 0) {
		lua_pushnumber(L, closeID);
		return 1;
	}
	return 0;
}


// mirror of LuaSyncedRead::GetUnitNearestEnemy. Reproduces
// CGameHelper::GetClosestEnemyUnit (Filter::Enemy_InLos + Query::ClosestUnit)
// and GetClosestEnemyUnitNoLosTest (Filter::Enemy + ClosestUnit_InLos /
// ClosestUnit_InLos_Cylinder) over the SnapshotPickGrid radius query + rows.
// The pick grid's selVol-extent insertion is a correct superset for both the
// raw-midPos and the radius-touching accept tests; the exact accept test
// narrows. Blessed ascending-id tie-break (Batch-4).
int LuaSnapshotServe::GetUnitNearestEnemy(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit gate mirror
	if (!rows.Valid(unitID))
		return 0;
	if (!rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const bool wantLOS = !lua_isboolean(L, 3) || lua_toboolean(L, 3);
	const bool testLOS = !CLuaHandle::GetHandleFullRead(L) || wantLOS;

	const bool sphereDistTest = luaL_optboolean(L, 4, false);
	const bool checkSightDist = luaL_optboolean(L, 5, false);

	const float range = luaL_optnumber(L, 2, 1.0e9f);
	const float3 pos = rows.pos[unitID];
	const int searchAllyteam = rows.allyTeam[unitID];

	static std::vector<int> cand;
	snapshotPickGrid.QueryUnitsInRadius(pos, range, cand);

	int closeID = -1;

	if (testLOS) {
		// GetClosestEnemyUnit: Filter::Enemy_InLos + Query::ClosestUnit
		float closeSqDist = range * range;
		for (const int id: cand) {
			if (id == unitID) // excludeUnit
				continue;
			if (rows.Neutral(id)) // Filter::Enemy::Unit (!IsNeutral)
				continue;
			if (rows.Allied(searchAllyteam, rows.AllyTeam(id))) // Filter::Enemy::Team (!Ally)
				continue;
			if ((rows.LosStatus(id, searchAllyteam) & (LOS_INLOS | LOS_INRADAR)) == 0) // Enemy_InLos
				continue;

			const float sqDist = (pos - rows.MidPos(id)).SqLength2D();
			if (sqDist <= closeSqDist) {
				closeSqDist = sqDist;
				closeID = id;
			}
		}
	} else if (sphereDistTest) {
		// GetClosestEnemyUnitNoLosTest sphere: Filter::Enemy + ClosestUnit_InLos
		// (3D distance minus target radius; closeDist init == range)
		float closeDist = range;
		for (const int id: cand) {
			if (id == unitID)
				continue;
			if (rows.Neutral(id))
				continue;
			if (rows.Allied(searchAllyteam, rows.AllyTeam(id)))
				continue;

			const float dist = pos.distance(rows.MidPos(id)) - rows.radius[id];
			if (dist <= closeDist && (!checkSightDist || dist <= rows.losRadius[id])) {
				closeDist = dist;
				closeID = id;
			}
		}
	} else {
		// GetClosestEnemyUnitNoLosTest cylinder: Filter::Enemy +
		// ClosestUnit_InLos_Cylinder (2D distance; closeSqDist init == range^2)
		float closeSqDist = range * range;
		for (const int id: cand) {
			if (id == unitID)
				continue;
			if (rows.Neutral(id))
				continue;
			if (rows.Allied(searchAllyteam, rows.AllyTeam(id)))
				continue;

			const int losR = rows.losRadius[id];
			const float sqDist = (pos - rows.MidPos(id)).SqLength2D();
			if (sqDist <= closeSqDist && (!checkSightDist || sqDist <= float(losR * losR))) {
				closeSqDist = sqDist;
				closeID = id;
			}
		}
	}

	if (closeID >= 0) {
		lua_pushnumber(L, closeID);
		return 1;
	}
	return 0;
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


/******************************************************************************
 * PR 34 (spatial/list remainder) twins. Whole-list projectile/feature twins
 * (ascending-id deviation, id-set compare), table centroids (order-exact, they
 * iterate the same arg table as the live path), and the draw-owned selection/
 * group aggregate twins (defID sort keys from UnitRows; the id containers are
 * draw-owned so the result order matches the live iteration exactly).
 ******************************************************************************/

// mirror of LuaSyncedRead::GetUnitArrayCentroid (GetUnitTableCentroid, value idx)
int LuaSnapshotServe::GetUnitArrayCentroid(lua_State* L, const char* caller)
{
	return GetUnitTableCentroidSnap(L, -1, caller);
}

// mirror of LuaSyncedRead::GetUnitMapCentroid (GetUnitTableCentroid, key idx)
int LuaSnapshotServe::GetUnitMapCentroid(lua_State* L, const char* caller)
{
	return GetUnitTableCentroidSnap(L, -2, caller);
}

// mirror of LuaSyncedRead::GetAllProjectiles: whole synced-projectile list (only
// synced projectiles have rows, so the live !pro->synced skip is by construction)
int LuaSnapshotServe::GetAllProjectiles(lua_State* L, const char* caller)
{
	const bool excludeWeaponProjectiles = luaL_optboolean(L, 1, false);
	const bool excludePieceProjectiles  = luaL_optboolean(L, 2, false);

	const auto& rows = simSnapshot.ReadProjectiles();

	// DEVIATION: ascending ids (master lists the active-projectile container order)
	sqObjectIDs.clear();
	for (size_t projID = 0; projID < rows.MaxSlots(); ++projID) {
		if (rows.valid[projID] != SimSnapshotValid::ACTIVE)
			continue;
		sqObjectIDs.push_back(static_cast<int>(projID));
	}

	GetProjectilesLuaTableSnap(L, rows, sqObjectIDs, excludeWeaponProjectiles, excludePieceProjectiles);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectilesInSphere: linear scan (no projectile
// grid rows) with the exact CQuadField::GetProjectilesExact(pos, radius) filter
// pos.SqDistance(p->pos) >= Square(radius + p->radius) -- the p->radius input is
// the PR-34 ProjectileRows::radius row
int LuaSnapshotServe::GetProjectilesInSphere(lua_State* L, const char* caller)
{
	const float3 sphereCenter(luaL_checkfloat(L, 1), luaL_checkfloat(L, 2), luaL_checkfloat(L, 3));
	const float radius = luaL_checkfloat(L, 4);

	const bool excludeWeaponProjectiles = luaL_optboolean(L, 5, false);
	const bool excludePieceProjectiles = luaL_optboolean(L, 6, false);

	const auto& rows = simSnapshot.ReadProjectiles();

	sqObjectIDs.clear();

	for (size_t projID = 0; projID < rows.MaxSlots(); ++projID) {
		if (rows.valid[projID] != SimSnapshotValid::ACTIVE)
			continue;

		const float totRad = radius + rows.radius[projID];
		if (sphereCenter.SqDistance(rows.pos[projID]) >= (totRad * totRad))
			continue;

		sqObjectIDs.push_back(static_cast<int>(projID));
	}

	GetProjectilesLuaTableSnap(L, rows, sqObjectIDs, excludeWeaponProjectiles, excludePieceProjectiles);
	return 1;
}

// mirror of LuaSyncedRead::GetAllFeatures (NB its own fullRead/IsFeatureVisible
// branch structure, distinct from ProcessFeatures' readAllyTeam<0 shape)
int LuaSnapshotServe::GetAllFeatures(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const Pov pov = HandlePov(L);

	// DEVIATION: ascending ids (master lists featureHandler's active-id order)
	lua_createtable(L, rows.MaxSlots(), 0);

	int count = 0;
	if (pov.fullRead) {
		for (size_t id = 0; id < rows.MaxSlots(); ++id) {
			if (rows.valid[id] != SimSnapshotValid::ACTIVE)
				continue;
			lua_pushnumber(L, static_cast<int>(id));
			lua_rawseti(L, -2, ++count);
		}
	} else {
		for (size_t id = 0; id < rows.MaxSlots(); ++id) {
			if (rows.valid[id] != SimSnapshotValid::ACTIVE)
				continue;
			if (!PovFeatureVisible(rows, static_cast<int>(id), pov))
				continue;
			lua_pushnumber(L, static_cast<int>(id));
			lua_rawseti(L, -2, ++count);
		}
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

		// PR 38 (Batch-1 amendment a, cmd_guard_remove class): the unit's allyteam
		// at capture. Used only when serving an event-time override for a unit that
		// died in the same batch it got the command (its snapshot row is invalid, so
		// the row's allyteam is gone) -- the ParseAllyUnitCmdSlot ally gate masks on
		// this captured (fire-time) value instead, reproducing master's synchronous
		// mid-sim dispatch. Unused by the normal (live-row) serving path.
		int allyTeam = -1;

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

		// sim|draw WS-5: lastSelectedCommandPage is no longer snapshotted here --
		// its only writers are main-thread (CSelectedUnitsHandler::GetAvailableCommands
		// + the CommandAI ctors), never the sim, so GetServedAvailableCommands reads
		// it live off commandAI on the main thread (race-free, removes the one
		// unversioned field with no choke to ride; design §4.5).
	};

	// indexed by unitID, sized unitHandler.MaxUnits() at first refresh.
	// PR 44a: TRUE PER-SLOT COPIES keyed by the SimSnapshot epoch-ring slot
	// (dissolving the PR-43 lockstep deviation where one physical cache
	// tracked the newest epoch): the producer refreshes the slot of the epoch
	// being produced, serving reads the consumer-held slot's copy.
	std::array<std::vector<UnitCmdQueueSlot>, SimSnapshot::EPOCH_RING_SLOTS> cmdQueueCaches;
	// PR 43 §2.5: keyed by the u64 EpochId (was the u32 Generation); per-slot
	std::array<uint64_t, SimSnapshot::EPOCH_RING_SLOTS> cmdQueueCacheEpochs = {};

	// the consumer-held slot's copies -- every serving read goes through this
	std::vector<UnitCmdQueueSlot>& ServedCmdCache() { return cmdQueueCaches[simSnapshot.HeldSlot()]; }

	// sim|draw WS-5: mutation-side dirty-list replacing RefreshCommandQueues'
	// O(maxUnits) scan. The mutation chokes (CCommandQueue::BumpVersion /
	// CCommandAI::BumpCmdDescVersion, plus the explicit unversioned tail chokes)
	// push the affected unit id here; the producer drains only the dirtied ids
	// for the slot it is producing. THE RING SUBTLETY (design §3): a queue
	// dirtied once must reach every one of the EPOCH_RING_SLOTS ring slots, so
	// the push arms ALL slot bits and each produce clears only its own bit --
	// the id stays live until every slot has recopied it.
	//
	// cmdDirtyPending[id]: bit s set = ring slot s still owes a recopy of id.
	// cmdDirtyActive: the ids with any pending bit (the drain domain). Sized/
	// touched only on the sim thread (chokes) and the producer drain (same
	// thread, sim-state-owning) -- no atomics (design §8).
	constexpr uint8_t CMD_ALL_SLOTS_MASK = (1u << SimSnapshot::EPOCH_RING_SLOTS) - 1;
	std::vector<uint8_t> cmdDirtyPending;
	std::vector<int> cmdDirtyActive;
	bool cmdDirtySeeded = false;

	void MarkCmdDirtyImpl(int id)
	{
		// flag-off the producer never drains, so keep the structures untouched
		// (byte-identical, no unbounded growth); the push is inert. The armed
		// diff-gate dual-runs the producer flag-off, so it needs the pushes too.
		if (!SimDrawSplit::Enabled() && !snapshotDiffGate.Armed())
			return;

		const size_t maxUnits = unitHandler.MaxUnits();

		if (id < 0 || static_cast<size_t>(id) >= maxUnits)
			return;

		if (cmdDirtyPending.size() != maxUnits)
			cmdDirtyPending.resize(maxUnits, 0);

		// design §6: dedup -- append to the active vector only on the 0->set
		// edge; an already-pending id just re-arms the mask (no-op if full)
		if (cmdDirtyPending[id] == 0)
			cmdDirtyActive.push_back(id);

		cmdDirtyPending[id] = CMD_ALL_SLOTS_MASK;
	}

	// install the header-inline hook (CommandQueue.h) so every queue/desc bump
	// and the explicit tail chokes route into MarkCmdDirtyImpl; done once at
	// process start (before any unit exists)
	[[maybe_unused]] const bool cmdDirtyHookInstalled = [] {
		CCommandQueue::cmdDirtyHook = &MarkCmdDirtyImpl;
		return true;
	}();

	// PR 38f (event-time command-queue presentation): a per-unit override
	// consulted FIRST by GetCmdQueueSlot. Installed by ScopedCmdQueueEventOverride
	// around a deferred UnitCommand/UnitCmdDone dispatch so the command-queue
	// twins present the event-time queue the sim captured at fire time (see the
	// appended PR-38f section at the end of this file). Main-thread
	// dispatch-window only; nullptr otherwise -> inert, so flag-off and the
	// diff-gate dual-run are byte-identical.
	const UnitCmdQueueSlot* cmdEvtOverrideSlot = nullptr;
	int cmdEvtOverrideUnitID = -1;

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
		// PR 38f: the event-time override wins for the unit whose command event
		// is currently being dispatched (see cmdEvtOverrideSlot)
		if (cmdEvtOverrideSlot != nullptr && unitID == cmdEvtOverrideUnitID)
			return cmdEvtOverrideSlot;

		const std::vector<UnitCmdQueueSlot>& cache = ServedCmdCache();

		if (unitID < 0 || static_cast<size_t>(unitID) >= cache.size())
			return nullptr;

		const UnitCmdQueueSlot& slot = cache[unitID];

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

		if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead)) {
			// PR 38 (Batch-1 amendment a, cmd_guard_remove class): a unit can
			// receive a command in the SAME batch it dies. Master fired
			// UnitCommand/UnitCmdDone synchronously mid-sim while the unit was still
			// alive & ally, so its deferred handler must see the event-time queue.
			// Under the split that handler runs at the barrier after the unit's row
			// went invalid (Valid==0), so the row-based gate above fails. When the
			// event-time override is installed for this unit, serve the captured
			// slot instead of nil-ing -- masking on the CAPTURED (fire-time)
			// allyteam since the dead row has none -- reproducing master's
			// synchronous dispatch. The override is only ever installed during the
			// deferred-dispatch drain (main thread); it is null flag-off and in the
			// armed diff-gate dual-run (immediate dispatch, nothing captured), so
			// this branch is inert there -> byte-identical.
			if (cmdEvtOverrideSlot != nullptr && unitID == cmdEvtOverrideUnitID) {
				// PovAlliedUnit mirror over the captured allyteam
				const bool allied = (pov.readAllyTeam < 0)
					? pov.fullRead
					: (cmdEvtOverrideSlot->allyTeam == pov.readAllyTeam);
				if (allied)
					return cmdEvtOverrideSlot;
			}
			return nullptr;
		}

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


uint64_t LuaSnapshotServe::CmdQueueCacheEpoch() { return cmdQueueCacheEpochs[simSnapshot.HeldSlot()]; }
uint64_t LuaSnapshotServe::CmdQueueCacheEpoch(int slot) { return cmdQueueCacheEpochs[slot]; }

void LuaSnapshotServe::RefreshCommandQueues(int ringSlot, uint64_t targetEpoch)
{
	// PR 46: attribution -- this walk was the largest unzoned span of the
	// epoch producer (Tracy showed it as a multi-ms gap on the sim thread)
	SCOPED_TIMER("Sim::EpochProduce::CmdQueues");

	// producer-only (the flip's sim frame edge, or the lockstep barrier with
	// the sim parked): reads live queues. Epoch-gated so the copies always
	// describe the same boundary as the target slot's rows -- and so the drain
	// is free when nothing was (re)published.
	//
	// sim|draw WS-5: iterate only the ids the mutation chokes dirtied (design
	// §2), not [0, maxUnits). THE RING SUBTLETY (§3): the per-slot cache stores
	// its own last-copied version per unit, so a queue changed once must be
	// recopied into EACH ring slot as the ring rotates. The dirty push arms all
	// slot bits; this drain processes the ids owing a recopy for THIS slot and
	// clears that slot's bit, retiring an id only once every slot has copied it.
	// The per-slot version compare below stays as an idempotency backstop (§3.5):
	// it makes an over-push (e.g. a desc-only or worker-task push whose queue is
	// unchanged) a harmless re-verify no-op.
	const uint64_t gen = targetEpoch;

	if (gen == 0 || gen == cmdQueueCacheEpochs[ringSlot])
		return;

	cmdQueueCacheEpochs[ringSlot] = gen;

	std::vector<UnitCmdQueueSlot>& cmdQueueCache = cmdQueueCaches[ringSlot];
	const size_t maxUnits = unitHandler.MaxUnits();

	if (cmdQueueCache.size() != maxUnits)
		cmdQueueCache.resize(maxUnits);

	// first produce of a game: the creation chokes may predate the split/gate
	// becoming push-eligible (load order), so seed the drain domain with every
	// active unit once; ClearCaches resets the seed for the next game
	if (!cmdDirtySeeded) {
		cmdDirtySeeded = true;
		MarkAllCmdQueuesDirty();
	}

	const uint8_t slotBit = uint8_t(1u << ringSlot);

	// drain: process the ids owing a recopy for this slot, then compact the
	// active vector (drop ids whose pending mask reached 0, keep ids still owing
	// other slots -- the §3.3 swap-pop, expressed as an in-place compaction)
	size_t writePos = 0;
	for (size_t r = 0, n = cmdDirtyActive.size(); r < n; ++r) {
		const int id = cmdDirtyActive[r];
		uint8_t& mask = cmdDirtyPending[id];

		if ((mask & slotBit) != 0) {
			UnitCmdQueueSlot& slot = cmdQueueCache[id];
			const CUnit* unit = unitHandler.GetUnit(id);

			// dead ids must serve the "no such unit" nil shape, never stale copies
			if (unit == nullptr) {
				if (slot.present)
					ClearCmdQueueSlot(slot);
			} else {
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
				// derivable; the transitions push explicitly -- design §4.3)
				ResolveWorkerTask(unit, slot);
			}

			mask &= ~slotBit;
		}

		if (mask != 0)
			cmdDirtyActive[writePos++] = id;
	}

	cmdDirtyActive.resize(writePos);
}


// sim|draw WS-5: post-creg-load / split-enable sweep (design §9). creg rebuilds
// each queue with a fresh version and each cmdDescVersion ctor-fresh but fires
// no dirty push for the loaded units (the ctor path may be bypassed), so seed
// the drain domain with every active unit once. Bounded O(active units); a no-op
// flag-off (MarkCmdDirtyImpl gates on the split).
void LuaSnapshotServe::MarkAllCmdQueuesDirty()
{
	for (const CUnit* unit: unitHandler.GetActiveUnits())
		CCommandQueue::MarkDirty(unit->id);
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

	const std::vector<UnitCmdQueueSlot>& cache = ServedCmdCache();
	const UnitCmdQueueSlot* slot =
		(unitID >= 0 && static_cast<size_t>(unitID) < cache.size() && cache[unitID].present)
			? &cache[unitID] : nullptr;

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

	// sim|draw WS-5: lastSelectedCommandPage is no longer snapshotted (served
	// live off commandAI on the main thread; design §4.5), so this compare is
	// tautological -- kept for the diff-gate field enum's stability
	res.pageOk = true;

	return res;
}


// sim|draw PR 44 (Gap B): served analogue of CCommandAI::GetPossibleCommands() +
// lastSelectedCommandPage for CSelectedUnitsHandler::GetAvailableCommands. Rebuilds
// SCommandDescriptions from the boundary cmd-desc cache (the same fields
// CopyDescsSnap flattened; refCount defaults to 1, as on a fresh desc). Direct
// cache lookup (not GetCmdQueueSlot) -- GetAvailableCommands never runs inside a
// deferred command-event dispatch, so the event-time override is irrelevant here.
bool LuaSnapshotServe::GetServedAvailableCommands(int unitID, std::vector<SCommandDescription>& outDescs, int& outPage)
{
	const std::vector<UnitCmdQueueSlot>& cache = ServedCmdCache();

	if (unitID < 0 || static_cast<size_t>(unitID) >= cache.size())
		return false;

	const UnitCmdQueueSlot& slot = cache[unitID];
	if (!slot.present)
		return false;

	// sim|draw WS-5: lastSelectedCommandPage is read live here (main thread) --
	// its only writers are main-thread (this handler + the CommandAI ctors), so
	// the read is race-free and the field is no longer snapshotted (design §4.5)
	const CUnit* unit = unitHandler.GetUnit(unitID);
	outPage = (unit != nullptr) ? unit->commandAI->lastSelectedCommandPage : 0;

	outDescs.clear();
	outDescs.reserve(slot.descs.size());
	for (const CmdDescRecord& r: slot.descs) {
		SCommandDescription d;
		d.id          = r.id;
		d.type        = r.type;
		d.queueing    = r.queueing;
		d.hidden      = r.hidden;
		d.disabled    = r.disabled;
		d.showUnique  = r.showUnique;
		d.onlyTexture = r.onlyTexture;
		d.name        = r.name;
		d.action      = r.action;
		d.iconname    = r.iconname;
		d.mouseicon   = r.mouseicon;
		d.tooltip     = r.tooltip;
		d.params      = r.params;
		outDescs.push_back(std::move(d));
	}

	return true;
}


// PR 44b: engine-side C++ scan over the served queue copy (see the header;
// CWaitCommandsAI's wait scans). Serves commandQue -- the surface the live
// wait scans walk (unit->commandAI->commandQue) for factories too.
bool LuaSnapshotServe::ForEachServedCommand(int unitID, const std::function<bool(int, int, float, float)>& fn)
{
	const std::vector<UnitCmdQueueSlot>& cache = ServedCmdCache();

	if (unitID < 0 || static_cast<size_t>(unitID) >= cache.size())
		return false;

	const UnitCmdQueueSlot& slot = cache[unitID];
	if (!slot.present)
		return false;

	for (const SnapCommand& c: slot.commandQue) {
		const float p0 = (c.numParams >= 1) ? slot.commandQueParams[c.paramOffset + 0] : 0.0f;
		const float p1 = (c.numParams >= 2) ? slot.commandQueParams[c.paramOffset + 1] : 0.0f;

		if (!fn(c.id, static_cast<int>(c.numParams), p0, p1))
			break;
	}

	return true;
}


std::vector<Command> LuaSnapshotServe::GetServedOverlapQueued(int unitID, const Command& c)
{
	const std::vector<UnitCmdQueueSlot>& cache = ServedCmdCache();

	if (unitID < 0 || static_cast<size_t>(unitID) >= cache.size())
		return {};

	const UnitCmdQueueSlot& slot = cache[unitID];
	if (!slot.present)
		return {};

	// reconstruct the unit's command queue from the barrier cache (id / options /
	// all params -- everything the overlap test reads via GetID/GetNumParams/
	// GetParam/GetOpts/IsInternalOrder + BuildInfo::Parse; tag is not read).
	std::vector<Command> queue;
	queue.reserve(slot.commandQue.size());
	for (const SnapCommand& sc: slot.commandQue) {
		Command cmd(sc.id, sc.options);
		for (unsigned int k = 0; k < sc.numParams; ++k)
			cmd.PushParam(slot.commandQueParams[sc.paramOffset + k]);
		queue.push_back(cmd);
	}

	return CCommandAI::GetOverlapQueued(c, queue);
}


int LuaSnapshotServe::TestUnitBuildSquareUIEpoch(const BuildInfo& buildInfo, const std::vector<Command>& commands,
                                                 std::vector<float3>& canbuildpos, std::vector<float3>& featurepos, std::vector<float3>& nobuildpos)
{
	// mirror the live UI overload's allyteam (gu->myAllyTeam) over the epoch.
	placement::EpochView view;
	return placement::TestUnitBuildSquareUIT(view, buildInfo, gu->myAllyTeam, commands, canbuildpos, featurepos, nobuildpos);
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

	// PR 44a: values behind unique_ptr (pointer-stable across rehash) + a
	// mutex -- the producer (sim thread) inserts on the first object of a
	// model while draw-side serving twins look up concurrently. Insertions
	// are rare (one per model type); lookups take an uncontended lock.
	spring::unordered_map<const void*, std::unique_ptr<ModelPieceMeta>> modelMetaCache;
	std::mutex modelMetaMtx;

	// captured dynamic (per-boundary) piece state, final computed values
	struct PieceDynamic {
		float3 absPos;               // LocalModelPiece::GetAbsolutePos()
		CMatrix44f modelSpaceMat;    // LocalModelPiece::GetModelSpaceMatrix()
		float3 posDirPos;            // GetSolidObjectPiecePosDir's pos (object space)
		float3 posDirDir;            // GetSolidObjectPiecePosDir's dir (object space)
		CollisionVolume pieceColVol; // LocalModelPiece::GetCollisionVolume()
		bool scriptVisible = true;   // LocalModelPiece::GetScriptVisible() (per-piece trace hit-test skip)
	};

	struct ObjectPieceSlot {
		bool present = false;
		// first-touch dead-miss tombstone: the live lookup under the counted
		// park found the object dead/absent; repeat queries of this id in the
		// same held window serve the miss WITHOUT re-parking (a dying-while-
		// queried object otherwise re-parks on every query until the next
		// epoch). Reset alongside `present` by the death-journal drain, and by
		// any successful (re)capture of the slot.
		bool deadMiss = false;
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

		// WS-1 skip key, stored by every successful RefreshObjectPieceSlot: a
		// full key match proves the slot content is bit-identical to what a
		// fresh capture would produce, so the capture is skipped. The version
		// covers all choked piece-tree mutations (LocalModel counter); the
		// value fields cover the un-choked object-level inputs. The object
		// colvol needs no extra storage: memcmp `colVol` above vs the live one.
		uint64_t captureVersion = 0;        // LocalModel::GetPieceTreeVersion(); 0 = never captured
		float3 keyPos;                      // object pos + FULL dir basis: the world-space
		float3 keyFrontdir;                 // emit points (GetObjectSpacePos/Vec) read all
		float3 keyRightdir;                 // three axes, so frontdir alone would skip
		float3 keyUpdir;                    // through a roll with stale emit points
		int32_t keyHitFrame = -1;           // pieceHitFrames[true]
		const void* keyHitPiece = nullptr;  // hitModelPieces[true]
	};

	// indexed by unitID / featureID, sized at first refresh.
	// PR 44a: TRUE PER-SLOT COPIES keyed by the SimSnapshot epoch-ring slot
	// (see the cmd-queue cache note); serving reads the consumer-held slot.
	std::array<std::vector<ObjectPieceSlot>, SimSnapshot::EPOCH_RING_SLOTS> unitPieceCaches;
	std::array<std::vector<ObjectPieceSlot>, SimSnapshot::EPOCH_RING_SLOTS> featurePieceCaches;
	// PR 43 §2.5: keyed by the u64 EpochId (was the u32 Generation); per-slot
	std::array<uint64_t, SimSnapshot::EPOCH_RING_SLOTS> pieceCacheEpochs = {};

	// PR 46: READ-SET-DRIVEN capture (operator-ruled) -- RefreshPieces
	// captures ONLY objects the draw side has actually queried; the eager
	// all-objects capture measured ~4ms/epoch mid-game while stock games
	// query pieces rarely. First touch of a valid-but-unregistered object is
	// served via a counted lazy park + inline live capture into the HELD
	// slot (the SYNCED-mirror first-touch precedent, [SimPauseSurvey] site
	// PIECE_FIRST_TOUCH), and registers the id so the producer captures it
	// at every subsequent edge until the object dies (then pruned; an id
	// reused by a new object re-registers on its own first touch).
	std::vector<uint8_t> unitPieceReadSet;      // [maxUnits]; producer-owned
	std::vector<uint8_t> featurePieceReadSet;   // grow-only; producer-owned
	std::mutex pieceRegMtx;                     // guards the two pending lists
	std::vector<int> pendingUnitPieceRegs;      // draw -> producer mailbox
	std::vector<int> pendingFeaturePieceRegs;

	// WS-1 §5.1: producer-owned death journal, replacing the all-slots
	// present/deadMiss wipe loops. Appended when the producer's capture loop
	// observes a registered id dead (the existing prune branches); each ring
	// slot drains the entries it has not yet applied at the top of its next
	// RefreshPieces, and the journal compacts once every slot has drained it.
	struct PieceDeathEntry {
		int id;
		bool isFeature;
	};
	std::vector<PieceDeathEntry> pieceDeathJournal;
	std::array<size_t, SimSnapshot::EPOCH_RING_SLOTS> pieceDeathDrainCursors = {};

	// WS-6: est-path demand gate -- the pieces read-set model specialized to the
	// GetUnitEstimatedPath waypoint block. Producer captures est-path only for
	// unit ids the draw has queried; first touch of an unregistered valid ground
	// unit serves the LIVE path under a counted park ([SimPauseSurvey] site
	// EST_PATH_FIRST_TOUCH) and enqueues the id. estPathReadSet is producer-owned
	// (drained + consulted on the sim thread); the draw thread only pushes onto
	// pendingEstPathRegs under estPathRegMtx. A dead / non-ground registered id is
	// pruned in the producer sweep (a reused id re-registers via its own first
	// touch). No coarse demand-latch or pre-registration heuristic is needed
	// (est-path has no predictable-first-touch consumer like the nano widget).
	std::vector<uint8_t> estPathReadSet;        // [maxUnits]; producer-owned
	std::mutex estPathRegMtx;                    // guards pendingEstPathRegs
	std::vector<int> pendingEstPathRegs;         // draw -> producer mailbox
	// demand gate for the producer's nano-job pre-registration: stays false
	// until the draw side queries ANY unit piece slot, so setups with no
	// piece-querying addon pay nothing for the pre-registered capture
	std::atomic<bool> unitPieceDemandSeen = {false};

	// build (once) the immutable metadata for o's model from a live LocalModel
	const ModelPieceMeta& GetOrBuildModelMeta(const void* key, const LocalModel& lm)
	{
		{
			std::lock_guard<std::mutex> lock(modelMetaMtx);
			auto it = modelMetaCache.find(key);
			if (it != modelMetaCache.end())
				return *it->second;
		}

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

		std::lock_guard<std::mutex> lock(modelMetaMtx);
		return *modelMetaCache.emplace(key, std::make_unique<ModelPieceMeta>(std::move(meta))).first->second;
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

			if (captureColVol) {
				pd.pieceColVol = *(lmp.GetCollisionVolume());
				pd.scriptVisible = lmp.GetScriptVisible();
			}
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

		// WS-1 §5.2: a dead-missed id can respawn before the next edge with the
		// journal never seeing a death (the edge finds it alive) -- a successful
		// capture must clear the tombstone (the retired wipe used to)
		slot.deadMiss = false;

		// WS-1 §4: store the skip key; the next refresh of this slot skips the
		// whole capture when the key still matches
		slot.captureVersion = lm.GetPieceTreeVersion();
		slot.keyPos = o->pos;
		slot.keyFrontdir = o->frontdir;
		slot.keyRightdir = o->rightdir;
		slot.keyUpdir = o->updir;
		slot.keyHitFrame = o->pieceHitFrames[true];
		slot.keyHitPiece = o->hitModelPieces[true];
	}

	// WS-1 §4: key match => the cached slot is bit-identical to what a fresh
	// capture would produce. captureVersion equality proves both "same
	// LocalModel instance" (instance-unique seed) and "no choked piece-tree
	// mutation since capture"; the value fields cover the object-level inputs
	// with no clean mutation choke (whole-object transform, last-hit pair,
	// object colvol). Exact compares throughout -- any value change recaptures.
	bool PieceSlotKeyMatches(const ObjectPieceSlot& slot, const CSolidObject* o)
	{
		return
			(slot.captureVersion == o->localModel.GetPieceTreeVersion()) &&
			slot.keyPos.same(o->pos) &&
			slot.keyFrontdir.same(o->frontdir) &&
			slot.keyRightdir.same(o->rightdir) &&
			slot.keyUpdir.same(o->updir) &&
			(slot.keyHitFrame == o->pieceHitFrames[true]) &&
			(slot.keyHitPiece == static_cast<const void*>(o->hitModelPieces[true])) &&
			slot.hasColVol &&
			(std::memcmp(&slot.colVol, &o->collisionVolume, sizeof(CollisionVolume)) == 0);
	}

	// WS-1 §6.2 DS oracle (PieceSkipOracle=N): re-run the full capture for a
	// SKIPPED slot into scratch and field-compare against the cache -- converts
	// a bypassed mutation choke (a §4.1 inventory miss) from silent staleness
	// into a named error line. Producer-thread only (static scratch).
	const char* PieceSlotFirstDiff(const ObjectPieceSlot& a, const ObjectPieceSlot& b)
	{
		if (a.metaKey != b.metaKey) return "metaKey";
		if (a.rootPieceIndex != b.rootPieceIndex) return "rootPieceIndex";
		if (a.numPieces != b.numPieces) return "numPieces";
		if (a.pieces.size() != b.pieces.size()) return "pieces.size";

		for (size_t i = 0; i < a.pieces.size(); ++i) {
			const PieceDynamic& pa = a.pieces[i];
			const PieceDynamic& pb = b.pieces[i];

			if (std::memcmp(&pa.absPos, &pb.absPos, sizeof(float3)) != 0) return "absPos";
			if (std::memcmp(&pa.modelSpaceMat, &pb.modelSpaceMat, sizeof(CMatrix44f)) != 0) return "modelSpaceMat";
			if (std::memcmp(&pa.posDirPos, &pb.posDirPos, sizeof(float3)) != 0) return "posDirPos";
			if (std::memcmp(&pa.posDirDir, &pb.posDirDir, sizeof(float3)) != 0) return "posDirDir";
			if (std::memcmp(&pa.pieceColVol, &pb.pieceColVol, sizeof(CollisionVolume)) != 0) return "pieceColVol";
			if (pa.scriptVisible != pb.scriptVisible) return "scriptVisible";
		}

		if (a.scriptToModel != b.scriptToModel) return "scriptToModel";
		if (a.hasColVol != b.hasColVol) return "hasColVol";
		if (std::memcmp(&a.colVol, &b.colVol, sizeof(CollisionVolume)) != 0) return "colVol";
		if (a.lastHitPieceIndex != b.lastHitPieceIndex) return "lastHitPieceIndex";
		if (a.lastHitFrame != b.lastHitFrame) return "lastHitFrame";

		return nullptr;
	}

	void RunPieceSkipOracle(const ObjectPieceSlot& slot, const CSolidObject* o,
	                        bool isFeature, int id, bool captureScript, uint64_t epoch)
	{
		static ObjectPieceSlot scratch;
		RefreshObjectPieceSlot(scratch, o, captureScript, /*colVol*/true, /*lastHit*/true);

		const char* diff = PieceSlotFirstDiff(slot, scratch);
		if (diff == nullptr)
			return;

		LOG_L(L_ERROR, "[PieceSkipOracle] %s id=%d field=%s epoch=%llu (a bumpless mutation path reached served piece state)",
				isFeature ? "feature" : "unit", id, diff, static_cast<unsigned long long>(epoch));
	}

	const ObjectPieceSlot* GetUnitPieceSlot(int unitID)
	{
		std::vector<ObjectPieceSlot>& cache = unitPieceCaches[simSnapshot.HeldSlot()];

		unitPieceDemandSeen.store(true, std::memory_order_relaxed);

		if (unitID < 0 || static_cast<size_t>(unitID) >= unitHandler.MaxUnits())
			return nullptr;

		if (static_cast<size_t>(unitID) < cache.size()) {
			if (cache[unitID].present)
				return &cache[unitID];
			if (cache[unitID].deadMiss)
				return nullptr;
		}

		// PR 46 first touch (draw thread; the Parse* gates validated the id
		// against the held epoch's rows): register for the producer's future
		// edges, then capture live under a counted park to serve THIS query.
		// A null live object means the unit died after the epoch's edge --
		// serve a miss for that dispatch window (enumerated deviation: the
		// eager cache would have served its edge-time capture for one window).
		{
			std::lock_guard<std::mutex> lk(pieceRegMtx);
			pendingUnitPieceRegs.push_back(unitID);
		}

		CGame::ScopedExternalSimPause park{CGame::SimPauseSite::PIECE_FIRST_TOUCH};

		const CUnit* u = unitHandler.GetUnit(unitID);
		if (u == nullptr || u->isDead) {
			if (cache.size() < unitHandler.MaxUnits())
				cache.resize(unitHandler.MaxUnits());
			cache[unitID].deadMiss = true;
			return nullptr;
		}

		if (cache.size() < unitHandler.MaxUnits())
			cache.resize(unitHandler.MaxUnits());

		ObjectPieceSlot& s = cache[unitID];
		RefreshObjectPieceSlot(s, u, /*script*/true, /*colVol*/true, /*lastHit*/true);
		return &s;
	}

	const ObjectPieceSlot* GetFeaturePieceSlot(int featureID)
	{
		std::vector<ObjectPieceSlot>& cache = featurePieceCaches[simSnapshot.HeldSlot()];

		if (featureID < 0)
			return nullptr;

		if (static_cast<size_t>(featureID) < cache.size()) {
			if (cache[featureID].present)
				return &cache[featureID];
			if (cache[featureID].deadMiss)
				return nullptr;
		}

		// PR 46 first touch -- see GetUnitPieceSlot
		{
			std::lock_guard<std::mutex> lk(pieceRegMtx);
			pendingFeaturePieceRegs.push_back(featureID);
		}

		CGame::ScopedExternalSimPause park{CGame::SimPauseSite::PIECE_FIRST_TOUCH};

		const CFeature* f = featureHandler.GetFeature(featureID);
		if (f == nullptr) {
			if (cache.size() <= static_cast<size_t>(featureID))
				cache.resize(featureID + 1);
			cache[featureID].deadMiss = true;
			return nullptr;
		}

		if (cache.size() <= static_cast<size_t>(featureID))
			cache.resize(featureID + 1);

		ObjectPieceSlot& s = cache[featureID];
		RefreshObjectPieceSlot(s, f, /*script*/false, /*colVol*/true, /*lastHit*/true);
		return &s;
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

	// PR 32: ParseInLosUnit mirror (rows.Valid + PovUnitInLos) + unit piece-slot
	// lookup -- GetUnitCollisionVolumeData / GetUnitPieceCollisionVolumeData gate
	const ObjectPieceSlot* ParseInLosUnitPieceSlot(lua_State* L, const char* caller)
	{
		const auto& rows = simSnapshot.Read();
		const int unitID = ParseUnitIDSynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
			return nullptr;

		return GetUnitPieceSlot(unitID);
	}

	// PR 32: ParseAllyUnit mirror (rows.Valid + PovAlliedUnit) + unit piece-slot
	// lookup -- GetUnitLastAttackedPiece gate (batch-1 amendment: PovAlliedUnit)
	const ObjectPieceSlot* ParseAllyUnitPieceSlot(lua_State* L, const char* caller)
	{
		const auto& rows = simSnapshot.Read();
		const int unitID = ParseUnitIDSynced(L, caller, 1);
		const Pov pov = HandlePov(L);

		if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
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
		std::lock_guard<std::mutex> lock(modelMetaMtx);
		auto it = modelMetaCache.find(slot->metaKey);
		return (it != modelMetaCache.end()) ? it->second.get() : nullptr;
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


uint64_t LuaSnapshotServe::PieceCacheEpoch() { return pieceCacheEpochs[simSnapshot.HeldSlot()]; }
uint64_t LuaSnapshotServe::PieceCacheEpoch(int slot) { return pieceCacheEpochs[slot]; }

// PR 43 §2.8 (/epochstats): approximate resident bytes of the cmd-queue and
// piece cache channels (flat-vector payloads; container overhead ignored)
void LuaSnapshotServe::EpochChannelBytes(size_t& cmdQueueBytes, size_t& pieceBytes)
{
	cmdQueueBytes = 0;
	for (const UnitCmdQueueSlot& slot : ServedCmdCache()) {
		cmdQueueBytes += sizeof(slot);
		cmdQueueBytes += slot.commandQue.capacity() * sizeof(slot.commandQue[0]);
		cmdQueueBytes += slot.commandQueParams.capacity() * sizeof(float);
		cmdQueueBytes += slot.newUnitCommands.capacity() * sizeof(slot.commandQue[0]);
		cmdQueueBytes += slot.newUnitCommandsParams.capacity() * sizeof(float);
		for (const auto& d : slot.descs)
			cmdQueueBytes += sizeof(d) + d.params.size() * 24; // rough string payload
	}

	pieceBytes = 0;
	const int heldSlot = simSnapshot.HeldSlot();
	for (const auto* cache : {&unitPieceCaches[heldSlot], &featurePieceCaches[heldSlot]}) {
		for (const ObjectPieceSlot& slot : *cache) {
			pieceBytes += sizeof(slot);
			pieceBytes += slot.pieces.capacity() * sizeof(PieceDynamic);
			pieceBytes += slot.scriptToModel.capacity() * sizeof(int32_t);
		}
	}
}

std::vector<uint8_t>* LuaSnapshotServe::AcquireEstPathReadSet()
{
	// flag-off (no contract, gate unarmed) never reads the est-path rows -- skip
	// the drain + capture entirely (mirrors RefreshPieces' flag-off early-out).
	if (!LuaSplitContract::Enabled() && !snapshotDiffGate.Armed())
		return nullptr;

	const size_t maxUnits = unitHandler.MaxUnits();
	if (estPathReadSet.size() != maxUnits)
		estPathReadSet.resize(maxUnits, 0); // producer-owned; resize off-lock

	// drain the draw->producer first-touch mailbox (mirrors the piece drain)
	{
		std::lock_guard<std::mutex> lk(estPathRegMtx);
		for (const int id: pendingEstPathRegs) {
			if (id >= 0 && static_cast<size_t>(id) < maxUnits)
				estPathReadSet[id] = 1;
		}
		pendingEstPathRegs.clear();
	}

	return &estPathReadSet;
}

void LuaSnapshotServe::RefreshPieces(int ringSlot, uint64_t targetEpoch)
{
	// PR 46: attribution (see RefreshCommandQueues)
	SCOPED_TIMER("Sim::EpochProduce::Pieces");

	// producer-only (the flip's sim frame edge, or the lockstep barrier with
	// the sim parked). Epoch-gated so the copies always describe the same
	// boundary as the target slot's rows, and so the walk is free when
	// nothing was (re)published.
	const uint64_t gen = targetEpoch;

	if (gen == 0 || gen == pieceCacheEpochs[ringSlot])
		return;

	// flag-off (no contract, gate unarmed) never serves these twins -- skip the
	// whole capture so flag-off pays only the generation compare above
	if (!LuaSplitContract::Enabled() && !snapshotDiffGate.Armed())
		return;

	pieceCacheEpochs[ringSlot] = gen;

	std::vector<ObjectPieceSlot>& unitPieceCache = unitPieceCaches[ringSlot];
	std::vector<ObjectPieceSlot>& featurePieceCache = featurePieceCaches[ringSlot];

	const size_t maxUnits = unitHandler.MaxUnits();
	if (unitPieceCache.size() != maxUnits)
		unitPieceCache.resize(maxUnits);
	if (unitPieceReadSet.size() != maxUnits)
		unitPieceReadSet.resize(maxUnits, 0);

	// PR 46: drain the first-touch registrations (draw -> producer mailbox)
	{
		std::lock_guard<std::mutex> lk(pieceRegMtx);

		for (const int id: pendingUnitPieceRegs) {
			if (id >= 0 && static_cast<size_t>(id) < maxUnits)
				unitPieceReadSet[id] = 1;
		}
		pendingUnitPieceRegs.clear();

		for (const int id: pendingFeaturePieceRegs) {
			if (id < 0)
				continue;
			if (static_cast<size_t>(id) >= featurePieceReadSet.size())
				featurePieceReadSet.resize(id + 1, 0);
			featurePieceReadSet[id] = 1;
		}
		pendingFeaturePieceRegs.clear();
	}

	// pre-register units with a live nano job: BAR-class widgets (nano
	// particles gl4) query the lathing unit's emit pieces on the first draw
	// frame of a job, so first-touch parks arrived in build-wave bursts
	// ([SimPauseSurvey] PIECE_FIRST_TOUCH ~1200/game, several serialized
	// mid-frame sim waits inside one draw frame). Registering at job start
	// captures the pieces one edge ahead of the first query; idle builders
	// stay unregistered and dead ids still prune, so the read-set stays
	// demand-shaped -- and the whole pass is demand-gated (unitPieceDemandSeen)
	// so a game with no piece-querying addon captures nothing. The unitDef
	// dispatch mirrors CUnitHandler::NewUnit (IsFactoryUnit => CFactory, any
	// other builder def => CBuilder).
	if (unitPieceDemandSeen.load(std::memory_order_relaxed)) {
		for (const CUnit* u: unitHandler.GetActiveUnits()) {
			const UnitDef* ud = u->unitDef;
			bool nanoActive = false;

			if (ud->IsFactoryUnit()) {
				nanoActive = (static_cast<const CFactory*>(u)->curBuild != nullptr);
			} else if (ud->IsBuilderUnit()) {
				const CBuilder* b = static_cast<const CBuilder*>(u);
				nanoActive =
					(b->curBuild != nullptr) || (b->curReclaim != nullptr) ||
					(b->curResurrect != nullptr) || (b->curCapture != nullptr) ||
					b->terraforming;
			}

			if (nanoActive)
				unitPieceReadSet[u->id] = 1;
		}
	}

	if (featurePieceCache.size() < featurePieceReadSet.size())
		featurePieceCache.resize(featurePieceReadSet.size());

	// WS-1 §5.1: drain the death journal into THIS ring slot's caches -- this
	// replaces the retired all-slots present/deadMiss wipe loops (O(deaths
	// since this slot last produced) instead of O(maxUnits + featureCacheSize)
	// strided writes per epoch, which the skip key would defeat anyway)
	for (size_t i = pieceDeathDrainCursors[ringSlot]; i < pieceDeathJournal.size(); ++i) {
		const PieceDeathEntry& e = pieceDeathJournal[i];
		std::vector<ObjectPieceSlot>& cache = e.isFeature ? featurePieceCache : unitPieceCache;

		if (static_cast<size_t>(e.id) >= cache.size())
			continue;

		ObjectPieceSlot& s = cache[e.id];
		s.present = false;
		s.deadMiss = false;
		s.captureVersion = 0;
	}

	// WS-1 §6.2: rate-limited oracle pass over the skipped slots (0 = off)
	static uint64_t pieceSkipOracleEpochs = 0;
	const int oracleN = configHandler->GetInt("PieceSkipOracle");
	const bool oracleThisEpoch = (oracleN > 0) && ((++pieceSkipOracleEpochs % oracleN) == 0);

	// PR 32: units also capture colVol + lastHit (GetUnitCollisionVolumeData /
	// GetUnitPieceCollisionVolumeData / GetUnitLastAttackedPiece), like features.
	// PR 46: registered ids only; a dead id prunes (a reused id re-registers
	// through its own first touch)
	{
		SCOPED_TIMER("Sim::EpochProduce::PiecesUnits");

		for (size_t id = 0; id < maxUnits; ++id) {
			if (!unitPieceReadSet[id])
				continue;

			const CUnit* u = unitHandler.GetUnit(id);
			if (u == nullptr || u->isDead) {
				unitPieceReadSet[id] = 0;

				// WS-1 §5.1: journal the death for the other ring slots; clear
				// THIS slot's entry inline (the cursor advances past the new
				// entry at the end of this refresh)
				pieceDeathJournal.push_back({static_cast<int>(id), false});
				ObjectPieceSlot& s = unitPieceCache[id];
				s.present = false;
				s.deadMiss = false;
				s.captureVersion = 0;
				continue;
			}

			ObjectPieceSlot& slot = unitPieceCache[id];

			// WS-1 §4: skip the capture when the key proves the slot current
			if (slot.present && PieceSlotKeyMatches(slot, u)) {
				if (oracleThisEpoch)
					RunPieceSkipOracle(slot, u, /*isFeature*/false, static_cast<int>(id), /*script*/true, gen);
				continue;
			}

			RefreshObjectPieceSlot(slot, u, /*script*/true, /*colVol*/true, /*lastHit*/true);
		}
	}

	{
		SCOPED_TIMER("Sim::EpochProduce::PiecesFeatures");

		for (size_t id = 0; id < featurePieceReadSet.size(); ++id) {
			if (!featurePieceReadSet[id])
				continue;

			const CFeature* f = featureHandler.GetFeature(id);
			if (f == nullptr) {
				featurePieceReadSet[id] = 0;

				pieceDeathJournal.push_back({static_cast<int>(id), true});
				ObjectPieceSlot& s = featurePieceCache[id];
				s.present = false;
				s.deadMiss = false;
				s.captureVersion = 0;
				continue;
			}

			ObjectPieceSlot& slot = featurePieceCache[id];

			if (slot.present && PieceSlotKeyMatches(slot, f)) {
				if (oracleThisEpoch)
					RunPieceSkipOracle(slot, f, /*isFeature*/true, static_cast<int>(id), /*script*/false, gen);
				continue;
			}

			RefreshObjectPieceSlot(slot, f, /*script*/false, /*colVol*/true, /*lastHit*/true);
		}
	}

	// WS-1 §5.1: entries appended by this refresh were applied inline to this
	// slot's cache, so the cursor advances over them too; compact the journal
	// once every ring slot has drained it
	pieceDeathDrainCursors[ringSlot] = pieceDeathJournal.size();

	if (*std::min_element(pieceDeathDrainCursors.begin(), pieceDeathDrainCursors.end()) == pieceDeathJournal.size()) {
		pieceDeathJournal.clear();
		pieceDeathDrainCursors.fill(0);
	}
}


// ===================================================================
// TRACE REHOST (stage 4b-2): the object-scan trace::EpochView primitives, defined
// here where the demand piece-cache colvols (GetUnitPieceSlot/GetFeaturePieceSlot),
// the draw-side pick grid, and the object-free CCollisionHandler are all in scope.
// A candidate's real collision volume comes from the demand piece cache; its
// SYNCED transform is reconstructed from the snapshot rows (== ComposeMatrix). A
// candidate whose colvol is not (yet) captured is skipped; per-piece hit volumes
// (DefaultToPieceTree) are deferred in the object-free DetectHit (the caller then
// finds no hit) -- documented residual until the per-piece trace path lands.
// ===================================================================
namespace {

struct TraceCandidate {
	const CollisionVolume* cv = nullptr;
	CMatrix44f transform;
	float3 midPos;
	float3 relMidPos;
	bool inVoid = false;
	const ObjectPieceSlot* slot = nullptr;   // per-piece volumes (DefaultToPieceTree path)
};

// resolve a unit candidate's real colvol + synced transform (CUnit::GetTransformMatrix
// == ComposeMatrix(pos) == CMatrix44f(pos, -rightdir, updir, frontdir))
bool ResolveUnitTraceCandidate(int id, TraceCandidate& out)
{
	const SimSnapshot::UnitRows& u = simSnapshot.Read();
	if (!u.Valid(id))
		return false;
	const ObjectPieceSlot* slot = GetUnitPieceSlot(id);
	if (slot == nullptr || !slot->hasColVol)
		return false;
	out.cv = &slot->colVol;
	out.transform = CMatrix44f(u.Pos(id), -u.Rightdir(id), u.Updir(id), u.Frontdir(id));
	out.midPos = u.MidPos(id);
	out.relMidPos = u.relMidPos[id];
	out.inVoid = u.InVoid(id);
	out.slot = slot;
	return true;
}

// CFeature::GetTransformMatrix() == transMatrix; its columns are the stored
// matX/Y/Zdir (fm.GetX/Y/Z) and its translation column is pos
bool ResolveFeatureTraceCandidate(int id, TraceCandidate& out)
{
	const SimSnapshot::FeatureRows& f = simSnapshot.ReadFeatures();
	if (!f.Valid(id))
		return false;
	const ObjectPieceSlot* slot = GetFeaturePieceSlot(id);
	if (slot == nullptr || !slot->hasColVol)
		return false;
	out.cv = &slot->colVol;
	out.transform = CMatrix44f(f.Pos(id), f.MatXdir(id), f.MatYdir(id), f.MatZdir(id));
	out.midPos = f.MidPos(id);
	out.relMidPos = f.relMidPos[id];
	out.inVoid = f.InVoid(id);
	out.slot = slot;
	return true;
}

// object-free mirror of IntersectPieceTree/IntersectPiecesHelper over the demand
// per-piece cache (pd.pieceColVol + pd.modelSpaceMat + pd.scriptVisible). The
// live bounding-volume early-out (IntersectPieceTree) is a perf-only skip -- the
// bounding volume encloses all pieces, so testing every piece is result-identical.
bool EpochPieceTreeIntersect(const ObjectPieceSlot* slot, const CMatrix44f& objTransform,
	const float3& p0, const float3& p1, CollisionQuery* cq)
{
	if (slot == nullptr)
		return false;

	bool hit = false;
	float minDistSq = std::numeric_limits<float>::max();

	for (const PieceDynamic& pd : slot->pieces) {
		const CollisionVolume* lmpVol = &pd.pieceColVol;
		if (!pd.scriptVisible || lmpVol->IgnoreHits())
			continue;

		CMatrix44f volMat = objTransform * pd.modelSpaceMat;
		volMat.Translate(lmpVol->GetOffsets());

		CollisionQuery cqn;
		if (!CCollisionHandler::Intersect(lmpVol, volMat, p0, p1, &cqn))
			continue;
		if (!cqn.AnyHit())
			continue;

		const float curDistSq = (cqn.GetHitPos()).SqDistance(p0);
		if (curDistSq >= minDistSq)
			continue;

		minDistSq = curDistSq;
		hit = true;
		if (cq == nullptr)
			return true;
		*cq = cqn;
	}

	return hit;
}

// EpochView DetectHit dispatcher: DefaultToPieceTree volumes override forceTrace/
// testType and route to the per-piece path (mirroring CCollisionHandler::DetectHit
// (o, v, m, ...)); simple volumes go to the object-free DetectHit.
bool EpochDetectHit(const TraceCandidate& tc, const CollisionVolume* v,
	const float3& p0, const float3& p1, CollisionQuery* cq, bool forceTrace)
{
	if (v->DefaultToPieceTree()) {
		if (cq != nullptr)
			cq->Reset();
		if (tc.inVoid)
			return false;
		return EpochPieceTreeIntersect(tc.slot, tc.transform, p0, p1, cq);
	}
	return CCollisionHandler::DetectHit(tc.midPos, tc.relMidPos, tc.inVoid, v, tc.transform, p0, p1, cq, forceTrace);
}

// object-free mirror of TestConeHelper (TraceRay.cpp:44); no unsynced-debug block.
// The live helper passes lmp=nullptr so GetPointSurfaceDistance always uses the
// object transform (not per-piece), which we reconstruct here.
bool EpochTestConeHelper(const float3& tstPos, const float3& tstDir, float length, float spread, const TraceCandidate& tc)
{
	const CollisionVolume* cv = tc.cv;
	const float3& off = cv->GetOffsets();

	// GetWorldSpacePos(obj) = midPos + GetObjectSpaceVec(offsets); GetObjectSpaceVec
	// reconstructed from the transform columns (m == ComposeMatrix: -rightdir=col0)
	const float3 cvWorldPos = tc.midPos + (tc.transform.GetZ() * off.z) - (tc.transform.GetX() * off.x) + (tc.transform.GetY() * off.y);

	const float3 cvRelVec = cvWorldPos - tstPos;
	const float cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float coneSize = cvRelDst * spread + 1.0f;

	const float3 hitPos = tstPos + tstDir * cvRelDst;

	// GetPointSurfaceDistance(obj, lmp=nullptr, pos): vm = transform; +relMidPos;
	// +offsets; invert; then the matrix-space overload
	CMatrix44f vm = tc.transform;
	vm.Translate(tc.relMidPos);
	vm.Translate(off);
	vm.InvertAffineInPlace();

	bool ret = false;
	ret = ret || ((cv->GetPointSurfaceDistance(vm, tstPos) - coneSize) <= 0.0f);
	ret = ret || ((cv->GetPointSurfaceDistance(vm, hitPos) - coneSize) <= 0.0f);
	return ret;
}

// CSTATE_BIT_QUADMAPRAYS is blockingBits slot 3 (SimSnapshot PackBlockingBits)
constexpr uint8_t QUADMAPRAYS_BIT = (1u << 3);

} // namespace


float3 trace::EpochView::TargetBorderPos(UnitRef u, const float3& rawPos, const float3& rawDir) const
{
	// CWeapon::GetTargetBorderPos (Weapon.cpp:893) over the epoch
	float3 targetBorderPos = rawPos;

	const WeaponDef* wd = Def();
	if (wd->targetBorder == 0.0f)
		return targetBorderPos;
	if (u < 0 || !urows.Valid(u))              // targetUnit == nullptr
		return targetBorderPos;
	if (rawDir == ZeroVector)
		return targetBorderPos;

	TraceCandidate tc;
	if (!ResolveUnitTraceCandidate(u, tc))     // colvol not captured -> no adjustment
		return targetBorderPos;

	const float tbScale = math::fabsf(wd->targetBorder);
	const float3 weaponMuzzlePos = WeaponMuzzlePos();

	CollisionVolume tmpColVol = *tc.cv;
	CollisionQuery  tmpColQry;

	tmpColVol.RescaleAxes(float3(tbScale, tbScale, tbScale));
	tmpColVol.SetBoundingRadius();
	tmpColVol.SetUseContHitTest(false);
	tmpColVol.SetDefaultToPieceTree(false);
	tmpColVol.SetIgnoreHits(false);

	// weapon muzzle inside the (scaled) volume -> border collapses to the muzzle
	if (EpochDetectHit(tc, &tmpColVol, weaponMuzzlePos, ZeroVector, nullptr, false))
		return (targetBorderPos = weaponMuzzlePos);

	tmpColVol.SetUseContHitTest(true);
	tmpColVol.SetDefaultToPieceTree(tc.cv->DefaultToPieceTree());
	tmpColVol.SetIgnoreHits(tc.cv->IgnoreHits());

	const float3 targetOffset = rawDir * (tmpColVol.GetBoundingRadius() * 2.0f);
	const float3 targetRayPos = rawPos + targetOffset;

	if (EpochDetectHit(tc, &tmpColVol, weaponMuzzlePos, targetRayPos, &tmpColQry, false) && tmpColQry.AllHit())
		targetBorderPos = mix(tmpColQry.GetIngressPos(), tmpColQry.GetEgressPos(), wd->targetBorder <= 0.0f);

	return targetBorderPos;
}


float trace::EpochView::TraceRayNoEnemyNoGroundDist(const float3& srcPos, const float3& dir, float length, uint32_t avoidFlags) const
{
	// LiveView routes TraceRay(srcPos, dir, length, avoidFlags | NOENEMIES | NOGROUND,
	// owner, ...): object scan only (no ground, no enemies), closest-hit length.
	const uint32_t traceFlags = avoidFlags | Collision::NOENEMIES | Collision::NOGROUND;
	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForCloaked  = ((traceFlags & Collision::NOCLOAKED   ) == 0);
	const bool scanForAnyUnits = scanForAllies || scanForNeutrals || scanForCloaked;

	if (dir == ZeroVector)
		return -1.0f;

	float traceLength = length;
	if (!scanForFeatures && !scanForAnyUnits)
		return traceLength;

	static std::vector<int> candUnits;
	static std::vector<int> candFeatures;
	snapshotPickGrid.QueryRayExact(srcPos, dir, traceLength, candUnits, candFeatures);

	const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();
	CollisionQuery cq;

	if (scanForFeatures) {
		for (const int id : candFeatures) {
			if ((frows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			TraceCandidate tc;
			if (!ResolveFeatureTraceCandidate(id, tc))
				continue;
			if (EpochDetectHit(tc, tc.cv, srcPos, srcPos + dir * traceLength, &cq, true)) {
				const float len = cq.GetHitPosDist(srcPos, dir);
				if (len < traceLength)
					traceLength = len;
			}
		}
	}

	if (scanForAnyUnits) {
		for (const int id : candUnits) {
			if (id == ownerID)
				continue;
			if ((urows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			const int uAllyTeam = urows.AllyTeam(id);
			bool doHitTest = false;
			doHitTest |= (scanForAllies   && uAllyTeam == ownerAllyTeam);
			doHitTest |= (scanForNeutrals && urows.Neutral(id));
			doHitTest |= (scanForCloaked  && urows.IsCloaked(id));
			if (!doHitTest)
				continue;
			TraceCandidate tc;
			if (!ResolveUnitTraceCandidate(id, tc))
				continue;
			if (EpochDetectHit(tc, tc.cv, srcPos, srcPos + dir * traceLength, &cq, true)) {
				const float len = cq.GetHitPosDist(srcPos, dir);
				if (len < traceLength)
					traceLength = len;
			}
		}
	}

	return traceLength;
}


bool trace::EpochView::TestCone(const float3& from, const float3& dir, float length, float spread, uint32_t avoidFlags) const
{
	// TraceRay::TestCone(from, dir, length, spread, ownerAllyTeam, avoidFlags, owner)
	const bool scanForAllies   = ((avoidFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((avoidFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((avoidFlags & Collision::NOFEATURES  ) == 0);

	static std::vector<int> candUnits;
	static std::vector<int> candFeatures;
	snapshotPickGrid.QueryRayExact(from, dir, length, candUnits, candFeatures);

	if (scanForAllies || scanForNeutrals) {
		for (const int id : candUnits) {
			if (id == ownerID)
				continue;
			if ((urows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			bool doTest = false;
			doTest |= (scanForAllies   && urows.AllyTeam(id) == ownerAllyTeam);
			doTest |= (scanForNeutrals && urows.Neutral(id));
			if (!doTest)
				continue;
			TraceCandidate tc;
			if (!ResolveUnitTraceCandidate(id, tc))
				continue;
			if (EpochTestConeHelper(from, dir, length, spread, tc))
				return true;
		}
	}

	if (scanForFeatures) {
		const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();
		for (const int id : candFeatures) {
			if ((frows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			TraceCandidate tc;
			if (!ResolveFeatureTraceCandidate(id, tc))
				continue;
			if (EpochTestConeHelper(from, dir, length, spread, tc))
				return true;
		}
	}

	return false;
}


// ===================================================================
// TRACE REHOST (stage 4b-3): ballistic HaveFreeLineOfFire primitives (Cannon
// trajectory + Missile pursuit-curve), object-free over the demand piece cache.
// ===================================================================
namespace {

// object-free mirror of TestTrajectoryConeHelper (TraceRay.cpp:93); coneSize is
// vestigial in the live body (the ret is decided by the DetectHit chord check).
bool EpochTestTrajectoryConeHelper(const float3& tstPos, const float3& tstDir, float length,
	float linear, float quadratic, const TraceCandidate& tc)
{
	const CollisionVolume* cv = tc.cv;
	const float3& off = cv->GetOffsets();
	const float3 cvWorldPos = tc.midPos + (tc.transform.GetZ() * off.z) - (tc.transform.GetX() * off.x) + (tc.transform.GetY() * off.y);

	const float3 cvRelVec = cvWorldPos - tstPos;
	const float cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);

	const float3 hitPos = (tstPos + tstDir * cvRelDst) + (UpVector * (quadratic * cvRelDst * cvRelDst + linear * cvRelDst));

	CollisionQuery cq;
	if ((2 * quadratic * cvRelDst + linear) > 0) {
		return EpochDetectHit(tc, cv, tstPos, hitPos, &cq, true);
	}
	const float3 endPos = (tstPos + tstDir * length) + (UpVector * (quadratic * length * length + linear * length));
	return EpochDetectHit(tc, cv, hitPos, endPos, &cq, true);
}

} // namespace


float trace::EpochView::TrajectoryGroundCol(const float3& srcPos, const float3& targetVec, float dist, float linCoeff, float qdrCoeff) const
{
	// unsynced heightmap (synced=false), the placement/ground precedent
	return CGround::TrajectoryGroundCol(srcPos, targetVec, dist, linCoeff, qdrCoeff, false);
}


bool trace::EpochView::TestTrajectoryCone(const float3& from, const float3& targetVec, float dist, float linCoeff, float qdrCoeff, float spread, uint32_t avoidFlags) const
{
	// TraceRay::TestTrajectoryCone -- broadphase is the plain XZ ray (NOT widened);
	// the per-candidate helper does the parabola/chord test
	const bool scanForAllies   = ((avoidFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((avoidFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((avoidFlags & Collision::NOFEATURES  ) == 0);

	static std::vector<int> candUnits;
	static std::vector<int> candFeatures;
	snapshotPickGrid.QueryRayExact(from, targetVec, dist, candUnits, candFeatures);

	if (scanForAllies || scanForNeutrals) {
		for (const int id : candUnits) {
			if (id == ownerID)
				continue;
			if ((urows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			bool doTest = false;
			doTest |= (scanForAllies   && urows.AllyTeam(id) == ownerAllyTeam);
			doTest |= (scanForNeutrals && urows.Neutral(id));
			if (!doTest)
				continue;
			TraceCandidate tc;
			if (!ResolveUnitTraceCandidate(id, tc))
				continue;
			if (EpochTestTrajectoryConeHelper(from, targetVec, dist, linCoeff, qdrCoeff, tc))
				return true;
		}
	}

	if (scanForFeatures) {
		const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();
		for (const int id : candFeatures) {
			if ((frows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			TraceCandidate tc;
			if (!ResolveFeatureTraceCandidate(id, tc))
				continue;
			if (EpochTestTrajectoryConeHelper(from, targetVec, dist, linCoeff, qdrCoeff, tc))
				return true;
		}
	}

	return false;
}


bool trace::EpochView::MissileTrajectoryLOF(const float3& srcPos, const float3& tgtPos, const Target& trg) const
{
	// CMissileLauncher::TrajectoryLOF (MissileLauncher.cpp:80) over the epoch. The
	// caller only reaches here with trajectoryHeight > 0; the pursuit-curve scalar
	// integration is verbatim, ground/object scans go object-free.
	const WeaponDef* wd = Def();
	const uint32_t avoidFlags = AvoidFlags();

	float3 targetVec = (tgtPos - srcPos) * XZVector;
	const float xzTargetDist = targetVec.LengthNormalize();
	if (xzTargetDist == 0.0f)
		return true;

	std::array<float, 9> mdist = {};
	std::array<float, 9> mheight = {};
	mdist[0] = 0; mheight[0] = 0;
	mdist[8] = xzTargetDist; mheight[8] = (tgtPos.y - srcPos.y);

	const float maxSpeed = wd->projectilespeed;
	const float pSpeed = wd->startvelocity;
	const float pAcc = wd->weaponacceleration;
	float curspeed = wd->startvelocity;
	float dist = srcPos.distance(tgtPos);
	const float rt = (tgtPos - srcPos).Length2D();
	const float yt = (tgtPos.y - srcPos.y);
	const float eH = (dist * wd->trajectoryHeight);
	const float eHT = math::truncf(dist / maxSpeed);
	const float hstep = eHT / 8.0f;

	// close target (impact within 8 frames): fall back to the base LOF (== the
	// TestTrajectoryCone path CWeapon::HaveFreeLineOfFire routes through)
	if (hstep < 1.0f)
		return trace::HaveFreeLineOfFireBaseT(*this, srcPos, tgtPos, trg);

	float drdt = 0.0f, dydt = 0.0f, rt_est = 0.0f, yt_est = 0.0f, drdt_est = 0.0f, dydt_est = 0.0f;
	float t = 0.0f;
	for (uint32_t i = 1; i < 8; i++) {
		dist = math::sqrt(math::pow((rt - mdist[i - 1]), 2) + math::pow((yt + eH * (1 - t / eHT) - mheight[i - 1]), 2));
		curspeed = std::min((pSpeed + pAcc * t), maxSpeed);
		drdt = curspeed * (rt - mdist[i - 1]) / dist;
		dydt = curspeed * (yt + eH * (1 - t / eHT) - mheight[i - 1]) / dist;
		rt_est = mdist[i - 1] + hstep * drdt;
		yt_est = mheight[i - 1] + hstep * dydt;
		t = t + hstep;
		dist = math::sqrt(math::pow((rt - rt_est), 2) + math::pow((yt + eH * (1 - t / eHT) - yt_est), 2));
		curspeed = std::min((pSpeed + pAcc * t), maxSpeed);
		drdt_est = curspeed * (rt - rt_est) / dist;
		dydt_est = curspeed * (yt + eH * (1 - t / eHT) - yt_est) / dist;
		mdist[i] = mdist[i - 1] + (hstep * 0.5f) * (drdt + drdt_est);
		mheight[i] = mheight[i - 1] + (hstep * 0.5f) * (dydt + dydt_est);
	}

	// ground collision (unsynced heightmap)
	uint32_t ii = 1;
	float delta1 = mdist[ii] - mdist[ii - 1];
	float delta2 = 0.0f, ratio = 0.0f, hitheight = 0.0f;
	if ((avoidFlags & Collision::NOGROUND) == 0) {
		for (float dd = 0; dd < xzTargetDist - DamageAreaOfEffect(); dd += SQUARE_SIZE) {
			while (dd > mdist[ii]) {
				ii = ii + 1;
				delta1 = mdist[ii] - mdist[ii - 1];
			}
			delta2 = dd - mdist[ii - 1];
			ratio = delta2 / delta1;
			hitheight = mheight[ii - 1] + ratio * (mheight[ii] - mheight[ii - 1]);
			if (CGround::GetApproximateHeight(srcPos + targetVec * dd, false) > (srcPos.y + hitheight))
				return false;
		}
	}

	// object collision (chord check per candidate along the XZ ray)
	static std::vector<int> candUnits;
	static std::vector<int> candFeatures;
	snapshotPickGrid.QueryRayExact(srcPos, targetVec, xzTargetDist, candUnits, candFeatures);

	const bool scanForAllies   = ((avoidFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((avoidFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((avoidFlags & Collision::NOFEATURES  ) == 0);

	const auto chordCheck = [&](const TraceCandidate& tc) -> bool {
		const CollisionVolume* cv = tc.cv;
		const float3& off = cv->GetOffsets();
		const float3 cvWorldPos = tc.midPos + (tc.transform.GetZ() * off.z) - (tc.transform.GetX() * off.x) + (tc.transform.GetY() * off.y);
		const float cvRelDst = std::clamp((cvWorldPos - srcPos).dot(targetVec), 0.0f, xzTargetDist);
		CollisionQuery cq;
		for (int i = 1; i < 9; i++) {
			if (cvRelDst < mdist[i]) {
				const float d1 = mdist[i] - mdist[i - 1];
				const float d2 = cvRelDst - mdist[i - 1];
				const float rr = d2 / d1;
				const float hh = mheight[i - 1] + rr * (mheight[i] - mheight[i - 1]);
				const float3 hitPos = srcPos + targetVec * cvRelDst + UpVector * hh;
				if (mheight[i] > mheight[i - 1])
					return EpochDetectHit(tc, cv, srcPos, hitPos, &cq, true);
				return EpochDetectHit(tc, cv, hitPos, tgtPos, &cq, true);
			}
		}
		return false;
	};

	if (scanForAllies || scanForNeutrals) {
		for (const int id : candUnits) {
			if (id == ownerID)
				continue;
			if ((urows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			bool doTest = false;
			doTest |= (scanForAllies   && urows.AllyTeam(id) == ownerAllyTeam);
			doTest |= (scanForNeutrals && urows.Neutral(id));
			if (!doTest)
				continue;
			TraceCandidate tc;
			if (!ResolveUnitTraceCandidate(id, tc))
				continue;
			if (chordCheck(tc))
				return false;
		}
	}

	if (scanForFeatures) {
		const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();
		for (const int id : candFeatures) {
			if ((frows.BlockingBits(id) & QUADMAPRAYS_BIT) == 0)
				continue;
			TraceCandidate tc;
			if (!ResolveFeatureTraceCandidate(id, tc))
				continue;
			if (chordCheck(tc))
				return false;
		}
	}

	return true;
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


// mirror of LuaUnsyncedRead::GetSelectedUnitsSorted (the selection id set is
// draw-owned; only the unitDef sort-key deref was a live sim read)
int LuaSnapshotServe::GetSelectedUnitsSorted(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();

	const auto numDefKeys = PushUnitListSortedByDefSnap(L, rows, selectedUnitsHandler.selectedUnits);
	lua_pushnumber(L, numDefKeys);

	return 2;
}

// mirror of LuaUnsyncedRead::GetSelectedUnitsCounts
int LuaSnapshotServe::GetSelectedUnitsCounts(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();

	const auto numDefKeys = PushSparseUnitTallyByDefSnap(L, rows, selectedUnitsHandler.selectedUnits);
	lua_pushnumber(L, numDefKeys);

	return 2;
}

// mirror of LuaUnsyncedRead::GetGroupUnitsSorted (the group id set is draw-owned
// UI state; only the unitDef sort-key deref was a live sim read)
int LuaSnapshotServe::GetGroupUnitsSorted(lua_State* L, const char* caller)
{
	const CGroup* group = GetGroupFromArgSnap(L, 1);
	if (group == nullptr)
		return 0;

	const auto& rows = simSnapshot.Read();

	PushUnitListSortedByDefSnap(L, rows, group->units);
	return 1;
}

// mirror of LuaUnsyncedRead::GetGroupUnitsCounts
int LuaSnapshotServe::GetGroupUnitsCounts(lua_State* L, const char* caller)
{
	const CGroup* group = GetGroupFromArgSnap(L, 1);
	if (group == nullptr)
		return 0;

	const auto& rows = simSnapshot.Read();

	PushSparseUnitTallyByDefSnap(L, rows, group->units);
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


void LuaSnapshotServe::InvalidatePieceCaches()
{
	// WS-1 §5.4: also the explicit invalidation hook for an IN-PLACE checkpoint
	// load (the replay-rewind flow), which never passes through game teardown's
	// ClearCaches -- unit/feature ids, model pointers, read-set registrations
	// and journal positions all alias across the load, and post-load skip
	// hygiene must not hang on the SetModel PostLoad re-seed alone
	for (auto* caches : {&unitPieceCaches, &featurePieceCaches}) {
		for (auto& cache : *caches) {
			cache.clear();
			cache.shrink_to_fit();
		}
	}
	{
		std::lock_guard<std::mutex> lock(modelMetaMtx);
		modelMetaCache.clear();
	}
	pieceCacheEpochs.fill(0);

	// PR 46: the piece read-set restarts with the next game (ids alias)
	{
		std::lock_guard<std::mutex> lk(pieceRegMtx);
		unitPieceReadSet.clear();
		featurePieceReadSet.clear();
		pendingUnitPieceRegs.clear();
		pendingFeaturePieceRegs.clear();
	}

	// WS-1 §5.1: journal entries and drain cursors describe dead ids of the
	// outgoing world; the caches they would be applied to were just dropped
	pieceDeathJournal.clear();
	pieceDeathJournal.shrink_to_fit();
	pieceDeathDrainCursors.fill(0);
}

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
	for (auto& cache : cmdQueueCaches) {
		cache.clear();
		cache.shrink_to_fit();
	}
	cmdQueueCacheEpochs.fill(0);

	// sim|draw WS-5: the dirty-list restarts with the next game (unit ids and
	// the per-slot version stamps reset), so a surviving entry could alias fresh
	// ones -- drop it (generation-safety, same rationale as the cache clear above)
	cmdDirtyPending.clear();
	cmdDirtyPending.shrink_to_fit();
	cmdDirtyActive.clear();
	cmdDirtyActive.shrink_to_fit();
	cmdDirtySeeded = false;

	// PR 33 piece caches: unit/feature ids and model pointers restart with the
	// next game, so a surviving entry could alias fresh ones
	InvalidatePieceCaches();

	// WS-6: the est-path read-set likewise restarts with the next game
	{
		std::lock_guard<std::mutex> lk(estPathRegMtx);
		estPathReadSet.clear();
		pendingEstPathRegs.clear();
	}

	// TRACE REHOST (stage 4b) / PLACEMENT REHOST (stage 4): the PR 35 weapon-trace
	// and PR 38e placement query/reply channels are both retired -- all four trace
	// callouts and all three placement callouts are served draw-side via EpochView,
	// synchronously, with no cross-frame queue or reply map to clear.
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

// sim|draw PR 38d: GetGroundInfo. Line-by-line mirror of
// LuaSyncedRead::GetGroundInfo + PushTerrainTypeData(tt, true). The two sim
// reads -- readMap->GetTypeMapSynced()[sqrIndex] and LuaMetalMap::GetMetalAmount
// (metalMap distribution map) -- are swapped for the drawMapMirrors typemap +
// metal queries (added by PR 38d); the terrain-type struct is the PR-28 table
// copy. Argument parsing, the pop-2/push-ix/push-iz stack shuffle (so the metal
// scratch coords match the live body) and the 9-value return are identical.
int LuaSnapshotServe::GetGroundInfo(lua_State* L, const char* caller)
{
	const float x = luaL_checkfloat(L, 1);
	const float z = luaL_checkfloat(L, 2);

	const int ix = std::clamp(x, 0.0f, float3::maxxpos) / (SQUARE_SIZE * 2);
	const int iz = std::clamp(z, 0.0f, float3::maxzpos) / (SQUARE_SIZE * 2);

	const int maxIndex = (mapDims.hmapx * mapDims.hmapy) - 1;
	const int sqrIndex = std::min(maxIndex, (mapDims.hmapx * iz) + ix);
	const int ttIndex  = drawMapMirrors.TypeMapAt(sqrIndex);

	// the live body pops x/z and pushes ix/iz so LuaMetalMap::GetMetalAmount's
	// absolute-index read sees the quantized coords; keep the same stack shape
	// (the ix/iz scratch is below the return window), then read metal from the
	// mirror directly with the same clamp+scale
	lua_pop(L, 2);
	lua_pushnumber(L, ix);
	lua_pushnumber(L, iz);

	// PushTerrainTypeData(L, &mapInfo->terrainTypes[ttIndex], true) mirror
	// (9 returns: index + name + metalAmount + 5 speed/hardness floats + receiveTracks)
	const DrawMapMirrors::TerrainType& tt = drawMapMirrors.TerrainTypeAt(ttIndex);
	lua_pushinteger(L, ttIndex);                          // tt - &mapInfo->terrainTypes[0]
	lua_pushsstring(L, tt.name);
	lua_pushnumber(L, drawMapMirrors.MetalAmount(ix, iz)); // LuaMetalMap::GetMetalAmount(ix, iz)
	lua_pushnumber(L, tt.hardness);
	lua_pushnumber(L, tt.tankSpeed);
	lua_pushnumber(L, tt.kbotSpeed);
	lua_pushnumber(L, tt.hoverSpeed);
	lua_pushnumber(L, tt.shipSpeed);
	lua_pushboolean(L, tt.receiveTracks);
	return 9;
}

// ===========================================================================
// sim|draw PR 29: blocking-map mirror + placement family
// GetGroundBlocked reads the DrawMapMirrors blocking mirror (per-square cell[0]
// id + kind) and gates visibility through the published unit/feature rows;
// Pos2BuildPos snaps to the build grid over the already-draw-safe unsynced
// heightmap. Line-by-line mirrors of the live LuaSyncedRead bodies; argument
// parsing, error text and return shapes identical by construction.
// ===========================================================================

namespace {
	// LuaSyncedRead.cpp's file-local ParseMapCoords mirror (keep in lockstep):
	// pure arg parse + quantize/clamp against mapDims -- no sim state.
	void ParseMapCoordsMirror(lua_State* L, const char* caller,
	                          int& tx1, int& tz1, int& tx2, int& tz2)
	{
		float fx1 = 0, fz1 = 0, fx2 = 0, fz2 = 0;

		const int args = lua_gettop(L);
		if (args == 2) {
			fx1 = fx2 = luaL_checkfloat(L, 1);
			fz1 = fz2 = luaL_checkfloat(L, 2);
		}
		else if (args == 4) {
			fx1 = luaL_checkfloat(L, 1);
			fz1 = luaL_checkfloat(L, 2);
			fx2 = luaL_checkfloat(L, 3);
			fz2 = luaL_checkfloat(L, 4);
		}
		else {
			luaL_error(L, "Incorrect arguments to %s()", caller);
		}

		tx1 = std::clamp((int)(fx1 / SQUARE_SIZE), 0, mapDims.mapxm1);
		tx2 = std::clamp((int)(fx2 / SQUARE_SIZE), 0, mapDims.mapxm1);
		tz1 = std::clamp((int)(fz1 / SQUARE_SIZE), 0, mapDims.mapym1);
		tz2 = std::clamp((int)(fz2 / SQUARE_SIZE), 0, mapDims.mapym1);
	}
}

int LuaSnapshotServe::GetGroundBlocked(lua_State* L, const char* caller)
{
	const Pov pov = HandlePov(L);

	// POV gate (before ParseMapCoords, exactly as the live body): a handle
	// without a read allyteam and without fullRead sees nothing
	if ((pov.readAllyTeam < 0) && !pov.fullRead)
		return 0;

	int tx1, tx2, tz1, tz2;
	ParseMapCoordsMirror(L, caller, tx1, tz1, tx2, tz2);

	const SimSnapshot::UnitRows& urows = simSnapshot.Read();
	const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();

	for (int z = tz1; z <= tz2; z++) {
		for (int x = tx1; x <= tx2; x++) {
			// cell[0] mirror lookup replaces groundBlockingObjectMap.GroundBlocked
			uint8_t kind = DrawMapMirrors::BLOCK_KIND_NONE;
			const int id = drawMapMirrors.BlockedAt(x, z, kind);

			if (kind == DrawMapMirrors::BLOCK_KIND_FEATURE) {
				// LuaUtils::IsFeatureVisible(L, feature) mirror (PovFeatureVisible
				// needs a valid row; the mirror captured this feature alive at the
				// same boundary, so Valid holds -- the guard is the stale/nil
				// contract, not an expected branch)
				if (frows.Valid(id) && PovFeatureVisible(frows, id, pov)) {
					HSTR_PUSH(L, "feature");
					lua_pushnumber(L, id);
					return 2;
				}
				continue;
			}

			if (kind == DrawMapMirrors::BLOCK_KIND_UNIT) {
				// unit->losStatus[readAllyTeam] & LOS_INLOS, mirrored from the
				// published all-allyteam losStatus stride (F_LOSSTATUS-verified);
				// fullRead short-circuits before the (guaranteed >= 0) allyteam read
				if (!urows.Valid(id))
					continue;
				if (pov.fullRead || (urows.LosStatus(id, pov.readAllyTeam) & LOS_INLOS)) {
					HSTR_PUSH(L, "unit");
					lua_pushnumber(L, id);
					return 2;
				}
				continue;
			}
			// BLOCK_KIND_NONE: empty cell or a non-unit/non-feature CSolidObject
			// -- the live "neither dynamic_cast matched" fall-through
		}
	}

	lua_pushboolean(L, false);
	return 1;
}

int LuaSnapshotServe::Pos2BuildPos(lua_State* L, const char* caller)
{
	const int unitDefID = luaL_checkint(L, 1);
	const UnitDef* ud = unitDefHandler->GetUnitDefByID(unitDefID);
	if (ud == nullptr)
		return 0;

	const float3 worldPos = {luaL_checkfloat(L, 2), luaL_checkfloat(L, 3), luaL_checkfloat(L, 4)};

	// build-grid snap over the UNSYNCED heightmap: CGameHelper::Pos2BuildPos with
	// synced=false reads GetCornerHeightMapUnsynced / GetHeight{Real,AboveWater}
	// (draw-safe, the reference dirty-rect split). ShouldServe rejects synced
	// handles, so GetHandleSynced is always false here; passing it mirrors the
	// live callout's exact argument.
	//
	// sim|draw PR 29 integration fix: for immobile+levelGround unitdefs (most
	// buildings -- the primary use of this callout) GetBuildHeight also clamps
	// against readMap->GetCurrMin/MaxHeight(), the sim-mutable currHeightBounds
	// float2 written by the synced heightmap/terraform path. That is a live
	// cross-thread read the armed diff-gate cannot catch (both legs read the same
	// live scalar -> EQUAL). Route it through the SimSnapshot GlobalRows mirror --
	// the same boundary-consistent scalars the GetGroundExtremes twin already
	// serves -- so the read is one-boundary-stale, never torn/half-updated.
	const auto& gRows = simSnapshot.ReadGlobals();
	const float2 currHeightBounds{gRows.currMinHeight, gRows.currMaxHeight};
	const float3 buildPos = CGameHelper::Pos2BuildPos({ud, worldPos, luaL_optint(L, 5, FACING_SOUTH)}, CLuaHandle::GetHandleSynced(L), &currHeightBounds);

	lua_pushnumber(L, buildPos.x);
	lua_pushnumber(L, buildPos.y);
	lua_pushnumber(L, buildPos.z);
	return 3;
}

/******************************************************************************
 * PR 31 (weapon/shield scalar family) serving twins. Served from the
 * SimSnapshot::UnitRows weapon block (per-unit scalars + a flat per-weapon SoA,
 * index = weaponOffset[unitID] + weaponNum). Line-by-line mirrors of the live
 * bodies in LuaSyncedRead.cpp; every sim-object read is replaced by a snapshot
 * row read, float expression order kept identical for bit equality. POV: all
 * gate on ParseAllyUnit (PovAlliedUnit) except GetUnitShieldState (ParseInLosUnit
 * -> PovUnitInLos). gs->frameNum is mirrored by rows.simFrame (the boundary
 * frame). Trace tests (TryTarget/TestTarget/TestRange/HaveFreeLineOfFire) are
 * PR 35's, not served here.
 ******************************************************************************/

namespace {
	// mirror of PushDamagesKey (LuaSyncedRead.cpp) over the flattened DamagesSnap;
	// same key set, same push types, same GetNumTypes/Get(index) semantics
	int PushDamagesKeySnap(lua_State* L, const SimSnapshot::UnitRows::DamagesSnap& damages, int index)
	{
		if (lua_isnumber(L, index)) {
			const unsigned armType = lua_toint(L, index);

			if (armType >= damages.damages.size())
				return 0;

			lua_pushnumber(L, damages.damages[armType]);
			return 1;
		}

		switch (hashString(luaL_checkstring(L, index))) {
			case hashString("paralyzeDamageTime"): { lua_pushnumber(L, damages.paralyzeDamageTime); } break;

			case hashString("impulseFactor"): { lua_pushnumber(L, damages.impulseFactor); } break;
			case hashString("impulseBoost"):  { lua_pushnumber(L, damages.impulseBoost);  } break;

			case hashString("craterMult"):  { lua_pushnumber(L, damages.craterMult);  } break;
			case hashString("craterBoost"): { lua_pushnumber(L, damages.craterBoost); } break;

			case hashString("dynDamageExp"):      { lua_pushnumber(L, damages.dynDamageExp);   } break;
			case hashString("dynDamageMin"):      { lua_pushnumber(L, damages.dynDamageMin);   } break;
			case hashString("dynDamageRange"):    { lua_pushnumber(L, damages.dynDamageRange); } break;
			case hashString("dynDamageInverted"): { lua_pushboolean(L, damages.dynDamageInverted); } break;

			case hashString("craterAreaOfEffect"): { lua_pushnumber(L, damages.craterAreaOfEffect); } break;
			case hashString("damageAreaOfEffect"): { lua_pushnumber(L, damages.damageAreaOfEffect); } break;

			case hashString("edgeEffectiveness"): { lua_pushnumber(L, damages.edgeEffectiveness); } break;
			case hashString("explosionSpeed"):    { lua_pushnumber(L, damages.explosionSpeed);    } break;

			default: { return 0; } break;
		}

		return 1;
	}
}

// mirror of LuaSyncedRead::GetUnitStockpile (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitStockpile(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.hasStockpile[unitID] == 0)
		return 0;

	lua_pushnumber(L, rows.stockpileNumStockpiled[unitID]);
	lua_pushnumber(L, rows.stockpileNumQueued[unitID]);
	lua_pushnumber(L, rows.stockpileBuildPercent[unitID]);
	return 3;
}

// mirror of LuaSyncedRead::GetUnitShieldState (ParseInLosUnit gate)
int LuaSnapshotServe::GetUnitShieldState(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseInLosUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const size_t idx = luaL_optint(L, 2, -1) - LUA_WEAPON_BASE_INDEX;

	bool hasShield;
	uint8_t shieldEnabled;
	float shieldPower;

	if (idx >= static_cast<size_t>(rows.weaponCount[unitID])) {
		// default: unit->shieldWeapon (static_cast in the live path)
		hasShield = (rows.hasShieldWeapon[unitID] != 0);
		shieldEnabled = rows.shieldWeaponEnabled[unitID];
		shieldPower = rows.shieldWeaponPower[unitID];
	} else {
		// explicit weapon index (dynamic_cast to CPlasmaRepulser in the live path)
		const int wi = rows.weaponOffset[unitID] + static_cast<int>(idx);
		hasShield = (rows.wIsShield[wi] != 0);
		shieldEnabled = rows.wShieldEnabled[wi];
		shieldPower = rows.wShieldPower[wi];
	}

	if (!hasShield)
		return 0;

	lua_pushnumber(L, shieldEnabled);
	lua_pushnumber(L, shieldPower);
	return 2;
}

// mirror of LuaSyncedRead::GetUnitFlanking (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitFlanking(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (lua_israwstring(L, 2)) {
		const char* key = lua_tostring(L, 2);

		switch (hashString(key)) {
			case hashString("mode"): {
				lua_pushnumber(L, rows.flankingMode[unitID]);
				return 1;
			} break;
			case hashString("dir"): {
				lua_pushnumber(L, rows.flankingDir[unitID].x);
				lua_pushnumber(L, rows.flankingDir[unitID].y);
				lua_pushnumber(L, rows.flankingDir[unitID].z);
				return 3;
			} break;
			case hashString("moveFactor"): {
				lua_pushnumber(L, rows.flankingMoveFactor[unitID]);
				return 1;
			} break;
			case hashString("minDamage"): {
				lua_pushnumber(L, rows.flankingAvgDamage[unitID] - rows.flankingDifDamage[unitID]);
				return 1;
			} break;
			case hashString("maxDamage"): {
				lua_pushnumber(L, rows.flankingAvgDamage[unitID] + rows.flankingDifDamage[unitID]);
				return 1;
			} break;
			default: {
			} break;
		}
	}
	else if (lua_isnoneornil(L, 2)) {
		lua_pushnumber(L, rows.flankingMode[unitID]);
		lua_pushnumber(L, rows.flankingMoveFactor[unitID]);
		lua_pushnumber(L, rows.flankingAvgDamage[unitID] - // min
		                  rows.flankingDifDamage[unitID]);
		lua_pushnumber(L, rows.flankingAvgDamage[unitID] + // max
		                  rows.flankingDifDamage[unitID]);
		lua_pushnumber(L, rows.flankingDir[unitID].x);
		lua_pushnumber(L, rows.flankingDir[unitID].y);
		lua_pushnumber(L, rows.flankingDir[unitID].z);
		lua_pushnumber(L, rows.flankingMobility[unitID]);
		return 8;
	}

	return 0;
}

// mirror of LuaSyncedRead::GetUnitWeaponState (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitWeaponState(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;

	if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
		return 0;

	const int wi = rows.weaponOffset[unitID] + static_cast<int>(weaponNum);
	const char* key = luaL_optstring(L, 3, "");

	if (key[0] == 0) { // backwards compatible
		lua_pushboolean(L, rows.wAngleGood[wi]);
		lua_pushboolean(L, rows.wReloadStatus[wi] <= rows.simFrame);
		lua_pushnumber(L,  rows.wReloadStatus[wi]);
		lua_pushnumber(L,  rows.wSalvoLeft[wi]);
		lua_pushnumber(L,  rows.wNumStockpiled[wi]);
		return 5;
	}

	switch (hashString(key)) {
		case hashString("reloadState"):
		case hashString("reloadFrame"): {
			lua_pushnumber(L, rows.wReloadStatus[wi]);
		} break;

		case hashString("reloadTime"): {
			lua_pushnumber(L, rows.wReloadTime[wi] * INV_GAME_SPEED);
		} break;
		case hashString("reloadTimeXP"): {
			// reloadSpeed is affected by unit experience
			lua_pushnumber(L, (rows.wReloadTime[wi] / rows.reloadSpeed[unitID]) / GAME_SPEED);
		} break;
		case hashString("reaimTime"): {
			lua_pushnumber(L, rows.wReaimTime[wi]);
		} break;

		case hashString("accuracy"): {
			lua_pushnumber(L, rows.wAccuracyExp[wi]);
		} break;
		case hashString("sprayAngle"): {
			lua_pushnumber(L, rows.wSprayAngleExp[wi]);
		} break;

		case hashString("range"): {
			lua_pushnumber(L, rows.wRange[wi]);
		} break;
		case hashString("projectileSpeed"): {
			lua_pushnumber(L, rows.wProjectileSpeed[wi]);
		} break;

		case hashString("autoTargetRangeBoost"): {
			lua_pushnumber(L, rows.wAutoTargetRangeBoost[wi]);
		} break;

		case hashString("burst"): {
			lua_pushnumber(L, rows.wSalvoSize[wi]);
		} break;
		case hashString("burstRate"): {
			lua_pushnumber(L, rows.wSalvoDelay[wi] * INV_GAME_SPEED);
		} break;
		case hashString("windup"): {
			lua_pushnumber(L, float(rows.wSalvoWindup[wi]) / GAME_SPEED);
		} break;

		case hashString("projectiles"): {
			lua_pushnumber(L, rows.wProjectilesPerShot[wi]);
		} break;

		case hashString("salvoError"): {
			const float3 salvoError = rows.wSalvoError[wi];

			lua_createtable(L, 3, 0);
			lua_pushnumber(L, salvoError.x); lua_rawseti(L, -2, 1);
			lua_pushnumber(L, salvoError.y); lua_rawseti(L, -2, 2);
			lua_pushnumber(L, salvoError.z); lua_rawseti(L, -2, 3);
		} break;

		case hashString("salvoLeft"): {
			lua_pushnumber(L, rows.wSalvoLeft[wi]);
		} break;
		case hashString("nextSalvo"): {
			lua_pushnumber(L, rows.wNextSalvo[wi]);
		} break;

		case hashString("targetMoveError"): {
			lua_pushnumber(L, rows.wMoveErrorExp[wi]);
		} break;

		case hashString("avoidFlags"): {
			lua_pushnumber(L, rows.wAvoidFlags[wi]);
		} break;
		case hashString("collisionFlags"): {
			lua_pushnumber(L, rows.wCollisionFlags[wi]);
		} break;
		case hashString("ttl"): {
			lua_pushnumber(L, rows.wTtl[wi] * INV_GAME_SPEED);
		} break;

		default: {
			return 0;
		} break;
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitWeaponDamages (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitWeaponDamages(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const SimSnapshot::UnitRows::DamagesSnap* damages = nullptr;

	if (lua_israwstring(L, 2)) {
		const char* key = lua_tostring(L, 2);

		switch (hashString(key)) {
			case hashString("explode"     ): { damages = &rows.deathExpDamages[unitID]; } break;
			case hashString("selfDestruct"): { damages = &rows.selfdExpDamages[unitID]; } break;
			default                        : {                              return 0; } break;
		}
	} else {
		const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;

		if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
			return 0;

		damages = &rows.wDamages[rows.weaponOffset[unitID] + static_cast<int>(weaponNum)];
	}

	// valid==0 reproduces the live "damages == nullptr" nil shape
	if (damages->valid == 0)
		return 0;

	return PushDamagesKeySnap(L, *damages, 3);
}

// mirror of LuaSyncedRead::GetUnitWeaponVectors (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitWeaponVectors(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;

	if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
		return 0;

	const int wi = rows.weaponOffset[unitID] + static_cast<int>(weaponNum);
	const float3& pos = rows.wMuzzlePos[wi];
	const float3* dir = &rows.wWantedDir[wi];

	switch (rows.wProjectileType[wi]) {
		case WEAPON_MISSILE_PROJECTILE  : { dir = &rows.wWeaponDir[wi]; } break;
		case WEAPON_TORPEDO_PROJECTILE  : { dir = &rows.wWeaponDir[wi]; } break;
		case WEAPON_STARBURST_PROJECTILE: { dir = &rows.wWeaponDir[wi]; } break;
		default                         : {                            } break;
	}

	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	lua_pushnumber(L, pos.z);

	lua_pushnumber(L, dir->x);
	lua_pushnumber(L, dir->y);
	lua_pushnumber(L, dir->z);

	return 6;
}

// mirror of LuaSyncedRead::GetUnitWeaponCanFire (ParseAllyUnit gate). CanFire is
// a pure-scalar predicate (no spatial/LOS query, unlike the PR-35 trace tests),
// so it is reproduced from the snapshot scalars: CWeapon::CanFire's decision
// tree (Weapon.cpp), same order + early-outs, plus the CBombDropper::CanFire
// override (ignoreAngleGood/ignoreRequestedDir forced true). gs->frameNum is
// rows.simFrame; the FPS-fire gate is the pre-extracted fpsNoFire bit.
int LuaSnapshotServe::GetUnitWeaponCanFire(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;

	if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
		return 0;

	bool ignoreAngleGood = luaL_optboolean(L, 3, false);
	const bool ignoreTargetType = luaL_optboolean(L, 4, false);
	bool ignoreRequestedDir = luaL_optboolean(L, 5, false);

	const int wi = rows.weaponOffset[unitID] + static_cast<int>(weaponNum);

	// CBombDropper::CanFire override -> CWeapon::CanFire(true, ignoreTargetType, true)
	if (rows.wIsBombDropper[wi] != 0) {
		ignoreAngleGood = true;
		ignoreRequestedDir = true;
	}

	// CWeapon::CanFire mirror (Weapon.cpp), from the snapshot scalars
	bool canFire = true;
	do {
		if (!ignoreAngleGood && rows.wAngleGood[wi] == 0) { canFire = false; break; }
		if ((rows.wSalvoLeft[wi] > 0) || (rows.wNextSalvo[wi] > rows.simFrame)) { canFire = false; break; }
		if (!ignoreTargetType && rows.wTargetType[wi] == Target_None) { canFire = false; break; } // !HaveTarget()
		if (rows.wReloadStatus[wi] > rows.simFrame) { canFire = false; break; }
		if (rows.wDefStockpile[wi] != 0 && rows.wNumStockpiled[wi] == 0) { canFire = false; break; }
		// muzzle is underwater but we cannot fire underwater
		if (rows.wDefFireSubmersed[wi] == 0 && rows.wAimFromPosY[wi] <= 0.0f) { canFire = false; break; }
		// sanity check to force new aim
		if (rows.wDefMaxFireAngle[wi] > -1.0f) {
			if (!ignoreRequestedDir && rows.wWantedDir[wi].dot(rows.wLastRequestedDir[wi]) <= rows.wDefMaxFireAngle[wi]) { canFire = false; break; }
		}
		// FPS mode: player must be pressing at least one button to fire
		if (rows.fpsNoFire[unitID] != 0) { canFire = false; break; }
	} while (false);

	lua_pushboolean(L, canFire);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitWeaponTarget (ParseAllyUnit gate)
int LuaSnapshotServe::GetUnitWeaponTarget(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;

	if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
		return 0;

	const int wi = rows.weaponOffset[unitID] + static_cast<int>(weaponNum);
	const int targetType = rows.wTargetType[wi];

	lua_pushnumber(L, targetType);

	switch (targetType) {
		case Target_None:
			return 1;
			break;
		case Target_Unit: {
			lua_pushboolean(L, rows.wTargetIsUser[wi]);
			lua_pushnumber(L, rows.wTargetUnitID[wi]);
			break;
		}
		case Target_Pos: {
			lua_pushboolean(L, rows.wTargetIsUser[wi]);
			lua_createtable(L, 3, 0);
			lua_pushnumber(L, rows.wTargetGroundPos[wi].x); lua_rawseti(L, -2, 1);
			lua_pushnumber(L, rows.wTargetGroundPos[wi].y); lua_rawseti(L, -2, 2);
			lua_pushnumber(L, rows.wTargetGroundPos[wi].z); lua_rawseti(L, -2, 3);
			break;
		}
		case Target_Intercept: {
			lua_pushboolean(L, rows.wTargetIsUser[wi]);
			lua_pushnumber(L, rows.wTargetInterceptID[wi]);
			break;
		}
	}

	return 3;
}

// ===========================================================================
// PR 32 (deep per-unit state): serving twins over the PR-32 SimSnapshot rows +
// the moveType full-table block. Each is a line-by-line mirror of its live
// LuaSyncedRead body with sim-object reads replaced by row reads; UnitDef
// derefs (immutable game data) stay direct via unitDefHandler. POV gates mirror
// the owning parse helper. The three collision-volume/last-hit twins reuse
// PR 33's unit piece cache (now colVol+lastHit-capturing).
// ===========================================================================

// mirror of LuaSyncedRead::GetUnitStates (ParseAllyUnit)
int LuaSnapshotServe::GetUnitStates(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const bool retTable = luaL_optboolean(L, 2,     true);
	const bool binState = luaL_optboolean(L, 3, retTable);
	const bool amtState = luaL_optboolean(L, 4, retTable);

	const uint8_t kind = rows.moveTypeKind[unitID];

	if (!retTable) {
		{
			lua_pushnumber(L, rows.fireState[unitID]);
			lua_pushnumber(L, rows.moveState[unitID]);
			lua_pushnumber(L, rows.repairBelowHealth[unitID]);
		}

		if (binState) {
			lua_pushboolean(L, rows.repeatOrders[unitID]);
			lua_pushboolean(L, rows.wantCloak[unitID]);
			lua_pushboolean(L, rows.activated[unitID]);
			lua_pushboolean(L, rows.useHighTrajectory[unitID]);
		}

		if (amtState) {
			if (kind == 2) { // hover air
				lua_pushboolean(L, rows.mtAutoLand[unitID]);
				lua_pushboolean(L, false);
				return (3 + (binState * 4) + 2);
			}
			if (kind == 3) { // strafe air
				lua_pushboolean(L, rows.mtAutoLand[unitID]);
				lua_pushboolean(L, rows.mtLoopbackAttack[unitID]);
				return (3 + (binState * 4) + 2);
			}
		}

		return (3 + (binState * 4));
	}

	{
		lua_createtable(L, 0, 9);

		{
			HSTR_PUSH_NUMBER(L, "firestate",  rows.fireState[unitID]);
			HSTR_PUSH_NUMBER(L, "movestate",  rows.moveState[unitID]);
			HSTR_PUSH_NUMBER(L, "autorepairlevel", rows.repairBelowHealth[unitID]);
		}

		if (binState) {
			HSTR_PUSH_BOOL(L, "repeat",     rows.repeatOrders[unitID]);
			HSTR_PUSH_BOOL(L, "cloak",      rows.wantCloak[unitID]);
			HSTR_PUSH_BOOL(L, "active",     rows.activated[unitID]);
			HSTR_PUSH_BOOL(L, "trajectory", rows.useHighTrajectory[unitID]);
		}

		if (amtState) {
			if (kind == 2) {
				HSTR_PUSH_BOOL(L, "autoland",       rows.mtAutoLand[unitID]);
				HSTR_PUSH_BOOL(L, "loopbackattack", false);
				return 1;
			}
			if (kind == 3) {
				HSTR_PUSH_BOOL(L, "autoland",       rows.mtAutoLand[unitID]);
				HSTR_PUSH_BOOL(L, "loopbackattack", rows.mtLoopbackAttack[unitID]);
				return 1;
			}
		}

		return 1;
	}
}

// mirror of LuaSyncedRead::GetUnitStorage (ParseAllyUnit)
int LuaSnapshotServe::GetUnitStorage(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.storage[unitID].metal);
	lua_pushnumber(L, rows.storage[unitID].energy);
	return 2;
}

// mirror of LuaSyncedRead::GetUnitMetalExtraction (ParseAllyUnit)
int LuaSnapshotServe::GetUnitMetalExtraction(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (!unitDefHandler->GetUnitDefByID(rows.defID[unitID])->extractsMetal)
		return 0;

	lua_pushnumber(L, rows.metalExtract[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitBuildeeRadius (ParseTypedUnit)
int LuaSnapshotServe::GetUnitBuildeeRadius(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, rows.buildeeRadius[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitPosErrorParams (ParseAllyUnit). posErrorVector /
// GetPosErrorBit are already snapshotted (posErrorVector / posErrorBits stride);
// the delta/nextUpdate rows are PR 32. NOTE: the live arg clamp is
// std::clamp(opt, 0, ActiveAllyTeams()) -- inclusive upper bound, so argAllyTeam
// can equal numAllyTeams (one past the posErrorBits stride). GetPosErrorBit(at)
// returns false for out-of-range at (it masks (1<<at) against posErrorMask, and
// posErrorMask has no such bit set), so PosErrorBit reproduces that: a >= stride
// argAllyTeam reads false, matching live.
int LuaSnapshotServe::GetUnitPosErrorParams(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int optAllyTeam = luaL_optinteger(L, 2, 0);
	const int argAllyTeam = std::clamp(optAllyTeam, 0, rows.numAllyTeams);

	const bool posErrorBit = (argAllyTeam >= 0 && argAllyTeam < rows.numAllyTeams) &&
		rows.posErrorBits[unitID * rows.numAllyTeams + argAllyTeam] != 0;

	lua_pushnumber(L, rows.posErrorVector[unitID].x);
	lua_pushnumber(L, rows.posErrorVector[unitID].y);
	lua_pushnumber(L, rows.posErrorVector[unitID].z);
	lua_pushnumber(L, rows.posErrorDelta[unitID].x);
	lua_pushnumber(L, rows.posErrorDelta[unitID].y);
	lua_pushnumber(L, rows.posErrorDelta[unitID].z);
	lua_pushnumber(L, rows.nextPosErrorUpdate[unitID]);
	lua_pushboolean(L, posErrorBit);

	return (3 + 3 + 1 + 1);
}

// mirror of LuaSyncedRead::GetUnitLastAttacker (ParseUnit; the attacker's own
// visibility gates the answer -- IsUnitVisible(lastAttacker) -> PovUnitVisible)
int LuaSnapshotServe::GetUnitLastAttacker(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitVisible(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int atkID = rows.lastAttackerID[unitID];
	if (atkID < 0 || !rows.Valid(atkID) || !rows.PovUnitVisible(atkID, pov.readAllyTeam, pov.fullRead))
		return 0;

	lua_pushnumber(L, atkID);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitIsBuilding (ParseAllyUnit; builder OR factory
// curBuild)
int LuaSnapshotServe::GetUnitIsBuilding(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// builderKind: 1 = CBuilder, 2 = CFactory (both push curBuild->id when set)
	if (rows.builderKind[unitID] != 0 && rows.curBuildID[unitID] >= 0) {
		lua_pushnumber(L, rows.curBuildID[unitID]);
		return 1;
	}

	return 0;
}

// mirror of LuaSyncedRead::GetUnitBuildParams (ParseAllyUnit; CBuilder only)
int LuaSnapshotServe::GetUnitBuildParams(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.builderKind[unitID] != 1) // not a CBuilder
		return 0;

	switch (hashString(luaL_checkstring(L, 2))) {
	case hashString("buildRange"):
	case hashString("buildDistance"): {
		lua_pushnumber(L, rows.buildDistance[unitID]);
		return 1;
	} break;
	case hashString("buildRange3D"): {
		lua_pushboolean(L, rows.range3D[unitID]);
		return 1;
	} break;
	default: {} break;
	};

	return 0;
}

// mirror of LuaSyncedRead::GetUnitInBuildStance (ParseAllyUnit; CBuilder only)
int LuaSnapshotServe::GetUnitInBuildStance(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.builderKind[unitID] != 1) // not a CBuilder
		return 0;

	lua_pushboolean(L, rows.inBuildStance[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitCurrentBuildPower (ParseAllyUnit; builder|factory)
int LuaSnapshotServe::GetUnitCurrentBuildPower(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.builderKind[unitID] == 0) // no NanoPieceCache (not builder/factory)
		return 0;

	lua_pushnumber(L, rows.buildPower[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitEffectiveBuildRange (ParseInLosUnit; CBuilderCAI
// only -- equivalent to CBuilder here, since a CBuilderCAI's ownerBuilder is a
// CBuilder). GetBuildRange(r) == buildDistance + r; the buildee model is
// immutable game data loaded exactly as the live path (main-thread GL is fine).
int LuaSnapshotServe::GetUnitEffectiveBuildRange(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.builderKind[unitID] != 1) // no CBuilderCAI
		return 0;

	const float buildDistance = rows.buildDistance[unitID];

	if (lua_isnoneornil(L, 2)) {
		lua_pushnumber(L, buildDistance + 0.0f);
		return 1;
	}

	const int buildeeDefID = luaL_checkint(L, 2);
	const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(buildeeDefID);
	if (unitDef == nullptr)
		luaL_error(L, "Nonexistent buildeeDefID %d passed to Spring.GetUnitEffectiveBuildRange", (int) buildeeDefID);

	const auto model = unitDef->LoadModel();
	if (model == nullptr)
		return 0;

	const auto radius = std::max(0.f, model->radius);

	lua_pushnumber(L, buildDistance + radius);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitNanoPieces (ParseAllyUnit; builder|factory)
int LuaSnapshotServe::GetUnitNanoPieces(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.builderKind[unitID] == 0) // no NanoPieceCache
		return 0;

	const std::vector<int32_t>& nanoPieces = rows.nanoPieces[unitID];

	if (nanoPieces.empty())
		return 0;

	lua_createtable(L, nanoPieces.size(), 0);

	for (size_t p = 0; p < nanoPieces.size(); p++) {
		lua_pushnumber(L, nanoPieces[p] + 1); // lua 1-indexed, c++ 0-indexed
		lua_rawseti(L, -2, p + 1);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitTransporter (ParseInLosUnit)
int LuaSnapshotServe::GetUnitTransporter(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovUnitInLos(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (rows.transporterID[unitID] < 0)
		return 0;

	lua_pushnumber(L, rows.transporterID[unitID]);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitIsTransporting (ParseAllyUnit; transport units)
int LuaSnapshotServe::GetUnitIsTransporting(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	if (!unitDefHandler->GetUnitDefByID(rows.defID[unitID])->IsTransportUnit())
		return 0;

	const std::vector<int32_t>& transportees = rows.transportees[unitID];

	lua_createtable(L, transportees.size(), 0);

	unsigned int unitCount = 1;
	for (const int32_t carriedID : transportees) {
		lua_pushnumber(L, carriedID);
		lua_rawseti(L, -2, unitCount++);
	}

	return 1;
}

// mirror of LuaSyncedRead::GetUnitTooltip (ParseTypedUnit). Composes the leader-
// name path from the team/player boundary copies (TeamRows/PlayerRows) and the
// decoy/custom path from immutable UnitDef + the customTooltip row; effectiveDef/
// decoyDef mirror LuaUtils::EffectiveUnitDef / IsAllyUnit over the snapshot POV.
int LuaSnapshotServe::GetUnitTooltip(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(rows.defID[unitID]);
	const bool allied = rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead);
	const UnitDef* decoyDef = allied ? nullptr : unitDef->decoyDef;
	// EffectiveUnitDef mirror
	const UnitDef* effectiveDef = allied ? unitDef : (unitDef->decoyDef ? unitDef->decoyDef : unitDef);

	std::string tooltip;

	if (effectiveDef->showPlayerName) {
		const auto& teams = simSnapshot.ReadTeams();
		const auto& players = simSnapshot.ReadPlayers();
		const int team = rows.team[unitID];

		if (teams.ValidTeam(team) && teams.leader[team] != -1) {
			const int leader = teams.leader[team];
			if (players.ValidPlayer(leader))
				tooltip = players.name[leader];
			tooltip = (teams.hasAIs[team] ? "AI@" : "") + tooltip;
		}
	} else {
		if (decoyDef == nullptr) {
			tooltip = rows.customTooltip[unitID];
		} else {
			tooltip = decoyDef->humanName + " - " + decoyDef->tooltip;
		}
	}

	lua_pushsstring(L, tooltip);
	return 1;
}

// mirror of LuaSyncedRead::GetUnitMoveTypeData (ParseAllyUnit). Full table from
// the flat base rows + the moveType full-table block (decision-2 full-copy).
int LuaSnapshotServe::GetUnitMoveTypeData(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const SimSnapshot::MoveTypeBlock& b = rows.moveTypeBlock[unitID];

	lua_createtable(L, 0, 26);
	HSTR_PUSH_NUMBER(L, "maxSpeed", rows.mtMaxSpeed[unitID]);
	HSTR_PUSH_NUMBER(L, "maxWantedSpeed", rows.mtMaxWantedSpeed[unitID]);
	HSTR_PUSH_NUMBER(L, "goalx", rows.mtGoalPos[unitID].x);
	HSTR_PUSH_NUMBER(L, "goaly", rows.mtGoalPos[unitID].y);
	HSTR_PUSH_NUMBER(L, "goalz", rows.mtGoalPos[unitID].z);

	switch (rows.mtProgressState[unitID]) {
		case 0: HSTR_PUSH_CSTRING(L, "progressState", "done");   break;
		case 1: HSTR_PUSH_CSTRING(L, "progressState", "active"); break;
		case 2: HSTR_PUSH_CSTRING(L, "progressState", "failed"); break;
	}

	const auto pushAircraftState = [&](int32_t st) {
		switch (st) {
			case AAirMoveType::AIRCRAFT_LANDED:   HSTR_PUSH_CSTRING(L, "aircraftState", "landed");   break;
			case AAirMoveType::AIRCRAFT_FLYING:   HSTR_PUSH_CSTRING(L, "aircraftState", "flying");   break;
			case AAirMoveType::AIRCRAFT_LANDING:  HSTR_PUSH_CSTRING(L, "aircraftState", "landing");  break;
			case AAirMoveType::AIRCRAFT_CRASHING: HSTR_PUSH_CSTRING(L, "aircraftState", "crashing"); break;
			case AAirMoveType::AIRCRAFT_TAKEOFF:  HSTR_PUSH_CSTRING(L, "aircraftState", "takeoff");  break;
			case AAirMoveType::AIRCRAFT_HOVERING: HSTR_PUSH_CSTRING(L, "aircraftState", "hovering"); break;
		}
	};

	switch (rows.moveTypeKind[unitID]) {
		case 1: { // ground
			HSTR_PUSH_CSTRING(L, "name", "ground");
			HSTR_PUSH_NUMBER(L, "turnRate", b.turnRate);
			HSTR_PUSH_NUMBER(L, "accRate", b.accRate);
			HSTR_PUSH_NUMBER(L, "decRate", b.decRate);
			HSTR_PUSH_NUMBER(L, "maxReverseSpeed", b.maxReverseSpeed);
			HSTR_PUSH_NUMBER(L, "wantedSpeed", b.wantedSpeed);
			HSTR_PUSH_NUMBER(L, "currentSpeed", b.currentSpeed);
			HSTR_PUSH_NUMBER(L, "goalRadius", b.goalRadius);
			HSTR_PUSH_NUMBER(L, "currwaypointx", b.currWayPoint.x);
			HSTR_PUSH_NUMBER(L, "currwaypointy", b.currWayPoint.y);
			HSTR_PUSH_NUMBER(L, "currwaypointz", b.currWayPoint.z);
			HSTR_PUSH_NUMBER(L, "nextwaypointx", b.nextWayPoint.x);
			HSTR_PUSH_NUMBER(L, "nextwaypointy", b.nextWayPoint.y);
			HSTR_PUSH_NUMBER(L, "nextwaypointz", b.nextWayPoint.z);
			HSTR_PUSH_NUMBER(L, "requestedSpeed", 0.0f);
			HSTR_PUSH_NUMBER(L, "pathFailures", 0);
			return 1;
		}
		case 2: { // hover air (gunship)
			HSTR_PUSH_CSTRING(L, "name", "gunship");
			HSTR_PUSH_NUMBER(L, "wantedHeight", b.wantedHeight);
			HSTR_PUSH_BOOL(L, "collide", b.collide);
			HSTR_PUSH_BOOL(L, "useSmoothMesh", b.useSmoothMesh);
			pushAircraftState(b.aircraftState);
			switch (b.flyState) {
				case CHoverAirMoveType::FLY_CRUISING:  HSTR_PUSH_CSTRING(L, "flyState", "cruising");  break;
				case CHoverAirMoveType::FLY_CIRCLING:  HSTR_PUSH_CSTRING(L, "flyState", "circling");  break;
				case CHoverAirMoveType::FLY_ATTACKING: HSTR_PUSH_CSTRING(L, "flyState", "attacking"); break;
				case CHoverAirMoveType::FLY_LANDING:   HSTR_PUSH_CSTRING(L, "flyState", "landing");   break;
			}
			HSTR_PUSH_NUMBER(L, "goalDistance", b.goalDistance);
			HSTR_PUSH_BOOL(L, "bankingAllowed", b.bankingAllowed);
			HSTR_PUSH_NUMBER(L, "currentBank", b.currentBank);
			HSTR_PUSH_NUMBER(L, "currentPitch", b.currentPitch);
			HSTR_PUSH_NUMBER(L, "turnRate", b.turnRate);
			HSTR_PUSH_NUMBER(L, "accRate", b.accRate);
			HSTR_PUSH_NUMBER(L, "decRate", b.decRate);
			HSTR_PUSH_NUMBER(L, "altitudeRate", b.altitudeRate);
			HSTR_PUSH_NUMBER(L, "brakeDistance", -1.0f); // DEPRECATED
			HSTR_PUSH_BOOL(L, "dontLand", b.dontLand);   // == GetAllowLanding()
			HSTR_PUSH_NUMBER(L, "maxDrift", b.maxDrift);
			return 1;
		}
		case 3: { // strafe air (airplane)
			HSTR_PUSH_CSTRING(L, "name", "airplane");
			pushAircraftState(b.aircraftState);
			HSTR_PUSH_NUMBER(L, "wantedHeight", b.wantedHeight);
			HSTR_PUSH_BOOL(L, "collide", b.collide);
			HSTR_PUSH_BOOL(L, "useSmoothMesh", b.useSmoothMesh);
			HSTR_PUSH_NUMBER(L, "myGravity", b.myGravity);
			HSTR_PUSH_NUMBER(L, "maxBank", b.maxBank);
			HSTR_PUSH_NUMBER(L, "maxPitch", b.maxBank);
			HSTR_PUSH_NUMBER(L, "turnRadius", b.turnRadius);
			HSTR_PUSH_NUMBER(L, "maxAcc", b.accRate);
			HSTR_PUSH_NUMBER(L, "maxAileron", b.maxAileron);
			HSTR_PUSH_NUMBER(L, "maxElevator", b.maxElevator);
			HSTR_PUSH_NUMBER(L, "maxRudder", b.maxRudder);
			return 1;
		}
		case 4: { // static
			HSTR_PUSH_CSTRING(L, "name", "static");
			return 1;
		}
		case 5: { // script
			HSTR_PUSH_CSTRING(L, "name", "script");
			return 1;
		}
		default: break;
	}

	HSTR_PUSH_CSTRING(L, "name", "unknown");
	return 1;
}

// ---- unit collision-volume / last-hit-piece twins (PR 33 unit piece cache) ----
// mirror of LuaSyncedRead::GetUnitCollisionVolumeData (ParseInLosUnit)
int LuaSnapshotServe::GetUnitCollisionVolumeData(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseInLosUnitPieceSlot(L, caller);
	if (slot == nullptr)
		return 0;
	return LuaUtils::PushColVolData(L, &slot->colVol);
}

// mirror of LuaSyncedRead::GetUnitPieceCollisionVolumeData (ParseInLosUnit +
// PushPieceCollisionVolumeData)
int LuaSnapshotServe::GetUnitPieceCollisionVolumeData(lua_State* L, const char* caller)
{
	const ObjectPieceSlot* slot = ParseInLosUnitPieceSlot(L, caller);
	if (slot == nullptr)
		return 0;
	const int pieceIdx = ParsePieceIndex(L, slot);
	if (pieceIdx < 0)
		return 0;
	return LuaUtils::PushColVolData(L, &slot->pieces[pieceIdx].pieceColVol);
}

// mirror of LuaSyncedRead::GetUnitLastAttackedPiece (ParseAllyUnit gate, batch-1
// amendment) over GetSolidObjectLastHitPiece
int LuaSnapshotServe::GetUnitLastAttackedPiece(lua_State* L, const char* caller)
{
	return ServeLastHitPiece(L, ParseAllyUnitPieceSlot(L, caller));
}

// ---- IsUnitInLos / InAirLos / InJammer unit variants (batch-1 reassignment) ----
// The stride rows hold the computed losHandler->InLos/InAirLos/InJammer(unit, at)
// answers; the twins mirror the live bodies incl. their GetEffectiveLosAllyTeam
// helper (which can raise "Invalid allyTeam" exactly like live).

// GetEffectiveLosAllyTeam mirror shared by the three; returns the resolved
// allyTeam or raises argerror. Mirrors ServeEffectiveLosAllyTeam but inlined
// against the numAllyTeams the rows carry.
namespace {
	int UnitLosEffectiveAllyTeam(lua_State* L, const Pov& pov, int numAllyTeams, int arg)
	{
		if (lua_isnoneornil(L, arg))
			return pov.readAllyTeam;

		const int aat = luaL_optint(L, arg, CEventClient::MinSpecialTeam - 1);

		if (aat == CEventClient::NoAccessTeam)
			return aat;

		if (pov.fullRead) {
			if (aat >= 0 && aat < numAllyTeams)
				return aat;
			if (aat == CEventClient::AllAccessTeam)
				return aat;
		} else {
			if (aat == pov.readAllyTeam)
				return aat;
		}

		return luaL_argerror(L, arg, "Invalid allyTeam");
	}
}

int LuaSnapshotServe::IsUnitInLos(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int allyTeamID = UnitLosEffectiveAllyTeam(L, pov, rows.numAllyTeams, 2);
	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, rows.UnitInLos(unitID, allyTeamID));
	return 1;
}

int LuaSnapshotServe::IsUnitInAirLos(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int allyTeamID = UnitLosEffectiveAllyTeam(L, pov, rows.numAllyTeams, 2);
	if (allyTeamID < 0) {
		lua_pushboolean(L, (allyTeamID == CEventClient::AllAccessTeam));
		return 1;
	}

	lua_pushboolean(L, rows.UnitInAirLos(unitID, allyTeamID));
	return 1;
}

int LuaSnapshotServe::IsUnitInJammer(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseTypedUnit mirror
	if (!rows.Valid(unitID) || !rows.PovUnitTyped(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	const int allyTeamID = UnitLosEffectiveAllyTeam(L, pov, rows.numAllyTeams, 2);
	if (allyTeamID < 0) {
		luaL_argerror(L, 2, "Invalid allyTeam");
		return 0;
	}

	lua_pushboolean(L, rows.UnitInJammer(unitID, allyTeamID));
	return 1;
}


/******************************************************************************
 * Weapon trace tests -- served DRAW-SIDE via trace::EpochView
 * (TRACE REHOST, doc/sim-draw-trace-rehost-plan.md; supersedes the PR 35
 * sim-side query/reply channel, retired in stage 4b).
 *
 * The four trace tests -- GetUnitWeapon{TryTarget,TestTarget,TestRange,
 * HaveFreeLineOfFire} -- run the SAME templated predicate stack the sim runs
 * (trace:: / WeaponPredicates.h), instantiated with EpochView (SimSnapshot
 * per-weapon SoA + UnitRows + demand piece-cache collision volumes + the
 * object-free CCollisionHandler backend + CGround mirrors) instead of live
 * CWeapon/CUnit pointers. The PR 35 channel deferred the draw-side recompute
 * because the full ballistics+collision subsystem had to be mirrored; stages
 * 1-4b did exactly that (read-set rows, WeaponPredicates.h, TraceEpochView.h,
 * per-piece IntersectPieceTree). RouteTraceQuery:
 *
 *   flag-OFF unarmed: runs the LIVE predicate inline -> bit-identical.
 *   flag-OFF armed  : dual-runs live vs EpochView -> proves epoch == live.
 *   flag-ON         : evaluates EpochView synchronously (no queue). The verdict
 *                     is at the CURRENT query position vs <=1-boundary-old world
 *                     (the recorded deviation swap -- the inverse of the retired
 *                     channel's exact-state / stale-position).
 *
 * This block keeps the arg-parse (BuildTraceQuery) + the packed WeaponTraceQuery
 * POD (now just the parsed-args carrier for the synchronous epoch eval, not a
 * reply-map key). Return shapes mirror the live bodies exactly (1 boolean); bad-
 * arg Lua errors raise identically. Still advisory-UI only -- the authoritative
 * synced targeting re-runs sim-side at fire time, unchanged. TestTarget/TestRange/
 * TryTarget are bit-identical to live; HaveFreeLineOfFire carries only the
 * inherent <=1-frame ground-staleness / pick-grid-broadphase advisory deviation.
 ******************************************************************************/

namespace {
	// Packed POD trace query -- the reply-map key. All fields are 4 bytes (no
	// padding) so the whole struct hashes/compares byte-wise. Unused fields are
	// zeroed at build time so two identical widget calls produce an identical key.
	struct WeaponTraceQuery {
		int32_t kind;       // LuaSnapshotServe::TraceKind
		int32_t ownerID;
		int32_t weaponNum;  // already range-validated against weaponCount at build
		int32_t enemyID;    // -1 == no enemy (pos form / ground form)
		int32_t variant;    // TryTarget/TestTarget/TestRange: 0 enemy-form, 1 pos-form
		                    // HaveFreeLineOfFire: the raw lua_gettop (3/5/6/8)
		int32_t srcMask;    // HFLOF: bit i set => srcPos[i] came from an arg (else GetAimFromPos)
		float px, py, pz;   // TT/TeT/TeR pos-form pos; HFLOF srcPos arg values
		float qx, qy, qz;   // HFLOF tgtPos (case 8; else 0)
	};
	static_assert(sizeof(WeaponTraceQuery) == 12 * sizeof(int32_t), "WeaponTraceQuery must be padding-free for byte-wise hash/eq");

	// Build a trace query from the Lua args, applying the SAME POV gates and
	// return-shape as the live body but sourced from the snapshot (owner
	// ParseAllyUnit -> PovAlliedUnit, enemy ParseUnit -> PovUnitVisible, weaponNum
	// range from weaponCount). Returns 1 with `q` filled, or 0 to reject (the
	// caller then returns 0, exactly master's "no such object" shape). Raises the
	// same Lua errors as the live body's ParseRawUnit / luaL_checkint on bad args.
	int BuildTraceQuery(lua_State* L, const char* caller, LuaSnapshotServe::TraceKind kind, WeaponTraceQuery& q)
	{
		using TK = LuaSnapshotServe::TraceKind;
		const auto& rows = simSnapshot.Read();
		const Pov pov = HandlePov(L);

		q = WeaponTraceQuery{};
		q.kind = static_cast<int32_t>(kind);
		q.enemyID = -1;

		// ParseAllyUnit(arg1) mirror: number check (may raise) + PovAlliedUnit gate
		const int unitID = ParseUnitIDSynced(L, caller, 1);
		if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
			return 0;
		q.ownerID = unitID;

		// weaponNum: luaL_checkint may raise; range-check against weaponCount
		const size_t weaponNum = luaL_checkint(L, 2) - LUA_WEAPON_BASE_INDEX;
		if (weaponNum >= static_cast<size_t>(rows.weaponCount[unitID]))
			return 0;
		q.weaponNum = static_cast<int32_t>(weaponNum);

		// enemy ParseUnit(argN) mirror: number check (may raise) + PovUnitVisible gate
		const auto parseVisibleEnemy = [&](int idx) -> bool {
			const int enemyID = ParseUnitIDSynced(L, caller, idx);
			if (!rows.Valid(enemyID) || !rows.PovUnitVisible(enemyID, pov.readAllyTeam, pov.fullRead))
				return false;
			q.enemyID = enemyID;
			return true;
		};

		if (kind == TK::HaveFreeLineOfFire) {
			const int top = lua_gettop(L);
			q.variant = top;
			switch (top) {
				case 3: { // [3] := targetID
					if (!parseVisibleEnemy(3))
						return 0;
				} break;
				case 5: { // [3,4,5] := srcPos
					// ParsePos over srcPos (default = GetAimFromPos, resolved sim-side)
					if (!lua_isnoneornil(L, 3)) { q.srcMask |= 1; q.px = luaL_optnumber(L, 3, 0.0f); }
					if (!lua_isnoneornil(L, 4)) { q.srcMask |= 2; q.py = luaL_optnumber(L, 4, 0.0f); }
					if (!lua_isnoneornil(L, 5)) { q.srcMask |= 4; q.pz = luaL_optnumber(L, 5, 0.0f); }
				} break;
				case 6: { // [3,4,5] := srcPos, [6] := targetID
					if (!lua_isnoneornil(L, 3)) { q.srcMask |= 1; q.px = luaL_optnumber(L, 3, 0.0f); }
					if (!lua_isnoneornil(L, 4)) { q.srcMask |= 2; q.py = luaL_optnumber(L, 4, 0.0f); }
					if (!lua_isnoneornil(L, 5)) { q.srcMask |= 4; q.pz = luaL_optnumber(L, 5, 0.0f); }
					if (!parseVisibleEnemy(6))
						return 0;
				} break;
				case 8: { // [3,4,5] := srcPos, [6,7,8] := tgtPos
					if (!lua_isnoneornil(L, 3)) { q.srcMask |= 1; q.px = luaL_optnumber(L, 3, 0.0f); }
					if (!lua_isnoneornil(L, 4)) { q.srcMask |= 2; q.py = luaL_optnumber(L, 4, 0.0f); }
					if (!lua_isnoneornil(L, 5)) { q.srcMask |= 4; q.pz = luaL_optnumber(L, 5, 0.0f); }
					// tgtPos default is the float3() zero, so no sim-side default needed
					q.qx = luaL_optnumber(L, 6, 0.0f);
					q.qy = luaL_optnumber(L, 7, 0.0f);
					q.qz = luaL_optnumber(L, 8, 0.0f);
				} break;
				default: return 0; // matches the live switch's default { return 0; }
			}
			return 1;
		}

		// TryTarget / TestTarget / TestRange: pos form iff top >= 5, else enemy@arg3
		if (lua_gettop(L) >= 5) {
			q.variant = 1; // pos form
			q.px = luaL_optnumber(L, 3, 0.0f);
			q.py = luaL_optnumber(L, 4, 0.0f);
			q.pz = luaL_optnumber(L, 5, 0.0f);
		} else {
			q.variant = 0; // enemy form
			if (!parseVisibleEnemy(3))
				return 0;
		}
		return 1;
	}

	// Evaluate a trace query against the published EPOCH state -- the SAME templated
	// trace:: predicates (WeaponPredicates.h) as the live body, instantiated over
	// trace::EpochView (SimSnapshot rows + mirrors + object-free collision backend)
	// instead of live CWeapon/CUnit pointers. Mirrors the live predicate control
	// flow exactly -- the derived positions (GetUnitLeadTargetPos / GetAimFromPos)
	// are recomputed epoch-side by the same templated chain. This IS the served
	// path flag-ON (RouteTraceQuery, synchronous) and the snap leg of the armed
	// flag-off dual-run (the gate proves epoch == live). Returns the boolean.
	//
	// TRACE REHOST (stage 4b): the trace::EpochView collision/cone/ground/piece-tree
	// primitives are wired bit-exact for the scalar/collision half; TestTarget/
	// TestRange/TryTarget are bit-identical to live and HaveFreeLineOfFire carries
	// only the inherent <=1-frame advisory ground/broadphase deviation (the same
	// draw-side class placement/pick-grid already carry).
	bool EvaluateTraceQueryEpoch(const WeaponTraceQuery& q)
	{
		using TK = LuaSnapshotServe::TraceKind;

		const auto& urows = simSnapshot.Read();
		if (!urows.Valid(q.ownerID))
			return false;
		if (q.weaponNum < 0 || static_cast<size_t>(q.weaponNum) >= static_cast<size_t>(urows.weaponCount[q.ownerID]))
			return false;

		trace::EpochView view(q.ownerID, q.weaponNum);

		const bool enemyForm = (q.enemyID >= 0);

		// mirror SWeaponTarget(enemy, pos, true): type = enemy ? Unit : Pos;
		// isUserTarget = true; groundPos = enemy ? p : Zero (Target_Unit never reads
		// groundPos, so its value is immaterial in enemy form -- see the ctor).
		const auto makeTarget = [&](const float3& p) {
			trace::EpochView::Target t;
			t.type = enemyForm ? Target_Unit : Target_Pos;
			t.isUserTarget = true;
			t.isAutoTarget = false;
			t.isManualFire = false;
			t.unitID = q.enemyID;
			t.groundPos = enemyForm ? p : ZeroVector;
			return t;
		};

		switch (static_cast<TK>(q.kind)) {
			case TK::TryTarget: {
				// an enemy-form query whose target vanished since enqueue -> "no target"
				if (enemyForm && !urows.Valid(q.enemyID))
					return false;
				// pos is (0,0,0) in enemy form (GetLeadTargetPos ignores it)
				const float3 pos = (q.variant == 1) ? float3(q.px, q.py, q.pz) : ZeroVector;
				return trace::TryTargetT(view, makeTarget(pos));
			}
			case TK::TestTarget:
			case TK::TestRange: {
				if (enemyForm && !urows.Valid(q.enemyID))
					return false;
				const float3 pos = (q.variant == 1) ? float3(q.px, q.py, q.pz)
				                                    : trace::GetUnitLeadTargetPosT(view, q.enemyID);
				return (static_cast<TK>(q.kind) == TK::TestTarget)
					? trace::TestTargetT(view, pos, makeTarget(pos))
					: trace::TestRangeT (view, pos, makeTarget(pos));
			}
			case TK::HaveFreeLineOfFire: {
				if ((q.variant == 3 || q.variant == 6) && !urows.Valid(q.enemyID))
					return false;
				float3 srcPos = trace::GetAimFromPosT(view, false);
				float3 tgtPos;
				if (q.srcMask & 1) srcPos.x = q.px;
				if (q.srcMask & 2) srcPos.y = q.py;
				if (q.srcMask & 4) srcPos.z = q.pz;
				switch (q.variant) {
					case 3: tgtPos = trace::GetUnitLeadTargetPosT(view, q.enemyID); break;
					case 5: /* tgtPos stays zero */ break;
					case 6: tgtPos = trace::GetUnitLeadTargetPosT(view, q.enemyID); break;
					case 8: tgtPos = float3(q.qx, q.qy, q.qz); break;
					default: return false;
				}
				return trace::HaveFreeLineOfFireT(view, srcPos, tgtPos, makeTarget(tgtPos));
			}
		}
		return false;
	}
}


// sim|draw split (Stage 0): served draw-side weapon range-ring primitives. Mirror
// the live glBallisticCircle CWeapon* path (CWeapon::GetLiveRange2D + weaponDef->
// heightmod) over the published epoch, so the GuiHandler range rings can drop their
// sim park. GetRange2DT is the exact predicate the sim runs; its epoch-vs-live
// bit-exactness is already gated by the TestRange trace dual-run (checked=0 mismatch).
float LuaSnapshotServe::SplitServedWeaponRange2D(int unitID, int weaponNum, float modHeightDiff)
{
	const auto& urows = simSnapshot.Read();
	if (!urows.Valid(unitID))
		return 0.0f;
	if (weaponNum < 0 || static_cast<size_t>(weaponNum) >= static_cast<size_t>(urows.weaponCount[unitID]))
		return 0.0f;

	trace::EpochView view(unitID, weaponNum);
	return trace::GetRange2DT(view, 0.0f, modHeightDiff);
}

float LuaSnapshotServe::SplitServedWeaponHeightMod(int unitID, int weaponNum)
{
	const auto& urows = simSnapshot.Read();
	if (!urows.Valid(unitID))
		return 0.0f;
	if (weaponNum < 0 || static_cast<size_t>(weaponNum) >= static_cast<size_t>(urows.weaponCount[unitID]))
		return 0.0f;

	trace::EpochView view(unitID, weaponNum);
	return view.Def()->heightmod;
}


/******************************************************************************
 * PR 38g (Batch-4 P1): sanctioned-tail serving twins. GetUnitEstimatedPath
 * (UnitRows est-path block), GetFeatureFireTime/SmokeTime (FeatureRows fire/
 * smoke timers), GetProjectileDamages (ProjectileRows DamagesSnap). Each mirrors
 * its live body's parse-gate POV + return shape over the snapshot rows.
 ******************************************************************************/

// mirror of LuaPathFinder::PushPathNodes over the extracted waypoint vectors:
// same two-table shape (points as 1-indexed {x,y,z} sub-tables, starts as
// 1-indexed starts[i]+1) and same 2-return arity
static int PushPathNodesFromSnap(lua_State* L, const std::vector<float3>& points, const std::vector<int32_t>& starts)
{
	const int pointCount = static_cast<int>(points.size());
	const int startCount = static_cast<int>(starts.size());

	lua_createtable(L, pointCount, 0);
	for (int i = 0; i < pointCount; i++) {
		lua_createtable(L, 3, 0);
		lua_pushnumber(L, points[i].x); lua_rawseti(L, -2, 1);
		lua_pushnumber(L, points[i].y); lua_rawseti(L, -2, 2);
		lua_pushnumber(L, points[i].z); lua_rawseti(L, -2, 3);
		lua_rawseti(L, -2, i + 1);
	}

	lua_createtable(L, startCount, 0);
	for (int i = 0; i < startCount; i++) {
		lua_pushnumber(L, starts[i] + 1);
		lua_rawseti(L, -2, i + 1);
	}

	return 2;
}

// mirror of LuaSyncedRead::GetUnitEstimatedPath (ParseAllyUnit gate; a
// dynamic_cast<CGroundMoveType> gate; PushPathNodes' pathID==0 early return)
int LuaSnapshotServe::GetUnitEstimatedPath(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.Read();
	const int unitID = ParseUnitIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseAllyUnit mirror -- always-present rows, run BEFORE any park. Dead /
	// enemy / non-ground ids return 0 here with NO park and NO registration,
	// which structurally avoids the pieces re-park storm (only a valid allied
	// GROUND unit not yet captured can reach the first-touch park below).
	if (!rows.Valid(unitID) || !rows.PovAlliedUnit(unitID, pov.readAllyTeam, pov.fullRead))
		return 0;

	// dynamic_cast<CGroundMoveType> == nullptr mirror (moveTypeKind==1 iff ground)
	if (rows.moveTypeKind[unitID] != 1)
		return 0;

	// WS-6 demand gate: this id's est-path was captured into the held slot ->
	// serve from the rows exactly as before.
	if (rows.estPathCaptured[unitID]) {
		// PushPathNodes' pathID==0 early return mirror (no tables)
		if (rows.estPathHasPath[unitID] == 0)
			return 0;

		return PushPathNodesFromSnap(L, rows.estPathPoints[unitID], rows.estPathStarts[unitID]);
	}

	// FIRST TOUCH of a valid allied ground unit not yet captured in this slot:
	// register it for the producer's future edges, then read the LIVE path under
	// a counted park (the sim is quiesced, so GetPathWayPoints on the live
	// CGroundMoveType is safe -- same model as the pieces first-touch live reads)
	// and serve THIS frame's value directly. A unit that died after the epoch
	// edge but before this touch serves a miss for that one dispatch window
	// (the enumerated pieces first-touch deviation, within contract).
	{
		std::lock_guard<std::mutex> lk(estPathRegMtx);
		pendingEstPathRegs.push_back(unitID);
	}

	CGame::ScopedExternalSimPause park{CGame::SimPauseSite::EST_PATH_FIRST_TOUCH};

	const CUnit* u = unitHandler.GetUnit(unitID);
	if (u == nullptr || u->isDead)
		return 0;

	const CGroundMoveType* g = dynamic_cast<const CGroundMoveType*>(u->moveType);
	if (g == nullptr)
		return 0;

	const unsigned int pathID = g->GetPathID();
	if (pathID == 0)
		return 0;

	std::vector<float3> points;
	std::vector<int> starts;
	pathManager->GetPathWayPoints(pathID, points, starts);
	return PushPathNodesFromSnap(L, points, starts);
}

// mirror of LuaSyncedRead::GetFeatureFireTime (ParseFeature POV; fireTime * INV_GAME_SPEED)
int LuaSnapshotServe::GetFeatureFireTime(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.fireTime[featureID] * INV_GAME_SPEED);
	return 1;
}

// mirror of LuaSyncedRead::GetFeatureSmokeTime (ParseFeature POV; smokeTime * INV_GAME_SPEED)
int LuaSnapshotServe::GetFeatureSmokeTime(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadFeatures();
	const int featureID = ParseFeatureIDSynced(L, caller, 1);
	const Pov pov = HandlePov(L);

	// ParseFeature mirror
	if (!rows.Valid(featureID) || !PovFeatureVisible(rows, featureID, pov))
		return 0;

	lua_pushnumber(L, rows.smokeTime[featureID] * INV_GAME_SPEED);
	return 1;
}

// mirror of LuaSyncedRead::GetProjectileDamages (ParseProjectile POV + isWeapon
// gate; the live body forces arg 2 to a string via luaL_checkstring before
// PushDamagesKey(*wpro->damages, 2), and a weapon projectile's damages is never null)
int LuaSnapshotServe::GetProjectileDamages(lua_State* L, const char* caller)
{
	const auto& rows = simSnapshot.ReadProjectiles();
	const int projID = luaL_checkint(L, 1);
	const Pov pov = HandlePov(L);

	if (!rows.Valid(projID) || !rows.PovVisible(projID, pov.readAllyTeam, pov.fullRead))
		return 0;
	if (!rows.isWeapon[projID])
		return 0;

	// valid==0 would be the live "damages == nullptr" nil shape; a weapon
	// projectile always has non-null damages, so this never fires in practice
	if (rows.damages[projID].valid == 0)
		return 0;

	luaL_checkstring(L, 2); // mirror the live body's arg-2 enforcement (converts a numeric key slot in place)
	return PushDamagesKeySnap(L, rows.damages[projID], 2);
}


int LuaSnapshotServe::RouteTraceQuery(lua_State* L, const char* caller, ServeFn liveFn, TraceKind kind)
{
	// non-draw context (synced gadget on the sim thread, sim-phase call): the
	// live state is owned here, run the predicate directly -- identical to master
	if (!ShouldServe(L))
		return liveFn(L, caller);

	// pregame: no sim thread yet, live tables exist, snapshot does not -- serve
	// live under the sanctioned live-exception bracket (the Route() precedent)
	if (simSnapshot.HeldEpochId() == 0) {
		LuaSplitContract::ScopedLiveException prePublishFallback;
		return liveFn(L, caller);
	}

	const bool splitRunning =
		SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning() && !SimDrawSplit::IsSimParked();

	if (splitRunning) {
		// flag-ON: the sim thread owns live state -- NEVER touch it from draw
		// context. TRACE REHOST (stage 4): every trace callout is evaluated
		// draw-side against the published epoch, synchronously (no queue) -- the
		// SAME templated trace:: predicates the sim runs, over trace::EpochView.
		// Current query position vs <=1-boundary-old world (the recorded deviation
		// swap -- the inverse of the retired channel's exact-state / stale-position).
		WeaponTraceQuery q;
		if (BuildTraceQuery(L, caller, kind, q) == 0)
			return 0;
		lua_pushboolean(L, EvaluateTraceQueryEpoch(q));
		return 1;
	}

	// flag-OFF from draw context: single-threaded, the sim is parked relative to
	// draw, so the live predicate IS the sim-side evaluation (sim==draw thread).
	// It must be the served value (flag-off bit-identity). Under a live-exception
	// bracket so the dress-rehearsal contract gates don't nil the live read.
	if (!snapshotDiffGate.Armed()) {
		LuaSplitContract::ScopedLiveException simParkedInline;
		return liveFn(L, caller);
	}

	// armed flag-OFF gate: dual-run for coverage of the query-record construction
	// + evaluation. Live leg = master predicate; snap leg = the SAME query built
	// and evaluated against the published EPOCH (EvaluateTraceQueryEpoch), so this
	// dual-run proves epoch == live bit-for-bit (TRACE REHOST stage 4a: was a
	// live-vs-live reconstruction check). Serve the LIVE result: flag-off must be
	// bit-identical, the epoch path is verified here, not served.
	const int base = lua_gettop(L);

	int liveN = 0;
	{
		LuaSplitContract::ScopedLiveException liveLeg;
		liveN = liveFn(L, caller);
	}

	// stash the live returns, reset the stack to the original args
	lua_createtable(L, liveN, 0);
	for (int i = 1; i <= liveN; ++i) {
		lua_pushvalue(L, base + i);
		lua_rawseti(L, -2, i);
	}
	const int liveRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_settop(L, base);

	// snap leg: build + evaluate the query inline (single-threaded, live objects).
	// Its returns sit at base+1 .. base+snapN.
	WeaponTraceQuery q;
	int snapN = 0;
	if (BuildTraceQuery(L, caller, kind, q) != 0) {
		lua_pushboolean(L, EvaluateTraceQueryEpoch(q));
		snapN = 1;
	}

	// compare return counts + the boolean slot (live values from the stashed table)
	bool equal = (liveN == snapN);
	char detail[160] = "";
	if (!equal) {
		snprintf(detail, sizeof(detail), "return counts differ: live=%d snap=%d", liveN, snapN);
	} else if (liveN == 1) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, liveRef); // live-returns table on top
		lua_rawgeti(L, -1, 1);                       // live return 1
		const bool le = (lua_toboolean(L, -1) != 0);
		const bool se = (lua_toboolean(L, base + 1) != 0);
		if (le != se) {
			equal = false;
			snprintf(detail, sizeof(detail), "return 1 differs: live=%s snap=%s", le ? "true" : "false", se ? "true" : "false");
		}
		lua_pop(L, 2); // live return 1 + the table
	}

	snapshotDiffGate.CountCallout(caller, equal, detail);

	// serve the LIVE returns (flag-off bit-identity): drop the snap returns, push
	// the stashed live values, then release the stash.
	lua_settop(L, base);
	lua_rawgeti(L, LUA_REGISTRYINDEX, liveRef); // live-returns table on top
	for (int i = 1; i <= liveN; ++i)
		lua_rawgeti(L, base + 1, i);            // push live value i above the table
	lua_remove(L, base + 1);                    // drop the table, leaving the liveN values
	luaL_unref(L, LUA_REGISTRYINDEX, liveRef);
	return liveN;
}


/******************************************************************************
 * Placement build/move tests -- served DRAW-SIDE via placement::EpochView
 * (PLACEMENT REHOST, doc/sim-draw-placement-rehost-plan.md §10; supersedes the
 * PR 38e sim-side query/reply channel, retired in stage 4).
 *
 * The last three sanctioned placement callouts -- TestBuildOrder / TestMoveOrder /
 * ClosestBuildPos -- run the SAME templated placement predicate stack the sim runs
 * (placement:: / movemath::), instantiated with EpochView (DrawMapMirrors +
 * SimSnapshot rows + immutable def data) instead of LiveView. The "one
 * implementation, two backends" rehost the PR 38e channel deferred: the terrain
 * speedmod arrays (center/max height, slope, centerNormals2D), buildingMaskMap,
 * yardmapStatusEffectsMap and the FULL per-cell blocking object list are all
 * mirrored (stage 1), so the draw-side recompute is bit-exact, not an
 * approximation. RoutePlacementQuery:
 *
 *   flag-OFF unarmed: runs the LIVE body inline -> bit-identical.
 *   flag-OFF armed  : dual-runs live vs EpochView -> proves epoch == live.
 *   flag-ON         : evaluates EpochView synchronously (no queue). The verdict
 *                     is at the CURRENT query position vs <=1-boundary-old world
 *                     (the recorded deviation swap -- the inverse of the retired
 *                     channel's exact-state / stale-position).
 *
 * This block keeps the arg-parse (BuildPlacementQuery) + reply-format
 * (PushPlacementReply) helpers -- the packed PlacementQuery POD is now just the
 * parsed-args carrier for the synchronous epoch eval, not a reply-map key.
 * Return shapes mirror the live bodies exactly (TMO: 1 boolean; TBO: 1 number, or
 * 2 with the blocking feature id; CBP: 3 numbers); bad-arg Lua errors raise
 * identically (same arg reads, same order). Still advisory-UI only -- the
 * authoritative synced placement test re-runs at command execution, unchanged.
 ******************************************************************************/

namespace {
	// Packed POD placement query -- the reply-map key. All members are 4 bytes
	// (int32_t/float) so the whole struct hashes/compares byte-wise with no
	// padding; unused-per-kind fields are zeroed at build time so two identical
	// widget calls produce an identical key (the PR 35 WeaponTraceQuery pattern).
	struct PlacementQuery {
		int32_t kind;         // LuaSnapshotServe::PlacementKind
		int32_t defID;        // TMO/TBO unitDefID; CBP udefID (arg 2)
		int32_t teamID;       // CBP team (arg 1); 0 otherwise
		int32_t readAllyTeam; // POV: TMO los gate + TBO TestUnitBuildSquare allyTeam
		int32_t fullRead;     // POV bool (TMO los gate); 0/1
		int32_t synced;       // handle synced flag (always 0 for a served handle); 0/1
		int32_t facing;       // TBO/CBP buildFacing
		int32_t minDistance;  // CBP
		int32_t flags;        // TMO: bit0 testTerrain, bit1 testObjects, bit2 centerOnly
		float px, py, pz;     // pos (all kinds)
		float dx, dy, dz;     // TMO dir
		float searchRadius;   // CBP
	};
	static_assert(sizeof(PlacementQuery) == 16 * sizeof(int32_t), "PlacementQuery must be padding-free for byte-wise hash/eq");

	// The sim-exact reply for one query. retCount is the Lua return count master
	// would push; i0/i1 carry integer/id results, f0/f1/f2 the float triple.
	struct PlacementReply {
		int32_t retCount;   // 1 (TMO / TBO-no-feature), 2 (TBO+feature), 3 (CBP)
		int32_t i0;         // TMO: bool(0/1); TBO: BUILDSQUARE_* status
		int32_t i1;         // TBO: blocking feature id (only when retCount==2)
		float f0, f1, f2;   // CBP buildPos x/y/z
	};


	// push a reply onto the Lua stack in the live callout's exact shape.
	int PushPlacementReply(lua_State* L, LuaSnapshotServe::PlacementKind kind, const PlacementReply& r)
	{
		using PK = LuaSnapshotServe::PlacementKind;
		switch (kind) {
			case PK::TestMoveOrder:
				lua_pushboolean(L, r.i0 != 0);
				return 1;
			case PK::TestBuildOrder:
				lua_pushnumber(L, r.i0);
				if (r.retCount == 2) {
					lua_pushnumber(L, r.i1);
					return 2;
				}
				return 1;
			case PK::ClosestBuildPos:
				lua_pushnumber(L, r.f0);
				lua_pushnumber(L, r.f1);
				lua_pushnumber(L, r.f2);
				return 3;
		}
		return 0;
	}

	// Build a placement query from the Lua args, reproducing the live body's arg
	// parse order (and its Lua errors) and the pure-def early-outs that read only
	// immutable UnitDef/MoveDef data (answered inline draw-side, never enqueued --
	// no staleness for the "invalid/immobile def" cases). Returns 1 with `q` filled
	// (enqueue + serve the last reply), or 0 having filled `inlineReply` (a def
	// early-out already resolved the answer). POV (readAllyTeam/fullRead/synced) is
	// captured into the query so the barrier evaluates the same gate as the live body.
	int BuildPlacementQuery(lua_State* L, const char* caller, LuaSnapshotServe::PlacementKind kind,
	                        PlacementQuery& q, PlacementReply& inlineReply)
	{
		using PK = LuaSnapshotServe::PlacementKind;
		const Pov pov = HandlePov(L);

		q = PlacementQuery{};
		q.kind = static_cast<int32_t>(kind);
		q.readAllyTeam = pov.readAllyTeam;
		q.fullRead = pov.fullRead ? 1 : 0;
		q.synced = CLuaHandle::GetHandleSynced(L) ? 1 : 0;

		switch (kind) {
			case PK::TestMoveOrder: {
				const int unitDefID = luaL_checkint(L, 1);
				const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(unitDefID);
				if (unitDef == nullptr || unitDef->pathType == -1u) {
					inlineReply = PlacementReply{}; inlineReply.retCount = 1; inlineReply.i0 = 0; // false
					return 0;
				}
				const MoveDef* moveDef = moveDefHandler.GetMoveDefByPathType(unitDef->pathType);
				if (moveDef == nullptr) {
					inlineReply = PlacementReply{}; inlineReply.retCount = 1;
					inlineReply.i0 = unitDef->IsImmobileUnit() ? 0 : 1; // !IsImmobileUnit()
					return 0;
				}
				q.defID = unitDefID;
				q.px = luaL_checkfloat(L, 2); q.py = luaL_checkfloat(L, 3); q.pz = luaL_checkfloat(L, 4);
				q.dx = luaL_optfloat(L, 5, 0.0f); q.dy = luaL_optfloat(L, 6, 0.0f); q.dz = luaL_optfloat(L, 7, 0.0f);
				if (luaL_optboolean(L, 8, true))   q.flags |= 1; // testTerrain
				if (luaL_optboolean(L, 9, true))   q.flags |= 2; // testObjects
				if (luaL_optboolean(L, 10, false)) q.flags |= 4; // centerOnly
				return 1;
			}
			case PK::TestBuildOrder: {
				const int unitDefID = luaL_checkint(L, 1);
				const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(unitDefID);
				if (unitDef == nullptr) {
					inlineReply = PlacementReply{}; inlineReply.retCount = 1; inlineReply.i0 = 0; // 0
					return 0;
				}
				// live parse order: ParseFacing(arg 5) BEFORE the pos floats (2,3,4)
				q.facing = LuaUtils::ParseFacing(L, caller, 5);
				q.defID = unitDefID;
				q.px = luaL_checkfloat(L, 2); q.py = luaL_checkfloat(L, 3); q.pz = luaL_checkfloat(L, 4);

				// PR 43 §7.3: canonicalize the key to the BUILD GRID CELL. The
				// verdict is grid-quantized -- CGameHelper::Pos2BuildPos snaps
				// x/z to the 16-elmo build grid and recomputes y from terrain
				// (the input y is ignored; GetBuildHeight reads pos.x/pos.z
				// only) -- so within-cell cursor motion must map to the SAME
				// key. The snap replicates Pos2BuildPos' x/z arithmetic over
				// immutable def data (footprint parity folded through the
				// facing, BuildInfo::GetXSize/GetZSize); it is idempotent, so
				// evaluating the snapped query equals evaluating the raw one.
				{
					BuildInfo bi(unitDef, float3(q.px, q.py, q.pz), q.facing);

					if (bi.GetXSize() & 2)
						q.px = math::floor((q.px              ) / BUILD_SQUARE_SIZE) * BUILD_SQUARE_SIZE + SQUARE_SIZE;
					else
						q.px = math::floor((q.px + SQUARE_SIZE) / BUILD_SQUARE_SIZE) * BUILD_SQUARE_SIZE;

					if (bi.GetZSize() & 2)
						q.pz = math::floor((q.pz              ) / BUILD_SQUARE_SIZE) * BUILD_SQUARE_SIZE + SQUARE_SIZE;
					else
						q.pz = math::floor((q.pz + SQUARE_SIZE) / BUILD_SQUARE_SIZE) * BUILD_SQUARE_SIZE;

					q.py = 0.0f;
				}
				return 1;
			}
			case PK::ClosestBuildPos: {
				// live parse order: 1,2,6,7,8,3,4,5
				q.teamID = luaL_checkint(L, 1);
				q.defID = luaL_checkint(L, 2);
				q.searchRadius = luaL_checkfloat(L, 6);
				q.minDistance = static_cast<int>(luaL_checkfloat(L, 7));
				q.facing = luaL_checkint(L, 8);
				q.px = luaL_checkfloat(L, 3); q.py = luaL_checkfloat(L, 4); q.pz = luaL_checkfloat(L, 5);
				return 1;
			}
		}
		return 1;
	}

	// PLACEMENT REHOST (stage 3c): mirrors CMoveMath::RangeIsBlocked ->
	// RangeIsBlockedMt exactly (step-2 footprint walk, OR-fold ObjectBlockType,
	// early-exit on BLOCK_STRUCTURE) but over the full-cell mirror instead of the
	// live blocking map. mtTempNum cross-square dedup is dropped -- ObjectBlockType
	// is OR-idempotent and the early-exit fires at the same object, so the result
	// is identical (proven by the armed dual-run). FOOTPRINT_[XZ]STEP == 2 (mirrors
	// the file-local constants in MoveMath.cpp).
	CMoveMath::BlockType EpochRangeIsBlocked(const placement::EpochView& view, int xmin, int xmax, int zmin, int zmax,
	                                         const MoveTypes::CheckCollisionQuery* collider)
	{
		constexpr int FOOTPRINT_STEP = 2;
		xmin = std::max(xmin, 0);
		zmin = std::max(zmin, 0);
		xmax = std::min(xmax, mapDims.mapx - 1);
		zmax = std::min(zmax, mapDims.mapy - 1);

		CMoveMath::BlockType ret = CMoveMath::BLOCK_NONE;
		for (int z = zmin; z <= zmax; z += FOOTPRINT_STEP) {
			for (int x = xmin; x <= xmax; x += FOOTPRINT_STEP) {
				const int n = view.FullCellCount(x, z);
				for (int i = 0; i < n; i++) {
					const placement::EpochView::Occ o = view.FullCellObj(x, z, i);
					if (((ret |= movemath::ObjectBlockTypeT(view, o, collider)) & CMoveMath::BLOCK_STRUCTURE) == 0)
						continue;
					return ret;
				}
			}
		}
		return ret;
	}

	// epoch reimplementation of the boolean result of MoveDef::TestMoveSquare(Range)
	// (the served TestMoveOrder path: minSpeedMod/maxBlockBit ptrs are null, thread 0).
	// The leaf verdicts (GetPosSpeedModT / ObjectBlockTypeT) are the SAME templates
	// the sim runs; only the terrain/blocking data source differs (mirror vs live).
	bool EpochTestMoveSquare(const placement::EpochView& view, const MoveDef& moveDef,
	                         const float3& pos, const float3& dir,
	                         bool testTerrain, bool testObjects, bool centerOnly)
	{
		// collider with the epoch elevation (CheckCollisionQuery(moveDef, pos) but
		// fed the mirror maxHeight instead of the live readMap)
		MoveTypes::CheckCollisionQuery collider(&moveDef);
		collider.pos = pos.cClampInBounds();
		{
			const int2 sqr{int(collider.pos.x / SQUARE_SIZE), int(collider.pos.z / SQUARE_SIZE)};
			collider.UpdateElevationForPos(sqr, view.MaxHeightAtSquare(sqr.y * mapDims.mapx + sqr.x));
		}

		const int xmid = int(pos.x / SQUARE_SIZE);
		const int zmid = int(pos.z / SQUARE_SIZE);
		const int xmin = xmid - moveDef.xsizeh * (1 - centerOnly);
		const int zmin = zmid - moveDef.zsizeh * (1 - centerOnly);
		const int xmax = xmid + moveDef.xsizeh * (1 - centerOnly);
		const int zmax = zmid + moveDef.zsizeh * (1 - centerOnly);

		bool retTestMove = true;

		if (testTerrain) {
			const bool dirPath = collider.moveDef->allowDirectionalPathing;
			for (int z = zmin; retTestMove && z <= zmax; ++z) {
				for (int x = xmin; retTestMove && x <= xmax; ++x) {
					const float speedMod = dirPath
						? movemath::GetPosSpeedModT(view, moveDef, x, z, dir)
						: movemath::GetPosSpeedModT(view, moveDef, x, z);
					retTestMove = (speedMod > 0.0f);
				}
			}
		}

		if (testObjects && retTestMove) {
			const CMoveMath::BlockType blockBits = EpochRangeIsBlocked(view, xmin, xmax, zmin, zmax, &collider);
			retTestMove = ((blockBits & CMoveMath::BLOCK_STRUCTURE) == 0);
		}

		return retTestMove;
	}

	// Evaluate a placement query against the published EPOCH (draw-side): the same
	// templated placement predicates the sim runs with LiveView, instantiated with
	// EpochView (DrawMapMirrors + SimSnapshot rows). Synchronous, no queue -- the
	// reply reflects the CURRENT query position against <=1-boundary-old world
	// state (the recorded deviation swap). Served flag-ON; the armed flag-OFF
	// dual-run compares this against the live callout body for bit equality.
	PlacementReply EvaluatePlacementQueryEpoch(const PlacementQuery& q)
	{
		using PK = LuaSnapshotServe::PlacementKind;
		PlacementReply r = {};

		switch (static_cast<PK>(q.kind)) {
			case PK::TestMoveOrder: {
				r.retCount = 1;
				const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(q.defID);
				if (unitDef == nullptr || unitDef->pathType == -1u) { r.i0 = 0; return r; }
				const MoveDef* moveDef = moveDefHandler.GetMoveDefByPathType(unitDef->pathType);
				if (moveDef == nullptr) { r.i0 = unitDef->IsImmobileUnit() ? 0 : 1; return r; }

				const float3 pos(q.px, q.py, q.pz);
				const float3 dir(q.dx, q.dy, q.dz);

				placement::EpochView view;
				const bool los = (q.readAllyTeam < 0) ? (q.fullRead != 0)
				                                      : view.InLos(pos, q.readAllyTeam);
				bool ret = false;
				if (los) {
					ret = EpochTestMoveSquare(view, *moveDef, pos, dir,
						(q.flags & 1) != 0, (q.flags & 2) != 0, (q.flags & 4) != 0);
				}
				r.i0 = ret ? 1 : 0;
				return r;
			}
			case PK::TestBuildOrder: {
				const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(q.defID);
				if (unitDef == nullptr) { r.retCount = 1; r.i0 = 0; return r; }

				placement::EpochView view;

				BuildInfo bi;
				bi.buildFacing = q.facing;
				bi.def = unitDef;
				bi.pos = { q.px, q.py, q.pz };
				// synced=false (draw-safe unsynced heightmap) + epoch currHeightBounds
				bi.pos = CGameHelper::Pos2BuildPos(bi, false, &view.heightBounds);

				int featureId = -1;
				int retval = placement::TestUnitBuildSquareT(view, bi, featureId, q.readAllyTeam, false);

				// live back-compat map: BUILDSQUARE_OPEN -> BUILDSQUARE_RECLAIMABLE
				if (retval == CGameHelper::BUILDSQUARE_OPEN)
					retval = CGameHelper::BUILDSQUARE_RECLAIMABLE;

				r.i0 = retval;
				if (featureId < 0) { r.retCount = 1; return r; }
				r.retCount = 2; r.i1 = featureId;
				return r;
			}
			case PK::ClosestBuildPos: {
				r.retCount = 3;
				placement::EpochView view;
				const float3 buildPos = placement::ClosestBuildPosT(view,
					q.teamID, unitDefHandler->GetUnitDefByID(q.defID),
					float3(q.px, q.py, q.pz), q.searchRadius, q.minDistance, q.facing, false);
				r.f0 = buildPos.x; r.f1 = buildPos.y; r.f2 = buildPos.z;
				return r;
			}
			// all PlacementKind values are handled above; unreachable
			default:
				return r;
		}
	}

}


int LuaSnapshotServe::RoutePlacementQuery(lua_State* L, const char* caller, ServeFn liveFn, PlacementKind kind)
{
	// non-draw context (synced gadget on the sim thread, sim-phase call): the
	// live state is owned here, run the body directly -- identical to master
	if (!ShouldServe(L))
		return liveFn(L, caller);

	// pregame: no sim thread yet, live tables exist, snapshot does not -- serve
	// live under the sanctioned live-exception bracket (the Route() precedent)
	if (simSnapshot.HeldEpochId() == 0) {
		LuaSplitContract::ScopedLiveException prePublishFallback;
		return liveFn(L, caller);
	}

	const bool splitRunning =
		SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning() && !SimDrawSplit::IsSimParked();

	if (splitRunning) {
		// flag-ON: the sim thread owns live terrain/blocking state -- NEVER touch it
		// from draw context. PLACEMENT REHOST (stage 3/4): every placement callout is
		// evaluated draw-side against the published epoch, synchronously (no queue).
		// Current query position vs <=1-boundary-old world -- the recorded deviation swap.
		PlacementQuery q;
		PlacementReply reply;
		if (BuildPlacementQuery(L, caller, kind, q, reply) != 0)
			reply = EvaluatePlacementQueryEpoch(q);
		return PushPlacementReply(L, kind, reply);
	}

	// flag-OFF from draw context: single-threaded, the sim is parked relative to
	// draw, so the live predicate IS the sim-side evaluation (sim==draw thread).
	// It must be the served value (flag-off bit-identity). Under a live-exception
	// bracket so the dress-rehearsal contract gates don't nil the live read.
	if (!snapshotDiffGate.Armed()) {
		LuaSplitContract::ScopedLiveException simParkedInline;
		return liveFn(L, caller);
	}

	// armed flag-OFF gate: dual-run for coverage of the query build + barrier
	// evaluation. Live leg = the master body; snap leg = the SAME query built and
	// evaluated synchronously inline (live-exact by construction, so it passes
	// trivially -- and any reconstruction bug is caught here). Serve the LIVE
	// result: flag-off must be bit-identical, the query path is verified, not served.
	const int base = lua_gettop(L);

	int liveN = 0;
	{
		LuaSplitContract::ScopedLiveException liveLeg;
		liveN = liveFn(L, caller);
	}

	// stash the live returns, reset the stack to the original args
	lua_createtable(L, liveN, 0);
	for (int i = 1; i <= liveN; ++i) {
		lua_pushvalue(L, base + i);
		lua_rawseti(L, -2, i);
	}
	const int liveRef = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_settop(L, base);

	// snap leg: build + evaluate the query inline (single-threaded, live objects).
	// Its returns sit at base+1 .. base+snapN.
	int snapN = 0;
	{
		PlacementQuery q;
		PlacementReply reply;
		if (BuildPlacementQuery(L, caller, kind, q, reply) != 0) {
			// PLACEMENT REHOST (stage 3/4): the snap leg is the EPOCH evaluation, so
			// this dual-run proves epoch == live bit-for-bit (was a live-vs-live
			// reconstruction check).
			reply = EvaluatePlacementQueryEpoch(q);
		}
		snapN = PushPlacementReply(L, kind, reply);
	}

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

	// serve the LIVE returns (flag-off bit-identity): drop everything above the
	// args, push the stashed live values, then release the stash.
	lua_settop(L, base);
	lua_rawgeti(L, LUA_REGISTRYINDEX, liveRef); // live-returns table on top
	for (int i = 1; i <= liveN; ++i)
		lua_rawgeti(L, base + 1, i);            // push live value i above the table
	lua_remove(L, base + 1);                    // drop the table, leaving the liveN values
	luaL_unref(L, LUA_REGISTRYINDEX, liveRef);
	return liveN;
}

/******************************************************************************
 * PR 38f -- event-time command-queue presentation (specs "PR-38 event-time
 * mechanism generalization"; the DEAD_THIS_BATCH dispatch-drain sibling).
 *
 * Capture (fire time, sim thread owns the queue) + per-unit override install
 * (drain time, main thread) around the deferred UnitCommand/UnitCmdDone
 * handler; see the header block for the full contract. UNSYNCED / draw-only.
 ******************************************************************************/

namespace {
	// Builds a COMPLETE event-time slot from a unit -- the unconditional
	// analogue of RefreshCommandQueues' per-unit body (queue + descs + factory
	// newUnitCommands + bugger-off scalars + worker task). Every command-queue
	// twin (not just GetUnitCommands) reads the override through GetCmdQueueSlot,
	// so all fields must be event-time-consistent, not just the queue. Keep in
	// sync with RefreshCommandQueues above.
	void BuildCmdQueueSlotFromUnit(UnitCmdQueueSlot& slot, const CUnit* unit)
	{
		const CCommandAI* cai = unit->commandAI; // never null

		slot.present = true;
		slot.allyTeam = unit->allyteam;
		slot.isFactoryCAI = (dynamic_cast<const CFactoryCAI*>(cai) != nullptr);
		slot.isFactoryUnit = (dynamic_cast<const CFactory*>(unit) != nullptr);

		CopyQueueSnap(cai->commandQue, slot.commandQue, slot.commandQueParams);
		slot.cmdQueVersion = cai->commandQue.GetVersion();

		CopyDescsSnap(cai->GetPossibleCommands(), slot.descs);
		slot.cmdDescVersion = cai->GetCmdDescVersion();

		if (slot.isFactoryCAI) {
			const CFactoryCAI* fcai = static_cast<const CFactoryCAI*>(cai);
			CopyQueueSnap(fcai->newUnitCommands, slot.newUnitCommands, slot.newUnitCommandsParams);
			slot.newUnitCmdsVersion = fcai->newUnitCommands.GetVersion();
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

		ResolveWorkerTask(unit, slot);
	}
}

std::shared_ptr<void> LuaSnapshotServe::CaptureCmdQueueEvent(const CUnit* unit)
{
	// Only capture when the dispatch will actually defer (split on AND in the
	// sim phase). Flag-off / immediate dispatch runs the handler synchronously
	// against the live queue -- no override needed, byte-identical.
	if (unit == nullptr || !SimDrawSplit::DeferUnsyncedNow())
		return nullptr;

	auto snap = std::make_shared<UnitCmdQueueSlot>();
	BuildCmdQueueSlotFromUnit(*snap, unit);
	// shared_ptr<UnitCmdQueueSlot> -> shared_ptr<void>: the typed deleter is
	// retained, so the slot (and its vectors) frees correctly even after the
	// closure that owns it is dropped (unregistered client at drain).
	return snap;
}

LuaSnapshotServe::ScopedCmdQueueEventOverride::ScopedCmdQueueEventOverride(int unitID, const std::shared_ptr<void>& snap)
	: prevSlot(cmdEvtOverrideSlot)
	, prevUnitID(cmdEvtOverrideUnitID)
{
	if (snap != nullptr) {
		cmdEvtOverrideSlot = static_cast<const UnitCmdQueueSlot*>(snap.get());
		cmdEvtOverrideUnitID = unitID;
	}
}

LuaSnapshotServe::ScopedCmdQueueEventOverride::~ScopedCmdQueueEventOverride()
{
	cmdEvtOverrideSlot = static_cast<const UnitCmdQueueSlot*>(prevSlot);
	cmdEvtOverrideUnitID = prevUnitID;
}


/******************************************************************************
 * PR 38j -- top-of-callout event-time override consults.
 *
 * A deferred UnitCommand/UnitCmdDone / UnitLeftLos handler runs at the barrier
 * under a live exception, so Route() serves the LIVE leg and never consults the
 * event-time overrides installed around the handler (38f/38g). These predicates
 * let the specific affected callouts detect, at their top and INDEPENDENT of the
 * live exception, that arg#1's unit currently has an override installed, and
 * serve it via the snapshot twin (where the override lives) for that ONE unit --
 * without the whole-handler strict enforcement 38h wrongly imposed (reverted).
 *
 * Both fast-reject on their inert global before touching the lua stack. The
 * overrides are only ever installed while a deferred handler dispatches at the
 * barrier (main thread, post-publish); flag-off and the armed diff-gate dual-run
 * install nothing (they dispatch immediately at fire time), so these return
 * false there and the callouts fall through to the normal Route() ->
 * byte-identical. The Generation()!=0 guard is defensive: the snapshot has
 * always published by the time an override can be installed.
 ******************************************************************************/

bool LuaSnapshotServe::CmdQueueEventOverrideActive(lua_State* L)
{
	if (cmdEvtOverrideSlot == nullptr) // no deferred command event dispatching
		return false;
	if (!lua_isnumber(L, 1))
		return false;

	return (lua_toint(L, 1) == cmdEvtOverrideUnitID) && (simSnapshot.HeldEpochId() != 0);
}

bool LuaSnapshotServe::LosEventOverrideActive(lua_State* L)
{
	if (!SimSnapshotLosEvent::Installed()) // no deferred UnitLeftLos dispatching
		return false;
	if (!lua_isnumber(L, 1))
		return false;

	return SimSnapshotLosEvent::ActiveForUnit(lua_toint(L, 1)) && (simSnapshot.HeldEpochId() != 0);
}
