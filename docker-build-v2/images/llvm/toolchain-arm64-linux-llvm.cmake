# Cross-compile to arm64 Linux with a glibc 2.27 (Ubuntu 18.04) floor.
# Identical scheme to toolchain-amd64-linux-llvm.cmake (see comments there);
# runs on a plain amd64 host - no emulation, no arm runners.

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR aarch64)
SET(CMAKE_SYSROOT /sysroots/arm64)

SET(CMAKE_C_COMPILER /opt/llvm-mingw/bin/clang)
SET(CMAKE_CXX_COMPILER /opt/llvm-mingw/bin/clang++)
SET(CMAKE_C_COMPILER_TARGET aarch64-unknown-linux-gnu)
SET(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-gnu)

SET(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
SET(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
SET(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
add_link_options(-fuse-ld=lld)

SET(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

set(ENV{PKG_CONFIG_LIBDIR} /build/spring-static-libs-arm64/lib/pkgconfig)
set(ENV{PKG_CONFIG} "pkg-config --define-prefix --static")
set(ENV{PREFER_STATIC_LIBS} TRUE)
SET(CMAKE_PREFIX_PATH /build/spring-static-libs-arm64)
set(ENV{PKG_CONFIG_SYSROOT_DIR} /)

SET(CMAKE_FIND_ROOT_PATH /build/spring-static-libs-arm64)
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
