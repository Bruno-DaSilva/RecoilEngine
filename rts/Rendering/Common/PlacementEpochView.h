/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * EpochView -- the DRAW-side state backend for the placement predicate stack
 * (sim/draw split placement rehost, stage 3). The epoch instantiation of the same
 * placement:: / movemath:: templated predicates the sim thread runs with LiveView:
 * terrain/map reads come from the published DrawMapMirrors, occupant attributes
 * from the SimSnapshot Unit/Feature rows joined by id, and immutable footprint /
 * yardmap / reclaimable / speedmod data from the (thread-safe) def handlers.
 *
 * Occupant is an {id, kind} pair (kind = DrawMapMirrors::BLOCK_KIND_UNIT/FEATURE),
 * never a live pointer. Constructed once per served query; caches the row buffers
 * + the boundary-consistent currHeightBounds. Read-only, one-boundary-stale by
 * design -- the armed dual-run (flag-off) proves it bit-equal to LiveView.
 */

#include <cstdint>

#include "DrawMapMirrors.h"
#include "SimSnapshot.h"
#include "SnapshotPickGrid.h"

#include "Game/GameHelper.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Sim/MoveTypes/MoveMath/MoveMathPredicates.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "System/float3.h"
#include "System/type2.h"

namespace placement {

struct EpochView {
	struct Occ {
		int32_t id = -1;
		uint8_t kind = DrawMapMirrors::BLOCK_KIND_NONE;
	};
	using Occupant = Occ;

	const SimSnapshot::UnitRows& urows;
	const SimSnapshot::FeatureRows& frows;
	float2 heightBounds;

	EpochView()
		: urows(simSnapshot.Read())
		, frows(simSnapshot.ReadFeatures())
	{
		const auto& g = simSnapshot.ReadGlobals();
		heightBounds = float2{g.currMinHeight, g.currMaxHeight};
	}

	// ---- terrain / map (DrawMapMirrors) ----
	float ApproxHeightUnsafe(int sqx, int sqz, bool /*synced*/) const { return drawMapMirrors.ApproxHeightUnsafe(sqx, sqz); }
	float Slope(float x, float z, bool /*synced*/) const { return drawMapMirrors.Slope(x, z); }
	bool  BuildingMaskTest(int hx, int hz, uint16_t mask) const { return drawMapMirrors.BuildingMaskTest(hx, hz, mask); }
	bool  YardBlockBuilding(int x, int z) const { return drawMapMirrors.YardStatusAnyFlags(x, z, YardmapStatusEffectsMap::BLOCK_BUILDING); }
	bool  InLos(const float3& pos, int allyTeam) const { return drawMapMirrors.PosInLos(pos, allyTeam); }
	float BuildHeight(const float3& pos, const UnitDef* def, bool /*synced*/) const {
		// synced=false path reads the draw-safe unsynced corner heightmap; the
		// currHeightBounds override routes the sim-mutable clamp through the epoch
		return CGameHelper::GetBuildHeight(pos, def, false, &heightBounds);
	}
	float MaxHeightAtSquare(int square) const { return drawMapMirrors.MaxHeightAtSquare(square); }
	float SlopeAtIndex(int square) const { return drawMapMirrors.SlopeAtIndex(square); }
	float3 CenterNormal2DAtIndex(int idx) const { return drawMapMirrors.CenterNormal2DAtSquare(idx); }
	int   TypeMapAt(int square) const { return drawMapMirrors.TypeMapAt(square); }
	const DrawMapMirrors::TerrainType& TerrainType(int idx) const { return drawMapMirrors.TerrainTypeAt(idx); }

	bool HasNearbyGeoFeature(const float3& testPos, int mindx, int mindz, float searchRadius) const {
		// draw-side broadphase over the snapshot feature positions (conservative
		// superset), re-filtered by geoThermal + the exact dx/dz test (== live)
		snapshotPickGrid.EnsureCurrent();
		std::vector<int> featIDs;
		snapshotPickGrid.QueryFeaturesInRadius(testPos, searchRadius, featIDs);
		for (int fid : featIDs) {
			const FeatureDef* fd = featureDefHandler->GetFeatureDefByID(frows.DefID(fid));
			if (fd == nullptr || !fd->geoThermal)
				continue;
			const float3 fpos = frows.Pos(fid);
			const float dx = math::fabs(fpos.x - testPos.x);
			const float dz = math::fabs(fpos.z - testPos.z);
			if (dx < mindx && dz < mindz)
				return true;
		}
		return false;
	}

	// ---- blocking occupant ----
	Occ GroundBlocked(int x, int z) const { uint8_t k; const int id = drawMapMirrors.BlockedAt(x, z, k); return Occ{id, k}; }
	int FullCellCount(int x, int z) const { return drawMapMirrors.FullCellCount(x, z); }
	Occ FullCellObj(int x, int z, int i) const { uint8_t k; const int id = drawMapMirrors.FullCellObj(x, z, i, k); return Occ{id, k}; }

	// ---- occupant attributes (rows joined by id; def data read directly) ----
	bool OccNull(Occ o) const { return o.id < 0 || o.kind == DrawMapMirrors::BLOCK_KIND_NONE; }
	bool OccIsFeature(Occ o) const { return o.kind == DrawMapMirrors::BLOCK_KIND_FEATURE; }
	int  OccId(Occ o) const { return o.id; }

	bool OccFeatureInLos(Occ o, int at) const { return frows.IsInLosForAllyTeam(o.id, at); }
	bool OccFeatureReclaimable(Occ o) const {
		const FeatureDef* fd = featureDefHandler->GetFeatureDefByID(frows.DefID(o.id));
		return fd != nullptr && fd->reclaimable;
	}
	bool OccUnitInLos(Occ o, int at) const { return (urows.LosStatus(o.id, at) & LOS_INLOS) != 0; }

	bool OccImmobile(Occ o) const { return OccIsFeature(o) ? true : urows.Immobile(o.id); }
	bool OccYardOpen(Occ o) const { return OccIsFeature(o) ? false : urows.YardOpen(o.id); }

	const YardMapStatus* OccBlockMap(Occ o) const {
		// only immobile units (buildings) override GetBlockMap -> def yardmap ptr;
		// mobile units and features return the base nullptr
		if (OccIsFeature(o) || !urows.Immobile(o.id))
			return nullptr;
		const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.DefID(o.id));
		return (ud != nullptr) ? ud->GetYardMapPtr() : nullptr;
	}

	float3 OccPos(Occ o) const { return OccIsFeature(o) ? frows.Pos(o.id) : urows.Pos(o.id); }
	float  OccHeight(Occ o) const { return OccIsFeature(o) ? frows.Height(o.id) : urows.Height(o.id); }
	int    OccBuildFacing(Occ o) const { return OccIsFeature(o) ? frows.BuildFacing(o.id) : urows.BuildFacing(o.id); }

	int OccXsize(Occ o) const {
		const int bf = OccBuildFacing(o);
		return ((bf & 1) == 0) ? defXsize(o) : defZsize(o);
	}
	int OccZsize(Occ o) const {
		const int bf = OccBuildFacing(o);
		return ((bf & 1) == 0) ? defZsize(o) : defXsize(o);
	}

	uint16_t OccPhysState(Occ o) const { return OccIsFeature(o) ? frows.PhysicalState(o.id) : urows.PhysicalState(o.id); }
	bool OccIsInWater(Occ o) const { return (OccPhysState(o) & CSolidObject::PSTATE_BIT_INWATER) != 0; }
	bool OccIsUnderWater(Occ o) const { return (OccPhysState(o) & CSolidObject::PSTATE_BIT_UNDERWATER) != 0; }
	bool OccIsMoving(Occ o) const { return (OccPhysState(o) & CSolidObject::PSTATE_BIT_MOVING) != 0; }

	uint8_t OccBlockingBits(Occ o) const { return OccIsFeature(o) ? frows.BlockingBits(o.id) : urows.BlockingBits(o.id); }
	bool OccIsBlocking(Occ o) const { return (OccBlockingBits(o) & (1u << 0)) != 0; }
	bool OccHasSolidObjectsBit(Occ o) const { return (OccBlockingBits(o) & (1u << 1)) != 0; }
	bool OccCrushable(Occ o) const { return (OccBlockingBits(o) & (1u << 4)) != 0; }
	float OccCrushResistance(Occ o) const { return OccIsFeature(o) ? frows.CrushResistance(o.id) : urows.CrushResistance(o.id); }

	bool OccIsIdle(Occ o) const { return urows.IsIdle(o.id); }            // only reached for mobile units
	bool OccIsPushResistant(Occ o) const { return urows.IsPushResistant(o.id); }

	const MoveDef* OccMoveDef(Occ o) const {
		if (OccIsFeature(o))
			return nullptr;
		const int pt = urows.MoveDefID(o.id);
		return (pt >= 0) ? moveDefHandler.GetMoveDefByPathType(pt) : nullptr;
	}

	// the placement collider is built from (moveDef, pos): it has no owning unit
	bool OccIsColliderSelf(Occ /*o*/, const MoveTypes::CheckCollisionQuery* /*collider*/) const { return false; }

	// build-path leaf: the epoch instantiation of the movemath template
	bool IsNonBlocking(Occ o, const MoveTypes::CheckCollisionQuery* q) const { return movemath::IsNonBlockingT(*this, o, q); }

private:
	int defXsize(Occ o) const {
		if (OccIsFeature(o)) {
			const FeatureDef* fd = featureDefHandler->GetFeatureDefByID(frows.DefID(o.id));
			return (fd != nullptr) ? fd->xsize : 1;
		}
		const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.DefID(o.id));
		return (ud != nullptr) ? ud->xsize : 1;
	}
	int defZsize(Occ o) const {
		if (OccIsFeature(o)) {
			const FeatureDef* fd = featureDefHandler->GetFeatureDefByID(frows.DefID(o.id));
			return (fd != nullptr) ? fd->zsize : 1;
		}
		const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.DefID(o.id));
		return (ud != nullptr) ? ud->zsize : 1;
	}
};

} // namespace placement
