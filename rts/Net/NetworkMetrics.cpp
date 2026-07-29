/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "NetworkMetrics.h"

#include <map>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/registry.h>

#include "GameParticipant.h"
#include "GameServer.h"
#include "System/Metrics/Delta.h"
#include "System/Metrics/Metrics.h"

using metrics::DeltaSince;


void NetworkMetrics::Init(prometheus::Registry& registry)
{
	const auto counterFamily = [&](const char* name, const char* help) {
		return &prometheus::BuildCounter().Name(name).Help(help).Register(registry);
	};
	const auto counter = [&](const char* name, const char* help) { return &counterFamily(name, help)->Add({}); };

	// server-wide aggregates: always exported
	metricTotalSentBytes = counter("recoil_network_sent_bytes_total",
		"Bytes sent over all client connections");
	metricTotalRecvBytes = counter("recoil_network_received_bytes_total",
		"Bytes received over all client connections");
	metricTotalSentPackets = counter("recoil_network_sent_packets_total",
		"UDP packets sent over all client connections");
	metricTotalRecvPackets = counter("recoil_network_received_packets_total",
		"UDP packets received over all client connections");

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
}


NetworkMetrics::ConnectionMetrics& NetworkMetrics::ConnectionSlot(int playerId)
{
	if (connectionMetrics.size() <= static_cast<size_t>(playerId))
		connectionMetrics.resize(playerId + 1);

	return connectionMetrics[playerId];
}


void NetworkMetrics::ResetConnectionDeltas(int playerId)
{
	ConnectionSlot(playerId) = ConnectionMetrics{};
}


void NetworkMetrics::Update(const CGameServer& server)
{
	const auto publishDelta = [](
		prometheus::Counter* total, ConnectionMetrics::DeltaCounter& dc, double cur, double scale = 1.0
	) {
		const double delta = DeltaSince(cur, dc.last) * scale;

		total->Increment(delta);

		if (dc.player != nullptr)
			dc.player->Increment(delta);
	};

	for (const GameParticipant& p: server.players) {
		ConnectionMetrics& cm = ConnectionSlot(p.id);

		if (p.clientLink == nullptr) {
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
			const std::map<std::string, std::string> labels = {{"playerid", std::to_string(p.id)}};
			cm.sentBytes.player   = &metricSentBytes->Add(labels);
			cm.recvBytes.player   = &metricRecvBytes->Add(labels);
			cm.sentPackets.player = &metricSentPackets->Add(labels);
			cm.recvPackets.player = &metricRecvPackets->Add(labels);
		}

		publishDelta(metricTotalSentBytes, cm.sentBytes, stats.sentBytes);
		publishDelta(metricTotalRecvBytes, cm.recvBytes, stats.receivedBytes);
		publishDelta(metricTotalSentPackets, cm.sentPackets, stats.sentPackets);
		publishDelta(metricTotalRecvPackets, cm.recvPackets, stats.receivedPackets);
	}
}
