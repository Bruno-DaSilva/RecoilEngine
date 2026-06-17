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
# lexer-compat patch or the vm_x86.dasc streflop redirects). GLOB is
# configure-time, which is fine for edits. Deliberately NOT globbing *.h: the
# build generates headers into src/ (lj_bcdef.h, lj_ffdef.h, lj_vm.S, ...) and a
# `make clean` deletes them, which would leave phantom DEPENDS with no rule.
# Tracking the .c/.dasc/Makefile inputs is sufficient to trigger rebuilds.
file(GLOB LUAJIT_SRCS
	${LUAJIT_DIR}/src/*.c ${LUAJIT_DIR}/src/*.dasc
	${LUAJIT_DIR}/Makefile ${LUAJIT_DIR}/src/Makefile)

# Native (host == target) build. -fPIC so the static archive links into the
# (PIE) engine. Driven through CC so the host tools pick it up too.
# We build ONLY the static archive (target `libluajit.a`), not LuaJIT's default
# `all`/`amalg` target. The latter also links the standalone `luajit` exe, which
# would fail: our deterministic math symbols (lj_sfm_*, referenced by the VM)
# are defined in liblua.a (spring_luajit_detmath.cpp, C++/streflop), not inside
# LuaJIT's own C world. The engine links liblua.a + libluajit.a together, so the
# refs resolve there. LJCORE_O=ljamalg.o keeps the fast amalgamated compile.
add_custom_command(
	OUTPUT  ${LUAJIT_LIB}
	COMMAND ${CMAKE_COMMAND} -E env make -C ${LUAJIT_DIR}/src
	        BUILDMODE=static
	        LJCORE_O=ljamalg.o
	        "CC=${CMAKE_C_COMPILER} -fPIC"
	        "XCFLAGS=-DLUAJIT_SECURITY_PRNG=0 -DLUAJIT_SECURITY_STRHASH=0 -DLUAJIT_SECURITY_STRID=0"
	        libluajit.a
	DEPENDS ${LUAJIT_SRCS}
	COMMENT "Building vendored LuaJIT 2.1 (static archive, PIC, det-math)"
	VERBATIM
)
add_custom_target(luajit_build DEPENDS ${LUAJIT_LIB})

# Deterministic transcendental math (lj_sfm_*) for the interpreter VM. These are
# REFERENCED by libluajit.a (lj_vm.o) and DEFINED here against streflop. The
# dependency is one-way (libluajit.a -> detmath -> streflop), so detmath must
# appear AFTER libluajit.a on the link line: it lives in its own archive linked
# after ${LUAJIT_LIB} below, not folded into liblua.a (which links first).
ADD_LIBRARY(luajit_detmath STATIC EXCLUDE_FROM_ALL "src/spring_luajit_detmath.cpp")
target_include_directories(luajit_detmath PUBLIC ${LUAJIT_DIR}/src)
if (ENABLE_STREFLOP)
	TARGET_LINK_LIBRARIES(luajit_detmath PUBLIC streflop)
endif ()

SET(luaSources
		"src/spring_luajit_shim.cpp"
		"include/LuaUser.cpp"
	)

ADD_LIBRARY(lua STATIC EXCLUDE_FROM_ALL ${luaSources})
add_dependencies(lua luajit_build)

# Consumers (engine, luasocket) get LuaJIT's headers (via the forwarders in
# include/) and link the LuaJIT archive. Order matters: libluajit.a first, then
# luajit_detmath (resolves lj_sfm_*), then streflop (resolves detmath).
target_include_directories(lua PUBLIC include ${LUAJIT_DIR}/src)
target_link_libraries(lua PUBLIC ${LUAJIT_LIB} luajit_detmath ${CMAKE_DL_LIBS})

if (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "-DLUA_USE_MKSTEMP ${PIC_FLAG}")
else (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")
endif (UNIX)
