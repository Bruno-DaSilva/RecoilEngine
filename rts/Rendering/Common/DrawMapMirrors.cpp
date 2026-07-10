/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "DrawMapMirrors.h"

#include <algorithm>
#include <cassert>

#include "Map/MapDimensions.h"
#include "Map/MapInfo.h"
#include "Map/MetalMap.h"                  // PR 38d: metal distribution mirror source
#include "Map/ReadMap.h"
#include "Rendering/Common/SimSnapshot.h"  // PR 44a: ring-depth static_assert
#include "Sim/Features/Feature.h"          // PR 29: blocking cell[0] classification
#include "Sim/Misc/BuildingMaskMap.h"      // PLACEMENT REHOST: build-mask mirror source
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/GroundBlockingObjectMap.h" // PR 29: blocking-map source
#include "Sim/Misc/YardmapStatusEffectsMap.h" // PLACEMENT REHOST: yard-status mirror source
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Objects/SolidObject.h"       // PR 29: CSolidObject cell[0]
#include "Sim/Units/Unit.h"                // PR 29: blocking cell[0] classification
#include "System/Log/ILog.h"
#include "System/SpringMath.h"
#include "System/TimeProfiler.h"

// PR 44a: one mirror payload per epoch-ring slot
static_assert(DrawMapMirrors::MIRROR_SLOTS == SimSnapshot::EPOCH_RING_SLOTS, "mirror slots must match the epoch ring depth");

DrawMapMirrors drawMapMirrors;

// ---------------------------------------------------------------------------
// choke point
// ---------------------------------------------------------------------------

void DrawMapMirrors::MarkLosDirty(int type, int ally)
{
	if (type < 0 || type >= LOS_MIRROR_TYPE_COUNT)
		return;

	// before the first drain (or after a resize) the per-ally version array
	// may not yet be sized; request a full re-copy in that case
	if (ally >= 0 && ally < static_cast<int>(losVersions[type].size()))
		++losVersions[type][ally];
	else
		++losFullVersion;
}

// PR 46: bound on the blocking dirty-rect log. Rects accumulate while no
// drain runs (production skipped: draw not consuming, pause); past the cap a
// blockingVersion bump reverts the affected slots to the whole-map walk,
// which is what the log exists to avoid but is always correct.
static constexpr size_t MAX_BLOCKING_RECTS = 1 << 16;

void DrawMapMirrors::MarkBlockingDirty(int x1, int z1, int x2, int z2)
{
	x1 = std::max(x1, 0);
	z1 = std::max(z1, 0);
	x2 = std::min(x2, mapDims.mapx);
	z2 = std::min(z2, mapDims.mapy);

	if (x1 >= x2 || z1 >= z2)
		return;

	// PLACEMENT REHOST: the full-cell mirror (move path) does a whole-map re-walk
	// rather than applying the rect log, so bump its version on every mutation.
	++fullCellVersion;

	if (blockingRects.size() >= MAX_BLOCKING_RECTS) {
		++blockingVersion;
		blockingRectBaseSerial = (blockingRectNextSerial += 1);
		blockingRects.clear();
		return;
	}

	blockingRects.push_back({x1, z1, x2, z2});
	blockingRectNextSerial += 1;
}

// ---------------------------------------------------------------------------
// producer drain (PR 44a: per-slot; the flip's sim frame edge, or the
// lockstep barrier under the park)
// ---------------------------------------------------------------------------

void DrawMapMirrors::DrainAtBarrier(int slot)
{
	// pre-game / teardown: the sim map state does not exist yet
	if (losHandler == nullptr || readMap == nullptr || mapInfo == nullptr)
		return;

	// PR 46: attribution (see LuaSnapshotServe::RefreshCommandQueues)
	SCOPED_TIMER("Sim::EpochProduce::MapMirrors");

	assert(slot >= 0 && slot < MIRROR_SLOTS);
	Payload& pl = payloads[slot];

	const int liveAllyTeams = teamHandler.ActiveAllyTeams();

	// --- LOS layers (whole-map version per type/ally) ---
	{
		// PR 46: sub-attribution -- the LOS copies are the other candidate
		// bulk of this drain's cost besides the (now incremental) blocking
		// walk; scoped to exactly this section
		SCOPED_TIMER("Sim::EpochProduce::MirrorLos");

		// map the ILosType members onto the mirror's fixed enum order
		const ILosType* lts[LOS_MIRROR_TYPE_COUNT];
		lts[LOS_MIRROR_TYPE_LOS]          = &losHandler->los;
		lts[LOS_MIRROR_TYPE_AIRLOS]       = &losHandler->airLos;
		lts[LOS_MIRROR_TYPE_RADAR]        = &losHandler->radar;
		lts[LOS_MIRROR_TYPE_SONAR]        = &losHandler->sonar;
		lts[LOS_MIRROR_TYPE_JAMMER]       = &losHandler->jammer;
		lts[LOS_MIRROR_TYPE_SEISMIC]      = &losHandler->seismic;
		lts[LOS_MIRROR_TYPE_SONAR_JAMMER] = &losHandler->sonarJammer;

		// this slot needs a full LOS re-copy when an unsized/out-of-range mark
		// arrived since it last caught up (pre-first-drain marks, resizes)
		bool slotLosAllDirty = (pl.losFullDrained != losFullVersion);

		// detect a resize (new game / allyteam count change) -> full re-copy
		for (int t = 0; !slotLosAllDirty && t < LOS_MIRROR_TYPE_COUNT; ++t) {
			if (static_cast<int>(pl.los[t].maps.size()) != int(lts[t]->losMaps.size()))
				slotLosAllDirty = true;
		}

		for (int t = 0; t < LOS_MIRROR_TYPE_COUNT; ++t) {
			LosMirror& m = pl.los[t];
			const ILosType* lt = lts[t];
			const int nAlly = static_cast<int>(lt->losMaps.size());

			m.invDiv = lt->invDiv;
			m.size = lt->size;

			if (static_cast<int>(m.maps.size()) != nAlly)
				m.maps.resize(nAlly);
			// producing-thread-owned version array (shared across slots)
			if (static_cast<int>(losVersions[t].size()) != nAlly)
				losVersions[t].assign(nAlly, 1);
			if (static_cast<int>(pl.losDrained[t].size()) != nAlly)
				pl.losDrained[t].assign(nAlly, 0);

			for (int at = 0; at < nAlly; ++at) {
				if (!slotLosAllDirty && pl.losDrained[t][at] == losVersions[t][at])
					continue;

				m.maps[at] = lt->losMaps[at].GetLosMap(); // vector copy
				pl.losDrained[t][at] = losVersions[t][at];
			}
		}
		pl.losFullDrained = losFullVersion;
	}

	// --- global-LOS + jammer config (tiny, unconditional) ---
	pl.numAllyTeams = liveAllyTeams;
	pl.globalLos.resize(pl.numAllyTeams);
	for (int at = 0; at < pl.numAllyTeams; ++at)
		pl.globalLos[at] = losHandler->GetGlobalLOS(at);
	pl.separateJammers = modInfo.separateJammers;

	// --- radar-error scalars (tiny, unconditional) ---
	pl.baseRadarErrorSize = losHandler->GetBaseRadarErrorSize();
	pl.baseRadarErrorMult = losHandler->GetBaseRadarErrorMult();
	pl.radarErrorSizes.resize(pl.numAllyTeams);
	for (int at = 0; at < pl.numAllyTeams; ++at)
		pl.radarErrorSizes[at] = losHandler->GetAllyTeamRadarErrorSize(at);

	// --- terrain-type table (version-gated whole copy; near-static) ---
	if (pl.terrainTypesDrained != terrainTypesVersion || pl.terrainTypes.empty()) {
		pl.terrainTypes.resize(CMapInfo::NUM_TERRAIN_TYPES);
		for (int i = 0; i < CMapInfo::NUM_TERRAIN_TYPES; ++i) {
			const CMapInfo::TerrainType& tt = mapInfo->terrainTypes[i];
			TerrainType& d = pl.terrainTypes[i];
			d.name = tt.name;
			d.hardness = tt.hardness;
			d.tankSpeed = tt.tankSpeed;
			d.kbotSpeed = tt.kbotSpeed;
			d.hoverSpeed = tt.hoverSpeed;
			d.shipSpeed = tt.shipSpeed;
			d.receiveTracks = tt.receiveTracks;
		}
		pl.terrainTypesDrained = terrainTypesVersion;
	}

	// --- typemap (PR 38d): per-square terrain-type index, version-gated ---
	// Near-static: Spring.SetMapSquareTerrainType is the sole runtime writer; the
	// load-time fill is caught by the size-mismatch clause on the first drain.
	{
		const size_t n = static_cast<size_t>(mapDims.hmapx) * static_cast<size_t>(mapDims.hmapy);
		if (pl.typeMapDrained != typeMapVersion || pl.typeMap.size() != n) {
			const uint8_t* src = readMap->GetTypeMapSynced();
			pl.typeMap.assign(src, src + n);
			pl.typeMapDrained = typeMapVersion;
		}
	}

	// --- metal distribution map (PR 38d): version-gated whole copy ---
	// Near-static: Spring.SetMetalAmount is the sole runtime writer. sizeX/Z and
	// metalScale are Init-time constants (captured unconditionally -- cheap
	// scalars); the distributionMap copy is gated on the version / a size change
	// (which also catches the load-time metalMap.Init fill on the first drain).
	{
		pl.metalSizeX = metalMap.GetSizeX();
		pl.metalSizeZ = metalMap.GetSizeZ();
		pl.metalScale = metalMap.GetMetalScale();
		const size_t n = static_cast<size_t>(pl.metalSizeX) * static_cast<size_t>(pl.metalSizeZ);
		if (pl.metalMapDrained != metalMapVersion || pl.metalDistribution.size() != n) {
			const unsigned char* src = metalMap.GetDistributionMap();
			if (n > 0)
				pl.metalDistribution.assign(src, src + n);
			else
				pl.metalDistribution.clear();
			pl.metalMapDrained = metalMapVersion;
		}

		// --- metal extraction map (PR 42): version-gated whole copy ---
		// Churns as extractors mine (RequestExtraction/RemoveExtraction mark it),
		// so this copies most drains; the size clause catches the load-time fill
		if (pl.extractionDrained != extractionVersion || pl.extractionMap.size() != n) {
			const float* src = metalMap.GetExtractionMap();
			if (n > 0)
				pl.extractionMap.assign(src, src + n);
			else
				pl.extractionMap.clear();
			pl.extractionDrained = extractionVersion;
		}
	}

	// --- smooth-height mesh (version-gated whole copy; window updater) ---
	pl.smoothMaxX = smoothGround.GetMaxX();
	pl.smoothMaxY = smoothGround.GetMaxY();
	pl.smoothRes = smoothGround.GetResolution();
	{
		const size_t n = static_cast<size_t>(pl.smoothMaxX) * static_cast<size_t>(pl.smoothMaxY);
		if (pl.smoothMeshDrained != smoothMeshVersion || pl.smoothMesh.size() != n) {
			// GetMeshData() dereferences mesh[0]; only touch it when non-empty
			if (n > 0)
				pl.smoothMesh.assign(smoothGround.GetMeshData(), smoothGround.GetMeshData() + n);
			else
				pl.smoothMesh.clear();
			pl.smoothMeshDrained = smoothMeshVersion;
		}
	}

	// --- original heightmap (version-gated whole copy) ---
	{
		const size_t n = static_cast<size_t>(mapDims.mapxp1) * static_cast<size_t>(mapDims.mapyp1);
		if (pl.origHeightDrained != origHeightVersion || pl.origHeight.size() != n) {
			const float* src = readMap->GetOriginalHeightMapSynced();
			pl.origHeight.assign(src, src + n);
			pl.origHeightDrained = origHeightVersion;
		}
	}

	// --- blocking map (PR 29): per-square cell[0] id + kind ---
	// GroundBlockedUnsafe(sq) returns the same cell[0] object the live
	// placement callouts read; classify it once here so the served twins never
	// dereference a live CSolidObject. PR 46: incremental -- the choke points
	// log each mutation's footprint rect, and a caught-up slot re-scans ONLY
	// the rects appended since its cursor (the whole-map walk was ~all of the
	// measured 6.65ms/epoch drain cost). The whole-map path remains for the
	// slot's first drain, a map-size change, and a version bump (log
	// overflow / Clear); the armed SnapshotDiffGate memcmp pass is the
	// deterministic detector for a rect this misses.
	{
		SCOPED_TIMER("Sim::EpochProduce::MirrorBlocking");

		const auto classifySquare = [&pl](size_t sq) {
			const CSolidObject* s = groundBlockingObjectMap.GroundBlockedUnsafe(static_cast<unsigned int>(sq));

			// live GetGroundBlocked order: feature cast first, then unit;
			// anything else -> NONE (the twin's "neither" fall-through)
			if (s == nullptr) {
				pl.blockId[sq] = -1;
				pl.blockKind[sq] = BLOCK_KIND_NONE;
			} else if (const CFeature* f = dynamic_cast<const CFeature*>(s)) {
				pl.blockId[sq] = f->id;
				pl.blockKind[sq] = BLOCK_KIND_FEATURE;
			} else if (const CUnit* u = dynamic_cast<const CUnit*>(s)) {
				pl.blockId[sq] = u->id;
				pl.blockKind[sq] = BLOCK_KIND_UNIT;
			} else {
				pl.blockId[sq] = -1;
				pl.blockKind[sq] = BLOCK_KIND_NONE;
			}
		};

		const size_t nSquares = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);

		if (pl.blockingDrained != blockingVersion || pl.blockId.size() != nSquares) {
			// full path: this slot cannot trust its arrays (first drain,
			// resize, or an overflow-class version bump)
			pl.blockId.assign(nSquares, -1);
			pl.blockKind.assign(nSquares, BLOCK_KIND_NONE);

			for (size_t sq = 0; sq < nSquares; ++sq)
				classifySquare(sq);

			pl.blockingDrained = blockingVersion;
			pl.blockingRectsDrained = blockingRectNextSerial; // log subsumed by the walk
		} else if (pl.blockingRectsDrained != blockingRectNextSerial) {
			// incremental path: apply the footprint rects logged since this
			// slot's cursor (rects were clamped at the choke point; overlaps
			// are idempotent -- classifySquare reads current live state)
			size_t idx = 0;
			if (pl.blockingRectsDrained > blockingRectBaseSerial)
				idx = static_cast<size_t>(pl.blockingRectsDrained - blockingRectBaseSerial);

			for (; idx < blockingRects.size(); ++idx) {
				const BlockingRect& r = blockingRects[idx];

				for (int z = r.z1; z < r.z2; ++z) {
					size_t sq = static_cast<size_t>(z) * static_cast<size_t>(mapDims.mapx) + static_cast<size_t>(r.x1);

					for (int x = r.x1; x < r.x2; ++x, ++sq)
						classifySquare(sq);
				}
			}

			pl.blockingRectsDrained = blockingRectNextSerial;
		}

		// prune the log prefix every slot has applied (slots that never
		// drained hold cursor 0 and block pruning only until their first
		// drain, which takes the full path above within the first frames)
		uint64_t minCursor = blockingRectNextSerial;
		for (const Payload& q : payloads)
			minCursor = std::min(minCursor, q.blockingRectsDrained);

		while (blockingRectBaseSerial < minCursor && !blockingRects.empty()) {
			blockingRects.pop_front();
			blockingRectBaseSerial += 1;
		}
	}

	// --- PLACEMENT REHOST: full-cell blocking mirror (CSR) ---
	// Every object per square (not just cell[0]) for the move-placement OR-fold.
	// Gated on fullCellVersion (bumped by MarkBlockingDirty on EVERY blocking
	// mutation) -- unlike the PR 46 cell[0] mirror, the full-cell mirror does a
	// whole-map re-walk rather than applying the rect log (correctness over the
	// producer-cost optimisation; the move path is rare).
	{
		const size_t nSquares = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);
		if (pl.fullCellDrained != fullCellVersion || pl.fullCellOffset.size() != nSquares + 1) {
			pl.fullCellOffset.assign(nSquares + 1, 0);
			pl.fullCellId.clear();
			pl.fullCellKind.clear();

			for (size_t sq = 0; sq < nSquares; ++sq) {
				pl.fullCellOffset[sq] = static_cast<int32_t>(pl.fullCellId.size());

				const auto cell = groundBlockingObjectMap.GetCellUnsafeConst(static_cast<unsigned int>(sq));
				const size_t n = cell.size();
				for (size_t i = 0; i < n; ++i) {
					const CSolidObject* s = cell[i];
					if (s == nullptr)
						continue;
					// same feature-first/unit classification as cell[0]; the blocking
					// map only ever holds units/features, so "neither" never occurs
					if (const CFeature* f = dynamic_cast<const CFeature*>(s)) {
						pl.fullCellId.push_back(f->id);
						pl.fullCellKind.push_back(BLOCK_KIND_FEATURE);
					} else if (const CUnit* u = dynamic_cast<const CUnit*>(s)) {
						pl.fullCellId.push_back(u->id);
						pl.fullCellKind.push_back(BLOCK_KIND_UNIT);
					}
				}
			}
			pl.fullCellOffset[nSquares] = static_cast<int32_t>(pl.fullCellId.size());
			pl.fullCellDrained = fullCellVersion;
		}
	}

	// --- PLACEMENT REHOST: height-derived layers (one shared version) ---
	// centerHeightMap / maxHeightMap / centerNormals2D are [mapx*mapy]; slopeMap
	// is [hmapx*hmapy]. All four are recomputed together by UpdateHeightMapSynced
	// (the MarkHeightDirty choke), so one version gate copies the set. Whole copy
	// (the plan sanctions whole-map granularity; terraform is the only churn and
	// the version gate makes a static map a first-drain-only cost).
	{
		const size_t nFull = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);
		const size_t nHalf = static_cast<size_t>(mapDims.hmapx) * static_cast<size_t>(mapDims.hmapy);
		if (pl.heightDrained != heightVersion || pl.centerHeight.size() != nFull || pl.slope.size() != nHalf) {
			const float*  ch = readMap->GetCenterHeightMapSynced();
			const float*  mh = readMap->GetMaxHeightMapSynced();
			const float3* cn = readMap->GetCenterNormals2DSynced();
			const float*  sl = readMap->GetSlopeMapSynced();
			pl.centerHeight.assign(ch, ch + nFull);
			pl.maxHeight.assign(mh, mh + nFull);
			pl.centerNormals2D.assign(cn, cn + nFull);
			pl.slope.assign(sl, sl + nHalf);
			pl.heightDrained = heightVersion;
		}
	}

	// --- PLACEMENT REHOST: building-mask (half-res uint16, all-ones default) ---
	{
		const size_t n = static_cast<size_t>(mapDims.hmapx) * static_cast<size_t>(mapDims.hmapy);
		if (pl.buildMaskDrained != buildMaskVersion || pl.buildMask.size() != n) {
			// guard against a barrier that drains before BuildingMaskMap::Init
			// (leave the mirror empty -> the accessor returns pass, matching the
			// load-time all-ones fill); the size-mismatch clause re-drains once filled
			if (buildingMaskMap.GetNumTiles() == n) {
				pl.buildMask.resize(n);
				for (size_t i = 0; i < n; ++i)
					pl.buildMask[i] = buildingMaskMap.GetTileMaskUnsafe(static_cast<unsigned int>(i));
				pl.buildMaskDrained = buildMaskVersion;
			} else {
				pl.buildMask.clear();
			}
		}
	}

	// --- PLACEMENT REHOST: yard-status (full-res uint8, flat logical (x,z)) ---
	// The live map stores 8x8 tiles; we flatten to z*mapx+x so the mirror accessor
	// is a plain index. Low churn (factory yard open/close); version-gated whole copy.
	{
		const size_t n = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);
		if (pl.yardStatusDrained != yardStatusVersion || pl.yardStatus.size() != n) {
			// guard against a barrier before InitNewYardmapStatusEffectsMap (leave
			// empty -> accessor returns false, matching an all-clear map); the
			// size-mismatch clause re-drains once the map is sized
			if (yardmapStatusEffectsMap.IsInitialized()) {
				pl.yardStatus.resize(n);
				for (int z = 0; z < mapDims.mapy; ++z)
					for (int x = 0; x < mapDims.mapx; ++x)
						pl.yardStatus[static_cast<size_t>(z) * mapDims.mapx + x] = yardmapStatusEffectsMap.GetMapState(x, z);
				pl.yardStatusDrained = yardStatusVersion;
			} else {
				pl.yardStatus.clear();
			}
		}
	}

	pl.ready = true;
	drainSerial += 1; // PR 43 §2.1 (per-slot channel-version scalar)
}

void DrawMapMirrors::Clear()
{
	drainSerial = 0;
	servingSlot = 0;

	for (Payload& pl : payloads)
		pl = Payload{};

	for (auto& v : losVersions)
		v.clear();
	losFullVersion = 1;

	terrainTypesVersion = 0;
	smoothMeshVersion = 0;
	origHeightVersion = 0;
	typeMapVersion = 0;
	metalMapVersion = 0;
	extractionVersion = 1;
	blockingVersion = 1;

	// PR 46: blocking dirty-rect log
	blockingRects.clear();
	blockingRectNextSerial = 1;
	blockingRectBaseSerial = 1;

	// PLACEMENT REHOST
	heightVersion = 1;
	buildMaskVersion = 1;
	yardStatusVersion = 1;
	fullCellVersion = 1;
}

// PR 29: cell[0] id + kind at map square (x, z). Mirrors
// CGroundBlockingObjectMap::GroundBlocked's bounds check + cell[0] read.
int DrawMapMirrors::BlockedAt(int x, int z, uint8_t& kindOut) const
{
	kindOut = BLOCK_KIND_NONE;

	if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(mapDims.mapx) ||
	    static_cast<unsigned int>(z) >= static_cast<unsigned int>(mapDims.mapy))
		return -1;

	const Payload& pl = P();

	const size_t sq = static_cast<size_t>(z) * static_cast<size_t>(mapDims.mapx) + static_cast<size_t>(x);
	if (sq >= pl.blockId.size())
		return -1;

	kindOut = pl.blockKind[sq];
	return pl.blockId[sq];
}

// PLACEMENT REHOST: full-cell mirror queries (CSR over fullCellOffset/Id/Kind).
int DrawMapMirrors::FullCellCount(int x, int z) const
{
	if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(mapDims.mapx) ||
	    static_cast<unsigned int>(z) >= static_cast<unsigned int>(mapDims.mapy))
		return 0;
	const Payload& pl = P();
	const size_t sq = static_cast<size_t>(z) * mapDims.mapx + x;
	if (sq + 1 >= pl.fullCellOffset.size())
		return 0;
	return pl.fullCellOffset[sq + 1] - pl.fullCellOffset[sq];
}

int DrawMapMirrors::FullCellObj(int x, int z, int i, uint8_t& kindOut) const
{
	kindOut = BLOCK_KIND_NONE;
	if (static_cast<unsigned int>(x) >= static_cast<unsigned int>(mapDims.mapx) ||
	    static_cast<unsigned int>(z) >= static_cast<unsigned int>(mapDims.mapy))
		return -1;
	const Payload& pl = P();
	const size_t sq = static_cast<size_t>(z) * mapDims.mapx + x;
	if (sq + 1 >= pl.fullCellOffset.size())
		return -1;
	const int base = pl.fullCellOffset[sq];
	const int cnt = pl.fullCellOffset[sq + 1] - base;
	if (i < 0 || i >= cnt)
		return -1;
	kindOut = pl.fullCellKind[base + i];
	return pl.fullCellId[base + i];
}

// ---------------------------------------------------------------------------
// PLACEMENT REHOST: height-derived + build-mask + yard-status queries. Each
// reproduces the live accessor's formula VERBATIM over the mirrored array.
// ---------------------------------------------------------------------------

// CGround::GetApproximateHeightUnsafe(x, z, synced): centerHeightMap[z*mapx + x],
// no clamp (caller guarantees the square is in range). Defensive 0 out of range.
float DrawMapMirrors::ApproxHeightUnsafe(int sqx, int sqz) const
{
	const Payload& pl = P();
	if (static_cast<unsigned int>(sqx) >= static_cast<unsigned int>(mapDims.mapx) ||
	    static_cast<unsigned int>(sqz) >= static_cast<unsigned int>(mapDims.mapy))
		return 0.0f;
	const size_t idx = static_cast<size_t>(sqz) * mapDims.mapx + sqx;
	return (idx < pl.centerHeight.size()) ? pl.centerHeight[idx] : 0.0f;
}

// CGround::GetSlope(x, z, synced): slopeMap[xh + zh*hmapx] over the half-res map.
float DrawMapMirrors::Slope(float x, float z) const
{
	const Payload& pl = P();
	const int xhsquare = std::clamp(int(x) / (2 * SQUARE_SIZE), 0, mapDims.hmapx - 1);
	const int zhsquare = std::clamp(int(z) / (2 * SQUARE_SIZE), 0, mapDims.hmapy - 1);
	const size_t idx = static_cast<size_t>(zhsquare) * mapDims.hmapx + xhsquare;
	return (idx < pl.slope.size()) ? pl.slope[idx] : 0.0f;
}

// readMap->GetMaxHeightMapSynced()[square]
float DrawMapMirrors::MaxHeightAtSquare(int square) const
{
	const Payload& pl = P();
	return (static_cast<size_t>(square) < pl.maxHeight.size()) ? pl.maxHeight[square] : 0.0f;
}

// readMap->GetCenterNormals2DSynced()[square]
float3 DrawMapMirrors::CenterNormal2DAtSquare(int square) const
{
	const Payload& pl = P();
	return (static_cast<size_t>(square) < pl.centerNormals2D.size()) ? pl.centerNormals2D[square] : UpVector;
}

// BuildingMaskMap::TestTileMaskUnsafe(hx, hz, mask): (maskMap[hx + hz*hmapx] & mask) == mask.
// Empty / out-of-range mirror -> pass (the load-time all-ones fill), so an undrained
// mirror never spuriously blocks placement.
bool DrawMapMirrors::BuildingMaskTest(int hx, int hz, uint16_t mask) const
{
	const Payload& pl = P();
	if (pl.buildMask.empty())
		return true;
	if (static_cast<unsigned int>(hx) >= static_cast<unsigned int>(mapDims.hmapx) ||
	    static_cast<unsigned int>(hz) >= static_cast<unsigned int>(mapDims.hmapy))
		return true;
	const size_t idx = static_cast<size_t>(hz) * mapDims.hmapx + hx;
	if (idx >= pl.buildMask.size())
		return true;
	return (pl.buildMask[idx] & mask) == mask;
}

// YardmapStatusEffectsMap::GetMapState(x, z) clamps to mapxm1/mapym1; reproduce it.
bool DrawMapMirrors::YardStatusAnyFlags(int x, int z, uint8_t flags) const
{
	const Payload& pl = P();
	if (pl.yardStatus.empty())
		return false;
	const int cx = std::clamp(x, 0, mapDims.mapxm1);
	const int cz = std::clamp(z, 0, mapDims.mapym1);
	const size_t idx = static_cast<size_t>(cz) * mapDims.mapx + cx;
	return (idx < pl.yardStatus.size()) && ((pl.yardStatus[idx] & flags) != 0);
}

bool DrawMapMirrors::YardStatusAllFlags(int x, int z, uint8_t flags) const
{
	const Payload& pl = P();
	if (pl.yardStatus.empty())
		return false;
	const int cx = std::clamp(x, 0, mapDims.mapxm1);
	const int cz = std::clamp(z, 0, mapDims.mapym1);
	const size_t idx = static_cast<size_t>(cz) * mapDims.mapx + cx;
	return (idx < pl.yardStatus.size()) && ((pl.yardStatus[idx] & flags) == flags);
}

// ---------------------------------------------------------------------------
// positional-LOS queries -- mirrors of the live CLosHandler formulas
// (LosHandler.cpp / LosMap.h / LosHandler.h) over the copied maps
// ---------------------------------------------------------------------------

// CLosMap::At(ILosType::PosToSquare(pos)) != 0
bool DrawMapMirrors::InSight(int type, const float3& pos, int allyTeam) const
{
	if (type < 0 || type >= LOS_MIRROR_TYPE_COUNT)
		return false;

	const LosMirror& m = P().los[type];
	if (allyTeam < 0 || allyTeam >= static_cast<int>(m.maps.size()))
		return false;
	if (m.size.x <= 0 || m.size.y <= 0)
		return false;

	// ILosType::PosToSquare
	int2 p(int(pos.x * m.invDiv), int(pos.z * m.invDiv));
	// CLosMap::At clamp
	p.x = std::clamp(p.x, 0, m.size.x - 1);
	p.y = std::clamp(p.y, 0, m.size.y - 1);

	return (m.maps[allyTeam][p.y * m.size.x + p.x] != 0);
}

// CLosHandler::InLos(const float3, allyTeam)
bool DrawMapMirrors::PosInLos(const float3& pos, int allyTeam) const
{
	const Payload& pl = P();
	if (allyTeam >= 0 && allyTeam < static_cast<int>(pl.globalLos.size()) && pl.globalLos[allyTeam])
		return true;
	return InSight(LOS_MIRROR_TYPE_LOS, pos, allyTeam);
}

// CLosHandler::InAirLos(const float3, allyTeam)
bool DrawMapMirrors::PosInAirLos(const float3& pos, int allyTeam) const
{
	const Payload& pl = P();
	if (allyTeam >= 0 && allyTeam < static_cast<int>(pl.globalLos.size()) && pl.globalLos[allyTeam])
		return true;
	return InSight(LOS_MIRROR_TYPE_AIRLOS, pos, allyTeam);
}

// CLosHandler::InRadar(const float3, allyTeam) -- NB no globalLOS shortcut,
// matching the live body
bool DrawMapMirrors::PosInRadar(const float3& pos, int allyTeam) const
{
	const bool sepJam = P().separateJammers;

	if (pos.y < 0.0f)
		return (InSight(LOS_MIRROR_TYPE_SONAR, pos, allyTeam) &&
			!(!sepJam && InSight(LOS_MIRROR_TYPE_SONAR_JAMMER, pos, 0)));

	return (InSight(LOS_MIRROR_TYPE_RADAR, pos, allyTeam) &&
		!(!sepJam && InSight(LOS_MIRROR_TYPE_JAMMER, pos, 0)));
}

// CLosHandler::InJammer(const float3, allyTeam)
bool DrawMapMirrors::PosInJammer(const float3& pos, int allyTeam) const
{
	const int jammerAlly = P().separateJammers ? allyTeam : 0;

	if (pos.y < 0.0f)
		return InSight(LOS_MIRROR_TYPE_SONAR_JAMMER, pos, jammerAlly);

	return InSight(LOS_MIRROR_TYPE_JAMMER, pos, jammerAlly);
}

// ---------------------------------------------------------------------------
// map-info queries -- mirrors of the live interpolation math
// ---------------------------------------------------------------------------

// InterpolateCornerHeight(x, z, originalHeightMap) -- CGround::GetOrigHeight
// (Map/Ground.cpp); the triangle interpolation is reproduced bit-for-bit
float DrawMapMirrors::OrigHeight(float x, float z) const
{
	const Payload& pl = P();

	if (pl.origHeight.empty())
		return 0.0f;

	const float* cornerHeightMap = pl.origHeight.data();

	x = std::clamp(x, 0.0f, float3::maxxpos) / SQUARE_SIZE;
	z = std::clamp(z, 0.0f, float3::maxzpos) / SQUARE_SIZE;

	const int ix = x;
	const int iz = z;
	const int hs = ix + iz * mapDims.mapxp1;

	const float dx = x - ix;
	const float dz = z - iz;

	float h = 0.0f;

	if (dx + dz < 1.0f) {
		const float h00 = cornerHeightMap[hs + 0                 ];
		const float h10 = cornerHeightMap[hs + 1                 ];
		const float h01 = cornerHeightMap[hs + 0 + mapDims.mapxp1];

		const float xdif = dx * (h10 - h00);
		const float zdif = dz * (h01 - h00);

		h = h00 + xdif + zdif;
	} else {
		const float h10 = cornerHeightMap[hs + 1                 ];
		const float h01 = cornerHeightMap[hs + 0 + mapDims.mapxp1];
		const float h11 = cornerHeightMap[hs + 1 + mapDims.mapxp1];

		const float xdif = (1.0f - dx) * (h01 - h11);
		const float zdif = (1.0f - dz) * (h10 - h11);

		h = h11 + xdif + zdif;
	}

	return h;
}

// Interpolate(x, y, maxx, maxy, res, mesh) -- SmoothHeightMesh::GetHeight
// (Sim/Misc/SmoothHeightMesh.cpp), reproduced bit-for-bit over the mirror
float DrawMapMirrors::SmoothMeshHeight(float x, float y) const
{
	const Payload& pl = P();

	if (pl.smoothMesh.empty() || pl.smoothMaxX <= 0 || pl.smoothMaxY <= 0 || pl.smoothRes <= 0.0f)
		return 0.0f;

	const float* heightmap = pl.smoothMesh.data();
	const int maxx = pl.smoothMaxX;
	const int maxy = pl.smoothMaxY;

	x = std::clamp(x / pl.smoothRes, 0.0f, (float)maxx);
	y = std::clamp(y / pl.smoothRes, 0.0f, (float)maxy);
	const int sx = std::min((int)x, maxx - 1);
	const int sy = std::min((int)y, maxy - 1);
	const float dx = (x - sx);
	const float dy = (y - sy);

	const int sxp1 = std::min(sx + 1, maxx - 1);
	const int syp1 = std::min(sy + 1, maxy - 1);

	const float& h1 = heightmap[sx   + sy   * maxx];
	const float& h2 = heightmap[sxp1 + sy   * maxx];
	const float& h3 = heightmap[sx   + syp1 * maxx];
	const float& h4 = heightmap[sxp1 + syp1 * maxx];

	const float hi1 = mix(h1, h2, dx);
	const float hi2 = mix(h3, h4, dx);
	return mix(hi1, hi2, dy);
}

// ---------------------------------------------------------------------------
// PR 38d: GetGroundInfo mirrors (typemap index + metal amount)
// ---------------------------------------------------------------------------

// terrain-type index at a typemap square (mirror of readMap->GetTypeMapSynced()
// [sqrIndex]); bounds-safe. The caller pre-clamps sqrIndex with the live
// ix/iz/hmapx math, so an out-of-range value only happens pre-drain.
int DrawMapMirrors::TypeMapAt(int sqrIndex) const
{
	const Payload& pl = P();

	if (sqrIndex < 0 || sqrIndex >= static_cast<int>(pl.typeMap.size()))
		return 0;

	return pl.typeMap[sqrIndex];
}

// CMetalMap::GetMetalAmount(x, z) mirror -- clamp to [0,size-1] then
// distributionMap[z*sizeX+x] * metalScale over the copied distribution map.
// Empty mirror -> 0 (pre-drain / unloaded metal map).
float DrawMapMirrors::MetalAmount(int x, int z) const
{
	const Payload& pl = P();

	if (pl.metalSizeX <= 0 || pl.metalSizeZ <= 0 || pl.metalDistribution.empty())
		return 0.0f;

	x = std::clamp(x, 0, pl.metalSizeX - 1);
	z = std::clamp(z, 0, pl.metalSizeZ - 1);

	return pl.metalDistribution[(z * pl.metalSizeX) + x] * pl.metalScale;
}
