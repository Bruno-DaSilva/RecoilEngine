/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

/**
 * trace::EpochView -- the DRAW-side state backend for the weapon-trace predicate
 * stack (sim/draw split trace re-host, stage 4). The epoch instantiation of the
 * same trace:: templated predicates (WeaponPredicates.h) the sim thread runs with
 * LiveView: per-weapon scalars/vectors come from the published PR-31 + stage-1
 * per-weapon SoA (flat arrays indexed weaponOffset[ownerID] + weaponNum), owner
 * and target attributes from the SimSnapshot UnitRows joined by id, immutable
 * WeaponDef / UnitDef scalars from the (thread-safe) def handlers, and the
 * collision / cone / ground primitives from the object-free CCollisionHandler +
 * SnapshotPickGrid + DrawMapMirrors backends. Mirrors PlacementEpochView.h.
 *
 * UnitRef is a SimSnapshot row id (int32), never a live pointer. Target is the
 * POD the serve layer builds from the query (type/isUserTarget/.../unitID/
 * groundPos); the Lua trace callouts only ever build Target_None/Unit/Pos, so the
 * Target_Intercept accessors are assert-unreachable. Read-only, one-boundary-
 * stale by design -- the armed flag-off dual-run proves it bit-equal to LiveView.
 *
 * STAGE 4a: the scalar / vector / owner / target / lead-chain half is complete
 * and bit-exact; the six trace primitives (+ the three ground reads) are STUBBED
 * to a conservative "clear line-of-fire" so the armed dual-run proves the scalar
 * half (TestTarget / TestRange) green while the collision-reaching callouts
 * (TryTarget / HaveFreeLineOfFire) mismatch only where a real obstruction exists.
 * STAGE 4b wires the primitives (object-free DetectHit / QueryCone / the mirrored
 * ground) to bit-exact and turns those green too.
 */

#include <cassert>
#include <cstdint>

#include "DrawMapMirrors.h"
#include "SimSnapshot.h"
#include "SnapshotPickGrid.h"

#include "Map/Ground.h"                   // CGround (synced=false unsynced-heightmap reads)
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Objects/SolidObject.h"      // PSTATE_BIT_*
#include "Sim/Units/Unit.h"               // LOS_INLOS / LOS_INRADAR (losStatus bits)
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Weapons/WeaponDef.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Sim/Weapons/WeaponTarget.h"     // TargetType
#include "Sim/Weapons/WeaponTraceClass.h" // trace::WeaponClass
#include "System/Matrix44f.h"
#include "System/float3.h"
#include "System/float4.h"

namespace trace {

struct EpochView {
	using UnitRef = int32_t;   // SimSnapshot unit row id

	// POD parallel of the SWeaponTarget the live callouts pass. The serve layer
	// fills it from the query (see EvaluateTraceQueryEpoch); Target_Unit reads
	// unitID (a row id), Target_Pos reads groundPos, Target_Intercept never built.
	struct Target {
		TargetType type = Target_None;
		bool isUserTarget = false;
		bool isAutoTarget = false;
		bool isManualFire = false;
		int32_t unitID = -1;
		float3 groundPos;
	};

	const SimSnapshot::UnitRows& urows;
	int32_t ownerID = -1;
	int32_t weaponNum = 0;
	int32_t widx = 0;          // weaponOffset[ownerID] + weaponNum (flat SoA index)
	int32_t ownerAllyTeam = -1;

	EpochView(int32_t owner, int32_t wnum)
		: urows(simSnapshot.Read())
		, ownerID(owner)
		, weaponNum(wnum)
	{
		// BuildTraceQuery validated owner Valid() + weaponNum < weaponCount[owner]
		widx = urows.weaponOffset[ownerID] + weaponNum;
		ownerAllyTeam = urows.AllyTeam(ownerID);
	}

	// ===================== weapon scalars / vectors (@ widx) =====================
	WeaponClass GetWeaponClass() const { return static_cast<WeaponClass>(urows.wWeaponClass[widx]); }
	const WeaponDef* Def() const { return weaponDefHandler->GetWeaponDefByID(urows.wWeaponDefID[widx]); }
	float Range() const { return urows.wRange[widx]; }
	float ProjectileSpeed() const { return urows.wProjectileSpeed[widx]; }
	float3 AimFromPos() const { return urows.wAimFromPos[widx]; }
	float3 WeaponMuzzlePos() const { return urows.wMuzzlePos[widx]; }
	float3 WeaponDir() const { return urows.wWeaponDir[widx]; }
	float3 MainDir() const { return urows.wMainDir[widx]; }
	float3 CurrentTargetPos() const { return urows.wCurrentTargetPos[widx]; }
	float3 ErrorVector() const { return urows.wErrorVector[widx]; }
	uint32_t AvoidFlags() const { return urows.wAvoidFlags[widx]; }
	bool OnlyForward() const { return urows.wOnlyForward[widx] != 0; }
	bool DoTargetGroundPos() const { return urows.wDoTargetGroundPos[widx] != 0; }
	uint32_t OnlyTargetCategory() const { return urows.wOnlyTargetCategory[widx]; }
	float MaxForwardAngleDif() const { return urows.wMaxForwardAngleDif[widx]; }
	float MaxMainDirAngleDif() const { return urows.wMaxMainDirAngleDif[widx]; }
	float HeightBoostFactor() const { return urows.wHeightBoostFactor[widx]; }
	float PredictSpeedMod() const { return urows.wPredictSpeedMod[widx]; }
	unsigned int AccurateLeading() const { return static_cast<unsigned int>(urows.wAccurateLeading[widx]); }
	int SalvoSize() const { return urows.wSalvoSize[widx]; }
	int SalvoDelay() const { return urows.wSalvoDelay[widx]; }
	float AccuracyExperience() const { return urows.wAccuracyExp[widx]; }
	float SprayAngleExperience() const { return urows.wSprayAngleExp[widx]; }
	float MoveErrorExperience() const { return urows.wMoveErrorExp[widx]; }
	float DamageAreaOfEffect() const { return urows.wDamages[widx].damageAreaOfEffect; }

	float CannonGravity() const { return urows.wCannonGravity[widx]; }
	float CannonRangeBoost() const { return urows.wCannonRangeBoost[widx]; }
	bool CannonHighTraj() const { return urows.wCannonHighTraj[widx] != 0; }
	bool BombDropTorpedoes() const { return urows.wBombDropTorpedoes[widx] != 0; }
	float BombTorpMoveRange() const { return urows.wBombTorpMoveRange[widx]; }

	// ============================== owner (@ ownerID) =============================
	int OwnerAllyTeam() const { return ownerAllyTeam; }
	bool OwnerCanManualFire() const {
		const UnitDef* ud = unitDefHandler->GetUnitDefByID(urows.DefID(ownerID));
		return (ud != nullptr) && ud->canManualFire;
	}
	int OwnerFireState() const { return urows.fireState[ownerID]; }
	bool OwnerIsUnderWater() const { return (urows.PhysicalState(ownerID) & CSolidObject::PSTATE_BIT_UNDERWATER) != 0; }
	float3 OwnerFrontDir() const { return urows.Frontdir(ownerID); }
	float OwnerExperience() const { return urows.Experience(ownerID); }
	bool OwnerUseHighTrajectory() const { return urows.useHighTrajectory[ownerID] != 0; }
	bool OwnerUnderFirstPersonControl() const { return urows.underFirstPersonControl[ownerID] != 0; }
	float3 OwnerObjectSpaceVec(const float3& v) const { return urows.ObjectSpaceVec(ownerID, v); }
	float3 OwnerSpeed() const { const float4 s = urows.Speed(ownerID); return float3(s.x, s.y, s.z); }

	// ================================ globals ====================================
	bool Ally(int a, int b) const { return urows.Allied(a, b); }
	// ---- ground: the UNSYNCED heightmap (synced=false), the placement BuildHeight
	// precedent -- identical to the live synced read in the flag-off dual-run
	// (unsynced == synced at the draw phase), a documented <=1-boundary deviation
	// flag-ON ----
	float GroundHeightReal(float x, float z) const { return CGround::GetHeightReal(x, z, false); }
	float GroundApproxHeight(float x, float z) const { return CGround::GetApproximateHeight(x, z, false); }
	float GroundHeightAboveWater(float x, float z) const { return CGround::GetHeightAboveWater(x, z, false); }

	// ======================== target derefs (Target POD) =========================
	UnitRef TargetUnit(const Target& t) const { return t.unitID; }
	bool TgtUnitIsNull(const Target& t) const { return t.unitID < 0 || !urows.Valid(t.unitID); }
	bool TgtUnitIsOwner(const Target& t) const { return t.unitID == ownerID; }
	uint32_t TgtUnitCategory(const Target& t) const { return urows.category[t.unitID]; }
	bool TgtUnitIsDead(const Target& t) const { return urows.IsDead(t.unitID); }
	bool TgtUnitIsCrashing(const Target& t) const { return urows.crashing[t.unitID] != 0; }
	bool TgtUnitInLosOrRadar(const Target& t) const {
		return (urows.LosStatus(t.unitID, ownerAllyTeam) & (LOS_INLOS | LOS_INRADAR)) != 0;
	}
	bool TgtUnitIsNeutral(const Target& t) const { return urows.Neutral(t.unitID); }
	int TgtUnitAllyTeam(const Target& t) const { return urows.AllyTeam(t.unitID); }
	bool TgtUnitHasTransporter(const Target& t) const { return urows.transporterID[t.unitID] >= 0; }
	float3 TgtUnitPos(const Target& t) const { return urows.Pos(t.unitID); }
	bool TgtUnitIsUnderWater(const Target& t) const { return (urows.PhysicalState(t.unitID) & CSolidObject::PSTATE_BIT_UNDERWATER) != 0; }
	bool TgtUnitIsInWater(const Target& t) const { return (urows.PhysicalState(t.unitID) & CSolidObject::PSTATE_BIT_INWATER) != 0; }

	// Target_Intercept never built by the Lua callouts (asserted unreachable)
	bool TgtInterceptIsBeingIntercepted(const Target&) const { assert(false); return false; }
	bool TgtInterceptCanBeInterceptedByDef(const Target&) const { assert(false); return false; }
	float3 TgtInterceptPos(const Target&) const { assert(false); return ZeroVector; }
	float3 TgtInterceptSpeed(const Target&) const { assert(false); return ZeroVector; }

	// ===================== lead-chain unit derefs (UnitRef) ======================
	float3 UnitPos(UnitRef u) const { return urows.Pos(u); }
	float3 UnitSpeed(UnitRef u) const { const float4 s = urows.Speed(u); return float3(s.x, s.y, s.z); }
	float UnitSpeedW(UnitRef u) const { return urows.Speed(u).w; }
	float3 UnitAimPos(UnitRef u) const { return urows.aimPos[u]; }
	// CUnit::GetErrorPos(allyteam, aiming=true) == aimPos + GetErrorVector(allyteam)
	float3 UnitErrorPos(UnitRef u) const { return urows.aimPos[u] + urows.ErrorVector(u, ownerAllyTeam); }

	// ------- trace primitives (STUB stage 4a; bit-exact in stage 4b) -------
	// TargetBorderPos returns rawPos verbatim, which is EXACT whenever
	// weaponDef->targetBorder == 0 (the early-out in the live body); non-zero
	// targetBorder mismatches until 4b wires the object-free DetectHit + colvol.
	float3 TargetBorderPos(UnitRef /*u*/, const float3& rawPos, const float3& /*rawDir*/) const { return rawPos; }
	// ground-only ray march (LiveView routes TraceRay(~NOGROUND) here): object scans
	// are all masked off, so this is exactly TraceRay's ground leg -- return the
	// ground-hit distance if the ray hits ground within [0,length], else length.
	float TraceRayGroundDist(const float3& srcPos, const float3& dir, float length) const {
		if (dir == ZeroVector)
			return -1.0f;
		float traceLength = length;
		const float groundLength = CGround::LineGroundCol(srcPos, srcPos + dir * traceLength, false);
		if (traceLength > groundLength && groundLength > 0.0f)
			traceLength = groundLength;
		return traceLength;
	}
	// the remaining four return "clear line of fire" (no obstruction found)
	float TraceRayNoEnemyNoGroundDist(const float3&, const float3&, float length, uint32_t) const { return length; } // FIXME(4b-2)
	bool TestCone(const float3&, const float3&, float, float, uint32_t) const { return false; }                // FIXME(4b)
	float TrajectoryGroundCol(const float3&, const float3&, float, float, float) const { return -1.0f; }       // FIXME(4b)
	bool TestTrajectoryCone(const float3&, const float3&, float, float, float, float, uint32_t) const { return false; } // FIXME(4b)
	bool MissileTrajectoryLOF(const float3&, const float3&, const Target&) const { return true; }              // FIXME(4b)
};

} // namespace trace
