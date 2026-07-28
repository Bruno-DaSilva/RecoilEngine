/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <vector>

#include "System/Net/Connection.h" // netcode::responseTimeNumBuckets

namespace prometheus
{
	template<typename T> class Family;
	class Counter;
	class Gauge;
	class Histogram;
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
	/// attribute packet bytes to a NETMSG type; aggregate only
	void CountMessageBytes(bool outgoing, unsigned char msgId, unsigned int bytes);

	void CountConnectionAttempt();
	void CountConnectionRejected(const char* reason);
	void CountConnectionEstablished(bool reconnect);
	void CountConnectionClosed(const char* reason);

	void CountThrottledPackets(int playerId, int numPackets);
	void CountIncomingThrottled(int playerId, double milliSecs);

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
		DeltaCounter resentChunks;
		DeltaCounter redundantChunks;
		DeltaCounter droppedChunks;
		DeltaCounter lostIncomingChunks;
		DeltaCounter outgoingThrottled;
		DeltaCounter reorderStall;

		// resolved lazily on first event, see AddPlayerMetric
		prometheus::Counter* throttledPackets = nullptr;
		prometheus::Counter* incomingThrottled = nullptr;

		prometheus::Gauge* lossFactor = nullptr;
		prometheus::Gauge* outgoingBw = nullptr;
		prometheus::Gauge* unackedChunks = nullptr;
		prometheus::Gauge* resendQueueDepth = nullptr;
		prometheus::Gauge* reorderQueueDepth = nullptr;
		prometheus::Gauge* sendQueueBytes = nullptr;
		prometheus::Gauge* responseTime = nullptr;
		prometheus::Gauge* responseTimeMax = nullptr;
		prometheus::Gauge* responseTimeJitter = nullptr;
		prometheus::Gauge* unackedAge = nullptr;
		prometheus::Gauge* incomingBandwidthUsage = nullptr;

		/// previous cumulative histogram state, so each poll contributes only
		/// the samples taken since the last one
		std::array<unsigned int, netcode::responseTimeNumBuckets> lastResponseTimeBuckets = {};
		double lastResponseTimeSumMs = 0.0;
	};

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
	prometheus::Family<prometheus::Counter>* metricResentChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricRedundantChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricDroppedChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricLostIncomingChunks = nullptr;
	prometheus::Family<prometheus::Counter>* metricMessageBytes = nullptr;
	/// [outgoing][NETMSG id] -> counter, resolved on first sighting so only
	/// message types that occur create series
	std::array<std::array<prometheus::Counter*, 256>, 2> messageBytesCounters = {};
	prometheus::Family<prometheus::Counter>* metricThrottledPackets = nullptr;
	prometheus::Family<prometheus::Counter>* metricIncomingThrottled = nullptr;
	prometheus::Family<prometheus::Counter>* metricOutgoingThrottled = nullptr;
	prometheus::Family<prometheus::Counter>* metricReorderStall = nullptr;
	prometheus::Family<prometheus::Gauge>* metricLossFactor = nullptr;
	prometheus::Family<prometheus::Gauge>* metricOutgoingBw = nullptr;
	prometheus::Family<prometheus::Gauge>* metricUnackedChunks = nullptr;
	prometheus::Family<prometheus::Gauge>* metricResendQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricReorderQueueDepth = nullptr;
	prometheus::Family<prometheus::Gauge>* metricSendQueueBytes = nullptr;
	prometheus::Family<prometheus::Gauge>* metricResponseTime = nullptr;
	prometheus::Family<prometheus::Gauge>* metricResponseTimeMax = nullptr;
	prometheus::Family<prometheus::Gauge>* metricResponseTimeJitter = nullptr;
	prometheus::Family<prometheus::Gauge>* metricUnackedAge = nullptr;
	prometheus::Family<prometheus::Gauge>* metricIncomingBwUsage = nullptr;

	// server-wide aggregates, exported whether or not per-player metrics are on
	prometheus::Counter* metricTotalSentBytes = nullptr;
	prometheus::Counter* metricTotalRecvBytes = nullptr;
	prometheus::Counter* metricTotalSentPackets = nullptr;
	prometheus::Counter* metricTotalRecvPackets = nullptr;
	/// three children of one {direction, socket}-labelled family
	prometheus::Counter* metricTotalSendErrors = nullptr;
	prometheus::Counter* metricTotalRecvErrors = nullptr;
	prometheus::Counter* metricListenerRecvErrors = nullptr;
	prometheus::Counter* metricTotalResentChunks = nullptr;
	prometheus::Counter* metricTotalRedundantChunks = nullptr;
	prometheus::Counter* metricTotalDroppedChunks = nullptr;
	prometheus::Counter* metricTotalLostIncomingChunks = nullptr;
	// connection lifecycle funnel. Plural connections_* throughout: the singular
	// recoil_network_connection_ prefix is reserved for the per-playerid series.
	prometheus::Counter* metricConnAttempted = nullptr;
	prometheus::Family<prometheus::Counter>* metricConnRejected = nullptr;
	prometheus::Family<prometheus::Counter>* metricConnEstablished = nullptr;
	prometheus::Family<prometheus::Counter>* metricConnClosed = nullptr;
	prometheus::Counter* metricTotalThrottledPackets = nullptr;
	prometheus::Counter* metricTotalIncomingThrottled = nullptr;
	prometheus::Counter* metricTotalOutgoingThrottled = nullptr;
	prometheus::Counter* metricTotalReorderStall = nullptr;
	prometheus::Gauge* metricRedundancyLinks = nullptr;
	prometheus::Gauge* metricTotalOutgoingBw = nullptr;
	prometheus::Gauge* metricTotalUnackedChunks = nullptr;
	prometheus::Gauge* metricTotalResendQueueDepth = nullptr;
	prometheus::Gauge* metricTotalReorderQueueDepth = nullptr;
	prometheus::Gauge* metricTotalSendQueueBytes = nullptr;

	prometheus::Gauge* metricMaxResponseTime = nullptr;
	prometheus::Gauge* metricMaxResponseTimeJitter = nullptr;
	prometheus::Gauge* metricMaxUnackedAge = nullptr;
	prometheus::Gauge* metricMaxIncomingBwUsage = nullptr;
	/// one entry per bucket for ObserveMultiple; reused across connections
	std::vector<double> histogramIncrements;
	/// aggregate only: a per-player histogram would multiply series by ~12
	prometheus::Histogram* metricResponseTimeHist = nullptr;

	unsigned int lastListenerRecvErrors = 0;
};
