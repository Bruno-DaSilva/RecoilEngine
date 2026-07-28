/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <algorithm>

namespace metrics {
	/**
	 * @brief advance a delta baseline and return what accrued since the last call
	 *
	 * Clamped rather than wrapped: a link's counters restart at zero when a
	 * client reconnects, and a negative delta would publish as a huge positive
	 * one.
	 */
	template<typename T> T DeltaSince(T cur, T& last)
	{
		const T delta = cur - std::min(cur, last);

		last = cur;
		return delta;
	}
}
