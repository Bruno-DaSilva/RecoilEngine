/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <vector>

#include "System/float3.h"

/**
 * @brief SnapshotPickGrid -- draw-side broadphase over SimSnapshot positions
 *
 * PR 25 of the sim/draw decoupling plan (doc/sim-draw-thread-decoupling-research.md
 * section D). Draw-side picking (GuiTraceRay, minimap closest-unit) used to walk
 * the sim quadField's live unit/feature lists; the split forbids that (no shared
 * lock on the quadfield, no live sim reads on the draw thread). This is the
 * replacement: a uniform grid rebuilt from the published SimSnapshot front
 * buffers, holding snapshot slot ids (== unit/feature ids) per cell, queried for
 * the coarse phase of ray picks and radius picks. The precise phase
 * (CCollisionHandler::MouseHit against the snapshot selection volume) decides the
 * winner, so the grid only has to return a conservative superset -- it never
 * changes which object is picked, only how the candidate set is found.
 *
 * Geometry mirrors CQuadField (BASE_QUAD_SIZE = 128-elmo cells,
 * numX = mapWidth / cell) so the cell coarseness matches master's quad walk;
 * reproducing master's picking is the bar, not improving it.
 *
 * Freshness: EnsureCurrent() rebuilds only when SimSnapshot::Generation() changed
 * (once per boundary). Synchronous input-side callers (outside CGame::Draw) query
 * whatever the last published boundary built -- the same <=1 draw-frame pick
 * latency decided for boundary picking (research doc section D / SimSnapshot.h).
 *
 * Determinism: an object is inserted into every cell its pickable extent overlaps
 * in ascending id order, and every query sorts its gathered candidate ids
 * ascending before returning, so picking results are stable run-to-run
 * regardless of cell-visit order (the coarse phase never breaks a tie -- the
 * precise MouseHit distance test does, exactly as in master).
 */
class SnapshotPickGrid {
public:
	// rebuild from the current SimSnapshot front buffers iff the generation
	// advanced since the last build; cheap no-op otherwise
	void EnsureCurrent();

	// game teardown: drop the grid so the next game's first query rebuilds
	void Clear();

	// coarse phase for a ray pick: gather unit + feature snapshot ids whose
	// pickable extent lies in a cell the segment [start, start + dir*len]
	// crosses, optionally widened by `width` (the radar-error wide-ray). Output
	// vectors are cleared then filled with deduplicated, ascending ids.
	void QueryRay(const float3& start, const float3& dir, float len, float width,
		std::vector<int>& unitIDs, std::vector<int>& featureIDs);

	// coarse phase for a radius pick (minimap closest-unit): gather unit ids
	// whose cell lies within `radius` (XZ) of `pos`. Ascending, deduplicated.
	void QueryUnitsInRadius(const float3& pos, float radius, std::vector<int>& unitIDs);

	// coarse phase for the Lua spatial-list queries (PR 27b, the
	// GetUnitsInRectangle/GetFeaturesIn* serving twins): gather ids whose cell
	// overlaps the XZ rect [mins, maxs] (radius form scans [pos - r, pos + r]).
	// Conservative superset -- the twins re-apply the exact live filters
	// (quadfield pos bounds / distance math) on the candidates. Ascending,
	// deduplicated.
	void QueryUnitsInRect(const float3& mins, const float3& maxs, std::vector<int>& unitIDs);
	void QueryFeaturesInRect(const float3& mins, const float3& maxs, std::vector<int>& featureIDs);
	void QueryFeaturesInRadius(const float3& pos, float radius, std::vector<int>& featureIDs);

private:
	void Rebuild();
	void GatherCells(const float3& start, const float3& end, int halo);
	int CellX(float wx) const;
	int CellZ(float wz) const;

private:
	int numX = 0;
	int numZ = 0;
	int cellWorld = 0;         // elmos per cell (BASE_QUAD_SIZE)
	uint64_t builtGeneration = 0; // PR 43 §2.5: EpochId key (u64, monotonic per game)
	bool built = false;

	// per-cell id lists, indexed [z * numX + x]; capacity is reused across
	// rebuilds (clear() keeps the allocation)
	std::vector<std::vector<int>> unitCells;
	std::vector<std::vector<int>> featureCells;

	// scratch reused every query: cell-visit dedup and the marked-cell list
	std::vector<uint32_t> cellStamp;   // [numX*numZ], compared against queryStamp
	std::vector<int> markedCells;
	uint32_t queryStamp = 0;

	// per-slot dedup for the gathered id lists (stamp == queryStamp => seen)
	std::vector<uint32_t> unitSlotStamp;
	std::vector<uint32_t> featureSlotStamp;
};

extern SnapshotPickGrid snapshotPickGrid;
