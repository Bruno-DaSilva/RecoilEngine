# WS-2 — ExtractTransforms: whole-object dirty-skip (child doc of 00-program-overview.md §4)

Status: DESIGN. Depends on WS-1 (`pieces-capture-version-skip.md`) landing the `LocalModel` capture-version counter first; §7 states the exact semantics WS-2 needs from it. Strategy class: DS (overview §3), with the object-transform half of the key being a value-shadow compare rather than a mutation-choke version (§4 explains why).

## 1. Problem

`CModelDrawerDataBase<T>::ExtractTransforms` (rts/Rendering/Common/ModelDrawerData.h:388-407) runs at the sim frame edge under the split, forced-serial on the sim thread (`ExtractTransformsAtSimEdge`, rts/Rendering/Common/ModelDrawerData.h:82, called from the epoch producer at rts/Game/Game.cpp:2193-2201 under the `Sim::EpochProduce::TransformsUnits` / `TransformsFeatures` timers).

Per registered, LOS-visible object it pays, every epoch, whether or not anything about the object changed (rts/Rendering/Common/ModelDrawerData.h:410-446):

- the `scTransMemAllocMap` hash lookup (`GetObjectTransformMemAlloc`, rts/Rendering/Common/ModelDrawerData.h:143-146);
- `o->GetTransformMatrix()` + `Transform::FromMatrix` — a full matrix→quaternion TRS decompose (rts/System/Transform.cpp:38-52) — for the current object transform (rts/Rendering/Common/ModelDrawerData.h:420);
- two `UpdateIfChanged` storage writes, each taking `CModelsLock` and doing an eps-compare (rts/Rendering/Models/ModelsMemStorage.h:35-52, 127-137);
- a full piece walk calling `GetModelSpaceTransform()` per piece (recompute-if-dirty branch, rts/Rendering/Models/LocalModelPiece.cpp:141-154) plus the `GetWasUpdated()` check (rts/Rendering/Common/ModelDrawerData.h:426-445).

Measured baseline (overview §1): `Sim::EpochProduce::Transforms` = 3.15 ms/epoch late game — units 2.73 ms, features 419 µs. Late game the overwhelming majority of units are idle and features are almost entirely static, so nearly all of this is re-confirming that nothing changed, one cold cache-line chase at a time (overview insight §2.1). This is paid on the sim thread at the exact point that sets the sim rate.

Target: units 2.73 ms → ~0.5–0.9 ms; features 419 µs → ~0 (see §10 for what "~0" means per PR).

## 2. What the extraction actually writes (the equivalence contract)

Per-object layout in `transformsMemStorage` (comment block at rts/Rendering/Common/ModelDrawerData.h:330-364, allocation at rts/Rendering/Common/ModelDrawerData.h:296-298):

- slot [0]: prev object transform = `o->preFrameTra` — saved by the sim at the START of each frame (`CSolidObject::UpdatePrevFrameTransform`, rts/Sim/Objects/SolidObject.cpp:465-495, driven per frame from `CUnitHandler::UpdatePreFrame` rts/Sim/Units/UnitHandler.cpp:440-450 and `CFeatureHandler::UpdatePreFrame` rts/Sim/Features/FeatureHandler.cpp:194-201). Written with `UpdateIfChanged` (rts/Rendering/Common/ModelDrawerData.h:423).
- slot [1]: curr object transform = `Transform::FromMatrix(o->GetTransformMatrix())` (rts/Rendering/Common/ModelDrawerData.h:420, 424).
- slots [2+2i]/[3+2i]: piece i prev/curr model-space transforms, written with `UpdateForced` only when `GetWasUpdated()` (rts/Rendering/Common/ModelDrawerData.h:429-444); script-invisible pieces write `Transform::Zero()` into both (rts/Rendering/Common/ModelDrawerData.h:434-438).

The pure-function inputs of those values:

- Units: `CUnit::GetTransformMatrix()` = `ComposeMatrix(pos)` (rts/Sim/Units/Unit.cpp:1476-1480) = `CMatrix44f(pos, -rightdir, updir, frontdir)` (rts/Sim/Objects/SolidObject.h:192). So slot [1] is a pure function of `{pos, frontdir, rightdir, updir}`. Slot [0] (`preFrameTra`) is `Transform{CQuaternion::MakeFrom(GetTransformMatrix()), pos}` recomputed from those same four vectors at frame start (rts/Sim/Objects/SolidObject.cpp:489-494) — a pure function of the same key, evaluated one frame later (see the settle rule, §5).
- Features: `CFeature::GetTransformMatrix()` returns the stored `transMatrix` (rts/Sim/Features/Feature.h:102), and `preFrameTra` derives from `{transMatrix, pos}` (same rts/Sim/Objects/SolidObject.cpp:489 path). So the feature object slots are a pure function of `{transMatrix, pos}`.
- Piece slots: pure functions of piece-local state — `pos/rot/scale/pieceSpaceTra` (via `CalcPieceSpaceTransform` chains, rts/Rendering/Models/LocalModelPiece.cpp:236-266), `scriptSetVisible`, the `noInterpolation` flags (via `GetEffectivePrevModelSpaceTransform`, rts/Rendering/Models/LocalModelPiece.cpp:176-187), `blockScriptAnims`, plus the parent chain of the same. Exactly the state WS-1's capture-version counter covers.

The contract for a valid skip: **skip ⇒ the storage content is already bit-equal to what re-extraction would write.** Note `FromMatrix`/`MakeFrom` are deterministic pure fp functions per machine; this is unsynced draw-side state, so per-machine determinism (same code, same inputs, same result) is the relevant guarantee, and bit-equal inputs give bit-equal outputs. Skipping also skips the `updateList.SetUpdate` marking on `UpdateForced` writes that would have written identical bytes (rts/Rendering/Models/ModelsMemStorage.h:55-64) — that only removes redundant SSBO re-uploads of identical data, a pure win with no content difference.

## 3. Mechanism

Add a per-object skip record to the drawer, dense-indexed by object id exactly like `drawPositions`/`drawFlags` (rts/Rendering/Common/ModelDrawerData.h:232-233), reinitialized on `AddObject` (rts/Rendering/Common/ModelDrawerData.h:275-307):

```
struct TransformSkipState {
    ObjKey   key;          // §4: units {pos, frontdir, rightdir, updir} (48 B); features {transMatrix, pos} (76 B)
    uint64_t lmVersion;    // WS-1 LocalModel capture-version seen at the last full walk
    uint8_t  quiescent;    // consecutive walked-or-skipped edges with an unchanged key, saturating at 2
    bool     everWalked;   // false until the first successful full extraction
};
```

At the top of `ExtractObjectTransforms` (rts/Rendering/Common/ModelDrawerData.h:410), after the existing LOS/alwaysUpdateMat gate (rts/Rendering/Common/ModelDrawerData.h:414 — order preserved, see §6.2):

```
unchanged = everWalked && lmVersion == o->localModel.GetCaptureVersion() && key == CurrentObjKey(o)   // bitwise compares
if (unchanged && quiescent >= 2) return;                       // SKIP: no stma lookup, no FromMatrix, no piece walk
quiescent = unchanged ? min(quiescent + 1, 2) : 0;
...run the existing extraction body unchanged...               // the walk; per-piece wasUpdated machinery untouched
key = CurrentObjKey(o); lmVersion = ...; everWalked = true;    // skip state refreshed ONLY on a completed walk
```

Two properties carry the whole correctness argument:

1. **Skip state is refreshed only by a completed full walk.** The LOS gate's early return (rts/Rendering/Common/ModelDrawerData.h:414) does not touch it. Therefore `key/lmVersion` always describe the sim state at the moment the storage was last written, and any change since then — including changes made while the object was out of LOS, or while a spectator viewed a different allyteam — forces a mismatch and a walk on the next gate-passing edge. This is what makes LOS re-entry, `spectatingFullView` toggles, allyteam switches, and `alwaysUpdateMat` flips need no special handling (§6).
2. **Skip requires two consecutive quiescent edges** (`quiescent >= 2`), mirroring the `wasUpdated[2]` double-trigger (§5). The first unchanged edge after any change still walks; that walk is what settles the prev slots.

The inner piece loop is not modified: when the walk runs, `GetWasUpdated()` / `ResetWasUpdated()` / `GetEffectivePrevModelSpaceTransform` behave exactly as today. WS-2 only wraps the whole object.

## 4. The object half of the key: value-shadow, not a mutation choke

The task the overview sets: enumerate everything that can change an object's transform-SSBO content WITHOUT bumping the WS-1 counter — unit move/rotate is not a piece mutation. The inventory of `pos`/orientation mutation sites (the inputs of §2):

Position (units and features funnel through one choke, plus wrappers):

- `CSolidObject::Move` — rts/Sim/Objects/SolidObject.cpp:154-163 (the only place `pos` itself is written; `midPos/aimPos` don't enter the SSBO).
- Wrappers: `CUnit::ForcedMove` rts/Sim/Units/Unit.cpp:560-569; `CFeature::ForcedMove` rts/Sim/Features/Feature.cpp:488-510; `CFeature::UpdateQuadFieldPosition` rts/Sim/Features/Feature.cpp:546-556; `CScriptMoveType` (MoveCtrl) rts/Sim/MoveTypes/ScriptMoveType.cpp:117-119, 132, 187, 207.

Orientation (heading/frontdir/rightdir/updir) — this is where a choke design dies:

- Member functions: `SetHeading`/`AddHeading`/`SetDirVectors` (header-inline, rts/Sim/Objects/SolidObject.h:163-176), `SetDirVectorsEuler` rts/Sim/Objects/SolidObject.cpp:402-413, `SetHeadingFromDirection` rts/Sim/Objects/SolidObject.cpp:415-427, `UpdateDirVectors` rts/Sim/Objects/SolidObject.cpp:429-452, `ForcedSpin` rts/Sim/Objects/SolidObject.cpp:497-517 and 519-530 (feature overrides at rts/Sim/Features/Feature.cpp:513-527).
- **In-place mutation through `SyncedFloat3&` references and compound assignment in movetypes**: rts/Sim/MoveTypes/StrafeAirMoveType.cpp:910-925 (`frontdir += …; rightdir = frontdir.cross(updir)`), 996-999, 1122, 1174-1185; rts/Sim/MoveTypes/GroundMoveType.cpp:1827-1828; rts/Sim/MoveTypes/HoverAirMoveType.cpp:696-698 (mutable refs taken).
- Transport attach writes the transportee's dirs directly: rts/Sim/Units/Unit.cpp:665, 801-802.
- `CWeapon` temporarily mutates and then restores the owner's dirs during aim (rts/Sim/Weapons/Weapon.cpp:1082-1090) — a net no-op that a choke-based version would double-bump.

Conclusion: the dir vectors are public members mutated by reference-aliasing arithmetic all over the movetypes; there is no finite choke set short of privatizing `frontdir/rightdir/updir` and rewriting every movetype expression (far beyond WS-2's budget, and WS-3's planned privatization sweep covers `pos`/velocity, not the dir-vector aliasing). **Decision: the object half of the skip key is a bitwise value-shadow compared at the edge**, not a version:

- Units: `{pos, frontdir, rightdir, updir}` — 4 × float3 = 48 B. Exactly and exhaustively the inputs of `ComposeMatrix(pos)` (rts/Sim/Objects/SolidObject.h:192), hence of both object slots (§2). `heading`, upright/ground-normal handling, physics `midPos` etc. all act on the SSBO only through these four vectors, so the key subsumes every mutation path above, including ones nobody has written yet — the compare happens on the consumed values, so the "choke inventory misses a site" failure class does not exist for the object half.
- Features: `{transMatrix, pos}` — 64 + 12 = 76 B. `transMatrix` is what `GetTransformMatrix()` serves (rts/Sim/Features/Feature.h:102) and is only assigned in `CFeature::UpdateTransform` (rts/Sim/Features/Feature.cpp:529-534); `pos` is keyed separately because `preFrameTra.t` is taken from `pos`, not from the matrix (rts/Sim/Objects/SolidObject.cpp:489), and a hypothetical `Move` without a following `UpdateTransform` would otherwise be missed. Keying the consumed values again spares us auditing whether every feature `Move` caller reaches `UpdateTransform` (e.g. `UpdateQuadFieldPosition` rts/Sim/Features/Feature.cpp:546-556 does not itself, its caller does at rts/Sim/Features/Feature.cpp:632).

The piece half of the key is the WS-1 `LocalModel` capture-version (§7) — value-shadowing every piece would be re-doing the walk we are trying to skip.

Bitwise (not eps) comparison is deliberate: the skip predicate must be strictly stronger than "re-extraction writes the same bytes", and the storage's own `UpdateIfChanged` eps-compare (rts/Rendering/Models/ModelsMemStorage.h:128-131) then never sees a value our skip judged unchanged.

## 5. The interpolation traps, explicitly

### 5.1 `wasUpdated[2]` double-trigger / prevModelSpaceTra settle

The engine's own comment states the trap (rts/Rendering/Models/LocalModelPiece.cpp:120-134): after a piece's animation stops, `prevModelSpaceTra` still changes ONE more frame (frame-start `SavePrevModelSpaceTransform`, rts/Sim/Objects/SolidObject.cpp:485-487, copies the now-final pose into prev), so the prev slot must be uploaded one extra edge or draw interpolation jerks between a stale prev and the final curr. `ResetWasUpdated`'s `wasUpdated[1] = std::exchange(wasUpdated[0], false)` (rts/Rendering/Models/LocalModelPiece.cpp:130) implements exactly a two-edge trigger.

The same settle applies to the OBJECT slots: `preFrameTra` at edge F is the start-of-frame-F pose; if the object moved during frame F−1 and stopped, slot [0] still needs one more update at edge F (from start-of-F−1 to start-of-F). Timeline with the §3 rule, last mutation in frame M:

- Edge M: key/version mismatch → walk (uploads moving prev + final curr; piece `ResetWasUpdated` leaves `{false, true}`). `quiescent = 0`.
- Edge M+1: key/version unchanged, `quiescent 0 → 1` → **still walks**: uploads the settled `preFrameTra` into slot [0], and `GetWasUpdated()` is still true via `wasUpdated[1]` so the pieces upload their settled `prevModelSpaceTra`; `ResetWasUpdated` leaves `{false, false}`.
- Edge M+2: unchanged, `quiescent = 2` → **skip**. At this point every value in the storage is a settled pure function of the (unchanged) key: provably nothing left to write.

So the `quiescent >= 2` rule is not an approximation of the double-trigger — it is the same rule, hoisted to object granularity. Invariant used by the skip: `quiescent >= 2` ⇒ all pieces have `wasUpdated == {false,false}` and `dirty == false`, so the skipped `GetModelSpaceTransform()` calls (rts/Rendering/Common/ModelDrawerData.h:429) would have been no-ops and the skipped `ResetWasUpdated` calls would have been idempotent (`noInterpolation` is already `{false}` after the edge-M+1 reset, rts/Rendering/Models/LocalModelPiece.cpp:132-133).

### 5.2 `noInterpolation`

`Set{Rotation,Position,Scaling}NoInterpolation(true)` changes the OUTPUT of `GetEffectivePrevModelSpaceTransform` (curr substituted for prev per-component, rts/Rendering/Models/LocalModelPiece.cpp:176-187) without setting `dirty` or `wasUpdated` itself (rts/Rendering/Models/LocalModelPiece.hpp:72-74). In current code every set-to-true site is immediately preceded by a value-changing `SetPosition/SetRotation/SetScaling` in the same call, guarded by early-outs when the value would not change (`MoveNow` rts/Sim/Units/Scripts/UnitScript.cpp:523-543, `TurnNow` :546-565, `ScaleNow` :567-581), so a version bump from the value change always accompanies the flag. The set-to-false calls in the anim ticks (rts/Sim/Units/Scripts/UnitScript.cpp:306, 317, 328, 338) can only transition the flag if a `*Now` set it true this frame — which already bumped. WS-2 nevertheless asks WS-1 to bump on the `Set*NoInterpolation(true)` sites as belt-and-braces (§7 ask 2c) so the pairing invariant is enforced rather than inherited. Because flags are consumed and reset only inside the walk (`ResetWasUpdated`, rts/Rendering/Models/LocalModelPiece.cpp:132-133), and a bump guarantees the walk runs on that edge and the next, skipped edges never hold a live `noInterpolation` flag.

### 5.3 `alwaysUpdateMat`

`alwaysUpdateMat` (rts/Sim/Objects/SolidObject.h:353) bypasses the LOS gate so hidden objects keep fresh matrices for Lua consumers (rts/Rendering/Common/ModelDrawerData.h:412-415; set from Lua via boundary-apply at rts/Lua/LuaUnsyncedCtrl.cpp:2324-2336 and 2670-2681). The skip does not weaken this: skip fires only when the stored content already equals what a fresh extraction would produce, which is precisely what the flag's consumers want to read. Flag flips need no skip-state invalidation either way: flipping ON an object that changed while gated out walks (key/version mismatch, since skip state only updates on walks — §3 property 1); flipping ON an object that did not change skips, correctly. The gate itself is evaluated before the skip check, unchanged.

### 5.4 The feature drawer instance

Same template, same mechanism (`CFeatureDrawerData::Update` → `ExtractTransforms`, rts/Rendering/Features/FeatureDrawerData.cpp:236-272; edge entry Game.cpp:2199-2201). Features have no unit-script engine ticking their pieces; piece mutation is only reachable through the shared Lua object-piece paths (`Impl::SetObjectPieceMatrix` rts/Lua/LuaSyncedCtrl.cpp:98-118, piece visibility rts/Lua/LuaSyncedCtrl.cpp:848), which are WS-1 chokes on the same `LocalModelPiece` setters — so the feature `LocalModel` counter is covered by construction. Object transform changes funnel through `UpdateTransform` (rts/Sim/Features/Feature.cpp:529-534, callers at :242, :504, :518, :526, :540, :632) and are caught by the `{transMatrix, pos}` value key regardless. Nearly every feature reaches `quiescent = 2` within two edges of spawning and stays there forever; the residual cost is the per-feature gate + key compare (§10).

## 6. Interactions kept exactly as today

- **LOS gate ordering**: gate first, skip check second. Gated-out objects neither walk nor touch skip state — the storage keeps their last-seen pose exactly as the current code's early return does (design rationale comment, rts/Rendering/Common/ModelDrawerData.h:339-345).
- **New objects / catch-up**: `AddObject` initializes the skip slot to `everWalked = false` (forced walk) alongside its `transformsExtractionPending = true` (rts/Rendering/Common/ModelDrawerData.h:296-304); `ExtractPendingNewObjectTransforms` (rts/Rendering/Common/ModelDrawerData.h:366-386) reaches the same `ExtractObjectTransforms` and therefore the same skip logic — a brand-new id never skips, and its first walk primes the key.
- **Id reuse / death**: `DelObject` frees the transform allocation (rts/Rendering/Common/ModelDrawerData.h:310-320); the skip slot goes stale exactly like `drawPositions` does (documented semantics, rts/Rendering/Common/ModelDrawerData.h:169-174) and is overwritten on the id's next `AddObject`. No ring interaction: `transformsMemStorage` is a single live storage with internal prev/curr slots, not an epoch-ring channel; the SSBO upload consumes it at the consumer's park (producer comment, rts/Game/Game.cpp:2189-2191).
- **creg / checkpoint load**: `LocalModelPiece::PostLoad` re-arms `wasUpdated` (rts/Rendering/Models/LocalModelPiece.cpp:321-324); drawer registration re-runs `AddObject` for every object on load, so every skip slot starts at `everWalked = false` and the first post-load edge walks everything. Skip state is drawer-owned and never serialized.
- **Mid-frame creation**: `CondUpdatePrevTransform` re-saves `preFrameTra` during the creation frame (rts/Sim/Objects/SolidObject.cpp:454-463, called from `Move` at :162) — irrelevant to skipping since creation-frame objects always walk (fresh slot).

Accepted residual (document, don't fix): if an object's pose returns BIT-EXACTLY to its last-walked key while it was continuously gated out of extraction, with motion continuing into the very frame of the reveal edge, slot [0] would skip with a start-of-frame `preFrameTra` differing from the stored settled prev — a one-frame interpolation-base inaccuracy on a hidden→revealed object requiring an exact float coincidence. Value-impossible to hit in practice; the oracle (§8) would name it if it ever occurred.

## 7. Dependency: exact semantics required from the WS-1 counter

WS-2 consumes the WS-1 `LocalModel` capture-version. The WS-1 doc must guarantee:

1. **Shape**: a monotonically non-decreasing per-`LocalModel` integer, plain member (no atomics), readable via a const getter. 64-bit preferred; 32-bit acceptable if WS-1 documents the wrap (equality after exactly 2^32 bumps between two reads of the same object is the only false-skip mode; at script-tick bump rates that is years of game time, but say it).
2. **Bump coverage** — the counter must have bumped since the previous extraction edge whenever any piece content feeding `modelSpaceTra`, `scriptSetVisible`, `noInterpolation`, or `blockScriptAnims/pieceSpaceTra` changed. Concretely: (a) value-changing `SetFloat3`/`SetFloat` (rts/Rendering/Models/LocalModelPiece.cpp:91-118 — the `SetBoundariesNeedsRecalc` aggregate notification at :99/:114 is the precedent hook, but note its guard is `!dirty && changed`, see 2e); (b) `SetScriptVisible` (rts/Rendering/Models/LocalModelPiece.cpp:165-169) — it sets `wasUpdated` without `dirty`; (c) the `Set*NoInterpolation(true)` sites (rts/Sim/Units/Scripts/UnitScript.cpp:542, 564, 580) — belt-and-braces per §5.2; (d) the external `SetDirty`/`SetPieceSpaceMatrix` callers (rts/Lua/LuaSyncedCtrl.cpp:113-114); (e) if WS-1 bumps only on the `dirty` false→true transition, it must also document the invariant that makes this sufficient: `dirty` is cleared before any frame's mutations begin (frame-start `SavePrevModelSpaceTransform` → `GetModelSpaceTransform` recompute, rts/Sim/Objects/SolidObject.cpp:485-487 + rts/Rendering/Models/LocalModelPiece.cpp:141-154), so mid-frame `dirty == true` implies a bump already happened this frame. A second value-change on an already-dirty piece then needs no second bump for WS-2's purposes (both precede the same edge).
3. **Timing/threading**: bumps happen on the sim thread strictly before the epoch edge of the frame containing the mutation; readers are the same sim thread at the edge, plus the parked-main catch-up path (§9) — no concurrent read/write pairs.
4. **Non-destructive**: the counter is never reset or consumed by WS-1's own per-slot skip; each consumer (WS-1 piece capture, WS-2 transforms) keeps its own last-seen copy. It must never move backwards while an object stays registered with a drawer.
5. **Recompute paths don't bump**: `UpdateChildTransformRec` / `UpdateParentMatricesRec` (rts/Rendering/Models/LocalModelPiece.cpp:210-253) set `wasUpdated[0]` when recomputing a dirty piece but are derived-state refreshes, not mutations; bumping there would cost idle objects extra walks (and a frame-start recompute bump would land after the prior edge, forcing a spurious walk at the next). If WS-1 finds it must bump on recompute for its own reasons, WS-2 stays correct (only slower) — state which it is.

## 8. Verification plan

Program rule (overview §6): NO A/B pixel gate for decoupling work — verification is value-equivalence reasoning + oracle, demo-resim, benchmarks, and eyeball for the interpolation semantics.

- **Value-equivalence oracle** (required for DS work, overview insight §2.4): config knob `SimDrawTransformSkipOracle=N` (0 = off). Every N epochs, for each object the skip predicate would skip, run the full legacy computation into scratch (`FromMatrix(GetTransformMatrix())`, `preFrameTra`, per-piece `GetModelSpaceTransform`/effective-prev — read-only, no `ResetWasUpdated`, no storage writes) and bit-compare against the storage slots; `LOG_L(L_ERROR, ...)` the object id, slot index, and which key half claimed quiescence on mismatch. This turns "missed a mutation path" into a named failure. Note the oracle's piece reads may recompute `dirty` pieces — legal at the edge (sim quiescent) and side-effect-equivalent to what the walk would do.
- **Flag-off byte-identity + demo-resim**: the skip state is drawer-owned and the mechanism adds only READS of sim state (`pos`, dirs, `transMatrix`, the WS-1 counter) — no synced writes, so resim must be trivially clean; run the standard gates anyway (handbook §5 recipes 1 and 4 — note the handbook's worktree paths are stale; everything runs from the main checkout, and the skip is active in BOTH split modes since `ExtractObjectTransforms` is shared, so recipe 1 flag-off runs exercise it too).
- **Benchmarks**: re-report the overview §1 zone table (`Sim::EpochProduce::TransformsUnits`/`TransformsFeatures`, rts/Game/Game.cpp:2195, 2199) on the established late-game replay; the `BoundaryStats` `traChecked/traChanged/traForced` counters (rts/Rendering/Models/ModelsMemStorage.h:39-58) quantify eliminated storage traffic; add a skip-rate counter (objects skipped / walked / gated per epoch) to the drawer for the report.
- **Eyeball (headful, the interpolation semantics)**: the failure signature is a one-frame pose jerk exactly when animation or motion STOPS. Watch: walking units halting; factory nano/pad animations completing; units stopping a turret turn; `MoveNow`/`TurnNow`-heavy scripts (instant-set paths, §5.2); transported units attach/detach (rts/Sim/Units/Unit.cpp:801-802); a wreck pushed by an explosion settling (feature path); paralyzed units mid-anim. Compare against a no-skip build at 1x and under FF. Per program memory, split-widget symptoms must be judged headful, not headless.
- **TSan**: not required — no new cross-thread access by design (§9); the epoch TSan segment recipe applies only if review disputes the threading argument.

## 9. Threading argument

All skip-state reads and writes occur inside `ExtractObjectTransforms`, which has exactly three callers, already mutually excluded today:

1. Split enabled, producer: sim thread at the frame edge, forced-serial (rts/Game/Game.cpp:2193-2201; `allowMT=false` contract at rts/Rendering/Common/ModelDrawerData.h:83-87). Single-threaded over objects.
2. Split enabled, consumer catch-up: main thread under the park (`ExtractPendingNewObjectTransforms`, rts/Rendering/Common/ModelDrawerData.h:88-98 — sim parked, so never concurrent with caller 1). Serial loop.
3. Split disabled: `ExtractTransforms(true)` from the drawer `Update()` on the main thread (rts/Rendering/Units/UnitDrawerData.cpp:229-232, rts/Rendering/Features/FeatureDrawerData.cpp:245-248), possibly `for_mt` (rts/Rendering/Common/ModelDrawerData.h:399-402). Workers write disjoint per-id slots of a preallocated dense vector — the same disjoint-per-id discipline the existing `UpdateForced` storage writes rely on; no growth, no shared mutable state beyond that (chunked at 256 ids, rts/Rendering/Common/ModelDrawerData.h:59, so false sharing across a chunk boundary is bounded and harmless — no correctness dependence).

Skip-state slot (re)initialization happens in `AddObject`/`DelObject`, which run where drawer registration already runs (boundary drain / parked windows). The sim-state reads the key adds (`pos`, dirs, `transMatrix`, WS-1 counter) happen only where the extraction already legally reads the same objects. TSan-neutral by construction; the counter is single-writer (sim thread) with readers only in the two sim-quiescent windows above (WS-1 ask 3).

## 10. Staging and expected numbers

**PR-1 (the workstream): whole-object skip, both drawers.** Skip-state vector + key compare + `quiescent>=2` rule in `ExtractObjectTransforms` (shared template — one implementation covers units and features), `AddObject` init, oracle knob, skip-rate counters, gates per §8. Lands only after WS-1's counter (overview §5 sequencing). Expected against the §1 baseline:

- Units: 2.73 ms → **~0.5–0.9 ms**. Floor = the per-object gate + key compare scan (~2–3 cold cache lines per unit: `pos`+dirs on `CSolidObject`, the `LocalModel` counter, the dense skip slot) over `unsortedObjects` — order 0.2–0.4 ms at late-game unit counts — plus full walks for the genuinely moving/animating minority (each mover also pays one extra settle walk after stopping, §5.1). The S0 telemetry finding that per-frame piece churn was 4–8x lower than estimated (program memory) is what makes the skip-rate high enough for this band.
- Features: 419 µs → **~60–120 µs** (compare floor over the feature population; nearly 100% skip rate after the first two edges).

**PR-2 (optional, evidence-gated): feature dirty-list + walker micro-costs.** If PR-1's measured feature floor matters: replace the per-feature scan with a DL choke — `CFeature::UpdateTransform` (rts/Sim/Features/Feature.cpp:529-534) pushes the id to a drawer-drained pending set; the edge visits only {pending ∪ pending-new}, with ids retained in the set until they pass the LOS gate (moved-while-hidden features must extract on reveal, §6). Requires the Move-caller audit the value key exempted us from (§4) — every feature `pos`/dir change must reach `UpdateTransform`; the WS-1 counter still guards Lua piece mutations. Gets features to true **~0** (visits ∝ changed features, typically zero). Same PR can cache the `ScopedTransformMemAlloc*`/offset in the skip slot to kill the per-walk hash `find` (rts/Rendering/Common/ModelDrawerData.h:143-146) for the objects that do walk — invalidated on `DelObject` with the slot. Only file this PR with PR-1's numbers in hand (mirror-layer churn rule: per-mutation chokes need the evidence first).

Combined with the overview program: WS-2's residue is further divisible by WS-8 (MT-at-edge) if that ruling lands — the key-compare scan is embarrassingly parallel — but nothing here depends on it.

## 11. LANDING NOTES (2026-07-12, PR-1 implementation)

Landed shape deviates from the design in these points; everything else is as specified above.

- **Quiescence is OBSERVED, not derived (post-gate correction).** The first landed cut implemented the §5.1 timeline literally (skip at the second consecutive unchanged edge) and the oracle immediately caught persistent stale `piecePrev` slots in both gate modes: the `wasUpdated[2]` double-trigger can drain without the prev slot settling (e.g. a pending-triggered re-extraction in the SAME sim frame consumes both triggers before the frame-start `prevModelSpaceTra` re-save — a drain path §5.1's one-pass-per-edge model does not cover). The landed rule is observational: `quiescentEdges` advances only on a frame-distinct completed walk that was unchanged AND observed zero armed pieces (wrote no piece slots); any piece write resets it; skip still requires `>= 2`. Corrected timeline, last mutation in frame M: edge M walks (mismatch), edge M+1 walks (pieces still armed via `wasUpdated[1]`, uploads the settled slot-[0] `preFrameTra` and settled `prevModelSpaceTra`), edges M+2/M+3 walk clean (counter 1, then 2), edge M+4 skips. Skip is then provable by observation: two clean walks show the piece channel drained, an unchanged WS-1 version excludes re-arming (arming requires recompute-of-dirty or `SetScriptVisible`, and every dirty/visible transition bumps; `SetDirtyRaw` is only the TickAllAnims BFS clearing), and the object slots are idempotent under the value-shadow key. The feature drawer shares the template, so the correction covers both drawers.
- **`lastWalkFrame` added to the record.** `ExtractTransforms` can legally run more than once per sim frame (`transformsExtractionPending` re-triggers, split-off mode); same-frame re-walks are byte-identical no-ops and must not double-count a quiescent edge, so the increment is gated on `gs->frameNum != lastWalkFrame`.
- **`alwaysUpdateMat` objects never skip** (orchestrator ruling, stricter than §5.3's it-would-be-correct argument): the flag's Lua consumers get a forced walk every edge. The record is still maintained for them.
- **Feature key stores the matrix as raw `float[16]`**, not a `CMatrix44f` member: `CMatrix44f` is `alignas(64)`, which would put 52 B of indeterminate tail padding under the `memcmp`.
- **Oracle compares against what a walk WOULD WRITE, not from-scratch freshness.** Object slots [0]/[1] via `Transform::equals` (the `UpdateIfChanged` eps-swallow means storage may legitimately differ from a fresh decompose by < eps). Piece slots bitwise (`UpdateForced` bytes) but ONLY for pieces whose `wasUpdated` trigger is armed — a walk leaves unarmed pieces' bytes untouched, so drained-trigger residue the flag-off baseline shares is value-equivalent by the byte-identity-to-baseline contract, not a skip defect (the initial unconditional compare flagged exactly this class). Under an unchanged WS-1 version, an armed piece on a skipped object indicates a bumpless arming path — the class the oracle exists to name. Knob `SimDrawTransformSkipOracle=N` counts extraction passes per drawer.
- **Counters are drawer-member relaxed atomics** (`gated/walked/skipped` + oracle checked/mismatched), logged as one `[EpochStats] transformSkip(<drawer>)` line at drawer teardown — not new BoundaryStats CSV columns.
- **`preFrameTra` is shadowed as a value in the record (second post-gate correction); the §6 "accepted residual" is eliminated.** The re-gate after the quiescence fix surfaced a transient `objPrev` class (58 strict / 2 flag-off hits, late-game, units only): slot [0]'s input `preFrameTra` (WorldObject.h `preFrameTra`, captured at frame START in `CSolidObject::UpdatePrevFrameTransform`) is a frame-start sample of {pos, dirs}, while the §4 key samples the frame END — and only at extraction edges. With several sim frames between edges (the FF gates; also the split producer under FF), a pose can change and return BIT-EXACTLY inside the blind window — common for rotation, since dirs re-derive from the quantized `heading` (`GetVectorFromHeading`: same heading, identical bits) and turn-in-place leaves `pos` untouched. The next edge's key then matches the record (skip) while `preFrameTra` still carries the excursion pose captured at that frame's start; it self-heals one frame later, matching the transient one-or-two-hits-per-id signature (and `objCurr` can never mismatch, being a pure function of the matching key). §6's "value-impossible" assessment of bit-exact return considered only float-valued motion, not heading-quantized rotation, and only the gated variant, not sparse edges. Fix: the record shadows the consumed `preFrameTra` value itself (32 B, bitwise); skip now requires the bytes `UpdateIfChanged(0, ...)` would reconcile to be the bytes the last walk already reconciled — exact by construction, closing both the sparse-edge and the gated (§6 residual) variants. Idle objects have a constant `preFrameTra`, so the skip rate is unaffected.
- **WS-1 dependency check (§7)**: the landed counter is the packed per-instance `{instanceSeed:32|localCount:32}` scheme (pieces-capture-version-skip.md §11), not the global-source scheme §7 sketched. All five asks hold: plain member + const getter (`LocalModel::GetPieceTreeVersion`), bump coverage at the SetDirty entry / `SetScriptVisible` / no-interpolation arming / colvol acquisition / `CLuaUnitScript::CreateScript`, sim-thread bumps phase-separated from the edge reads, never consumed/reset (monotonic across a `SetModel` reseed), and the recompute paths (`UpdateChildTransformRec`/`UpdateParentMatricesRec`) do not bump. The accepted false-skip mode is the per-instance 2^32 `localCount` wrap (~33 days of continuous max-rate animation of one object between two edges of the same instance).
