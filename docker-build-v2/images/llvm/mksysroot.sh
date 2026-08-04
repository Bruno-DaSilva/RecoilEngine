#!/bin/bash
# Assemble an Ubuntu 18.04 (bionic) sysroot for cross-compilation, without
# emulation: apt resolves the package closure for the target architecture
# against an empty package state, the debs are downloaded and unpacked with
# dpkg-deb -x (pure extraction, works for any architecture).
#
# Usage: mksysroot.sh {amd64|arm64} <dest-dir> <archive-keyring.gpg> <toolchain-ppa-keyring.gpg>
set -e -u -o pipefail

ARCH="$1"
DEST="$2"
KEYRING="$3"
PPA_KEYRING="$4"

case "$ARCH" in
	amd64)
		MIRROR=http://archive.ubuntu.com/ubuntu
		SECURITY=http://security.ubuntu.com/ubuntu
		;;
	arm64)
		MIRROR=http://ports.ubuntu.com/ubuntu-ports
		SECURITY=$MIRROR
		;;
	*)
		echo "unsupported arch: $ARCH"; exit 1
		;;
esac

# The same library set the GCC 18.04 build image installs (libc/libgcc dev plus
# the four dynamically-linked engine deps); everything else comes statically
# from spring-static-libs. libstdc++-13-dev comes from the toolchain-r PPA,
# built ON bionic against glibc 2.27 - the same libstdc++ the GCC image links
# and the one the spring-static-libs C++ archives (e.g. libIL.a) were compiled
# against, so clang links it for ABI compatibility with them.
PACKAGES="libc6-dev libgcc-7-dev linux-libc-dev libatomic1
          libstdc++-13-dev
          libsdl2-dev libopenal-dev libfreetype6-dev libfontconfig1-dev
          libxmu-dev libxi-dev libgl-dev"

APTROOT=$(mktemp -d)
mkdir -p "$APTROOT/lists/partial" "$APTROOT/cache" "$DEST"
touch "$APTROOT/status"
cat > "$APTROOT/sources.list" <<EOF
deb [signed-by=$KEYRING] $MIRROR bionic main universe
deb [signed-by=$KEYRING] $MIRROR bionic-updates main universe
deb [signed-by=$KEYRING] $SECURITY bionic-security main universe
deb [signed-by=$PPA_KEYRING] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu bionic main
EOF

APTOPTS=(
	-o Dir::Etc::sourcelist="$APTROOT/sources.list"
	-o Dir::Etc::sourceparts=/dev/null
	-o Dir::State="$APTROOT"
	-o Dir::State::lists="$APTROOT/lists"
	-o Dir::Cache="$APTROOT/cache"
	-o Dir::State::status="$APTROOT/status"
	-o APT::Architecture="$ARCH"
	-o APT::Architectures::="$ARCH"
	-o APT::Install-Recommends=false
)

apt-get "${APTOPTS[@]}" update
apt-get "${APTOPTS[@]}" install --print-uris -y $PACKAGES \
	| grep -oP "^'\K[^']+" > "$APTROOT/uris.txt"
test -s "$APTROOT/uris.txt"

DEBS=$(mktemp -d)
while read -r uri; do
	curl -sfLO --output-dir "$DEBS" "$uri"
done < "$APTROOT/uris.txt"
echo "$(ls "$DEBS" | wc -l) packages for $ARCH"

for deb in "$DEBS"/*.deb; do
	dpkg-deb -x "$deb" "$DEST"
done

# Absolute symlinks (e.g. libgcc_s.so -> /lib/...) escape the sysroot and
# resolve against the host; rewrite them relative.
(cd "$DEST" && find . -lname '/*' | while read -r link; do
	rel=$(realpath -s --relative-to="$(dirname "$link")" ".$(readlink "$link")")
	ln -sf "$rel" "$link"
done)
test "$(cd "$DEST" && find . -lname '/*' | wc -l)" = 0

# CMake package configs (e.g. sdl2-config.cmake) hardcode literal /usr paths
# that escape the sysroot; rewrite them in place. They must stay usable: the
# engine's FindSDL2 module consumes the config's SDL2_* variables.
find "$DEST"/usr/lib -path '*/cmake/*' -name '*.cmake' \
	-exec sed -i "s|\"/usr|\"$DEST/usr|g" {} +

# -latomic (mimalloc, tests) must resolve to the static archive: desktop
# 18.04 does not preinstall libatomic1, so a dynamic link would fail at load
# on end-user machines. libgcc-13-dev provides libatomic.a; delete the
# dynamic variants so the linker cannot prefer them.
find "$DEST" -name 'libatomic.so*' -delete
test -n "$(find "$DEST" -name libatomic.a | head -1)"

rm -rf "$APTROOT" "$DEBS"
