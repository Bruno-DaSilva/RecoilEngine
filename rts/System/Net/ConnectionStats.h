/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <atomic>
#include <string>

namespace netcode
{

/**
 * @brief per-link traffic counters
 *
 * Single extension point for link telemetry: add a field here and fill it in
 * GetStats rather than adding another virtual getter. Byte counts are tracked by
 * every connection type; the rest is UDP-only -- see isNetworkLink.
 */
struct ConnectionStats {
	unsigned int sentBytes = 0;
	unsigned int receivedBytes = 0;
	unsigned int sentPackets = 0;
	unsigned int receivedPackets = 0;
	/// chunks put back on the wire after having been sent once
	unsigned int retransmittedChunks = 0;
	/// chunks discarded on arrival because the same chunk had already been received
	unsigned int discardedChunks = 0;
	/// protocol header bytes, against the payload bytes above
	unsigned int sentOverheadBytes = 0;
	unsigned int receivedOverheadBytes = 0;
	/// inbound chunks delivered in order so far
	unsigned int processedChunks = 0;

	/// whether the fields beyond the byte counts describe anything. False on a
	/// loopback, which moves bytes but has no wire to report on.
	bool isNetworkLink = false;
};

namespace detail {
	inline std::atomic<bool> statsSampling = false;
}

/**
 * @brief whether links should collect the optional telemetry in ConnectionStats
 *
 * Byte and packet counters are always kept; the rest exists purely to be
 * exported, so it is off unless something is exporting.
 *
 * Atomic because a host client's own CNetProtocol link is already being
 * serviced when the server it just started flips this.
 */
inline void SetStatsSampling(bool enable) { detail::statsSampling.store(enable, std::memory_order_relaxed); }
inline bool StatsSampling() { return detail::statsSampling.load(std::memory_order_relaxed); }

/// human-readable dump of a link's counters, for the disconnect log
std::string FormatConnectionStats(const ConnectionStats& stats);

}
