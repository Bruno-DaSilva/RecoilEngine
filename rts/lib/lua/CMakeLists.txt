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
# the vm_x64.dasc lj_sfm_* redirects). The synced VM additionally runs
# interpreter-only (jit.off, see CSyncedLuaHandle::Init).

# deactivate signaling-NANs for this library
remove_flag("-fsignaling-nans")

INCLUDE_DIRECTORIES(BEFORE ../streflop include)

set(LUAJIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../luajit)
# LuaJIT's Makefile builds strictly in-tree: it writes intermediate objects
# (host/minilua.o, ...) and *generated* headers (luajit.h, lj_bcdef.h, lj_vm.S,
# ...) straight into src/. The official docker build mounts the source tree
# read-only ("can't create host/minilua.o: Read-only file system"), so we build
# a private copy of the whole vendored tree under the (writable) CMake binary
# dir and link/include from there. Local (writable-source) builds work the same
# way; nothing is written back into the source tree anymore.
set(LUAJIT_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/luajit)
set(LUAJIT_LIB ${LUAJIT_BUILD_DIR}/src/libluajit.a)

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
# LuaJIT's Makefile knobs differ for a native build vs cross-compiling. On a
# native (host == target) build we just hand it the engine's compiler. When
# cross-compiling to Windows with mingw we MUST follow LuaJIT's documented cross
# recipe (see its src/Makefile header): the host tools minilua/buildvm run on
# the *build* machine and emit the target VM, so HOST_CC has to be a native
# compiler (the image carries plain `gcc`), while the target objects go through
# the mingw cross-compiler via CROSS+CC. TARGET_SYS=Windows picks the Win64
# VM/ABI. Jamming the mingw compiler into CC (the native recipe) wrongly builds
# the host tools for Windows, which then can't execute on the linux host.
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set(LUAJIT_MAKE_CC
		HOST_CC=gcc-13
		CROSS=x86_64-w64-mingw32-
		CC=gcc-posix
		TARGET_SYS=Windows)
else ()
	set(LUAJIT_MAKE_CC "CC=${CMAKE_C_COMPILER} -fPIC")
endif ()

add_custom_command(
	OUTPUT  ${LUAJIT_LIB}
	# Wipe any previous copy, THEN copy fresh. copy_directory MERGES into an
	# existing dir and never prunes, so a stale/partial tree left in the build
	# dir by an earlier run would survive (this broke CI: a leftover truncated
	# Makefile had no `clean` target). Removing first guarantees a pristine,
	# from-scratch tree -- which also lets us drop the previous `make clean`
	# step (its only purpose was to prune copy_directory leftovers). The build
	# regenerates luajit.h / lj_vm.S / ... into the copy, never the source.
	COMMAND ${CMAKE_COMMAND} -E remove_directory ${LUAJIT_BUILD_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${LUAJIT_DIR} ${LUAJIT_BUILD_DIR}
	COMMAND ${CMAKE_COMMAND} -E env make -C ${LUAJIT_BUILD_DIR}/src
	        BUILDMODE=static
	        LJCORE_O=ljamalg.o
	        ${LUAJIT_MAKE_CC}
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
target_include_directories(luajit_detmath PUBLIC ${LUAJIT_BUILD_DIR}/src)
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
target_include_directories(lua PUBLIC include ${LUAJIT_BUILD_DIR}/src)
target_link_libraries(lua PUBLIC ${LUAJIT_LIB} luajit_detmath ${CMAKE_DL_LIBS})

if (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "-DLUA_USE_MKSTEMP ${PIC_FLAG}")
else (UNIX)
	SET_TARGET_PROPERTIES(lua PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")
endif (UNIX)
