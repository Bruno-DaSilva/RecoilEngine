# WS-4 — Quick win: cached weapon class tag (1.0 ms → ~0.4 ms)

Status: SCOPED, ready to implement. Child doc of `00-program-overview.md` §4 WS-4. Shared context (§2 core insights, §3 taxonomy, §6 verification protocol) is binding and not re-litigated here. Strategy class: a construction-time class tag replacing runtime RTTI — the same pattern as `AMoveType::moveTypeClass` (§7 "baseline state already landed", commits `7e0fbeb7af` / `6570827861`).

## 1. Problem

The `UnitsWeaponsPerWeapon` extraction pass (SimSnapshot.cpp:1418, `Update::SimSnapshot::UnitsWeaponsPerWeapon`) costs 1.0 ms per epoch late-game (§1 baseline). For every live weapon of every active unit it calls `trace::ClassifyWeapon(weapon)` (SimSnapshot.cpp:1488) to stamp the draw-side dispatch discriminator `rows.wWeaponClass[wi]`.

`trace::ClassifyWeapon` (rts/Sim/Weapons/WeaponTraceClass.cpp:24) is a straight-line chain of up to 15 sequential `dynamic_cast<const CSubclass*>(w)` tests, returning on the first hit (WeaponTraceClass.cpp:29-44). Each `dynamic_cast` is an RTTI type-graph walk; the chain runs to completion for the last-listed classes (DGunWeapon) and for the unreachable `Base` fallthrough. Paid per weapon per epoch, over a cold `CWeapon*`, this is the bulk of the pass's compute.

Two further redundant `dynamic_cast`s in the same loop body recompute information the enum already carries:

- SimSnapshot.cpp:1463 — `rows.wIsBombDropper[wi] = (dynamic_cast<const CBombDropper*>(weapon) != nullptr)` (this is exactly `wc == WeaponClass::BombDropper`).
- SimSnapshot.cpp:1476 — `const CPlasmaRepulser* repulser = dynamic_cast<const CPlasmaRepulser*>(weapon)` used to set `wIsShield`/`wShieldEnabled`/`wShieldPower` (the null test is exactly `wc == WeaponClass::PlasmaRepulser`).

So the pass performs, per weapon, one ≤15-deep RTTI chain plus two more RTTI casts, where a single cached-enum load would answer all three.

The same `ClassifyWeapon` call also sits on the **live synced sim** targeting path: `trace::LiveView::GetWeaponClass()` (WeaponPredicates.h:782) forwards to `ClassifyWeapon(weapon)`, and the templated trace-predicate dispatchers switch on it (WeaponPredicates.h:169, 201, 366, 394, 495, 560, 707). Every `TryTarget`/`TestTarget`/`TestRange`/`HaveFreeLineOfFire`/`GetRange2D` the sim runs re-derives the class by RTTI (Weapon.cpp:962, 1013, 1019, 1026, 1032, 1242; and the subclass overrides in Cannon.cpp:64, MissileLauncher.cpp:75, BeamLaser.cpp:277, etc.). WS-4 removes the RTTI from that path too, for free, since the fix is at `ClassifyWeapon` itself.

## 2. Mechanism — construction-time tag on `CWeapon` (per-subclass ctor)

Add a `trace::WeaponClass` member to `CWeapon` (Weapon.h:27, class body after the `CR_DECLARE_DERIVED(CWeapon)` at Weapon.h:29), default-initialized to `WeaponClass::Base`, with a `GetWeaponClass()` accessor. Each concrete subclass ctor stamps its own value. `trace::ClassifyWeapon(w)` is rewritten to `return w->GetWeaponClass();` — a single byte load — so **all three call sites** (extractor, LiveView, diff gate) pick up the tag with no per-site edits.

### 2.1 Choice: per-subclass ctor, NOT loader dispatch

Two options were considered; the moveTypeClass precedent (per-subclass ctor) wins, and the deciding factor is creg.

- **Loader dispatch (rejected).** `CWeaponLoader::LoadWeapon` (WeaponLoader.cpp:100) already switches on `weaponDef->type[0]` and allocates the concrete class, so it could set the tag right at each `weaponMemPool.alloc<CSubclass>` call. But weapons are **also constructed by creg** on checkpoint load (`CR_BIND_DERIVED_POOL(CWeapon, …)`, Weapon.cpp:41) — that path does not run through `LoadWeapon`. A creg-reconstructed `CBombDropper` would keep the `Base` default and mis-classify. Loader dispatch also does not map 1:1 to the class: the `'T'` case allocates *either* `CBombDropper` (flying owner, WeaponLoader.cpp:131) *or* `CTorpedoLauncher` (WeaponLoader.cpp:133), so a def-string tag would need to duplicate the allocation logic and stay in sync with it. Fragile on both counts.
- **Per-subclass ctor (chosen).** Each concrete ctor sets the tag, so it is stamped by the *actual* runtime type — exactly what `dynamic_cast` reports — and it is set on **every** construction path including creg. This mirrors `AMoveType::moveTypeClass` (MoveType.h:112-123), which each movetype ctor sets (GroundMoveType.cpp:502, StaticMoveType.cpp:22, HoverAirMoveType.cpp:140, StrafeAirMoveType.cpp:377, ScriptMoveType.cpp:60).

### 2.2 Set the tag BEFORE any creg null-owner/null-def early-return

The precedent sets `moveTypeClass = MT_GROUND;` as the **first statement** of the ctor body, ahead of the `if (owner == nullptr) return;` creg guard (GroundMoveType.cpp:502-506; StaticMoveType.cpp:22-28 does the same before its `if (unit == nullptr) return;`). A creg-loaded object is constructed with a null owner and takes that early return, so a tag set after the return would never run on load. The weapon subclasses use the same guard shape — e.g. CBombDropper gates on `if (def != nullptr)` (BombDropper.cpp:36), CBeamLaser on `if (def != nullptr)` (BeamLaser.cpp:94) — so the tag assignment must precede those blocks.

Concretely (15 subclasses, all `: public CWeapon` directly — flat hierarchy, verified across rts/Sim/Weapons/*.h):

- Out-of-line ctors — add one line at the top of the ctor body, before the def-null guard: `CBombDropper` (BombDropper.cpp:31), `CBeamLaser` (BeamLaser.cpp:88), `CLaserCannon` (LaserCannon.cpp:19), `CLightningCannon` (LightningCannon.cpp:23), `CTorpedoLauncher` (TorpedoLauncher.cpp:20), `CStarburstLauncher` (StarburstLauncher.cpp:21).
- Trivial inline `{}` ctors — convert the `{}` body to `{ weaponClass = WeaponClass::X; }` in the header: `CCannon` (Cannon.h:29), `CPlasmaRepulser` (PlasmaRepulser.h:16), `CNoWeapon` (NoWeapon.h:12), `CRifle` (Rifle.h:12), `CMeleeWeapon` (MeleeWeapon.h:12), `CEmgCannon` (EmgCannon.h:12), `CFlameThrower` (FlameThrower.h:12), `CDGunWeapon` (DGunWeapon.h:12), `CMissileLauncher` (MissileLauncher.h:12).

The base `CWeapon` ctor default (`weaponClass = WeaponClass::Base`) covers the fallthrough case that `ClassifyWeapon` returns today when no cast matches — a case that is never instantiated in practice (the loader always allocs a concrete subclass, WeaponLoader.cpp:106-156; `CNoWeapon` for isNulled/unknown). The invariant is only required for *instantiable* classes, and it holds for all 15.

### 2.3 creg serialization — no CR_MEMBER needed

The tag is **class-constant** (a property of the C++ type, identical on every construction of that subclass), so it does not need to be serialized. On checkpoint load the subclass ctor runs and re-establishes the correct value — exactly how `moveTypeClass` is handled: it is NOT listed in any `CR_MEMBER` and the commit message for `7e0fbeb7af` states it explicitly ("Class-constant, so not creg-serialized"). Do the same here: no entry in `CR_REG_METADATA(CWeapon, …)`. (Contrast `CUnit::damagesVersion` from the same commit, which IS creg-relevant and was marked `CR_IGNORED` precisely because a loaded unit must re-derive a *fresh* serial — the weapon-class tag has no such need because the value is fixed per type.)

## 3. Class-is-construction-determined — the 1:1 invariant (no blocker)

The correctness contract is: **for every constructed weapon object, `weapon->GetWeaponClass()` equals what `trace::ClassifyWeapon` returned under the old dynamic_cast chain.** This holds unconditionally because:

- The concrete C++ type of an object is fixed at construction and cannot change during the object's lifetime. There is no in-place re-classing of weapons. Unit morph in BAR frees and reconstructs weapons (`CWeaponLoader::FreeWeapons` → `LoadWeapons`, WeaponLoader.cpp:52-96) — a new object with a new ctor and a freshly-stamped tag, not a mutated type.
- The hierarchy is flat — every subclass derives directly from `CWeapon` (verified: all 15 headers declare `: public CWeapon`; asserted by the ClassifyWeapon comment, WeaponTraceClass.cpp:26-27) — so `dynamic_cast` is unambiguous and each object matches exactly one enum value.
- The enum maps 1:1 onto the 15 subclasses (WeaponTraceClass.h:23-40): Cannon↔CCannon, MissileLauncher↔CMissileLauncher, StarburstLauncher↔CStarburstLauncher, BeamLaser↔CBeamLaser, LightningCannon↔CLightningCannon, LaserCannon↔CLaserCannon, BombDropper↔CBombDropper, TorpedoLauncher↔CTorpedoLauncher, MeleeWeapon↔CMeleeWeapon, PlasmaRepulser↔CPlasmaRepulser, NoWeapon↔CNoWeapon, Rifle↔CRifle, EmgCannon↔CEmgCannon, FlameThrower↔CFlameThrower, DGunWeapon↔CDGunWeapon; Base = unreachable fallthrough.
- The two `'T'`/`'A'` loader special-cases (a torpedo-on-a-flyer and an aircraft bomb both allocate `CBombDropper`, WeaponLoader.cpp:126/131) collapse to the *same* concrete class → the same enum → the same tag. The `useTorps` distinction is a member flag (`dropTorpedoes`), not a class distinction, and is captured separately (`wBombDropTorpedoes`, SimSnapshot.cpp:1519).

**No weapon class is non-construction-determined. No blocker.**

## 4. Exact choke/replace inventory (file:line)

`ClassifyWeapon` call sites (3):

1. SimSnapshot.cpp:1488 — extractor hot path (the 1.0 ms). After the rewrite this reads the tag. `static_cast` member access downstream (Cannon branch SimSnapshot.cpp:1507-1508, BombDropper branch 1517-1518) is unchanged and already enum-gated.
2. WeaponPredicates.h:782 — `LiveView::GetWeaponClass()`, the live synced targeting path. Reads the tag; RTTI removed from live targeting (see §6 for the flag-off argument).
3. SnapshotDiffGate.cpp:1994 — `rows.wWeaponClass[wi] == static_cast<uint8_t>(trace::ClassifyWeapon(weapon))`. **This compare becomes tautological** once `ClassifyWeapon` returns the tag (both sides become the same byte). See §5.

Redundant `dynamic_cast`s to replace in the extractor loop (2):

4. SimSnapshot.cpp:1463 — `rows.wIsBombDropper[wi] = (dynamic_cast<const CBombDropper*>(weapon) != nullptr)` → `rows.wIsBombDropper[wi] = (wc == trace::WeaponClass::BombDropper)`.
5. SimSnapshot.cpp:1476 — `const CPlasmaRepulser* repulser = dynamic_cast<const CPlasmaRepulser*>(weapon)` → gate on `wc == trace::WeaponClass::PlasmaRepulser` and use `static_cast<const CPlasmaRepulser*>(weapon)` for the `IsEnabled()`/`GetCurPower()` member reads (SimSnapshot.cpp:1477-1479), matching the enum-gate+static_cast pattern already used two branches down.

Consumer that already reads the row (no change): TraceEpochView.h:88 reads `wWeaponClass` straight from the row, no cast — draw-side. SnapshotHash.cpp:174 notes wWeaponClass is defID-deterministic and excluded from the fold — unaffected.

`ExtractDeadRowsFromShells` (SimSnapshot.cpp:1809) — **not a call site.** Dead units emit the count-0 nil weapon shape (`weaponOffset=0`, `weaponCount=0`, `hasShieldWeapon=0`, SimSnapshot.cpp ~1970s) and never re-classify; weapons are freed in PreDestruct. Listed as key code only to confirm the change is confined to the live per-weapon pass — dead rows need no edit.

Out-of-scope RTTI that could later reuse the tag but is NOT touched by WS-4 (not epoch-produce): LuaSyncedRead.cpp:5716, SSkirmishAICallbackImpl.cpp:4768/4788 (`dynamic_cast<const CPlasmaRepulser*>` for shield queries). Optional cleanup, separate change.

## 5. Diff-gate tautology — keep the chain honest

After §4.3, the gate's wWeaponClass line (SnapshotDiffGate.cpp:1994) compares tag-against-tag and always passes — it stops validating anything. Recommended fix: retain the dynamic_cast chain as a debug/gate-only `trace::ClassifyWeaponSlow(const CWeapon*)` (the current body of WeaponTraceClass.cpp, unchanged) and point the gate compare at it, so the tag is still checked against ground-truth RTTI across a demo. `ClassifyWeapon` (the hot path) reads the tag; `ClassifyWeaponSlow` (gate + optional `assert` in debug builds) walks RTTI.

Two of the fifteen classes stay independently validated even without `ClassifyWeaponSlow`: the gate's `wIsBombDropper` compare (SnapshotDiffGate.cpp:1958, live side `dynamic_cast<const CBombDropper*>`) and `wIsShield` compare (SnapshotDiffGate.cpp:1977, live side `dynamic_cast<const CPlasmaRepulser*>`) keep their live-side casts — those compares check the tag-derived stored value against a real cast and are NOT tautological. But that only covers BombDropper and PlasmaRepulser; `ClassifyWeaponSlow` is what keeps the other 13 honest, so it is the recommended path, not optional. The gate's live-side casts at SnapshotDiffGate.cpp:1958/1977/1991/1992 (liveCannon/liveBomb, used for cannon/bomb member compares) all stay as-is — they are ground truth, not to be converted.

## 6. Flag-off byte-identity argument (the load-bearing one)

WS-4 adds a member to a synced sim object and changes a live synced code path (LiveView, §4.2), so it must be inert flag-off (`SimDrawSplit=0`). The argument:

- **The added member does not enter synced state.** `weaponClass` is a plain `uint8_t`/enum, not a `SyncedPrimitive`, so it is not folded into the SYNCCHECK checksum. It is not `CR_MEMBER`, so it does not perturb checkpoint serialization. It is written once at construction (deterministically — the ctor and the def type are identical across clients) and never mutated, so it cannot diverge.
- **The value read is identical to the value computed today.** By the §3 invariant, `GetWeaponClass()` returns exactly what the old dynamic_cast chain returned. Every `switch (view.GetWeaponClass())` in the live predicate stack (WeaponPredicates.h:169/201/366/394/495/560/707) selects the identical branch, so `TryTarget`/`TestTarget`/`TestRange`/`HaveFreeLineOfFire`/`GetRange2D` produce bit-identical synced results. The change is a strict compute-substitution (RTTI walk → byte load) with the same output.
- **The extractor edits (§4.1, §4.4, §4.5) are producer-side only** and run only under the split; they never touch synced state.

Therefore a flag-off resim is byte-identical, and the only thing that can break it is a *wrong* tag in a ctor — which is a mechanical error caught by both the armed diff-gate (§5, via `ClassifyWeaponSlow`) and the flag-off resim itself.

## 7. Threading

The tag is written on the sim thread at construction and read on the sim thread — in the live predicate path (sim proper) and in the extractor (producer runs at the sim frame edge, on the sim thread; no MT here, WS-8 pending). Single-writer-at-construction, read-only thereafter, no cross-thread access, no atomics. Consistent with §6 of the program overview ("WS-1/2/3 add no cross-thread access"; WS-4 likewise).

## 8. Verification plan

- **Armed diff-gate, wWeaponClass field pass.** With `ClassifyWeaponSlow` retained (§5), the diff gate validates every per-weapon tag against the live RTTI chain across the late-game demo. Watch the trace family (the `wWeaponClass` compare, SnapshotDiffGate.cpp:1994) plus the `W_VECTORS` (wIsBombDropper) and `W_SHIELD` (wIsShield) families that cross-check two of the classes against live casts. Expect zero mismatches.
- **Flag-OFF resim.** `SimDrawSplit=0` headless replay gate on ≥1 representative replay (the DEBUG headless replay gate / demo-resim, per program §6). The LiveView path change is the only flag-off-reachable behaviour change; a clean resim confirms the §6 byte-identity argument. No desync expected.
- **Benchmarks.** Re-run the established late-game replay measurement and report the `Update::SimSnapshot::UnitsWeaponsPerWeapon` zone against the §1 baseline (1.0 ms). Expect ~0.4 ms.

No A/B pixel gate (program rule; this is decoupling-adjacent producer work).

## 9. Staging and expected numbers

**One small PR.** Contents:

1. `CWeapon`: add `trace::WeaponClass weaponClass = trace::WeaponClass::Base;` member + `GetWeaponClass()` accessor (Weapon.h); include WeaponTraceClass.h. No CR_MEMBER.
2. Stamp `weaponClass` in all 15 subclass ctors, before the def/owner-null guard (§2.2 inventory).
3. `WeaponTraceClass`: `ClassifyWeapon(w)` → `return w->GetWeaponClass();`; retain the chain as `ClassifyWeaponSlow(w)` for the diff gate / debug assert.
4. Extractor: replace the two redundant casts (SimSnapshot.cpp:1463, 1476) with enum tests (§4).
5. Diff gate: point the wWeaponClass compare at `ClassifyWeaponSlow` (SnapshotDiffGate.cpp:1994).

Expected: `UnitsWeaponsPerWeapon` 1.0 ms → **~0.4 ms** (§1 target). The ~0.6 ms saved is the ≤15-deep RTTI chain + two extra casts per weapon, replaced by one byte load + a branch. WS-4 does **not** retire the pass (insight §2.3) — the loop still fetches the cold `CWeapon` and writes ~40 scalar/vector columns per weapon; that residual gather is the ~0.4 ms floor and is out of scope for this quick win (it belongs to a future WT migration, explicitly deferred in WS-3). A modest, unbudgeted side benefit: the live synced targeting path (LiveView) also drops its per-predicate RTTI, but that time is not in the epoch-produce column.
