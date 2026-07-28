/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GameServerMetrics.h"

#include "GameServer.h"
#include "System/Metrics/Metrics.h"

/// how often metric values are republished; well below any sane scrape interval
static const spring_time metricsUpdateTime = spring_secs(1);


void ServerMetrics::Init()
{
	metrics::Init();

	if (!metrics::Enabled())
		return;

	registered = true;
}


void ServerMetrics::Shutdown()
{
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
}
