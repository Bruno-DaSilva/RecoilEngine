#!/usr/bin/env bash
# Resolve the per-call-site census (RDOC_SITE_CENSUS=1) to source lines + counts.
#
# This is the actual burn-down work list: every site that reached a
# RenderDoc-unsupported function on that run, with how many times. Static grep
# cannot produce it -- most legacy call sites in the engine are unreachable for
# any given game, and only the reached ones block capture.
#
#   RDOC_SITE_CENSUS=1 AB_WRITE_DIR=... run_rdoc_offenders.sh
#   AB_WRITE_DIR=... rdoc_site_census.sh [--by func|count] [--only FUNC]
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

log=${AB_WRITE_DIR:+$AB_WRITE_DIR/rdoc_offenders.log}
springBin=$root/build/spring
only=
by=count

die() { printf '\n[census] FAIL: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
	case $1 in
		--log)    log=$2; shift 2 ;;
		--spring) springBin=$2; shift 2 ;;
		--only)   only=$2; shift 2 ;;
		--by)     by=$2; shift 2 ;;
		*)        die "unknown option $1" ;;
	esac
done

[[ -n ${log:-} ]]   || die "no log: pass --log FILE or set AB_WRITE_DIR"
[[ -r $log ]]       || die "log not readable: $log"
[[ -x $springBin ]] || die "engine binary not executable: $springBin"
grep -q '^\[RDOC-SITE\] ' "$log" \
	|| die "no [RDOC-SITE] lines in $log -- re-run the meter with RDOC_SITE_CENSUS=1"

springReal=$(readlink -f "$springBin")

# Resolve every engine-binary site in one addr2line pass; librenderdoc/libGL/libc
# frames are interception plumbing, never a call site to convert.
rows=$(grep '^\[RDOC-SITE\] ' "$log" | while read -r _tag count func frame; do
	addr=${frame##*+}
	module=${frame%+*}
	[[ $(readlink -f "$module" 2>/dev/null) == "$springReal" ]] || { printf '%s\t%s\t%s\n' "$count" "$func" "(non-engine)"; continue; }
	printf '%s\t%s\t%s\n' "$count" "$func" "$addr"
done)

# Resolve each distinct address on its own: -i yields a variable-length inline
# chain, so a batch call cannot be indexed back to its address. The innermost
# frame is often an inlined STL accessor that merely happens to hold the return
# address, so take the OUTERMOST non-STL frame -- that is the function whose
# source actually contains the call.
mapfile -t addrs < <(printf '%s\n' "$rows" | cut -f3 | grep '^0x' | sort -u)
declare -A loc
for a in "${addrs[@]}"; do
	best=
	while read -r fn; do
		read -r l
		[[ $fn == "??" && $l == "??:0" ]] && continue
		[[ $l == /usr/include/* || $l == /usr/lib/* ]] && continue
		best="${l#$root/} ($fn)"
	done < <(addr2line -f -C -i -e "$springReal" "$a" 2>/dev/null)
	loc[$a]=${best:-$a}
done

printf '%s\n' "$rows" | while IFS=$'\t' read -r count func addr; do
	[[ -n $only && $func != "$only" ]] && continue
	printf '%10s  %-24s %s\n' "$count" "$func" "${loc[$addr]:-$addr}"
done | if [[ $by == func ]]; then sort -k2,2 -k1,1rn; else sort -k1,1rn; fi
