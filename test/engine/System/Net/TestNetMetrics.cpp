/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
 * Unit tests for the parts of the metrics path that are pure logic: the
 * send->ack bucketing, and the delta baselines.
 *
 * All header-only by design, so this needs no socket, no registry and no
 * running server, and unlike test_UDPListener is not gated off under CI.
 */

#include <array>

#include <catch_amalgamated.hpp>

#include "System/Metrics/Helpers.h"
#include "System/Net/ResponseTimeHistogram.h"


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


TEST_CASE("ResponseTimeHistogram")
{
	using netcode::ResponseTimeHistogram;

	SECTION("the generated ladder is the one we meant to publish") {
		const std::array<float, 15> expected = {
			5.0f, 7.5f, 10.0f, 15.0f, 20.0f, 30.0f, 40.0f, 60.0f,
			80.0f, 120.0f, 160.0f, 240.0f, 320.0f, 480.0f, 640.0f
		};

		REQUIRE(ResponseTimeHistogram::numBounds == expected.size());

		for (std::size_t i = 0; i < expected.size(); ++i)
			CHECK(ResponseTimeHistogram::boundsMs[i] == Catch::Approx(expected[i]));
	}

	SECTION("bounds are strictly ascending") {
		// prometheus-cpp throws on construction otherwise, which would take the
		// server down at Init rather than at the first sample
		for (std::size_t i = 1; i < ResponseTimeHistogram::numBounds; ++i)
			CHECK(ResponseTimeHistogram::boundsMs[i - 1] < ResponseTimeHistogram::boundsMs[i]);
	}

	SECTION("a sample lands in the first bucket at or above it") {
		// prometheus buckets are "le", so a sample exactly on a bound belongs to
		// that bound and not the next
		CHECK(ResponseTimeHistogram::BucketIndex(0.0f) == 0u);
		CHECK(ResponseTimeHistogram::BucketIndex(5.0f) == 0u);
		CHECK(ResponseTimeHistogram::BucketIndex(5.01f) == 1u);
		CHECK(ResponseTimeHistogram::BucketIndex(7.5f) == 1u);
		CHECK(ResponseTimeHistogram::BucketIndex(100.0f) == 9u);
	}

	SECTION("no bucket is more than half again as wide as the one below it") {
		// what lets a quantile be interpolated inside any bucket and trusted about
		// as much as one interpolated inside any other
		for (std::size_t i = 1; i < ResponseTimeHistogram::numBounds; ++i)
			CHECK(ResponseTimeHistogram::boundsMs[i] <= ResponseTimeHistogram::boundsMs[i - 1] * 1.5f);
	}

	SECTION("anything past the top bound goes to the overflow bucket") {
		CHECK(ResponseTimeHistogram::BucketIndex(640.0f) == ResponseTimeHistogram::numBounds - 1);
		CHECK(ResponseTimeHistogram::BucketIndex(640.01f) == ResponseTimeHistogram::numBounds);
		CHECK(ResponseTimeHistogram::BucketIndex(1.0e6f) == ResponseTimeHistogram::numBounds);

		// the overflow is what makes counts one longer than the bounds, which is
		// the size prometheus-cpp requires of the increments it is handed
		CHECK(ResponseTimeHistogram::numBuckets == ResponseTimeHistogram::numBounds + 1);
	}

	SECTION("observing fills the bucket the sample belongs to and no other") {
		ResponseTimeHistogram hist;

		hist.Observe(4.0f);
		hist.Observe(4.5f);
		hist.Observe(50.0f);

		CHECK(hist.counts[0] == 2u);
		CHECK(hist.counts[ResponseTimeHistogram::BucketIndex(50.0f)] == 1u);

		unsigned int total = 0;

		for (const unsigned int count: hist.counts)
			total += count;

		CHECK(total == 3u);
	}
}
