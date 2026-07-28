/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/Misc/SpringTime.h"

class CGameServer;

/**
 * @brief every prometheus series the game server exports
 *
 * Owns the endpoint's lifetime and the publish interval. Every method no-ops
 * when the subsystem is disabled (the default), so callers never test for it.
 *
 * Entry points are called from both the netcode and the game thread, but always
 * under CGameServer::gameServerMutex.
 */
class ServerMetrics
{
public:
	/// register every family
	void Init();

	/**
	 * @brief stop the endpoint and drop the registry
	 *
	 * The registry outlives any one CGameServer, so a finished game's series
	 * would otherwise go on being scraped. Invalidates every metric pointer
	 * held here, so the netcode thread must be joined first.
	 */
	void Shutdown();

	/// republish every value; rate-limited internally, cheap to call per loop
	void Update(const CGameServer& server);

private:
	/// null families until Init() has registered against an enabled registry
	bool registered = false;

	/// when Update() last republished, against the server's own update clock
	spring_time lastPublishTime = spring_notime;
};
