/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * WeaponPredicates -- the four Lua-reachable weapon trace predicates
 * (TryTarget/TestTarget/TestRange/HaveFreeLineOfFire) + the GetLeadTargetPos
 * chain, as templated pure functions over an abstract state View (sim/draw split
 * trace re-host, stage 3). One implementation, two backends: LiveView (globals +
 * live CWeapon and CUnit pointers) and, in stage 4, trace::EpochView (mirrors +
 * snapshot rows). Mirrors the placement rehost's PlacementPredicates.h.
 *
 * The CWeapon virtual predicates (TestTarget/TestRange/HaveFreeLineOfFire +
 * GetRange2D/GetPredictedImpactTime/GetAimFromPos) are dispatched draw-side on a
 * captured trace::WeaponClass discriminator (the epoch has no vtable). Each
 * override body becomes a templated free function (...CannonT / ...BombDropperT / ...);
 * the base body is ...BaseT; the dispatchers switch on view.GetWeaponClass().
 *
 * The existing CWeapon / subclass member functions become thin wrappers that call
 * these with a LiveView, so every caller keeps its exact signature and the
 * LiveView instantiation inlines to today's code (byte-identical -- these run in
 * synced aim/auto-target loops every frame). See doc/sim-draw-trace-rehost-*.md.
 *
 * Target handling: the templated predicates take the target as `const V::Target&`.
 * Its POD (type/isManualFire/isUserTarget/isAutoTarget/groundPos) is read directly;
 * every object dereference (trg.unit->..., trg.intercept->...) goes through a view
 * accessor, so the templated bodies never include Unit.h/WeaponProjectile.h --
 * those live only where LiveView's accessors are defined (bottom of this header).
 * The Lua trace callouts only ever build Target_None/Target_Unit/Target_Pos
 * targets; the Target_Intercept accessors exist for the LiveView aim-loop callers
 * and are never reached by the epoch backend.
 */

#include <algorithm>
#include <array>
#include <cmath>

#include "WeaponTarget.h"
#include "WeaponTraceClass.h"
#include "WeaponDef.h"
#include "Game/TraceRay.h"                    // Collision:: avoid-flag bits
#include "Map/Ground.h"
#include "Map/MapInfo.h"                      // mapInfo->map.gravity
#include "Sim/Misc/GlobalConstants.h"         // GAME_SPEED
#include "Sim/Misc/ModInfo.h"                 // modInfo.fireAt*/targetableTransportedUnits
#include "Sim/Units/CommandAI/Command.h"      // FIRESTATE_FIREATNEUTRAL
#include "System/float3.h"
#include "System/SpringMath.h"

namespace trace {

// ---------------------------------------------------------------------------
// Shared leaf helpers (View-agnostic)
// ---------------------------------------------------------------------------

// CWeapon::TargetUnderWater (static) -- object derefs routed through the view
template<class V>
bool TargetUnderWaterT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	switch (trg.type) {
		case Target_None:      return false;
		case Target_Unit:      return view.TgtUnitIsUnderWater(trg);
		case Target_Pos:       return (tgtPos.y < 0.0f);
		case Target_Intercept: return (view.TgtInterceptPos(trg).y < 0.0f);
		default:               return false;
	}
}

// CWeapon::TargetInWater (static)
template<class V>
bool TargetInWaterT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	switch (trg.type) {
		case Target_None:      return false;
		case Target_Unit:      return view.TgtUnitIsInWater(trg);
		case Target_Pos:       return (tgtPos.y <= 0.0f);
		case Target_Intercept: return (view.TgtInterceptPos(trg).y <= 0.0f);
		default:               return false;
	}
}

// CWeapon::CheckTargetAngleConstraint
template<class V>
bool CheckTargetAngleConstraintT(const V& view, const float3& worldTargetDir, const float3& worldWeaponDir)
{
	if (worldTargetDir.same(ZeroVector))
		return true;

	if (view.OnlyForward()) {
		if (view.MaxForwardAngleDif() > -1.0f) {
			if (view.OwnerFrontDir().dot(worldTargetDir) < view.MaxForwardAngleDif())
				return false;
		}
	} else {
		if (view.MaxMainDirAngleDif() > -1.0f) {
			if (worldWeaponDir.dot(worldTargetDir) < view.MaxMainDirAngleDif())
				return false;
		}
	}

	return true;
}

// CWeapon::GetRange2D (base)
template<class V>
float GetRange2DBaseT(const V& view, float boost, float ydiff)
{
	const float rangeSq = Square(view.Range() + boost);
	const float ydiffSq = Square(ydiff);
	const float    root = rangeSq - ydiffSq;
	return (math::sqrt(std::max(root, 0.0f)));
}

// CStarburstLauncher::GetRange2D
template<class V>
float GetRange2DStarburstT(const V& view, float boost, float ydiff)
{
	return boost + view.Range() + (ydiff * view.Def()->heightmod);
}

// CCannon::GetStaticRange2D (static math; identical body)
inline float GetStaticRange2D(const float2& baseConsts, const float2& projConsts, const float2& boostFacts)
{
	const auto CalcRange2D = [](const float3& bc, const float2& pc, const float2& bf) {
		float heightDiff = bc.x;
		const float speedFactor = bc.y;
		const float smoothHeight = bc.z;
		const float   speed2D = pc.x * speedFactor;
		const float sqSpeed2D = speed2D * speed2D;

		if (heightDiff < -smoothHeight) {
			heightDiff *= bf.y;
		} else if (heightDiff < 0.0f) {
			heightDiff *= (1.0f + (bf.y - 1.0f) * -heightDiff / smoothHeight);
		}

		const float root = sqSpeed2D + 2.0f * pc.y * heightDiff;
		if (root < 0.0f)
			return 0.0f;

		return (bf.x * (sqSpeed2D + speed2D * math::sqrt(root)) / -pc.y);
	};

	if (boostFacts.x > 0.0f)
		return (CalcRange2D({baseConsts.y, 0.7071067f, 100.0f}, projConsts, boostFacts));

	const float wdRangeExclBoost = CalcRange2D({0.0f, 0.7071067f, 100.0f}, projConsts, {1.0f, boostFacts.y});
	const float wdRangeBoostFact = std::clamp(baseConsts.x / wdRangeExclBoost, 0.0f, 1.0f);

	float wdHeightBoostFact = boostFacts.y;
	if (wdHeightBoostFact < 0.0f && wdRangeBoostFact > 0.0f)
		wdHeightBoostFact = (2.0f - wdRangeBoostFact) / math::sqrt(wdRangeBoostFact);

	return (CalcRange2D({baseConsts.y, 0.7071067f, 100.0f}, projConsts, {wdRangeBoostFact, wdHeightBoostFact}));
}

// CCannon::GetRange2D(boost, ydiff): GetStaticRange2D({range,ydiff},{speed,gravity},{rangeBoost,heightBoost})
template<class V>
float GetRange2DCannonT(const V& view, float boost, float ydiff)
{
	return GetStaticRange2D({view.Range(), ydiff}, {view.ProjectileSpeed(), view.CannonGravity()},
	                        {view.CannonRangeBoost(), view.HeightBoostFactor()}) + boost;
}

// GetRange2D dispatch (virtual: Cannon / Starburst override)
template<class V>
float GetRange2DT(const V& view, float boost, float ydiff)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::Cannon:            return GetRange2DCannonT(view, boost, ydiff);
		case WeaponClass::StarburstLauncher: return GetRange2DStarburstT(view, boost, ydiff);
		default:                             return GetRange2DBaseT(view, boost, ydiff);
	}
}

// CWeapon::GetShapedWeaponRange
template<class V>
float GetShapedWeaponRangeT(const V& view, const float3& dir, float maxLength)
{
	maxLength = std::max(maxLength, 1e-6f);
	const WeaponDef* wd = view.Def();
	if (wd->cylinderTargeting > 0.01f) {
		const float invSinA = math::isqrt(1.0f - dir.y * dir.y);
		maxLength = std::min(math::fabs(maxLength * invSinA), math::fabs(maxLength * wd->cylinderTargeting / dir.y));
	} else if (wd->heightmod != 1.0f) {
		const float maxVertLen = maxLength / std::max(wd->heightmod, 1e-6f);
		maxLength = math::isqrt(Square(dir.x / maxLength) + Square(dir.z / maxLength) + Square(dir.y / maxVertLen));
	}
	return maxLength;
}

// ---------------------------------------------------------------------------
// Predicted-impact-time chain (GetLeadVec) -- virtual GetPredictedImpactTime
// ---------------------------------------------------------------------------

// CWeapon::GetPredictedImpactTime dispatch (virtual: BeamLaser / BombDropper /
// Rifle / LightningCannon override). p is the impact position.
template<class V>
float GetPredictedImpactTimeT(const V& view, const float3& p)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::BeamLaser:
			return (view.SalvoSize() * 0.5f * (1 - view.Def()->beamburst));
		case WeaponClass::BombDropper: {
			if (view.WeaponMuzzlePos().y <= p.y)
				return 0.0f;
			const float d = p.y - view.WeaponMuzzlePos().y;
			const float v = view.OwnerSpeed().y;
			const float g = (view.Def()->myGravity == 0) ? mapInfo->map.gravity : -view.Def()->myGravity;
			const float tt = v * v + 2.f * d * g;
			return ((tt >= 0.0f) ? ((-v - math::sqrt(tt)) / g) : 0.0f);
		}
		case WeaponClass::Rifle:
		case WeaponClass::LightningCannon:
			return 0.0f;
		default:
			return view.AimFromPos().distance(p) / view.ProjectileSpeed();
	}
}

// CWeapon::GetSafeInterceptTime (reached from GetAccuratePredictedImpactTime)
template<class V>
float GetSafeInterceptTimeT(const V& view, typename V::UnitRef tgt, float predictMult)
{
	const float3 unitSpeed = view.UnitSpeed(tgt) * predictMult;
	const float3 dist = view.UnitPos(tgt) - view.WeaponMuzzlePos();
	const float aa = unitSpeed.dot(unitSpeed) - (view.Def()->projectilespeed) * (view.Def()->projectilespeed);
	const float bb = 2 * (dist.dot(unitSpeed));
	const float cc = dist.dot(dist);
	const float temp1 = 4 * aa * cc;
	const float temp2 = (bb * bb);
	float predictTime = 0.0;
	if (aa < -1) {
		predictTime = (-bb - math::sqrt(temp2 - temp1)) / (2 * aa);
	} else if (aa <= 0) {
		if ((std::abs(aa) < (float3::cmp_eps())) && (bb > (-float3::cmp_eps()))) {
			return -1.0;
		}
		predictTime = (2 * cc) / (-bb + math::sqrt(temp2 - temp1));
	} else if (aa > 0) {
		if (temp1 >= temp2) {
			return -1.0;
		}
		if (bb >= 0) {
			return -1.0;
		}
		if (aa > 1) {
			predictTime = (-bb - math::sqrt(temp2 - temp1)) / (2 * aa);
		} else {
			if ((std::abs(aa) < (float3::cmp_eps())) && (bb > (-float3::cmp_eps()))) {
				return -1.0;
			}
			predictTime = (2 * cc) / (-bb + math::sqrt(temp2 - temp1));
		}
	}
	return predictTime;
}

// CWeapon::GetAccuratePredictedImpactTime
template<class V>
float GetAccuratePredictedImpactTimeT(const V& view, typename V::UnitRef tgt)
{
	const WeaponDef* wd = view.Def();
	float predictTime = GetPredictedImpactTimeT(view, view.UnitPos(tgt));
	const float predictMult = mix(view.PredictSpeedMod(), 1.0f, wd->predictBoost);
	const float gravity = mix(mapInfo->map.gravity, -wd->myGravity, wd->myGravity != 0.0f);
	if (gravity < 0) {
		float highTrajectorySwitch = -1.0f;
		if (wd->highTrajectory == 1)
			highTrajectorySwitch = 1.0f;
		if (view.OwnerUseHighTrajectory())
			highTrajectorySwitch = 1.0f;
		float3 dist = view.UnitPos(tgt) + view.UnitSpeed(tgt) * predictMult * predictTime - view.WeaponMuzzlePos();
		const float gg = (gravity) * (gravity);
		const float ps2 = (wd->projectilespeed) * (wd->projectilespeed);
		float t1 = 1.0f;
		float dt1 = 1.0f;
		float temp1 = 1.0f;
		float temp2 = 1.0f;
		float cc = 1.0f;
		float deltatime = predictTime;
		for (int ii = 0; ii < int(view.AccurateLeading()); ii++) {
			if (deltatime < 1)
				break;
			cc = -ps2 - dist.y * (gravity);
			temp1 = (dist.dot(dist) * gg);
			temp2 = (cc * cc);
			if (temp1 >= temp2)
				break;
			t1 = math::sqrt((-cc + highTrajectorySwitch * math::sqrt(temp2 - temp1)) / (0.5f * gg));
			dt1 = (t1 - predictTime) / deltatime;
			if (std::abs(dt1 + float3::cmp_eps()) < 1)
				t1 = predictTime - (t1 - predictTime) / (dt1 - 1);
			if (std::abs(t1 - predictTime) < 1) {
				predictTime = t1;
				break;
			}
			deltatime = t1 - predictTime;
			predictTime = t1;
			dist = view.UnitPos(tgt) + view.UnitSpeed(tgt) * predictMult * predictTime - view.WeaponMuzzlePos();
		}
	} else {
		const float interceptTime = GetSafeInterceptTimeT(view, tgt, predictMult);
		if (interceptTime > 0)
			predictTime = interceptTime;
	}
	return predictTime;
}

// CWeapon::GetLeadVec
template<class V>
float3 GetLeadVecT(const V& view, typename V::UnitRef tgt)
{
	const WeaponDef* wd = view.Def();
	const float predictMult = mix(view.PredictSpeedMod(), 1.0f, wd->predictBoost);
	const float predictTime = (view.AccurateLeading() > 0)
		? GetAccuratePredictedImpactTimeT(view, tgt)
		: GetPredictedImpactTimeT(view, view.UnitPos(tgt));
	float3 lead = view.UnitSpeed(tgt) * predictTime * predictMult;

	if (wd->leadLimit < 0.0f)
		return lead;

	const float leadLenSq = lead.SqLength();
	const float leadBonus = wd->leadLimit + wd->leadBonus * view.OwnerExperience();

	if (leadLenSq > Square(leadBonus))
		lead *= (leadBonus / (math::sqrt(leadLenSq) + 0.01f));

	return lead;
}

// CWeapon::GetUnitPositionWithError
template<class V>
float3 GetUnitPositionWithErrorT(const V& view, typename V::UnitRef tgt)
{
	float3 errorPos = view.UnitErrorPos(tgt);
	if (view.DoTargetGroundPos())
		errorPos -= view.UnitAimPos(tgt) - view.UnitPos(tgt);
	const float errorScale = (view.MoveErrorExperience() * GAME_SPEED * view.UnitSpeedW(tgt));
	return errorPos + view.ErrorVector() * errorScale;
}

// CWeapon::GetUnitLeadTargetPos
template<class V>
float3 GetUnitLeadTargetPosT(const V& view, typename V::UnitRef tgt)
{
	const float3 tmpTargetPos = GetUnitPositionWithErrorT(view, tgt) + GetLeadVecT(view, tgt);
	const float3 tmpTargetDir = (tmpTargetPos - view.AimFromPos()).SafeNormalize();

	float3 aimPos = view.TargetBorderPos(tgt, tmpTargetPos, tmpTargetDir);

	aimPos.y = std::max(aimPos.y, view.GroundApproxHeight(aimPos.x, aimPos.z) + 2.0f);
	aimPos.y = std::max(aimPos.y, aimPos.y * view.Def()->waterweapon);

	return aimPos;
}

// CWeapon::AdjustTargetPosToWater (attackGround == true at the only call site)
template<class V>
void AdjustTargetPosToWaterT(const V& view, float3& tgtPos)
{
	tgtPos.y = std::max(tgtPos.y, view.GroundHeightReal(tgtPos.x, tgtPos.z));
	tgtPos.y = std::max(tgtPos.y, tgtPos.y * view.Def()->waterweapon);

	if (view.OwnerUnderFirstPersonControl() && view.GetWeaponClass() == WeaponClass::Cannon)
		tgtPos.y = view.GroundHeightAboveWater(tgtPos.x, tgtPos.z);
}

// CWeapon::GetLeadTargetPos
template<class V>
float3 GetLeadTargetPosT(const V& view, const typename V::Target& trg)
{
	switch (trg.type) {
		case Target_None:      return view.CurrentTargetPos();
		case Target_Unit:      return GetUnitLeadTargetPosT(view, view.TargetUnit(trg));
		case Target_Pos: {
			float3 p = trg.groundPos;
			AdjustTargetPosToWaterT(view, p);
			return p;
		}
		case Target_Intercept: return view.TgtInterceptPos(trg) + view.TgtInterceptSpeed(trg);
	}
	return view.CurrentTargetPos();
}

// ---------------------------------------------------------------------------
// GetAimFromPos dispatch. Cannon / MissileLauncher / StarburstLauncher all
// override it to always return weaponMuzzlePos (ignoring useMuzzle).
// ---------------------------------------------------------------------------
template<class V>
float3 GetAimFromPosT(const V& view, bool useMuzzle)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::Cannon:
		case WeaponClass::MissileLauncher:
		case WeaponClass::StarburstLauncher:
			return view.WeaponMuzzlePos();
		default:
			return (useMuzzle ? view.WeaponMuzzlePos() : view.AimFromPos());
	}
}

// ---------------------------------------------------------------------------
// TestTarget (virtual: BombDropper / TorpedoLauncher / NoWeapon override)
// ---------------------------------------------------------------------------
template<class V>
bool TestTargetBaseT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	const WeaponDef* wd = view.Def();
	if ((trg.isManualFire != wd->manualfire) && view.OwnerCanManualFire())
		return false;

	switch (trg.type) {
		case Target_None: {
			return true;
		} break;
		case Target_Unit: {
			if (view.TgtUnitIsOwner(trg) || view.TgtUnitIsNull(trg))
				return false;
			if ((view.TgtUnitCategory(trg) & view.OnlyTargetCategory()) == 0)
				return false;
			if (view.TgtUnitIsDead(trg) && !modInfo.fireAtKilled)
				return false;
			if (view.TgtUnitIsCrashing(trg) && !modInfo.fireAtCrashing)
				return false;
			if (!view.TgtUnitInLosOrRadar(trg))
				return false;
			if (!trg.isUserTarget && view.TgtUnitIsNeutral(trg) && view.OwnerFireState() < FIRESTATE_FIREATNEUTRAL)
				return false;
			if (!trg.isUserTarget && view.Ally(view.OwnerAllyTeam(), view.TgtUnitAllyTeam(trg)))
				return false;

			if (view.TgtUnitHasTransporter(trg)) {
				if (!modInfo.targetableTransportedUnits)
					return false;
				const float3 tup = view.TgtUnitPos(trg);
				if (tup.y < view.GroundHeightReal(tup.x, tup.z))
					return false;
			}
		} break;
		case Target_Pos: {
			if (!wd->canAttackGround)
				return false;
		} break;
		case Target_Intercept: {
			if (wd->interceptSolo && view.TgtInterceptIsBeingIntercepted(trg))
				return false;
			if (!wd->interceptor)
				return false;
			if (!view.TgtInterceptCanBeInterceptedByDef(trg))
				return false;
		} break;
		default: break;
	}

	if (trg.type != Target_Intercept && wd->interceptor)
		return false;

	if (!wd->waterweapon) {
		if (!view.OwnerIsUnderWater() && TargetUnderWaterT(view, tgtPos, trg))
			return false;
		if (view.OwnerIsUnderWater() && TargetInWaterT(view, tgtPos, trg))
			return false;
	}

	return true;
}

// CBombDropper::TestTarget
template<class V>
bool TestTargetBombDropperT(const V& view, const float3& pos, const typename V::Target& trg)
{
	if (!view.BombDropTorpedoes() && TargetUnderWaterT(view, pos, trg))
		return false;
	if (view.BombDropTorpedoes() && !TargetInWaterT(view, pos, trg))
		return false;
	return TestTargetBaseT(view, pos, trg);
}

// CTorpedoLauncher::TestTarget
template<class V>
bool TestTargetTorpedoT(const V& view, const float3& pos, const typename V::Target& trg)
{
	if (view.WeaponMuzzlePos().y > 0.0f && !TargetInWaterT(view, pos, trg))
		return false;
	if (view.WeaponMuzzlePos().y <= 0.0f && !view.Def()->submissile && !TargetInWaterT(view, pos, trg))
		return false;
	return TestTargetBaseT(view, pos, trg);
}

template<class V>
bool TestTargetT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::BombDropper:    return TestTargetBombDropperT(view, tgtPos, trg);
		case WeaponClass::TorpedoLauncher:return TestTargetTorpedoT(view, tgtPos, trg);
		case WeaponClass::NoWeapon:       return false;
		default:                          return TestTargetBaseT(view, tgtPos, trg);
	}
}

// ---------------------------------------------------------------------------
// TestRange (virtual: BeamLaser / LightningCannon / BombDropper override)
// ---------------------------------------------------------------------------
template<class V>
bool TestRangeBaseT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	const WeaponDef* wd = view.Def();
	const float3 aimFromPos = view.AimFromPos();
	const float heightDiff = tgtPos.y - aimFromPos.y;
	const float targetDist = aimFromPos.SqDistance2D(tgtPos);

	float weaponRange = 0.0f;

	if (trg.type == Target_Pos || wd->cylinderTargeting < 0.01f) {
		weaponRange = GetRange2DT(view, 0.0f, heightDiff * wd->heightmod);
	} else {
		if ((wd->cylinderTargeting * view.Range()) > (math::fabsf(heightDiff) * wd->heightmod))
			weaponRange = GetRange2DT(view, 0.0f, 0.0f);
	}

	if (targetDist > (weaponRange * weaponRange))
		return false;

	return (CheckTargetAngleConstraintT(view, (tgtPos - aimFromPos).SafeNormalize(), view.OwnerObjectSpaceVec(view.MainDir())));
}

// CBeamLaser::TestRange / CLightningCannon::TestRange (identical bodies)
template<class V>
bool TestRangeShapedT(const V& view, const float3& tgtPos, const typename V::Target& /*trg*/)
{
	float3 aimDir = (tgtPos - view.AimFromPos());
	const float targetDist = aimDir.LengthNormalize();

	if (const auto shapedRange = GetShapedWeaponRangeT(view, aimDir, view.Range()); targetDist > shapedRange)
		return false;

	return (CheckTargetAngleConstraintT(view, aimDir, view.OwnerObjectSpaceVec(view.MainDir())));
}

// CBombDropper::TestRange
template<class V>
bool TestRangeBombDropperT(const V& view, const float3& tgtPos, const typename V::Target& /*trg*/)
{
	const float3 aimFromPos = view.AimFromPos();
	if (aimFromPos.y < tgtPos.y)
		return false;

	const float fallTime = GetPredictedImpactTimeT(view, tgtPos);
	const float dropDist = std::max(1, view.SalvoSize() - 1) * view.SalvoDelay() * view.OwnerSpeed().Length2D() * 0.5f;
	const float torpDist = view.BombTorpMoveRange() * (view.OwnerFrontDir().dot(tgtPos - aimFromPos) > 0.0f);

	return (tgtPos.SqDistance2D(aimFromPos + view.OwnerSpeed() * fallTime) < Square(dropDist + torpDist));
}

template<class V>
bool TestRangeT(const V& view, const float3& tgtPos, const typename V::Target& trg)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::BeamLaser:
		case WeaponClass::LightningCannon: return TestRangeShapedT(view, tgtPos, trg);
		case WeaponClass::BombDropper:     return TestRangeBombDropperT(view, tgtPos, trg);
		default:                           return TestRangeBaseT(view, tgtPos, trg);
	}
}

// ---------------------------------------------------------------------------
// HaveFreeLineOfFire (virtual: Cannon / MissileLauncher / StarburstLauncher /
// BombDropper(true) / Melee(true) / PlasmaRepulser(true) override)
// ---------------------------------------------------------------------------
template<class V>
bool HaveFreeLineOfFireBaseT(const V& view, const float3& srcPos, const float3& tgtPos, const typename V::Target& /*trg*/)
{
	float3 tgtDir = tgtPos - srcPos;
	const float length = tgtDir.LengthNormalize();
	const float spread = view.AccuracyExperience() + view.SprayAngleExperience();

	if (length == 0.0f)
		return true;

	const uint32_t avoidFlags = view.AvoidFlags();

	if ((avoidFlags & Collision::NOGROUND) == 0) {
		const float gndDst = view.TraceRayGroundDist(srcPos, tgtDir, length);
		const float tgtDst = tgtPos.SqDistance(srcPos + tgtDir * gndDst);
		if ((gndDst > 0.0f) && (tgtDst > Square(view.DamageAreaOfEffect())))
			return false;
	}

	if (spread < 0.001f)
		return (view.TraceRayNoEnemyNoGroundDist(srcPos, tgtDir, length, avoidFlags) >= length);

	return (!view.TestCone(srcPos, tgtDir, length, spread, avoidFlags));
}

// CCannon::CalcWantedDir (pure ballistic math over captured scalars)
template<class V>
float3 CalcWantedDirCannonT(const V& view, const float3& targetVec)
{
	const float Dsq = targetVec.SqLength();
	const float DFsq = targetVec.SqLength2D();
	const float g = view.CannonGravity();
	const float v = view.ProjectileSpeed();
	const float dy = targetVec.y;
	const float dxz = math::sqrt(DFsq);
	const bool highTrajectory = view.CannonHighTraj();

	float Vxz = 0.0f;
	float Vy = 0.0f;

	if (Dsq == 0.0f) {
		Vy = highTrajectory ? v : -v;
	} else {
		if (Dsq < 1e12f && math::fabs(dy) < 1e6f) {
			const float vsq = v * v;
			const float root1 = vsq * vsq + 2.0f * vsq * g * dy - g * g * DFsq;
			if (root1 >= 0.0f) {
				const float root2 = 2.0f * DFsq * Dsq * (vsq + g * dy + (highTrajectory ? -1.0f : 1.0f) * math::sqrt(root1));
				if (root2 >= 0.0f) {
					Vxz = math::sqrt(root2) / (2.0f * Dsq);
					Vy = (dxz == 0.0f || Vxz == 0.0f) ? v : (Vxz * dy / dxz - dxz * g / (2.0f * Vxz));
				}
			}
		}
	}

	float3 nextWantedDir;
	nextWantedDir.x = targetVec.x;
	nextWantedDir.z = targetVec.z;
	nextWantedDir.SafeNormalize();

	if (Vxz != 0.0f || Vy != 0.0f) {
		nextWantedDir *= Vxz;
		nextWantedDir.y = Vy;
		nextWantedDir.SafeNormalize();
	}

	return nextWantedDir;
}

// CCannon::HaveFreeLineOfFire
template<class V>
bool HaveFreeLineOfFireCannonT(const V& view, const float3& srcPos, const float3& tgtPos, const typename V::Target& trg)
{
	if (!view.Def()->waterweapon && TargetUnderWaterT(view, tgtPos, trg))
		return false;

	if (view.ProjectileSpeed() == 0.0f)
		return true;

	const float3 launchDir = CalcWantedDirCannonT(view, tgtPos - srcPos);
	const float3 targetVec = (tgtPos - srcPos) * XZVector;

	if (launchDir.SqLength() == 0.0f)
		return false;
	if (targetVec.SqLength2D() == 0.0f)
		return true;

	float3 tv = targetVec;
	const float xzTargetDist = tv.LengthNormalize();

	const float projectileSpeedHorizontal = std::max(0.001f, view.ProjectileSpeed() * launchDir.Length2D());
	const float projectileSpeedVertical = view.ProjectileSpeed() * launchDir.y;
	const float g = view.CannonGravity();
	const float linCoeff = (projectileSpeedVertical + (g * 0.5f)) / projectileSpeedHorizontal;
	const float qdrCoeff = (g * 0.5f) / (projectileSpeedHorizontal * projectileSpeedHorizontal);

	const float groundColCheckDistance = std::max(10.0f, 0.9375f * xzTargetDist);
	const float groundDist = ((view.AvoidFlags() & Collision::NOGROUND) == 0)
		? view.TrajectoryGroundCol(srcPos, tv, groundColCheckDistance, linCoeff, qdrCoeff)
		: -1.0f;
	const float angleSpread = (view.AccuracyExperience() + view.SprayAngleExperience()) * 0.6f * 0.9f;

	if (groundDist > 0.0f)
		return false;

	return (!view.TestTrajectoryCone(srcPos, tv, xzTargetDist, linCoeff, qdrCoeff, angleSpread, view.AvoidFlags()));
}

// CStarburstLauncher::HaveFreeLineOfFire
template<class V>
bool HaveFreeLineOfFireStarburstT(const V& view, const float3& srcPos, const float3& /*tgtPos*/, const typename V::Target& /*trg*/)
{
	return (!view.TestCone(srcPos, view.Def()->fixedLauncher ? view.WeaponDir() : UpVector, 100.0f, 0.0f, view.AvoidFlags()));
}

// CMissileLauncher::HaveFreeLineOfFire. The trajectoryHeight branch is a
// pursuit-curve integration + a bespoke per-segment quad/ground collision scan
// that is singularly unique to this weapon (its own comment says so) and reads
// unsynced draw-debug globals inline. Rather than reconstruct that math (a
// byte-identity hazard) it stays a VIEW PRIMITIVE: LiveView backs it with the
// verbatim live body (byte-identical by construction), the epoch view with the
// pick-grid equivalent. The linear (non-trajectory) case reuses the base body.
template<class V>
bool HaveFreeLineOfFireMissileT(const V& view, const float3& srcPos, const float3& tgtPos, const typename V::Target& trg)
{
	if (view.Def()->trajectoryHeight <= 0.0f)
		return HaveFreeLineOfFireBaseT(view, srcPos, tgtPos, trg);

	return view.MissileTrajectoryLOF(srcPos, tgtPos, trg);
}

template<class V>
bool HaveFreeLineOfFireT(const V& view, const float3& srcPos, const float3& tgtPos, const typename V::Target& trg)
{
	switch (view.GetWeaponClass()) {
		case WeaponClass::Cannon:            return HaveFreeLineOfFireCannonT(view, srcPos, tgtPos, trg);
		case WeaponClass::MissileLauncher:   return HaveFreeLineOfFireMissileT(view, srcPos, tgtPos, trg);
		case WeaponClass::StarburstLauncher: return HaveFreeLineOfFireStarburstT(view, srcPos, tgtPos, trg);
		case WeaponClass::BombDropper:
		case WeaponClass::MeleeWeapon:
		case WeaponClass::PlasmaRepulser:    return true;
		default:                             return HaveFreeLineOfFireBaseT(view, srcPos, tgtPos, trg);
	}
}

// ---------------------------------------------------------------------------
// TryTarget (non-virtual)
// ---------------------------------------------------------------------------
template<class V>
bool TryTargetT(const V& view, const float3& tgtPos, const typename V::Target& trg, bool preFire)
{
	if (!TestTargetT(view, tgtPos, trg))
		return false;

	if (!trg.isAutoTarget && !TestRangeT(view, tgtPos, trg))
		return false;

	const float3 wmp = view.WeaponMuzzlePos();
	if (preFire && (wmp.y < view.GroundHeightReal(wmp.x, wmp.z)))
		return false;

	return (HaveFreeLineOfFireT(view, GetAimFromPosT(view, preFire), tgtPos, trg));
}

// CWeapon::TryTarget(const SWeaponTarget&): TryTarget(GetLeadTargetPos(trg), trg)
// -- the 3-arg TryTarget's preFire defaults to FALSE, so this passes preFire=false
// (NOT true: preFire gates the muzzle-below-ground check + GetAimFromPos source).
template<class V>
bool TryTargetT(const V& view, const typename V::Target& trg)
{
	return TryTargetT(view, GetLeadTargetPosT(view, trg), trg, false);
}

} // namespace trace


// ---------------------------------------------------------------------------
// LiveView -- the live-state backend (globals + live CWeapon*/CUnit*). Every
// accessor is an inline forwarder to today's exact expression, so the LiveView
// instantiation of the templates above inlines to the original code
// (byte-identical -- these run in synced aim loops every frame). The two
// pathological scans (missile trajectory, target-border colvol) forward to
// verbatim live methods. Heavy sim includes live here, pulled in only by the
// weapon TUs (+ the stage-4 epoch serve TU) that instantiate the predicates.
// ---------------------------------------------------------------------------

#include "Cannon.h"
#include "BombDropper.h"
#include "MissileLauncher.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"

namespace trace {

struct LiveView {
	using Target = SWeaponTarget;
	using UnitRef = const CUnit*;

	const CWeapon* weapon;
	const CUnit* owner;

	explicit LiveView(const CWeapon* w) : weapon(w), owner(w->owner) {}

	// ---- weapon scalars / vectors ----
	WeaponClass GetWeaponClass() const { return ClassifyWeapon(weapon); }
	const WeaponDef* Def() const { return weapon->weaponDef; }
	float Range() const { return weapon->range; }
	float ProjectileSpeed() const { return weapon->projectileSpeed; }
	float3 AimFromPos() const { return weapon->aimFromPos; }
	float3 WeaponMuzzlePos() const { return weapon->weaponMuzzlePos; }
	float3 WeaponDir() const { return weapon->weaponDir; }
	float3 MainDir() const { return weapon->mainDir; }
	float3 CurrentTargetPos() const { return weapon->GetCurrentTargetPos(); }
	float3 ErrorVector() const { return weapon->errorVector; }
	uint32_t AvoidFlags() const { return weapon->avoidFlags; }
	bool OnlyForward() const { return weapon->onlyForward; }
	bool DoTargetGroundPos() const { return weapon->doTargetGroundPos; }
	uint32_t OnlyTargetCategory() const { return weapon->onlyTargetCategory; }
	float MaxForwardAngleDif() const { return weapon->maxForwardAngleDif; }
	float MaxMainDirAngleDif() const { return weapon->maxMainDirAngleDif; }
	float HeightBoostFactor() const { return weapon->heightBoostFactor; }
	float PredictSpeedMod() const { return weapon->predictSpeedMod; }
	unsigned int AccurateLeading() const { return weapon->accurateLeading; }
	int SalvoSize() const { return weapon->salvoSize; }
	int SalvoDelay() const { return weapon->salvoDelay; }
	float AccuracyExperience() const { return weapon->AccuracyExperience(); }
	float SprayAngleExperience() const { return weapon->SprayAngleExperience(); }
	float MoveErrorExperience() const { return weapon->MoveErrorExperience(); }
	float DamageAreaOfEffect() const { return weapon->damages->damageAreaOfEffect; }

	float CannonGravity() const { return static_cast<const CCannon*>(weapon)->GetGravity(); }
	float CannonRangeBoost() const { return static_cast<const CCannon*>(weapon)->GetRangeBoostFactor(); }
	bool CannonHighTraj() const { return static_cast<const CCannon*>(weapon)->GetHighTrajectory(); }
	bool BombDropTorpedoes() const { return static_cast<const CBombDropper*>(weapon)->GetDropTorpedoes(); }
	float BombTorpMoveRange() const { return static_cast<const CBombDropper*>(weapon)->GetTorpMoveRange(); }

	// ---- owner ----
	int OwnerAllyTeam() const { return owner->allyteam; }
	bool OwnerCanManualFire() const { return owner->unitDef->canManualFire; }
	int OwnerFireState() const { return owner->fireState; }
	bool OwnerIsUnderWater() const { return owner->IsUnderWater(); }
	float3 OwnerFrontDir() const { return owner->frontdir; }
	float OwnerExperience() const { return owner->experience; }
	bool OwnerUseHighTrajectory() const { return owner->useHighTrajectory; }
	bool OwnerUnderFirstPersonControl() const { return owner->UnderFirstPersonControl(); }
	float3 OwnerObjectSpaceVec(const float3& v) const { return owner->GetObjectSpaceVec(v); }
	float3 OwnerSpeed() const { return owner->speed; }

	// ---- globals ----
	bool Ally(int a, int b) const { return teamHandler.Ally(a, b); }
	float GroundHeightReal(float x, float z) const { return CGround::GetHeightReal(x, z); }
	float GroundApproxHeight(float x, float z) const { return CGround::GetApproximateHeight(x, z); }
	float GroundHeightAboveWater(float x, float z) const { return CGround::GetHeightAboveWater(x, z); }

	// ---- target (SWeaponTarget object derefs) ----
	UnitRef TargetUnit(const Target& t) const { return t.unit; }
	bool TgtUnitIsNull(const Target& t) const { return t.unit == nullptr; }
	bool TgtUnitIsOwner(const Target& t) const { return t.unit == owner; }
	uint32_t TgtUnitCategory(const Target& t) const { return t.unit->category; }
	bool TgtUnitIsDead(const Target& t) const { return t.unit->isDead; }
	bool TgtUnitIsCrashing(const Target& t) const { return t.unit->IsCrashing(); }
	bool TgtUnitInLosOrRadar(const Target& t) const { return (t.unit->losStatus[owner->allyteam] & (LOS_INLOS | LOS_INRADAR)) != 0; }
	bool TgtUnitIsNeutral(const Target& t) const { return t.unit->IsNeutral(); }
	int TgtUnitAllyTeam(const Target& t) const { return t.unit->allyteam; }
	bool TgtUnitHasTransporter(const Target& t) const { return t.unit->GetTransporter() != nullptr; }
	float3 TgtUnitPos(const Target& t) const { return t.unit->pos; }
	bool TgtUnitIsUnderWater(const Target& t) const { return t.unit->IsUnderWater(); }
	bool TgtUnitIsInWater(const Target& t) const { return t.unit->IsInWater(); }

	bool TgtInterceptIsBeingIntercepted(const Target& t) const { return t.intercept->IsBeingIntercepted(); }
	bool TgtInterceptCanBeInterceptedByDef(const Target& t) const { return t.intercept->CanBeInterceptedBy(weapon->weaponDef); }
	float3 TgtInterceptPos(const Target& t) const { return t.intercept->pos; }
	float3 TgtInterceptSpeed(const Target& t) const { return t.intercept->speed; }

	// ---- lead-chain unit derefs (target unit) ----
	float3 UnitPos(UnitRef u) const { return u->pos; }
	float3 UnitSpeed(UnitRef u) const { return u->speed; }
	float UnitSpeedW(UnitRef u) const { return u->speed.w; }
	float3 UnitAimPos(UnitRef u) const { return u->aimPos; }
	float3 UnitErrorPos(UnitRef u) const { return u->GetErrorPos(owner->allyteam, true); }
	float3 TargetBorderPos(UnitRef u, const float3& rawPos, const float3& rawDir) const {
		return weapon->GetTargetBorderPos(u, rawPos, rawDir);
	}

	// ---- trace primitives (forward to the live TraceRay / CGround) ----
	float TraceRayGroundDist(const float3& srcPos, const float3& dir, float length) const {
		CUnit* u = nullptr; CFeature* f = nullptr;
		return TraceRay::TraceRay(srcPos, dir, length, ~Collision::NOGROUND, owner, u, f);
	}
	float TraceRayNoEnemyNoGroundDist(const float3& srcPos, const float3& dir, float length, uint32_t avoidFlags) const {
		CUnit* u = nullptr; CFeature* f = nullptr;
		return TraceRay::TraceRay(srcPos, dir, length, avoidFlags | Collision::NOENEMIES | Collision::NOGROUND, owner, u, f);
	}
	bool TestCone(const float3& srcPos, const float3& dir, float length, float spread, uint32_t avoidFlags) const {
		return TraceRay::TestCone(srcPos, dir, length, spread, owner->allyteam, avoidFlags, const_cast<CUnit*>(owner));
	}
	float TrajectoryGroundCol(const float3& srcPos, const float3& targetVec, float dist, float linCoeff, float qdrCoeff) const {
		return CGround::TrajectoryGroundCol(srcPos, targetVec, dist, linCoeff, qdrCoeff);
	}
	bool TestTrajectoryCone(const float3& srcPos, const float3& targetVec, float dist, float linCoeff, float qdrCoeff, float spread, uint32_t avoidFlags) const {
		return TraceRay::TestTrajectoryCone(srcPos, targetVec, dist, linCoeff, qdrCoeff, spread, owner->allyteam, avoidFlags, const_cast<CUnit*>(owner));
	}
	bool MissileTrajectoryLOF(const float3& srcPos, const float3& tgtPos, const Target& trg) const {
		return static_cast<const CMissileLauncher*>(weapon)->TrajectoryLOF(srcPos, tgtPos, trg);
	}
};

} // namespace trace
