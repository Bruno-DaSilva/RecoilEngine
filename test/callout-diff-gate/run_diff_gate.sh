#!/usr/bin/env bash
#
# PR 17 SimSnapshot differential gate: play a demo headless at max speed with
# /snapshotdiffgate armed. Every draw frame the engine compares every value the
# snapshot would serve (all v1 fields + the GetUnitPosition-shaped callout
# comparator) against the live sim read, bit-exact. A clean run reports
#   [SnapshotDiffGate] ... PASS (0 mismatches)
# and no per-field mismatches. Any mismatch is a snapshot contract violation
# (torn extraction, missed field, stale-row / validity bug).
#
# The demo-stream sync check runs as usual, so a clean run also doubles as a
# resim-clean gate: grep the infolog for 'DESYNC'.
#
# Usage:
#   test/callout-diff-gate/run_diff_gate.sh <demo.sdfz> <label> [spring-binary]
#
# Env overrides:
#   SPRING_DATADIR   writable data dir with BAR content (default /www/projects/bar-data)
#   DG_ARM           arm the gate; 0 = plain resim run   (default 1)
#   DG_QUIT_FRAME    hard quit frame, 0 = at game end   (default 0)
#   DG_FF            fast-forward (setspeed 20)          (default 1)
#   DG_EXERCISE      widget calls positions callouts/frame (default 1)
#   DG_POV_FRAME     frame to drop fullview for a single-team POV segment
#                    (masking coverage; 0 = never)       (default 6000)
#   DG_POV_SPAN      POV segment length in frames        (default 6000)
#   DG_POV_TEAM      team to spectate (-1 = auto)        (default -1)
#   DG_SPLIT_CONTRACT      SplitDrawContract mode: 0 off, 1 count, 2 strict (PR 27a)
#   DG_SPLIT_CONTRACT_WARN once-per-callout trip warnings (default 1)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

DEMO="${1:?usage: run_diff_gate.sh <demo.sdfz> <label> [spring-binary]}"
LABEL="${2:?usage: run_diff_gate.sh <demo.sdfz> <label> [spring-binary]}"
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
cp "$HERE/callout_diff_driver.lua" "$WIDGET_DIR/callout_diff_driver.lua"

ENABLED_ANY=0
for WCFG in "$SPRING_DATADIR"/LuaUI/Config/*.lua; do
	[[ -f "$WCFG" ]] || continue
	if grep -q '"Callout Diff Gate Driver"' "$WCFG"; then
		sed -i 's/\["Callout Diff Gate Driver"\] *= *[0-9]*,/["Callout Diff Gate Driver"] = 1000,/' "$WCFG"
		ENABLED_ANY=1
	fi
done
if [[ "$ENABLED_ANY" -eq 0 ]]; then
	echo "widget : not registered in any LuaUI/Config yet; first run registers it disabled - re-run once."
fi

# --- isolated config ------------------------------------------------------------
CFG="$(mktemp -t dgcfg.XXXXXX)"
trap 'rm -f "$CFG"' EXIT
if [[ -f "$SPRING_DATADIR/springsettings.cfg" ]]; then
	cp "$SPRING_DATADIR/springsettings.cfg" "$CFG"
fi
cat >> "$CFG" <<EOF
DiffGateLabel = $LABEL
DiffGateArm = ${DG_ARM:-1}
DiffGateFastForward = ${DG_FF:-1}
DiffGateQuitFrame = ${DG_QUIT_FRAME:-0}
DiffGateExercise = ${DG_EXERCISE:-1}
DiffGatePovFrame = ${DG_POV_FRAME:-6000}
DiffGatePovSpan = ${DG_POV_SPAN:-6000}
DiffGatePovTeam = ${DG_POV_TEAM:--1}
DiffGateCtrlPokes = ${DG_CTRL_POKES:-0}
DiffGateForce1x = ${DG_FORCE_1X:-0}
SplitDrawContract = ${DG_SPLIT_CONTRACT:-0}
SplitDrawContractWarn = ${DG_SPLIT_CONTRACT_WARN:-1}
DiffGateProfDumpStart = ${DG_PROF_START:-0}
DiffGateProfDumpEnd = ${DG_PROF_END:-0}
WorkerThreadCount = ${WORKERS:--1}
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
cp "$INFOLOG" "$SPRING_DATADIR/${LABEL}_infolog.txt" 2>/dev/null || true

echo "--- [SnapshotDiffGate] report ---"
grep -F "[SnapshotDiffGate]" "$INFOLOG" || echo "(no gate output - was it armed? is the driver widget enabled?)"

echo
# per-mismatch lines carry "field=" (boundary pass) or "callout=" + "differ"
# (serving-path dual-run comparator)
if grep -qE "\[SnapshotDiffGate\].*(field=|callout=.*differ)" "$INFOLOG"; then
	echo "GATE   : MISMATCHES DETECTED - snapshot does not match live sim"
	grep -m 5 -E "\[SnapshotDiffGate\].*(field=|callout=.*differ)" "$INFOLOG"
elif grep -q "PASS (0 mismatches)" "$INFOLOG"; then
	echo "GATE   : PASS (0 mismatches)"
else
	echo "GATE   : inconclusive - no PASS line found (check widget enablement)"
fi

if grep -q "DESYNC" "$INFOLOG"; then
	echo "SYNC   : DESYNC DETECTED - resim diverged from the demo (build/demo mismatch or a sync bug)"
	grep -m 5 "DESYNC" "$INFOLOG"
else
	echo "SYNC   : clean (no DESYNC in infolog)"
fi
