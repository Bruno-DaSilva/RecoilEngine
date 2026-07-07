/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "DrawMapMirrors.h"

#include <algorithm>

#include "Map/MapDimensions.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/TeamHandler.h"
#include "System/Log/ILog.h"
#include "System/SpringMath.h"

DrawMapMirrors drawMapMirrors;

// ---------------------------------------------------------------------------
// choke point
// ---------------------------------------------------------------------------

void DrawMapMirrors::MarkLosDirty(int type, int ally)
{
	if (type < 0 || type >= LOS_MIRROR_TYPE_COUNT)
		return;

	// before the first drain (or after a resize) the per-slot dirty array may
	// not yet be sized; request a full re-copy in that case
	if (ally >= 0 && ally < static_cast<int>(losDirty[type].size()))
		losDirty[type][ally] = 1;
	else
		losAllDirty = true;
}

// ---------------------------------------------------------------------------
// barrier drain
// ---------------------------------------------------------------------------

void DrawMapMirrors::DrainAtBarrier()
{
	// pre-game / teardown: the sim map state does not exist yet
	if (losHandler == nullptr || readMap == nullptr || mapInfo == nullptr)
		return;

	// --- LOS layers (whole-map dirty flag per type/ally) ---
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

	// detect a resize (new game / allyteam count change) -> full re-copy
	for (int t = 0; !losAllDirty && t < LOS_MIRROR_TYPE_COUNT; ++t) {
		if (static_cast<int>(los[t].maps.size()) != int(lts[t]->losMaps.size()))
			losAllDirty = true;
	}

	for (int t = 0; t < LOS_MIRROR_TYPE_COUNT; ++t) {
		LosMirror& m = los[t];
		const ILosType* lt = lts[t];
		const int nAlly = static_cast<int>(lt->losMaps.size());

		m.invDiv = lt->invDiv;
		m.size = lt->size;

		if (static_cast<int>(m.maps.size()) != nAlly)
			m.maps.resize(nAlly);
		if (static_cast<int>(losDirty[t].size()) != nAlly)
			losDirty[t].assign(nAlly, 1);

		for (int at = 0; at < nAlly; ++at) {
			if (!losAllDirty && losDirty[t][at] == 0)
				continue;

			m.maps[at] = lt->losMaps[at].GetLosMap(); // vector copy
			losDirty[t][at] = 0;
		}
	}
	losAllDirty = false;

	// --- global-LOS + jammer config (tiny, unconditional) ---
	numAllyTeams = liveAllyTeams;
	globalLos.resize(numAllyTeams);
	for (int at = 0; at < numAllyTeams; ++at)
		globalLos[at] = losHandler->GetGlobalLOS(at);
	separateJammers = modInfo.separateJammers;

	// --- radar-error scalars (tiny, unconditional) ---
	baseRadarErrorSize = losHandler->GetBaseRadarErrorSize();
	baseRadarErrorMult = losHandler->GetBaseRadarErrorMult();
	radarErrorSizes.resize(numAllyTeams);
	for (int at = 0; at < numAllyTeams; ++at)
		radarErrorSizes[at] = losHandler->GetAllyTeamRadarErrorSize(at);

	// --- terrain-type table (version-gated whole copy; near-static) ---
	if (terrainTypesDrained != terrainTypesVersion || terrainTypes.empty()) {
		terrainTypes.resize(CMapInfo::NUM_TERRAIN_TYPES);
		for (int i = 0; i < CMapInfo::NUM_TERRAIN_TYPES; ++i) {
			const CMapInfo::TerrainType& tt = mapInfo->terrainTypes[i];
			TerrainType& d = terrainTypes[i];
			d.name = tt.name;
			d.hardness = tt.hardness;
			d.tankSpeed = tt.tankSpeed;
			d.kbotSpeed = tt.kbotSpeed;
			d.hoverSpeed = tt.hoverSpeed;
			d.shipSpeed = tt.shipSpeed;
			d.receiveTracks = tt.receiveTracks;
		}
		terrainTypesDrained = terrainTypesVersion;
	}

	// --- smooth-height mesh (version-gated whole copy; window updater) ---
	smoothMaxX = smoothGround.GetMaxX();
	smoothMaxY = smoothGround.GetMaxY();
	smoothRes = smoothGround.GetResolution();
	{
		const size_t n = static_cast<size_t>(smoothMaxX) * static_cast<size_t>(smoothMaxY);
		if (smoothMeshDrained != smoothMeshVersion || smoothMesh.size() != n) {
			// GetMeshData() dereferences mesh[0]; only touch it when non-empty
			if (n > 0)
				smoothMesh.assign(smoothGround.GetMeshData(), smoothGround.GetMeshData() + n);
			else
				smoothMesh.clear();
			smoothMeshDrained = smoothMeshVersion;
		}
	}

	// --- original heightmap (version-gated whole copy) ---
	{
		const size_t n = static_cast<size_t>(mapDims.mapxp1) * static_cast<size_t>(mapDims.mapyp1);
		if (origHeightDrained != origHeightVersion || origHeight.size() != n) {
			const float* src = readMap->GetOriginalHeightMapSynced();
			origHeight.assign(src, src + n);
			origHeightDrained = origHeightVersion;
		}
	}

	ready = true;
}

void DrawMapMirrors::Clear()
{
	for (LosMirror& m : los) {
		m.maps.clear();
		m.invDiv = 0.0f;
		m.size = {0, 0};
	}
	for (auto& d : losDirty)
		d.clear();
	losAllDirty = true;

	globalLos.clear();
	separateJammers = false;

	terrainTypes.clear();
	smoothMesh.clear();
	origHeight.clear();
	smoothMaxX = smoothMaxY = 0;
	smoothRes = 0.0f;

	// force a re-copy of the version-gated layers on the next game's first drain
	terrainTypesDrained = 0xffffffffu;
	smoothMeshDrained = 0xffffffffu;
	origHeightDrained = 0xffffffffu;

	numAllyTeams = 0;
	baseRadarErrorSize = baseRadarErrorMult = 0.0f;
	radarErrorSizes.clear();

	ready = false;
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

	const LosMirror& m = los[type];
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
	if (allyTeam >= 0 && allyTeam < static_cast<int>(globalLos.size()) && globalLos[allyTeam])
		return true;
	return InSight(LOS_MIRROR_TYPE_LOS, pos, allyTeam);
}

// CLosHandler::InAirLos(const float3, allyTeam)
bool DrawMapMirrors::PosInAirLos(const float3& pos, int allyTeam) const
{
	if (allyTeam >= 0 && allyTeam < static_cast<int>(globalLos.size()) && globalLos[allyTeam])
		return true;
	return InSight(LOS_MIRROR_TYPE_AIRLOS, pos, allyTeam);
}

// CLosHandler::InRadar(const float3, allyTeam) -- NB no globalLOS shortcut,
// matching the live body
bool DrawMapMirrors::PosInRadar(const float3& pos, int allyTeam) const
{
	if (pos.y < 0.0f)
		return (InSight(LOS_MIRROR_TYPE_SONAR, pos, allyTeam) &&
			!(!separateJammers && InSight(LOS_MIRROR_TYPE_SONAR_JAMMER, pos, 0)));

	return (InSight(LOS_MIRROR_TYPE_RADAR, pos, allyTeam) &&
		!(!separateJammers && InSight(LOS_MIRROR_TYPE_JAMMER, pos, 0)));
}

// CLosHandler::InJammer(const float3, allyTeam)
bool DrawMapMirrors::PosInJammer(const float3& pos, int allyTeam) const
{
	const int jammerAlly = separateJammers ? allyTeam : 0;

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
	if (origHeight.empty())
		return 0.0f;

	const float* cornerHeightMap = origHeight.data();

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
	if (smoothMesh.empty() || smoothMaxX <= 0 || smoothMaxY <= 0 || smoothRes <= 0.0f)
		return 0.0f;

	const float* heightmap = smoothMesh.data();
	const int maxx = smoothMaxX;
	const int maxy = smoothMaxY;

	x = std::clamp(x / smoothRes, 0.0f, (float)maxx);
	y = std::clamp(y / smoothRes, 0.0f, (float)maxy);
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
