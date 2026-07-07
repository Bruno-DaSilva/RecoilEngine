/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNIT_TOOLTIP_MAP_H
#define UNIT_TOOLTIP_MAP_H

#include "System/UnorderedMap.hpp"

struct UnitToolTipMap {
public:
	void Clear() {
		tooltips.clear();
		tooltips.reserve(256);
	}

	void Set(int id, std::string&& tip) { tooltips[id] = std::move(tip); }
	const std::string& Get(int id) { return tooltips[id]; }

	// non-inserting const lookup (SimSnapshot PR 32 extraction: reads the custom
	// tooltip for every unit at the parked boundary, must not mutate the map --
	// Get()'s operator[] would insert empty entries). Returns "" for a missing
	// id, exactly what Get() yields for one (Get inserts then returns the empty).
	const std::string& GetConst(int id) const {
		static const std::string empty;
		const auto it = tooltips.find(id);
		return (it != tooltips.end()) ? it->second : empty;
	}

private:
	spring::unordered_map<int, std::string> tooltips;
};

extern UnitToolTipMap unitToolTipMap;

#endif

