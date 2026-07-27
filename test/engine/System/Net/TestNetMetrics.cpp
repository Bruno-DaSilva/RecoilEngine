/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/*
 * Unit tests for the parts of the metrics path that are pure logic: the
 * send->ack smoothing and binning, and the delta baselines the publish loop
 * turns cumulative link counters into.
 *
 * All header-only by design, so this needs no socket, no registry and no
 * running server, and unlike test_UDPListener is not gated off under CI.
 */

#include <catch_amalgamated.hpp>

#include "System/Metrics/Delta.h"
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


TEST_CASE("DeltaSince")
{
	using metrics::DeltaSince;

	SECTION("a rising counter publishes what accrued since the last poll") {
		unsigned int last = 0;

		CHECK(DeltaSince(10u, last) == 10u);
		CHECK(last == 10u);

		CHECK(DeltaSince(25u, last) == 15u);
		CHECK(last == 25u);

		CHECK(DeltaSince(25u, last) == 0u);
		CHECK(last == 25u);
	}

	SECTION("a link that restarts at zero contributes nothing, not its old total") {
		// the reconnect case this exists for: a fresh link's counters begin at
		// zero, and an unclamped delta would underflow into a huge positive one
		unsigned int last = 4000u;

		CHECK(DeltaSince(0u, last) == 0u);
		CHECK(last == 0u);

		// and the new link's own traffic is published normally from there
		CHECK(DeltaSince(120u, last) == 120u);
	}

	SECTION("any backwards step re-baselines instead of publishing a spike") {
		unsigned int last = 4000u;

		CHECK(DeltaSince(2500u, last) == 0u);
		CHECK(last == 2500u);
	}

	SECTION("a 32-bit counter wrap costs its interval rather than spiking") {
		// dataSent/dataRecv are unsigned int, so a wrap is reachable in
		// principle; it has to under-report by one interval, never over-report
		unsigned int last = 0xFFFFFFF0u;

		CHECK(DeltaSince(0x10u, last) == 0u);
		CHECK(last == 0x10u);
	}

	SECTION("the duration accumulators go through the same clamp") {
		double last = 0.0;

		// explicit <double>: the engine builds with -fsingle-precision-constant
		// under gcc, so an unsuffixed literal is a float here and deduction
		// against the double baseline would be ambiguous
		CHECK(DeltaSince<double>(2.5, last) == Catch::Approx(2.5));
		CHECK(DeltaSince<double>(4.0, last) == Catch::Approx(1.5));
		CHECK(DeltaSince<double>(0.0, last) == Catch::Approx(0.0));
		CHECK(last == Catch::Approx(0.0));
	}
}
