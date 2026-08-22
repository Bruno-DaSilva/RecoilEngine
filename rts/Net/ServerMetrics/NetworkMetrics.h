/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <vector>

#include "System/Misc/SpringTime.h"

namespace prometheus
{
	template<typename T> class Family;
	class Counter;
	class Gauge;
	class Registry;
}

class CGameServer;

/**
 * @brief the recoil_network_ series: per-link traffic, loss, and queue health
 *
 * Owned by ServerMetrics. Covers real network conns only, no local loopback
 * connections.
 */
class NetworkMetrics
{
public:
	void Init(prometheus::Registry& registry);
	void Update(const CGameServer& server);

	/// a fresh connection restarts its counters at zero, so the delta baselines
	/// must also be reset.
	void ResetConnectionDeltas(int playerId);

private:
	struct ConnectionMetrics {
		/// Last seen value of a monotonically increasing link counter, which
		/// Update() then publishes as a delta to metrics.
		///
		/// Ideally we'd do a PrometheusCounter::SetValue(), but there's no API
		/// for that like there is in other languages. So we are forced to instead
		/// calculate a delta off an already monotonically-increasing stat.
		struct DeltaCounter {
			prometheus::Counter* player = nullptr;
			double last = 0.0;
		};
		DeltaCounter sentBytes;
		DeltaCounter recvBytes;
		DeltaCounter sentPackets;
		DeltaCounter recvPackets;
		DeltaCounter resentOutgoingChunks;
		DeltaCounter redundantOutgoingChunks;
		DeltaCounter duplicateIncomingChunks;
		DeltaCounter missingIncomingChunks;
		DeltaCounter outgoingThrottled;
		DeltaCounter incomingReorderStall;

		prometheus::Gauge* lossFactor = nullptr;
		prometheus::Gauge* outgoingBw = nullptr;
		prometheus::Gauge* unackedOutgoingChunks = nullptr;
		prometheus::Gauge* outgoingResendQueueDepth = nullptr;
		prometheus::Gauge* incomingReorderQueueDepth = nullptr;
		prometheus::Gauge* outgoingQueueBytes = nullptr;
		prometheus::Gauge* unackedOutgoingAge = nullptr;

		DeltaCounter responseTimeSum;
		prometheus::Counter* responseTimeCount = nullptr;
		/// baseline for responseTimeCount, tracking accumulated.responseTimeCount
		double lastResponseTimeCount = 0.0;
	};

	/// this player's metric slot, created on first use
	ConnectionMetrics& ConnectionSlot(int playerId);

	/// drop a connection's gauges from the registry rather than leave them
	/// reporting a connection that is gone. Counters are cumulative and stay.
	void ReleaseConnectionGauges(ConnectionMetrics& cm);

	std::vector<ConnectionMetrics> connectionMetrics;

	/// MetricsPerPlayer, latched in Init(). Gates all the per-player metrics below.
	bool perPlayerEnabled = false;

	// per-player metrics, null unless perPlayerEnabled
	prometheus::Family<prometheus::Counter>* metricBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricPackets = nullptr;
	prometheus::Family<prometheus::Counter>* metricResentOutgoingChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricRedundantOutgoingChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricDuplicateIncomingChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricMissingIncomingChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricThrottled = nullptr;
	prometheus::Family<prometheus::Counter>* metricIncomingReorderStall = nullptr;
	prometheus::Family<prometheus::Gauge>* metricLossFactor = nullptr;
	prometheus::Family<prometheus::Gauge>* metricOutgoingBw = nullptr;
	prometheus::Family<prometheus::Gauge>* metricUnackedOutgoingChunks = nullptr;
	prometheus::Family<prometheus::Gauge>* metricOutgoingResendQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricIncomingReorderQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricOutgoingQueueBytes = nullptr;
	prometheus::Family<prometheus::Counter>* metricResponseTimeSum = nullptr;
	prometheus::Family<prometheus::Counter>* metricResponseTimeCount = nullptr;
	prometheus::Family<prometheus::Gauge>* metricUnackedOutgoingAge = nullptr;

	// server-wide aggregates, exported whether or not per-player metrics are on
	prometheus::Counter* metricTotalSentBytes = nullptr;
	prometheus::Counter* metricTotalRecvBytes = nullptr;
	prometheus::Counter* metricTotalSentPackets = nullptr;
	prometheus::Counter* metricTotalRecvPackets = nullptr;
	prometheus::Counter* metricTotalResentOutgoingChunks = nullptr;
	prometheus::Counter* metricTotalRedundantOutgoingChunks = nullptr;
	prometheus::Counter* metricTotalDuplicateIncomingChunks = nullptr;
	prometheus::Counter* metricTotalMissingIncomingChunks = nullptr;
	/// holds the family so a later direction child can be added to it
	prometheus::Family<prometheus::Counter>* metricThrottledFamily = nullptr;
	prometheus::Counter* metricTotalOutgoingThrottled = nullptr;
	prometheus::Counter* metricTotalIncomingReorderStall = nullptr;
	prometheus::Gauge* metricRedundancyLinks = nullptr;
	prometheus::Gauge* metricTotalOutgoingBw = nullptr;
	prometheus::Gauge* metricTotalUnackedOutgoingChunks = nullptr;
	prometheus::Gauge* metricTotalOutgoingResendQueueDepth = nullptr;
	prometheus::Gauge* metricTotalIncomingReorderQueueDepth = nullptr;
	prometheus::Gauge* metricTotalOutgoingQueueBytes = nullptr;

	prometheus::Gauge* metricMaxUnackedOutgoingAge = nullptr;

	/// running peak of the worst link's oldest-unacked age, feeding the gauge
	/// above. Cleared on a fixed cadence (unackedAgePeakWindow) rather than per
	/// scrape, so a spike that opens and clears between two scrapes still gets
	/// read. See NetworkMetrics::Update.
	float windowMaxUnackedAgeMs = 0.0f;
	spring_time lastUnackedAgePeakReset = spring_notime;
};
