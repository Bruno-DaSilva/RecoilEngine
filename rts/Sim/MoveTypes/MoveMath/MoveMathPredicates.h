/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * MoveMathPredicates -- the move-placement / pathfinding LEAF verdict functions
 * (IsNonBlocking, CrushResistant, ObjectBlockType, GetPosSpeedMod) as templated
 * pure functions over an abstract state view (sim/draw split placement rehost,
 * stage 2b). One implementation, two backends: the CMoveMath members become thin
 * wrappers calling these with placement::LiveView (byte-identical), and the epoch
 * path (stage 3) calls them with EpochView over the DrawMapMirrors + snapshot rows.
 *
 * Only the LEAF verdicts are templatized -- the cell-iteration machinery
 * (SquareIsBlocked / RangeIsBlocked* / TestMoveSquare, incl. the mtTempNum
 * cross-square dedup) is UNCHANGED for the live path: it calls these leaves as
 * CMoveMath members, which now forward here. The epoch path gets its own
 * mirror-iterating cell walk (stage 3) that OR-folds ObjectBlockTypeT over the
 * full-cell mirror (ObjectBlockType is OR-idempotent, so mtTempNum is not needed).
 *
 * Occupant reads go through the view (view.OccXxx). The collider is the local
 * CheckCollisionQuery (moveDef + heightmap-derived pos), read directly. Terrain
 * reads for GetPosSpeedMod go through the view (TypeMapAt / MaxHeightAtSquare /
 * SlopeAtIndex / CenterNormal2DAtIndex / TerrainType); the pure *SpeedMod helpers
 * stay on CMoveMath (now public).
 */

#include "MoveMath.h"
#include "Map/MapDimensions.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "System/SpringMath.h"

namespace movemath {

// CMoveMath::IsNonBlocking
template<class V>
bool IsNonBlockingT(const V& view, typename V::Occupant collidee, const MoveTypes::CheckCollisionQuery* collider)
{
	if (view.OccIsColliderSelf(collidee, collider))
		return true;
	if (!view.OccHasSolidObjectsBit(collidee))
		return true;
	// if obstacle is out of map bounds, it cannot block us
	if (!view.OccPos(collidee).IsInBounds())
		return true;
	// same if obstacle is not currently marked on blocking-map
	if (!view.OccIsBlocking(collidee))
		return true;

	if ( !collider->IsHeightChecksEnabled() ) {
		const bool colliderIsSub = collider->moveDef->isSubmarine;
		const MoveDef* collideeMD = view.OccMoveDef(collidee);
		const bool collideeIsSub = collideeMD != nullptr && collideeMD->isSubmarine;

		if (colliderIsSub)
			return (!view.OccIsUnderWater(collidee) && !collideeIsSub);

		if (collider->moveDef->followGround)
			return false;

		return (view.OccIsUnderWater(collidee) || collideeIsSub);
	}

	if (collider->IsInWater() && view.OccIsInWater(collidee)) {
		float colliderHeight = (collider->moveDef != nullptr) ? collider->moveDef->height : math::fabs(collider->unit->height);
		if ((collider->pos.y + colliderHeight) < view.OccPos(collidee).y)
			return true;

		const MoveDef* collideeMD = view.OccMoveDef(collidee);
		float collideeHeight = (collideeMD != nullptr) ? collideeMD->height : math::fabs(view.OccHeight(collidee));
		if ((view.OccPos(collidee).y + collideeHeight) < collider->pos.y)
			return true;
	}
	return false;
}

// CMoveMath::CrushResistant
template<class V>
bool CrushResistantT(const V& view, const MoveDef& colliderMD, typename V::Occupant collidee)
{
	if (!view.OccHasSolidObjectsBit(collidee))
		return false;
	if (!view.OccCrushable(collidee))
		return true;

	return (view.OccCrushResistance(collidee) > colliderMD.crushStrength);
}

// CMoveMath::ObjectBlockType
template<class V>
CMoveMath::BlockType ObjectBlockTypeT(const V& view, typename V::Occupant collidee, const MoveTypes::CheckCollisionQuery* collider)
{
	if (IsNonBlockingT(view, collidee, collider))
		return CMoveMath::BLOCK_NONE;

	if (view.OccImmobile(collidee))
		return ((CrushResistantT(view, *(collider->moveDef), collidee))? CMoveMath::BLOCK_STRUCTURE: CMoveMath::BLOCK_NONE);

	// mobile obstacle, must be a unit
	// if moving, unit is probably following a path
	if (view.OccIsMoving(collidee))
		return CMoveMath::BLOCK_MOVING;

	// not moving and not pushable, treat as blocking
	if (view.OccIsPushResistant(collidee))
		return CMoveMath::BLOCK_STRUCTURE;

	// otherwise, unit is idling (no orders) or busy with a command
	return ((view.OccIsIdle(collidee))? CMoveMath::BLOCK_MOBILE: CMoveMath::BLOCK_MOBILE_BUSY);
}

// CMoveMath::GetPosSpeedMod (non-directional)
template<class V>
float GetPosSpeedModT(const V& view, const MoveDef& moveDef, unsigned xSquare, unsigned zSquare)
{
	if (xSquare >= mapDims.mapx || zSquare >= mapDims.mapy)
		return 0.0f;

	const int accurateSquare = xSquare + (zSquare * mapDims.mapx);
	const int square = (xSquare >> 1) + ((zSquare >> 1) * mapDims.hmapx);
	const int squareTerrType = view.TypeMapAt(square);

	const float height = view.MaxHeightAtSquare(accurateSquare);
	const float slope  = view.SlopeAtIndex(square);

	const auto& tt = view.TerrainType(squareTerrType);

	switch (moveDef.speedModClass) {
		case MoveDef::Tank:  { return (CMoveMath::GroundSpeedMod(moveDef, height, slope) * tt.tankSpeed ); } break;
		case MoveDef::KBot:  { return (CMoveMath::GroundSpeedMod(moveDef, height, slope) * tt.kbotSpeed ); } break;
		case MoveDef::Hover: { return ( CMoveMath::HoverSpeedMod(moveDef, height, slope) * tt.hoverSpeed); } break;
		case MoveDef::Ship:  { return (  CMoveMath::ShipSpeedMod(moveDef, height, slope) * tt.shipSpeed ); } break;
		default: {} break;
	}

	return 0.0f;
}

// CMoveMath::GetPosSpeedMod (directional)
template<class V>
float GetPosSpeedModT(const V& view, const MoveDef& moveDef, unsigned xSquare, unsigned zSquare, float3 moveDir)
{
	if (xSquare >= mapDims.mapx || zSquare >= mapDims.mapy)
		return 0.0f;

	const int accurateSquare = xSquare + (zSquare * mapDims.mapx);
	const int square = (xSquare >> 1) + ((zSquare >> 1) * mapDims.hmapx);
	const int squareTerrType = view.TypeMapAt(square);

	const float height = view.MaxHeightAtSquare(accurateSquare);
	const float slope  = view.SlopeAtIndex(square);

	const auto& tt = view.TerrainType(squareTerrType);

	const float3 sqrNormal = view.CenterNormal2DAtIndex(xSquare + zSquare * mapDims.mapx);

	// with a flat normal, only consider the normalized xz-direction
	assert(float3(moveDir).SafeNormalize2D() == moveDir);

	const float dirSlopeMod = -moveDir.dot(sqrNormal);

	switch (moveDef.speedModClass) {
		case MoveDef::Tank:  { return (CMoveMath::GroundSpeedMod(moveDef, height, slope, dirSlopeMod) * tt.tankSpeed ); } break;
		case MoveDef::KBot:  { return (CMoveMath::GroundSpeedMod(moveDef, height, slope, dirSlopeMod) * tt.kbotSpeed ); } break;
		case MoveDef::Hover: { return ( CMoveMath::HoverSpeedMod(moveDef, height, slope, dirSlopeMod) * tt.hoverSpeed); } break;
		case MoveDef::Ship:  { return (  CMoveMath::ShipSpeedMod(moveDef, height, slope, dirSlopeMod) * tt.shipSpeed ); } break;
		default: {} break;
	}

	return 0.0f;
}

} // namespace movemath
