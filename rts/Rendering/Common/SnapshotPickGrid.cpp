/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotPickGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "SimSnapshot.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/QuadField.h"

SnapshotPickGrid snapshotPickGrid;

int SnapshotPickGrid::CellX(float wx) const
{
	return std::clamp(static_cast<int>(std::floor(wx / cellWorld)), 0, numX - 1);
}

int SnapshotPickGrid::CellZ(float wz) const
{
	return std::clamp(static_cast<int>(std::floor(wz / cellWorld)), 0, numZ - 1);
}

void SnapshotPickGrid::Clear()
{
	built = false;
	builtGeneration = 0;
	for (auto& c : unitCells) c.clear();
	for (auto& c : featureCells) c.clear();
}

void SnapshotPickGrid::EnsureCurrent()
{
	const uint64_t gen = simSnapshot.HeldEpochId(); // PR 43 §2.5 / PR 44a: held-epoch key (EpochId advances mid-frame under the flip)
	if (built && gen == builtGeneration)
		return;

	Rebuild();
	built = true;
	builtGeneration = gen;
}

void SnapshotPickGrid::Rebuild()
{
	cellWorld = static_cast<int>(CQuadField::BASE_QUAD_SIZE);
	numX = std::max(1, (mapDims.mapx * SQUARE_SIZE) / cellWorld);
	numZ = std::max(1, (mapDims.mapy * SQUARE_SIZE) / cellWorld);

	const size_t numCells = static_cast<size_t>(numX) * numZ;
	unitCells.resize(numCells);
	featureCells.resize(numCells);
	for (auto& c : unitCells) c.clear();
	for (auto& c : featureCells) c.clear();

	cellStamp.assign(numCells, 0);
	queryStamp = 0;

	// insert an object into every cell its pickable extent overlaps, in
	// ascending id order (the pickable extent is the max of the model radius
	// and the selection-volume bounding radius -- the ray can only hit the
	// object inside that sphere, so any cell the ray crosses that could hit it
	// is covered)
	const auto insert = [](std::vector<std::vector<int>>& cells,
		int nx, int nz, int cw, int id, const float3& pos, float ext) {
		const int cx0 = std::clamp(static_cast<int>(std::floor((pos.x - ext) / cw)), 0, nx - 1);
		const int cx1 = std::clamp(static_cast<int>(std::floor((pos.x + ext) / cw)), 0, nx - 1);
		const int cz0 = std::clamp(static_cast<int>(std::floor((pos.z - ext) / cw)), 0, nz - 1);
		const int cz1 = std::clamp(static_cast<int>(std::floor((pos.z + ext) / cw)), 0, nz - 1);
		for (int cz = cz0; cz <= cz1; ++cz)
			for (int cx = cx0; cx <= cx1; ++cx)
				cells[cz * nx + cx].push_back(id);
	};

	{
		const SimSnapshot::UnitRows& u = simSnapshot.Read();
		unitSlotStamp.assign(u.MaxUnits(), 0);
		for (size_t id = 0; id < u.MaxUnits(); ++id) {
			// PR 43: only ACTIVE rows enter the pick grid -- NOT DEAD_THIS_BATCH.
			// The index is cached per epoch; a rebuild triggered by a query during
			// the dispatch window would otherwise bake a corpse into the grid and
			// keep it pickable past the window close. A dead unit is not a pick
			// candidate anyway (master unlinked it from the quadfield).
			if (u.valid[id] != SimSnapshotValid::ACTIVE)
				continue;
			const float ext = std::max(u.radius[id], u.selVol[id].GetBoundingRadius());
			insert(unitCells, numX, numZ, cellWorld, static_cast<int>(id), u.pos[id], ext);
		}
	}
	{
		const SimSnapshot::FeatureRows& f = simSnapshot.ReadFeatures();
		featureSlotStamp.assign(f.MaxSlots(), 0);
		for (size_t id = 0; id < f.MaxSlots(); ++id) {
			if (f.valid[id] != SimSnapshotValid::ACTIVE) // PR 43: see the unit loop
				continue;
			const float ext = std::max(f.radius[id], f.selVol[id].GetBoundingRadius());
			insert(featureCells, numX, numZ, cellWorld, static_cast<int>(id), f.pos[id], ext);
		}
	}
}

// clip segment (p0->p1) in XZ against the grid AABB [0,numX*cell]x[0,numZ*cell];
// returns false if it misses entirely. Bounds the DDA to grid size even when the
// camera (ray origin) is far off-map.
static bool ClipSegmentXZ(float& x0, float& z0, float& x1, float& z1, float w, float h)
{
	const float dx = x1 - x0;
	const float dz = z1 - z0;
	float t0 = 0.0f, t1 = 1.0f;

	const auto clip = [&](float p, float q) {
		// p*t <= q for the entering/leaving half-planes
		if (p == 0.0f)
			return (q >= 0.0f);
		const float r = q / p;
		if (p < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
		else          { if (r < t0) return false; if (r < t1) t1 = r; }
		return true;
	};

	if (clip(-dx, x0 - 0.0f) && clip(dx, w - x0) &&
	    clip(-dz, z0 - 0.0f) && clip(dz, h - z0)) {
		const float nx0 = x0 + t0 * dx, nz0 = z0 + t0 * dz;
		const float nx1 = x0 + t1 * dx, nz1 = z0 + t1 * dz;
		x0 = nx0; z0 = nz0; x1 = nx1; z1 = nz1;
		return true;
	}
	return false;
}

void SnapshotPickGrid::GatherCells(const float3& start, const float3& end, int halo)
{
	markedCells.clear();

	float wx0 = start.x, wz0 = start.z, wx1 = end.x, wz1 = end.z;
	if (!ClipSegmentXZ(wx0, wz0, wx1, wz1,
			static_cast<float>(numX * cellWorld), static_cast<float>(numZ * cellWorld)))
		return;

	const auto mark = [&](int cx, int cz) {
		for (int dz = -halo; dz <= halo; ++dz) {
			const int z = cz + dz;
			if (z < 0 || z >= numZ)
				continue;
			for (int dx = -halo; dx <= halo; ++dx) {
				const int x = cx + dx;
				if (x < 0 || x >= numX)
					continue;
				const int c = z * numX + x;
				if (cellStamp[c] == queryStamp)
					continue;
				cellStamp[c] = queryStamp;
				markedCells.push_back(c);
			}
		}
	};

	// Amanatides-Woo 2D DDA in cell space
	const float x0 = wx0 / cellWorld, z0 = wz0 / cellWorld;
	const float x1 = wx1 / cellWorld, z1 = wz1 / cellWorld;
	const float ddx = x1 - x0, ddz = z1 - z0;

	int ix = std::clamp(static_cast<int>(std::floor(x0)), 0, numX - 1);
	int iz = std::clamp(static_cast<int>(std::floor(z0)), 0, numZ - 1);
	const int ixEnd = std::clamp(static_cast<int>(std::floor(x1)), 0, numX - 1);
	const int izEnd = std::clamp(static_cast<int>(std::floor(z1)), 0, numZ - 1);

	const int stepX = (ddx > 0.0f) ? 1 : ((ddx < 0.0f) ? -1 : 0);
	const int stepZ = (ddz > 0.0f) ? 1 : ((ddz < 0.0f) ? -1 : 0);

	constexpr float INF = std::numeric_limits<float>::max();
	const float tDeltaX = (ddx != 0.0f) ? std::fabs(1.0f / ddx) : INF;
	const float tDeltaZ = (ddz != 0.0f) ? std::fabs(1.0f / ddz) : INF;
	float tMaxX = (ddx != 0.0f) ? (((stepX > 0 ? (ix + 1) : ix) - x0) / ddx) : INF;
	float tMaxZ = (ddz != 0.0f) ? (((stepZ > 0 ? (iz + 1) : iz) - z0) / ddz) : INF;

	mark(ix, iz);

	const int maxSteps = numX + numZ + 4;
	for (int s = 0; s < maxSteps && (ix != ixEnd || iz != izEnd); ++s) {
		if (tMaxX < tMaxZ) {
			ix += stepX; tMaxX += tDeltaX;
		} else if (tMaxZ < tMaxX) {
			iz += stepZ; tMaxZ += tDeltaZ;
		} else {
			ix += stepX; iz += stepZ; tMaxX += tDeltaX; tMaxZ += tDeltaZ;
		}
		if (ix < 0 || ix >= numX || iz < 0 || iz >= numZ)
			break;
		mark(ix, iz);
	}
}

void SnapshotPickGrid::QueryRay(const float3& start, const float3& dir, float len, float width,
	std::vector<int>& unitIDs, std::vector<int>& featureIDs)
{
	unitIDs.clear();
	featureIDs.clear();

	EnsureCurrent();
	if (!built || numX <= 0 || numZ <= 0)
		return;

	++queryStamp;

	const float3 end = start + dir * std::max(0.0f, len);
	const int halo = std::max(1, static_cast<int>(std::ceil(width / cellWorld)));
	GatherCells(start, end, halo);

	for (const int c : markedCells) {
		for (const int id : unitCells[c]) {
			if (unitSlotStamp[id] == queryStamp)
				continue;
			unitSlotStamp[id] = queryStamp;
			unitIDs.push_back(id);
		}
		for (const int id : featureCells[c]) {
			if (featureSlotStamp[id] == queryStamp)
				continue;
			featureSlotStamp[id] = queryStamp;
			featureIDs.push_back(id);
		}
	}

	// deterministic candidate order (the coarse phase must never break a tie)
	std::sort(unitIDs.begin(), unitIDs.end());
	std::sort(featureIDs.begin(), featureIDs.end());
}

void SnapshotPickGrid::QueryUnitsInRadius(const float3& pos, float radius, std::vector<int>& unitIDs)
{
	QueryUnitsInRect(float3(pos.x - radius, 0.0f, pos.z - radius), float3(pos.x + radius, 0.0f, pos.z + radius), unitIDs);
}

void SnapshotPickGrid::QueryUnitsInRect(const float3& mins, const float3& maxs, std::vector<int>& unitIDs)
{
	unitIDs.clear();

	EnsureCurrent();
	if (!built || numX <= 0 || numZ <= 0)
		return;

	++queryStamp;

	const int cx0 = std::clamp(static_cast<int>(std::floor(mins.x / cellWorld)), 0, numX - 1);
	const int cx1 = std::clamp(static_cast<int>(std::floor(maxs.x / cellWorld)), 0, numX - 1);
	const int cz0 = std::clamp(static_cast<int>(std::floor(mins.z / cellWorld)), 0, numZ - 1);
	const int cz1 = std::clamp(static_cast<int>(std::floor(maxs.z / cellWorld)), 0, numZ - 1);

	for (int cz = cz0; cz <= cz1; ++cz) {
		for (int cx = cx0; cx <= cx1; ++cx) {
			for (const int id : unitCells[cz * numX + cx]) {
				if (unitSlotStamp[id] == queryStamp)
					continue;
				unitSlotStamp[id] = queryStamp;
				unitIDs.push_back(id);
			}
		}
	}

	std::sort(unitIDs.begin(), unitIDs.end());
}

void SnapshotPickGrid::QueryFeaturesInRect(const float3& mins, const float3& maxs, std::vector<int>& featureIDs)
{
	featureIDs.clear();

	EnsureCurrent();
	if (!built || numX <= 0 || numZ <= 0)
		return;

	++queryStamp;

	const int cx0 = std::clamp(static_cast<int>(std::floor(mins.x / cellWorld)), 0, numX - 1);
	const int cx1 = std::clamp(static_cast<int>(std::floor(maxs.x / cellWorld)), 0, numX - 1);
	const int cz0 = std::clamp(static_cast<int>(std::floor(mins.z / cellWorld)), 0, numZ - 1);
	const int cz1 = std::clamp(static_cast<int>(std::floor(maxs.z / cellWorld)), 0, numZ - 1);

	for (int cz = cz0; cz <= cz1; ++cz) {
		for (int cx = cx0; cx <= cx1; ++cx) {
			for (const int id : featureCells[cz * numX + cx]) {
				if (featureSlotStamp[id] == queryStamp)
					continue;
				featureSlotStamp[id] = queryStamp;
				featureIDs.push_back(id);
			}
		}
	}

	std::sort(featureIDs.begin(), featureIDs.end());
}

void SnapshotPickGrid::QueryFeaturesInRadius(const float3& pos, float radius, std::vector<int>& featureIDs)
{
	QueryFeaturesInRect(float3(pos.x - radius, 0.0f, pos.z - radius), float3(pos.x + radius, 0.0f, pos.z + radius), featureIDs);
}
