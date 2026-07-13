/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef COLLISION_HANDLER_H
#define COLLISION_HANDLER_H

#include "System/creg/creg_cond.h"
#include "System/float3.h"
#include "System/Matrix44f.h"

#include <algorithm>

class CSolidObject;
struct LocalModelPiece;
struct CollisionVolume;

enum {
	CQ_POINT_NO_INT = 0,
	CQ_POINT_ON_RAY = 1,
	CQ_POINT_IN_VOL = 2,
};

struct CollisionQuery {
public:
	bool SwapParams() {
		if (!AllHit() || ValidRay())
			return false;

		std::swap(t1, t0);
		std::swap(p1, p0);
		std::swap(b1, b0);
		return true;
	}
	void Transform(const CMatrix44f& m) {
		// transform intersection points (iff not a special
		// case, otherwise calling code should not use them)
		if (b0 == CQ_POINT_ON_RAY) { p0 = m.Mul(p0); }
		if (b1 == CQ_POINT_ON_RAY) { p1 = m.Mul(p1); }
	}

	void Reset(const CollisionQuery* cq = nullptr) {
		*this = (cq != nullptr) ? *cq : CollisionQuery{};
	}

	// t0 > t1 can happen when intersecting cylinder endcaps
	bool ValidRay() const { return (t0 <= t1); }
	bool InsideHit() const { return (b0 == CQ_POINT_IN_VOL); }
	bool IngressHit() const { return (b0 == CQ_POINT_ON_RAY); }
	bool EgressHit() const { return (b1 == CQ_POINT_ON_RAY); }
	bool AllHit() const { return (b0 != CQ_POINT_NO_INT && b1 != CQ_POINT_NO_INT); }
	bool AnyHit() const { return (b0 != CQ_POINT_NO_INT || b1 != CQ_POINT_NO_INT); }

	const float3& GetIngressPos() const { return p0; }
	const float3& GetEgressPos() const { return p1; }
	const float3& GetHitPos() const {
		if (IngressHit()) return GetIngressPos();
		if (EgressHit()) return GetEgressPos();
		if (InsideHit()) return p0;
		return ZeroVector;
	}

	// if the hit-position equals ZeroVector (i.e. if we have an
	// inside-hit special case), the projected distance could be
	// positive or negative depending on <dir> but we want it to
	// be 0 --> turn <pos> into a ZeroVector if InsideHit()
	float GetHitPosDist(const float3& pos, const float3& dir) const { return (std::max(0.0f, dir.dot(GetHitPos() - pos * (1 - InsideHit())))); }
	float GetIngressPosDist(const float3& pos, const float3& dir) const { return (std::max(0.0f, dir.dot(GetIngressPos() - pos))); }
	float GetEgressPosDist(const float3& pos, const float3& dir) const { return (std::max(0.0f, dir.dot(GetEgressPos() - pos))); }

	const LocalModelPiece* GetHitPiece() const { return lmp; }
	void SetHitPiece(const LocalModelPiece* p) { lmp = p; }

private:
	friend class CCollisionHandler;

	///< true (non-zero) if {in,e}gress (b{0,1}) point on ray segment
	int    b0 = CQ_POINT_NO_INT;
	int    b1 = CQ_POINT_NO_INT;
	///< distance parameter for ingress and egress point
	float  t0 = 0.0f;
	float  t1 = 0.0f;
	///< ray-volume ingress and egress points
	float3 p0;
	float3 p1;

	///< impacted piece
	const LocalModelPiece* lmp = nullptr;
};

/**
 * Responsible for detecting hits between projectiles
 * and solid objects (units, features), each SO has a
 * collision volume.
 */
class CCollisionHandler {
	public:
		static void PrintStats();

		static bool DetectHit(
			const CSolidObject* o,
			const CMatrix44f& m,
			const float3 p0,
			const float3 p1,
			CollisionQuery* cq = nullptr,
			bool forceTrace = false
		);
		static bool DetectHit(
			const CSolidObject* o,
			const CollisionVolume* v,
			const CMatrix44f& m,
			const float3 p0,
			const float3 p1,
			CollisionQuery* cq = nullptr,
			bool forceTrace = false
		);
		static bool MouseHit(
			const CSolidObject* o,
			const CMatrix44f& m,
			const float3& p0,
			const float3& p1,
			const CollisionVolume* v,
			CollisionQuery* cq = nullptr
		);
		// draw-side picking variant (PR 25 sim/draw decoupling): the object's
		// midpos-relative offset and in-void state are passed explicitly so the
		// hit-test needs no live CSolidObject (the caller reads them from the
		// render-side SimSnapshot). The piece-tree branch (DefaultToPieceTree)
		// is NOT served here -- it returns false and the caller falls back to a
		// live sim read (see TraceRay.cpp). Simple-volume math is byte-identical
		// to the object overload (same Intersect(v, mr, ...)).
		static bool MouseHit(
			const float3& relMidPos,
			bool isInVoid,
			const CMatrix44f& m,
			const float3& p0,
			const float3& p1,
			const CollisionVolume* v,
			CollisionQuery* cq = nullptr
		);
		// object-free DetectHit variant (sim/draw split trace re-host): the
		// object's midPos / relMidPos / in-void state are passed explicitly (read
		// from the render-side SimSnapshot rows / derived transform), so the test
		// needs no live CSolidObject. Mirrors the object DetectHit overload
		// (disc/ray + forceTrace) with simple-volume math byte-identical to it.
		// Two volume classes need a live read and are NOT served here (the caller
		// pre-checks the volume and falls back, exactly like the object-free
		// MouseHit defers the piece tree): DefaultToPieceTree (per-piece hit
		// volumes -> demand piece cache) and DefaultToFootPrint (blocking-map
		// object identity) both return false.
		static bool DetectHit(
			const float3& midPos,
			const float3& relMidPos,
			bool isInVoid,
			const CollisionVolume* v,
			const CMatrix44f& m,
			const float3 p0,
			const float3 p1,
			CollisionQuery* cq = nullptr,
			bool forceTrace = false
		);

	private:
		// HITTEST_DISC helpers for DetectHit
		static bool Collision(
			const CSolidObject* o,
			const CollisionVolume* v,
			const CMatrix44f& m,
			const float3 p,
			CollisionQuery* cq
		);
		// HITTEST_CONT helpers for DetectHit
		static bool Intersect(
			const CSolidObject* o,
			const CollisionVolume* v,
			const CMatrix44f& m,
			const float3 p0,
			const float3 p1,
			CollisionQuery* cq,
			float s = 1.0f
		);

	private:
		/**
		 * Test if a point lies inside a volume.
		 * @param v volume
		 * @param m volumes transformation matrix
		 * @param p point in world-coordinates
		 */
		static bool Collision(const CollisionVolume* v, const CMatrix44f& m, const float3& p);
		static bool CollisionFootPrint(const CSolidObject* o, const float3& p);

		/**
		 * Test if a ray intersects a volume.
		 * @param v volume
		 * @param m volumes transformation matrix
		 * @param p0 start of ray (in world-coordinates)
		 * @param p1 end of ray (in world-coordinates)
		 */
		static bool IntersectPieceTree(const CSolidObject* o, const CMatrix44f& m, const float3& p0, const float3& p1, CollisionQuery* cq);
		static bool IntersectPiecesHelper(const CSolidObject* o, const CMatrix44f& m, const float3& p0, const float3& p1, CollisionQuery* cqp);

	public:
		// volume-only ray intersect -- public for the sim/draw split trace re-host's
		// object-free per-piece path (mirrors IntersectPiecesHelper over the demand
		// piece-cache colvols; same rationale as the object-free MouseHit)
		static bool Intersect(const CollisionVolume* v, const CMatrix44f& m, const float3& p0, const float3& p1, CollisionQuery* cq);
		static bool IntersectEllipsoid(const CollisionVolume* v, const float3& pi0, const float3& pi1, CollisionQuery* cq);
		static bool IntersectCylinder(const CollisionVolume* v, const float3& pi0, const float3& pi1, CollisionQuery* cq);
		static bool IntersectBox(const CollisionVolume* v, const float3& pi0, const float3& pi1, CollisionQuery* cq);

	private:
		static unsigned int numDiscTests; // number of discrete hit-tests executed
		static unsigned int numContTests; // number of continuous hit-tests executed (inc. unsynced)
};

#endif // COLLISION_HANDLER_H
