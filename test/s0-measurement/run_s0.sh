#!/usr/bin/env bash
#
# S0 measurement pass: play a demo headless at max speed with the boundary-size
# instrumentation armed, collecting:
#   <label>_boundary.csv  - /boundarydump, one row per sim frame (full game)
#   <label>_callouts.csv  - /calloutcensus, cumulative per-callout context split
#   <label>_profile.csv   - optional /profiledump window (S0_PROF_START)
# All outputs land in $SPRING_DATADIR. The demo-stream sync check runs as usual,
# so a clean run doubles as the "resims clean with instrumentation" gate:
# grep infolog for 'DESYNC' afterwards.
#
# Usage:
#   test/s0-measurement/run_s0.sh <demo.sdfz> <label> [spring-binary]
#
# Env overrides:
#   SPRING_DATADIR   writable data dir with BAR content (default /www/projects/bar-data)
#   S0_DUMP_END      boundarydump end frame            (default 200000)
#   S0_PROF_START    profiledump start frame, -1 = off (default -1)
#   S0_PROF_LEN      profiledump window length         (default 900)
#   S0_QUIT_FRAME    hard quit frame, 0 = at game end  (default 0)
#   S0_CALLOUTS      LuaTrackCalloutCounts level       (default 1)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

DEMO="${1:?usage: run_s0.sh <demo.sdfz> <label> [spring-binary]}"
LABEL="${2:?usage: run_s0.sh <demo.sdfz> <label> [spring-binary]}"
SPRING_DATADIR="${SPRING_DATADIR:-/www/projects/bar-data}"

SPRING_BIN="${3:-${SPRING_BIN:-}}"
if [[ -z "$SPRING_BIN" ]]; then
	# exclude install trees: build/install/ may hold a stale engine from a
	# different branch (this once silently invalidated a whole gate run)
	SPRING_BIN="$(find "$REPO/build" -maxdepth 3 -type f -name spring-headless -not -path "*/install/*" 2>/dev/null | head -n1 || true)"
fi
if [[ -z "$SPRING_BIN" || ! -x "$SPRING_BIN" ]]; then
	echo "ERROR: no spring-headless binary found; build it or pass as arg 3 / \$SPRING_BIN" >&2
	exit 1
fi
echo "engine : $SPRING_BIN"
echo "demo   : $DEMO"
echo "label  : $LABEL"

# --- install + enable the driver widget ----------------------------------------
WIDGET_DIR="$SPRING_DATADIR/LuaUI/Widgets"
mkdir -p "$WIDGET_DIR"
cp "$HERE/s0_stats_driver.lua" "$WIDGET_DIR/s0_stats_driver.lua"

ENABLED_ANY=0
for WCFG in "$SPRING_DATADIR"/LuaUI/Config/*.lua; do
	[[ -f "$WCFG" ]] || continue
	if grep -q '"S0 Stats Driver"' "$WCFG"; then
		sed -i 's/\["S0 Stats Driver"\] *= *[0-9]*,/["S0 Stats Driver"] = 1000,/' "$WCFG"
		ENABLED_ANY=1
	fi
done
if [[ "$ENABLED_ANY" -eq 0 ]]; then
	echo "widget : not registered in any LuaUI/Config yet; first run registers it disabled - re-run once."
fi

# --- isolated config ------------------------------------------------------------
CFG="$(mktemp -t s0cfg.XXXXXX)"
trap 'rm -f "$CFG"' EXIT
if [[ -f "$SPRING_DATADIR/springsettings.cfg" ]]; then
	cp "$SPRING_DATADIR/springsettings.cfg" "$CFG"
fi
cat >> "$CFG" <<EOF
LuaTrackCalloutCounts = ${S0_CALLOUTS:-1}
S0Label = $LABEL
S0Boundary = ${S0_BOUNDARY:-1}
S0DumpEndFrame = ${S0_DUMP_END:-200000}
S0ProfStart = ${S0_PROF_START:--1}
S0ProfLen = ${S0_PROF_LEN:-900}
S0QuitFrame = ${S0_QUIT_FRAME:-0}
S0FastForward = ${S0_FF:-1}
WorkerThreadCount = ${WORKERS:--1}
SplitDrawContract = ${SPLIT_CONTRACT:-0}
SplitDrawContractWarn = ${SPLIT_CONTRACT_WARN:-1}
SimDrawSplit = ${SIM_DRAW_SPLIT:-0}
EOF

DEMO_ABS="$(cd "$(dirname "$DEMO")" && pwd)/$(basename "$DEMO")"
INFOLOG="$SPRING_DATADIR/infolog.txt"

echo "== launching replay =="
set +e
"$SPRING_BIN" --isolation --write-dir "$SPRING_DATADIR" --config "$CFG" "$DEMO_ABS"
RC=$?
set -e
echo "== engine exited rc=$RC =="

echo
echo "============ RESULTS ============"
for f in "$SPRING_DATADIR/${LABEL}_boundary.csv" "$SPRING_DATADIR/${LABEL}_callouts.csv" "$SPRING_DATADIR/${LABEL}_profile.csv"; do
	if [[ -f "$f" ]]; then
		echo "output : $f ($(wc -l < "$f") lines)"
	fi
done
if grep -q "DESYNC" "$INFOLOG"; then
	echo "SYNC   : DESYNC DETECTED - instrumentation is NOT sync-safe (or demo/build mismatch)"
	grep -m 5 "DESYNC" "$INFOLOG"
else
	echo "SYNC   : clean (no DESYNC in infolog)"
fi
cp "$INFOLOG" "$SPRING_DATADIR/${LABEL}_infolog.txt" 2>/dev/null || true
