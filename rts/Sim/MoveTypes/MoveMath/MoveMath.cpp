/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "MoveMath.h"
#include "MoveMathPredicates.h"          // PLACEMENT REHOST: templated leaf verdicts
#include "Game/PlacementPredicates.h"    // PLACEMENT REHOST: placement::LiveView

#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Units/Unit.h"
#include "System/Platform/Threading.h"

#include "System/Misc/TracyDefs.h"

bool CMoveMath::noHoverWaterMove = false;
float CMoveMath::waterDamageCost = 0.0f;

static constexpr int FOOTPRINT_XSTEP = 2;
static constexpr int FOOTPRINT_ZSTEP = 2;

MoveTypes::CheckCollisionQuery::CheckCollisionQuery(const CSolidObject* ref)
	: unit(ref)
	, moveDef(ref->moveDef)
	, pos(ref->pos)
	, physicalState(ref->physicalState)
{
	if (moveDef != nullptr)
		inExitOnlyZone = moveDef->IsInExitOnly(ref->pos);
}

MoveTypes::CheckCollisionQuery::CheckCollisionQuery(const MoveDef* refMoveDef, float3 testPos)
	: moveDef(refMoveDef)
	, pos(testPos.cClampInBounds())
{
	UpdateElevationForPos(pos);
}

void MoveTypes::CheckCollisionQuery::UpdateElevationForPos(int2 sqr) {
	UpdateElevationForPos(sqr, readMap->GetMaxHeightMapSynced()[sqr.y * mapDims.mapx + sqr.x]);
}

void MoveTypes::CheckCollisionQuery::UpdateElevationForPos(int2 /*sqr*/, float mapHeight) {
	pos.y = std::max(mapHeight, -moveDef->waterline);

	bool inWater = (pos.y < 0.f);
	if (inWater)
		SetPhysicalStateBit(CSolidObject::PhysicalState::PSTATE_BIT_INWATER);
	else
		ClearPhysicalStateBit(CSolidObject::PhysicalState::PSTATE_BIT_INWATER);
}

float CMoveMath::yLevel(const MoveDef& moveDef, int xSqr, int zSqr)
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (moveDef.speedModClass) {
		case MoveDef::Tank: // fall-through
		case MoveDef::KBot:  { return (CGround::GetHeightReal      (xSqr * SQUARE_SIZE, zSqr * SQUARE_SIZE) + 10.0f); } break;
		case MoveDef::Hover: { return (CGround::GetHeightAboveWater(xSqr * SQUARE_SIZE, zSqr * SQUARE_SIZE) + 10.0f); } break;
		case MoveDef::Ship:  { return (CGround::GetWaterLevel(xSqr * SQUARE_SIZE, zSqr * SQUARE_SIZE)); } break;
	}

	return 0.0f;
}

float CMoveMath::yLevel(const MoveDef& moveDef, const float3& pos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (moveDef.speedModClass) {
		case MoveDef::Tank: // fall-through
		case MoveDef::KBot:  { return (CGround::GetHeightReal      (pos.x, pos.z) + 10.0f); } break;
		case MoveDef::Hover: { return (CGround::GetHeightAboveWater(pos.x, pos.z) + 10.0f); } break;
		case MoveDef::Ship:  { return (CGround::GetWaterLevel(pos.x, pos.z)); } break;
	}

	return 0.0f;
}



/* calculate the local speed-modifier for this MoveDef */
float CMoveMath::GetPosSpeedMod(const MoveDef& moveDef, unsigned xSquare, unsigned zSquare)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// PLACEMENT REHOST (stage 2b): LiveView instantiation of the templated leaf.
	return movemath::GetPosSpeedModT(placement::LiveView{}, moveDef, xSquare, zSquare);
}

float CMoveMath::GetPosSpeedMod(const MoveDef& moveDef, unsigned xSquare, unsigned zSquare, float3 moveDir)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return movemath::GetPosSpeedModT(placement::LiveView{}, moveDef, xSquare, zSquare, moveDir);
}

/* Check if a given square-position is accessible by the MoveDef footprint. */
CMoveMath::BlockType CMoveMath::IsBlockedNoSpeedModCheck(const MoveDef& moveDef, int xSquare, int zSquare, const CSolidObject* collider, int thread)
{
	MoveTypes::CheckCollisionQuery collisionQuery = (collider != nullptr)
			? MoveTypes::CheckCollisionQuery(collider)
			: MoveTypes::CheckCollisionQuery(&moveDef);
	collisionQuery.UpdateElevationForPos(int2{xSquare, zSquare});

	return RangeIsBlocked(xSquare - moveDef.xsizeh, xSquare + moveDef.xsizeh, zSquare - moveDef.zsizeh, zSquare + moveDef.zsizeh, &collisionQuery, thread);
}

CMoveMath::BlockType CMoveMath::IsBlockedNoSpeedModCheckDiff(const MoveDef& moveDef, int2 prevSqr, int2 newSqr, const CSolidObject* collider, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// prev allows {-1, -1} so that the first check is always treated as a full test
	const int prev_xmin = std::max(prevSqr.x - moveDef.xsizeh,               -1);
	const int prev_zmin = std::max(prevSqr.y - moveDef.zsizeh,               -1);
	const int prev_xmax = std::min(prevSqr.x + moveDef.xsizeh, mapDims.mapx - 1);
	const int prev_zmax = std::min(prevSqr.y + moveDef.zsizeh, mapDims.mapy - 1);

	const int xmin = std::max(newSqr.x - moveDef.xsizeh,                0);
	const int zmin = std::max(newSqr.y - moveDef.zsizeh,                0);
	const int xmax = std::min(newSqr.x + moveDef.xsizeh, mapDims.mapx - 1);
	const int zmax = std::min(newSqr.y + moveDef.zsizeh, mapDims.mapy - 1);

	BlockType ret = BLOCK_NONE;

	MoveTypes::CheckCollisionQuery colliderInfo = (collider != nullptr)
			? MoveTypes::CheckCollisionQuery(collider)
			: MoveTypes::CheckCollisionQuery(&moveDef);
	colliderInfo.UpdateElevationForPos(int2{newSqr.x, newSqr.y});

	const int tempNum = gs->GetMtTempNum(thread);

	// footprints are point-symmetric around <xSquare, zSquare>
	for (int z = zmin; z <= zmax; z += FOOTPRINT_ZSTEP) {
		const int zOffset = z * mapDims.mapx;

		for (int x = xmin; x <= xmax; x += FOOTPRINT_XSTEP) {

			if (		z <= prev_zmax && z >= prev_zmin
			 		&& 	x <= prev_xmax && x >= prev_xmin)
				continue;

			const CGroundBlockingObjectMap::BlockingMapCell& cell = groundBlockingObjectMap.GetCellUnsafeConst(zOffset + x);

			for (size_t i = 0, n = cell.size(); i < n; i++) {
				CSolidObject* collidee = cell[i];

				if (collidee->mtTempNum[thread] == tempNum)
					continue;

				collidee->mtTempNum[thread] = tempNum;

				if (((ret |= ObjectBlockType(collidee, &colliderInfo)) & BLOCK_STRUCTURE) == 0)
					continue;

				return ret;
			}
		}
	}

	return ret;
}

bool CMoveMath::CrushResistant(const MoveDef& colliderMD, const CSolidObject* collidee)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// PLACEMENT REHOST (stage 2b): LiveView instantiation of the templated leaf.
	return movemath::CrushResistantT(placement::LiveView{}, colliderMD, collidee);
}

bool CMoveMath::IsNonBlocking(const CSolidObject* collidee, const MoveTypes::CheckCollisionQuery* collider)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return movemath::IsNonBlockingT(placement::LiveView{}, collidee, collider);
}

CMoveMath::BlockType CMoveMath::ObjectBlockType(const CSolidObject* collidee, const MoveTypes::CheckCollisionQuery* collider)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return movemath::ObjectBlockTypeT(placement::LiveView{}, collidee, collider);
}

CMoveMath::BlockType CMoveMath::SquareIsBlocked(const MoveDef& moveDef, int xSquare, int zSquare, MoveTypes::CheckCollisionQuery* collider)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (static_cast<unsigned>(xSquare) >= mapDims.mapx || static_cast<unsigned>(zSquare) >= mapDims.mapy)
		return BLOCK_IMPASSABLE;

	collider->UpdateElevationForPos(int2(xSquare, zSquare));
	BlockType r = BLOCK_NONE;

	const CGroundBlockingObjectMap::BlockingMapCell& cell = groundBlockingObjectMap.GetCellUnsafeConst(zSquare * mapDims.mapx + xSquare);

	for (size_t i = 0, n = cell.size(); i < n; i++) {
		r |= ObjectBlockType(cell[i], collider);
	}

	return r;
}

CMoveMath::BlockType CMoveMath::RangeIsBlocked(int xmin, int xmax, int zmin, int zmax, const MoveTypes::CheckCollisionQuery* collider, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	xmin = std::max(xmin,                0);
	zmin = std::max(zmin,                0);
	xmax = std::min(xmax, mapDims.mapx - 1);
	zmax = std::min(zmax, mapDims.mapy - 1);

	const int tempNum = gs->GetMtTempNum(thread);
	return CMoveMath::RangeIsBlockedMt(xmin, xmax, zmin, zmax, collider, tempNum, thread);
}

CMoveMath::BlockType CMoveMath::RangeIsBlockedTempNum(int xmin, int xmax, int zmin, int zmax, const MoveTypes::CheckCollisionQuery* collider, int tempNum, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	xmin = std::max(xmin,                0);
	zmin = std::max(zmin,                0);
	xmax = std::min(xmax, mapDims.mapx - 1);
	zmax = std::min(zmax, mapDims.mapy - 1);

	return CMoveMath::RangeIsBlockedHashedMt(xmin, xmax, zmin, zmax, collider, tempNum, thread);
}


CMoveMath::BlockType CMoveMath::RangeIsBlockedMt(int xmin, int xmax, int zmin, int zmax, const MoveTypes::CheckCollisionQuery* collider, int tempNum, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	BlockType ret = BLOCK_NONE;

	// footprints are point-symmetric around <xSquare, zSquare>
	for (int z = zmin; z <= zmax; z += FOOTPRINT_ZSTEP) {
		const int zOffset = z * mapDims.mapx;

		for (int x = xmin; x <= xmax; x += FOOTPRINT_XSTEP) {
			const CGroundBlockingObjectMap::BlockingMapCell& cell = groundBlockingObjectMap.GetCellUnsafeConst(zOffset + x);

			for (size_t i = 0, n = cell.size(); i < n; i++) {
				CSolidObject* collidee = cell[i];

				if (collidee->mtTempNum[thread] == tempNum)
					continue;

				collidee->mtTempNum[thread] = tempNum;

				if (((ret |= ObjectBlockType(collidee, collider)) & BLOCK_STRUCTURE) == 0)
					continue;

				return ret;
			}
		}
	}

	return ret;
}


static std::array<spring::unordered_map<CSolidObject*, CMoveMath::BlockType>, ThreadPool::MAX_THREADS> blockMaps;
static std::array<int, ThreadPool::MAX_THREADS> lastTempNums;

// Called by GeneralMoveSystem::Init()
void CMoveMath::InitRangeIsBlockedHashes() {
	for (auto& blockMap : blockMaps) {
		blockMap.reserve(10);
	}
	for (auto& lastTempNum : lastTempNums) {
		lastTempNum = -1;
	}
}

CMoveMath::BlockType CMoveMath::RangeIsBlockedHashedMt(int xmin, int xmax, int zmin, int zmax, const MoveTypes::CheckCollisionQuery* collider, int tempNum, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	BlockType ret = BLOCK_NONE;

	spring::unordered_map<CSolidObject*, CMoveMath::BlockType>& blockMap = blockMaps[thread];
	int& lastTempNum = lastTempNums[thread];

	if (lastTempNum != tempNum){
		blockMap.clear();
		lastTempNum = tempNum;
	}

	// footprints are point-symmetric around <xSquare, zSquare>
	for (int z = zmin; z <= zmax; z += FOOTPRINT_ZSTEP) {
		const int zOffset = z * mapDims.mapx;

		for (int x = xmin; x <= xmax; x += FOOTPRINT_XSTEP) {
			const CGroundBlockingObjectMap::BlockingMapCell& cell = groundBlockingObjectMap.GetCellUnsafeConst(zOffset + x);

			for (size_t i = 0, n = cell.size(); i < n; i++) {
				CSolidObject* collidee = cell[i];
  
				auto blockMapResult = blockMap.find(collidee);
				if (blockMapResult == blockMap.end()) {
					blockMapResult = blockMap.emplace(collidee, ObjectBlockType(collidee, collider)).first;
				}

				ret |= blockMapResult->second;

				if ((ret & BLOCK_STRUCTURE) == 0)
					continue;

				return ret;
			}
		}
	}

	return ret;
}

void CMoveMath::FloodFillRangeIsBlocked(const MoveDef& moveDef, const CSolidObject* collider, const SRectangle& areaToSample, std::vector<std::uint8_t>& results, int thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	spring::unordered_map<CSolidObject*, CMoveMath::BlockType>& blockMap = blockMaps[thread];
	blockMap.clear();

	results.clear();
	results.reserve(areaToSample.GetArea());

	MoveTypes::CheckCollisionQuery colliderInfo = (collider != nullptr)
			? MoveTypes::CheckCollisionQuery(collider)
			: MoveTypes::CheckCollisionQuery(&moveDef, {float(areaToSample.x1*SQUARE_SIZE), 0.f, float(areaToSample.z1*SQUARE_SIZE)});

	for (int z = areaToSample.z1; z < areaToSample.z2; ++z) {
		const int zOffset = z * mapDims.mapx;

		for (int x = areaToSample.x1; x < areaToSample.x2; ++x) {
			const CGroundBlockingObjectMap::BlockingMapCell& cell = groundBlockingObjectMap.GetCellUnsafeConst(zOffset + x);
			BlockType ret = BLOCK_NONE;

			for (size_t i = 0, n = cell.size(); i < n; i++) {
				CSolidObject* collidee = cell[i];

				auto blockMapResult = blockMap.find(collidee);
				if (blockMapResult == blockMap.end()) {
					blockMapResult = blockMap.emplace(collidee, ObjectBlockType(collidee, &colliderInfo)).first;
				}

				ret |= blockMapResult->second;

				if ((ret & BLOCK_STRUCTURE) != 0)
					break;
			}
			results.emplace_back(ret);
		}
	}
}

bool CMoveMath::RangeHasExitOnly(int xmin, int xmax, int zmin, int zmax, const ObjectCollisionMapHelper& object) {
	for (int z = zmin; z <= zmax; z += FOOTPRINT_ZSTEP) {
		for (int x = xmin; x <= xmax; x += FOOTPRINT_XSTEP)
			if (object.IsExitOnlyAt(x, z)) return true;
	}
	return false;
}

