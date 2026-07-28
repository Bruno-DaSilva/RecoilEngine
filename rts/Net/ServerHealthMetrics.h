/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <string>
#include <vector>

namespace prometheus
{
	template<typename T> class Family;
	class Gauge;
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
	void Init(prometheus::Registry& registry, const std::string& gameIDHex);
	void Update(const CGameServer& server);

	void SetGameStartTime(double unixSecs);

private:
	struct PlayerMetrics {
		prometheus::Gauge* lagSeconds = nullptr;
		prometheus::Gauge* cpuUsage = nullptr;
	};

	std::vector<PlayerMetrics> playerMetrics;

	prometheus::Gauge* metricServerFrame = nullptr;
	prometheus::Gauge* metricMaxLag = nullptr;
	prometheus::Gauge* metricMaxCpu = nullptr;
	prometheus::Gauge* metricMedianLag = nullptr;
	prometheus::Gauge* metricMedianCpu = nullptr;
	prometheus::Gauge* metricPlayers = nullptr;
	prometheus::Gauge* metricSpectators = nullptr;
	prometheus::Gauge* metricInternalSpeed = nullptr;
	prometheus::Gauge* metricUserSpeed = nullptr;
	prometheus::Gauge* metricPaused = nullptr;
	prometheus::Gauge* metricGameStartTs = nullptr;

	// per-player families; null unless MetricsPerPlayer is on
	prometheus::Family<prometheus::Gauge>* metricPlayerLag = nullptr;
	prometheus::Family<prometheus::Gauge>* metricPlayerCpu = nullptr;
};
