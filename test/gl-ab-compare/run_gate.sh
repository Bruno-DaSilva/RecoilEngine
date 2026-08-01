#!/usr/bin/env bash
# Whole-frame A/B compare gate runner (see README.md).
#
# A gate run that renders nothing reads exactly like a clean one -- both grep
# zero divergences -- so every failure mode below is asserted, not assumed:
# the engine STRIPS GLFrameABCompare from the write-dir config on exit, so a
# second run without re-adding it compares nothing; and a mistyped binary path
# exits 127 while a stale infolog still holds the previous run's passes. The
# minimum-compare assertion is what makes "clean" mean "compared and matched".
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

writeDir=${AB_WRITE_DIR:-}
springBin=$root/build/spring
minCompares=400
runTimeout=420
forceLegacy=0
# The engine REWRITES springsettings.cfg on exit and drops every key sitting at
# its default value, so a knob set to its default silently disappears and the
# next run measures a different configuration. Reported rather than asserted:
# gating a knob deliberately OFF is a legitimate run.
migrationKnobs=(LuaModernGLBackend LuaCmdListBakedStreams LuaCmdListSuspendOnObjectCreate ModernModelAttribs ModernModelFFShader FFVertexAttribRewrite FFMatrixSuppress)
ffExperiment=0
mixedOK=0
content=

die() { printf '\n[gate] FAIL: %s\n' "$*" >&2; exit 1; }
usage() {
	cat <<'EOF'
usage: run_gate.sh [options] <startscript|replay|watertest|idletest>
  --write-dir DIR    engine write-dir (default: $AB_WRITE_DIR)
  --spring BIN       engine binary (default: <repo>/build/spring)
  --min-compares N   fail if fewer frames were compared (default: 400)
  --timeout SEC      wall-clock limit for the run (default: 420)
  --force-legacy     [L,L,L,L] null test: control AND signal must both be 0
  --ff-experiment N  render the last pass with candidate FF removal N dropped
                     (GL::FFExperiment); implies --force-legacy so the removal is
                     the only variable. control AND signal must both be 0.
  --mixed-ok         allow --ff-experiment WITHOUT --force-legacy, so the last
                     pass varies the Lua backend AND the removal together. Sound
                     only when the backend's own signal has been measured at 0 on
                     this same content, which makes any remaining signal the
                     removal's. Answers whether a candidate is inert in the
                     SHIPPED configuration rather than under forced legacy.
EOF
}

while (( $# )); do
	case $1 in
		--write-dir)    writeDir=$2; shift 2 ;;
		--spring)       springBin=$2; shift 2 ;;
		--min-compares) minCompares=$2; shift 2 ;;
		--timeout)      runTimeout=$2; shift 2 ;;
		--force-legacy) forceLegacy=1; shift ;;
		--mixed-ok)     mixedOK=1; shift ;;
		--ff-experiment) ffExperiment=$2; shift 2 ;;
		# --mixed-ok may be given either side of --ff-experiment, so the implied
		# --force-legacy is applied after the whole line is parsed, not here.
		-h|--help)      usage; exit 0 ;;
		-*)             die "unknown option $1" ;;
		*)              content=$1; shift ;;
	esac
done

(( ffExperiment && !mixedOK )) && forceLegacy=1

[[ -n $content ]]  || die "no startscript/replay given (try: $0 watertest)"
[[ -n $writeDir ]] || die "no write-dir: pass --write-dir DIR or set AB_WRITE_DIR"

# bare gate names resolve to the committed content scripts
[[ -e $content ]] || [[ ! -e $here/${content}_startscript.txt ]] || content=$here/${content}_startscript.txt

[[ -e $content ]]      || die "content not found: $content"
[[ -x $springBin ]]    || die "engine binary not executable: $springBin"
[[ -d $writeDir ]]     || die "write-dir not a directory: $writeDir"
content=$(cd -- "$(dirname -- "$content")" && pwd)/$(basename -- "$content")

cfg=$writeDir/springsettings.cfg
infolog=$writeDir/infolog.txt

# The engine rewrites springsettings.cfg on exit and drops GLFrameABCompare, so
# these are re-applied per run rather than once. Interval is pinned to 1 so the
# compare count below equals the number of compared frames.
touch "$cfg"
for kv in "GLFrameABCompare = 1" "GLFrameABCompareDump = 1" "GLFrameABCompareInterval = 1" "GLFFRemovalExperiment = $ffExperiment"; do
	k=${kv%% =*}
	if grep -q "^$k = " "$cfg"; then
		sed -i "s|^$k = .*|$kv|" "$cfg"
	else
		printf '%s\n' "$kv" >> "$cfg"
	fi
done

: > "$infolog"

printf '[gate] %s\n' "$(basename -- "$content")"
printf '[gate] engine   %s\n' "$springBin"
printf '[gate] writedir %s\n' "$writeDir"
(( forceLegacy )) && printf '[gate] AB_FORCE_LEGACY=1 (null test)\n'
if (( ffExperiment )); then
	printf '[gate] GLFFRemovalExperiment=%s (FF removal on the last pass)\n' "$ffExperiment"
	# Without --force-legacy the last pass would differ in BOTH the Lua backend and
	# the removal, so a signal would not attribute to either.
	if (( !forceLegacy )); then
		(( mixedOK )) || die "--ff-experiment requires --force-legacy (or --mixed-ok, see usage)"
		printf '[gate] --mixed-ok: last pass varies the backend AND the removal.\n'
		printf '[gate]   A signal only attributes to the removal if the backend measures 0 on this content.\n'
	fi
fi

# the engine tests AB_FORCE_LEGACY for PRESENCE, so it must be absent (not 0)
# for a normal run
legacyEnv=()
(( forceLegacy )) && legacyEnv=(AB_FORCE_LEGACY=1)

env DISPLAY=:0 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
	AB_DUMP_MIN_PIXELS=0 "${legacyEnv[@]}" \
	timeout "$runTimeout" "$springBin" --isolation --write-dir "$writeDir" "$content" \
	> "$writeDir/gate_stdout.txt" 2>&1
rc=$?

# A crash or a timeout can leave a log full of clean compares from before the
# failure point, so the exit code is a gate of its own.
case $rc in
	0)   ;;
	124) die "run TIMED OUT after ${runTimeout}s (rc=124)" ;;
	126|127) die "engine binary could not be executed (rc=$rc) -- check --spring path" ;;
	*)   die "engine exited rc=$rc (tail of $writeDir/gate_stdout.txt: $(tail -n 3 "$writeDir/gate_stdout.txt" | tr '\n' ' '))" ;;
esac

[[ -s $infolog ]] || die "empty infolog: $infolog"

# The engine echoes only explicitly-set, non-default keys, so a knob missing here
# is one that reverted between runs -- which reads as a result rather than as an
# error, and is how a meter reading jumped 16 -> 26 once.
knobState=
for k in "${migrationKnobs[@]}"; do
	if grep -qE "^\[[^]]*\]  *$k = 1\$|  $k = 1\$" "$infolog"; then
		knobState+=" $k=1"
	else
		knobState+=" $k=off"
	fi
done
printf '[gate] knobs:%s\n' "$knobState"

# A shader that failed to compile draws nothing in EVERY pass alike, so the
# compare below reports a clean run over a broken frame -- measured: 646 frames
# control 0 / signal 0 while BAR's whole unit-material set was dead. That reads
# exactly like a pass, which makes it the worst of the vacuous-clean class.
if grep -qE 'shader error\(s\)|shader errors:|FFVertexAttribRewrite broke' "$infolog"; then
	printf '\n[gate] shader compile failures:\n'
	grep -E 'shader error\(s\)|shader errors:|FFVertexAttribRewrite broke' "$infolog" | head -n 10
	die "shader(s) failed to compile -- whatever this run compared, it was not the frame"
fi

# [Frame A/B] control(L<->L)=N px (max D), signal(L<->M)=N / T px (masked, max D)
# -- stripped of the log prefix, the digits are exactly (ctl, ctlMax, sig, total, sigMax)
read -r compares ctlFrames ctlMax sigFrames sigMax malformed <<<"$(
	awk '/\[Frame A\/B\] control/ {
		line = $0
		sub(/.*\[Frame A\/B\] /, "", line)
		gsub(/[^0-9]+/, " ", line)
		if (split(line, f, " ") < 5) { malformed++; next }
		compares++
		if (f[1] + 0 > 0) { ctlFrames++; if (f[2] + 0 > ctlMax) ctlMax = f[2] + 0 }
		if (f[3] + 0 > 0) { sigFrames++; if (f[5] + 0 > sigMax) sigMax = f[5] + 0 }
	}
	END { printf "%d %d %d %d %d %d\n", compares, ctlFrames, ctlMax, sigFrames, sigMax, malformed }' "$infolog"
)"

printf '[gate] compared %s frames: control nonzero on %s (max delta %s), signal nonzero on %s (max delta %s)\n' \
	"$compares" "$ctlFrames" "$ctlMax" "$sigFrames" "$sigMax"

(( malformed == 0 )) || die "$malformed unparsable [Frame A/B] lines"
(( compares >= minCompares )) || die "only $compares frames compared (min $minCompares) -- the gate did not run; is GLFrameABCompare set and did the run reach in-game?"

if (( ctlFrames || sigFrames )); then
	printf '\n'
	grep -E '\[Frame A/B\] (control|pairwise)' "$infolog" |
		grep -A1 -E 'control\(L<->L\)=[1-9]|signal\(L<->M\)=[1-9]' | head -n 20
	# The documented async flake perturbs ONE random pass of the four. On pass 1
	# or 2 it shows up as control and its pixels are masked out of the signal; on
	# pass 3 -- the modern pass -- there is nothing to mask it against, so it
	# reads as pure signal on a single frame. A real backend divergence is
	# deterministic, so re-running separates them.
	if (( sigFrames > 0 && sigFrames <= 2 && ctlFrames == 0 )); then
		printf '\n[gate] NOTE: a signal this small with a clean control can also be the async\n'
		printf '[gate]       flake landing on the modern pass (see doc/bar-gl4-immediate-mode-inventory.md).\n'
		printf '[gate]       Re-run: a real divergence repeats, the flake does not.\n'
	fi

	(( sigFrames == 0 )) || die "signal(L<->M) nonzero on $sigFrames/$compares frames -- modern backend diverges"
	die "control(L<->L) nonzero on $ctlFrames/$compares frames -- content leaks across passes (see the flake note in doc/bar-gl4-immediate-mode-inventory.md)"
fi

printf '[gate] PASS\n'
