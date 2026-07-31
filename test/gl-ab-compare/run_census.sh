#!/usr/bin/env bash
# Fallback-reason census runner: which gate rejected each modern Lua immediate
# flush, per Lua draw mode (config LuaImmediateFallbackStats, dumped by the
# "/luaimmfallback" action -- censustest_startscript.txt fires it from
# debugcommands at sim frame 880, then quits).
#
# This is the measurement that ranks the remaining conversion work, so it runs
# with the A/B gate OFF: the gate renders each frame 4x and would multiply every
# tally by the pass count.
#
# usage: run_census.sh [--write-dir DIR] [--spring BIN] [--timeout SEC] [<startscript>]
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

writeDir=${AB_WRITE_DIR:-}
springBin=$root/build/spring
runTimeout=420
content=$here/censustest_startscript.txt

die() { printf '\n[census] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--write-dir) writeDir=$2; shift 2 ;;
		--spring)    springBin=$2; shift 2 ;;
		--timeout)   runTimeout=$2; shift 2 ;;
		-*)          die "unknown option $1" ;;
		*)           content=$1; shift ;;
	esac
done

[[ -n $writeDir ]]  || die "no write-dir: pass --write-dir DIR or set AB_WRITE_DIR"
[[ -e $content ]]   || die "content not found: $content"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"
[[ -d $writeDir ]]  || die "write-dir not a directory: $writeDir"
content=$(cd -- "$(dirname -- "$content")" && pwd)/$(basename -- "$content")

cfg=$writeDir/springsettings.cfg
infolog=$writeDir/infolog.txt

touch "$cfg"
for kv in "LuaImmediateFallbackStats = 1" "GLFrameABCompare = 0"; do
	k=${kv%% =*}
	if grep -q "^$k = " "$cfg"; then
		sed -i "s|^$k = .*|$kv|" "$cfg"
	else
		printf '%s\n' "$kv" >> "$cfg"
	fi
done

: > "$infolog"

printf '[census] %s\n' "$(basename -- "$content")"
env DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
	timeout "$runTimeout" "$springBin" --isolation --write-dir "$writeDir" "$content" \
	> "$writeDir/census_stdout.txt" 2>&1
rc=$?

case $rc in
	0)   ;;
	124) die "run TIMED OUT after ${runTimeout}s (rc=124)" ;;
	126|127) die "engine binary could not be executed (rc=$rc) -- check --spring path" ;;
	*)   die "engine exited rc=$rc (tail of $writeDir/census_stdout.txt: $(tail -n 3 "$writeDir/census_stdout.txt" | tr '\n' ' '))" ;;
esac

# an empty census reads like "nothing falls back"; it far more often means the
# dump action never fired or the config key was stripped
grep -q 'LuaImmFallback' "$infolog" || die "no census in $infolog -- is LuaImmediateFallbackStats set and does the startscript fire 'luaimmfallback'?"

grep 'LuaImmFallback' "$infolog"
