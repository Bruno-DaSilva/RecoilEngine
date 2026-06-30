# Engine-GL unit tests: render via the REAL engine rendering path and A/B
# against legacy GL. These must NOT be built with -DUNIT_TEST, because the
# engine GL headers (myGL.h) deliberately #error under UNIT_TEST. The render
# path's external globals (globalRendering, configHandler, ...) are provided as
# minimal stubs (GLEngineStubs.cpp) so we don't pull in the window system/VFS.
remove_definitions(-DUNIT_TEST)

if (TARGET SDL2::SDL2 AND TARGET glad)
	set(test_name GLRenderBufferCompare)
	set(test_src
			"${CMAKE_CURRENT_SOURCE_DIR}/testGLRenderBufferCompare.cpp"
			"${CMAKE_CURRENT_SOURCE_DIR}/GLEngineStubs.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/GL/RenderBuffers.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/GL/VAO.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/GL/VBO.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/GL/StreamBuffer.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/GL/VertexArrayTypes.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/Shaders/Shader.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/Shaders/ShaderHandler.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/Shaders/ShaderStates.cpp"
			"${ENGINE_SOURCE_DIR}/Rendering/Shaders/GLSLCopyState.cpp"
			"${ENGINE_SOURCE_DIR}/System/Matrix44f.cpp"
			"${ENGINE_SOURCE_DIR}/System/Quaternion.cpp"
			"${ENGINE_SOURCE_DIR}/System/float3.cpp"
			"${ENGINE_SOURCE_DIR}/System/float4.cpp"
			"${ENGINE_SOURCE_DIR}/System/StringUtil.cpp"
			"${ENGINE_SOURCE_DIR}/System/MemoryOverride.cpp"
			"${ENGINE_SOURCE_DIR}/System/Config/ConfigVariable.cpp"
			${test_Common_sources}
		)

	set(test_libs
			SDL2::SDL2
			glad
			streflop
			${WINMM_LIBRARY}
		)

	# -DNOT_USING_CREG: avoid pulling the creg serializer (CMatrix44f's CR_BIND).
	# --gc-sections: strip Shader.cpp's file/Lua shader-loading paths (CFileHandler,
	# LuaMatTexture, ...) that the inline-shader RenderBuffer path never calls.
	add_spring_test(${test_name} "${test_src}" "${test_libs}"
		"-DNOT_USING_CREG -ffunction-sections -fdata-sections")
	target_link_options(test_${test_name} PRIVATE -Wl,--gc-sections)
	# the test .cpp includes GLTestHarness.h from the parent test dir
	target_include_directories(test_${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
