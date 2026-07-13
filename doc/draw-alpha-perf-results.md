# DrawAlpha particle perf — benchmark results

Fixed-workload replay, map "Hellas Basin v1.4", 5000 sim frames (~332 s wall each).
Sim is locked to ~15 fps, so the metric is per-frame **draw** cost; render FPS = draw frame count / wall time.

## Cumulative (vs master)

| Build | Draw mean (ms) | Δ mean vs master | Render FPS | Δ FPS vs master |
|---|---|---|---|---|
| master | 12.13 | — | 46.0 | — |
| + Tier 1 (per-camera build cache) | 9.13 | −24.7% | 59.8 | +29.8% |
| + Tier 2 (parallel billboard fill) | 7.12 | −41.3% | 72.9 | +58.3% |
| + Tier 3 (radix sort) | 6.37 | −47.5% | 81.3 | +76.5% |

## Marginal (each tier vs the one before)

| Step | Draw mean (ms) | Δ mean | Render FPS | Δ FPS |
|---|---|---|---|---|
| master → Tier 1 | 12.13 → 9.13 | −24.7% | 46.0 → 59.8 | +29.8% |
| Tier 1 → Tier 2 | 9.13 → 7.12 | −22.0% | 59.8 → 72.9 | +21.9% |
| Tier 2 → Tier 3 | 7.12 → 6.37 | −10.5% | 72.9 → 81.3 | +11.5% |

Net: per-frame draw cost roughly halved (−47.5%); render framerate +76.5%; no sim regression (sim mean ~16 ms throughout).
