/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/Object.h"
#include "System/float4.h"
#include "System/Transform.hpp"
#include "System/Threading/ThreadPool.h"
#include "System/SpringMath.h"

struct S3DModel;

// Render-side draw-visibility bitset. The per-object drawFlag/previousDrawFlag
// values are authored and consumed exclusively by draw code and live in
// drawer-owned storage (CModelDrawerDataBase<T> for units/features,
// CProjectileDrawer for projectiles) since the sim/draw §A eviction (PR 4).
// The enum only lives in this (sim) header for include locality — it is
// transitively visible everywhere the drawers and their consumers need it.
enum DrawFlags : uint8_t {
	SO_NODRAW_FLAG = 0, // must be 0
	SO_OPAQUE_FLAG = 1,
	SO_ALPHAF_FLAG = 2, //design oversight, should be split to alpha_below_water, alpha_above_water for better CPU side culling
	SO_REFLEC_FLAG = 4,
	SO_REFRAC_FLAG = 8,
	SO_SHOPAQ_FLAG = 16,
	SO_SHTRAN_FLAG = 32,
	SO_DRICON_FLAG = 128,
};

class CWorldObject: public CObject
{
public:
	CR_DECLARE(CWorldObject)

	CWorldObject() = default;
	CWorldObject(const float3& pos, const float3& spd): CWorldObject()
	{
		SetPosition(pos);
		SetVelocity(spd);
	}

	virtual ~CWorldObject() {}

	// NOTE: used only by projectiles, SolidObject's override this!
	virtual float GetDrawRadius() const { return drawRadius; }
	virtual void  SetDrawRadius(float r) { drawRadius = r; }

	virtual void SetPosition(const float3& p) {   pos = p; }
	virtual void SetVelocity(const float3& v) { speed = v; }

	virtual void SetVelocityAndSpeed(const float3& v) {
		// set velocity first; do not assume f4::op=(f3) will not touch .w
		SetVelocity(v);
		SetSpeed(v);
	}

	// by default, SetVelocity does not set magnitude (for efficiency)
	// so SetSpeed must be explicitly called to update the w-component
	float SetSpeed(const float3& v) { return (speed.w = v.Length()); }
	float SetSpeed(const float s) { return (speed.w = s); }

	void SetRadiusAndHeight(float r, float h) {
		radius = r;
		height = h;
		sqRadius = r * r;
		drawRadius = r;
	}

	void SetRadiusAndHeight(const S3DModel* model);

	// extrapolated base-positions; used in unsynced code
	float3 GetDrawPos(float t) const { return mix(preFrameTra.t, pos, t); }
	float3 GetDrawPosOther(const float3& prevFramePos, const float3& currFramePos, float t) const { return preFrameTra.t + (currFramePos - prevFramePos) * t; }

	inline int GetMtTempNum() const { return mtTempNum[ThreadPool::GetThreadNum()]; }
	inline void SetMtTempNum(int value) { mtTempNum[ThreadPool::GetThreadNum()] = value; }

public:
	int id = -1;
	// Synced/sim-thread scratch marker, paired with CGlobalSynced::GetTempNum();
	// used to dedup an object across QuadField query cells. Synced determinism
	// depends on this counter/field pair keeping its exact increment sequence, so
	// it is written ONLY by sim-context queries. Draw/unsynced-context queries use
	// unsyncedTempNum below instead (see PR 9, sim/draw decoupling).
	int syncedTempNum = 0;      ///< used to check if object has already been processed (in synced QuadField queries, etc)
	// Draw/unsynced counterpart of syncedTempNum, paired with
	// CGlobalUnsynced::GetTempNum(). Written ONLY by draw/unsynced-context queries
	// (LuaUnsyncedRead visibility scans, MiniMap picking, ...); sim never reads it,
	// so it is not creg-serialized. Single-threaded today (every draw-side caller
	// runs on the main thread); if the draw side is ever multithreaded this needs
	// the same per-thread treatment as mtTempNum.
	int unsyncedTempNum = 0;

	Transform preFrameTra;      ///< used for interpolation

	float3 pos;                 ///< position of the very bottom of the object
	float4 speed;               ///< current velocity vector (elmos/frame), .w = |velocity|

	float buildeeRadius = 0.f;	///< used for build, repair, reclaim, capture, resurrect
	float radius = 0.0f;        ///< used for collisions
	float height = 0.0f;        ///< The height of this object
	float sqRadius = 0.0f;

	bool useAirLos = false;     ///< if true, the object's visibility is checked against airLosMap[allyteam]
	bool alwaysVisible = false; ///< if true, object is drawn even if not in LOS

	// drawFlag/previousDrawFlag evicted to drawer-owned storage (sim/draw §A, PR 4)

	S3DModel* model = nullptr;
protected:
	float drawRadius = 0.0f;    ///< unsynced, used for projectile visibility culling
public:
	std::array<int, ThreadPool::MAX_THREADS> mtTempNum = {};
};
