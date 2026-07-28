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
#include "Protocol/NetMessageTypes.h"
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
	metricTotalSentPackets = counter("recoil_network_sent_packets_total",
		"UDP packets sent over all client connections");
	metricTotalRecvPackets = counter("recoil_network_received_packets_total",
		"UDP packets received over all client connections");
	// the listening socket only receives, so there is no {send,listener} child
	auto& socketErrors = *counterFamily("recoil_network_socket_errors_total",
		"Socket-level failures, by direction and socket. Ours or the host environment's, not a peer's link");
	metricTotalSendErrors    = &socketErrors.Add({{"direction", "send"},    {"socket", "connection"}});
	metricTotalRecvErrors    = &socketErrors.Add({{"direction", "receive"}, {"socket", "connection"}});
	metricListenerRecvErrors = &socketErrors.Add({{"direction", "receive"}, {"socket", "listener"}});

	metricTotalResentChunks = counter("recoil_network_resent_chunks_total",
		"Chunks retransmitted to clients because they looked lost (nak or ack timeout), excluding redundancy-mode duplication. Counts retransmission events, not distinct chunks, so it is inflated on links with a non-zero loss factor");
	metricTotalRedundantChunks = counter("recoil_network_redundant_chunks_total",
		"Chunks retransmitted purely because the link duplicates by policy; the bandwidth cost of redundancy mode, not a loss symptom");
	metricTotalDroppedChunks = counter("recoil_network_duplicate_chunks_received_total",
		"Chunks discarded on arrival because the same chunk had already been received");
	metricTotalLostIncomingChunks = counter("recoil_network_lost_incoming_chunks_total",
		"Chunks from clients observed missing at a send pass. Reordering that outlives a pass counts here as well as real loss");
	// ObserveMultiple throws length_error on a size mismatch
	static_assert(netcode::responseTimeNumBuckets == netcode::responseTimeNumBucketBounds + 1,
		"one increment per bucket, i.e. one more than the bounds");
	histogramIncrements.assign(netcode::responseTimeNumBuckets, 0.0);

	metricMaxResponseTime = gauge("recoil_network_max_response_time_seconds",
		"Worst smoothed send->ack time across client connections; latency plus client processing, not pure RTT. Retransmitted chunks contribute no sample, so this stays a latency signal and does not rise with loss -- see resent_chunks_total for that");
	metricMaxUnackedAge = gauge("recoil_network_max_unacked_age_seconds",
		"Longest an already-sent chunk has waited for an ack, across client connections; sustained growth means a stalled link rather than a slow one");
	metricMaxResponseTimeJitter = gauge("recoil_network_max_response_time_jitter_seconds",
		"Worst response-time mean deviation across client connections; how unsteady the links are rather than how slow");
	{
		// Scaled to seconds here rather than in the netcode, which bins against
		// the millisecond bounds it measures in; the scaling is monotone, so
		// only the labels move and the bucket count stays tied to the source.
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

	metricMessageBytes = counterFamily("recoil_network_message_bytes_total",
		"Message payload bytes by NETMSG type and direction, unicast and broadcast alike (counted once per recipient). Payload only, so it does not sum to sent_bytes_total / received_bytes_total");
	metricTotalThrottledPackets = counter("recoil_network_throttle_dropped_packets_total",
		"Incoming packets dropped because a client exceeded the waiting-packet limit");
	metricTotalIncomingThrottled = counter("recoil_network_incoming_throttled_seconds_total",
		"Time spent delaying clients' incoming packets at the incoming bandwidth limit, summed over players. Near zero while the limiter accumulator is never written back");
	metricConnAttempted = counter("recoil_network_connections_attempted_total",
		"Incoming connection attempts including invalid ones, plus the host's own local client; denominator of the connections_* funnel. Counts handshake packets rather than distinct clients, so retries inflate it");
	metricConnRejected = counterFamily("recoil_network_connections_rejected_total",
		"Connection attempts refused, by reason");
	metricConnEstablished = counterFamily("recoil_network_connections_established_total",
		"Connections bound to a player slot. reconnect=1 is a resumed session, either an existing link retargeted or a fresh link plus packetCache replay; reconnect=0 is a slot filled for the first time");
	metricConnClosed = counterFamily("recoil_network_connections_closed_total",
		"Connections the server tore down, by reason. Links still open when the game ends are not counted, so attempted - rejected - closed trends to the participant count rather than to zero");

	metricMaxIncomingBwUsage = gauge("recoil_network_max_incoming_bandwidth_usage",
		"Highest incoming-limiter accumulator across connections. Pinned at zero while ServerReadNet never writes the accumulator back; if it ever goes non-zero the limiter has been repaired");

	gauge("recoil_network_incoming_peak_bandwidth_limit",
		"Per-connection incoming peak bandwidth cap in limiter units (0 = unlimited)")->Set(globalConfig.linkIncomingPeakBandwidth);
	gauge("recoil_network_incoming_sustained_bandwidth_limit",
		"Per-connection incoming sustained bandwidth cap in limiter units (0 = unlimited)")->Set(globalConfig.linkIncomingSustainedBandwidth);
	gauge("recoil_network_incoming_max_waiting_packets_limit",
		"Per-connection cap on queued incoming packets before they are dropped (0 = unlimited)")->Set(globalConfig.linkIncomingMaxWaitingPackets);

	metricTotalOutgoingThrottled = counter("recoil_network_outgoing_throttled_seconds_total",
		"Time sending to clients was blocked by the outgoing bandwidth cap while data was queued, summed over connections");
	metricTotalReorderStall = counter("recoil_network_reorder_stall_seconds_total",
		"Time inbound delivery was stalled behind a missing chunk, summed over connections. Accumulated at loop rate, so it cannot miss a stall that opens and closes between two scrapes");
	metricRedundancyLinks = gauge("recoil_network_redundancy_mode_connections",
		"Live connections with a non-zero loss factor, i.e. running in proactive-retransmit mode");
	metricTotalOutgoingBw = gauge("recoil_network_outgoing_bandwidth_bytes_per_second",
		"Rolling average of the send rate over all client connections");
	metricTotalUnackedChunks = gauge("recoil_network_unacked_chunks",
		"Chunks sent to clients and not yet acked, summed over all connections");
	metricTotalResendQueueDepth = gauge("recoil_network_resend_queue_depth",
		"Chunks queued for retransmission, summed over all connections; growth without draining is a resend flood");
	metricTotalReorderQueueDepth = gauge("recoil_network_reorder_queue_depth",
		"Chunks from clients held in the reorder buffer behind a missing chunk, summed over all connections. Fills on benign reordering as well as on real loss");
	metricTotalSendQueueBytes = gauge("recoil_network_send_queue_bytes",
		"Application bytes queued for clients and not yet transmitted, summed over all connections");

	gauge("recoil_network_outgoing_bandwidth_limit_bytes_per_second",
		"Per-connection outgoing bandwidth cap (0 = unlimited)")->Set(globalConfig.linkOutgoingBandwidth);

	if (!metrics::PerPlayerEnabled())
		return;

	metricSentBytes = counterFamily("recoil_network_connection_sent_bytes_total",
		"Bytes sent over this client connection. No series exists for a listen-server host's own slot, which has no network link");
	metricRecvBytes = counterFamily("recoil_network_connection_received_bytes_total",
		"Bytes received over this client connection");
	metricSentPackets = counterFamily("recoil_network_connection_sent_packets_total",
		"UDP packets sent over this client connection");
	metricRecvPackets = counterFamily("recoil_network_connection_received_packets_total",
		"UDP packets received over this client connection");
	metricSocketErrors = counterFamily("recoil_network_connection_socket_errors_total",
		"Socket failures on this link by direction");
	metricResentChunks = counterFamily("recoil_network_connection_resent_chunks_total",
		"Chunks retransmitted to this client because they looked lost, excluding redundancy-mode duplication; see the aggregate resent_chunks_total");
	metricRedundantChunks = counterFamily("recoil_network_connection_redundant_chunks_total",
		"Chunks retransmitted to this client purely because the link duplicates by policy");
	metricDroppedChunks = counterFamily("recoil_network_connection_duplicate_chunks_received_total",
		"Chunks from this client discarded on arrival because the same chunk had already been received");
	metricLostIncomingChunks = counterFamily("recoil_network_connection_lost_incoming_chunks_total",
		"Chunks from this client observed missing at a send pass; long-lived reordering counts here as well as real loss");
	metricIncomingBwUsage = gaugeFamily("recoil_network_connection_incoming_bandwidth_usage",
		"Incoming-limiter accumulator for this link, in the same units as the incoming bandwidth caps");
	metricThrottledPackets = counterFamily("recoil_network_connection_throttle_dropped_packets_total",
		"Incoming packets dropped because the client exceeded the waiting-packet limit");
	metricIncomingThrottled = counterFamily("recoil_network_connection_incoming_throttled_seconds_total",
		"Time spent delaying this client's incoming packets at the incoming bandwidth limit; near zero, see the aggregate");
	metricOutgoingThrottled = counterFamily("recoil_network_connection_outgoing_throttled_seconds_total",
		"Time sending to this client was blocked by the outgoing bandwidth cap while data was queued");
	metricReorderStall = counterFamily("recoil_network_connection_reorder_stall_seconds_total",
		"Time inbound delivery from this client was stalled behind a missing chunk; rate() is the fraction of the interval this player's input was blocked");
	metricLossFactor = gaugeFamily("recoil_network_connection_loss_factor",
		"Client-declared network loss factor for this link (0 = normal). Above zero the link duplicates chunks by policy; that lands in redundant_chunks_total, not resent_chunks_total");
	metricOutgoingBw = gaugeFamily("recoil_network_connection_outgoing_bandwidth_bytes_per_second",
		"Rolling average of the send rate to this client, as used by outgoing bandwidth limiting");
	metricUnackedChunks = gaugeFamily("recoil_network_connection_unacked_chunks",
		"Chunks sent to this client and not yet acked");
	metricResendQueueDepth = gaugeFamily("recoil_network_connection_resend_queue_depth",
		"Chunks queued for retransmission to this client");
	metricReorderQueueDepth = gaugeFamily("recoil_network_connection_reorder_queue_depth",
		"Chunks from this client held in the reorder buffer behind a missing chunk");
	metricSendQueueBytes = gaugeFamily("recoil_network_connection_send_queue_bytes",
		"Application bytes queued for this client and not yet transmitted");
	metricResponseTime = gaugeFamily("recoil_network_connection_response_time_seconds",
		"Smoothed send->ack time for this client; latency plus client processing, not pure RTT. Retransmitted chunks contribute no sample, so loss shows up in this link's resent_chunks_total rather than here");
	metricResponseTimeMax = gaugeFamily("recoil_network_connection_response_time_spike_seconds",
		"Worst single send->ack sample for this client over the trailing window");
	metricResponseTimeJitter = gaugeFamily("recoil_network_connection_response_time_jitter_seconds",
		"Mean deviation of this client's send->ack samples; how unsteady the link is rather than how slow");
	metricUnackedAge = gaugeFamily("recoil_network_connection_unacked_age_seconds",
		"How long this client's oldest un-acked chunk has waited; 0 when nothing is outstanding. Rising while response_time holds steady means the link stalled, not slowed");
}


void NetworkMetrics::ReleaseConnectionGauges(ConnectionMetrics& cm)
{
	const auto release = [](prometheus::Family<prometheus::Gauge>* family, prometheus::Gauge*& child) {
		if (child == nullptr)
			return;

		family->Remove(child);
		child = nullptr;
	};

	release(metricLossFactor, cm.lossFactor);
	release(metricOutgoingBw, cm.outgoingBw);
	release(metricUnackedChunks, cm.unackedChunks);
	release(metricResendQueueDepth, cm.resendQueueDepth);
	release(metricReorderQueueDepth, cm.reorderQueueDepth);
	release(metricSendQueueBytes, cm.sendQueueBytes);
	release(metricResponseTime, cm.responseTime);
	release(metricResponseTimeMax, cm.responseTimeMax);
	release(metricResponseTimeJitter, cm.responseTimeJitter);
	release(metricUnackedAge, cm.unackedAge);
	release(metricIncomingBwUsage, cm.incomingBandwidthUsage);
}


void NetworkMetrics::CountMessageBytes(bool outgoing, unsigned char msgId, unsigned int bytes)
{
	if (metricMessageBytes == nullptr)
		return;

	prometheus::Counter*& counter = messageBytesCounters[outgoing][msgId];

	if (counter == nullptr) {
		counter = &metricMessageBytes->Add({
			{"direction", outgoing ? "send" : "receive"},
			{"type", NetMessageName(msgId)}
		});
	}

	counter->Increment(bytes);
}


void NetworkMetrics::CountConnectionAttempt()
{
	if (metricConnAttempted != nullptr)
		metricConnAttempted->Increment();
}

void NetworkMetrics::CountConnectionRejected(const char* reason)
{
	CountEvent(metricConnRejected, "reason", reason);
}

void NetworkMetrics::CountConnectionEstablished(bool reconnect)
{
	CountEvent(metricConnEstablished, "reconnect", reconnect ? "1" : "0");
}

void NetworkMetrics::CountConnectionClosed(const char* reason)
{
	CountEvent(metricConnClosed, "reason", reason);
}


// the aggregate counter is null exactly when metrics are off; testing it before
// AtGrowing keeps the Count* below from growing a vector nobody reads
void NetworkMetrics::CountThrottledPackets(int playerId, int numPackets)
{
	if (metricTotalThrottledPackets == nullptr)
		return;

	AddPlayerMetric(metricTotalThrottledPackets, metricThrottledPackets,
		AtGrowing(connectionMetrics, playerId).throttledPackets, playerId, numPackets);
}

void NetworkMetrics::CountIncomingThrottled(int playerId, double milliSecs)
{
	if (metricTotalIncomingThrottled == nullptr)
		return;

	AddPlayerMetric(metricTotalIncomingThrottled, metricIncomingThrottled,
		AtGrowing(connectionMetrics, playerId).incomingThrottled, playerId, milliSecs * msToSecs);
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

		if (dc.player != nullptr)
			dc.player->Increment(delta);
	};

	for (const GameParticipant& p: server.players) {
		ConnectionMetrics& cm = AtGrowing(connectionMetrics, p.id);

		if (p.clientLink == nullptr) {
			// dropped from the registry, or they go on being scraped
			ReleaseConnectionGauges(cm);
			cm = ConnectionMetrics{};
			continue;
		}

		const netcode::ConnectionStats stats = p.clientLink->GetStats();

		// a loopback is not a network
		if (!stats.isNetworkLink)
			continue;

		// Label by slot id: stable within a game, and not PII the way names are.
		// sentBytes stands in for the whole group -- they are resolved together.
		if (metricSentBytes != nullptr && cm.sentBytes.player == nullptr) {
			const std::string playerIdStr = std::to_string(p.id);
			const std::map<std::string, std::string> labels = {{"playerid", playerIdStr}};
			cm.sentBytes.player   = &metricSentBytes->Add(labels);
			cm.recvBytes.player   = &metricRecvBytes->Add(labels);
			cm.sentPackets.player = &metricSentPackets->Add(labels);
			cm.recvPackets.player = &metricRecvPackets->Add(labels);
			cm.sendErrors.player  = &metricSocketErrors->Add({{"playerid", playerIdStr}, {"direction", "send"}});
			cm.recvErrors.player  = &metricSocketErrors->Add({{"playerid", playerIdStr}, {"direction", "receive"}});
			cm.resentChunks.player   = &metricResentChunks->Add(labels);
			cm.redundantChunks.player = &metricRedundantChunks->Add(labels);
			cm.droppedChunks.player  = &metricDroppedChunks->Add(labels);
			cm.lostIncomingChunks.player = &metricLostIncomingChunks->Add(labels);
			cm.outgoingThrottled.player = &metricOutgoingThrottled->Add(labels);
			cm.reorderStall.player = &metricReorderStall->Add(labels);
			cm.lossFactor         = &metricLossFactor->Add(labels);
			cm.outgoingBw         = &metricOutgoingBw->Add(labels);
			cm.unackedChunks      = &metricUnackedChunks->Add(labels);
			cm.resendQueueDepth   = &metricResendQueueDepth->Add(labels);
			cm.reorderQueueDepth  = &metricReorderQueueDepth->Add(labels);
			cm.sendQueueBytes     = &metricSendQueueBytes->Add(labels);
			cm.unackedAge         = &metricUnackedAge->Add(labels);
			cm.incomingBandwidthUsage = &metricIncomingBwUsage->Add(labels);
		}

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
		publishDelta(metricTotalRedundantChunks, cm.redundantChunks, stats.duplicatedChunks);
		publishDelta(metricTotalDroppedChunks, cm.droppedChunks, stats.discardedChunks);
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

		// only once measured: an absent series says "unknown" where a zero would
		// claim a perfect link
		if (stats.hasResponseSample) {
			maxResponseTime = std::max(maxResponseTime, stats.responseTimeMs);
			maxResponseTimeJitter = std::max(maxResponseTimeJitter, stats.responseTimeJitterMs);

			if (metricResponseTime != nullptr) {
				if (cm.responseTime == nullptr) {
					const std::map<std::string, std::string> labels = {{"playerid", std::to_string(p.id)}};

					cm.responseTime = &metricResponseTime->Add(labels);
					cm.responseTimeMax = &metricResponseTimeMax->Add(labels);
					cm.responseTimeJitter = &metricResponseTimeJitter->Add(labels);
				}

				cm.responseTime->Set(stats.responseTimeMs * msToSecs);
				cm.responseTimeMax->Set(stats.responseTimePeakMs * msToSecs);
				cm.responseTimeJitter->Set(stats.responseTimeJitterMs * msToSecs);
			}
		}

		if (cm.outgoingBw != nullptr) {
			cm.lossFactor->Set(stats.lossFactor);
			cm.outgoingBw->Set(stats.sendRateBytesPerSec);
			cm.unackedChunks->Set(stats.unackedChunks);
			cm.resendQueueDepth->Set(stats.queuedResendChunks);
			cm.reorderQueueDepth->Set(stats.queuedInboundChunks);
			cm.sendQueueBytes->Set(stats.queuedSendBytes);
			cm.unackedAge->Set(stats.oldestUnackedMs * msToSecs);
			cm.incomingBandwidthUsage->Set(maxLinkBwUsage);
		}
	}

	metricRedundancyLinks->Set(numRedundancyLinks);
	metricMaxResponseTime->Set(maxResponseTime * msToSecs);
	metricMaxResponseTimeJitter->Set(maxResponseTimeJitter * msToSecs);
	metricMaxUnackedAge->Set(maxUnackedAge * msToSecs);
	metricMaxIncomingBwUsage->Set(maxIncomingBwUsage);
	metricTotalOutgoingBw->Set(totalOutgoingBw);
	metricTotalUnackedChunks->Set(totalUnackedChunks);
	metricTotalResendQueueDepth->Set(totalResendQueueDepth);
	metricTotalReorderQueueDepth->Set(totalReorderQueueDepth);
	metricTotalSendQueueBytes->Set(totalSendQueueBytes);

	if (server.udpListener != nullptr)
		metricListenerRecvErrors->Increment(DeltaSince(server.udpListener->GetReceiveErrors(), lastListenerRecvErrors));
}
