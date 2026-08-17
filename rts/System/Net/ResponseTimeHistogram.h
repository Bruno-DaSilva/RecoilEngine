/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cstddef>

namespace netcode
{

/**
 * @brief send->ack times sorted into buckets, 5-640ms
 *
 * Not round-trip time, due to limitations of the current implementation:
 * - A sample carries the client's wait before it next transmits and its
 *   frame processing
 * - An ack is cumulative and names no packet, so we cannot tell which sent
 *   packet it answers
 * - A chunk we resent is never sampled (see AckChunks)
 *
 * For these reasons, a badly-losing link shows up as fewer samples rather
 * than a longer tail. Read the low quantiles as latency and the width
 * above them as jitter and client responsiveness.
 *
 * Use oldestUnackedOutgoingMs for the worst connections as that will remain
 * accurate at longer response times.
 */
struct ResponseTimeHistogram {
	static constexpr float baseMs = 5.0f;
	static constexpr std::array<float, 2> mantissas = {1.0f, 1.5f};
	static constexpr std::size_t numBounds = 15;
	/// one more than the bounds, as the final bucket holds everything from the top bound to +Inf
	static constexpr std::size_t numBuckets = numBounds + 1;

	static constexpr std::array<float, numBounds> boundsMs = [] {
		std::array<float, numBounds> bounds = {};

		for (std::size_t i = 0; i < numBounds; ++i)
			bounds[i] = baseMs * mantissas[i % mantissas.size()] * (1 << (i / mantissas.size()));

		return bounds;
	}();

	/// index of the first bound at or above the sample, matching prometheus's
	/// "le" buckets
	static constexpr std::size_t BucketIndex(float sampleMs)
	{
		for (std::size_t i = 0; i < numBounds; ++i) {
			if (sampleMs <= boundsMs[i])
				return i;
		}

		return numBounds;
	}

	void Observe(float sampleMs) { counts[BucketIndex(sampleMs)] += 1; }

	/// how many samples fell in each bucket, cumulative over the connection's life
	std::array<unsigned int, numBuckets> counts = {};
};

}
