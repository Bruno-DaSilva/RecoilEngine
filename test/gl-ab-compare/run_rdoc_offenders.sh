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
# NOTE ON RUNTIME. ~1 minute on the canonical RELWITHDEBINFO build. If a run
# takes many minutes instead, check CMAKE_BUILD_TYPE before suspecting anything
# else: a Debug build takes 10-20+ minutes for this same content, because the
# run is also under LD_PRELOAD=librenderdoc.so and every GL call is intercepted.
# A Debug build additionally aborts on the startscript's NullAI assert. Draw no
# performance conclusion without checking the cache first -- this cost a whole
# false "the conversions are too expensive" investigation.
#
# Companions, both reading the log this leaves in the write-dir:
#   rdoc_offender_sites.sh  resolves each function's FIRST-USE backtrace to source
#   rdoc_site_census.sh     with RDOC_SITE_CENSUS=1 set here, resolves EVERY
#                           reachable call site with a call count -- the actual
#                           burn-down work list, and the 1-minute replacement for
#                           the ~20-minute apitrace census
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

# The A/B compare harness renders a deliberate LEGACY pass every frame and blits
# it with glWindowPos2i/glDrawPixels, so leaving it on inflates the offender list
# with functions no shipping frame uses (measured: 49 vs 47). The meter has to own
# this setting rather than inherit whatever the last gate run left behind.
cfg=$writeDir/springsettings.cfg
if [[ -f $cfg ]] && grep -qE '^GLFrameABCompare(Dump)? *= *[^0]' "$cfg"; then
	printf '[rdoc] forcing GLFrameABCompare=0 for this run (was on)\n'
	sed -i -E 's/^(GLFrameABCompare(Dump)?) *= *.*/\1 = 0/' "$cfg"
fi

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
