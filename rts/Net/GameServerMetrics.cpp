/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GameServerMetrics.h"

#include "GameServer.h"
#include "System/Metrics/Metrics.h"
#include "System/Net/Connection.h"

/// how often metric values are republished; well below any sane scrape interval
static const spring_time metricsUpdateTime = spring_secs(1);


void ServerMetrics::Init(const std::string& gameIDHex)
{
	metrics::Init();

	netcode::CConnection::SetStatsSampling(metrics::Enabled());

	if (!metrics::Enabled())
		return;

	auto& registry = metrics::GetRegistry();

	network.Init(registry);
	health.Init(registry, gameIDHex);
	registered = true;
}


void ServerMetrics::Shutdown()
{
	// nothing else clears this: a client that hosted a game and then joins a
	// remote one runs no server, so its link would sample forever
	netcode::CConnection::SetStatsSampling(false);

	metrics::Shutdown();

	// the registry is gone, so every metric pointer cached in this object now
	// dangles. Assigning a default-constructed instance clears all of them at
	// once, and goes on doing so as fields are added.
	*this = ServerMetrics{};
}


void ServerMetrics::Update(const CGameServer& server)
{
	if (!registered)
		return;

	if (lastPublishTime > (server.lastUpdate - metricsUpdateTime))
		return;

	lastPublishTime = server.lastUpdate;

	network.Update(server);
	health.Update(server);
}

void ServerMetrics::SetGameStartTime(double unixSecs)       { health.SetGameStartTime(unixSecs); }
void ServerMetrics::ResetConnectionDeltas(int playerId)     { network.ResetConnectionDeltas(playerId); }

void ServerMetrics::CountConnectionAttempt()                    { network.CountConnectionAttempt(); }
void ServerMetrics::CountConnectionRejected(const char* reason)  { network.CountConnectionRejected(reason); }
void ServerMetrics::CountConnectionEstablished(bool reconnect)   { network.CountConnectionEstablished(reconnect); }
void ServerMetrics::CountConnectionClosed(const char* reason)    { network.CountConnectionClosed(reason); }

bool ServerMetrics::CountDesyncEvent()              { return health.CountDesyncEvent(); }
void ServerMetrics::CountPlayerDesync(int playerId) { health.CountPlayerDesync(playerId); }

void ServerMetrics::CountThrottledPackets(int playerId, int numPackets)
{
	network.CountThrottledPackets(playerId, numPackets);
}

void ServerMetrics::CountIncomingThrottled(int playerId, double milliSecs)
{
	network.CountIncomingThrottled(playerId, milliSecs);
}
