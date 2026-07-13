#!/usr/bin/env bash
#
# Instrumentation-overhead measurement: replay the same demo segment three ways
# and compare wall-clock sim throughput (frames/sec between frame 1 and QUIT).
#
#   base   - pre-instrumentation binary (no BoundaryStats/census code at all)
#   idle   - instrumented binary, counters compiled in but nothing armed
#   active - instrumented binary, /boundarydump armed + LuaTrackCalloutCounts=1
#
# Usage: run_overhead.sh <demo.sdfz> <base-binary> <instr-binary> [quitframe]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEMO="${1:?usage: run_overhead.sh <demo> <base-bin> <instr-bin> [quitframe]}"
BASE_BIN="${2:?}"
INSTR_BIN="${3:?}"
QUIT="${4:-8000}"
SPRING_DATADIR="${SPRING_DATADIR:-/www/projects/bar-data}"

run_one() {
	local name="$1" bin="$2" callouts="$3" boundary="$4"
	echo "== $name =="
	S0_QUIT_FRAME=$QUIT S0_CALLOUTS=$callouts S0_BOUNDARY=$boundary S0_PROF_START=-1 \
		SPRING_BIN="$bin" "$HERE/run_s0.sh" "$DEMO" "ovh_$name" >/dev/null 2>&1 || true
	# wall time between the first and last sim frame, from infolog timestamps
	python3 - "$SPRING_DATADIR/ovh_${name}_infolog.txt" "$QUIT" <<'EOF'
import re, sys
t0 = t1 = f1 = None
pat = re.compile(r"\[t=(\d+):(\d+):(\d+)\.(\d+)\]\[f=(\d+)\]")
for line in open(sys.argv[1], errors="ignore"):
    m = pat.match(line)
    if not m:
        continue
    h, mnt, s, us, f = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5])
    t = h*3600 + mnt*60 + s + us/1e6
    if f >= 1 and t0 is None:
        t0 = t
    t1, f1 = t, f
print(f"  frames 1..{f1}  wall {t1-t0:.1f}s  {f1/(t1-t0):.1f} frames/s  {1000*(t1-t0)/f1:.3f} ms/frame")
EOF
}

run_one base   "$BASE_BIN"  0 0
run_one idle   "$INSTR_BIN" 0 0
run_one active "$INSTR_BIN" 1 1
