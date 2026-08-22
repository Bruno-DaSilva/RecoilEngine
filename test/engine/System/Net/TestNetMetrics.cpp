/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
 * Unit tests for the parts of the metrics path that are pure logic: the delta
 * baselines behind the cumulative counters.
 *
 * All header-only by design, so this needs no socket, no registry and no
 * running server, and unlike test_UDPListener is not gated off under CI.
 */

#include <catch_amalgamated.hpp>

#include "System/Metrics/Helpers.h"


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
