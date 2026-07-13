# ProjectileDrawer::DrawAlpha — late-game perf findings

Profiling of ultra-settings late game flagged `Draw::World::Particles` as the dominant draw cost. This is what's actually going on and where the time is.

## What "Particles" actually is

`Draw::World::Particles` is just the `SCOPED_TIMER` label `WorldDrawer::DrawAlphaObjects` wraps around `CProjectileDrawer::DrawAlpha`. The code lives in `Rendering/Env/Particles/ProjectileDrawer.cpp`; the label brands it by what it draws, not the class.

The term is overloaded. `DrawAlpha` is the alpha-blended billboard-quad path for **every model-less projectile**, plus the billboard parts (trails, glows, muzzle/impact FX) of projectiles that also have a model. That's two different things sharing one fx-shader + texture-atlas + additive-blend path:

- Gameplay projectiles drawn as sprites — laser, plasma (explosive), EMG, beam lasers, lightning, flame, fireball.
- Pure FX particles — fire, smoke, explosions, flares, exp-gen spawnables.

So in plasma/laser-heavy fights a large slice of "Particles" is **weapon fire**, not cosmetic noise — it can't simply be LOD-culled as eye-candy.

Projectiles with a model draw the model through `DrawOpaque`; their trail/glow quads still go through `DrawAlpha`.

## It runs 4× per frame, under two labels

`DrawAlpha` is called four times when water is on, and only two are counted under `Draw::World::Particles`:

| Pass | Camera | Counted under |
|---|---|---|
| below-water | player | `Draw::World::Particles` |
| above-water | player | `Draw::World::Particles` |
| reflection | UWREFL | `Draw::World::Water` |
| refraction | player | `Draw::World::Water` |

So the headline `Particles` number undercounts total particle CPU: reflection + refraction are two more full builds hidden inside `Water`. Real per-frame particle cost ≈ `Particles` + a chunk of `Water` ≈ ~8 ms.

## Benchmark (Tracy, ultra late game)

Mean per `ProjectileDrawer::DrawAlpha` call (~2 ms total per call), by sub-zone:

| Zone | Time | % | What it is |
|---|---|---|---|
| DS | 928 µs | 47% | CPU billboard fill, sorted bucket |
| SO | 446 µs | 22% | `std::sort` of the sorted bucket |
| DU | 428 µs | 21% | CPU billboard fill, unsorted bucket |
| RR | 143 µs | 7% | VBO/EBO upload + `glDrawElements` + driver |
| DP | 79 µs | 4% | partition scan over all renderProjectiles |

## Verdict: CPU-bound on fill + sort, not GPU

- DS + DU = **68% pure CPU quad expansion** (per-particle billboard corner math: `camera->GetRight()/GetUp()`, `sin`/`cos` rotation, color, 4 verts each).
- SO = **22% CPU sort**.
- RR (everything GPU-facing: upload, draw call, driver) = **7%**.

Consequences:
- Shrinking the fat `VA_TYPE_PROJ` vertex attacks the 7%. Skip.
- Fillrate / overdraw / soft-particle depth sampling lives in RR, also the 7%. Not the bottleneck yet.
- DU being ~half of DS means the unsorted (additive, order-independent) bucket is large — it needs filling but no sort.
- DS ≈ 2× DU: the sorted (alpha-blended) bucket is both the bigger filler and the only one paying SO.

## Optimization ideas, in leverage order

**1. Build once per camera (Tier 1).** Highest leverage, lowest risk, no math/shader/determinism change. Player camera serves below-water + above-water + refraction (3 of 4 passes); UWREFL serves reflection. Collapsing 4 builds → 2 roughly halves DP+SO+DS+DU at the frame level (~3–4 ms). Doesn't speed one call; deletes two of them. Plan in [projectile-drawalpha-percamera-cache-todos.md](projectile-drawalpha-percamera-cache-todos.md).

**2. Parallelize the DS+DU fill.** `UpdateDrawFlags` is already `for_mt`; the fill isn't, only because all particles `emplace` into one global render buffer. Give each worker a thread-local vert/idx buffer over a contiguous slice of the (already-sorted) array, concatenate in order, upload once. Order preserved for free; billboard math is embarrassingly parallel. 1356 µs serial → ~`/Ncores` + cheap merge. Compounds with Tier 1.

**3. Radix sort the SO zone.** `std::sort` with the `forward_as_tuple(dist, ptr)` comparator is a comparison sort (O(n log n)) with an expensive per-comparison body. The key is a single float (`GetSortDist`); projected distances are ≥ 0, so positive IEEE-754 floats sort correctly as their raw `uint32_t` bit pattern — radix key is free. O(n), no comparator, cache-friendly, parallelizable. A stable LSD radix also reproduces the determinism the pointer tiebreak currently provides. Keep `std::sort` for small buckets; switch to radix when large. (SO is 22%, so this is contained — sequence behind 1 and 2.)

**4. Defer GPU billboarding (Tier 2).** Expanding center points into camera-facing quads in a vertex/geometry shader would erase DS+DU from the CPU and let reflection reuse the player build, but it's a rewrite of every `Draw()` override plus a shader path. Only justified if fill still dominates after 1+2, or to fold in the 4th (reflection) build.

## Side levers (not free)

- Screen-size cull / LOD in `UpdateDrawFlags`: skip particles whose projected radius is sub-pixel. Cuts both fill and overdraw, but cannot blindly drop weapon-fire sprites that are gameplay-readable.
- Moving sorted (alpha) particle types to additive blending drops them out of SO entirely — but it's a per-effect visual-correctness call, not a toggle.
