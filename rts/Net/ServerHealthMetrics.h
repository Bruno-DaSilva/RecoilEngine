/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace prometheus
{
	class Registry;
}

class CGameServer;

/**
 * @brief the recoil_server_ series: frame progress, speed, participants
 *
 * Owned by ServerMetrics, which forwards to it. Participant-level rather than
 * link-level: a listen-server host lags like any other player even though its
 * loopback link has no network to describe.
 */
class ServerHealthMetrics
{
public:
	void Init(prometheus::Registry& registry);
	void Update(const CGameServer& server);
};
