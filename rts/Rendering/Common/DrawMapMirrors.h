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
 *  - DrainAtBarrier(slot) copies the dirty layers into ring slot `slot`.
 *
 * PR 44a (producer flip): the single draw-owned mirror became TRUE PER-SLOT
 * COPIES keyed by the SimSnapshot epoch-ring slot (dissolving the PR-43
 * lockstep deviation where one physical mirror tracked the newest epoch):
 *  - the PRODUCER (sim thread at its frame edge under the flip; the barrier
 *    under the park pre-flip/flag-off) drains dirty layers into the slot of
 *    the epoch being produced;
 *  - the CONSUMER points the serving side at the acquired epoch's slot
 *    (SetServingSlot, called at the barrier's acquire) -- every query below
 *    reads the SERVING slot, so draw-side readers (Lua twins, info textures)
 *    see one immutable epoch's layers for the whole frame while the producer
 *    fills another slot.
 *  - dirtiness became per-(layer, slot): the choke points bump monotonic
 *    per-layer VERSIONS (whole-map granularity, as before) and each slot
 *    records the version it last copied -- so a slot re-copies exactly the
 *    layers that changed since IT was last produced. The choke points and the
 *    drain run on the producing thread (sim under the flip); no locks.
 *
 * Granularity (enumerated deviation from the plan's per-rect recommendation):
 *  the LOS layers use a whole-map version per (losType, allyTeam) and copy
 *  the whole map when moved, rather than coalescing per-instance circle
 *  bboxes into a rect queue. This is correctness-identical (the memcmp gate
 *  passes either way) and much simpler; rect-granularity is a later cost
 *  optimisation. The metal/extraction map deviation the plan already
 *  sanctions is the same shape. Extraction/blocking churn most frames, the
 *  terrain-type / typemap / metal / smooth-mesh / orig-heightmap layers are
 *  near-static -- all now share the one version-gated mechanism.
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
	// keep in sync with SimSnapshot::EPOCH_RING_SLOTS (static_assert in the
	// .cpp); a local constant keeps this header light for the sim-side choke
	// points (LosMap.cpp et al)
	static constexpr int MIRROR_SLOTS = 3;

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
	// drain: they only bump monotonic layer versions) ----

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
	/// PR 42 (sim|draw): CMetalMap::RequestExtraction / RemoveExtraction -- the
	/// two runtime writers of metalMap's extractionMap (the per-square extraction
	/// depth the MetalExtraction info-texture reads via GetExtractionMap()).
	/// Extraction churns most frames while extractors mine.
	void MarkExtractionMapDirty() { ++extractionVersion; }
	/// SmoothHeightMesh::UpdateSmoothMesh / MakeSmoothMesh -- the mesh's own
	/// window updater (the sole writer of its height array via its Set/Add
	/// helpers)
	void MarkSmoothMeshDirty() { ++smoothMeshVersion; }
	/// LuaSyncedCtrl {Set,Level,Revert}OriginalHeightMap -- the Spring.
	/// SetOriginalHeight-class writers of readMap's originalHeightMap
	void MarkOrigHeightDirty() { ++origHeightVersion; }
	/// PR 29: CGroundBlockingObjectMap {Add,Remove}GroundBlockingObject (and
	/// thus Open/CloseBlockingYard, which call Remove+Add) -- the object-move
	/// funnels through which every cell[0] change flows. Blocking state churns
	/// most frames.
	void MarkBlockingDirty() { ++blockingVersion; }

	// ---- producer + consumer + lifecycle ----

	/// copy layers whose version moved since slot `slot` was last produced
	/// (whole-layer granularity). Producer side: the flip's sim frame edge, or
	/// the lockstep barrier under the park. Cheap when nothing moved.
	void DrainAtBarrier(int slot);

	/// PR 44a consumer half: point every query below at the acquired epoch's
	/// slot; called from the barrier right after SimSnapshot's acquire
	void SetServingSlot(int slot) { servingSlot = slot; }
	int ServingSlot() const { return servingSlot; }

	/// PR 43 §2.1: monotonic count of completed DrainAtBarrier() calls -- the
	/// "mirrors' drained-version" scalar the epoch ring records per slot
	uint32_t DrainSerial() const { return drainSerial; }
	/// game teardown: forget everything so the next game re-initialises
	void Clear();

	/// true once DrainAtBarrier ran at least once this game for the SERVING
	/// slot (it holds real data). Serving twins are additionally gated by the
	/// snapshot generation in LuaSnapshotServe::Route, which only reaches >0
	/// after a drain, so this is a belt-and-braces guard.
	bool Ready() const { return P().ready; }

	// ---- positional-LOS queries (mirror the live CLosHandler formulas over
	// the copied maps; see the .cpp for the source lines) ----
	bool PosInLos   (const float3& pos, int allyTeam) const;
	bool PosInAirLos(const float3& pos, int allyTeam) const;
	bool PosInRadar (const float3& pos, int allyTeam) const;
	bool PosInJammer(const float3& pos, int allyTeam) const;

	// PR 42: per-allyteam global-LOS flag (losHandler->GetGlobalLOS mirror). The
	// data is already drained unconditionally every barrier (globalLos below);
	// the info-texture uploads (Los/AirLos/Radar) need this getter to stop
	// dereferencing losHandler when the sim thread runs live.
	bool GlobalLos(int ally) const {
		const Payload& pl = P();
		return (ally >= 0 && ally < static_cast<int>(pl.globalLos.size())) && pl.globalLos[ally] != 0;
	}

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
	int TerrainTypeCount() const { return static_cast<int>(P().terrainTypes.size()); }
	const TerrainType& TerrainTypeAt(int i) const { return P().terrainTypes[i]; }

	// radar-error scalars (GetRadarErrorParams); baseRadarErrorSize/Mult are
	// scalars, radarErrorSizes is per-allyteam
	int   NumAllyTeams() const { return P().numAllyTeams; }
	float BaseRadarErrorSize() const { return P().baseRadarErrorSize; }
	float BaseRadarErrorMult() const { return P().baseRadarErrorMult; }
	float AllyTeamRadarErrorSize(int at) const {
		const Payload& pl = P();
		return (at >= 0 && at < static_cast<int>(pl.radarErrorSizes.size())) ? pl.radarErrorSizes[at] : pl.baseRadarErrorSize;
	}

	// ---- diff-gate accessors (SnapshotDiffGate::CheckMapMirrors compares
	// these against the live sim; the mirror is the draw-side authority) ----
	int LosMapCount(int type) const { return (type >= 0 && type < LOS_MIRROR_TYPE_COUNT) ? int(P().los[type].maps.size()) : 0; }
	const std::vector<uint16_t>* LosMap(int type, int ally) const {
		if (type < 0 || type >= LOS_MIRROR_TYPE_COUNT)
			return nullptr;
		const Payload& pl = P();
		if (ally < 0 || ally >= int(pl.los[type].maps.size()))
			return nullptr;
		return &pl.los[type].maps[ally];
	}
	const std::vector<float>& OrigHeightMap() const { return P().origHeight; }
	const std::vector<float>& SmoothMeshData() const { return P().smoothMesh; }

	// PR 38d diff-gate accessors (typemap + metal distribution mirrors;
	// SnapshotDiffGate::CheckMapMirrors memcmps these against the live sim)
	const std::vector<uint8_t>& TypeMapData() const { return P().typeMap; }
	int   MetalSizeX() const { return P().metalSizeX; }
	int   MetalSizeZ() const { return P().metalSizeZ; }
	float MetalScale() const { return P().metalScale; }
	const std::vector<uint8_t>& MetalDistributionData() const { return P().metalDistribution; }

	// PR 42: metal EXTRACTION-map mirror (float per metal square, same dims as
	// the distribution mirror). ExtractionMapData() feeds the MetalExtraction
	// info-texture upload (const float* -> GL_R32F); ExtractionMapVec() is the
	// SnapshotDiffGate::CheckMapMirrors float-memcmp source.
	const float* ExtractionMapData() const { return P().extractionMap.data(); }
	const std::vector<float>& ExtractionMapVec() const { return P().extractionMap; }

	// PR 29: blocking-mirror diff-gate accessors (SnapshotDiffGate::
	// CheckMapMirrors compares these per map square against the live
	// groundBlockingObjectMap cell[0])
	int BlockMapSquares() const { return static_cast<int>(P().blockId.size()); }
	const std::vector<int32_t>& BlockIds() const { return P().blockId; }
	const std::vector<uint8_t>& BlockKinds() const { return P().blockKind; }

private:
	struct LosMirror {
		float invDiv = 0.0f;
		int2 size = {0, 0};
		std::vector<std::vector<uint16_t>> maps; // [allyTeam][size.x*size.y]
	};

	// PR 44a: one complete mirror payload per epoch-ring slot, plus the
	// per-layer versions this slot last copied (the "drained" counters)
	struct Payload {
		std::array<LosMirror, LOS_MIRROR_TYPE_COUNT> los;
		std::array<std::vector<uint32_t>, LOS_MIRROR_TYPE_COUNT> losDrained; // [type][ally]
		uint32_t losFullDrained = 0;                 // != losFullVersion -> full LOS re-copy
		std::vector<uint8_t> globalLos;              // [numAllyTeams]
		bool separateJammers = false;

		std::vector<TerrainType> terrainTypes;
		std::vector<float> smoothMesh;               // [smoothMaxX*smoothMaxY]
		std::vector<float> origHeight;               // [(mapx+1)*(mapy+1)]

		int   smoothMaxX = 0;
		int   smoothMaxY = 0;
		float smoothRes = 0.0f;

		uint32_t terrainTypesDrained = 0xffffffffu;  // != version -> copy on next drain
		uint32_t smoothMeshDrained = 0xffffffffu;
		uint32_t origHeightDrained = 0xffffffffu;

		std::vector<uint8_t> typeMap;                // [hmapx*hmapy]
		uint32_t typeMapDrained = 0xffffffffu;

		std::vector<uint8_t> metalDistribution;      // [metalSizeX*metalSizeZ]
		int   metalSizeX = 0;
		int   metalSizeZ = 0;
		float metalScale = 0.0f;                     // Init-time constant (maxMetal)
		uint32_t metalMapDrained = 0xffffffffu;

		std::vector<float> extractionMap;            // [metalSizeX*metalSizeZ]
		uint32_t extractionDrained = 0xffffffffu;

		// radar-error scalars (GetRadarErrorParams)
		int numAllyTeams = 0;
		float baseRadarErrorSize = 0.0f;
		float baseRadarErrorMult = 0.0f;
		std::vector<float> radarErrorSizes;          // [numAllyTeams]

		// PR 29: blocking-map mirror. Per map square (row-major, mapx*mapy),
		// the cell[0] object id (blockId, -1 == empty) and its kind
		std::vector<int32_t> blockId;                // [mapx*mapy], -1 == empty
		std::vector<uint8_t> blockKind;              // [mapx*mapy], BLOCK_KIND_*
		uint32_t blockingDrained = 0xffffffffu;

		bool ready = false;
	};

	const Payload& P() const { return payloads[servingSlot]; }

	// InSight(type, pos, at): CLosMap::At(ILosType::PosToSquare(pos)) != 0 over
	// the mirror. Bounds-safe (out-of-range allyteam / empty mirror -> false).
	bool InSight(int type, const float3& pos, int allyTeam) const;

private:
	Payload payloads[MIRROR_SLOTS];
	// the slot the draw side reads (== SimSnapshot's held slot; set at acquire)
	int servingSlot = 0;

	// PR 43 §2.1: see DrainSerial()
	uint32_t drainSerial = 0;

	// ---- global (producing-thread-owned) layer versions; a slot copies a
	// layer when its drained counter differs ----
	std::array<std::vector<uint32_t>, LOS_MIRROR_TYPE_COUNT> losVersions; // [type][ally]
	// bumped when an out-of-range/unsized LOS mark arrives (pre-first-drain);
	// forces a full LOS re-copy for slots that have not caught up
	uint32_t losFullVersion = 1;

	uint32_t terrainTypesVersion = 0;
	uint32_t smoothMeshVersion = 0;
	uint32_t origHeightVersion = 0;
	uint32_t typeMapVersion = 0;
	uint32_t metalMapVersion = 0;
	uint32_t extractionVersion = 1;
	uint32_t blockingVersion = 1;
};

extern DrawMapMirrors drawMapMirrors;
