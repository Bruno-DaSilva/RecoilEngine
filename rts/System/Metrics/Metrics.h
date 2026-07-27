/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace prometheus {
	class Registry;
}

/**
 * @brief the process-wide Prometheus registry and its /metrics endpoint
 *
 * Assumes one game per process. The endpoint's lifetime is the game's, so
 * hosting back-to-back games rebinds the port with no retry -- the rebind can
 * lose a race with TIME_WAIT and leave that game unmonitored.
 */
namespace metrics {
	/**
	 * @brief start the embedded Prometheus /metrics endpoint
	 *
	 * Reads MetricsPort / MetricsBindAddress; an unset port keeps the endpoint
	 * off. Idempotent until the next Shutdown().
	 */
	void Init();

	/// stop serving and drop the registry, invalidating every metric pointer
	/// into it
	void Shutdown();

	/// true iff the /metrics endpoint is being served
	bool Enabled();

	/// true iff per-player breakdowns should be exported alongside the
	/// instance-level aggregates; they multiply the series count by the
	/// number of participants
	bool PerPlayerEnabled();

	/// registry to Register() metric families in; only valid when Enabled()
	prometheus::Registry& GetRegistry();
}
