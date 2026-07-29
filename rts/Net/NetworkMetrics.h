/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <vector>

namespace prometheus
{
	template<typename T> class Family;
	class Counter;
	class Registry;
}

class CGameServer;

/**
 * @brief the recoil_network_ series: per-link traffic, loss and queue health
 *
 * Owned by ServerMetrics, which forwards to it. Covers real network links only:
 * a listen-server host's own loopback client is excluded throughout, which is
 * what makes a hosted game's numbers comparable to a dedicated one's.
 */
class NetworkMetrics
{
public:
	void Init(prometheus::Registry& registry);
	void Update(const CGameServer& server);

	/// a fresh link restarts its counters at zero, so the delta baselines must
	/// follow or DeltaSince's clamp swallows everything up to the old totals
	void ResetConnectionDeltas(int playerId);

private:
	struct ConnectionMetrics {
		/// last seen value of a monotonically increasing link counter, which
		/// Update() publishes as a delta
		struct DeltaCounter {
			prometheus::Counter* player = nullptr;
			double last = 0.0;
		};
		DeltaCounter sentBytes;
		DeltaCounter recvBytes;
		DeltaCounter sentPackets;
		DeltaCounter recvPackets;
	};

	/// this player's metric slot, created on first use
	ConnectionMetrics& ConnectionSlot(int playerId);

	std::vector<ConnectionMetrics> connectionMetrics;

	// per-player families; null unless MetricsPerPlayer is on
	prometheus::Family<prometheus::Counter>* metricSentBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricRecvBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricSentPackets = nullptr;
	prometheus::Family<prometheus::Counter>* metricRecvPackets = nullptr;

	// server-wide aggregates, exported whether or not per-player metrics are on
	prometheus::Counter* metricTotalSentBytes = nullptr;
	prometheus::Counter* metricTotalRecvBytes = nullptr;
	prometheus::Counter* metricTotalSentPackets = nullptr;
	prometheus::Counter* metricTotalRecvPackets = nullptr;
};
