#!/bin/bash

set -e -u -o pipefail

cd /build/out/install

function split_debug {
    set -e -u -o pipefail

    file="$1"
    if ! objdump -h "$file" | grep -q ".debug_" || objdump -h "$file" | grep -q .gnu_debuglink; then
        echo "skipping $file"
        return
    fi
    echo "stripping $file"
    filename="$(basename "$file")"
    debugfile="$(dirname "$file")/${filename%.*}.dbg"
    objcopy --only-keep-debug ${file} ${debugfile}
    strip --strip-debug --strip-unneeded ${file}
    objcopy --add-gnu-debuglink=${debugfile} ${file}
}
export -f split_debug

# We split debug info from all binaries in parallel using xargs -P0
find \( -regex '.*\.\(dll\|so\|exe\)' -o -type f ! -name '*.dbg' -executable \) -print0 \
    | xargs -0 -P0 -n1 bash -c 'split_debug "$0"'

# CodeView builds (llvm-mingw): debug info is already separate - lld writes a
# <name>.pdb next to each binary in the build tree and embeds its absolute path
# in the PE debug directory (RSDS record). CMake's install step does not copy
# PDBs, and Windows debuggers look next to the binary first, so pair each
# installed binary with its PDB via the embedded path and copy it alongside.
# Binaries without an RSDS record (prebuilt mingwlibs DLLs, GCC/DWARF builds)
# are skipped, which makes this a no-op on non-CodeView images.
function place_pdb {
    set -e -u -o pipefail

    file="$1"
    pdb="$(strings -a "$file" | grep -m1 -E '^/.*\.pdb$' || true)"
    if [[ -z "$pdb" || ! -f "$pdb" ]]; then
        return
    fi
    echo "placing $(basename "$pdb") next to $file"
    cp -f "$pdb" "$(dirname "$file")/$(basename "$pdb")"
}
export -f place_pdb

find . -regex '.*\.\(dll\|exe\)' -print0 \
    | xargs -0 -P0 -n1 bash -c 'place_pdb "$0"'
