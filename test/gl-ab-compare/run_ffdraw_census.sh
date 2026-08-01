#!/usr/bin/env bash
# Which draws still reach the FIXED-FUNCTION pipeline (no program bound)?
#
# That set decides whether the remaining fixed-function state families can be
# retired at all: alpha test, fog, clip planes, line stipple and the current
# color are read by the RASTERIZER on those draws, so no shader-side
# substitution reaches them. Engine-side counter (config GLFFDrawCensus, see
# rts/Rendering/GL/FFDrawCensus.cpp) plus the address resolution below.
#
# The knobs matter more here than anywhere else, because every one of them
# moves a whole family of draws off fixed function -- and the engine REWRITES
# springsettings.cfg on exit, dropping every key that sits at its default. A
# run whose knobs silently reverted reads as "lots of fixed-function draws
# left" and is indistinguishable from the real thing, so this re-applies them
# per run and then asserts the engine echoed each one back.
#
# usage: run_ffdraw_census.sh [--write-dir DIR] [--spring BIN] [--timeout SEC]
#                             [--knobs-off] [<startscript>]
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

writeDir=${AB_WRITE_DIR:-}
springBin=$root/build/spring
runTimeout=420
content=$here/watertest_startscript.txt
knobsOn=1

die() { printf '\n[ffdraw] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--write-dir) writeDir=$2; shift 2 ;;
		--spring)    springBin=$2; shift 2 ;;
		--timeout)   runTimeout=$2; shift 2 ;;
		--knobs-off) knobsOn=0; shift ;;
		-*)          die "unknown option $1" ;;
		*)           content=$1; shift ;;
	esac
done

[[ -n $writeDir ]]  || die "no write-dir: pass --write-dir DIR or set AB_WRITE_DIR"
[[ -d $writeDir ]]  || die "write-dir not a directory: $writeDir"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"
[[ -e $content ]]   || die "content not found: $content"

content=$(cd -- "$(dirname -- "$content")" && pwd)/$(basename -- "$content")
cfg=$writeDir/springsettings.cfg

cfgSet() { # substitute in place or append; never silently miss
	if grep -qE "^$1 *=" "$cfg" 2>/dev/null; then
		sed -i -E "s/^$1 *=.*/$1 = $2/" "$cfg"
	else
		printf '%s = %s\n' "$1" "$2" >> "$cfg"
	fi
	grep -qE "^$1 = $2\$" "$cfg" || die "could not set $1=$2 in $cfg"
}

cfgSet GLFFDrawCensus 1
cfgSet GLFrameABCompare 0
cfgSet GLFrameABCompareDump 0
cfgSet ABUnitShapeDriver 1
knobs=(LuaCmdListBakedStreams LuaCmdListSuspendOnObjectCreate ModernModelAttribs ModernModelFFShader FFVertexAttribRewrite)
for k in "${knobs[@]}"; do
	cfgSet "$k" "$knobsOn"
done

# Same persisted-disabled trap as the offender meter: BAR writes order=0 for a
# widget that removed itself, and one run with the driver off disables it for
# every run after.
byar=$writeDir/LuaUI/Config/BYAR.lua
install -D -m644 "$here/ab_unitshape_driver.lua" "$writeDir/LuaUI/Widgets/ab_unitshape_driver.lua" \
	|| die "could not install the unit-shape driver widget"
if [[ -f $byar ]] && grep -q '\["AB Gate UnitShape Driver"\] *= *0,' "$byar"; then
	sed -i 's/\["AB Gate UnitShape Driver"\] *= *0,/["AB Gate UnitShape Driver"] = 1,/' "$byar"
fi

infolog=$writeDir/infolog.txt
: > "$infolog"

printf '[ffdraw] %s (knobs=%d)\n' "$(basename -- "$content")" "$knobsOn"
timeout "$runTimeout" env DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
	"$springBin" --isolation --write-dir "$writeDir" "$content" > "$writeDir/ffdraw_run.log" 2>&1
rc=$?
case $rc in
	0)   ;;
	124) die "run TIMED OUT after ${runTimeout}s" ;;
	*)   die "engine exited rc=$rc (tail: $(tail -n 3 "$writeDir/ffdraw_run.log" | tr '\n' ' '))" ;;
esac

# The engine echoes every explicitly-set config key at startup; a knob that
# reverted to its default is simply absent, which is exactly the failure this
# script exists to make loud.
(( knobsOn )) && for k in "${knobs[@]}"; do
	grep -qE "^\[[^]]*\]\s+$k = 1\$|  $k = 1\$" "$infolog" \
		|| die "$k did not reach the engine (config rewritten between runs?) -- the census would be measuring a different configuration"
done

grep -q '\[FFDrawCensus\] ACTIVE' "$infolog" || die "the census never installed -- GLFFDrawCensus did not reach the engine"
grep -q '\[AB UnitShape Driver\] active' "$infolog" \
	|| die "the unit-shape driver never activated -- the legacy model path is unmeasured, and it is the biggest fixed-function draw site there is"

# The census wraps every draw entry point, so leaving it on silently
# instruments whatever runs next out of the same write-dir.
cfgSet GLFFDrawCensus 0

summary=$(grep -o '\[FFDrawCensus\] .* draws total' "$infolog" | tail -n 1)
[[ -n $summary ]] || die "no census summary in the infolog -- the run did not reach shutdown"
printf '[ffdraw] %s\n' "$summary"

springReal=$(readlink -f "$springBin")
grep -h '\[FFDRAW-SITE\] ' "$infolog" | sed 's/^.*\[FFDRAW-SITE\] //' | sort -k1,1rn | while read -r count fn frames; do
	printf '%10s  %s\n' "$count" "$fn"
	for frame in $frames; do
		module=${frame%+*}
		addr=${frame##*+}
		[[ $(readlink -f "$module" 2>/dev/null) == "$springReal" ]] || continue
		best=
		while read -r sym; do
			read -r loc
			[[ $sym == "??" && $loc == "??:0" ]] && continue
			[[ $loc == /usr/include/* || $loc == /usr/lib/* ]] && continue
			best="${loc#$root/} ($sym)"
		done < <(addr2line -f -C -i -e "$springReal" "$addr" 2>/dev/null)
		[[ -n $best ]] && printf '              %s\n' "$best"
	done
done
