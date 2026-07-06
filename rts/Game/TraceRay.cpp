/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "TraceRay.h"
#include "Camera.h"
#include "GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotPickGrid.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/GlobalConfig.h"
#include "System/SpringMath.h"

#include <algorithm>
#include <vector>

#include "System/Misc/TracyDefs.h"

//////////////////////////////////////////////////////////////////////
// Local/Helper functions
//////////////////////////////////////////////////////////////////////

/**
 * helper for TestCone
 * @return true if object <o> is in the firing cone, false otherwise
 */
inline static bool TestConeHelper(
	const float3& tstPos,
	const float3& tstDir,
	const float length,
	const float spread,
	const CSolidObject* obj
) {
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + 1.0f;

	// theoretical impact position assuming no spread
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = tstPos + hitVec;

	bool ret = false;

	if (obj->GetBlockingMapID() < unitHandler.MaxUnits()) {
		// obj is a unit
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	} else {
		// obj is a feature
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	}

	if (globalRendering->drawDebugTraceRay) {
		#define go geometricObjects

		if (ret) {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
		} else {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}

/**
 * helper for TestTrajectoryCone
 * @return true if object <o> is in the firing trajectory, false otherwise
 */
inline static bool TestTrajectoryConeHelper(
	const float3& tstPos,
	const float3& tstDir, // 2D
	float length,
	float linear,
	float quadratic,
	float spread,
	float baseSize,
	const CSolidObject* obj
) {
	// trajectory is a parabola f(x)=a*x*x + b*x with
	// parameters a = quadratic, b = linear, and c = 0
	// (x = objDst1D, negative values represent objects
	// "behind" the testee whose collision volumes might
	// still be intersected by its trajectory arc)
	//
	// firing-cone is centered along tstDir with radius
	// <x * spread + baseSize> (usually baseSize != 0
	// so weapons with spread = 0 will test against a
	// cylinder, not an infinitely thin line as safety
	// measure against friendly-fire damage in tightly
	// packed unit groups)
	// 
	// baseSize is usually hardcoded to 0 in the functions calling this 
	//
	// return true iff the world-space point <x, f(x)>
	// lies on or inside the object's collision volume
	// (where 'x' is actually the projected xz-distance
	// to the object's colvol-center along tstDir)
	//
	// !NOTE!:
	//   THE TRAJECTORY CURVE MIGHT STILL INTERSECT
	//   EVEN WHEN <x, f(x)> DOES NOT LIE INSIDE CV
	//   SO THIS CAN GENERATE FALSE NEGATIVES
	//   Chord checks should solve this problem. 
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + baseSize;

	// theoretical impact position assuming no spread
	// note that unlike TestConeHelper these positions
	// lie along curve f(x) here, not a straight line
	// (if object-distance is 0, tstPos == hitPos)
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = (tstPos + hitVec) + (UpVector * (quadratic * cvRelDst * cvRelDst + linear * cvRelDst));
	float3 endPos = 0.f; 
	bool ret = false;

	CollisionQuery cq;
	// chord check to hitPos
	const CMatrix44f objTransform = obj->GetTransformMatrix();

	// use heuristic to choose which chord to check
	// if projectile is traveling upwards, check from muzzle to position
	// if projectile is traveling downwards, check from position to target
	if ((2 * quadratic * cvRelDst + linear) > 0) {
		if (CCollisionHandler::DetectHit(obj, objTransform, tstPos, hitPos, &cq, true)) {
			ret = true;
		}
	}
	else {
		//only compute endPos if needed
		endPos = (tstPos + tstDir * length) + (UpVector * (quadratic * length * length + linear * length));
		if (CCollisionHandler::DetectHit(obj, objTransform, hitPos, endPos, &cq, true)) {
			ret = true;
		}
	}

	if (globalRendering->drawDebugTraceRay) {

		#define go geometricObjects

		if (ret) {
			// red line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
			// blue lines showing chord checks
			if ((2 * quadratic * cvRelDst + linear) > 0) {
				go->SetColor(go->AddLine(tstPos, hitPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			else {
				go->SetColor(go->AddLine(hitPos, endPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			// While using this debug, on SlowUpdate Frames, CWeapon::TryTargetHeading will assume the unit chassis rotates to face the target, 
			// so the tstPos will not be lined up with any part of the unit. 
			// resulting in two visible debug lines. One from the weaponmuzzle, one from the rotated weaponmuzzle
		} else {
			// green line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}



//////////////////////////////////////////////////////////////////////
// Raytracing
//////////////////////////////////////////////////////////////////////

namespace TraceRay {

// called by {CRifle, CBeamLaser, CLightningCannon}::Fire(), CWeapon::HaveFreeLineOfFire(), and Skirmish AIs
float TraceRay(const float3& p, const float3& d, float l, int f, const CUnit* o, CUnit*& hu, CFeature*& hf, CollisionQuery* cq)
{
	assert(o != nullptr);
	return (TraceRay(p, d, l, f, o->allyteam, o, hu, hf, cq));
}

float TraceRay(
	const float3& pos,
	const float3& dir,
	float traceLength,
	int traceFlags,
	int allyTeam,
	const CUnit* owner,
	CUnit*& hitUnit,
	CFeature*& hitFeature,
	CollisionQuery* hitColQuery
) {
	RECOIL_DETAILED_TRACY_ZONE;
	// NOTE:
	//   the bits here and in Test*Cone are interpreted as "do not scan for {enemy,friendly,...}
	//   objects in quads" rather than "return false if ray hits an {enemy,friendly,...} object"
	//   consequently a weapon with (e.g.) avoidFriendly=true that wants to check whether it has
	//   a free line of fire should *not* set the NOFRIENDLIES bit in its trace-flags, etc
	const bool scanForEnemies  = ((traceFlags & Collision::NOENEMIES   ) == 0);
	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForGround   = ((traceFlags & Collision::NOGROUND    ) == 0);
	const bool scanForCloaked  = ((traceFlags & Collision::NOCLOAKED   ) == 0);

	const bool scanForAnyUnits = scanForEnemies || scanForAllies || scanForNeutrals || scanForCloaked;

	hitFeature = nullptr;
	hitUnit = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	if (scanForFeatures || scanForAnyUnits) {
		CollisionQuery cq;

		QuadFieldQuery qfQuery;
		quadField.GetQuadsOnRay(qfQuery, pos, dir, traceLength);

		// locally point somewhere non-NULL; we cannot pass hitColQuery
		// to DetectHit directly because each call resets it internally
		if (hitColQuery == nullptr)
			hitColQuery = &cq;

		// feature intersection
		if (scanForFeatures) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CFeature* f: quad.features) {
					// NOTE:
					//   if f is non-blocking, ProjectileHandler will not test
					//   for collisions with projectiles so we can skip it here
					if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					if (CCollisionHandler::DetectHit(f, f->GetTransformMatrix(), pos, pos + dir * traceLength, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);

						// we want the closest feature (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitFeature = f;
						*hitColQuery = cq;
					}
				}
			}
		}

		// unit intersection
		if (scanForAnyUnits) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CUnit* u: quad.units) {
					if (u == owner)
						continue;

					if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					bool doHitTest = false;

					doHitTest |= (scanForAllies   && u->allyteam == owner->allyteam);
					doHitTest |= (scanForEnemies  && u->allyteam != owner->allyteam);
					doHitTest |= (scanForNeutrals && u->IsNeutral());
					doHitTest |= (scanForCloaked  && u->IsCloaked());

					if (!doHitTest)
						continue;

					if (CCollisionHandler::DetectHit(u, u->GetTransformMatrix(), pos, pos + dir * traceLength, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);

						// we want the closest unit (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitUnit = u;
						*hitColQuery = cq;
					}
				}
			}

			// units override features, so feature != null implies no unit was hit
			if (hitUnit != nullptr)
				hitFeature = nullptr;

		}
	}

	if (scanForGround) {
		// ground intersection
		const float groundLength = CGround::LineGroundCol(pos, pos + dir * traceLength);

		if (traceLength > groundLength && groundLength > 0.0f) {
			traceLength = groundLength;

			hitUnit = nullptr;
			hitFeature = nullptr;
		}
	}

	// no intersection if no decrease in length
	return traceLength;
}


void TraceRayShields(
	const CWeapon* emitter,
	const float3& start,
	const float3& dir,
	float length,
	std::vector<SShieldDist>& hitShields
) {
	RECOIL_DETAILED_TRACY_ZONE;
	CollisionQuery cq;

	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, start, dir, length);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		for (CPlasmaRepulser* r: quad.repulsers) {
			if (!r->CanIntercept(emitter->weaponDef->interceptedByShieldType, emitter->owner->allyteam))
				continue;

			if (CCollisionHandler::DetectHit(r->owner, &r->collisionVolume, r->owner->GetTransformMatrix(), start, start + dir * length, &cq, true)) {
				if (cq.InsideHit() && r->weaponDef->exteriorShield)
					continue;

				const float len = cq.GetHitPosDist(start, dir);

				if (len <= 0.0f)
					continue;

				const auto hitCmp = [](const float a, const SShieldDist& b) { return (a < b.dist); };
				const auto insPos = std::upper_bound(hitShields.begin(), hitShields.end(), len, hitCmp);

				hitShields.insert(insPos, {r, len});
			}
		}
	}
}


float GuiTraceRay(
	const float3& start,
	const float3& dir,
	const float length,
	const CUnit* exclude,
	const CUnit*& hitUnit,
	const CFeature*& hitFeature,
	bool useRadar,
	bool groundOnly,
	bool ignoreWater
) {
	RECOIL_DETAILED_TRACY_ZONE;
	hitUnit = nullptr;
	hitFeature = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	// ground and water-plane intersection
	const float    guiRayLength = length;
	const float groundRayLength = CGround::LineGroundCol(start, dir, guiRayLength, false);
	const float  waterRayLength = CGround::LinePlaneCol(start, dir, guiRayLength, CGround::GetWaterPlaneLevel());

	float minRayLength = groundRayLength;
	float minIngressDist = length;
	float minEgressDist = length;

	bool hitFactory = false;

	// if ray cares about water, take minimum
	// of distance to ground and water surface
	if (!ignoreWater)
		minRayLength = std::min(groundRayLength, waterRayLength);
	if (groundOnly)
		return minRayLength;

	// set maximum ray until ground intersection taking lenience into account later
	float maxRayLength;
	if (minRayLength >= 0.0) {
		// normal intersection
		maxRayLength = minRayLength;
	} else if (waterRayLength >= 0.0) {
		// out of map we still want to intersect somewhere if possible
		maxRayLength = waterRayLength;
	} else {
		// pointing upwards
		maxRayLength = length;
	}
	maxRayLength = std::min(maxRayLength + globalConfig.selectThroughGround, length);

	// PR 25: draw-side picking against the SimSnapshot + the draw-side spatial
	// grid (section D). The grid coarse phase replaces the sim quadfield walk;
	// the precise CCollisionHandler::MouseHit test (against the snapshot
	// selection volume + the drawer's unsynced transform) still decides the
	// winner, so the pick is identical to the live path apart from how
	// candidates are discovered. Reads the last published boundary -- for
	// synchronous input-side callers that is <=1 draw frame stale, the decided
	// pick-latency contract (SimSnapshot.h / research doc section D).
	const SimSnapshot::UnitRows& urows = simSnapshot.Read();
	const SimSnapshot::FeatureRows& frows = simSnapshot.ReadFeatures();

	const int myAllyTeam = gu->myAllyTeam;
	const bool fullView = gu->spectatingFullView;
	const int excludeID = (exclude != nullptr) ? exclude->id : -1;
	const float3 segEnd = start + dir * guiRayLength;

	const float wideErr = (useRadar && myAllyTeam >= 0 && myAllyTeam < urows.numAllyTeams)
		? urows.radarErrorSizes[myAllyTeam] : 0.0f;

	// candidate id lists reused across calls (main-thread only)
	static std::vector<int> candUnits;
	static std::vector<int> candFeatures;
	snapshotPickGrid.QueryRay(start, dir, maxRayLength, wideErr, candUnits, candFeatures);

	CollisionQuery cq;
	int hitUnitID = -1;
	int hitFeatureID = -1;

	// Unit intersection (ascending snapshot id -- deterministic; only exact-tie
	// / factory-overlap ordering differs from master's sim-quad walk order)
	for (const int id: candUnits) {
		const bool unitIsEnemy = !urows.Allied(urows.AllyTeam(id), myAllyTeam);
		const bool unitOnRadar = (useRadar && urows.InRadar(id, myAllyTeam));
		const bool unitInSight = (urows.LosStatus(id, myAllyTeam) & (LOS_INLOS | LOS_CONTRADAR)) != 0;
		const bool unitVisible = !unitIsEnemy || unitOnRadar || unitInSight || fullView;

		if (id == excludeID)
			continue;
		if (urows.NoSelect(id))
			continue;
		if (!unitVisible)
			continue;

		CollisionVolume cv = urows.SelVol(id);

		// for iconified units (and enemy radar blips) pretend the volume is a
		// sphere of the icon radius; GetIsIcon / GetUnitIconRadius stay drawer-
		// owned, addressed by id
		if (CUnitDrawer::GetIsIcon(id) || (!unitInSight && unitOnRadar && unitIsEnemy))
			cv.InitSphere(CUnitDrawer::GetUnitIconRadius(id));

		bool hit;
		if (cv.DefaultToPieceTree()) {
			// piece-tree selection volumes are deferred draw-side (PR 25; unused
			// in BAR) -- fall back to a live sim read for these objects. This is
			// the ONLY live-sim read remaining on the unit pick path; TODO remove
			// at split-enable (PR 27). See SimSnapshot.h PR-25 note.
			const CUnit* lu = unitHandler.GetUnit(id);
			hit = (lu != nullptr) && CCollisionHandler::MouseHit(lu, CUnitDrawer::GetUnsyncedTransformMatrix(lu), start, segEnd, &cv, &cq);
		} else {
			// reconstruct the unsynced (drawPos-based) transform from the
			// snapshot basis + the drawer drawPos + the snapshot error vector --
			// exactly CUnitDrawerData::GetUnsyncedTransformMatrix, id-keyed
			float3 interPos = CUnitDrawer::GetDrawPos(id);
			if (!fullView)
				interPos += urows.ErrorVector(id, myAllyTeam);
			const CMatrix44f m(interPos, -urows.Rightdir(id), urows.Updir(id), urows.Frontdir(id));

			hit = CCollisionHandler::MouseHit(urows.relMidPos[id], urows.InVoid(id), m, start, segEnd, &cv, &cq);
		}

		if (hit) {
			// get the distance to the ray-volume ingress point
			const float ingressDist = cq.GetIngressPosDist(start, dir);
			const float  egressDist = cq.GetEgressPosDist(start, dir);

			const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.DefID(id));
			const bool factoryUnderCursor = ud->IsFactoryUnit();
			const bool factoryHitBeforeUnit = ((hitFactory && ingressDist < minIngressDist) || (!hitFactory &&  egressDist < minIngressDist));
			const bool unitHitInsideFactory = ((hitFactory && ingressDist <  minEgressDist) || (!hitFactory && ingressDist < minIngressDist));

			// give units in a factory higher priority than the factory itself
			if (hitUnitID < 0 || (factoryUnderCursor && factoryHitBeforeUnit) || (!factoryUnderCursor && unitHitInsideFactory)) {
				hitFactory = factoryUnderCursor;
				minIngressDist = ingressDist;
				minEgressDist = egressDist;

				hitUnitID = id;
				hitFeatureID = -1;
			}
		}
	}

	// Feature intersection
	for (const int id: candFeatures) {
		if (!fullView && !frows.IsInLosForAllyTeam(id, myAllyTeam))
			continue;
		if (frows.NoSelect(id))
			continue;

		const CollisionVolume& cv = frows.SelVol(id);

		bool hit;
		if (cv.DefaultToPieceTree()) {
			const CFeature* lf = featureHandler.GetFeature(id);
			hit = (lf != nullptr) && CCollisionHandler::MouseHit(lf, CFeatureDrawer::GetUnsyncedTransformMatrix(lf), start, segEnd, &cv, &cq);
		} else {
			hit = CCollisionHandler::MouseHit(frows.relMidPos[id], frows.InVoid(id), CFeatureDrawer::GetUnsyncedTransformMatrix(id), start, segEnd, &cv, &cq);
		}

		if (hit) {
			const float hitDist = cq.GetHitPosDist(start, dir);

			const bool factoryHitBeforeUnit = ( hitFactory && hitDist <  minEgressDist);
			const bool unitHitInsideFactory = (!hitFactory && hitDist < minIngressDist);

			// we want the closest feature (intersection point) on the ray;
			// a held unit (hitUnitID >= 0) is never displaced by a feature
			if (hitUnitID < 0 || factoryHitBeforeUnit || unitHitInsideFactory) {
				hitFactory = false;
				minIngressDist = hitDist;

				hitFeatureID = id;
				hitUnitID = -1;
			}
		}
	}

	if ((minRayLength > 0.0f) && (maxRayLength < minIngressDist)) {
		minIngressDist = minRayLength;
		hitUnitID = -1;
		hitFeatureID = -1;
	}

	// resolve the winning snapshot id to a live pointer for the caller: the pick
	// DECISION above is fully snapshot-driven, and this id -> pointer lookup is
	// the seam left for the split (callers still dereference the object, which
	// their own section-C conversions handle). TODO at split-enable (PR 27):
	// return ids and drop this lookup.
	hitUnit = (hitUnitID >= 0) ? unitHandler.GetUnit(hitUnitID) : nullptr;
	hitFeature = (hitFeatureID >= 0) ? featureHandler.GetFeature(hitFeatureID) : nullptr;

	return minIngressDist;
}


bool TestCone(
	const float3& from,
	const float3& dir,
	float length,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, f))
					return true;
			}
		}
	}

	return false;
}



bool TestTrajectoryCone(
	const float3& from,
	const float3& dir,
	float length,
	float linear,
	float quadratic,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		// friendly units in this quad
		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;

			}
		}

		// neutral units in this quad
		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;
			}
		}

		// features in this quad
		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, f))
					return true;
			}
		}
	}

	return false;
}



} //namespace TraceRay
