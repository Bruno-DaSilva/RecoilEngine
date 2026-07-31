#!/usr/bin/env bash
# Resolve each RenderDoc-unsupported function's FIRST-USE call site to source lines.
#
# run_rdoc_offenders.sh answers "which functions are left"; this answers "where",
# which is the part that used to cost a grep-and-guess. It needs the extended
# renderdoc-unsupported-log.patch (the one that also emits [RDOC-UNSUPPORTED-BT]
# frames) -- see that script's header for the librenderdoc build recipe.
#
# Reads the log a meter run already produced, so the normal loop is:
#   run_rdoc_offenders.sh && / || rdoc_offender_sites.sh
#
# usage: rdoc_offender_sites.sh [--log FILE] [--spring BIN] [--only FUNC]
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

log=${AB_WRITE_DIR:+$AB_WRITE_DIR/rdoc_offenders.log}
springBin=$root/build/spring
only=

die() { printf '\n[sites] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--log)    log=$2; shift 2 ;;
		--spring) springBin=$2; shift 2 ;;
		--only)   only=$2; shift 2 ;;
		*)        die "unknown option $1" ;;
	esac
done

[[ -n ${log:-} ]] || die "no log: pass --log FILE or set AB_WRITE_DIR"
[[ -r $log ]]     || die "log not readable: $log (run run_rdoc_offenders.sh first)"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"

grep -q '^\[RDOC-UNSUPPORTED-BT\] ' "$log" \
	|| die "no [RDOC-UNSUPPORTED-BT] frames in $log -- librenderdoc predates the backtrace patch, rebuild it"

command -v addr2line >/dev/null || die "addr2line not found (binutils)"

springReal=$(readlink -f "$springBin")

# Frames arrive as: <func> <module-path> +0xoffset. Only frames from the engine
# binary itself can be resolved to source; librenderdoc/libGL frames are the
# interception plumbing and are noise here.
declare -A seen
current=

while read -r _tag func frame; do
	[[ -n $only && $func != "$only" ]] && continue

	if [[ $func != "$current" ]]; then
		current=$func
		printf '\n=== %s\n' "$func"
	fi

	# frame is "<module-path>+0x<offset>" -- the path itself may contain '+',
	# so split on the LAST '+'.
	addr=${frame##*+}
	module=${frame%+*}
	[[ $(readlink -f "$module" 2>/dev/null) == "$springReal" ]] || continue

	key=$func$addr
	[[ -n ${seen[$key]:-} ]] && continue
	seen[$key]=1

	# -f -C -i: function name, demangled, including inlined frames
	while read -r fn; do
		read -r loc
		[[ $fn == "??" && $loc == "??:0" ]] && continue
		# Inlined STL frames are an artifact of where the return address landed,
		# never the call site being looked for.
		[[ $loc == /usr/include/* || $loc == /usr/lib/* ]] && continue
		# Trim the absolute build prefix so lines are clickable repo paths.
		printf '    %-44s %s\n' "$fn" "${loc#$root/}"
	done < <(addr2line -f -C -i -e "$springReal" "$addr" 2>/dev/null)
done < <(grep '^\[RDOC-UNSUPPORTED-BT\] ' "$log")

printf '\n'
