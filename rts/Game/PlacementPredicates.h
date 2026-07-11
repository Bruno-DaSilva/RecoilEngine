/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * PlacementPredicates -- the build/move placement predicate stack as templated
 * pure functions over an abstract state View (sim/draw split placement rehost,
 * stage 2). One implementation, two backends: LiveView (globals + live
 * CSolidObject*) and, in stage 3, EpochView (draw-side mirrors + snapshot rows).
 *
 * The existing CGameHelper / CMoveMath / MoveDef member functions become thin
 * wrappers that call these with a LiveView, so every caller keeps its exact
 * signature and the LiveView instantiation inlines to today's code (byte-identical
 * -- these run in synced pathfinding every frame). See
 * doc/sim-draw-placement-rehost-stage2-design.md for the binding design.
 *
 * Occupant is View::Occupant: const CSolidObject* for LiveView, an id+kind pair
 * for EpochView. Every occupant dereference goes through a view accessor, so the
 * templated bodies below never include Unit.h/Feature.h -- those live only where
 * LiveView's accessors are defined (bottom of this header).
 */

#include <algorithm>

#include "Game/GameHelper.h"
#include "Map/Ground.h"
#include "Map/MapDimensions.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/UnitDef.h"
#include "System/float3.h"
#include "System/type2.h"

namespace placement {

// ---------------------------------------------------------------------------
// Build-placement predicates (GameHelper.cpp today)
// ---------------------------------------------------------------------------

// CGameHelper::TestBlockSquareForBuildOnly
template<class V>
bool TestBlockSquareForBuildOnlyT(const V& view, typename V::Occupant so, const int2 yardpos)
{
	bool ret = false;

	// check whether the current building allows for building in the given square.
	auto soYardMap = view.OccBlockMap(so);
	if (soYardMap != nullptr) {
		const float3 soPos = view.OccPos(so);
		const int soXs = view.OccXsize(so);
		const int soZs = view.OccZsize(so);
		const int sox1 = int(soPos.x / SQUARE_SIZE) - (soXs >> 1), sox2 = sox1 + soXs;
		const int soz1 = int(soPos.z / SQUARE_SIZE) - (soZs >> 1), soz2 = soz1 + soZs;
		const int2 soxrange = int2(sox1, sox2);
		const int2 sozrange = int2(soz1, soz2);

		auto soYmIdx = CGameHelper::GetYardMapIndex(view.OccBuildFacing(so), yardpos, soxrange, sozrange);
		if (soYardMap[soYmIdx] == YardmapStates::YARDMAP_BUILDONLY)
			// While the square is blocked for walking, it is open for building.
			ret = true;
	}

	return ret;
}

// CGameHelper::TestBuildSquare -- returns the verdict; featureId out-param carries
// the reclaimable blocking feature id (-1 if none), resolved to CFeature* by the
// LiveView wrapper.
template<class V>
CGameHelper::BuildSquareStatus TestBuildSquareT(
	const V& view,
	const float3& pos,
	const int2& xrange,
	const int2& zrange,
	const BuildInfo& buildInfo,
	const MoveDef* moveDef,
	int& featureId,
	int allyteam,
	bool synced
) {
	assert(pos.IsInBounds());

	const int sqx = unsigned(pos.x) / SQUARE_SIZE;
	const int sqz = unsigned(pos.z) / SQUARE_SIZE;

	const float groundHeight = view.ApproxHeightUnsafe(sqx, sqz, synced);
	const UnitDef* unitDef = buildInfo.def;

	if (!CGameHelper::CheckTerrainConstraints(unitDef, moveDef, pos.y, groundHeight, view.Slope(pos.x, pos.z, synced)))
		return CGameHelper::BUILDSQUARE_BLOCKED;

	if (!view.BuildingMaskTest(sqx >> 1, sqz >> 1, unitDef->buildingMask))
		return CGameHelper::BUILDSQUARE_BLOCKED;

	CGameHelper::BuildSquareStatus ret = CGameHelper::BUILDSQUARE_OPEN;
	const int yardxpos = unsigned(pos.x) / SQUARE_SIZE;
	const int yardypos = unsigned(pos.z) / SQUARE_SIZE;
	const int2 yardpos = { yardxpos, yardypos };
	const int ymIdx = CGameHelper::GetYardMapIndex(buildInfo.buildFacing, yardpos, xrange, zrange);

	if (view.YardBlockBuilding(sqx, sqz)) {
		bool isStackable = (!unitDef->yardmap.empty() && unitDef->yardmap[ymIdx] <= YardmapStates::YARDMAP_STACKABLE);
		if ( !isStackable && (synced || ((allyteam < 0) || view.InLos(pos, allyteam))) ) {
			return CGameHelper::BUILDSQUARE_BLOCKED;
		}
	}

	typename V::Occupant so = view.GroundBlocked(yardxpos, yardypos);

	if (!view.OccNull(so)) {
		const bool soIsFeature = view.OccIsFeature(so);

		if (soIsFeature) {
			if ((allyteam < 0) || view.OccFeatureInLos(so, allyteam)) {
				if (!view.OccFeatureReclaimable(so)) {
					ret = CGameHelper::BUILDSQUARE_BLOCKED;
				} else {
					ret = CGameHelper::BUILDSQUARE_RECLAIMABLE;
					featureId = view.OccId(so);
				}
			}
		} else {
			if ((allyteam < 0) || view.OccUnitInLos(so, allyteam)) {
				if (view.OccImmobile(so)) {
					bool isStackable = (!unitDef->yardmap.empty() && unitDef->yardmap[ymIdx] <= YardmapStates::YARDMAP_GEOSTACKABLE);
					ret = isStackable ? CGameHelper::BUILDSQUARE_OPEN :
							(TestBlockSquareForBuildOnlyT(view, so, yardpos) ? CGameHelper::BUILDSQUARE_OPEN : CGameHelper::BUILDSQUARE_BLOCKED);
				} else {
					ret = CGameHelper::BUILDSQUARE_OCCUPIED;
				}
			}
		}

		if (ret == CGameHelper::BUILDSQUARE_BLOCKED || ret == CGameHelper::BUILDSQUARE_OCCUPIED) {
			if (moveDef != nullptr) {
				MoveTypes::CheckCollisionQuery collisionQuery(moveDef, pos);
				if (view.IsNonBlocking(so, &collisionQuery))
					ret = CGameHelper::BUILDSQUARE_OPEN;
			}

			if (ret == CGameHelper::BUILDSQUARE_BLOCKED)
				return ret;
		}
	}

	return ret;
}

// CGameHelper::TestUnitBuildSquare (the canbuildpos/featurepos/nobuildpos +
// commands overload is unsynced UI only and is NOT served draw-side; this
// template covers the served (commands == nullptr) path -- the wrapper keeps the
// live body for the commands overload).
template<class V>
CGameHelper::BuildSquareStatus TestUnitBuildSquareT(
	const V& view,
	const BuildInfo& buildInfo,
	int& featureId,
	int allyteam,
	bool synced
) {
	featureId = -1;

	const int xsize = buildInfo.GetXSize();
	const int zsize = buildInfo.GetZSize();

	const float3 testPos = buildInfo.pos;
	      float3 sqrPos;

	const int x1 = int(testPos.x / SQUARE_SIZE) - (xsize >> 1), x2 = x1 + xsize;
	const int z1 = int(testPos.z / SQUARE_SIZE) - (zsize >> 1), z2 = z1 + zsize;
	const int2 xrange = int2(x1, x2);
	const int2 zrange = int2(z1, z2);

	const MoveDef* moveDef = (buildInfo.def->pathType != -1U) ? moveDefHandler.GetMoveDefByPathType(buildInfo.def->pathType) : nullptr;

	sqrPos.y = view.BuildHeight(testPos, buildInfo.def, synced);

	CGameHelper::BuildSquareStatus testStatus = CGameHelper::BUILDSQUARE_OPEN;

	if (buildInfo.def->needGeo) {
		testStatus = CGameHelper::BUILDSQUARE_BLOCKED;

		const int mindx = xsize * (SQUARE_SIZE >> 1) - (SQUARE_SIZE >> 1);
		const int mindz = zsize * (SQUARE_SIZE >> 1) - (SQUARE_SIZE >> 1);

		// look for a nearby geothermal feature if we need one
		if (view.HasNearbyGeoFeature(testPos, mindx, mindz, std::max(xsize, zsize) * 6))
			testStatus = CGameHelper::BUILDSQUARE_OPEN;
	}

	// NB: the synced-only quadField slow-update block (unit position refresh) is
	// intentionally absent here -- it runs only for synced==true (the live wrapper
	// keeps it); the served draw-side path is always synced==false.

	// out of map?
	if (static_cast<unsigned>(x1) > mapDims.mapx || static_cast<unsigned>(x2) > mapDims.mapx ||
		static_cast<unsigned>(z1) > mapDims.mapy || static_cast<unsigned>(z2) > mapDims.mapy) {
		return CGameHelper::BUILDSQUARE_BLOCKED;
	}

	for (int z = z1; z < z2; z++) {
		for (int x = x1; x < x2; x++) {
			sqrPos.x = x * SQUARE_SIZE;
			sqrPos.z = z * SQUARE_SIZE;

			const CGameHelper::BuildSquareStatus sqrStatus = TestBuildSquareT(view, sqrPos, xrange, zrange, buildInfo, moveDef, featureId, allyteam, synced);

			if ((testStatus = std::min(testStatus, sqrStatus)) == CGameHelper::BUILDSQUARE_BLOCKED) {
				return CGameHelper::BUILDSQUARE_BLOCKED;
			}
		}
	}

	return testStatus;
}

// CGameHelper::ClosestBuildPos -- DEFINED (with explicit LiveView + EpochView
// instantiations) in GameHelper.cpp, where the file-local GetSearchOffsetTable
// and teamHandler live. Declared here so LuaSnapshotServe can call the EpochView
// instantiation (linked against the GameHelper.cpp instantiation).
template<class V>
float3 ClosestBuildPosT(const V& view, int team, const UnitDef* unitDef, const float3& worldPos,
                        float searchRadius, int minDistance, int buildFacing, bool synced);

} // namespace placement

// ---------------------------------------------------------------------------
// LiveView -- the live-state backend (globals + live CSolidObject*). Every
// accessor is an inline forwarder to today's exact expression, so the LiveView
// instantiation of the templates above inlines to the original code. Occupant is
// a bare const CSolidObject*. Heavy sim includes live here (not in the templated
// bodies), so only the two TUs that instantiate placement predicates pull them in.
// ---------------------------------------------------------------------------

#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h" // PLACEMENT REHOST: ClosestBuildPos feature allyteam
#include "Sim/Misc/BuildingMaskMap.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/Units/Unit.h"

namespace placement {

struct LiveView {
	using Occupant = const CSolidObject*;

	// the quadField thread owner for the geo-feature search (served path passes it)
	int threadOwner = ThreadPool::GetThreadNum();

	// ---- terrain / map ----
	float ApproxHeightUnsafe(int sqx, int sqz, bool synced) const { return CGround::GetApproximateHeightUnsafe(sqx, sqz, synced); }
	float Slope(float x, float z, bool synced) const { return CGround::GetSlope(x, z, synced); }
	bool  BuildingMaskTest(int hx, int hz, uint16_t mask) const { return buildingMaskMap.TestTileMaskUnsafe(hx, hz, mask); }
	bool  YardBlockBuilding(int x, int z) const { return yardmapStatusEffectsMap.AreAnyFlagsSet(x, z, YardmapStatusEffectsMap::BLOCK_BUILDING); }
	bool  InLos(const float3& pos, int allyTeam) const { return losHandler->InLos(pos, allyTeam); }
	float BuildHeight(const float3& pos, const UnitDef* def, bool synced) const { return CGameHelper::GetBuildHeight(pos, def, synced); }
	// ---- ClosestBuildPos helpers ----
	float3 SnapBuildPos(const BuildInfo& bi) const { return CGameHelper::Pos2BuildPos(bi, false); }
	int FeatureAllyteam(int featureId) const {
		const CFeature* f = featureHandler.GetFeature(featureId);
		return (f != nullptr) ? f->allyteam : -1;
	}

	bool HasNearbyGeoFeature(const float3& testPos, int mindx, int mindz, float searchRadius) const {
		QuadFieldQuery qfQuery;
		qfQuery.threadOwner = threadOwner;
		quadField.GetFeaturesExact(qfQuery, testPos, searchRadius);
		for (const CFeature* f: *qfQuery.features) {
			if (!f->def->geoThermal)
				continue;
			const float dx = math::fabs(f->pos.x - testPos.x);
			const float dz = math::fabs(f->pos.z - testPos.z);
			if (dx < mindx && dz < mindz)
				return true;
		}
		return false;
	}

	// ---- blocking occupant (cell[0], build path) ----
	Occupant GroundBlocked(int x, int z) const { return groundBlockingObjectMap.GroundBlocked(x, z); }

	// ---- occupant attributes ----
	bool OccNull(Occupant o) const { return o == nullptr; }
	bool OccIsFeature(Occupant o) const { return dynamic_cast<const CFeature*>(o) != nullptr; }
	int  OccId(Occupant o) const {
		if (const CFeature* f = dynamic_cast<const CFeature*>(o)) return f->id;
		return static_cast<const CUnit*>(o)->id;
	}
	bool OccFeatureInLos(Occupant o, int at) const { return static_cast<const CFeature*>(o)->IsInLosForAllyTeam(at); }
	bool OccFeatureReclaimable(Occupant o) const { return static_cast<const CFeature*>(o)->def->reclaimable; }
	bool OccUnitInLos(Occupant o, int at) const { return (static_cast<const CUnit*>(o)->losStatus[at] & LOS_INLOS) != 0; }
	bool OccImmobile(Occupant o) const { return o->immobile; }
	bool OccYardOpen(Occupant o) const { return o->yardOpen; }
	const YardMapStatus* OccBlockMap(Occupant o) const { return o->GetBlockMap(); }
	float3 OccPos(Occupant o) const { return o->pos; }
	int OccXsize(Occupant o) const { return o->xsize; }
	int OccZsize(Occupant o) const { return o->zsize; }
	int OccBuildFacing(Occupant o) const { return o->buildFacing; }

	// ---- CMoveMath leaf (build path uses this view method; forwards to the
	// member, which is itself a wrapper over movemath::IsNonBlockingT<LiveView>) ----
	bool IsNonBlocking(Occupant o, const MoveTypes::CheckCollisionQuery* q) const { return CMoveMath::IsNonBlocking(o, q); }

	// ---- move-path occupant attributes (movemath:: leaf templates) ----
	bool OccIsColliderSelf(Occupant o, const MoveTypes::CheckCollisionQuery* collider) const { return collider->unit == o; }
	bool OccHasSolidObjectsBit(Occupant o) const { return o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_SOLIDOBJECTS); }
	bool OccIsBlocking(Occupant o) const { return o->IsBlocking(); }
	bool OccIsUnderWater(Occupant o) const { return o->IsUnderWater(); }
	bool OccIsInWater(Occupant o) const { return o->IsInWater(); }
	const MoveDef* OccMoveDef(Occupant o) const { return o->moveDef; }
	float OccHeight(Occupant o) const { return o->height; }
	bool OccCrushable(Occupant o) const { return o->crushable; }
	float OccCrushResistance(Occupant o) const { return o->crushResistance; }
	bool OccIsMoving(Occupant o) const { return o->IsMoving(); }
	bool OccIsPushResistant(Occupant o) const { return static_cast<const CUnit*>(o)->moveType->IsPushResistant(); }
	bool OccIsIdle(Occupant o) const { return static_cast<const CUnit*>(o)->IsIdle(); }

	// ---- move-path terrain reads (GetPosSpeedModT) ----
	int   TypeMapAt(int square) const { return readMap->GetTypeMapSynced()[square]; }
	float MaxHeightAtSquare(int square) const { return readMap->GetMaxHeightMapSynced()[square]; }
	float SlopeAtIndex(int square) const { return readMap->GetSlopeMapSynced()[square]; }
	float3 CenterNormal2DAtIndex(int idx) const { return readMap->GetCenterNormals2DSynced()[idx]; }
	const CMapInfo::TerrainType& TerrainType(int idx) const { return mapInfo->terrainTypes[idx]; }
};

} // namespace placement

