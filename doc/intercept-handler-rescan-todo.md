# InterceptHandler: avoid full rescan on every projectile creation

## Problem
`CInterceptHandler::AddInterceptTarget` ends with `Update(true)`, a forced full `O(interceptors × interceptables)` rescan. It's called from the `CWeaponProjectile` constructor, so every projectile creation re-evaluates *every* interceptor against *every* in-flight interceptable — not just the new projectile.

On salvo frames (many projectiles created at once, many interceptables airborne, many interceptor/shield weapons) this multiplies into tens of thousands of `eventHandler.AllowWeaponInterceptTarget` calls — observed ~45k in one frame. The work nests under `Sim::Unit::Weapon` because firing → projectile ctor → `AddInterceptTarget` happens on the weapon-update call stack, and spikes correlate with `G:ProjectileCreated`.

## Why it's wasteful
The new projectile only needs checking against the interceptor list once (`O(interceptors)`). Re-running all the *already-known* interceptables through the loop is redundant — the periodic `Update(false)` (every `UNIT_SLOWUPDATE_RATE` frames, from `Game.cpp`) already handles ongoing re-checks of the full set.

## Fix
- [ ] Add an `AddInterceptTarget`-only path that evaluates just the newly-added projectile against `interceptors`, instead of calling `Update(true)`.
- [ ] Factor the per-(interceptor, target) body out of `Update`'s inner loop so the single-target path and the full periodic scan share one code path (avoid divergence in the coverage/trajectory/AllowWeaponInterceptTarget logic).
- [ ] Keep the periodic `Update(false)` for ongoing re-checks; only the immediate forced full rescan on creation is removed.

## Validation
- [ ] Confirm anti-nuke / shield behavior unchanged: new projectiles still get matched to interceptors on the creation frame (not delayed to the next slow-update tick).
- [ ] Profile a salvo-heavy frame: `AllowWeaponInterceptTarget` count should drop from `~P × I × N` toward `~P × I` (P = projectiles created that frame, I = interceptors, N = interceptables).
- [ ] Sync check: `AddInterceptTarget` runs in synced sim, so the new path must be deterministic and order-preserving vs the old full-scan results.

## Related
- Parent-zone instrumentation already added: `ZoneScopedN("InterceptHandler::Update")` in `CInterceptHandler::Update` (covers both the periodic and forced paths) — measure with this before/after.
