include_directories(${Spring_SOURCE_DIR}/rts)
add_library(engineMetrics STATIC
		"${CMAKE_CURRENT_SOURCE_DIR}/Metrics.cpp"
	)

# PUBLIC so targets linking engineMetrics can use the prometheus API directly
target_link_libraries(engineMetrics PUBLIC prometheus-cpp::core prometheus-cpp::pull)
