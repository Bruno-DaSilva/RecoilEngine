# Cross-compile to amd64 Linux with a glibc 2.27 (Ubuntu 18.04) floor.
#
# clang + lld from the llvm-mingw toolchain, compiling against a bionic sysroot.
# clang, unlike gcc, has no baked-in C++ stdlib or runtime for the target: it
# takes headers and archives from the sysroot, which is what makes the old-glibc
# floor hold. The C++ stdlib is the toolchain-r PPA's libstdc++-13, built ON
# bionic against glibc 2.27 - the same library today's GCC image links and the
# ABI the spring-static-libs C++ archives (libIL.a et al.) were compiled
# against. (The sysroot also carries a bionic-built libc++, gated at image
# bake; switching to it is one -stdlib=libc++ flag away once spring-static-libs
# ships a libc++ build.) The engine's PreferStaticLibs adds -static-libstdc++,
# so the runtime is linked statically from the sysroot.

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR x86_64)
SET(CMAKE_SYSROOT /sysroots/amd64)

SET(CMAKE_C_COMPILER /opt/llvm-mingw/bin/clang)
SET(CMAKE_CXX_COMPILER /opt/llvm-mingw/bin/clang++)
SET(CMAKE_C_COMPILER_TARGET x86_64-unknown-linux-gnu)
SET(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-linux-gnu)

# tools/CMakeLists.txt wipes the global CMAKE_*_FLAGS before pr-downloader;
# directory link options survive that, so the linker choice lives here.
#
# VERIFIED libc++ flip (once spring-static-libs ships its libc++ build - see
# its _scripts/build-in-llvm-image.sh - and the clone below points at it):
#   CMAKE_CXX_FLAGS_INIT   = "-stdlib=libc++ -include cstdlib -include cmath -include type_traits"
#   all *_LINKER_FLAGS_INIT = "-fuse-ld=lld -stdlib=libc++"
#   add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
#   add_link_options(-fuse-ld=lld -stdlib=libc++)
# Validated 2026-08-04: engine builds, boots on clean 18.04, floor GLIBC_2.27,
# streflop sync reference reproduced bit-exactly (47852/47852).
SET(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
SET(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
SET(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
add_link_options(-fuse-ld=lld)

SET(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

# Static engine dependencies (same resolution scheme as the GCC linux image,
# where these were baked container env).
set(ENV{PKG_CONFIG_LIBDIR} /build/spring-static-libs-amd64/lib/pkgconfig)
set(ENV{PKG_CONFIG} "pkg-config --define-prefix --static")
set(ENV{PREFER_STATIC_LIBS} TRUE)
SET(CMAKE_PREFIX_PATH /build/spring-static-libs-amd64)
# CMake would otherwise derive PKG_CONFIG_SYSROOT_DIR from CMAKE_SYSROOT and
# re-root the static-libs paths, which live on the host side, not the sysroot.
set(ENV{PKG_CONFIG_SYSROOT_DIR} /)

# Cross-compile search rules: target libs/headers under the sysroot and the
# static-libs tree, host programs resolved normally.
SET(CMAKE_FIND_ROOT_PATH /build/spring-static-libs-amd64)
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
