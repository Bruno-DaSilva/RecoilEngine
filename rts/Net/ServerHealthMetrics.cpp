/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "ServerHealthMetrics.h"

#include <algorithm>
#include <map>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include "GameParticipant.h"
#include "GameServer.h"
#include "MetricsCommon.h"
#include "Game/GameVersion.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/Metrics/Metrics.h"

using metrics::AddPlayerMetric;
using metrics::AtGrowing;
using metrics::msToSecs;


void ServerHealthMetrics::Init(prometheus::Registry& registry, const std::string& gameIDHex)
{
	const auto counterFamily = [&](const char* name, const char* help) {
		return &prometheus::BuildCounter().Name(name).Help(help).Register(registry);
	};
	const auto gaugeFamily = [&](const char* name, const char* help) {
		return &prometheus::BuildGauge().Name(name).Help(help).Register(registry);
	};
	const auto counter = [&](const char* name, const char* help) { return &counterFamily(name, help)->Add({}); };
	const auto gauge = [&](const char* name, const char* help) { return &gaugeFamily(name, help)->Add({}); };

	metricServerFrame = gauge("recoil_server_frame",
		"Current server simulation frame");
	metricMaxLag = gauge("recoil_server_max_lag_seconds",
		"Worst player lag: how far the most-behind player's last acked sim frame is behind the server, in game time");
	metricMaxCpu = gauge("recoil_server_max_cpu_usage",
		"Highest client-reported cpu usage in [0,1] across players");
	metricMedianLag = gauge("recoil_server_median_lag_seconds",
		"Median player lag used by lag protection (only computed when SpeedControl=1)");
	metricMedianCpu = gauge("recoil_server_median_cpu_usage",
		"Median client cpu usage used by lag protection (only computed when SpeedControl=1)");

	auto& participants = *gaugeFamily("recoil_server_participants",
		"Connected participants by type. Counts a listen-server host's local client, which the recoil_network_ series do not");
	metricPlayers = &participants.Add({{"type", "player"}});
	metricSpectators = &participants.Add({{"type", "spectator"}});

	metricInternalSpeed = gauge("recoil_server_speed_factor",
		"Speed the simulation is actually running at (1.0 = normal); lowered by lag protection");
	metricUserSpeed = gauge("recoil_server_wanted_speed_factor",
		"Speed requested by the users (1.0 = normal)");
	metricPaused = gauge("recoil_server_paused",
		"1 while the game is paused");

	metricDesyncEvents = counter("recoil_server_desync_events_total",
		"Desyncs detected by sync checking; counted once per detection, not per frame");
	metricTotalPlayerDesyncs = counter("recoil_server_desynced_players_total",
		"Summed count of players whose checksum differed from the reference, over all desyncs");
	metricGameStartTs = gauge("recoil_server_game_start_timestamp_seconds",
		"Unix time the game started; 0 while still in the lobby");

	// The game id is a label here rather than a constant label on every family:
	// prometheus fixes those at registration, and since the registry outlives a
	// CGameServer, re-registering with a new id throws outright.
	gaugeFamily("recoil_server_info", "Constant 1; labels carry engine build and game identity")
		->Add({{"engine_version", SpringVersion::GetFull()}, {"gameid", gameIDHex}}).Set(1);

	if (!metrics::PerPlayerEnabled())
		return;

	metricPlayerLag = gaugeFamily("recoil_server_player_lag_seconds",
		"How far the player's last acked sim frame is behind the server, in game time");
	metricPlayerCpu = gaugeFamily("recoil_server_player_cpu_usage",
		"Client-reported cpu usage in [0,1]");
	metricPlayerDesyncs = counterFamily("recoil_server_player_desyncs_total",
		"Desyncs in which this player had a checksum differing from the reference");
}







bool ServerHealthMetrics::CountDesyncEvent()
{
	if (metricDesyncEvents == nullptr)
		return false;

	metricDesyncEvents->Increment();
	return true;
}

// the aggregate counter is null exactly when metrics are off; testing it before
// AtGrowing keeps this from growing a vector nobody reads
void ServerHealthMetrics::CountPlayerDesync(int playerId)
{
	if (metricTotalPlayerDesyncs == nullptr)
		return;

	AddPlayerMetric(metricTotalPlayerDesyncs, metricPlayerDesyncs,
		AtGrowing(playerMetrics, playerId).desyncs, playerId, 1);
}


void ServerHealthMetrics::SetGameStartTime(double unixSecs)
{
	if (metricGameStartTs != nullptr)
		metricGameStartTs->Set(unixSecs);
}


void ServerHealthMetrics::Update(const CGameServer& server)
{
	int numPlayers = 0;
	int numSpectators = 0;
	float maxLag = 0.0f;
	float maxCpu = 0.0f;

	for (const GameParticipant& p: server.players) {
		if (p.clientLink == nullptr)
			continue;

		if (p.spectator)
			numSpectators++;
		else
			numPlayers++;

		PlayerMetrics& pm = AtGrowing(playerMetrics, p.id);

		if (server.gameHasStarted && p.myState == GameParticipant::INGAME) {
			// same lag definition as LagProtection(), including its 0.1 speed floor:
			// a zero internalSpeed would publish an infinity std::max does not catch
			const float simSpeed = std::max(0.1f, server.internalSpeed);
			const float lagMs = std::max(0.0f, ((server.serverFrameNum - p.lastFrameResponse) * 1000.0f) / (GAME_SPEED * simSpeed));
			const float cpu = std::clamp(p.cpuUsage, 0.0f, 1.0f);

			maxLag = std::max(maxLag, lagMs);
			maxCpu = std::max(maxCpu, cpu);

			if (metricPlayerLag != nullptr) {
				if (pm.lagSeconds == nullptr) {
					const std::map<std::string, std::string> labels = {{"playerid", std::to_string(p.id)}};

					pm.lagSeconds = &metricPlayerLag->Add(labels);
					pm.cpuUsage = &metricPlayerCpu->Add(labels);
				}

				pm.lagSeconds->Set(lagMs * msToSecs);
				pm.cpuUsage->Set(cpu);
			}
		} else if (pm.lagSeconds != nullptr) {
			// dropped rather than held: a frozen gauge scrapes like a live one
			metricPlayerLag->Remove(pm.lagSeconds);
			metricPlayerCpu->Remove(pm.cpuUsage);

			pm.lagSeconds = nullptr;
			pm.cpuUsage = nullptr;
		}
	}

	metricMaxLag->Set(maxLag * msToSecs);
	metricMaxCpu->Set(maxCpu);
	metricServerFrame->Set(server.serverFrameNum);
	// medianPing is LagProtection's per-player lag, in milliseconds
	metricMedianLag->Set(server.medianPing * msToSecs);
	metricMedianCpu->Set(server.medianCpu);
	metricPlayers->Set(numPlayers);
	metricSpectators->Set(numSpectators);
	metricInternalSpeed->Set(server.internalSpeed);
	metricUserSpeed->Set(server.userSpeedFactor);
	metricPaused->Set(server.isPaused ? 1 : 0);
}
