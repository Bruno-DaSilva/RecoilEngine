/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef BUILDINGMASKMAP_H
#define BUILDINGMASKMAP_H

#include <vector>

#include "System/creg/creg_cond.h"

class BuildingMaskMap
{
	CR_DECLARE_STRUCT(BuildingMaskMap)

public:
	void Init(unsigned int numSquares) {
		maskMap.clear();
		maskMap.resize(numSquares, 1); // 1st bit set to 1 indicates a normal tile
	};
	void Kill() {
		maskMap.clear();
	}

	bool SetTileMask(unsigned int x, unsigned int z, std::uint16_t value);
	bool TestTileMask(unsigned int x, unsigned int z, std::uint16_t value) const { return (CheckBounds(x, z) && TestTileMaskUnsafe(x, z, value)); }
	bool TestTileMaskUnsafe(unsigned int x, unsigned int z, std::uint16_t value) const;

	// PLACEMENT REHOST: raw tile-mask read for the draw-side mirror drain (flat
	// index == x + z*hmapx, matching the internal maskMap layout).
	std::uint16_t GetTileMaskUnsafe(unsigned int idx) const { return maskMap[idx]; }
	unsigned int GetNumTiles() const { return static_cast<unsigned int>(maskMap.size()); }

private:
	bool CheckBounds(unsigned int x, unsigned int z) const;

private:
	std::vector<std::uint16_t> maskMap;
};

extern BuildingMaskMap buildingMaskMap;

#endif

