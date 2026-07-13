# ProjectileDrawer::DrawAlpha — per-camera build cache (Tier 1)

## Goal
Build the sorted alpha-particle geometry once per *camera* instead of once per *pass*, so the heavy DP+SO+DS work drops from 4× to 2× per frame.

## Why this works
`DrawAlpha` runs 4× per frame (water on): below-water, above-water, reflection, refraction. The per-camType draw-flag assignment sets `SO_ALPHAF_FLAG` and `SO_REFRAC_FLAG` in the `CAMTYPE_PLAYER` branch and `SO_REFLEC_FLAG` in the `CAMTYPE_UWREFL` branch. There is no refraction camera — refraction renders from the player camera, clipped below water.

So 3 of the 4 passes share the player camera (below-water, above-water, refraction) and one uses the mirrored UWREFL camera (reflection). Passes sharing a camera produce identical billboards and identical sort order; they differ only in which subset they draw and which GPU clip plane / soften state they bind. `REFRAC` is a subset of `ALPHAF`, and a subset of a back-to-front-sorted list is still sorted — so the subset passes need no re-sort.

Net: the player-camera build serves 3 passes, the UWREFL build serves 1.

## Tier 1 plan (no vertex-format change)

### Build once per camera (PLAYER, UWREFL)
- [ ] DP: collect visible alpha projectiles for the camera into the sorted/unsorted buckets.
- [ ] SO: sort the bucket once by that camera's `sortDist`.
- [ ] DS: generate quads once into a *retained* render buffer (don't clear after submit).
- [ ] During DS, record per projectile: `baseVertex`, `vertexCount`, `drawFlags`.

### Per render pass (4): take what you want
- [ ] Select the matching camera's prebuilt buffer (PLAYER for below/above/refraction, UWREFL for reflection).
- [ ] Build a lightweight index buffer by walking the sorted list and emitting the recorded vertex ranges for projectiles whose flag matches the pass mask (`ALPHAF` / `REFRAC` / `REFLEC`). Order is preserved for free.
- [ ] Submit with the pass's own clip plane + soften state.

## Required plumbing
- [ ] Per-projectile vertex-range bookkeeping captured during DS.
- [ ] Retain the render buffer across passes instead of clearing after each RR submit (the buffer is already a persistent verts/indcs pair).
- [ ] Per-pass index range / sub-index-buffer submit path.
- [ ] Decide where the two builds live in the frame so both cameras' data is current before any pass consumes it (player build before WorldDrawer alpha; UWREFL build before IWater reflection).

## Out of scope (Tier 2)
Making reflection reuse the player build too. That needs camera-agnostic per-particle records + GPU-side billboarding (rewrite every `Draw()` override in `Rendering/Env/Particles/Classes/` + a shader billboard path). Only scope if particle CPU time still dominates after Tier 1.

## Validation
- [ ] Confirm `REFRAC ⊆ ALPHAF` holds for particles (non-model projectiles always get `ALPHAF`; `REFRAC` adds the below-water-touching ones).
- [ ] Visual parity check: below/above-water layering still correct (water drawn between the two player-camera submits), reflections + refractions unchanged.
- [ ] Profile: heavy DP+SO+DS should show 2 builds/frame instead of 4; index-filter passes should be cheap.
