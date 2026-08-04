# One build image, every platform, one toolchain: clang/lld from llvm-mingw
# cross-compiles all engine targets from a single amd64 ubuntu 24.04 image:
#
#   amd64-windows  clang + libc++ (msvcrt, Windows 7+), native CodeView PDBs
#   amd64-linux    clang + libstdc++ against glibc 2.27 (Ubuntu 18.04+)
#   arm64-linux    same, cross-compiled - no emulation, no arm runners
#
# Design constraints, and what forces each choice:
# - No compilation of tooling: the toolchain is the official prebuilt llvm-mingw
#   release tarball (no distro packages a clang-targeting mingw runtime stack).
#   The only things compiled at image-bake time are the libc++ runtime archives
#   (a library, not a toolchain - same category as spring-static-libs), because
#   nobody ships a libc++ built against glibc 2.27.
# - Ubuntu 18.04 support WITHOUT building on 18.04: the Linux targets compile
#   against sysroots assembled from pinned-and-verified bionic debs. The old
#   "modern gcc + old sysroot" approach fails because a gcc install carries its
#   own libstdc++ headers/archives baked against the build distro's glibc;
#   clang carries neither and takes both from the sysroot, where we place a
#   libc++ compiled against glibc 2.27. Floor gates below make this loud.
# - Windows 7 support: the *msvcrt* llvm-mingw variant (not the ucrt default),
#   which is also the CRT the prebuilt gcc-built mingwlibs64 DLLs use.
# - arm64 builds run on plain amd64 hosts/runners: the arm64 sysroot is
#   assembled by resolving the bionic package closure with apt for a foreign
#   architecture and unpacking with dpkg-deb -x - pure extraction, no qemu.
# - Newer C++: bump the LLVM_* args below; one version drives compiler,
#   Windows runtime, and both Linux libc++ builds, so they cannot drift.

FROM docker.io/ubuntu:24.04 AS builder

RUN apt-get update \
 && apt-get install --no-install-recommends --yes \
    curl ca-certificates xz-utils cmake ninja-build python3 \
 && rm -rf /var/lib/apt/lists/*

# Bionic archive signing key (2012), dropped from the 24.04 keyring; pinned by
# hash so everything downstream of it is signature-verified.
ARG UBUNTU_KEYRING_DEB=ubuntu-keyring_2018.02.28_all.deb
ARG UBUNTU_KEYRING_SHA256=c0763889cec4196839354d99b71f3c369dfa51ff17f59cabea87d1c7d55fe5e0
RUN curl -sfLO "http://archive.ubuntu.com/ubuntu/pool/main/u/ubuntu-keyring/${UBUNTU_KEYRING_DEB}" \
 && echo "${UBUNTU_KEYRING_SHA256}  ${UBUNTU_KEYRING_DEB}" | sha256sum -c - \
 && dpkg-deb -x "${UBUNTU_KEYRING_DEB}" /tmp/keyring \
 && cp /tmp/keyring/usr/share/keyrings/ubuntu-archive-keyring.gpg /bionic-keyring.gpg \
 && rm -rf "${UBUNTU_KEYRING_DEB}" /tmp/keyring

# Official llvm-mingw release: clang + lld + per-target mingw runtimes.
# The ubuntu-22.04 host tarball runs on 24.04 (glibc 2.39 >= 2.35).
ARG LLVM_MINGW_TAG=20260602
ARG LLVM_MINGW_SHA256=1ced4ca873d48eca3ad9b8b8bbda64cfbd8f8bfc7db739291d75b83275de0e30
RUN curl -sfL -o llvm-mingw.tar.xz \
      "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_TAG}/llvm-mingw-${LLVM_MINGW_TAG}-msvcrt-ubuntu-22.04-x86_64.tar.xz" \
 && echo "${LLVM_MINGW_SHA256}  llvm-mingw.tar.xz" | sha256sum -c - \
 && mkdir /opt/llvm-mingw \
 && tar -xf llvm-mingw.tar.xz --strip-components=1 -C /opt/llvm-mingw \
 && rm llvm-mingw.tar.xz

# Ubuntu 18.04 sysroots for both Linux target architectures. The toolchain-r
# PPA key signs the bionic-built libstdc++-13 packages (see mksysroot.sh).
COPY llvm/mksysroot.sh llvm/ubuntu-toolchain-r.gpg /
RUN /mksysroot.sh amd64 /sysroots/amd64 /bionic-keyring.gpg /ubuntu-toolchain-r.gpg
RUN /mksysroot.sh arm64 /sysroots/arm64 /bionic-keyring.gpg /ubuntu-toolchain-r.gpg

# libc++/libc++abi, version-matched to the compiler, compiled against the
# bionic sysroots and installed into them (so the engine toolchain needs only
# --sysroot and -stdlib=libc++). The ABI is merged into libc++.a, mirroring
# how llvm-mingw itself packages its runtimes.
ARG LLVM_VERSION=22.1.7
ARG LLVM_PROJECT_SHA256=5cc4a3f12bba50b6bdfb4b61bdc852117a0ff2517807c3902fc13267fb93562e
RUN curl -sfL -o llvm-project.tar.xz \
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz" \
 && echo "${LLVM_PROJECT_SHA256}  llvm-project.tar.xz" | sha256sum -c - \
 && tar -xf llvm-project.tar.xz \
 && rm llvm-project.tar.xz

COPY llvm/smoke.cpp /
RUN for arch in "amd64 x86_64-unknown-linux-gnu" "arm64 aarch64-unknown-linux-gnu"; do \
      set -e; set -- $arch; \
      cmake -S "llvm-project-${LLVM_VERSION}.src/runtimes" -B "libcxx-$1" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_COMPILER=/opt/llvm-mingw/bin/clang \
        -DCMAKE_CXX_COMPILER=/opt/llvm-mingw/bin/clang++ \
        -DCMAKE_C_COMPILER_TARGET="$2" \
        -DCMAKE_CXX_COMPILER_TARGET="$2" \
        -DCMAKE_SYSROOT="/sysroots/$1" \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DCMAKE_C_FLAGS=-fuse-ld=lld \
        -DCMAKE_CXX_FLAGS=-fuse-ld=lld \
        -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi' \
        -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
        -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX="/sysroots/$1/usr" \
      && ninja -C "libcxx-$1" install \
      && rm -rf "libcxx-$1"; \
    done

# Floor gates: link the smoke test the way the engine will be linked and
# refuse to bake an image whose binaries could reference glibc > 2.27. Both
# stdlibs are gated: the default (PPA libstdc++-13, what engine builds link
# today for spring-static-libs ABI compatibility) and libc++ (the endgame,
# one -stdlib flag away). The amd64 binaries must also actually run.
RUN set -e; \
    for arch in "amd64 x86_64-unknown-linux-gnu" "arm64 aarch64-unknown-linux-gnu"; do \
      set -- $arch; \
      for stdlib in "gnu -static-libstdc++" "llvm -stdlib=libc++"; do \
        set -- $1 $2 $stdlib; \
        /opt/llvm-mingw/bin/clang++ --target="$2" --sysroot="/sysroots/$1" \
          -fuse-ld=lld -std=c++23 "$4" -O2 /smoke.cpp -o "/smoke-$1-$3" -pthread; \
        maxver=$(/opt/llvm-mingw/bin/llvm-readelf --dyn-syms "/smoke-$1-$3" \
          | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1); \
        echo "smoke-$1-$3: max ${maxver}"; \
        dpkg --compare-versions "${maxver#GLIBC_}" le 2.27; \
        ! /opt/llvm-mingw/bin/llvm-readelf --dyn-syms "/smoke-$1-$3" \
          | grep -qE 'arc4random|_dl_find_object|__libc_single_threaded'; \
      done; \
    done; \
    /smoke-amd64-gnu && /smoke-amd64-llvm

FROM docker.io/ubuntu:24.04

RUN apt-get update \
 && apt-get upgrade --yes \
 && apt-get install --no-install-recommends --yes \
    git ca-certificates ninja-build ccache pipx p7zip-full zstd pkg-config binutils \
 && rm -rf /var/lib/apt/lists/*

# Using `pipx --global` would be better but it's available only since pipx 1.5
RUN PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin pipx install cmake==3.27.*

COPY --from=builder /opt/llvm-mingw /opt/llvm-mingw
COPY --from=builder /sysroots /sysroots
ENV PATH="/opt/llvm-mingw/bin:${PATH}"

# split-debug-info.sh uses bare objdump/objcopy/strip/strings; route them to
# the llvm tools, which handle every target this image produces (amd64+arm64
# ELF and PE/COFF) with one implementation.
RUN for tool in objdump objcopy strip strings; do \
      ln -s /opt/llvm-mingw/bin/llvm-$tool /usr/local/bin/$tool; \
    done

WORKDIR /build
RUN mkdir src cache out artifacts && chmod a+rwx cache out artifacts

# Per-platform library dependencies
RUN git clone --depth=1 https://github.com/beyond-all-reason/mingwlibs64.git
ENV MINGWLIBS=/build/mingwlibs64
# The .pc files hardcode prefix=/build (their original build location) as
# literal paths; bionic's pkg-config 0.29 relocated those under
# --define-prefix but 24.04's pkgconf does not, so relocate them for real.
RUN git clone --depth=1 https://github.com/beyond-all-reason/spring-static-libs.git -b 18.04 spring-static-libs-amd64 \
 && find spring-static-libs-amd64/lib/pkgconfig -name '*.pc' \
      -exec sed -i 's|/build|/build/spring-static-libs-amd64|g' {} +
RUN git clone --depth=1 https://github.com/beyond-all-reason/spring-static-libs.git -b 18.04-arm spring-static-libs-arm64 \
 && find spring-static-libs-arm64/lib/pkgconfig -name '*.pc' \
      -exec sed -i 's|/build|/build/spring-static-libs-arm64|g' {} +

# One toolchain file per platform; build.sh selects via CMAKE_TOOLCHAIN_FILE.
# (Unlike the single-platform images, ENGINE_PLATFORM is not baked here -
# build.sh passes it per run.)
COPY llvm/toolchain-amd64-windows.cmake /build/
COPY llvm/toolchain-amd64-linux.cmake /build/
COPY llvm/toolchain-arm64-linux.cmake /build/

# Configure ccache caching
COPY all/ccache.conf .
ENV CCACHE_CONFIGPATH=/build/ccache.conf
ENV CMAKE_CXX_COMPILER_LAUNCHER=ccache
ENV CMAKE_C_COMPILER_LAUNCHER=ccache
