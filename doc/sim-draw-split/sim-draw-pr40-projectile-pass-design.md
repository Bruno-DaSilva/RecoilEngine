# PR 40 — projectile pass conversion (pointer-free projectiles) — design + decision-4 ruling

Status: IN PROGRESS (Opus, 2026-07-09). Branch `epoch-integration`. Governing docs: `sim-draw-pr43-44-contract-refresh.md` §4.4 / §3.7, `sim-draw-pr44-prerequisites.md` ("Also folds in here"). Precedent: `sim-draw-pr39-scope1-design.md` (the SCOPE-1 record-captured-handle interim is the sanctioned pattern here).

> ⚠️ WORKTREE-ONLY: all edits/builds in `/www/projects/RecoilEngine/.claude/worktrees/epoch-integration`, never the main checkout. Ignore clangd cross-checkout diagnostics; judge only by `ninja -C build`.

## Task

Stop `renderProjectiles`/`UpdateDrawFlags` from resolving through `projectileHandler` (plan PR 40 [O]); delete the projectile resolve cache. Under PR 44 `UpdateDrawFlags` and the model/particle draw passes stay draw-side (camera-dependent culling, §3.7) and run CONCURRENT with the sim — so no projectile draw pass may walk the sim-owned `FreeListMapCompact` containers.

`CProjectileDrawer` is structurally SEPARATE from the unit/feature machinery SCOPE-1 touched: it is a `CEventClient` (not a `CModelDrawerBase`) with its OWN `splitResolveCache`/`BuildSplitResolveCache`. SCOPE-1 deliberately left it intact; this PR retires it.

## Decision-point 4 — projectile extraction budget: FULL POD EXTRACTION IS OUT, INTERIM IS IN

The task's headline ("extract per-class projectile visual state into POD draw packets; the §C projectile family becomes extracted, not torn-tolerated") is the ambition; decision-4 pre-authorises the SCOPE-1-style deferred-safe-handle interim "if over budget". It is over budget, on TWO independent grounds:

1. **Code surface (decisive, not just memory).** The projectile draw passes do not read a fixed handful of scalar fields off `p` (as the unit/feature passes did — team/pos/radius/…). They dispatch deep, per-subclass **virtual** draws: `p->Draw()` (particle alpha + transparent-shadow passes), `p->DrawOnMinimap()`, and `DrawProjectileModel(p)`. Each of ~30 `CProjectile` subclasses (laser, missile, beam-laser, large-beam-laser, lightning, torpedo, starburst, explosive, fireball, flare, gfx, heatcloud, piece, repulse, shield-part, simple-particle-system, smoke, sphere-part, tracer, wreck, weapon, the CEG spawnables, nano-particles, …) has a bespoke `Draw()` reading extensive live per-instance state. Converting to POD packets = re-architecting the entire particle draw system into a data-driven form, subclass by subclass — far beyond one lifetime-touching PR and orthogonal to the flip's actual requirement (stop resolving through `projectileHandler`).

2. **Memory / per-boundary copy (S0-style measure, this PR).** A full extraction copies one packet per registered projectile per boundary. Measured peak concurrent registered projectiles + particle count on both replays (flag-OFF, full-length FF, `[PR40-MEASURE]` instrument):
   - **Rosetta:** peakProj = **11008**, peakParticles = **15622**.
   - **All That Glitters:** peakProj = **5115**, peakParticles = **11915**.
   - A faithful per-subclass packet is order-100–500 B (positions, colours, texcoords, per-type scalars); at Rosetta's peak (11008 objects) that is **~1.1–5.5 MB/boundary** of pure copy, churned every frame — over the decision-4 "1–2 MB/boundary heavy" line at any realistic packet size, and that is only the projectile OBJECTS, not the per-projectile particle expansion (15622 particles) the `Draw()` methods emit.

**Operator escalation (decision-4 budget number):** peak concurrent registered projectiles = **11008 (Rosetta) / 5115 (ATG)**, peak particle expansion = 15622 / 11915. Full per-class POD extraction is over budget on both the copy-cost and (decisively) the per-subclass virtual-draw code-surface grounds; landed as the SCOPE-1-style interim per decision-4. No operator ruling was pending (decision-4 pre-authorises the interim); this number is for the record and for the eventual full-extraction budget call.

**Ruling (decision-4, operator-escalated):** land the **SCOPE-1-style deferred-safe handle interim**. This is the pre-documented fallback; the SCOPE-1 precedent is explicitly "the pattern for the record-captured stable pointers interim". Escalated budget numbers above are for the operator's record; full POD extraction is deferred (see §Finish path).

## The interim — producer-captured deferred-safe handle store

Mirror of SCOPE-1's `record.obj`, adapted to the projectile drawer's id-keyed arrays.

**`std::array<std::vector<const CProjectile*>, 2> renderObjects;`** — id-keyed (`[synced][id]`), parallel to the existing `drawPositions`/`drawFlags`/`sortDists`/`renderIndices`. Maintained by the render events, in lockstep with `renderHandles`:
- `RenderProjectileCreated`: grow alongside the sibling arrays, `renderObjects[synced][id] = p` (the drain-time-resolved pointer — the live object).
- `RenderProjectileDestroyed`: `renderObjects[synced][id] = nullptr`.
- `Kill`: cleared with the sibling arrays.

Accessor `GetRenderObject(id, synced)` — bounds-checked, `nullptr` for unregistered ids (same shape as `GetDrawFlag`/`GetDrawPos`).

**Safety (identical class to the pre-existing `renderProjectiles` and `splitResolveCache`).** A projectile object is pool-allocated and never relocated during its lifetime, so the create-captured pointer equals what `projectileHandler.GetProjectileBy*ID(id)` returns for the whole life of the id. On death the sim does `deferredObjectDeleter.Defer(p)` (ProjectileHandler.cpp:378) — the object becomes a poisoned shell released only at the draw-boundary ack — and `RenderProjectileDestroyed` (drained at the boundary, ProjectileHandler.cpp:358 → RenderEventQueue drain) nulls the slot at that same edge. So within any draw frame every registered `renderHandle` resolves to a valid (non-freed) pointer, and no dead id is ever read. This is exactly the window `renderProjectiles` (resolved per-frame in `UpdateDrawFlags`) and the barrier-built `splitResolveCache` already relied on — the change only swaps *how* the pointer is obtained (captured once at create vs. re-walked through the sim-owned containers).

**Consumers repointed off `projectileHandler`:**
- `ResolveProjectileHandle(handle)` (used by `DrawOpaque` / `DrawShadowOpaque` model-bin loops) → `GetRenderObject`. Drops the `SimDrawSplit::Enabled()`/`SplitResolveCacheBuilt()` branch and the live-handler fallback.
- `UpdateDrawFlags` per-handle resolution (the `for_mt` cull loop) → `GetRenderObject`. This is the pass §3.7 keeps draw-side under the flip; it must not walk `projectileHandler`.

**Deleted:** `splitResolveCache`, `splitResolveCacheBuilt`, `BuildSplitResolveCache` (the id→pointer walk), `SplitResolveCacheBuilt`, `ResolveSplitCachedProjectile`.

**Effect-container barrier copies are NOT the resolve cache — preserved, relocated.** `splitGroundFlashes` / `splitFlyingPieces` are boundary snapshots of the sim-owned `groundFlashes` / `flyingPieces` containers the model/flying-piece/ground-flash passes iterate (the sim mutates them mid-frame under the split). They were folded into `BuildSplitResolveCache`; they move verbatim into a slimmed **`SnapshotEffectContainers()`** called at the same two boundary sites (SimDrawBarrier step 1c + the pool-valve service, Game.cpp). Sim-quiescent producer work today; moves to the sim frame edge under the flip like every other channel. (These are a distinct §1.4-adjacent extraction, out of "delete the resolve cache" scope.)

## Flag-off byte-identity

`GetRenderObject(id,synced)` returns the same pointer `projectileHandler.GetProjectileBy*ID(id)` would — proven empirically by a temporary flag-OFF sweep in `UpdateDrawFlags` (`renderObjects[id] == live` for every registered handle; 0 mismatches, see gates) on top of the resim gate. All draw-window reads are unchanged (still deref the same live/deferred object), so flag-off output is bit-identical.

## Files

- `rts/Rendering/Env/Particles/ProjectileDrawer.h` — `renderObjects` member + `GetRenderObject` + `SnapshotEffectContainers` decl; delete the resolve-cache API.
- `rts/Rendering/Env/Particles/ProjectileDrawer.cpp` — `ResolveProjectileHandle`/`UpdateDrawFlags` → `GetRenderObject`; `BuildSplitResolveCache` → `SnapshotEffectContainers` (container copies only); create/destroy/Kill maintain `renderObjects`.
- `rts/Game/Game.cpp` — two barrier call sites `BuildSplitResolveCache` → `SnapshotEffectContainers` (+ comments).

## Gates (per task)

flag-OFF resim ×2 both replays + armed diff-gate both replays + flag-ON headful both replays (38b widget-error watch) + TSan segment. Numbers recorded at the bottom on completion.

## Finish path (full POD extraction, deferred)

When (if) the operator wants the §C projectile family truly extracted: it is a per-subclass data-driven-draw rewrite (each `Draw()`/`DrawOnMinimap()` emits from an extracted packet rather than reading `this`), independent of the epoch threading and gated on its own measured memory budget. The interim already removes the `projectileHandler` dependency the flip requires, so PR 44 is unblocked without it.

## Gate results (Build B — instrumentation stripped, `build/spring`, bar-data2, headful FF full-length)

All runs full-length (~f=44500+), headful (`DISPLAY=:0`), one spring at a time.

- **flag-OFF resim ×2** — Rosetta rc=0 f=44537 **0 DESYNC**; ATG rc=0 f=44857 **0 DESYNC**. (Build-A measurement runs additionally proved `renderObjects == projectileHandler.GetProjectileBy*ID` for every registered handle: **0 mismatches** on both, byte-identity confirmed.) Only the known pre-existing `gfx_decals_gl4.lua:564 table index is NaN` on Rosetta (~f=39685), not branch-caused.
- **Armed diff-gate (DG_ARM=1) both replays** — Rosetta **PASS (0 mismatches, 4612 boundary checks)**; ATG **PASS (0 mismatches)**. 0 DESYNC, 0 `Invalid Feature id`.
- **flag-ON headful, 2 replays × 2 runs** (armed + plain) — all rc=0, full-length, **0 DESYNC, 0 `Invalid Feature id` (no 38b nil-storm), no `unit_healthbars_widget_forwarding` errors** (only the load-time gadget banner). Only the known gfx_decals NaN.
- **TSan segment** — worktree `build-tsan` (`-fsanitize=thread`, `USE_MIMALLOC=OFF`, gcc-13), flag-ON (split running → real draw/sim concurrency). **PASS: my change is TSan-neutral.** `renderObjects`, `GetRenderObject`, `ResolveProjectileHandle`, `SnapshotEffectContainers` appear in **zero** data races. The only `CProjectileDrawer` races are the pre-existing tolerated §C torn-read family on the draw arrays — `GetDrawFlag`(→`drawFlags`, 1024-B block) / `GetSortDist`(→`sortDists`) read from `DrawAlpha`/`DrawProjectilesMiniMap`/`DrawShadowTransparent` while the `UpdateDrawFlags` for_mt workers write them — identical to the prior TSan baseline (`tsan_final.2578086`: `GetSortDist`/`GetDrawFlag`). `RenderProjectileCreated` shows only as the *allocation* site of those pre-existing arrays, never as a racing access. Consistent with design: `renderObjects` is written at the boundary (sim-parked, draw-window closed) and read in the draw window — non-overlapping. All touched paths were exercised under the split (create + `UpdateDrawFlags` for_mt + `DrawAlpha`/minimap draw passes ran).
  - Two build-environment notes (NOT code issues, NOT in the commit): (1) the TSan build needs `TSAN_OPTIONS=allocator_may_return_null=1` + `HangTimeout=-1`, and a **build-tsan-only** round-up in `recoil::aligned_alloc` (MemoryOverride.cpp) because TSan's `aligned_alloc` interceptor is C11-strict about `size % alignment == 0` where glibc is lenient — otherwise it aborts during Lua load before gameplay. That accommodation was reverted before commit. (2) TSan runtime is ~hundreds× slower here (f=1 after ~6 min), so the run was stopped once all projectile-drawer paths had executed rather than run to the frame window — TSan detects the access *pattern* (count-independent), so early-frame coverage is conclusive.

Verdict: flag-off byte-identical, flag-on value/timing-identical (diff-gate PASS), no draw pass resolves through `projectileHandler`, resolve cache deleted. The projectile drawer is now pointer-free of the sim-owned containers for the flip (§3.7).
