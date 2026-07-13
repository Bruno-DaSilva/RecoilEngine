/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _FEATURE_H
#define _FEATURE_H

#include "System/Misc/NonCopyable.h"

#include "Sim/Objects/SolidObject.h"
#include "System/Matrix44f.h"
#include "Sim/Misc/Resource.h"

#define TREE_RADIUS 20

struct SolidObjectDef;
struct FeatureDef;
struct FeatureLoadParams;
class CUnit;
struct UnitDef;
class DamageArray;



class CFeature: public CSolidObject, public spring::noncopyable
{
	CR_DECLARE(CFeature)

public:
	CFeature();
	~CFeature();

	void PreDestruct() override;

	CR_DECLARE_SUB(MoveCtrl)
	struct MoveCtrl {
		CR_DECLARE_STRUCT(MoveCtrl)
	public:
		MoveCtrl(): enabled(false) {
			movementMask = OnesVector;
			velocityMask = OnesVector;
			 impulseMask = OnesVector;
		}

		void SetMovementMask(const float3& movMask) { movementMask = movMask; }
		void SetVelocityMask(const float3& velMask) { velocityMask = velMask; }

	public:
		// if true, feature will not apply any unwanted position
		// updates (but is still considered moving so long as its
		// velocity is non-zero, so it stays in the UQ)
		bool enabled;

		// dimensions in which feature can move or receive impulse
		// note: these should always be binary vectors (.xyz={0,1})
		float3 movementMask;
		float3 velocityMask;
		float3 impulseMask;

		float3 velVector;
		float3 accVector;
	};

	/**
	 * Pos of quad must not change after this.
	 * This will add this to the FeatureHandler.
	 */
	void Initialize(const FeatureLoadParams& params);

	const SolidObjectDef* GetDef() const { return ((const SolidObjectDef*) def); }

	int GetBlockingMapID() const;

	/**
	 * Negative amount = reclaim
	 * @return true if reclaimed
	 */
	bool AddBuildPower(CUnit* builder, float amount);
	void DoDamage(const DamageArray& damages, const float3& impulse, CUnit* attacker, int weaponDefID, int projectileID);
	void SetVelocity(const float3& v);
	void ForcedMove(const float3& newPos) override;
	void ForcedSpin(const float3& newDir) override;
	void ForcedSpin(const float3& newFrontDir, const float3& newRightDir) override; 

	bool Update();
	bool UpdatePosition();
	bool UpdateVelocity(const float3& dragAccel, const float3& gravAccel, const float3& movMask, const float3& velMask);

	void UpdateTransform(const float3& p);
	void UpdateTransformAndPhysState();
	void UpdateQuadFieldPosition(const float3& moveVec);

	void StartFire();
	void EmitGeoSmoke();

	void DependentDied(CObject *o);
	void ChangeTeam(int newTeam);

	bool IsInLosForAllyTeam(int argAllyTeam) const;

	// NOTE:
	//   unlike CUnit which recalculates the matrix on each call
	//   CFeature caches it; the draw-time matrix lives in CFeatureDrawerData
	CMatrix44f GetTransformMatrix() const override final { return transMatrix; }
	const CMatrix44f& GetTransformMatrixRef() const { return transMatrix; }

	CFeature* CreateWreck(int wreckLevel, int smokeTime);

private:
	void PostLoad();

	static int ChunkNumber(float f);

public:
	/**
	 * This flag is used to stop a potential exploit involving tripping
	 * a unit back and forth across a chunk boundary to get unlimited resources.
	 * Basically, once a corpse has been a little bit reclaimed,
	 * if they start rezzing, then they cannot reclaim again
	 * until the corpse has been fully 'repaired'.
	 */
	bool isRepairingBeforeResurrect = false;
	bool inUpdateQue = false;
	bool deleteMe = false;
	bool alphaFade = true; // unsynced

	float resurrectProgress = 0.0f;
	float reclaimTime = 0.0f;
	float reclaimLeft = 1.0f;

	int lastReclaimFrame = 0;
	int fireTime = 0;
	int smokeTime = 0;

	SResourcePack defResources = {0.0f, 1.0f};
	SResourcePack resources = {0.0f, 1.0f};

	MoveCtrl moveCtrl;

	const FeatureDef* def = nullptr;
	const UnitDef* udef = nullptr; /// type of unit this feature should be resurrected to

	/// object on top of us if we are a geothermal vent
	CSolidObject* solidOnTop = nullptr;


private:
	// [0] := unsynced, [1] := synced
	CMatrix44f transMatrix;
};

#endif // _FEATURE_H
