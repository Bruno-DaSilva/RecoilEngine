#pragma once

#include <vector>
#include <array>
#include <cstdint>

#include "LocalModelPiece.hpp"
#include "Sim/Misc/CollisionVolume.h"
#include "Lua/LuaObjectMaterial.h"

struct S3DModel;
struct S3DModelPiece;

struct LocalModel
{
	CR_DECLARE_STRUCT(LocalModel)

	LocalModel() {}
	~LocalModel() { pieces.clear(); }


	bool HasPiece(unsigned int i) const { return (i < pieces.size()); }
	bool Initialized() const { return (!pieces.empty()); }

	const LocalModelPiece* GetPiece(unsigned int i)  const { assert(HasPiece(i)); return &pieces[i]; }
	      LocalModelPiece* GetPiece(unsigned int i)        { assert(HasPiece(i)); return &pieces[i]; }

	const LocalModelPiece* GetRoot() const { return (GetPiece(0)); }
	const CollisionVolume* GetBoundingVolume() const { return &boundingVolume; }

	const float3 GetRelMidPos() const { return (boundingVolume.GetOffsets()); }

	// raw forms, the piece-index must be valid
	const float3 GetRawPiecePos(int pieceIdx) const { return pieces[pieceIdx].GetAbsolutePos(); }

	// used by all SolidObject's; accounts for piece movement
	float GetDrawRadius() const { return (boundingVolume.GetBoundingRadius()); }


	// luaMaterialData + per-piece lodDispLists were evicted to the drawer-owned
	// render record (sim/draw PR 10); draw callers thread them in as primitives
	// so LocalModel stays free of the drawer types
	void Draw(const LuaObjectMaterialData* lmd, const std::vector<std::vector<uint32_t>>* lodLists) const {
		if (!lmd->Enabled()) {
			DrawPieces();
			return;
		}

		DrawPiecesLOD(lmd->GetCurrentLOD(), lmd, lodLists);
	}

	void SetModel(const S3DModel* model, bool initialize = true);
	void SetLODCount(unsigned int lodCount, LuaObjectMaterialData* lmd, std::vector<std::vector<uint32_t>>* lodLists) const;
	void UpdateBoundingVolume();

	void GetBoundingBoxVerts(std::vector<float3>& verts) const {
		verts.resize(8 + 2); GetBoundingBoxVerts(&verts[0]);
	}

	void GetBoundingBoxVerts(float3* verts) const {
		const float3 bbMins = GetRelMidPos() - boundingVolume.GetHScales();
		const float3 bbMaxs = GetRelMidPos() + boundingVolume.GetHScales();

		// bottom
		verts[0] = float3(bbMins.x,  bbMins.y,  bbMins.z);
		verts[1] = float3(bbMaxs.x,  bbMins.y,  bbMins.z);
		verts[2] = float3(bbMaxs.x,  bbMins.y,  bbMaxs.z);
		verts[3] = float3(bbMins.x,  bbMins.y,  bbMaxs.z);
		// top
		verts[4] = float3(bbMins.x,  bbMaxs.y,  bbMins.z);
		verts[5] = float3(bbMaxs.x,  bbMaxs.y,  bbMins.z);
		verts[6] = float3(bbMaxs.x,  bbMaxs.y,  bbMaxs.z);
		verts[7] = float3(bbMins.x,  bbMaxs.y,  bbMaxs.z);
		// extrema
		verts[8] = bbMins;
		verts[9] = bbMaxs;
	}

	void SetBoundariesNeedsRecalc()       { needsBoundariesRecalc = true; }
	bool GetBoundariesNeedsRecalc() const { return needsBoundariesRecalc; }

	// sim|draw WS-1: piece-tree capture version, packed {instanceSeed:32 |
	// localCount:32}. The seed is assigned from a process-wide monotonic source
	// in SetModel ONLY (unit/feature creation and creg PostLoad, both single-
	// threaded contexts), so values are unique across LocalModel instances and a
	// died-then-reused id can never alias a predecessor's captured version. The
	// count is a plain per-instance increment at every piece-mutation choke
	// (LocalModelPiece::SetDirty / SetScriptVisible / colvol + no-interpolation
	// chokes, CLuaUnitScript::CreateScript): the anim-tick for_mt partitions by
	// unit script (one script per unit, touching only its own model's pieces)
	// and every other mutator is single-threaded sim code phase-separated from
	// it, so the member is single-writer at any instant -- no atomics. Readers
	// (the epoch producer's piece capture, WS-2's transform extraction) run at
	// the sim frame edge, after the anim for_mt joined. localCount wraps after
	// 2^32 bumps of one instance (~33 days of continuous max-rate animation),
	// the only false-skip mode -- accepted and documented (WS-2 §7 ask 1). The
	// masked increment keeps the seed bits intact across a wrap. The draw-side
	// ResetWasUpdated must never bump (it is not a sim mutation).
	uint64_t GetPieceTreeVersion() const { return pieceTreeVersion; }
	void BumpPieceTreeVersion() {
		pieceTreeVersion = (pieceTreeVersion & PIECE_TREE_SEED_MASK) | ((pieceTreeVersion + 1) & PIECE_TREE_COUNT_MASK);
	}

private:
	static constexpr uint64_t PIECE_TREE_SEED_MASK  = 0xFFFFFFFF00000000ull;
	static constexpr uint64_t PIECE_TREE_COUNT_MASK = 0x00000000FFFFFFFFull;

	void SeedPieceTreeVersion();

	LocalModelPiece* CreateLocalModelPieces(const S3DModelPiece* mpParent);

	void DrawPieces() const;
	void DrawPiecesLOD(unsigned int lod, const LuaObjectMaterialData* lmd, const std::vector<std::vector<uint32_t>>* lodLists) const;

public:
	std::vector<LocalModelPiece> pieces;

private:
	// object-oriented box; accounts for piece movement
	CollisionVolume boundingVolume;

	bool needsBoundariesRecalc = true;

	uint64_t pieceTreeVersion = 0;
};
