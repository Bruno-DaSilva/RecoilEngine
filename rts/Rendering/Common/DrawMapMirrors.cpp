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
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/GroundBlockingObjectMap.h" // PR 29: blocking-map source
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Objects/SolidObject.h"       // PR 29: CSolidObject cell[0]
#include "Sim/Units/Unit.h"                // PR 29: blocking cell[0] classification
#include "System/Log/ILog.h"
#include "System/SpringMath.h"

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

// ---------------------------------------------------------------------------
// producer drain (PR 44a: per-slot; the flip's sim frame edge, or the
// lockstep barrier under the park)
// ---------------------------------------------------------------------------

void DrawMapMirrors::DrainAtBarrier(int slot)
{
	// pre-game / teardown: the sim map state does not exist yet
	if (losHandler == nullptr || readMap == nullptr || mapInfo == nullptr)
		return;

	assert(slot >= 0 && slot < MIRROR_SLOTS);
	Payload& pl = payloads[slot];

	// --- LOS layers (whole-map version per type/ally) ---
	// map the ILosType members onto the mirror's fixed enum order
	const ILosType* lts[LOS_MIRROR_TYPE_COUNT];
	lts[LOS_MIRROR_TYPE_LOS]          = &losHandler->los;
	lts[LOS_MIRROR_TYPE_AIRLOS]       = &losHandler->airLos;
	lts[LOS_MIRROR_TYPE_RADAR]        = &losHandler->radar;
	lts[LOS_MIRROR_TYPE_SONAR]        = &losHandler->sonar;
	lts[LOS_MIRROR_TYPE_JAMMER]       = &losHandler->jammer;
	lts[LOS_MIRROR_TYPE_SEISMIC]      = &losHandler->seismic;
	lts[LOS_MIRROR_TYPE_SONAR_JAMMER] = &losHandler->sonarJammer;

	const int liveAllyTeams = teamHandler.ActiveAllyTeams();

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
	// Whole-map re-walk when a blocking mutation moved the version since this
	// slot last copied (or on the slot's first drain / a map-size change).
	// GroundBlockedUnsafe(sq) returns the same cell[0] object the live
	// placement callouts read; classify it once here so the served twins never
	// dereference a live CSolidObject.
	{
		const size_t nSquares = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);
		if (pl.blockingDrained != blockingVersion || pl.blockId.size() != nSquares) {
			pl.blockId.assign(nSquares, -1);
			pl.blockKind.assign(nSquares, BLOCK_KIND_NONE);

			for (size_t sq = 0; sq < nSquares; ++sq) {
				const CSolidObject* s = groundBlockingObjectMap.GroundBlockedUnsafe(static_cast<unsigned int>(sq));
				if (s == nullptr)
					continue;

				// live GetGroundBlocked order: feature cast first, then unit;
				// anything else stays NONE (the twin's "neither" fall-through)
				if (const CFeature* f = dynamic_cast<const CFeature*>(s)) {
					pl.blockId[sq] = f->id;
					pl.blockKind[sq] = BLOCK_KIND_FEATURE;
				} else if (const CUnit* u = dynamic_cast<const CUnit*>(s)) {
					pl.blockId[sq] = u->id;
					pl.blockKind[sq] = BLOCK_KIND_UNIT;
				}
			}

			pl.blockingDrained = blockingVersion;
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
