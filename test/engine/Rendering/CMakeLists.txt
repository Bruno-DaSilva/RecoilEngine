# Engine-GL unit tests: render via the REAL engine rendering path and A/B
# against legacy GL. These must NOT be built with -DUNIT_TEST, because the
# engine GL headers (myGL.h) deliberately #error under UNIT_TEST. The render
# path's external globals (globalRendering, configHandler, ...) are provided as
# minimal stubs (GLEngineStubs.cpp) so we don't pull in the window system/VFS.
remove_definitions(-DUNIT_TEST)

if (TARGET SDL2::SDL2 AND TARGET glad)
	# shared engine rendering sources + stubs that every engine-GL test links.
	# -DNOT_USING_CREG drops CMatrix44f's creg; --gc-sections (set per target
	# below) strips Shader.cpp's file/Lua shader-loading paths that the inline
	# RenderBuffer shaders never call.
	set(gl_engine_srcs
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
			"${ENGINE_SOURCE_DIR}/Lua/LuaImmediateBuffer.cpp"
			"${ENGINE_SOURCE_DIR}/System/Matrix44f.cpp"
			"${ENGINE_SOURCE_DIR}/System/Quaternion.cpp"
			"${ENGINE_SOURCE_DIR}/System/float3.cpp"
			"${ENGINE_SOURCE_DIR}/System/float4.cpp"
			"${ENGINE_SOURCE_DIR}/System/StringUtil.cpp"
			"${ENGINE_SOURCE_DIR}/System/MemoryOverride.cpp"
			"${ENGINE_SOURCE_DIR}/System/Config/ConfigVariable.cpp"
			${test_Common_sources}
		)
	set(gl_engine_libs SDL2::SDL2 glad streflop ${WINMM_LIBRARY})
	set(gl_engine_flags "-DNOT_USING_CREG -ffunction-sections -fdata-sections")

	# helper to declare an engine-GL test linking the shared set
	macro(add_engine_gl_test name srcfile)
		set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${srcfile}" ${gl_engine_srcs})
		add_spring_test(${name} "${_src}" "${gl_engine_libs}" "${gl_engine_flags}")
		target_link_options(test_${name} PRIVATE -Wl,--gc-sections)
		target_include_directories(test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
	endmacro()

	add_engine_gl_test(GLRenderBufferCompare testGLRenderBufferCompare.cpp)
	add_engine_gl_test(GLImmediateEmitter    testGLImmediateEmitter.cpp)
	add_engine_gl_test(GLFontMVPCompare      testGLFontMVPCompare.cpp)
endif()
