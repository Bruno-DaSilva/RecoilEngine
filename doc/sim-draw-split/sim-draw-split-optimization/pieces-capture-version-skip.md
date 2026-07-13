# WS-1 — Pieces: capture version-skip (child design doc)

Status: DESIGN (no code). Parent: `doc/sim-draw-split-optimization/00-program-overview.md` §4 WS-1. The parent's §2 (core insights), §3 (strategy taxonomy) and §6 (verification protocol) are binding shared context and are not re-argued here. Strategy class: DS (dirty-skip) with a version-counter primitive that WS-2 also consumes (§5 of the parent: WS-1 builds it, WS-2 consumes it).

Baseline being attacked: `Sim::EpochProduce::Pieces` = **2.47 ms** per publish, units dominate (parent §1; the `PiecesUnits`/`PiecesFeatures` sub-timers are at rts/Lua/LuaSnapshotServe.cpp:7847 and :7871). Target: **~0.4–0.8 ms**.

All file:line references below were verified against the branch head (`bruno/poc-split-sim-draw`, post-7c367fe65d) on 2026-07-12. Note SimSnapshot lives at rts/Rendering/Common/SimSnapshot.{h,cpp} and the diff gate at rts/Rendering/Common/SnapshotDiffGate.cpp (the agent handbook's older paths are stale).

## 1. Problem

`LuaSnapshotServe::RefreshPieces(ringSlot, targetEpoch)` (rts/Lua/LuaSnapshotServe.cpp:7756-7886) is the epoch producer's piece-cache channel. Per produced epoch it:

1. Drains the draw→producer first-touch registration mailbox into the read-sets (:7787-7804).
2. Walks ALL active units to pre-register live nano jobs (demand-gated on `unitPieceDemandSeen`, :7817-7835).
3. Wipes `present=false; deadMiss=false` across the ENTIRE `unitPieceCache` (:7837-7840) and `featurePieceCache` (:7865-7868). These vectors are sized `maxUnits` (:7781-7782) and read-set-size respectively; `ObjectPieceSlot` (:7245-7265) is ~170 B (2 bools + meta ptr + 2×int32 + two 24-B vector headers + a ~80-B `CollisionVolume` + last-hit pair), so the wipes stride ~5–6 MB of slot memory to write two bytes per slot — a fixed O(maxUnits) floor even on a quiet epoch.
4. For every read-set id, re-captures the object FROM SCRATCH via `RefreshObjectPieceSlot` (:7328-7388): per piece, `GetAbsolutePos()` + `GetModelSpaceMatrix()` + `GetEmitDirPos()` + object-space emit conversion + a full `CollisionVolume` copy + `scriptVisible` (`PieceDynamic`, :7236-7243, ~200 B/piece), plus the unit's `scriptToModel` table (:7369-7376), object colvol (:7378-7380) and last-hit pair (:7382-7387).

The read-set is demand-shaped but effectively grow-only while an object lives: "every builder that ever lathed, until death" (nano pre-registration keeps re-adding active lathers; a queried id stays registered until death, :7853-7857). Late game that is thousands of mostly-IDLE units whose piece trees have not moved in minutes — re-gathered in full every epoch. This is exactly insight §2.2 of the parent: the object is cold at the edge, and the mutation site already knows when it changed.

## 2. Mechanism overview

Three parts, matching the parent §4 preamble:

- **(a)** A **capture-version counter on `LocalModel`**, bumped from the existing piece-mutation chokes. Semantics in §3; it is the primitive WS-2 consumes too.
- **(b)** A **per-slot skip key** stored in `ObjectPieceSlot` at capture time; at refresh, key match ⇒ the slot's content is bit-identical to what a fresh capture would produce ⇒ skip the whole capture. Key composition in §4.
- **(c)** **Retirement of the all-slots `present=false` wipe loops** (:7837-7840, :7865-7868) — which would defeat (b) anyway — in favor of a producer-owned death journal with per-ring-slot drain cursors. §5.

Everything stays producer-owned, single-threaded-at-the-edge state; no new cross-thread access (threading argument in §3.4, per parent §6).

## 3. The capture-version counter primitive (built here, consumed by WS-1 and WS-2)

### 3.1 Definition

- New member `uint64_t pieceTreeVersion` on `LocalModel` (rts/Rendering/Models/LocalModel.hpp, beside `needsBoundariesRecalc` at :94), with accessor `GetPieceTreeVersion()` and choke method `BumpPieceTreeVersion()`. `CR_IGNORED` in creg (LocalModel.cpp:8-14 metadata; see §3.3 for why load stays safe).
- **Values are globally unique, never per-instance-sequential**: every bump assigns `pieceTreeVersion = ++g_pieceTreeVersionSource` from a file-scope monotonic `uint64_t`, and `LocalModel::SetModel` (rts/Rendering/Models/LocalModel.cpp:55-96) assigns a fresh value on BOTH its paths — initialize (:77-95) and creg PostLoad (`initialize=false`, :62-75).
- This is the command-queue cache's proven id-reuse pattern: `CCommandQueue` versions are globally unique, so "a died-and-respawned id always lands in the recopy branch … the flags can never go stale across id reuse" (rts/Lua/LuaSnapshotServe.cpp:6625-6631, and the cmd-desc variant :6644-6651). With global uniqueness, `key.captureVersion == lm.pieceTreeVersion` proves BOTH "same LocalModel instance" AND "no piece-tree mutation since capture" in one u64 compare. A per-instance 0-based counter would NOT be safe: two same-def never-animated units (predecessor died, id reused within one FF multi-frame produce window — §5.3 case 4) would both sit at version 0 and could alias on an otherwise-matching key.
- u64 kills any wraparound discussion; the source is bumped only by sim-side mutations, so overflow is unreachable.

### 3.2 Exact semantics (the contract both consumers rely on)

`GetPieceTreeVersion()` is unchanged between two reads at sim edges ⇒ **no mutation occurred in between that can change any live-accessor-visible value of the piece tree**, specifically: piece-space pos/rot/scale (hence all model-space transforms, absolute positions, emit pos/dirs), `SetUnitPieceMatrix`-class matrix pokes / `blockScriptAnims`, per-piece script visibility, per-piece collision volumes, the unit's script→model piece mapping, and (as a WS-2 provision) `noInterpolation` arm-flips.

It deliberately does **NOT** cover, and consumers must value-key instead:

- **Whole-object transform** (`pos`, `frontdir`/`rightdir`/`updir`). Reason: there is no clean choke — `CSolidObject::pos` and the dir vectors have direct-assignment writers all over the sim (the parent's WS-3 stage-2 inventory counts ~25 direct `->pos =` bypasses; e.g. `ForcedMove`/`SetDirVectorsEuler` reached from rts/Lua/LuaSyncedCtrl.cpp:786-813, plus every movetype). A counter claiming to cover movement would be **bypassable**, which parent §2.5/handbook §7 class as a blocking review finding; a value key is bypass-proof by construction and costs a ~40-B compare. This is the answer to "WS-2 needs it to also cover whole-object transform change": the **primitive is the pair** {counter for the piece tree (clean chokes exist)} + {value key for the object transform (no clean chokes exist)} — both consumers compose the same two ingredients. WS-2's key must use the full basis (see §4 note), not `frontdir` alone.
- Last-hit piece / hit frames (`SetLastHitPiece` is a clean funnel, rts/Sim/Objects/SolidObject.h:209-212, but it is object-level state, not LocalModel state; WS-1 value-keys it, §4).
- Object-level `collisionVolume` (same: object state, Lua-settable, value-keyed, §4).
- `wasUpdated`/`prevModelSpaceTra` interpolation bookkeeping — draw-side consumption semantics; WS-2's own doc handles the double-trigger trap (`ResetWasUpdated`, rts/Rendering/Models/LocalModelPiece.cpp:120-134).

Bumps are conservative (may over-bump: e.g. `SetDirty`'s child recursion bumps once per non-dirty child entered, `SetScriptVisible` bumps even on a same-value write). Over-bumping only costs a spurious recapture; under-bumping is the bug class the oracle (§6) exists to catch.

### 3.3 Coverage proof for the conditional dirty-notify

`SetFloat3`/`SetFloat` call `SetDirty()` only on a `!dirty && value-changed` transition (rts/Rendering/Models/LocalModelPiece.cpp:96-99, :111-114). The bump lives at `SetDirty()` entry (:80-89), so a piece that mutates while already dirty does not re-bump. This is safe **because the counter is model-level and every capture clears every dirty flag**: `RefreshObjectPieceSlot` reads `GetAbsolutePos()`/`GetModelSpaceMatrix()` for ALL pieces (rts/Lua/LuaSnapshotServe.cpp:7349-7355), and a dirty piece's accessor runs `UpdateParentMatricesRec`, clearing `dirty` (LocalModelPiece.cpp:141-163, :236-253; the producer/parked contexts pass the :150 recompute predicate). Likewise the sim's own anim pass clears all dirty flags every tick (`TickAllAnims` BFS, rts/Sim/Units/Scripts/UnitScript.cpp:199-278, `SetDirtyRaw(false)` at :240/:256). Therefore, after any capture, `dirty==false` on every piece; any later value change necessarily takes the `SetDirty` branch and bumps. Invariant: **key match ⇒ version unchanged ⇒ no `SetDirty` since capture ⇒ no piece value changed since capture**. (Per-PIECE counters would break this proof — the parent-marks-child recursion suppresses the child's own notify — which is why the counter is on `LocalModel`.)

The recompute-only paths that clear or set `dirty`/`wasUpdated` without any source-value change correctly do NOT bump: `TickAllAnims`'s BFS (:236-252), `UpdateParentMatricesRec` (LocalModelPiece.cpp:236-253), `UpdateChildTransformRec` (:210-234), `UpdatePieceSpaceTransform` (:189-192, called only inside the dirty branches at UnitScript.cpp:242/:258).

### 3.4 Threading

Single-writer, no atomics, stated per parent §6:

- All bump sites (§4 table) are synced sim code: unit-script ticks, COB threads, synced `LuaSyncedCtrl` — which runs on the SIM thread under the running split (handbook §6, the 44b ownership lesson) and single-threaded flag-off.
- The draw thread never bumps: its only legal piece-state writes are (a) the lazy matrix recompute, gated to parked/sim-not-running contexts (LocalModelPiece.cpp:150, :159) and touching only the mutable cached fields — no bump site on that path — and (b) the first-touch capture, which runs under a counted sim park (rts/Lua/LuaSnapshotServe.cpp:7417, :7455).
- Readers: `RefreshPieces` on the producer (sim) thread at the edge (rts/Game/Game.cpp:2186) — same thread as the writers; the lockstep-barrier refresh with the sim parked (Game.cpp:1879); WS-2's `ExtractTransformsAtSimEdge` at the same edge (Game.cpp:2196). Every read is either same-thread or inside an existing park/publish bracket, so ordering is inherited from the split's existing fences. TSan should be quiet by design; no dedicated TSan leg needed beyond the program's usual cadence.

## 4. Per-slot skip key and skip logic

Stored in `ObjectPieceSlot` (rts/Lua/LuaSnapshotServe.cpp:7245-7265) by every successful `RefreshObjectPieceSlot` — producer path AND the first-touch park path (:7430-7432, :7468-7470), so a first-touch capture is skippable at that slot's next production like any other. Fields, each justified by a captured value that depends on it:

| Key field | Covers captured state | Dependency site |
|---|---|---|
| `uint64_t captureVersion` (= `lm.pieceTreeVersion`) | `absPos`, `modelSpaceMat`, model-space emit pos/dir, `pieceColVol`, `scriptVisible`, `scriptToModel` | :7354-7359, :7364-7365, :7369-7376 via §3 chokes |
| `float3 pos, frontdir, rightdir, updir` (object) | `posDirPos`/`posDirDir` — world-space emit points via `GetObjectSpacePos/Vec` | :7360-7361; rts/Sim/Objects/SolidObject.h:234-235 uses **all three** axes + pos |
| `int32 pieceHitFrame` + `const void* hitPiece` (= `pieceHitFrames[true]`, `hitModelPieces[true]`) | `lastHitPieceIndex`/`lastHitFrame` | :7382-7387; writers §4.1 group F |
| object `CollisionVolume` — no extra storage: memcmp `slot.colVol` (:7262) against `o->collisionVolume` | `colVol` served by Get{Unit,Feature}CollisionVolumeData | :7378-7380; POD block, rts/Sim/Misc/CollisionVolume.h:142-160 (5×float3 + 2 floats + int8[4] + 4 bools) |

Notes:

- **The parent §4's key shorthand "unit pos, frontdir" is widened to the full `{pos, frontdir, rightdir, updir}` basis.** `GetObjectSpacePos` (SolidObject.h:235) uses all three dir vectors; a roll (updir/rightdir change with frontdir fixed — transports, aircraft, floating wrecks) would otherwise skip with stale world-space emit points. This is a correctness completion, not a re-litigation. WS-2 inherits the same widening for its object-transform key.
- `pieceHitFrame` alone is almost sufficient (hits happen during frame processing; the capture runs at the frame edge after them, Game.cpp:2104-2257, so any post-capture hit carries a strictly larger frame number), but including the 8-byte `hitPiece` pointer makes the argument unconditional (same-frame multi-hit reordering) at negligible cost.
- `numPieces`/`metaKey`/model identity need no key field: a `LocalModel`'s model is set exactly once per instance life (`SetModel` callers: rts/Sim/Units/Unit.cpp:262 creation, rts/Sim/Features/Feature.cpp:224 creation, rts/Sim/Objects/SolidObject.cpp:103 creg PostLoad) and each `SetModel` re-seeds a globally-unique version (§3.1), so version equality implies model identity.
- `scriptToModel` needs no key field: the mapping is fixed from unit creation (rts/Rendering/Models/LocalModel.cpp:98-122, COB remap at CobInstance init — both complete before the spawn frame's edge), and the one mid-life mutator is choked (§4.1 group E).

Skip logic in the producer loop (:7849-7860 units, :7873-7884 features): for a registered id with a live object, if `slot.present && KeyMatches(o, slot)` → skip; else `RefreshObjectPieceSlot` + store key. `KeyMatches` costs one cold `CUnit`/`LocalModel` fetch + a ~100-B compare — the per-object loop body shrinks from ~`numPieces × 200 B` of gather to that compare (insight §2.3: the pass and its cold object fetch survive; the win is the body).

`RefreshObjectPieceSlot` must additionally reset `slot.deadMiss = false` (today the wipe loop guarantees that; see §5.2).

### 4.1 EXACT choke-point inventory (every mutation path, and where the bump lands)

Grep-audited over `rts/` (excluding `lib/`, build dirs); the compiler-enforcement backstop of parent §2.5 applies: `pos/rot/scale`, `scriptSetVisible`, `dirty` are already private (rts/Rendering/Models/LocalModelPiece.hpp:103-122), and `colvol` is reachable only via `GetCollisionVolume()` (:92-93), so the writer sets below are closed by the type system plus the listed public entry points. The one public data hazard is `blockScriptAnims` (:124) — written only by `SetPieceSpaceMatrix` (LocalModelPiece.cpp:136-139); grep found no other writer (reads: LuaSyncedCtrl.cpp:116, LocalModelPiece.cpp:93/:111/:262).

**A. Piece pos/rot/scale — bump site: `LocalModelPiece::SetDirty()` entry (LocalModelPiece.cpp:80-89)**

- `SetFloat3` LocalModelPiece.cpp:91-103 / `SetFloat` :105-118 (call SetDirty at :97/:113); wrappers `SetPosition`/`SetRotation`/`SetScaling` LocalModelPiece.hpp:68-70. All callers funnel here:
  - `CUnitScript::TickMoveAnim` rts/Sim/Units/Scripts/UnitScript.cpp:301-309 (:305)
  - `CUnitScript::TickTurnAnim` :311-320 (:316)
  - `CUnitScript::TickSpinAnim` :322-331 (:327)
  - `CUnitScript::TickScaleAnim` :333-342 (:337)
  - `CUnitScript::MoveNow` :523-544 (:541), `TurnNow` :546-565 (:563), `ScaleNow` :567-582 (:579)
  - (COB `MOVE/TURN/SPIN` opcodes and `Spring.UnitScript.*` both drive these `CUnitScript` methods; no other `LocalModelPiece::Set{Position,Rotation,Scaling}` caller exists in `rts/` — the repo-wide grep's other `SetPosition/SetRotation` hits are camera/moveType/projectile/RmlUi classes.)

**B. `Spring.SetUnitPieceMatrix` / `Spring.SetFeaturePieceMatrix` — bump site: rides the existing `SetDirty` call**

- `Impl::SetObjectPieceMatrix` rts/Lua/LuaSyncedCtrl.cpp:98-119: `SetPieceSpaceMatrix` (:113 → LocalModelPiece.cpp:136-139, sets only `blockScriptAnims`) then `SetDirty()` (:114) when it returned true. Entry points: `SetUnitPieceMatrix` :3787-3791, `SetFeaturePieceMatrix` :5343-5347.
- The `blockScriptAnims` true→false transition takes no `SetDirty` — and changes no served value at that instant: with the flag off, `CalcPieceSpaceTransform` (LocalModelPiece.cpp:260-266) resumes reading pos/rot/scale only at the NEXT recompute, which only a dirty-set (choked, group A) can trigger. Skip stays value-equivalent to live at every edge. **Reviewer note:** the current `SetPieceSpaceMatrix` discards the matrix payload entirely (:136-139 stores nothing but the flag) — if upstream ever restores matrix storage, the write must keep routing through this `SetDirty` funnel.

**C. Piece script-visibility — bump site: inside `LocalModelPiece::SetScriptVisible` (LocalModelPiece.cpp:165-169)**

- Captured at :7365 (and consumed by the trace rehost's piece hit-test skip, :7952 family). Callers:
  - `CUnitScript::SetVisibility` UnitScript.cpp:584-594 (:593) ← COB hide/show CobThread.cpp:712/:727, `Spring.UnitScript.SetVisibility` LuaUnitScript.cpp:1335-1344
  - `SetSolidObjectPieceVisible` LuaSyncedCtrl.cpp:839-851 (:848) ← `SetUnitPieceVisible` :3863-3866, `SetFeaturePieceVisible` :5329-5332

**D. Piece collision volumes — bump site: a new `LocalModelPiece` choke method wrapping the mutation (preferred), or at the single Lua funnel**

- Sole mutator in the tree: `SetSolidObjectPieceCollisionVolumeData` LuaSyncedCtrl.cpp:815-837 — `InitShape` :834 + `SetIgnoreHits` :835 on `lmp->GetCollisionVolume()` (:825). Entry points: `SetUnitPieceCollisionVolumeData` :3849-3852, `SetFeaturePieceCollisionVolumeData` :5316-5319.
- Grep of every non-const `GetCollisionVolume()` use: rts/Sim/Misc/CollisionHandler.cpp:342 is inside a commented-out block (:330-360); rts/Sim/Objects/SolidObject.cpp:542-548 returns it const; rts/Lua/LuaSyncedRead.cpp:483 is a read-only push. No bypass exists today; making `GetCollisionVolume()`'s non-const overload private-to-friends (or renaming it `GetCollisionVolumeMutable` with the bump inside) is the cheap compiler lock per parent §2.5.

**E. Script→model mapping — bump site: `CLuaUnitScript::CreateScript` at the swap**

- rts/Sim/Units/Scripts/LuaUnitScript.cpp:1147-1172 destroys and replaces `unit->script` (:1166-1170). The captured `scriptToModel` (:7369-7376) and served `GetUnitScriptPiece`/`GetUnitScriptNames` (rts/Lua/LuaSnapshotServe.cpp:8439/:8471 bodies) change with it while the piece tree may be value-identical — the one genuinely non-obvious choke found by this audit. Bump `unit->localModel`'s version right after the swap. (Initial script creation happens inside unit spawn, strictly before the spawn frame's edge capture — no choke needed there.)

**F. Object-level inputs — NO bump; covered by the value key (§4)**

- Object transform: direct writers are legion and bypass-prone (see §3.2) — value key `{pos, frontdir, rightdir, updir}`.
- Last-hit: `SetLastHitPiece` SolidObject.h:209-212; writers rts/Sim/Projectiles/ProjectileHandler.cpp:524/:563, rts/Sim/Weapons/BeamLaser.cpp:406, rts/Sim/Weapons/LightningCannon.cpp:78 — value key `{pieceHitFrame, hitPiece}`. (`hitModelPieces[false]` — unsynced hits — is neither captured nor served today, :7384-7387; unchanged.)
- Object colvol: `SetSolidObjectCollisionVolumeData` LuaSyncedCtrl.cpp:597 (+ engine init/def defaults) — memcmp vs `slot.colVol`.

**G. WS-2 provision — bump site: `Set{Rotation,Position,Scaling}NoInterpolation` on a false→true transition only (LocalModelPiece.hpp:72-74)**

- WS-1's captured values don't depend on these flags (capture reads current transforms, not the interpolation pair), but WS-2's skip must not miss a `MoveNow`/`TurnNow` no-interp arm whose value write was a no-op (`SetFloat3` same-value ⇒ no bump; UnitScript.cpp:541-542/:563-564/:579-580 always pair the two calls). Bump on the arming transition only — the per-tick `...NoInterpolation(false)` calls (:306/:317/:328/:338) and the consumer-side reset in `ResetWasUpdated` (LocalModelPiece.cpp:133, runs at the extraction edge) must NOT bump (the latter is not a sim mutation at all). Cheap, closes WS-2's corner case; WS-2's doc records whether it relies on it.

Anything not listed either cannot mutate served piece state (init-only: `SetParent`/`AddChild`/indices, LocalModel.cpp:98-122; `dir` set once in the ctor, LocalModelPiece.cpp:64; `AttachUnit`/`DropUnit` move the transportee object, not pieces, UnitScript.cpp:828-861) or is a recompute path (§3.3). **A mutation path found later that reaches served piece state without passing a bump site is a blocking finding** — the §6 oracle is the runtime detector for exactly that.

## 5. Slot/ring lifecycle analysis

### 5.1 Wipe retirement — the death journal

Replace the two all-slots wipe loops (:7837-7840, :7865-7868) with:

- A producer-owned, append-only **death journal** (`std::vector<{int id; bool isFeature}>` + a monotonic length): appended when the producer's capture loop observes a registered id dead — exactly the existing prune branches at :7853-7857 (`u == nullptr || u->isDead`) and :7877-7881 (`f == nullptr`), which already clear the read-set bit.
- Per-ring-slot **drain cursors**: at the top of `RefreshPieces(slot)`, apply all journal entries past this slot's cursor to THIS slot's cache (`present=false; deadMiss=false;` key invalidated), then advance the cursor. Compact the journal when all `SimSnapshot::EPOCH_RING_SLOTS` (= 3, rts/Rendering/Common/SimSnapshot.h:1439) cursors reach its end.
- Cost: O(deaths since this slot last produced), replacing O(maxUnits + featureCacheSize) strided writes.

### 5.2 deadMiss tombstones (the 7c367fe65d machinery) under the journal

The draw-side first-touch marks `deadMiss` in the HELD slot only (:7420-7424, :7457-7462), and today's per-refresh wipe clears it when that slot is next produced. The journal preserves the exact same window: the first-touch pushed the id into the registration mailbox BEFORE the park (:7412-7415, :7450-7453), so the next edge registers it, observes it dead, journals it, and the flag is cleared in each slot as the ring rotates — the same up-to-ring-depth lifetime the wipe gave it (wipes also only ran at that slot's production). One new requirement: `RefreshObjectPieceSlot` resets `slot.deadMiss = false` on a successful capture, covering the dead-missed-then-respawned-before-the-edge sequence where the journal never sees a death (the edge finds the id alive). No behavioral deviation to enumerate.

### 5.3 Died-then-reused id safety — the ordering proof

Claim: a reused id can never serve the predecessor's capture.

1. **Refresh precedes publish.** Producer path: `BeginEpochProduction` (rts/Game/Game.cpp:2181) → `RefreshPieces(slot, epochId)` (:2186) → `PublishEpoch(slot, …, pieceCacheEpoch, …)` (:2253-2256). The consumer can only acquire a slot after its publish, and the slot's meta carries the piece-cache epoch it was refreshed for (SimSnapshot.h:1363, :1292). Lockstep path: the refresh (Game.cpp:1875-1885) runs inside the same fully-parked barrier bracket as all serving — nothing reads between publish and refresh there either.
2. Therefore every slot the consumer holds was refreshed at its epoch's edge, and its journal cursor covers every death observed at or before that edge (§5.1 drain runs before the capture loop).
3. A reused id becomes `rows.Valid` (the Parse gates check rows first, :7476-7483 etc.) only in epochs at/after its spawn frame, which postdates the predecessor's death; every slot produced for such an epoch drained the predecessor's journal entry first ⇒ `present=false` ⇒ the id is served only after a fresh first-touch capture of the successor (:7412-7432) or the producer's re-registered capture. Slots holding OLDER epochs still serve the predecessor's data for those epochs — where the predecessor was validly alive; correct staleness, not a leak.
4. **Within-window reuse (no death observed):** in an FF multi-frame epoch the predecessor can die and the id respawn between two edges; the edge then finds the id alive and never journals it. The read-set bit is still 1, so the capture loop runs the key compare against the predecessor's slot — and cannot spuriously match, because `captureVersion` values are globally unique per `LocalModel` instance (§3.1): the successor's version can equal no value the predecessor's key ever held. Full recapture follows. This case is what makes global uniqueness load-bearing rather than belt-and-braces.
5. Residual (pre-existing, unchanged): the successor inherits the predecessor's read-set registration and is captured until it dies even if never queried — same as today's behavior for any registered id.

### 5.4 creg / checkpoint load

- `SetModel(model, false)` runs on the creg PostLoad path (rts/Sim/Objects/SolidObject.cpp:103) and re-seeds a fresh globally-unique version (§3.1) ⇒ every pre-load slot key mismatches ⇒ full recapture after load. The counter itself is `CR_IGNORED` — correct, since it is unsynced serving-cache state and any serialized value would be wrong under the global-source scheme.
- `LuaSnapshotServe::ClearCaches` (rts/Lua/LuaSnapshotServe.cpp:8620-8665) drops the piece caches, read-sets and mailboxes, but its only call site is game teardown (`CGame::KillRendering`, rts/Game/Game.cpp:1056-1065) — an in-place checkpoint load (the replay-rewind flow, test/replay-rewind/) does NOT pass through it, and the SimSnapshot ring reset at rts/Rendering/Common/SimSnapshot.cpp:817-835 is likewise teardown-class. Today this is masked by the every-epoch full recapture; under skip it is carried by the PostLoad re-seed alone. **Required in the same PR:** an explicit invalidation hook on the checkpoint-load path (clear read-sets/journal/cursors and zero `pieceCacheEpochs`, :7273) — ids and journal positions alias across a load, and hygiene should not hang on one PostLoad call chain. Add a rewind-harness leg to the gate list (§6) since this is the exact area the replay-checkpoint memory notes flag as fragile.

### 5.5 Side-effect note (deviation shrinks, not grows)

The PR-33 enumerated deviation (c) — `RefreshPieces` pre-touching hidden pieces' `dirty`/`wasUpdated` recompute every epoch (:7193-7206) — only ever fired for objects with dirty pieces, and a skipped object provably has none (§3.3 invariant: skip ⇒ no dirty-set since last capture ⇒ all clean). So the skip removes edge-time recomputes precisely when they are no-ops; behavior moves CLOSER to master for idle hidden objects, and WS-2's `wasUpdated`-based extraction sees exactly what master's lazy path would produce. No new deviation to enumerate.

## 6. Verification plan

Contract: served values bit-identical (parent §6). Families and legs:

1. **Armed diff-gate, flag-off, both replays, full length** (handbook §5 recipe 2; runner test/callout-diff-gate/run_diff_gate.sh): the `Route`/`RoutePieceCache` armed dual-run (rts/Lua/LuaSnapshotServe.cpp:528, :514) executes live + twin for every `GetUnitPiece*`, `GetUnitScript*`, `GetFeaturePiece*`, `Get{Unit,Feature}CollisionVolumeData`, `Get*LastAttackedPiece` call and bit-compares the Lua return slots — the piece families' existing gate. PASS = 0 mismatches, modulo the **pre-existing ≤2 `GetUnitPiece*` POV-window flaky baseline** (live=1 snap=0 shape; non-deterministic baseline, not a regression — do not chase).
2. **DS oracle (the §2.4 obligation — the missed-choke detector):** config knob `PieceSkipOracle=N` (0 = off): every N produced epochs the producer re-runs `RefreshObjectPieceSlot` into a scratch slot for every SKIPPED id and field-compares against the cached slot, logging `{unit|feature, id, first differing field, epoch}` as an error line. Enable for at least one full-length replay per gate battery and in any run investigating piece staleness. This converts a bypassed choke from silent staleness into a named failure.
3. **Flag-off byte-identity resim** (recipe 1), both replays: the bump sites live in synced files (UnitScript.cpp, LuaSyncedCtrl.cpp, LuaUnitScript.cpp) — the bumps are unsynced-consumed writes with no synced reads/gsRNG, but this gate is the cheap machine proof, and it is mandatory for any change reachable flag-off.
4. **Strict contract flag-ON + one headful flag-ON full-length** (recipes 3/4, gate economy per handbook §7): zero denials, idCoverage violations=0, no new Lua errors beyond the §4 baselines; nano/emit-point widgets (the PIECE_FIRST_TOUCH clientele) are headful-only coverage.
5. **Trace-rehost coverage rides along:** the demand piece-cache colvols feed `trace::EpochView`/`EpochPieceTreeIntersect` (:7889-7898, :7952); the armed trace dual-runs already gate those verdicts bit-exact.
6. **Checkpoint-load leg:** one replay-rewind cycle (test/replay-rewind/) with the oracle armed post-load, for §5.4.
7. **Telemetry:** `[SimPauseSurvey]` PIECE_FIRST_TOUCH engages must not jump (Game.h:135 — a jump means skip broke registration persistence); `Sim::EpochProduce::PiecesUnits/PiecesFeatures` (:7847/:7871) for the before/after; `/epochstats` pieceBytes (:7732-7754) should be ~unchanged (slots persist either way).
8. TSan: not required — no new cross-thread access by design (§3.4); ride the next program-cadence TSan segment if one runs anyway.

## 7. Staging into PRs

Single PR (matching the parent's "1 PR + optional follow-up"), two reviewable commits:

- **Commit 1 — the counter primitive (inert):** `LocalModel::pieceTreeVersion` + global source + `GetPieceTreeVersion()` + all §4.1 bump sites (A–E, G) + the colvol-mutator compiler lock (D). No behavior change anywhere (nothing reads the counter yet); flag-off trivially inert. This commit is the WS-2 dependency (parent §5) and can be reviewed as a pure choke-audit against the §4.1 table.
- **Commit 2 — the skip:** key storage + `KeyMatches` + skip branch in both capture loops + wipe retirement + death journal/cursors + `deadMiss` reset in `RefreshObjectPieceSlot` + checkpoint-load invalidation hook (§5.4) + `PieceSkipOracle` knob. All changes inside `RefreshPieces`/`RefreshObjectPieceSlot` and slot state — flag-off pays nothing new (the :7772 flag-off early-out precedes everything).
- Gate battery of §6 on the PR head; report the §1 zone table so the parent's baseline column can be updated in place (parent §6).
- **Optional follow-up PR (separate decision):** the slim-capture tier, §9 — filed only on post-landing evidence.

## 8. Expected numbers vs the §1 baseline

Cost anatomy of the 2.47 ms and where each part goes:

| Component | Today | After |
|---|---|---|
| All-slots wipe loops (:7837-7840, :7865-7868) | O(maxUnits+features) strided ~5-6 MB/epoch | death-journal drains, O(deaths/epoch) — ~0 |
| Full recapture of every registered id | ~200 B/piece gather × pieces × registered ids | only ids whose key mismatches (animated/moved/hit/mutated in the slot's ≤3-epoch window) |
| Unchanged registered ids | (same as above) | 1 cold object fetch + ~100 B key compare each |
| Nano pre-registration walk (:7817-7835) | O(activeUnits), unitDef + CAI derefs | unchanged (~50–150 µs late game, est.) |
| Mailbox drain, read-set bookkeeping | trivial | trivial |

Late game the read-set is dominated by idle ex-lathers (parent §4 WS-1), so the expected skip rate is high (order 70–90% of registered ids per epoch); actively-lathing builders re-capture mainly via the object-transform key (mobile cons) and their own emit-piece animation. Estimated landing point: **~0.4–0.8 ms**, floor = key-compare walk over the read-set + the nano walk + genuine recaptures. If post-landing measurement shows the nano walk dominating the residual, the follow-up choke is registration at nano-job start/end (`CBuilder::curBuild`-class transitions, :7822-7830 enumerate the fields) — noted, out of scope here. These are reasoning-based estimates (this task is read-only: no build, no measurement); the §6/§7 re-measure is the authority, per the parent's protocol.

## 9. Optional follow-up tier: slim capture (separate decision — NOT part of the WS-1 PR)

Sketch, per the parent §4: nano-pre-registered ids (the :7817-7835 population) get a **slim tier** capturing only `posDirPos`/`posDirDir` per piece (~36 B/piece: what `GetUnitPiecePosDir` — the nano/emit widgets' callout, served at :7676-7691 — reads), skipping the matrix/colvol/scriptVisible/absPos columns. A query to any full-capture callout (`GetUnitPieceMatrix`, colvols, `GetUnitPieceInfo`, …) on a slim slot takes one counted `PIECE_FIRST_TOUCH` park to upgrade the id to the full tier permanently. The tier is part of the skip key (a tier change is a key mismatch). Decision inputs to collect AFTER the WS-1 PR lands: (a) a counter splitting the read-set into nano-only vs full-touched ids over a replay; (b) the measured share of `RefreshObjectPieceSlot` spent on the matrix/colvol columns; (c) `[SimParkStats]` headroom for the new upgrade-park class (must stay demand-rare — the re-park storm history behind the 7c367fe65d tombstones is the cautionary precedent). If the version-skip alone lands in the 0.4–0.8 ms band, the tier is likely not worth its complexity — that is the expected outcome, and this section exists to record the option and its evidence bar, not to commit to it.

## 10. Risks and open questions

1. **Checkpoint-load invalidation gap** (§5.4) — the one piece of required work outside `LuaSnapshotServe`'s producer path; must land in the same PR. Without it, skip correctness after an in-place load hangs entirely on the `SetModel(false)` PostLoad re-seed.
2. **`SetUnitPieceMatrix` payload oddity** (§4.1-B): the matrix is currently discarded by `SetPieceSpaceMatrix` (LocalModelPiece.cpp:136-139). Value-equivalence to live holds regardless; flagged so a future upstream fix keeps the `SetDirty` funnel.
3. **Key widening vs the parent's shorthand** (§4 note): full dir basis instead of `frontdir` — needs no ruling (correctness completion), recorded here for the parent's next revision.
4. **No clean choke exists for the object transform** — by design handled with the value key; this is also the recorded answer to WS-2's "counter must cover whole-object transform" requirement (§3.2). If WS-2 wants a pure-counter check, that becomes a WS-3-stage-2 dependency (private-member sweep of `pos`/dirs), not a WS-1 item.
5. **Over-registration residue** (§5.3.5): reused ids inherit registration; pre-existing, bounded by object lifetime, not worsened.
6. No other blocking findings: the choke audit closed every mutation path in §4.1 with either an existing funnel, one new choke line (piece colvol, CreateScript), or a value-key field.

## 11. AMENDMENT (2026-07-12, implementation ruling): packed per-instance version scheme supersedes §3.1's global-source-per-bump

Implementation found §3.4's claim that every bump site runs on the SIM thread to be wrong for bump group A, the dominant group: `CUnitScriptEngine::Tick` runs `TickAllAnims` under `for_mt` (rts/Sim/Units/Scripts/UnitScriptEngine.cpp:137-140), so the tick anim functions (UnitScript.cpp:300-341) reach `SetFloat3`/`SetFloat` → `SetDirty` on threadpool worker threads — the branch even documents this at rts/Rendering/Models/LocalModelPiece.cpp ("script ticks, incl. its for_mt workers"). A file-scope `pieceTreeVersion = ++g_source` at every bump would therefore be a multi-writer data race, and a duplicated value would silently break the cross-instance uniqueness that §5.3.4 makes load-bearing.

Ruling (orchestrator): `pieceTreeVersion = {instanceSeed:32 | localCount:32}`. The seed is assigned from the process-wide monotonic source ONLY in `LocalModel::SetModel` — both the initialize and creg-PostLoad paths, both single-threaded contexts (unit/feature creation, load) — so the shared source has exactly one writer context. The count is a plain per-instance increment at every §4.1 bump site: the anim `for_mt` partitions by unit script (one script per unit, touching only its own model's pieces), and all other mutators (COB opcodes in `cobEngine->Tick`, `TickAnimFinished` listeners, synced Lua) are single-threaded sim code phase-separated from it, so the member is single-writer at any instant — no atomics, no locks. The increment is masked so a count wrap never disturbs the seed bits.

Consequences for the proofs: cross-instance uniqueness — and with it the §5.3.4 within-window id-reuse case — now rests on the seed: a successor's seed can equal no value any predecessor's key ever held. Per-object monotonicity (WS-2 §7 ask 4) holds: within a seed epoch only the count grows, and a `SetModel` reseed assigns a strictly larger seed, so the packed value also grows across the reseed. The one new false-skip mode is a per-instance `localCount` wrap after exactly 2^32 bumps between two reads of the SAME instance — order ~33 days of continuous max-rate animation for one unit — accepted and documented per WS-2 §7 ask 1. §3.4's reader argument is unchanged: edge reads happen after the anim `for_mt` join, so no read/write pair is concurrent.

Group-G drift note (current HEAD): `MoveNow`/`TurnNow`/`ScaleNow` early-return on a same-value write before arming `Set*NoInterpolation(true)` (UnitScript.cpp:531-540/:558-565/:576-581), so §4.1-G's "no-op value write still arms" scenario is narrower than written; the arming bump is still required (a value change on an already-dirty piece bumps nothing while the arm flips draw-consumed state), and the per-tick disarm calls plus the draw-side `ResetWasUpdated` reset remain bump-free as specified.
