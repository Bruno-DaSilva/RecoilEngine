/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/*
 * Unit tests for the pure-logic parts of the link telemetry: here, the send->ack
 * smoothing and the bucket binning that feeds the exported histogram.
 *
 * Both are header-only by design, so this needs no socket and no live
 * connection -- which is why, unlike test_UDPListener, it is not gated off
 * under CI.
 */

#include <catch_amalgamated.hpp>

#include "System/Net/Connection.h"


TEST_CASE("ResponseTimeBucketIndex")
{
	using namespace netcode;

	SECTION("a sample equal to a bound lands in that bound's bucket") {
		// lower_bound semantics, matching prometheus-cpp's Histogram::Observe:
		// bucket i counts samples in (bounds[i-1], bounds[i]]
		for (unsigned i = 0; i < responseTimeNumBucketBounds; ++i) {
			INFO("bound " << responseTimeBucketBoundsMs[i] << "ms");
			CHECK(ResponseTimeBucketIndex(responseTimeBucketBoundsMs[i]) == i);
		}
	}

	SECTION("a sample under the first bound lands in the first bucket") {
		CHECK(ResponseTimeBucketIndex(0.0f) == 0u);
		CHECK(ResponseTimeBucketIndex(responseTimeBucketBoundsMs[0] - 0.1f) == 0u);
	}

	SECTION("a sample over the last bound lands in the trailing bucket") {
		const float lastBoundMs = responseTimeBucketBoundsMs[responseTimeNumBucketBounds - 1];

		CHECK(ResponseTimeBucketIndex(lastBoundMs + 0.1f) == responseTimeNumBucketBounds);
		CHECK(ResponseTimeBucketIndex(1.0e9f) == responseTimeNumBucketBounds);
	}

	SECTION("every index is a valid array position") {
		// an out-of-range index is an out-of-bounds write into
		// ConnectionStats::responseTimeBuckets, on the netcode thread
		const float samplesMs[] = {-1.0f, 0.0f, 4.9f, 5.0f, 5.1f, 1599.0f, 1600.0f, 1601.0f, 1.0e30f};

		for (const float sampleMs: samplesMs) {
			INFO("sample " << sampleMs << "ms");
			CHECK(ResponseTimeBucketIndex(sampleMs) < responseTimeNumBuckets);
		}
	}
}


TEST_CASE("UpdateResponseTimeMovingAvg")
{
	using namespace netcode;

	SECTION("the first sample seeds rather than smooths") {
		float movingAvgMs = 0.0f;
		float jitterMs = 0.0f;

		UpdateResponseTimeMovingAvg(80.0f, false, movingAvgMs, jitterMs);

		CHECK(movingAvgMs == Catch::Approx(80.0f));
		CHECK(jitterMs == Catch::Approx(40.0f)); // RFC 6298 seeds RTTVAR at R/2
	}

	SECTION("seeding is gated on having sampled, not on the value being zero") {
		// a link fast enough to sample 0ms must not re-seed on every ack, which
		// would make the "smoothed" value track the latest sample exactly
		float movingAvgMs = 0.0f;
		float jitterMs = 0.0f;

		UpdateResponseTimeMovingAvg(0.0f, false, movingAvgMs, jitterMs);
		REQUIRE(movingAvgMs == Catch::Approx(0.0f));

		UpdateResponseTimeMovingAvg(100.0f, true, movingAvgMs, jitterMs);

		CHECK(movingAvgMs == Catch::Approx(12.5f)); // smoothed, not re-seeded to 100
		CHECK(jitterMs == Catch::Approx(25.0f));
	}

	SECTION("deviation is taken against the previous smoothed value") {
		// order matters: the jitter has to move before the average does, or it
		// measures deviation from a value that already absorbed this sample
		float movingAvgMs = 100.0f;
		float jitterMs = 0.0f;

		UpdateResponseTimeMovingAvg(200.0f, true, movingAvgMs, jitterMs);

		CHECK(jitterMs == Catch::Approx(25.0f)); // 0 + (|100 - 200| - 0) * 1/4
		CHECK(movingAvgMs == Catch::Approx(112.5f));  // 100 + (200 - 100) * 1/8
	}

	SECTION("a steady link converges on its sample and loses its jitter") {
		float movingAvgMs = 500.0f;
		float jitterMs = 250.0f;

		for (int i = 0; i < 200; ++i)
			UpdateResponseTimeMovingAvg(40.0f, true, movingAvgMs, jitterMs);

		CHECK(movingAvgMs == Catch::Approx(40.0f).margin(0.5f));
		CHECK(jitterMs == Catch::Approx(0.0f).margin(0.5f));
	}
}
