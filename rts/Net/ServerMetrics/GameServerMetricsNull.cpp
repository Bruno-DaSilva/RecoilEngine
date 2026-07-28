/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
 * ServerMetrics for a build without the endpoint, i.e. ENABLE_METRICS=OFF
 *
 * NOTE: Make sure you define every method the real implementation does, as
 * a missing one breaks this build at link time.
 */

#include <string>

#include "GameServerMetrics.h"

void ServerMetrics::Init(const std::string&) {}

void ServerMetrics::Shutdown() {}

void ServerMetrics::Update(const CGameServer&) {}

void ServerMetrics::SetGameStartTime(double) {}

void ServerMetrics::ResetConnectionDeltas(int) {}

void ServerMetrics::CountConnectionAttempt() {}
void ServerMetrics::CountConnectionRejected(const char*) {}
void ServerMetrics::CountConnectionEstablished(bool) {}
void ServerMetrics::CountConnectionClosed(const char*) {}

/// false, so CheckSync skips the per-player walk over the desync groups
bool ServerMetrics::CountDesyncEvent() { return false; }
void ServerMetrics::CountPlayerDesync(int) {}

void ServerMetrics::CountThrottledPackets(int, int) {}
void ServerMetrics::CountIncomingThrottled(int, double) {}
