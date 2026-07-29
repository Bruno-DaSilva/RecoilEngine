/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <vector>

namespace prometheus
{
	template<typename T> class Family;
	class Counter;
	class Gauge;
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
		DeltaCounter sendErrors;
		DeltaCounter recvErrors;

		prometheus::Gauge* outgoingBw = nullptr;
		prometheus::Gauge* unackedChunks = nullptr;
		prometheus::Gauge* resendQueueDepth = nullptr;
		prometheus::Gauge* reorderQueueDepth = nullptr;
		prometheus::Gauge* sendQueueBytes = nullptr;
	};

	/// this player's metric slot, created on first use
	ConnectionMetrics& ConnectionSlot(int playerId);

	/// drop a connection's gauges from the registry rather than leave them
	/// reporting a link that is gone. Counters are cumulative and stay.
	void ReleaseConnectionGauges(ConnectionMetrics& cm);

	std::vector<ConnectionMetrics> connectionMetrics;

	// per-player families; null unless MetricsPerPlayer is on
	prometheus::Family<prometheus::Counter>* metricSentBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricRecvBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricSentPackets = nullptr;
	prometheus::Family<prometheus::Counter>* metricRecvPackets = nullptr;
	prometheus::Family<prometheus::Counter>* metricSocketErrors = nullptr;
	prometheus::Family<prometheus::Gauge>* metricOutgoingBw = nullptr;
	prometheus::Family<prometheus::Gauge>* metricUnackedChunks = nullptr;
	prometheus::Family<prometheus::Gauge>* metricResendQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricReorderQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricSendQueueBytes = nullptr;

	// server-wide aggregates, exported whether or not per-player metrics are on
	prometheus::Counter* metricTotalSentBytes = nullptr;
	prometheus::Counter* metricTotalRecvBytes = nullptr;
	prometheus::Counter* metricTotalSentPackets = nullptr;
	prometheus::Counter* metricTotalRecvPackets = nullptr;
	/// three children of one {direction, socket}-labelled family
	prometheus::Counter* metricTotalSendErrors = nullptr;
	prometheus::Counter* metricTotalRecvErrors = nullptr;
	prometheus::Counter* metricListenerRecvErrors = nullptr;
	prometheus::Gauge* metricTotalOutgoingBw = nullptr;
	prometheus::Gauge* metricTotalUnackedChunks = nullptr;
	prometheus::Gauge* metricTotalResendQueueDepth = nullptr;
	prometheus::Gauge* metricTotalReorderQueueDepth = nullptr;
	prometheus::Gauge* metricTotalSendQueueBytes = nullptr;

	unsigned int lastListenerRecvErrors = 0;
};
