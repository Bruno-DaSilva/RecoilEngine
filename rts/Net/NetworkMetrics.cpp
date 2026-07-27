/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "NetworkMetrics.h"

#include <algorithm>
#include <map>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "GameParticipant.h"
#include "GameServer.h"
#include "MetricsCommon.h"
#include "System/GlobalConfig.h"
#include "System/Metrics/Delta.h"
#include "System/Metrics/Metrics.h"
#include "System/Net/UDPListener.h"

using metrics::AddPlayerMetric;
using metrics::AtGrowing;
using metrics::CountEvent;
using metrics::DeltaSince;
using metrics::msToSecs;


void NetworkMetrics::Init(prometheus::Registry& registry)
{
	const auto counterFamily = [&](const char* name, const char* help) {
		return &prometheus::BuildCounter().Name(name).Help(help).Register(registry);
	};
	const auto gaugeFamily = [&](const char* name, const char* help) {
		return &prometheus::BuildGauge().Name(name).Help(help).Register(registry);
	};
	const auto counter = [&](const char* name, const char* help) { return &counterFamily(name, help)->Add({}); };
	const auto gauge = [&](const char* name, const char* help) { return &gaugeFamily(name, help)->Add({}); };

	// server-wide aggregates: always exported
	metricTotalSentBytes = counter("recoil_network_sent_bytes_total",
		"Bytes sent over all client connections");
	metricTotalRecvBytes = counter("recoil_network_received_bytes_total",
		"Bytes received over all client connections");
	metricTotalOutgoingThrottled = counter("recoil_network_outgoing_throttled_seconds_total",
		"Time sending to clients was blocked by the outgoing bandwidth cap while data was queued, summed over connections");
	metricTotalOutgoingBw = gauge("recoil_network_outgoing_bandwidth_bytes_per_second",
		"Rolling average of the send rate over all client connections");
	metricRedundancyLinks = gauge("recoil_network_redundancy_mode_connections",
		"Live connections with a non-zero loss factor, i.e. running in proactive-retransmit mode");

	gauge("recoil_network_outgoing_bandwidth_limit_bytes_per_second",
		"Per-connection outgoing bandwidth cap (0 = unlimited)")->Set(globalConfig.linkOutgoingBandwidth);
	gauge("recoil_network_incoming_peak_bandwidth_limit",
		"Per-connection incoming peak bandwidth cap in limiter units (0 = unlimited)")->Set(globalConfig.linkIncomingPeakBandwidth);
	gauge("recoil_network_incoming_sustained_bandwidth_limit",
		"Per-connection incoming sustained bandwidth cap in limiter units (0 = unlimited)")->Set(globalConfig.linkIncomingSustainedBandwidth);
	gauge("recoil_network_incoming_max_waiting_packets_limit",
		"Per-connection cap on queued incoming packets before they are dropped (0 = unlimited)")->Set(globalConfig.linkIncomingMaxWaitingPackets);

	// ObserveMultiple throws length_error on a size mismatch
	static_assert(netcode::responseTimeNumBuckets == netcode::responseTimeNumBucketBounds + 1,
		"one increment per bucket, i.e. one more than the bounds");
	histogramIncrements.assign(netcode::responseTimeNumBuckets, 0.0);

	// the listening socket only receives, so there is no {send,listener} child
	auto& socketErrors = *counterFamily("recoil_network_socket_errors_total",
		"Socket-level failures, by direction and socket. Ours or the host environment's, not a peer's link");
	metricTotalSendErrors    = &socketErrors.Add({{"direction", "send"},    {"socket", "connection"}});
	metricTotalRecvErrors    = &socketErrors.Add({{"direction", "receive"}, {"socket", "connection"}});
	metricListenerRecvErrors = &socketErrors.Add({{"direction", "receive"}, {"socket", "listener"}});
	metricTotalResendQueueDepth = gauge("recoil_network_resend_queue_depth",
		"Chunks queued for retransmission, summed over all connections; growth without draining is a resend flood");
	metricTotalReorderQueueDepth = gauge("recoil_network_reorder_queue_depth",
		"Chunks from clients held in the reorder buffer behind a missing chunk, summed over all connections. Fills on benign reordering as well as on real loss");
	metricTotalReorderStall = counter("recoil_network_reorder_stall_seconds_total",
		"Time inbound delivery was stalled behind a missing chunk, summed over connections. Accumulated at loop rate, so it cannot miss a stall that opens and closes between two scrapes");
	metricTotalSendQueueBytes = gauge("recoil_network_send_queue_bytes",
		"Application bytes queued for clients and not yet transmitted, summed over all connections");
	metricMaxIncomingBwUsage = gauge("recoil_network_max_incoming_bandwidth_usage",
		"Highest incoming-limiter accumulator across connections. Pinned at zero while ServerReadNet never writes the accumulator back; if it ever goes non-zero the limiter has been repaired");

	metricTotalSentPackets = counter("recoil_network_sent_packets_total",
		"UDP packets sent over all client connections");
	metricTotalRecvPackets = counter("recoil_network_received_packets_total",
		"UDP packets received over all client connections");
	metricTotalResentChunks = counter("recoil_network_resent_chunks_total",
		"Chunks retransmitted to clients because they looked lost (nak or ack timeout), excluding redundancy-mode duplication. Counts retransmission events, not distinct chunks, so it is inflated on links with a non-zero loss factor");
	metricTotalRedundantChunks = counter("recoil_network_redundant_chunks_total",
		"Chunks retransmitted purely because the link duplicates by policy; the bandwidth cost of redundancy mode, not a loss symptom");
	metricTotalDroppedChunks = counter("recoil_network_duplicate_chunks_received_total",
		"Chunks discarded on arrival because the same chunk had already been received");
	metricTotalLostIncomingChunks = counter("recoil_network_lost_incoming_chunks_total",
		"Chunks from clients observed missing at a send pass. Reordering that outlives a pass counts here as well as real loss");
	metricMaxResponseTime = gauge("recoil_network_max_response_time_seconds",
		"Worst smoothed send->ack time across client connections; latency plus client processing, not pure RTT. Retransmitted chunks contribute no sample, so this stays a latency signal and does not rise with loss -- see resent_chunks_total for that. Measured samples only, so a link that stops acking holds its last value here and climbs in max_unacked_age_seconds instead");
	metricMaxUnackedAge = gauge("recoil_network_max_unacked_age_seconds",
		"Longest an already-sent chunk has waited for an ack, across client connections; sustained growth means a stalled link rather than a slow one");
	metricMaxResponseTimeJitter = gauge("recoil_network_max_response_time_jitter_seconds",
		"Worst response-time mean deviation across client connections; how unsteady the links are rather than how slow");
	{
		prometheus::Histogram::BucketBoundaries bounds;
		bounds.reserve(netcode::responseTimeNumBucketBounds);

		for (const float boundMs: netcode::responseTimeBucketBoundsMs)
			bounds.push_back(boundMs * msToSecs);

		auto& family = prometheus::BuildHistogram()
			.Name("recoil_network_response_time_seconds")
			.Help("Distribution of send->ack times over all client connections; one observation per ack, not per acked chunk, and none at all for an ack whose newest chunk had been retransmitted. A lossy link therefore contributes fewer observations rather than inflated ones")
			.Register(registry);

		metricResponseTimeHist = &family.Add({}, std::move(bounds));
	}

	metricTotalUnackedChunks = gauge("recoil_network_unacked_chunks",
		"Chunks sent to clients and not yet acked, summed over all connections");
}


void NetworkMetrics::ResetConnectionDeltas(int playerId)
{
	AtGrowing(connectionMetrics, playerId) = ConnectionMetrics{};
}


void NetworkMetrics::Update(const CGameServer& server)
{
	int numRedundancyLinks = 0;
	int maxIncomingBwUsage = 0;
	float totalOutgoingBw = 0.0f;
	float maxResponseTime = 0.0f;
	float maxResponseTimeJitter = 0.0f;
	float maxUnackedAge = 0.0f;
	unsigned int totalUnackedChunks = 0;
	unsigned int totalResendQueueDepth = 0;
	unsigned int totalReorderQueueDepth = 0;
	unsigned int totalSendQueueBytes = 0;

	const auto publishDelta = [](
		prometheus::Counter* total, ConnectionMetrics::DeltaCounter& dc, double cur, double scale = 1.0
	) {
		const double delta = DeltaSince(cur, dc.last) * scale;

		total->Increment(delta);
	};

	for (const GameParticipant& p: server.players) {
		ConnectionMetrics& cm = AtGrowing(connectionMetrics, p.id);

		if (p.clientLink == nullptr) {
			cm = ConnectionMetrics{};
			continue;
		}

		const netcode::ConnectionStats stats = p.clientLink->GetStats();

		// a loopback is not a network
		if (!stats.isNetworkLink)
			continue;

		// always zero while ServerReadNet never writes the accumulator back
		int maxLinkBwUsage = 0;

		for (const auto& pair: p.aiClientLinks)
			maxLinkBwUsage = std::max(maxLinkBwUsage, pair.second.bandwidthUsage);

		maxIncomingBwUsage = std::max(maxIncomingBwUsage, maxLinkBwUsage);

		publishDelta(metricTotalSentBytes, cm.sentBytes, stats.sentBytes);
		publishDelta(metricTotalRecvBytes, cm.recvBytes, stats.receivedBytes);
		publishDelta(metricTotalSentPackets, cm.sentPackets, stats.sentPackets);
		publishDelta(metricTotalRecvPackets, cm.recvPackets, stats.receivedPackets);
		publishDelta(metricTotalResentChunks, cm.resentChunks, stats.retransmittedChunks);
		publishDelta(metricTotalDroppedChunks, cm.droppedChunks, stats.discardedChunks);
		publishDelta(metricTotalRedundantChunks, cm.redundantChunks, stats.duplicatedChunks);
		publishDelta(metricTotalLostIncomingChunks, cm.lostIncomingChunks, stats.missingChunks);
		publishDelta(metricTotalSendErrors, cm.sendErrors, stats.sendErrors);
		publishDelta(metricTotalRecvErrors, cm.recvErrors, stats.receiveErrors);
		publishDelta(metricTotalOutgoingThrottled, cm.outgoingThrottled, stats.sendBlockedMs, msToSecs);
		publishDelta(metricTotalReorderStall, cm.reorderStall, stats.receiveStalledMs, msToSecs);

		totalOutgoingBw += stats.sendRateBytesPerSec;
		totalUnackedChunks += stats.unackedChunks;
		totalResendQueueDepth += stats.queuedResendChunks;
		totalReorderQueueDepth += stats.queuedInboundChunks;
		totalSendQueueBytes += stats.queuedSendBytes;

		numRedundancyLinks += (stats.lossFactor > 0);

		maxUnackedAge = std::max(maxUnackedAge, stats.oldestUnackedMs);

		if (metricResponseTimeHist != nullptr) {
			bool anySamples = false;

			for (unsigned b = 0; b < netcode::responseTimeNumBuckets; b++) {
				const unsigned int delta = DeltaSince(stats.responseTimeBuckets[b], cm.lastResponseTimeBuckets[b]);

				histogramIncrements[b] = delta;
				anySamples |= (delta > 0);
			}

			// bucket counts are scale-free; only the sum follows the bounds
			// into seconds
			const double sumDelta = DeltaSince(stats.responseTimeSumMs, cm.lastResponseTimeSumMs) * msToSecs;

			if (anySamples)
				metricResponseTimeHist->ObserveMultiple(histogramIncrements, sumDelta);
		}

		if (stats.hasResponseSample) {
			maxResponseTime = std::max(maxResponseTime, stats.responseTimeMs);
			maxResponseTimeJitter = std::max(maxResponseTimeJitter, stats.responseTimeJitterMs);
		}
	}

	metricTotalOutgoingBw->Set(totalOutgoingBw);
	metricRedundancyLinks->Set(numRedundancyLinks);
	metricMaxResponseTime->Set(maxResponseTime * msToSecs);
	metricMaxResponseTimeJitter->Set(maxResponseTimeJitter * msToSecs);
	metricMaxUnackedAge->Set(maxUnackedAge * msToSecs);
	metricTotalUnackedChunks->Set(totalUnackedChunks);
	metricTotalResendQueueDepth->Set(totalResendQueueDepth);
	metricTotalReorderQueueDepth->Set(totalReorderQueueDepth);
	metricTotalSendQueueBytes->Set(totalSendQueueBytes);
	metricMaxIncomingBwUsage->Set(maxIncomingBwUsage);

	if (server.udpListener != nullptr)
		metricListenerRecvErrors->Increment(DeltaSince(server.udpListener->GetReceiveErrors(), lastListenerRecvErrors));
}
