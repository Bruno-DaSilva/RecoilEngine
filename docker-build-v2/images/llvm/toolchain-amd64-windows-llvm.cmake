# Cross-compile to 64-bit Windows using the self-contained llvm-mingw toolchain
# (clang + lld + libc++ + libunwind + compiler-rt, all built for clang with NATIVE TLS).
#
# Unlike a distro clang paired with the gcc-mingw sysroot (libstdc++ + libgcc +
# -femulated-tls), this is a fully self-consistent clang stack: storage and
# thread_local destructors both use native Windows TLS, so the emulated-TLS
# thread_local-teardown crash at thread exit does not occur.
#
# msvcrt variant, to stay CRT/ABI-compatible with the gcc-built mingwlibs64
# prebuilts and with Windows 7.

SET(CMAKE_SYSTEM_NAME Windows)
SET(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_MINGW_ROOT /opt/llvm-mingw CACHE PATH "Root of the llvm-mingw toolchain")

set(_tt x86_64-w64-mingw32)
set(_bin ${LLVM_MINGW_ROOT}/bin)

# The target-triple drivers come pre-wired for the target, llvm-mingw sysroot, libc++,
# lld and compiler-rt -- no --target/--sysroot/-stdlib needed.
SET(CMAKE_C_COMPILER   ${_bin}/${_tt}-clang)
SET(CMAKE_CXX_COMPILER ${_bin}/${_tt}-clang++)

SET(CMAKE_RC_COMPILER ${_bin}/${_tt}-windres)
SET(WINDRES_BIN       ${_bin}/${_tt}-windres)
SET(CMAKE_DLLTOOL     ${_bin}/llvm-dlltool)
SET(DLLTOOL           ${_bin}/llvm-dlltool)

# -fms-extensions: treat __cpuidex & friends as builtins so clang's <cpuid.h>
#   doesn't clash with mingw's <intrin.h>.
# NOTE: deliberately NO -femulated-tls and NO -static-libstdc++/-static-libgcc here.
# -static links libc++/libc++abi/libunwind/libwinpthread/compiler-rt statically so the
#   binary is self-contained (and avoids any cross-DLL TLS subtleties).
SET(CMAKE_C_FLAGS_INIT   "-fms-extensions")
# libc++ pulls in far fewer headers transitively than libstdc++, so a lot of code
# (vendored assimp/smmalloc/RmlUi especially) trips over std::abs / std::is_trivial /
# std::floor / std::malloc without the right include. Force-include the usual suspects
# for every C++ TU rather than patching third-party sources file-by-file. CXX only --
# these are C++ headers.
SET(CMAKE_CXX_FLAGS_INIT "-fms-extensions -include cstdlib -include cmath -include type_traits")
add_link_options(-static -pthread)

# Native CodeView debug info, PDB written by lld next to each exe/dll.
# -gcodeview selects the format; the -g that turns debug info on comes from the
# build type. --pdb= (empty) derives <output>.pdb from the output name.
add_compile_options(-gcodeview)
add_link_options(-Wl,--pdb=)

SET(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

# Cross-compile search rules: target libs/headers under the roots, host programs normally.
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
