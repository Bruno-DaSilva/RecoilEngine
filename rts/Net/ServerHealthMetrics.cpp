/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "ServerHealthMetrics.h"

#include <prometheus/registry.h>

#include "GameServer.h"


void ServerHealthMetrics::Init(prometheus::Registry&)
{
}


void ServerHealthMetrics::Update(const CGameServer&)
{
}
