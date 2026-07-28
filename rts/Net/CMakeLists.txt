
# One ServerMetrics class, two implementations, so no call site in GameServer
# has to know which it got; a target that opts out takes the stub and links
# neither prometheus-cpp nor the civetweb HTTP server it vendors. An entry point
# added without a matching stub is a link error rather than a metric that
# silently stops being exported in some configurations.
#
# Kept out of sources_engine_NetServer because the dedicated and client builds
# are gated separately, so consumers of that list pick an implementation.
make_global_var(sources_engine_ServerMetrics
		"${CMAKE_CURRENT_SOURCE_DIR}/GameServerMetrics.cpp"
	)
make_global_var(sources_engine_ServerMetricsNull "${CMAKE_CURRENT_SOURCE_DIR}/GameServerMetricsNull.cpp")

if    (ENABLE_METRICS_CLIENT)
	set(sources_engine_ServerMetricsClient ${sources_engine_ServerMetrics})
else  (ENABLE_METRICS_CLIENT)
	set(sources_engine_ServerMetricsClient ${sources_engine_ServerMetricsNull})
endif (ENABLE_METRICS_CLIENT)

make_global_var(sources_engine_NetServer
		"${CMAKE_CURRENT_SOURCE_DIR}/AutohostInterface.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/GameServer.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/GameParticipant.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/Protocol/BaseNetProtocol.cpp"
	)
set(sources_engine_NetClient
		"${CMAKE_CURRENT_SOURCE_DIR}/Protocol/NetProtocol.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/NetCommands.cpp"
	)

make_global_var(sources_engine_Net
		${sources_engine_NetServer}
		${sources_engine_NetClient}
		${sources_engine_ServerMetricsClient}
	)

