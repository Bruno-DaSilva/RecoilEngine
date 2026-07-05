#!/usr/bin/env python3
"""Analyze S0 measurement outputs (boundary CSV + callout census).

Usage: analyze_s0.py <label_boundary.csv> [label_callouts.csv]

Reports full-run and last-third statistics for the boundary columns plus the
derived stream-sizing numbers the replay-seeking doc needs, and the top-30
draw-context callouts from the census.
"""
import sys
import csv
import statistics as st


def pct(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p))]


def col_stats(rows, name):
    vals = [r[name] for r in rows]
    return {
        "mean": st.fmean(vals) if vals else 0.0,
        "p50": pct(vals, 0.50),
        "p95": pct(vals, 0.95),
        "p99": pct(vals, 0.99),
        "max": max(vals) if vals else 0.0,
    }


def fmt(v):
    if v >= 1e6:
        return f"{v/1e6:.2f}M"
    if v >= 1e4:
        return f"{v/1e3:.1f}k"
    if isinstance(v, float) and v < 100:
        return f"{v:.2f}"
    return f"{v:.0f}"


def report_block(title, rows, cols):
    print(f"\n--- {title} ({len(rows)} frames) ---")
    print(f"{'column':<28}{'mean':>10}{'p50':>10}{'p95':>10}{'p99':>10}{'max':>10}")
    for c in cols:
        s = col_stats(rows, c)
        print(f"{c:<28}{fmt(s['mean']):>10}{fmt(s['p50']):>10}{fmt(s['p95']):>10}{fmt(s['p99']):>10}{fmt(s['max']):>10}")


def main():
    bpath = sys.argv[1]
    cpath = sys.argv[2] if len(sys.argv) > 2 else None

    rows = []
    with open(bpath) as fh:
        rd = csv.DictReader(fh)
        for r in rd:
            rows.append({k: float(v) for k, v in r.items()})

    if not rows:
        print("no rows")
        return

    # derived columns
    tra_elem_size = rows[-1]["tra_bytes"] / max(1, rows[-1]["tra_elems"])
    for r in rows:
        r["cmd_mutations"] = (r["d_cmd_pushback"] + r["d_cmd_pushfront"] + r["d_cmd_insert"]
                              + r["d_cmd_popback"] + r["d_cmd_popfront"] + r["d_cmd_erase"] + r["d_cmd_clear"])
        r["piece_delta_bytes"] = r["d_piece_changed"] * tra_elem_size
        r["piece_churn_pct"] = 100.0 * r["d_piece_changed"] / max(1, r["d_piece_sampled"])
        r["obj_churn_pct"] = 100.0 * r["d_obj_piecechanged"] / max(1, r["d_obj_sampled"])
        # exact-change hot-field delta payload (field bytes only, no keys/framing)
        r["hf_delta_bytes"] = (r["hf_pos_changed"] * 12 + r["hf_speed_changed"] * 16
                               + r["hf_health_changed"] * 4 + r["hf_heading_changed"] * 2
                               + r["hf_build_changed"] * 4)
        r["proj_spawns"] = (r["d_proj_spawn_piece"] + r["d_proj_spawn_hitscan"] + r["d_proj_spawn_guided"]
                            + r["d_proj_spawn_ballistic"] + r["d_proj_spawn_syncother"] + r["d_proj_spawn_unsynced"])

    cols = [
        "units_active", "feats_live", "projs_synced", "projs_unsynced", "particles",
        "tra_elems", "tra_bytes", "uni_elems", "uni_bytes",
        "d_tra_checked", "d_tra_changed", "d_tra_forced",
        "d_piece_sampled", "d_piece_changed", "piece_churn_pct", "piece_delta_bytes",
        "d_obj_sampled", "d_obj_piecechanged", "obj_churn_pct", "d_obj_moved",
        "cmd_mutations", "cmdq_cmds_total", "cmdq_nonempty", "cmdq_max",
        "proj_spawns", "d_proj_spawn_piece", "d_proj_spawn_hitscan", "d_proj_spawn_guided",
        "d_proj_spawn_ballistic", "d_proj_spawn_unsynced",
        "d_unit_created", "d_unit_destroyed", "d_feat_created", "d_feat_destroyed",
        "d_los_enter_los", "d_los_leave_los", "d_los_enter_radar", "d_los_leave_radar",
        "hf_units_compared", "hf_pos_changed", "hf_speed_changed", "hf_health_changed",
        "hf_heading_changed", "hf_build_changed", "hf_delta_bytes",
        "sample_cost_ms",
    ]

    print(f"boundary file: {bpath}")
    print(f"frames {rows[0]['frame']:.0f}..{rows[-1]['frame']:.0f}; transform element size = {tra_elem_size:.0f} B")
    report_block("full run", rows, cols)
    third = len(rows) // 3
    report_block("last third", rows[-third:], cols)

    if cpath:
        meta = {}
        crows = []
        with open(cpath) as fh:
            first = fh.readline()
            if first.startswith("#"):
                for tok in first[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=")
                        meta[k] = int(v)
                header = fh.readline()
            else:
                header = first
            names = header.strip().split(",")
            for line in fh:
                p = line.rstrip("\n").split(",")
                # name may contain commas -> parse from the right
                vals = p[-4:]
                nm = ",".join(p[:-4])
                crows.append((nm, *[int(x) for x in vals]))

        sf = max(1, meta.get("simFrames", 1))
        df = max(1, meta.get("drawFrames", 1))
        print(f"\n=== callout census: {cpath} (simFrames={sf}, drawFrames={df}) ===")
        print(f"{'callout':<34}{'count':>12}{'draw':>12}{'sim':>12}{'other':>12}{'draw/drawfrm':>14}")
        for nm, cnt, cd, cs, co in crows[:30]:
            print(f"{nm:<34}{cnt:>12}{cd:>12}{cs:>12}{co:>12}{cd/df:>14.1f}")

        tot = sum(r[1] for r in crows)
        totd = sum(r[2] for r in crows)
        tots = sum(r[3] for r in crows)
        print(f"\ntotals: {tot} callouts, draw {totd} ({100*totd/max(1,tot):.1f}%), "
              f"sim {tots} ({100*tots/max(1,tot):.1f}%), other {tot-totd-tots}")


if __name__ == "__main__":
    main()
