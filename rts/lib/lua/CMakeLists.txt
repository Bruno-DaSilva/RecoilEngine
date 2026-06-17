# Spring supplied CMake build file
#
# luajit-spike: the engine's `lua` target is built against vendored LuaJIT 2.1
# (rts/lib/luajit) instead of the modified PUC Lua 5.1 that used to live in
# this directory's src/. We build LuaJIT via its own (well-tested) Makefile and
# wrap it in a small static lib that carries the engine-side glue (LuaUser.cpp)
# and the fork's custom-symbol shim.
#
# Tier 1 synced determinism: LuaJIT is built with a fixed string-hash seed
# (LUAJIT_SECURITY_*=0) so table iteration order is identical across clients,
# and its interpreter math is routed through streflop (spring_luajit_detmath +
# the vm_x86.dasc lj_sfm_* redirects). The synced VM additionally runs
# interpreter-only (jit.off, see CSyncedLuaHandle::Init).

# deactivate signaling-NANs for this library
remove_flag("-fsignaling-nans")

INCLUDE_DIRECTORIES(BEFORE ../streflop include)

set(LUAJIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../luajit)
set(LUAJIT_LIB ${LUAJIT_DIR}/src/libluajit.a)

# Rebuild the archive when any vendored LuaJIT source changes (e.g. our 5.1
# lexer-compat patch). GLOB is configure-time, which is fine for edits.
file(GLOB LUAJIT_SRCS
	${LUAJIT_DIR}/src/*.c ${LUAJIT_DIR}/src/*.h ${LUAJIT_DIR}/src/*.dasc
	${LUAJIT_DIR}/Makefile ${LUAJIT_DIR}/src/Makefile)

# Native (host == target) build. -fPIC so the static archive links into the
# (PIE) engine. Driven through CC so the host tools pick it up too.
add_custom_command(
	OUTPUT  ${LUAJIT_LIB}
	COMMAND ${CMAKE_COMMAND} -E env make -C ${LUAJIT_DIR}
	        BUILDMODE=static
	        "CC=${CMAKE_C_COMPILER} -fPIC"
	        "XCFLAGS=-DLUAJIT_SECURITY_PRNG=0 -DLUAJIT_SECURITY_STRHASH=0 -DLUAJIT_SECURITY_STRID=0"
	        amalg
	DEPENDS ${LUAJIT_SRCS}
	COMMENT "Building vendored LuaJIT 2.1 (static, PIC)"
	VERBATIM
)
add_custom_target(luajit_build DEPENDS ${LUAJIT_LIB})

SET(luaSources
		"src/spring_luajit_shim.cpp"
		"src/spring_luajit_detmath.cpp"
		"include/LuaUser.cpp"
	)

ADD_LIBRARY(lua STATIC EXCLUDE_FROM_ALL ${luaSources})
add_dependencies(lua luajit_build)

# Consumers (engine, luasocket) get LuaJIT's headers (via the forwarders in
# include/) and link the LuaJIT archive.
target_include_directories(lua PUBLIC include ${LUAJIT_DIR}/src)
target_link_libraries(lua PUBLIC ${LUAJIT_LIB} ${CMAKE_DL_LIBS})

if (ENABLE_STREFLOP)
	TARGET_LINK_LIBRARIES(lua PUBLIC streflop)
endif ()

if (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "-DLUA_USE_MKSTEMP ${PIC_FLAG}")
else (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")
endif (UNIX)
