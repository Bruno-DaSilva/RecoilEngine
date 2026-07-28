/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/*
 * ServerMetrics for a target built without the endpoint: spring and
 * spring-headless unless ENABLE_METRICS_CLIENT is on, and the dedicated server
 * under ENABLE_METRICS=OFF. Neither prometheus-cpp nor the civetweb HTTP server
 * it vendors is linked here.
 */

#include <string>

#include "GameServerMetrics.h"

void ServerMetrics::Init(const std::string&) {}

void ServerMetrics::Shutdown() {}

void ServerMetrics::Update(const CGameServer&) {}

void ServerMetrics::SetGameStartTime(double) {}

void ServerMetrics::ResetConnectionDeltas(int) {}
