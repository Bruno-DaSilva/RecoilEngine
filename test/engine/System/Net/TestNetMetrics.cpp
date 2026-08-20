/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
 * Unit tests for the parts of the metrics path that are pure logic: the NETMSG
 * label table and the delta baselines.
 *
 * All header-only by design, so this needs no socket, no registry and no
 * running server, and unlike test_UDPListener is not gated off under CI.
 */

/*
 * MUST be the first include. -Wswitch fires on a NETMSG missing from
 * TryNetMessageName; promoting it to an error makes that a build failure in
 * every configuration, not just the DEBUG and PROFILE ones where the engine
 * turns -Wall on. NetMessageTypes.h is #pragma once, so an earlier include
 * anywhere would leave the switch uncovered.
 */
#if defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic error "-Wswitch"
#endif
#include "Net/Protocol/NetMessageTypes.h"
#if defined(__GNUC__)
	#pragma GCC diagnostic pop
#endif


#include <set>
#include <string>

#include <catch_amalgamated.hpp>

#include "System/Metrics/Helpers.h"


TEST_CASE("NetMessageName")
{
	SECTION("undeclared ids are not labelled") {
		// the id is a wire byte, so anything unrecognised has to collapse to a
		// single bucket: an open-ended label value is unbounded cardinality
		CHECK(std::string(NetMessageName(0)) == "unknown");
		// a gap inside the declared range (17 and 18 are unused)
		CHECK(std::string(NetMessageName(17)) == "unknown");

		for (int id = NETMSG_LAST; id <= 255; ++id) {
			INFO("id " << id);
			CHECK(std::string(NetMessageName(static_cast<unsigned char>(id))) == "unknown");
		}
	}

	SECTION("declared ids map to their own name") {
		CHECK(std::string(NetMessageName(NETMSG_KEYFRAME)) == "keyframe");
		CHECK(std::string(NetMessageName(NETMSG_CHAT)) == "chat");
		CHECK(std::string(NetMessageName(NETMSG_ATTEMPTCONNECT)) == "attemptconnect");
		CHECK(std::string(NetMessageName(NETMSG_PING)) == "ping");
	}

	// -Wswitch above covers "a NETMSG has no name". These cover the two ways a
	// name can be wrong in a way no compiler can see.
	SECTION("names are unique") {
		// two message types sharing a name silently merge their series, and the
		// merged total still looks plausible
		std::set<std::string> seen;

		for (int id = 0; id < NETMSG_LAST; ++id) {
			const char* const name = TryNetMessageName(static_cast<NETMSG>(id));

			if (name == nullptr)
				continue;

			INFO("id " << id << " name '" << name << "'");
			CHECK(seen.insert(name).second);
		}
	}

	SECTION("names are label-safe") {
		for (int id = 0; id < NETMSG_LAST; ++id) {
			const char* const name = TryNetMessageName(static_cast<NETMSG>(id));

			if (name == nullptr)
				continue;

			const std::string str = name;

			INFO("id " << id << " name '" << str << "'");
			CHECK(!str.empty());
			CHECK(str.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos);
		}
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
