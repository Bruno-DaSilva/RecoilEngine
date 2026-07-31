#!/usr/bin/env bash
# Progress meter for full RenderDoc capturability.
#
# One call to any function on RenderDoc's unsupported list permanently and
# SILENTLY disables capture for the whole process (see the root-cause entry in
# doc/bar-gl4-immediate-mode-inventory.md), so the goal is not "few" but ZERO,
# and the only thing that matters is which distinct functions are still used.
#
# RenderDoc reports that only to its UI, never to a log, so this needs a
# librenderdoc built from source with renderdoc-unsupported-log.patch applied:
#
#   cd <renderdoc-src> && git apply <this-dir>/renderdoc-unsupported-log.patch
#   mkdir -p build && cd build
#   cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_QRENDERDOC=OFF \
#         -DENABLE_PYRENDERDOC=OFF -DENABLE_XCB=OFF -DENABLE_VULKAN=OFF ..
#   make -j$(nproc) renderdoc
#
# (XCB off avoids needing libxcb-keysyms1-dev; xlib/GLX is what the engine uses.)
#
# usage: run_rdoc_offenders.sh [--write-dir DIR] [--spring BIN] [--rdoc LIB]
#                              [--timeout SEC] [<startscript>]
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

writeDir=${AB_WRITE_DIR:-}
springBin=$root/build/spring
rdocLib=${RDOC_LIB:-/www/projects/renderdoc/build/lib/librenderdoc.so}
runTimeout=420
content=$here/watertest_startscript.txt

die() { printf '\n[rdoc] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--write-dir) writeDir=$2; shift 2 ;;
		--spring)    springBin=$2; shift 2 ;;
		--rdoc)      rdocLib=$2; shift 2 ;;
		--timeout)   runTimeout=$2; shift 2 ;;
		-*)          die "unknown option $1" ;;
		*)           content=$1; shift ;;
	esac
done

[[ -n $writeDir ]]  || die "no write-dir: pass --write-dir DIR or set AB_WRITE_DIR"
[[ -d $writeDir ]]  || die "write-dir not a directory: $writeDir"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"
[[ -e $content ]]   || die "content not found: $content"
[[ -r $rdocLib ]]   || die "patched librenderdoc not found at $rdocLib -- see the build recipe at the top of this script"
grep -q "RDOC-UNSUPPORTED" "$rdocLib" \
	|| die "$rdocLib has no RDOC-UNSUPPORTED marker; it is a stock build, apply renderdoc-unsupported-log.patch"

content=$(cd -- "$(dirname -- "$content")" && pwd)/$(basename -- "$content")
log=$writeDir/rdoc_offenders.log
capture=$writeDir/rdoc_capture

rm -f "$capture"*.rdc
: > "$writeDir/infolog.txt"

printf '[rdoc] %s\n' "$(basename -- "$content")"
timeout "$runTimeout" env DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
	LD_PRELOAD="$rdocLib" RENDERDOC_CAPFILE="$capture" \
	"$springBin" --isolation --write-dir "$writeDir" "$content" > "$log" 2>&1
rc=$?

case $rc in
	0)   ;;
	124) die "run TIMED OUT after ${runTimeout}s" ;;
	*)   die "engine exited rc=$rc (tail: $(tail -n 3 "$log" | tr '\n' ' '))" ;;
esac

# A run that drew nothing would also report zero offenders, so require evidence
# the engine actually got somewhere.
grep -q "Adding debug command\|Game::Load\|GameServer" "$writeDir/infolog.txt" \
	|| die "engine does not appear to have started a game -- zero offenders would be meaningless"

mapfile -t offenders < <(grep '^\[RDOC-UNSUPPORTED\] ' "$log" | sed 's/^\[RDOC-UNSUPPORTED\] //')

printf '[rdoc] distinct unsupported functions still used: %d\n' "${#offenders[@]}"
if (( ${#offenders[@]} )); then
	printf '[rdoc] in first-use order (the FIRST one is what kills capture):\n'
	printf '         %s\n' "${offenders[@]}"
fi

if ls "$capture"*.rdc >/dev/null 2>&1; then
	printf '\n[rdoc] CAPTURE SUCCEEDED: %s\n' "$(ls "$capture"*.rdc)"
	exit 0
fi

printf '\n[rdoc] no capture (expected while any function above remains)\n'
exit 1
