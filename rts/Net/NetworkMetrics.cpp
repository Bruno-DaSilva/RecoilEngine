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
		publishDelta(metricTotalSendErrors, cm.sendErrors, stats.sendErrors);
		publishDelta(metricTotalRecvErrors, cm.recvErrors, stats.receiveErrors);

		totalOutgoingBw += stats.sendRateBytesPerSec;
		totalUnackedChunks += stats.unackedChunks;
		totalResendQueueDepth += stats.queuedResendChunks;
		totalReorderQueueDepth += stats.queuedInboundChunks;
		totalSendQueueBytes += stats.queuedSendBytes;

		if (cm.outgoingBw != nullptr) {
			cm.outgoingBw->Set(stats.sendRateBytesPerSec);
			cm.unackedChunks->Set(stats.unackedChunks);
			cm.resendQueueDepth->Set(stats.queuedResendChunks);
			cm.reorderQueueDepth->Set(stats.queuedInboundChunks);
			cm.sendQueueBytes->Set(stats.queuedSendBytes);
		}
	}

	metricTotalOutgoingBw->Set(totalOutgoingBw);
	metricTotalUnackedChunks->Set(totalUnackedChunks);
	metricTotalResendQueueDepth->Set(totalResendQueueDepth);
	metricTotalReorderQueueDepth->Set(totalReorderQueueDepth);
	metricTotalSendQueueBytes->Set(totalSendQueueBytes);

	if (server.udpListener != nullptr)
		metricListenerRecvErrors->Increment(DeltaSince(server.udpListener->GetReceiveErrors(), lastListenerRecvErrors));
}
