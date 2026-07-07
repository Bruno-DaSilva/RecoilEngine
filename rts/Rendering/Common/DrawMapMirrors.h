/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "System/float3.h"
#include "System/type2.h"

/**
 * @brief DrawMapMirrors -- draw-owned, boundary-drained copies of the sim's
 *        map-layer state (PR 28 of the sim|draw decoupling plan)
 *
 * The companion to SimSnapshot (the per-object row copy) for the whole-map
 * layers: the per-allyteam LOS/airLos/radar/sonar/jammer maps, the mutable
 * terrain-type table, the smooth-height mesh, and the original heightmap.
 * Draw-context Lua callouts (the positional-LOS + map-info families) and the
 * info-texture / scattered draw-side point queries read these mirrors instead
 * of dereferencing losHandler / mapInfo / smoothGround / the synced original
 * heightmap, so the draw side never touches sim-owned map state.
 *
 * Mechanism (the dirty-copy pattern the later mirror PRs follow):
 *  - The mirror is the exact draw-side twin of the live layout (same
 *    resolution, same cell type) so the positional query math is the live
 *    formula verbatim, reading the mirror arrays.
 *  - Sim mutations of a mirrored layer are funnelled through a small set of
 *    named choke points (Mark*Dirty below); the grep-audit of every writer to
 *    each choke point is in the PR 28 commit message. A mutation path that
 *    bypasses the choke points is a design defect, not a runtime bug -- the
 *    armed SnapshotDiffGate memcmp pass (SnapshotDiffGate::CheckMapMirrors)
 *    is the deterministic detector.
 *  - DrainAtBarrier() copies the dirty layers into the mirror. It runs inside
 *    CGame::SimDrawBarrier, BEFORE simSnapshot.Update() (a numbered barrier
 *    step), so the mirror, the object rows and the drawer containers all
 *    describe the same completed sim frame. Under the Phase-2 split this runs
 *    in the sim-pause window like the rest of the barrier.
 *
 * Granularity (enumerated deviation from the plan's per-rect recommendation):
 *  the LOS layers use a whole-map dirty flag per (losType, allyTeam) and copy
 *  the whole map when dirty, rather than coalescing per-instance circle bboxes
 *  into a rect queue. This is correctness-identical (the memcmp gate passes
 *  either way -- a full copy of a dirty map trivially equals the live map) and
 *  much simpler; rect-granularity is a later cost optimisation. The metal/
 *  extraction map deviation the plan already sanctions is the same shape. The
 *  terrain-type / smooth-mesh / original-heightmap layers are near-static and
 *  use a version counter (copy when the version moved since the last drain).
 *
 * POV: the LOS maps are per-allyteam exactly like the source, so the serving
 * twins index the requested allyteam's mirror -- one mirror serves every
 * handle POV (player, spectator /specteam) with no re-extraction, matching the
 * SimSnapshot masking policy.
 *
 * Flag-off: the mirror is maintained and drained the same way whether the
 * split flag is on or off (the serving twins run in draw context regardless,
 * and are validated single-threaded by the diff gate) -- it changes no synced
 * state and adds no locks, so flag-off game/rendering output is bit-identical.
 */
class DrawMapMirrors
{
public:
	// keep in sync with ILosType::LosType (mapped explicitly in DrainAtBarrier);
	// a local copy keeps this header free of the heavy LosHandler.h include so
	// the sim-side choke points can include it cheaply
	static constexpr int LOS_MIRROR_TYPE_LOS          = 0;
	static constexpr int LOS_MIRROR_TYPE_AIRLOS       = 1;
	static constexpr int LOS_MIRROR_TYPE_RADAR        = 2;
	static constexpr int LOS_MIRROR_TYPE_SONAR        = 3;
	static constexpr int LOS_MIRROR_TYPE_JAMMER       = 4;
	static constexpr int LOS_MIRROR_TYPE_SEISMIC      = 5;
	static constexpr int LOS_MIRROR_TYPE_SONAR_JAMMER = 6;
	static constexpr int LOS_MIRROR_TYPE_COUNT        = 7;

	// PR 29: blocking-map mirror -- the kind of the first (cell[0]) blocking
	// object mirrored per map square (matches the live GroundBlocked dynamic_cast
	// chain: feature first, then unit; anything else -> NONE, which the placement
	// twins skip exactly as the live code's "neither cast matched" fall-through).
	static constexpr uint8_t BLOCK_KIND_NONE    = 0;
	static constexpr uint8_t BLOCK_KIND_UNIT    = 1;
	static constexpr uint8_t BLOCK_KIND_FEATURE = 2;

	// ---- sim-side choke points (cheap, lock-free, safe before the first
	// drain: they only set flags / bump versions) ----

	/// CLosMap::AddCircle / AddRaycast -- the two writers of a losMap cell
	/// (LosMap.cpp). type is the ILosType::LosType enum value, ally the
	/// losMaps index (== allyTeam).
	void MarkLosDirty(int type, int ally);
	/// LuaSyncedCtrl::SetTerrainTypeData -- the sole runtime writer of
	/// mapInfo->terrainTypes
	void MarkTerrainTypesDirty() { ++terrainTypesVersion; }
	/// PR 38d: LuaSyncedCtrl::SetMapSquareTerrainType -- the sole runtime writer
	/// of readMap's per-square typeMap (the terrain-type index array GetGroundInfo
	/// reads via readMap->GetTypeMapSynced()). The load-time fill is picked up by
	/// the first drain (size-mismatch clause).
	void MarkTypeMapDirty() { ++typeMapVersion; }
	/// PR 38d: LuaMetalMap::SetMetalAmount (Spring.SetMetalAmount) -- the sole
	/// runtime writer of metalMap's distributionMap (the metal-amount array
	/// GetGroundInfo reads via LuaMetalMap::GetMetalAmount). The load-time
	/// metalMap.Init fill is picked up by the first drain (size-mismatch clause).
	void MarkMetalMapDirty() { ++metalMapVersion; }
	/// SmoothHeightMesh::UpdateSmoothMesh / MakeSmoothMesh -- the mesh's own
	/// window updater (the sole writer of its height array via its Set/Add
	/// helpers)
	void MarkSmoothMeshDirty() { ++smoothMeshVersion; }
	/// LuaSyncedCtrl {Set,Level,Revert}OriginalHeightMap -- the Spring.
	/// SetOriginalHeight-class writers of readMap's originalHeightMap
	void MarkOrigHeightDirty() { ++origHeightVersion; }
	/// PR 29: CGroundBlockingObjectMap {Add,Remove}GroundBlockingObject (and
	/// thus Open/CloseBlockingYard, which call Remove+Add) -- the object-move
	/// funnels through which every cell[0] change flows. A dirty mark forces a
	/// whole-map re-walk of the blocking mirror on the next drain (blocking
	/// state churns most frames, so this is the LOS whole-map class, not the
	/// near-static version-gated class).
	void MarkBlockingDirty() { blockingDirty = true; }

	// ---- barrier + lifecycle ----

	/// copy dirty layers from live sim; called from CGame::SimDrawBarrier
	/// before simSnapshot.Update(). No-op cheap when nothing is dirty.
	void DrainAtBarrier();
	/// game teardown: forget everything so the next game re-initialises
	void Clear();

	/// true once DrainAtBarrier ran at least once this game (the mirror holds
	/// real data). Serving twins are additionally gated by the snapshot
	/// generation in LuaSnapshotServe::Route, which only reaches >0 after a
	/// drain, so this is a belt-and-braces guard.
	bool Ready() const { return ready; }

	// ---- positional-LOS queries (mirror the live CLosHandler formulas over
	// the copied maps; see the .cpp for the source lines) ----
	bool PosInLos   (const float3& pos, int allyTeam) const;
	bool PosInAirLos(const float3& pos, int allyTeam) const;
	bool PosInRadar (const float3& pos, int allyTeam) const;
	bool PosInJammer(const float3& pos, int allyTeam) const;

	// ---- map-info queries ----
	float OrigHeight(float x, float z) const;
	float SmoothMeshHeight(float x, float z) const;

	// ---- PR 38d: GetGroundInfo map-info queries ----
	// terrain-type INDEX at a typemap square (the readMap->GetTypeMapSynced()
	// [sqrIndex] read); bounds-safe (0 for an out-of-range / not-yet-drained
	// square). The caller computes sqrIndex with the live ix/iz/hmapx math.
	int TypeMapAt(int sqrIndex) const;
	// metal amount at metal-map square (x, z) -- CMetalMap::GetMetalAmount(x, z)
	// mirror (clamp + distributionMap[z*sizeX+x] * metalScale); 0 when the mirror
	// is empty (matches an unloaded metal map).
	float MetalAmount(int x, int z) const;

	// ---- PR 29: blocking-map query ----
	// cell[0] object id + kind (BLOCK_KIND_*) at map square (x, z); returns id
	// < 0 with kindOut == BLOCK_KIND_NONE for an empty cell or an out-of-range /
	// not-yet-drained square. Mirrors CGroundBlockingObjectMap::GroundBlocked's
	// cell[0] read -- the single object every placement callout inspects per
	// square (GroundBlocked/GroundBlockedUnsafe both return cell[0]).
	int BlockedAt(int x, int z, uint8_t& kindOut) const;

	// terrain-type mirror access (the serving twin reads these; count is the
	// fixed CMapInfo::NUM_TERRAIN_TYPES)
	struct TerrainType {
		std::string name;
		float hardness = 0.0f;
		float tankSpeed = 0.0f;
		float kbotSpeed = 0.0f;
		float hoverSpeed = 0.0f;
		float shipSpeed = 0.0f;
		bool receiveTracks = false;
	};
	int TerrainTypeCount() const { return static_cast<int>(terrainTypes.size()); }
	const TerrainType& TerrainTypeAt(int i) const { return terrainTypes[i]; }

	// radar-error scalars (GetRadarErrorParams); baseRadarErrorSize/Mult are
	// scalars, radarErrorSizes is per-allyteam
	int   NumAllyTeams() const { return numAllyTeams; }
	float BaseRadarErrorSize() const { return baseRadarErrorSize; }
	float BaseRadarErrorMult() const { return baseRadarErrorMult; }
	float AllyTeamRadarErrorSize(int at) const {
		return (at >= 0 && at < static_cast<int>(radarErrorSizes.size())) ? radarErrorSizes[at] : baseRadarErrorSize;
	}

	// ---- diff-gate accessors (SnapshotDiffGate::CheckMapMirrors compares
	// these against the live sim; the mirror is the draw-side authority) ----
	int LosMapCount(int type) const { return (type >= 0 && type < LOS_MIRROR_TYPE_COUNT) ? int(los[type].maps.size()) : 0; }
	const std::vector<uint16_t>* LosMap(int type, int ally) const {
		if (type < 0 || type >= LOS_MIRROR_TYPE_COUNT)
			return nullptr;
		if (ally < 0 || ally >= int(los[type].maps.size()))
			return nullptr;
		return &los[type].maps[ally];
	}
	const std::vector<float>& OrigHeightMap() const { return origHeight; }
	const std::vector<float>& SmoothMeshData() const { return smoothMesh; }

	// PR 38d diff-gate accessors (typemap + metal distribution mirrors;
	// SnapshotDiffGate::CheckMapMirrors memcmps these against the live sim)
	const std::vector<uint8_t>& TypeMapData() const { return typeMap; }
	int   MetalSizeX() const { return metalSizeX; }
	int   MetalSizeZ() const { return metalSizeZ; }
	float MetalScale() const { return metalScale; }
	const std::vector<uint8_t>& MetalDistributionData() const { return metalDistribution; }

	// PR 29: blocking-mirror diff-gate accessors (SnapshotDiffGate::
	// CheckMapMirrors compares these per map square against the live
	// groundBlockingObjectMap cell[0])
	int BlockMapSquares() const { return static_cast<int>(blockId.size()); }
	const std::vector<int32_t>& BlockIds() const { return blockId; }
	const std::vector<uint8_t>& BlockKinds() const { return blockKind; }

private:
	// InSight(type, pos, at): CLosMap::At(ILosType::PosToSquare(pos)) != 0 over
	// the mirror. Bounds-safe (out-of-range allyteam / empty mirror -> false).
	bool InSight(int type, const float3& pos, int allyTeam) const;

private:
	struct LosMirror {
		float invDiv = 0.0f;
		int2 size = {0, 0};
		std::vector<std::vector<uint16_t>> maps; // [allyTeam][size.x*size.y]
	};

	std::array<LosMirror, LOS_MIRROR_TYPE_COUNT> los;
	std::vector<uint8_t> globalLos;              // [numAllyTeams]
	bool separateJammers = false;

	// dirty state for the LOS layers (whole-map flag per type/ally)
	std::array<std::vector<uint8_t>, LOS_MIRROR_TYPE_COUNT> losDirty; // [type][ally]
	bool losAllDirty = true;   // full re-copy requested (first drain, resize, out-of-range mark)

	// map-info mirrors + their version counters
	std::vector<TerrainType> terrainTypes;
	std::vector<float> smoothMesh;               // [smoothMaxX*smoothMaxY]
	std::vector<float> origHeight;               // [(mapx+1)*(mapy+1)]

	int   smoothMaxX = 0;
	int   smoothMaxY = 0;
	float smoothRes = 0.0f;

	uint32_t terrainTypesVersion = 0;
	uint32_t smoothMeshVersion = 0;
	uint32_t origHeightVersion = 0;
	uint32_t terrainTypesDrained = 0xffffffffu;  // != version -> copy on next drain
	uint32_t smoothMeshDrained = 0xffffffffu;
	uint32_t origHeightDrained = 0xffffffffu;

	// PR 38d: GetGroundInfo mirrors. Per-square terrain-type index (readMap's
	// typeMap) + the metal distribution map (metalMap). Both are near-static --
	// only Spring.SetMapSquareTerrainType / Spring.SetMetalAmount mutate them at
	// runtime -- so both use the version-gated whole-copy pattern (copy when the
	// version moved since the last drain, or the mirror size mismatches). The
	// twin reproduces the live GetTypeMapSynced() index read and the
	// CMetalMap::GetMetalAmount clamp+scale formula over these copies.
	std::vector<uint8_t> typeMap;                // [hmapx*hmapy]
	uint32_t typeMapVersion = 0;
	uint32_t typeMapDrained = 0xffffffffu;

	std::vector<uint8_t> metalDistribution;      // [metalSizeX*metalSizeZ]
	int   metalSizeX = 0;
	int   metalSizeZ = 0;
	float metalScale = 0.0f;                      // Init-time constant (maxMetal)
	uint32_t metalMapVersion = 0;
	uint32_t metalMapDrained = 0xffffffffu;

	// radar-error scalars (GetRadarErrorParams)
	int numAllyTeams = 0;
	float baseRadarErrorSize = 0.0f;
	float baseRadarErrorMult = 0.0f;
	std::vector<float> radarErrorSizes;          // [numAllyTeams]

	// PR 29: blocking-map mirror. Per map square (row-major, mapx*mapy), the
	// cell[0] object id (blockId, -1 == empty) and its kind (blockKind,
	// BLOCK_KIND_*). Whole-map dirty flag (blocking churns most frames), full
	// re-walk on dirty via CGroundBlockingObjectMap::GroundBlockedUnsafe.
	std::vector<int32_t> blockId;                // [mapx*mapy], -1 == empty
	std::vector<uint8_t> blockKind;              // [mapx*mapy], BLOCK_KIND_*
	bool blockingDirty = true;

	bool ready = false;
};

extern DrawMapMirrors drawMapMirrors;
