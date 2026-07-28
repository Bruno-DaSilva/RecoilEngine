/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <string>

#include "NetworkMetrics.h"
#include "ServerHealthMetrics.h"
#include "System/Misc/SpringTime.h"

class CGameServer;

/**
 * @brief every prometheus series the game server exports
 *
 * A facade over the metric groups: it owns the endpoint's lifetime and the
 * publish interval, and forwards everything else. Every method no-ops when the
 * subsystem is disabled (the default), so callers never test for it.
 *
 * Entry points are called from both the netcode and the game thread, but always
 * under CGameServer::gameServerMutex.
 */
class ServerMetrics
{
public:
	/**
	 * @brief register every family
	 *
	 * @param gameIDHex identity label for recoil_server_info; must be known
	 *   before this runs.
	 */
	void Init(const std::string& gameIDHex);

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

	// connection lifecycle funnel
	void CountConnectionAttempt();
	void CountConnectionRejected(const char* reason);
	void CountConnectionEstablished(bool reconnect);
	void CountConnectionClosed(const char* reason);

	/// @return whether CountPlayerDesync() is worth calling for the players in it
	bool CountDesyncEvent();
	void CountPlayerDesync(int playerId);

	void CountThrottledPackets(int playerId, int numPackets);
	/// takes milliseconds; scaled to prometheus base units downstream
	void CountIncomingThrottled(int playerId, double milliSecs);

	void SetGameStartTime(double unixSecs);

	void ResetConnectionDeltas(int playerId);

private:
	NetworkMetrics network;
	ServerHealthMetrics health;

	/// null families until Init() has registered against an enabled registry
	bool registered = false;

	/// when Update() last republished, against the server's own update clock
	spring_time lastPublishTime = spring_notime;
};
