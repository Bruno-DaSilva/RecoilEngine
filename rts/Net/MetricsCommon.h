/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <string>
#include <vector>

#include <prometheus/counter.h>
#include <prometheus/family.h>

/// shared by the metric groups; not included by the facade header, so a build
/// without prometheus never sees these
namespace metrics {

/// the netcode works in milliseconds, prometheus wants base units
inline constexpr double msToSecs = 0.001;

/**
 * @brief add to a server-wide counter and, when enabled, its per-player counterpart
 *
 * @param perPlayerChild cache for the labelled child; Family::Add builds a label
 *   map and allocates a throwaway metric on every call
 */
inline void AddPlayerMetric(
	prometheus::Counter* total,
	prometheus::Family<prometheus::Counter>* perPlayer,
	prometheus::Counter*& perPlayerChild,
	int playerId,
	double value
) {
	if (total == nullptr || value <= 0.0)
		return;

	total->Increment(value);

	if (perPlayer == nullptr)
		return;

	if (perPlayerChild == nullptr)
		perPlayerChild = &perPlayer->Add({{"playerid", std::to_string(playerId)}});

	perPlayerChild->Increment(value);
}

inline void CountEvent(prometheus::Family<prometheus::Counter>* family, const char* labelKey, const char* labelValue)
{
	if (family != nullptr)
		family->Add({{labelKey, labelValue}}).Increment();
}

/// element at `index`, growing the vector to fit
template<typename T> T& AtGrowing(std::vector<T>& v, int index)
{
	if (v.size() <= static_cast<size_t>(index))
		v.resize(index + 1);

	return v[index];
}

}
