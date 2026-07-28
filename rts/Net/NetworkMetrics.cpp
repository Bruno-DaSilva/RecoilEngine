/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "NetworkMetrics.h"

#include <algorithm>
#include <map>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "GameParticipant.h"
#include "GameServer.h"
#include "MetricsCommon.h"
#include "System/GlobalConfig.h"
#include "System/Metrics/Delta.h"
#include "System/Metrics/Metrics.h"
#include "System/Net/UDPListener.h"

using metrics::AtGrowing;
using metrics::DeltaSince;


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
}


void NetworkMetrics::ResetConnectionDeltas(int playerId)
{
	AtGrowing(connectionMetrics, playerId) = ConnectionMetrics{};
}


void NetworkMetrics::Update(const CGameServer& server)
{
	int numRedundancyLinks = 0;
	float totalOutgoingBw = 0.0f;
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
			cm.lossFactor         = &metricLossFactor->Add(labels);
			cm.outgoingBw         = &metricOutgoingBw->Add(labels);
			cm.unackedChunks      = &metricUnackedChunks->Add(labels);
			cm.resendQueueDepth   = &metricResendQueueDepth->Add(labels);
			cm.reorderQueueDepth  = &metricReorderQueueDepth->Add(labels);
			cm.sendQueueBytes     = &metricSendQueueBytes->Add(labels);
		}

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

		totalOutgoingBw += stats.sendRateBytesPerSec;
		totalUnackedChunks += stats.unackedChunks;
		totalResendQueueDepth += stats.queuedResendChunks;
		totalReorderQueueDepth += stats.queuedInboundChunks;
		totalSendQueueBytes += stats.queuedSendBytes;

		numRedundancyLinks += (stats.lossFactor > 0);

		if (cm.outgoingBw != nullptr) {
			cm.lossFactor->Set(stats.lossFactor);
			cm.outgoingBw->Set(stats.sendRateBytesPerSec);
			cm.unackedChunks->Set(stats.unackedChunks);
			cm.resendQueueDepth->Set(stats.queuedResendChunks);
			cm.reorderQueueDepth->Set(stats.queuedInboundChunks);
			cm.sendQueueBytes->Set(stats.queuedSendBytes);
		}
	}

	metricRedundancyLinks->Set(numRedundancyLinks);
	metricTotalOutgoingBw->Set(totalOutgoingBw);
	metricTotalUnackedChunks->Set(totalUnackedChunks);
	metricTotalResendQueueDepth->Set(totalResendQueueDepth);
	metricTotalReorderQueueDepth->Set(totalReorderQueueDepth);
	metricTotalSendQueueBytes->Set(totalSendQueueBytes);

	if (server.udpListener != nullptr)
		metricListenerRecvErrors->Increment(DeltaSince(server.udpListener->GetReceiveErrors(), lastListenerRecvErrors));
}
