#!/usr/bin/env bash
# Take an actual RenderDoc capture of a BAR frame.
#
# run_rdoc_offenders.sh scores the burn-down -- how many distinct functions on
# RenderDoc's unsupported list a run still reaches -- and that score reaching
# zero is a prediction, not a capture. This is the experiment that settles it:
# same content, same knobs, but a capture is requested and either a .rdc lands or
# it does not.
#
# The requesting is done by rdoc_trigger.so, preloaded alongside librenderdoc;
# see the rationale at the top of rdoc_trigger.c. RenderDoc refuses SILENTLY once
# any unsupported function has been called -- it tears down the frame capturers
# and a later trigger is simply ignored -- so "no file appeared" is the expected
# shape of failure, and the shim reports it explicitly rather than leaving an
# empty directory to interpret.
#
# The migration knobs have to be ON for this to mean anything: with them off the
# engine deliberately still uses the fixed-function pipeline, and the capture is
# expected to fail. Every knob is set here and checked in the engine's echo,
# because the engine rewrites springsettings.cfg on exit and DROPS any key at its
# default value -- a setting written before a run is not a setting the run used.
#
# usage: run_rdoc_capture.sh [--write-dir DIR] [--spring BIN] [--rdoc LIB]
#                            [--legacy] [--no-validate] [--timeout SEC]
#                            [<startscript>]
#
#   --legacy       turn the migration knobs OFF: the control, which must FAIL to
#                  capture, and the evidence that this harness can tell the two
#                  apart
#   --no-validate  stop after the .rdc appears, skipping the replay check
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

writeDir=${AB_WRITE_DIR:-}
springBin=$root/build/spring
rdocLib=${RDOC_LIB:-/www/projects/renderdoc/build/lib/librenderdoc.so}
runTimeout=420
content=$here/watertest_startscript.txt
legacy=0
validate=1

die() { printf '\n[capture] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--write-dir) writeDir=$2; shift 2 ;;
		--spring)    springBin=$2; shift 2 ;;
		--rdoc)      rdocLib=$2; shift 2 ;;
		--timeout)   runTimeout=$2; shift 2 ;;
		--legacy)      legacy=1; shift ;;
		--no-validate) validate=0; shift ;;
		-*)          die "unknown option $1" ;;
		*)           content=$1; shift ;;
	esac
done

[[ -n $writeDir ]]  || die "no write-dir: pass --write-dir DIR or set AB_WRITE_DIR"
[[ -d $writeDir ]]  || die "write-dir not a directory: $writeDir"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"
[[ -e $content ]]   || die "content not found: $content"
[[ -r $rdocLib ]]   || die "librenderdoc not found at $rdocLib -- see the build recipe in run_rdoc_offenders.sh"

content=$(cd -- "$(dirname -- "$content")" && pwd)/$(basename -- "$content")
log=$writeDir/rdoc_capture.log
# Per-mode path template. The control run clears its own captures before
# starting, and sharing one template means the run that must produce NOTHING
# deletes the capture the real run just produced.
capture=$writeDir/rdoc_capture$( ((legacy)) && printf '_legacy' )
cfg=$writeDir/springsettings.cfg

# The header ships with librenderdoc's source; find it from the library path.
rdocSrc=${RDOC_SRC:-$(cd -- "$(dirname -- "$rdocLib")/../.." && pwd)}
rdocHdr=$rdocSrc/renderdoc/api/app/renderdoc_app.h
[[ -r $rdocHdr ]] || die "renderdoc_app.h not found at $rdocHdr -- set RDOC_SRC to the renderdoc source tree"

triggerLib=$root/build/rdoc_trigger.so
printf '[capture] building the trigger shim\n'
cc -shared -fPIC -O2 -Wall -I"$(dirname -- "$rdocHdr")" \
	-o "$triggerLib" "$here/rdoc_trigger.c" -lpthread \
	|| die "could not build rdoc_trigger.so"

cfgSet() { # cfgSet KEY VALUE -- substitute in place or append; never silently miss
	if grep -qE "^$1 *=" "$cfg" 2>/dev/null; then
		sed -i -E "s/^$1 *=.*/$1 = $2/" "$cfg"
	else
		printf '%s = %s\n' "$1" "$2" >> "$cfg"
	fi
	grep -qE "^$1 = $2\$" "$cfg" || die "could not set $1=$2 in $cfg"
}

# The A/B harness renders a deliberate legacy pass and blits it with
# glDrawPixels, which would put functions back on the unsupported list that no
# shipping frame uses. Same reason the offender meter forces it off.
cfgSet GLFrameABCompare 0
cfgSet GLFrameABCompareDump 0

migrationKnobs=(LuaModernGLBackend LuaCmdListBakedStreams LuaCmdListSuspendOnObjectCreate
                ModernModelAttribs ModernModelFFShader FFVertexAttribRewrite FFMatrixSuppress)
want=$(( 1 - legacy ))
for k in "${migrationKnobs[@]}"; do cfgSet "$k" "$want"; done

# The offender meter drives gl.UnitShape deliberately, because a path the run
# never touches scores the same as a path that was retired. A capture wants the
# opposite: a frame that looks like BAR, not one with manufactured coverage in
# it. The driver also leaks GL state badly enough to leave the world white --
# measured, 100% of the world region, bisected to this widget out of 117 -- which
# turns the capture into a picture of nothing.
cfgSet ABUnitShapeDriver 0

# The gate sets FFRewriteBuiltinArm so its forced-legacy passes render rewritten
# shaders correctly, and the knob PERSISTS in the config. A capture must not
# inherit it: the arm re-names gl_Vertex/gl_MultiTexCoord0, which pins those
# shaders to the compatibility profile and re-breaks RenderDoc's core replay.
cfgSet FFRewriteBuiltinArm 0

rm -f "$capture"*.rdc
: > "$writeDir/infolog.txt"

printf '[capture] %s, migration knobs %s\n' "$(basename -- "$content")" \
	"$( ((legacy)) && echo 'OFF (control -- capture is EXPECTED to fail)' || echo 'ON')"

timeout "$runTimeout" env DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
	LD_PRELOAD="$rdocLib:$triggerLib" \
	RDOC_TRIGGER_INFOLOG="$writeDir/infolog.txt" \
	RDOC_TRIGGER_MARKER="[Game::Load][8]" \
	RDOC_TRIGGER_DELAY="${RDOC_TRIGGER_DELAY:-20}" \
	RDOC_TRIGGER_FRAMES="${RDOC_TRIGGER_FRAMES:-1}" \
	RDOC_TRIGGER_CAPFILE="$capture" \
	"$springBin" --isolation --write-dir "$writeDir" "$content" > "$log" 2>&1
rc=$?

case $rc in
	0)   ;;
	124) die "run TIMED OUT after ${runTimeout}s" ;;
	*)   die "engine exited rc=$rc (tail: $(tail -n 3 "$log" | tr '\n' ' '))" ;;
esac

infolog=$writeDir/infolog.txt

# A run that never drew would also produce no unsupported calls.
grep -q "Game::Load\|GameServer" "$infolog" \
	|| die "engine does not appear to have started a game"
grep -q "\[AB UnitShape Driver\] active" "$infolog" \
	&& die "the unit-shape driver ran -- it whites out the world, so the capture would be of nothing"

# What the engine ECHOES is what it used; the config file is only what was asked.
# The echo omits keys sitting at their default, so an absent FFMatrixSuppress is
# on and an absent anything-else is off.
defaultOnKnobs=(FFMatrixSuppress)
knobState=
for k in "${migrationKnobs[@]}"; do
	if grep -qE "^\[[^]]*\]  *$k = 1\$|  $k = 1\$" "$infolog"; then
		got=1
	elif grep -qE "^\[[^]]*\]  *$k = 0\$|  $k = 0\$" "$infolog"; then
		got=0
	elif [[ " ${defaultOnKnobs[*]} " == *" $k "* ]]; then
		got=1
	else
		got=0
	fi

	knobState+=" $k=$got"
	(( got == want )) || die "$k is $got, wanted $want -- the run did not use the configuration this test is about"
done
printf '[capture] knobs:%s\n' "$knobState"

printf '[capture] trigger:\n'
grep '^\[rdoc-trigger\] ' "$log" | sed 's/^/         /'

mapfile -t offenders < <(grep '^\[RDOC-UNSUPPORTED\] ' "$log" | sed 's/^\[RDOC-UNSUPPORTED\] //')
printf '[capture] distinct unsupported functions used this run: %d\n' "${#offenders[@]}"
(( ${#offenders[@]} )) && printf '         %s\n' "${offenders[@]}"

shopt -s nullglob
rdcs=("$capture"*.rdc)
shopt -u nullglob

if (( ${#rdcs[@]} == 0 )); then
	if (( legacy )); then
		printf '\n[capture] no capture, as expected with the migration knobs off.\n'
		exit 0
	fi
	die "no capture was produced"
fi

if (( legacy )); then
	die "a capture appeared with the migration knobs OFF -- the control is not a control, something is not actually running legacy"
fi

printf '\n[capture] CAPTURE SUCCEEDED\n'
for f in "${rdcs[@]}"; do
	printf '         %s (%s bytes)\n' "$f" "$(stat -c%s "$f")"
	# RDOC magic, so a truncated or empty file cannot pass as a capture
	head -c 8 "$f" | grep -q "RDOC" || die "$f does not start with the RDOC magic"
done

((validate)) || exit 0

# A file with the right magic is not the claim. The claim is that RenderDoc can
# OPEN this frame, and only its replay side can answer that -- see the header of
# rdoc_validate_capture.cpp.
validator=$root/build/rdoc_validate_capture
printf '[capture] building the validator\n'
c++ -O2 -std=c++17 -rdynamic -DRENDERDOC_PLATFORM_LINUX -I"$rdocSrc/renderdoc/api" \
	-o "$validator" "$here/rdoc_validate_capture.cpp" \
	-L"$(dirname -- "$rdocLib")" -lrenderdoc -Wl,-rpath,"$(dirname -- "$rdocLib")" \
	|| die "could not build rdoc_validate_capture"

# MESA_SHADER_CACHE_DISABLE is not a workaround for a bad capture, it is a
# workaround for a MESA bug: the shader disk cache is not keyed on GL profile, so
# the "#version 150 compatibility" shaders this very run just compiled in its
# compatibility context are served back from cache to RenderDoc's CORE-profile
# replay context, bypassing the "compatibility profile is not supported"
# rejection -- and then the linker segfaults on them. See
# mesa_compat_cache_repro.c, which reproduces it in 40 lines. Without this the
# replay crashes and reads exactly like an unopenable capture.
printf '[capture] opening the capture with RenderDoc (Mesa shader cache off)\n'
timeout "$runTimeout" env DISPLAY=:0 MESA_SHADER_CACHE_DISABLE=true MESA_GLSL_CACHE_DISABLE=1 \
	"$validator" "${rdcs[0]}" "$writeDir/rdoc_capture_thumb.png" \
	> "$writeDir/rdoc_validate.log" 2>&1
vrc=$?

grep -E "^(driver|local replay|thumbnail|replay|actions):" "$writeDir/rdoc_validate.log" | sed 's/^/         /'

case $vrc in
	0) printf '\n[capture] VALIDATED: RenderDoc opened and replayed the capture.\n'; exit 0 ;;
	3) printf '\n[capture] capture is valid but was not replayed here (see %s)\n' "$writeDir/rdoc_validate.log"; exit 0 ;;
	*) die "RenderDoc could not open the capture (rc=$vrc, see $writeDir/rdoc_validate.log)" ;;
esac
