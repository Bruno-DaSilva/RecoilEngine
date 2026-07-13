#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>

#include "System/creg/creg_cond.h"
#include "System/Transform.hpp"
#include "Sim/Misc/CollisionVolume.h"


struct S3DModelPiece;
struct LocalModel;

/**
 * LocalModel
 * Instance of S3DModel. Container for the geometric properties & piece visibility status of the agent's instance of a 3d model.
 */

struct LocalModelPiece
{
	CR_DECLARE_STRUCT(LocalModelPiece)

	LocalModelPiece()
		: dirty(true)
		, wasUpdated{ true }
		, noInterpolation { false }
	{}
	LocalModelPiece(const S3DModelPiece* piece);
	~LocalModelPiece();

	void AddChild(LocalModelPiece* c) { children.push_back(c); }
	void RemoveChild(LocalModelPiece* c) { children.erase(std::find(children.begin(), children.end(), c)); }
	void SetParent(LocalModelPiece* p) { parent = p; }
	void SetLocalModel(LocalModel* lm) { localModel = lm; }

	void SetLModelPieceIndex(uint32_t idx) { lmodelPieceIndex = idx; }
	void SetScriptPieceIndex(uint32_t idx) { scriptPieceIndex = idx; }
	uint32_t GetLModelPieceIndex() const { return lmodelPieceIndex; }
	uint32_t GetScriptPieceIndex() const { return scriptPieceIndex; }

	void Draw() const;
	// lodDispLists was evicted to the drawer render record (sim/draw PR 10); the
	// caller threads in this piece's per-LOD display-list vector
	void DrawLOD(uint32_t lod, const std::vector<uint32_t>& pieceLodLists) const;


	// on-demand functions
	void UpdatePieceSpaceTransform();
	void UpdateModelSpaceTransform(const Transform& pTra);
	void UpdateModelSpaceTransform(const LocalModelPiece* parent);
	void UpdateChildTransformRec(bool updateChildMatrices) const;
	void UpdateParentMatricesRec() const;

	Transform CalcPieceSpaceTransformOrig(const float3& p, const float3& r, float s) const;
	Transform CalcPieceSpaceTransform(const float3& p, const float3& r, float s) const;

	// note: actually OBJECT_TO_WORLD but transform is the same
	float3 GetAbsolutePos() const { return (GetModelSpaceTransform().t * WORLD_TO_OBJECT_SPACE); }

	bool GetEmitDirPos(float3& emitPos, float3& emitDir) const;

	void SetDirtyRaw(bool state) { dirty = state; }
	void SetDirty();
	bool GetDirty() const { return dirty; }
	void SetFloat3(const float3& src, float3& dst); // anim-script only
	void SetFloat(const float& src, float& dst); // anim-script only
	void SetPosition(const float3& p) { SetFloat3(p, pos); } // anim-script only
	void SetRotation(const float3& r) { SetFloat3(r, rot); } // anim-script only
	void SetScaling(const float& s) { SetFloat(s, scale); }    // anim-script only

	// WS-1 §4.1-G: arming no-interpolation changes draw-consumed piece state even
	// when the paired Set{Rotation,Position,Scaling} write bumps nothing (value
	// unchanged or piece already dirty); bump on the false->true transition only.
	// The per-tick disarm calls and the draw-side ResetWasUpdated must not bump.
	void SetRotationNoInterpolation(bool noInterpolate) { if (noInterpolate && !noInterpolation[0]) BumpTreeVersion(); noInterpolation[0] = noInterpolate; }
	void SetPositionNoInterpolation(bool noInterpolate) { if (noInterpolate && !noInterpolation[1]) BumpTreeVersion(); noInterpolation[1] = noInterpolate; }
	void SetScalingNoInterpolation (bool noInterpolate) { if (noInterpolate && !noInterpolation[2]) BumpTreeVersion(); noInterpolation[2] = noInterpolate; }

	void SetWasUpdatedRaw(bool state = true) { wasUpdated[0] = state; }
	auto GetWasUpdated() const { return wasUpdated[0] || wasUpdated[1]; }
	void ResetWasUpdated() const; /*fake*/

	bool SetPieceSpaceMatrix(const CMatrix44f& mat);

	const float3& GetPosition() const { return pos; }
	const float3& GetRotation() const { return rot; }
	const float&  GetScaling() const { return scale; }

	const float3& GetDirection() const { return dir; }

	const Transform& GetModelSpaceTransformRaw() const { return modelSpaceTra; }
	const Transform&  GetModelSpaceTransform() const;
	const CMatrix44f& GetModelSpaceMatrix()    const;

	const CollisionVolume* GetCollisionVolume() const { return colvol; }
	// WS-1 §4.1-D: the only mutable access to the piece colvol -- bumping at the
	// acquisition choke closes the Set*PieceCollisionVolumeData mutation path,
	// and the rename is the compiler lock (read paths resolve to the const
	// overload; a new writer must go through here)
	CollisionVolume* GetCollisionVolumeMutable() { BumpTreeVersion(); return colvol; }

	bool GetScriptVisible() const { return scriptSetVisible; }
	void SetScriptVisible(bool b);

	void SavePrevModelSpaceTransform();
	const Transform& GetPrevModelSpaceTransformRaw() const { return prevModelSpaceTra; }
	Transform GetEffectivePrevModelSpaceTransform() const;

	void PostLoad();
private:
	void BumpTreeVersion();

	mutable CMatrix44f modelSpaceMat; // transform relative to root LMP (SYNCED), chained pieceSpaceMat's
	mutable Transform pieceSpaceTra;  // transform relative to parent LMP (SYNCED), combines <pos> and <rot>
	mutable Transform modelSpaceTra;  // transform relative to root LMP (SYNCED), chained pieceSpaceTra's

	float3 pos;      // translation relative to parent LMP, *INITIALLY* equal to original->offset
	float3 rot;      // orientation relative to parent LMP, in radians (updated by scripts)
	float scale;     // uniform scaling

	mutable std::array<bool, 3> noInterpolation; // rotate, move, scale
	mutable bool dirty;

	Transform prevModelSpaceTra;

	CollisionVolume* colvol;

	float3 dir;      // cached copy of original->GetEmitDir()

	mutable std::array<bool, 2> wasUpdated; // currFrame, prevFrame
	bool scriptSetVisible; // TODO: add (visibility) maxradius!
public:
	bool blockScriptAnims; // if true, Set{Position,Rotation} are ignored for this piece
	int32_t lmodelPieceIndex; // index of this piece into LocalModel::pieces
	int32_t scriptPieceIndex; // index of this piece into UnitScript::pieces

	std::vector<LocalModelPiece*> children;
	LocalModelPiece* parent;

	const S3DModelPiece* original;

	LocalModel* localModel;
};