# WS-3 — Unit/feature rows: write-through mirrors (child doc)

Status: DESIGN — no code. Parent: `doc/sim-draw-split-optimization/00-program-overview.md` §4 WS-3; insights §2.1–2.3, §2.5 and the §6 verification protocol are binding and not re-litigated here.

Scope: the four gather passes `UnitsScalars`, `UnitsBuildMove`, `UnitsWeaponsPerUnit`, `Features` (SimSnapshot.cpp:1145, 1245, 1297, 1673). NOT in scope: `UnitsLos` (WS-7), `UnitsRules` (already version-skipped, SimSnapshot.cpp:1284–1288; optional dirty-list rider noted in §5.7), `UnitsWeaponsPerWeapon` flat arrays (deferred, §8.1), projectiles (§8.3), estPath (WS-6, §8.4).

Path note: SimSnapshot and the gate infra live at `rts/Rendering/Common/` (SimSnapshot.{h,cpp}, SnapshotHash.cpp, SnapshotDiffGate.cpp) — the handbook §2's `rts/Sim/Misc/` paths are outdated. Everything is in the MAIN checkout `/www/projects/RecoilEngine`.

## 1. Problem

Every epoch publish, `SimSnapshot::Extract` (SimSnapshot.cpp:1121) walks `unitHandler.GetActiveUnits()` once per pass and re-gathers every column of every live unit from the cold `CUnit`/`CWeapon`/`AMoveType` objects: ~80 member reads per unit in `UnitsScalars` alone (SimSnapshot.cpp:1147–1241), a movetype/builder dynamic dispatch per unit in `UnitsBuildMove` (990–1119), the flanking/stockpile/shield block in `UnitsWeaponsPerUnit` (1299–1344), and ~35 member reads per feature in `ExtractFeatures` (1736–1787).

Measured late-game cost (overview §1 baseline): UnitsScalars 2.48 ms + UnitsBuildMove 778 µs + UnitsWeaponsPerUnit ~300 µs (post damages gate) + Features 567 µs ≈ 4.1 ms, of which ~3.5 ms is WS-3's share once the estPath fraction of BuildMove is credited to WS-6. This is O(world) work on the sim thread at the frame edge — the exact place that sets the sim rate (overview §1).

Per insight §2.1 the cost is the gather, not the copy: the objects are cold at the edge, hot at their own mutation sites. Per insight §2.2 the mutation site is the cheapest change-detector — the mutation IS the notification. WS-3 moves the capture to the mutation site (write-through) so the edge degenerates to dirty-column memcpys.

The in-repo precedents this generalizes: `CUnit::damagesVersion` (Unit.h:289–294, consumed at SimSnapshot.cpp:1339–1343 — took WeaponsPerUnit from 1.22 ms to ~300 µs), `CSolidObject::modParamsVersion` (SolidObject.h:434–447, consumed at SimSnapshot.cpp:1284–1288 and 1780–1783), `CCommandQueue::version` (CommandQueue.h:46–56: every structural mutator bumps a globally-unique serial, ctor draws a fresh one so id reuse can never alias). WS-3 is the same idea at column granularity, with the change-check free.

## 2. Mechanism overview

A **live column store** shadows the row namespaces: one flat array per migrated column, indexed by id, owned and written exclusively by the sim thread. Every mutation of a migrated field double-writes: the authoritative member (unchanged) and the column entry, at a compiler-enforced choke (§4). Each column carries a **mutation counter** bumped on any write. At the frame edge, the publisher copies into the target ring slot only the columns whose counter differs from that slot's **copy serial** for the column — a straight prefix memcpy, no object access. The legacy per-object gather body survives as `WriteLiveRow(id)` (creation/creg init, §3.5) and as the **oracle** comparator (§3.6) — it is never run over all ids in steady state.

What this buys, per pass: the per-unit loop (one cold object fetch + N column writes per unit) is replaced by ≤N column memcpys whose total payload is the ~2–3 MB/epoch streaming copy of insight §2.1 (~150 µs class). The double-write at the mutation site is one extra L1-resident store — the object is hot there by definition.

What it does NOT change: the ring (`EPOCH_RING_SLOTS = 3`, SimSnapshot.h:1439), the publish/acquire/retire protocol, `valid[]` semantics, the served values (bit-identical contract), the SnapshotHash/SnapshotDiffGate wiring, or anything flag-off-visible (the store is unsynced render-side state; nothing synced reads it — the render-event-queue sync-safety precedent, SimSnapshot.h:1258–1260).

## 3. Stage 0 infrastructure (PR 1 of 7)

Stage 0 lands the machinery with ZERO migrated columns (or, if the reviewer prefers a live proof, with the Stage-1 pilot folded in — operator's call, §12 Q5). Everything below is producer-side code in SimSnapshot.{h,cpp} plus one choke-helper header.

### 3.1 Live column store layout

One `LiveUnitStore` (and later `LiveFeatureStore`) singleton member of `SimSnapshot`, structurally a UnitRows-shaped SoA holding ONLY migrated columns:

- Unit columns are sized `unitHandler.MaxUnits()` — dense ids, same indexing as the slot rows (SimSnapshot.h:60–63). Feature columns are grow-only to the max id seen, exactly like `FeatureRows` (SimSnapshot.cpp:1678–1685); growth happens at the creation choke (sim thread), never at the edge.
- Column element types match the slot rows byte-for-byte (`float3` 12 B, `float4` 16 B, `SResourcePack`, `CollisionVolume`, `uint8_t`, …) so the publisher is `memcpy`. POD-copyable columns only in the memcpy path; deep columns (std::string/vector-valued: `customTooltip`, `transportees`, `nanoPieces`, `estPathPoints/Starts`, `DamagesSnap`) take the pending-apply path (§3.4).
- The store also keeps `highWaterId` — max live id + 1, maintained at creation (monotone within a game, reset at Clear()) — the memcpy prefix bound (§3.3). It must also cover this batch's died-in-batch ids so DEAD_THIS_BATCH overlays land inside the copied prefix (the §7.7 minSlots analogue, SimSnapshot.cpp:385–407).
- Memory: ~80 scalar unit columns × 32 k ids ≈ 10–15 MB resident (one extra copy of the row payload). Accepted; noted for the §7.1 epoch-memory ledger.

### 3.2 Per-column mutation counters — single-writer, no atomics

Each column carries a plain `uint64_t mutCounter`, bumped by every choke write (`store.pos[id] = v; ++store.posCounter;` — a helper macro/inline keeps the pair atomic-by-convention). NOT std::atomic, deliberately:

- The only writers are sim-thread contexts: synced sim code, synced Lua ctrl, ClientReadNet-driven net mutation, and the boundary-apply drain for unsynced ctrl pokes (§4.3) — all on the sim thread under the split, all on the main thread flag-off. Single writer either way.
- The only reader is the publisher, which runs at the frame edge on the SAME thread (`ProduceSlotInternal`, SimSnapshot.cpp:361, called from lockstep `Update()` and from the flip's `BeginEpochProduction`, cpp:458). No cross-thread access exists at all — see §9.
- Counter granularity is per-column, not per-(column,id): a bump means "this column has ≥1 changed entry since serial X", triggering a whole-prefix copy. Per insight §2.1 bandwidth is cheap and this keeps the choke write to one increment; do NOT build per-id dirty bitmaps for POD columns.

### 3.3 Per-ring-slot copy serials + the dirty-column prefix memcpy publisher

Each ring slot's meta gains a parallel array of copy serials, one per migrated column: `slotColSerial[slot][col]` = the column's `mutCounter` value when this slot last copied it.

Publisher, inside `ProduceSlotInternal` after `PickFreeSlot()` (SimSnapshot.cpp:370):

- for each migrated column: if `mutCounter != slotColSerial[target][col]` → `memcpy(slotRows.col.data(), store.col.data(), highWaterId * sizeof(elem))`; `slotColSerial[target][col] = mutCounter`.
- This is the WS-5 ring subtlety solved by construction: a column dirtied once reaches EVERY slot as slots rotate, because each slot keeps its own serial and only advances it when it copies. A slot parked (held/newest, skipped by `PickFreeSlot`, cpp:486–500) simply catches up on all columns it missed when it next becomes the target.
- Hot columns (bumped every frame — pos/speed/posErrorVector/flankingMobility class, see §5.2/§5.1) are always-dirty: they memcpy every publish. That is the design working as intended — ~10–20 hot columns × ~16 k live-id prefix × 4–16 B ≈ 1–2 MB ≈ 50–150 µs, versus 2.5 ms of pointer chasing.
- `Resize` interplay: slot rows keep their current sizing (`Resize`, SimSnapshot.cpp:1434 path; feature grow-only block cpp:1687–1727). When a slot resizes, zero its col serials so every column re-copies.

### 3.4 Deep (non-POD) columns: per-slot pending-apply lists

Strings/vectors/DamagesSnap can't memcpy. For these, the choke pushes the id onto per-slot pending lists (3 lists, one per ring slot — the cmd-queue-cache death-handling pattern the overview §4 WS-1(c) cites); the publisher drains the target slot's list with per-id deep copies (`slotRows.customTooltip[id] = store.customTooltip[id]`), then clears it.

- Push-to-all-slots at the choke keeps the "must reach every rotating slot" property without a per-id serial array. Deep-column mutations are rare (tooltip set, transport attach/detach, damages Lua override), so list sizes are trivial.
- The Stage-1 damages pair keeps its existing per-slot `expDamagesVersion` compare (SimSnapshot.cpp:1339–1343) but driven from a pending list instead of the all-units scan — behaviorally identical, loop-free (see §5.1).
- Alternatively deep columns may simply stay version-compared per id IF some pass retains a loop anyway; but any deep column in a pass we intend to kill must be list-driven, or the loop survives (insight §2.3).

### 3.5 Creation / creg-load full-row init

- `WriteLiveRow(id, const CUnit*)` — the retained legacy per-unit gather body, refactored out of `Extract`'s loops — writes EVERY migrated column entry for one id and bumps every touched column counter. Called from the unit-creation completion choke (end of `CUnitLoader::LoadUnit` / after `CUnit::PostInit`, where PreInit-block defaults like the flanking init at Unit.cpp:331–336 are final) and the feature analogue at `CFeatureHandler` creation. This is also the id-reuse story: a reused id's creation rewrites all its entries before it can turn ACTIVE (§7.2).
- Creg/checkpoint load: a store-wide `RebuildLiveStore()` — `WriteLiveRow` over all live objects + bump all counters + zero all slot col serials — hooked where the existing machinery already forces a full re-extraction on frame discontinuity (the "any frameNum mismatch including backwards jumps" contract, SimSnapshot.h:162–163). Belt-and-braces: the publisher itself detects a backwards `gs->frameNum` jump and triggers the rebuild, so replay-rewind checkpoint loads (test/replay-rewind class) cannot serve a pre-load store. Version/counter fields are runtime-only, never serialized — the `CR_IGNORED(damagesVersion)` precedent (Unit.cpp:2968) and the deliberately-unserialized `modParamsVersion` note (SolidObject.h:438–441).
- Mid-game `MaxUnits` never changes; `teamHandler.ActiveAllyTeams()` never changes; no other store-resize trigger exists besides feature growth (§3.1).

### 3.6 ORACLE MODE (mandatory — insight §2.4 / overview §6)

Config knob `SimSnapshotOracle` (int, default 0 = off; N>0 = verify every Nth publish; registered like `CONFIG(int, SimDrawSplit)`, SimDrawSplit.cpp:16). When a publish is selected:

- run the UNMODIFIED legacy gather (the retained `WriteLiveRow` body / the original pass loops) into the existing never-published scratch rows (`hashScratch` etc., SimSnapshot.h:1496–1499 — the exact precedent for producer-thread scratch extraction);
- for every id that is ACTIVE in the just-published slot, for every migrated column, compare the slot entry against the scratch entry (memcmp of the element). Per-id/per-column compare, NOT whole-column memcmp — INACTIVE entries legitimately hold stale garbage (SimSnapshot.h:111–115) and DEAD_THIS_BATCH rows are shell-sourced, not store-sourced (§7.3);
- on mismatch: `LOG_L(L_ERROR, "[WTOracle] frame=%d family=%s col=%s id=%d")` with both values rendered, log-capped per column (the `[EpochIdCoverage]` cap pattern, SimSnapshot.h:1374–1383), plus running per-column mismatch counters reported at teardown next to the Clear() stats (SimSnapshot.cpp extraction-stat block, h:1528–1532).
- A mismatch means a missed/mis-ordered mutation choke and names the column and id — the failure class this converts from silent staleness into a greppable gate failure. Oracle-armed full-replay runs are a REQUIRED per-stage gate (§10); an extra burst knob (`SimSnapshotOracleBurst` — always verify the first M publishes) catches init/creation bugs cheaply.
- Cost: one legacy extraction per N publishes; at N=16 that is ~4 ms/16 epochs ≈ noise, so oracle-armed runs are full-speed.

### 3.7 What stays edge-computed (not WT columns)

- `valid[]` — rebuilt per publish from `activeUnits` membership exactly as today (fill-0 at SimSnapshot.cpp:1129 + per-unit ACTIVE mark at 1150; feature analogue 1729/1741). It is the lifecycle arbiter, not a mirrored field. The fill+mark walk is retained as the one residual O(active) loop — trivially cheap (2 byte-writes/unit) and it doubles as the `simFrame`/`aliveCount` stamp source. (A pending-clear-list retirement of the wipe is a WS-1(c)-style follow-up, not load-bearing here.)
- `weaponOffset` — an epoch-relative prefix sum over live units (SimSnapshot.cpp:1303–1305); it exists to index the flat per-weapon arrays, which stay legacy (§8.1). It moves into the surviving `UnitsWeaponsPerWeapon` loop (§5.1), keeping `weaponCount` as a WT column (creation-init only — the weapons list is filled once at CWeaponLoader::LoadWeapons, WeaponLoader.cpp:73, and never changes size afterwards).
- The UnitRows global block (radar error scalars, alliance matrix — SimSnapshot.cpp:1131–1137) and the Teams/Players/Globals namespaces — cheap, net-mutable, out of scope (overview §1 lists them separately).
- The `UnitsLos` stride rows (WS-7) and `UnitsRules` (already version-skipped) passes keep their loops until their own workstreams retire them.

## 4. The choke-point inventory procedure (compiler-enforced, insight §2.5)

This doc cannot and must not hand-enumerate all ~80 UnitsScalars fields' writers (insight §2.5: "Do not hand-audit"). The per-stage procedure, which every WS-3 PR must follow and record in its commit message as the grep-audit table (handbook §7 choke-point rule):

1. For each field migrating in the stage, make the authoritative member `private` in its owning class (temporarily, on a scratch build) — or, where privacy is permanent policy anyway, keep it private and add the accessor pair. The `CUnit::stunned` member is the in-repo precedent: private, "access via IsStunned/SetStunned(bool)" (Unit.h:563–564, 175).
2. Build. The compiler errors ARE the complete writer inventory — every mutation site, including the ones grep misses (member-internal writes like `pos.x += …` inside `CUnit::ReleaseTransportees`, Unit.cpp:851–853, which no `->pos =` grep finds).
3. Classify each site: (a) already inside an existing choke funnel (e.g. everything routing through `CSolidObject::Move`) — one write-through at the funnel covers it; (b) a bypass write — reroute through the funnel or add a local write-through; (c) an initialization write pre-registration (PreInit/ctor) — covered by the creation `WriteLiveRow`, no choke needed, but MUST be ordered before the object can appear in `activeUnits`.
4. Land the stage with the member either left private + setter (preferred for small writer sets) or restored public with the choke-helper writes in place (for hot kinematics where a setter per `+=` is unreasonable, the funnel functions are the setters).
5. The oracle (§3.6) + the armed diff-gate field pass (§10) are the enforcement that the inventory was complete; a compiler sweep that "looked complete" but missed a placement-new/creg path shows up as a named `[WTOracle]` column.

Two writer classes need explicit handling in every inventory:

- **Unsynced-mutable fields** (`noSelect`, `selectionVolume` — flagged unsynced at SolidObject.h:350–351 and SimSnapshot.h:298–300): under the split, draw-context ctrl pokes are boundary-applied — e.g. `SetUnitNoSelect` queues its write via `LuaSplitContract::QueueBoundaryApply` and the lambda mutates on the drain (LuaUnsyncedCtrl.cpp:2472–2485). The choke is inside the lambda (and the sim-side co-writer, `UpdateVoidState`) — still single-thread, still pre-publish. Any migrated unsynced-mutable field's inventory must list its boundary-apply lambdas explicitly.
- **Creg PostLoad recomputation** (`CSolidObject::PostLoad`, CUnit::PostLoad): covered wholesale by `RebuildLiveStore()` (§3.5) — individual PostLoad writes need no chokes, but the rebuild MUST run after all PostLoads complete.

## 5. Stage plan

Sequencing rule (insight §2.3, binding): a pass's wall win lands only when its LAST column migrates and the loop dies. Stages are therefore pass-complete units; partial migrations inside a stage are fine across its commits but the PR lands whole.

### 5.1 Stage 1 (PR 2) — pilot: retire the `UnitsWeaponsPerUnit` pass

The pass (SimSnapshot.cpp:1297–1345): 18 columns + the version-gated damages pair. Chosen as pilot because it is small, already half-modernized (damages gate), and exhibits every column class the later stages need: per-frame-hot, Lua-choked, computed-from-two-objects, structural, and deep.

Disposition and FULL mutation-site inventory (verified this session):

- `weaponCount` — structural, WT column, creation-init only: `CWeaponLoader::LoadWeapons` fills `unit->weapons` once (WeaponLoader.cpp:73); no runtime resize exists. `weaponOffset` — NOT a column; its prefix-sum fold moves into the top of the surviving `UnitsWeaponsPerWeapon` loop (which iterates the same units anyway, SimSnapshot.cpp:1421–1428), at ~zero marginal cost. This is what lets the PerUnit LOOP die.
- `reloadSpeed` — one writer: `CUnit::AddExperience` (Unit.cpp:1511). Plus creation init.
- `fpsNoFire` — computed from `fpsControlPlayer` + `fpsController.mouse1/mouse2` (SimSnapshot.cpp:1310–1311). Recompute-at-input chokes: `CPlayer::StartControllingUnit` (Player.cpp:216), `CPlayer::StopControllingUnit` (Player.cpp:259), `FPSUnitController::RecvStateUpdate` (FPSUnitController.cpp:95, 102 — net-message driven, ClientReadNet context). Note RecvStateUpdate must resolve the controlled unit to write its column (the controller knows its unit).
- `flankingMode/flankingDir/flankingMoveFactor/flankingAvgDamage/flankingDifDamage/flankingMobility` — writers: `CUnit::PreInit` init block (Unit.cpp:331–336, covered by creation init); `CUnit::Update` per-frame `flankingBonusMobility += flankingBonusMobilityAdd` (Unit.cpp:734 — unconditional for every live non-beingBuilt unit, so the flankingMobility column is a per-frame-hot column: one extra store per unit per frame at an L1-hot site, and an always-dirty column memcpy — both fine, §3.3); `CUnit::GetFlankingDamageBonus` (Unit.cpp:1246–1260, DoDamage path — mutates flankingBonusDir and zeroes flankingBonusMobility); `LuaSyncedCtrl::SetUnitFlanking` (LuaSyncedCtrl.cpp:3461–3484); `CUnitScript::GetUnitVal` case 7 (UnitScript.cpp:1397 — a GETTER that writes flankingBonusDir, do not miss it); `CUnitScript::SetUnitVal` (UnitScript.cpp:1757–1771).
- `hasStockpile/stockpileNumStockpiled/stockpileNumQueued/stockpileBuildPercent/stockpileIsInterceptor` — pointer establishment: `CWeapon::Init` (Weapon.cpp:1111) + `CCommandAI::AddStockpileWeapon` (CommandAI.cpp:1742) — both run during unit creation, covered by creation init ordering (verify AddStockpileWeapon cannot run post-creation; if a Lua path can add one later, that site is a choke). Runtime writers: `CWeapon::UpdateFire` (Weapon.cpp:500, `numStockpiled--`), `CWeapon::UpdateStockpile` (Weapon.cpp:527–533, `buildPercent += p` per-frame while actively stockpiling; `numStockpileQued--`, `numStockpiled++`), `CCommandAI::ExecuteStateCommand` CMD_STOCKPILE (CommandAI.cpp:940–941, `numStockpileQued` +=/clamp), `LuaSyncedCtrl::SetUnitStockpile` (LuaSyncedCtrl.cpp:2408 `numStockpiled`, 2413 `buildPercent`). All are CWeapon members; the write-through helper targets the OWNER's unit-row columns via `weapon->owner->id` (all these weapons know their owner). `stockpileIsInterceptor` is def-derived off the same pointer — creation-init only.
- `hasShieldWeapon/shieldWeaponEnabled/shieldWeaponPower` — pointer establishment `CWeapon::Init` (Weapon.cpp:1118, creation). `shieldWeaponPower` (curPower) writers: `CPlasmaRepulser::Init` (PlasmaRepulser.cpp:111), `CPlasmaRepulser::Update` regen (cpp:164 — per-frame-hot for shielded units only), damage absorption in `IncomingProjectile` (cpp:221, 230, 255–256) and `IncomingBeam` (cpp:299), `SetCurPower` (PlasmaRepulser.h:29; callers LuaSyncedCtrl.cpp:3401 `SetUnitShieldState`, UnitScript.cpp:1722). `shieldWeaponEnabled` — TRAP: the extracted value is `IsEnabled()` = `isEnabled && !owner->IsStunned() && !owner->beingBuilt` (PlasmaRepulser.cpp:138) — a fold over unit-level state. Decision (proposed, §12 Q1): DECOMPOSE — store raw `isEnabled` as the column (writers: `SetEnabled`, PlasmaRepulser.h:27; callers LuaSyncedCtrl.cpp:3400 and CobInstance.cpp:718) and let the serving twin/diff-gate fold in the already-extracted `stunned` and `beingBuilt` rows — bit-identical boolean logic, no fan-out from the stunned/beingBuilt chokes into a weapon column. Same decomposition applies to the unit-level `stunned` reload interplay noted for Stage 4.
- `deathExpDamages/selfdExpDamages/expDamagesVersion` — keep the damagesVersion gate but convert the driver from the all-units scan to the §3.4 pending-apply list, pushed at the existing single choke `BumpDamagesVersion` (Unit.h:291; sole runtime caller `LuaSyncedCtrl::SetUnitWeaponDamages` per Unit.h:289 comment). Creation seeds via `WriteLiveRow`.

Inventory size: ~21 mutation sites across 10 files (the overview's "~10–20" estimate, top end). Per-frame-hot sites among them: Unit.cpp:734, Weapon.cpp:527 (active stockpilers only), PlasmaRepulser.cpp:164 (shields only) — all owner-hot at the write.

Loop retirement: `UnitsWeaponsPerUnit` (SimSnapshot.cpp:1297–1345) is DELETED; its `SCOPED_TIMER` zone reports 0 (drop the zone; note in the §6 benchmark table). weaponOffset/weaponCount fold lands in the PerWeapon pass the same PR. Expected: ~300 µs → ~0 (+ a few µs of memcpys attributed to the publisher zone).

### 5.2 Stage 2 (PR 3) — UnitsScalars: hot kinematics riding the existing CSolidObject chokes

Columns: `pos, midPos, aimPos, speed, heading, frontdir, updir, rightdir, relMidPos, physicalState, posErrorVector, posErrorDelta, nextPosErrorUpdate` (13 of the ~80; the pass loop SURVIVES until Stage 4 — this stage only removes its writes and, more importantly, builds/validates the kinematics chokes that everything else trusts).

The funnel already exists — these fields were designed around chokes (SolidObject.h:150–152 "this should be called whenever the direction vectors are changed"):

- `CSolidObject::Move` (SolidObject.cpp:154–163) — pos/midPos/aimPos triple; the single hottest choke (every movetype step).
- `UpdateMidAndAimPos` / `SetMidAndAimPos` (SolidObject.h:153–160), private `SetMidPos/SetAimPos` (306–319) — midPos/aimPos/relMidPos (relAimPos is not a served row).
- `SetHeading/AddHeading` (SolidObject.h:170–176), `SetHeadingFromDirection`/`SetFacingFromHeading` (180–182), `UpdateDirVectors` both overloads (186–187), `SetDirVectors` (164–168), `SetDirVectorsEuler` (163), `ForcedSpin` (143–144) — heading/frontdir/updir/rightdir (+ the midPos/aimPos knock-on via UpdateMidAndAimPos).
- `ForcedMove` overrides: CUnit (Unit.cpp:560), CBuilding (Building.cpp:49), CFeature (Feature.cpp:488, Stage 6).
- `SetVelocity/SetVelocityAndSpeed/SetSpeed` (WorldObject.h:50–62, virtual; CFeature override Feature.cpp:472, CScriptMoveType wrapper ScriptMoveType.cpp:208) — `speed` float4 (w component via SetSpeed).
- `physicalState`: `Set/Clear/Push/Pop/UpdatePhysicalStateBit` (SolidObject.h:272–283) + `UpdatePhysicalState` (virtual, SolidObject.cpp) — one choke pair; note this column also feeds Stage 4's computed bits (inVoid/crashing via HasPhysicalStateBit).
- `posErrorVector/posErrorDelta/nextPosErrorUpdate`: `CUnit::UpdatePosErrorParams` (Unit.cpp:634–…, called per-frame from CUnit::Update at 726 and from the LOS-status paths) — a contained choke; per-frame-hot columns.

The known hole: direct bypass writes to the PUBLIC pos/midPos/aimPos/dir members (SolidObject.h:403–414). The overview estimates ~25 such sites engine-wide; verified examples: `CUnit::ReleaseTransportees` wreck scatter (Unit.cpp:851–853 writes pos.x/z/y in place). Session greps found the member-internal class dominates (pointer-style `x->pos =` writes are essentially absent from rts/Sim — the writes hide inside methods of the owning classes and in movetype code operating on `owner`). The §4 private-member sweep on `pos/midPos/aimPos/speed/frontdir/updir/rightdir/heading` is the authoritative enumerator; each hit is rerouted through `Move`/`SetPosition`/the dir funnels or given a local write-through. SyncedFloat3's assignment-operator surface means the sweep must privatize the members, not just grep.

Risk note: this is the highest-blast-radius stage (every movetype, collision handler, script and transport path touches these funnels). Mitigations: the funnels get the write-through, not the call sites (a few dozen lines total if the sweep confirms funnel coverage); oracle at low N + burst for the whole gate run; the `SimSnapshotWriteThrough` fallback knob (§10) flips the pass's affected columns back to gather.

### 5.3 Stage 3 (PR 4) — UnitsScalars: cold plain scalars via compiler-driven private-member sweeps

Columns (~45): `health, maxHealth, paralyzeDamage, captureProgress, buildProgress, beingBuilt, experience, limExperience, team, allyTeam, neutral, activated, isCloaked, wantCloak, armoredState, armoredMultiple, mass, height, radius, maxRange, decloakDistance, seismicSignature, selfDCountdown, losRadius, airLosRadius, radarRadius, sonarRadius, seismicRadius, jammerRadius, sonarJamRadius, resourcesMake, resourcesUse, harvested, harvestStorage, cost, buildTime, storage, metalExtract, buildeeRadius, fireState, moveState, repeatOrders, useHighTrajectory, immobile, yardOpen, crushResistance, category, leavesGhost, buildFacing, moveDefID, defID`.

- Nothing here is per-frame-hot except `experience/limExperience` (combat-rate) and `health` (combat + regen paths); all are plain members with writer sets of 1–10 sites each (e.g. reloadSpeed-class). Procedure §4 per family: sweep, classify, choke. Families with existing notification hooks ride them (`SetMass` is already virtual, SolidObject.h:303; `team/allyTeam` changes funnel through `CUnit::ChangedTeam`, which already calls `MarkMutatedOutsideFrame`, SimSnapshot.h:1252–1260 — the WT choke lands in the same function).
- `moveDefID`/`defID`/`cost`/`buildTime`-class are creation-fixed or near-fixed: creation init + the odd Lua mutator found by the sweep.
- Commit-message audit tables per family (handbook §7); no exhaustive enumeration in this doc by design (§4).

### 5.4 Stage 4 (PR 5) — UnitsScalars: computed, reference and deep tail; LOOP DEATH

Columns (~22): `stunned, isDead, inVoid, crashing, underFirstPersonControl, blockingBits, isIdle, isPushResistant, selVol, noSelect, lastAttackerID, transporterID, customTooltip, transportees, repairBelowHealth` (+ anything Stage 2/3 consciously deferred).

- Computed-from-inputs columns recompute at their inputs' chokes (overview §4 WS-3 "oddballs"): `blockingBits` = PackBlockingBits fold over physicalState/collidableState/crushable/blockEnemyPushing/blockHeightChanges (SimSnapshot.cpp:1199, bits documented at SimSnapshot.h:333–336) → recompute at the physical/collidable bit setters (SolidObject.h:272–297) + the rare bool writers; `inVoid`/`crashing` are PSTATE bit reads → same choke as physicalState (or serve from the physicalState column and drop the separate columns — value-identical, twin-side fold, §12 Q1 class); `isIdle` = `!beingBuilt && commandAI->commandQue.empty()` (Unit.cpp:1787–1794) → recompute at the CCommandQueue mutators (they already all funnel through `BumpVersion`, CommandQueue.h:56–133 — add the isIdle write-through beside it) + the beingBuilt choke; `isPushResistant` → movetype choke (Stage 5 coordination); `underFirstPersonControl` → the same three fpsControlPlayer sites as Stage 1's fpsNoFire.
- `stunned` — already private with the `SetStunned` choke (Unit.h:175). `isDead` — the death choke (`CUnit::KillUnit` path).
- Reference columns: `lastAttackerID` (`SetLastAttacker`/DoDamage path), `transporterID` (transport attach/detach) — the id is captured at the choke, so the dangling-pointer question dies with the gather (the choke runs while the pointee is alive; death of the pointee is itself a choke via the existing DependentDied plumbing — inventory must include it).
- Unsynced-mutable: `selVol`, `noSelect` — chokes are the sim-side writers (`UpdateVoidState`, SolidObject.cpp:166–199 pushes noSelect-adjacent state; the actual noSelect sim writer per LuaUnsyncedCtrl.cpp:2465–2468 comment) plus the boundary-apply lambdas (§4). CollisionVolume is memcpy-able → POD column.
- Deep columns `customTooltip` (unitToolTipMap.Set choke), `transportees` (attach/detach) → §3.4 pending lists.
- With every UnitsScalars column migrated, the `UnitsScalars` LOOP DIES (SimSnapshot.cpp:1145–1242 deleted; the valid[]/simFrame stamp walk of §3.7 remains, folded into the publisher). The 2.48 ms lands HERE — Stages 2–3 show little wall change on this pass (insight §2.3); their gain is de-risked increments and the choke infrastructure.

### 5.5 Stage 5 (PR 6) — UnitsBuildMove (minus estPath = WS-6)

Columns: builder/factory block (`builderKind, curBuildID, buildDistance, range3D, inBuildStance, buildPower, nanoPieces` — SimSnapshot.cpp:990–1024) + movetype block (`moveTypeKind, mtMaxSpeed, mtMaxWantedSpeed, mtGoalPos, mtProgressState, mtAutoLand, mtLoopbackAttack` + the 28-field `MoveTypeBlock`, cpp:1026–1119).

- Movetype fields mutate almost exclusively inside their own Update/handler code (GroundMoveType.cpp, HoverAirMoveType.cpp, StrafeAirMoveType.cpp) plus the `LuaSyncedMoveCtrl` setters and the MoveCtrl subtype swap — CONTAINED chokes: the §4 sweep per subclass converges fast because the writer set is one .cpp + one Lua file per field family. The write-through helpers live in the movetype classes and target the owner's columns (`owner->id`).
- Per-frame-hot subset (currentSpeed/wantedSpeed/waypoints for MOVING units) behaves like Stage 2's kinematics: hot-site double-writes, always-dirty columns for the moving subset — the design's intended degradation.
- `moveTypeKind` choke: movetype construction + the MoveCtrl swap site (the `AMoveType::moveTypeClass` tag landed pre-program is the value source, overview §7).
- Builder block: `curBuild` set/clear in CBuilder/CFactory, `inBuildStance` script choke, NanoPieceCache mutators for `buildPower`/`nanoPieces` (deep column → pending list). The nano pre-registration work from the pieces PRs (memory: "gate nano pre-registration") already mapped this surface.
- SEQUENCING GATE: the pass loop dies only if `estPath` extraction (cpp:1066–1073) is ALREADY gone — Stage 5 therefore lands AFTER WS-6, or the loop survives serving estPath alone and the 778 µs win is deferred (insight §2.3). Recommendation: hard-order WS-6 → Stage 5 (§12 Q3).

### 5.6 Stage 6 (PR 7) — Features

Columns: the ~35-field feature row set (SimSnapshot.cpp:1741–1786) minus `inLosAll` (positional LOS recompute — stays edge-computed with the LOS loop, or joins WS-7's treatment).

- Nearly all cold: features mutate on damage/reclaim/resurrect (`DoDamage`/`AddBuildPower` — health/reclaimLeft/resurrectProgress chokes), burning (`fireTime/smokeTime`), rare movement (falling/impulse — `ForcedMove` Feature.cpp:488, `SetVelocity` Feature.cpp:472, the UpdatePosition path), and Lua setters. The kinematics chokes are shared with Stage 2 (CSolidObject base) — features get them for free; the sweep only chases CFeature-specific writers.
- `matXdir/matYdir/matZdir` are reads of `transMatrix` (cpp:1760–1763) — recompute at the transMatrix update choke (`CFeature::UpdateTransform`-class sites found by the sweep).
- The store is grow-only (§3.1); `modParams` stays on its existing version skip (cpp:1780–1783). Loop death retires the 567 µs zone minus the inLosAll walk (which is numAllyTeams×features and small; measure and report).

### 5.7 Optional rider (any late stage): UnitsRules dirty-list

`UnitsRules` (267 µs) is already version-skipped per id but still walks all active units to compare versions (SimSnapshot.cpp:1281–1289). The `BumpModParamsVersion` choke (SolidObject.h:444) can push to the §3.4 pending lists, killing the walk. Cheap, but OUT of the WS-3 headline scope/numbers — do it only if it rides an existing PR for free.

## 6. Pass-retirement economics (insight §2.3, applied)

Wall-time ledger — where each PR's win actually lands:

| PR | Stage | Loop retired | Expected zone delta |
|---|---|---|---|
| 1 | 0 | none | ~0 (infra + oracle; publisher zone appears, ~0) |
| 2 | 1 | UnitsWeaponsPerUnit | −0.3 ms |
| 3 | 2 | none | ~0 on the pass; chokes proven |
| 4 | 3 | none | ~0 on the pass |
| 5 | 4 | UnitsScalars | −2.4 ms (the program's single biggest step) |
| 6 | 5 | UnitsBuildMove | −0.7 ms (requires WS-6 landed) |
| 7 | 6 | Features (row part) | −0.5 ms |

Do not reorder columns across stages to "show early wins" — a pass with one un-migrated column keeps its full loop cost (one cold fetch per unit dominates; the column writes are the cheap part). The overview §6 note applies: the per-pass `SCOPED_TIMER` split in `Extract` exists for attribution and gets re-fused/deleted as passes retire.

## 7. Ring/slot lifecycle

### 7.1 Publisher position in the produce body

Column memcpys + pending-list drains replace the retired pass loops inside `Extract` (called from `ProduceSlotInternal`, SimSnapshot.cpp:414), BEFORE the dead-row shell overlay (cpp:437–440) — ordering is load-bearing, see §7.3. The lockstep no-produce path (`Update()`'s !ProduceDue() branch, cpp:334–351) is untouched: it refreshes only team/player/global channels, which are not WT columns.

### 7.2 Id reuse

A reused id's creation runs `WriteLiveRow(id)` (§3.5), which rewrites every column entry and bumps every column counter → every ring slot re-copies every column before the new object's rows can be served. Between death and reuse the store holds at-death values; slots may hold either at-death values or older ones — both unreadable behind `valid[]` (the documented stale/nil contract, SimSnapshot.h:110–120). The globally-unique-serial trick (damagesVersion/modParamsVersion/CCommandQueue) is inherited automatically for the pending-list columns because lists are drained per publish, not compared.

### 7.3 DEAD_THIS_BATCH rows (§7.7 interplay)

`ExtractDeadRowsFromShells` (SimSnapshot.cpp:1811+) stays EXACTLY as is and runs AFTER the column memcpys, overwriting the dying ids' row entries in the publishing slot from their DeferredObjectDeleter shells. Rationale:

- For plain migrated columns the overlay is value-neutral (the store's last choke write IS the at-death value the shell holds), but for sub-block-derived columns (weapon family, movetype, build-state) the shells serve DOCUMENTED DEFAULTS because PreDestruct destroyed the sub-objects (cpp:1794–1806) — the WT store would otherwise serve last-live values there, a silent behavior change vs the §7.7 contract. Keeping the overlay wholesale preserves bit-identical DEAD_THIS_BATCH semantics with zero analysis burden.
- The overlay dirties slot bytes without bumping store counters. Harmless: the affected ids are DEAD_THIS_BATCH→INACTIVE (never served after `ClearDeadThisBatch`, SimSnapshot.h:1402–1407), and reuse re-copies via §7.2. The oracle compares ACTIVE ids only (§3.6), so it cannot false-positive on overlay residue.
- `highWaterId` must cover died-in-batch ids (the minSlots analogue, cpp:385–407) so overlays land inside copied prefixes.
- The "KEEP THE FIELD LISTS IN SYNC" contract (cpp:1804–1806) gains a third leg: a column migrated to WT must ALSO keep its shell-overlay line. The oracle+diff-gate catch violations.

### 7.4 Held/newest slots and the flip

The publisher only ever writes the `PickFreeSlot()` target (cpp:486–500) — never the held or newest slot — identical to today's extraction. Copy serials make skipped slots self-healing (§3.3). Under the 44a flip the produce runs on the sim thread; under lockstep on the main thread — same code path (`ProduceSlotInternal`), and the store writers are on that same thread in both modes (§9).

### 7.5 Creg checkpoint load / replay rewind

`RebuildLiveStore()` per §3.5: full re-init from live objects, all counters bumped, all slot serials zeroed. Trigger points: the existing frameNum-discontinuity re-extraction path plus an explicit post-load hook. The rewind harness (test/replay-rewind/) plus the checkpoint gates exercise this; a missed rebuild is an immediate oracle storm (every column mismatches), which is exactly why the oracle burst knob exists.

### 7.6 Teardown

`SimSnapshot::Clear()` resets the store, counters, serials, pending lists alongside the existing stamp/ring reset. Oracle counters report at teardown (§3.6).

## 8. Explicitly deferred (out of WS-3, recorded so nobody "helpfully" adds them)

1. **Per-weapon flat arrays** (`UnitsWeaponsPerWeapon`, 1.0 ms): indexed `weaponOffset[unitID] + weaponNum` where weaponOffset is an epoch-relative prefix sum over LIVE units (SimSnapshot.cpp:1303–1305) — the layout changes whenever any unit dies/spawns, so a write-through target address is not stable across epochs. Migrating it needs stable per-unit weapon slots (e.g. `id * maxWeaponsPerDef` or a unit-lifetime-fixed offset table) — a layout rework with its own consumers (all `w*` accessors + diff gate + hash) and its own doc. WS-4's ClassifyWeapon caching already takes the worst sting out of this pass.
2. **LOS stride rows** (`UnitsLos`, 691 µs): WS-7 (layout interleave + jammer reuse) first; possibly event-driven later. The per-(unit,allyteam) computed answers (`losHandler->InLos/InAirLos/InJammer/InRadar`, cpp:1259–1267) have diffuse inputs (LOS maps), not object chokes — wrong shape for WT.
3. **Projectiles** (55 µs): churn-hostile (high create/destroy rate, free-list ids, grow-only rows) and already cheap. Leave.
4. **estPath** (`GetPathWayPoints` copy, cpp:1066–1073): WS-6 demand gate. Stage 5 depends on it (§5.5).
5. **Rules-params full dirty-list**: §5.7, opportunistic only.

## 9. Threading argument (overview §6 requires it stated explicitly)

- The live store + counters + pending lists have EXACTLY ONE writing thread: all mutation chokes execute in synced sim code, ClientReadNet handlers, or boundary-apply drains — sim-thread contexts under the split (flag-on), main-thread contexts flag-off. The publisher (sole reader of counters/store) runs in `ProduceSlotInternal` on that same thread in both modes (lockstep `Update()` = main thread; flip `BeginEpochProduction` = sim thread, cpp:458–468). Therefore: no concurrent access to any new state, no atomics needed, nothing for TSan to see — by construction, not by fencing.
- The ring slots keep their existing publish/acquire memory-order protocol (cpp:470–484); WT changes what fills a free slot, not who sees it when.
- The chokes write render-side unsynced state from synced code — sync-safe by the `MarkMutatedOutsideFrame` precedent (SimSnapshot.h:1258–1260): no synced read ever touches the store, no gsRNG/streflop involvement, flag-off demo resim must stay byte-identical (gated, §10).
- One TSan segment run at Stage 0 + one at Stage 4 (the big landing) per the epoch TSan recipe, as belt-and-braces despite the by-construction argument (gate economy: TSan alone, handbook §5.7).

## 10. Verification plan (per stage — overview §6 protocol)

Every WS-3 PR runs, in order:

1. **Flag-OFF byte identity** (handbook §5 recipe 1): `SimDrawSplit=0`, full-length both gate replays, 0 DESYNC. Catches any choke that leaked into synced behavior. Note WT is ACTIVE flag-off (the snapshot machinery runs flag-off), so this also exercises the store single-threaded.
2. **Armed diff-gate** (recipe 2): `DG_ARM=1` full-length → `[SnapshotDiffGate] … PASS (0 mismatches)` (SnapshotDiffGate.cpp:488). The existing per-field passes (unitScalars/unitEco/sensorRadii/unit:placement/unit:traceOcc/… — cpp:205–257, 631–710) compare every published row against live per boundary — this is the strongest per-column check we have and it already covers every migrated field. Known flaky baseline: ≤2 GetUnitPiece* POV mismatches (pre-existing, memory-documented) — unrelated to rows.
3. **Oracle-armed runs** (§3.6): one full replay with `SimSnapshotOracle=16` + burst, per stage → zero `[WTOracle]` lines. This is the WS-specific gate the overview §2.4/§6 mandates; unlike the diff gate it isolates store-vs-legacy at the PUBLISH edge (the diff gate compares at the consumer boundary and could in principle be masked by a compensating twin bug).
4. **Strict + headful flag-ON** (recipes 3–4) per the standing gate economy (one headful full-length per landing, alternating replays).
5. **Benchmarks**: the established late-game replay measurement; report the §1 zone table delta per PR (the retired zone AND the new publisher zone) so the overview baseline updates in place.
6. **Fallback knob**: `SimSnapshotWriteThrough` (int; 0 = legacy gather for all families, bitmask or per-family int if finer bisection proves useful). Flag-off-the-knob must be byte-identical to pre-WS-3 — it selects the retained legacy body per family. Keeps stage regressions one config line from mitigated and gives the diff-gate a self-check (knob-off vs knob-on runs must both pass).
7. New columns / changed extraction join `SnapshotHash` (HashUnitRow, SnapshotHash.cpp:40) unchanged — WT does not alter WHAT is hashed, only how the bytes got there; the hash scratch path (`HashCompletedFrame`) keeps using the legacy extractor (it hashes per sim frame off-cadence and must not consult ring serials).

## 11. Expected numbers

- Retired gather: ~3.5 ms (2.48 UnitsScalars + 0.778 UnitsBuildMove − estPath share + ~0.3 WeaponsPerUnit + 0.567 Features, per overview §1).
- Residual: publisher memcpys ~0.1–0.2 ms (hot always-dirty columns ~1–2 MB/epoch prefix + rotating-slot catch-up; worst-case all-columns ~5 MB ≈ 250 µs on a parked-cold slot), valid[]/stamp walk + weaponOffset fold ~0.05 ms, pending-list drains + dead-row overlay ~0.05 ms, choke-side double-writes amortized into sim frames (individually unmeasurable; aggregate ≈ one store per mutated field per frame — the same writes the extraction loop performed, relocated to L1-hot sites).
- Net: **~3.5 ms → ~0.4 ms** on the WS-3 passes. Confidence is moderate-high on the destination, high on the direction: the mechanism's floor is the §2.1 streaming-copy bound (~150 µs), and the residual estimate has ~2× headroom before the workstream's value proposition erodes. The main uncertainty is how many columns end up per-frame-hot (each adds ~50–70 µs/epoch of always-dirty memcpy at 16 k live ids... prefix scaling keeps this honest).
- WS-8 multiplier: the publisher's memcpys are embarrassingly parallel per column; if the MT-at-edge ruling lands, the residual publisher cost drops further — but do not design for it (overview §3 MT is barred until re-ruled).

## 12. Open questions needing operator ruling

1. **Computed-column decomposition policy** (§5.1 shieldWeaponEnabled, §5.4 inVoid/crashing): store raw inputs and move the fold to the serving twin (bit-identical, touches LuaSnapshotServe + diff gate), vs recompute-at-input-chokes (fan-out from unit-level chokes into derived columns). Proposal: decompose where the fold is pure boolean over already-served rows; recompute where it is not (blockingBits). Needs a ruling because twin changes were previously escalation-gated.
2. **MoveTypeBlock choke style** (§5.5): per-field write-through inside movetype code (clean, ~28 fields × 4 subclasses of sweep work) vs a coarser per-movetype "block dirty" flag + edge re-gather of the block for dirty ids only (less invasive, but keeps a per-dirty-id gather and its cost floor for moving armies — which are exactly the late-game load). Proposal: per-field WT; ruling requested because the fallback changes the Stage 5 win profile.
3. **Hard-order WS-6 before Stage 5** (§5.5)? Proposal: yes — otherwise Stage 5's loop survives for estPath and the PR lands winless (insight §2.3).
4. **Stage 2 fallback posture**: is the `SimSnapshotWriteThrough` per-family knob (§10.6) sufficient risk cover for the kinematics sweep, or does the operator want Stage 2 split into two PRs (funnel write-throughs first, bypass-reroute sweep second)?
5. **Stage 0 landing shape**: infra-only PR (dead code until Stage 1) vs Stage 0+1 combined (pilot proves the infra in one landing). Proposal: combined — the oracle is untestable without at least one migrated column.
6. **Oracle default**: ship `SimSnapshotOracle` default-off (proposal) or default-on at high N for a soak period on the dogfood build?

## 13. PR sequencing summary

| PR | Content | Depends on |
|---|---|---|
| 1 (+2 if combined per §12 Q5) | Stage 0 infra: store, counters, serials, publisher, pending lists, creation/creg init, oracle, fallback knob | — |
| 2 | Stage 1 pilot: UnitsWeaponsPerUnit retirement (~21 chokes, §5.1 inventory) | PR 1 |
| 3 | Stage 2: hot kinematics chokes + bypass sweep | PR 1 |
| 4 | Stage 3: cold scalar sweeps | PR 3 (shares choke helpers) |
| 5 | Stage 4: computed/deep tail + UnitsScalars loop death | PR 4 |
| 6 | Stage 5: UnitsBuildMove | PR 5, **WS-6 landed** |
| 7 | Stage 6: Features | PR 3 (shared CSolidObject chokes) |

Each PR: choke audit table in the commit message (§4), the §10 gate battery, zone-table delta reported against the overview §1 baseline.

## 14. Landing notes — Stage 0+1 combined (per §12 Q5 proposal)

Landed as one PR: LiveUnitStore (17 POD columns + the deep damages pair) in SimSnapshot, per-column plain-uint64 mutation counters, per-slot per-column copy serials, the dirty-column prefix-memcpy publisher (`PublishLiveStore`, zone `Update::SimSnapshot::WTPublish`), per-slot damages pending-apply lists, creation full-row init (`SimSnapshotWT::UnitCreated` at the end of `CUnitLoader::LoadUnit`), creg-load `RebuildLiveStore` (explicit hook at the end of `CCregLoadSaveHandler::LoadGame` + a backwards-frameNum belt-and-braces rebuild in `ProduceSlotInternal`), the `SimSnapshotOracle`/`SimSnapshotOracleBurst` oracle (§3.6, legacy re-gather into `hashScratch`, per-ACTIVE-id per-column compare, log-capped `[WTOracle]` lines + teardown counters), and the `SimSnapshotWriteThrough` fallback knob (§10.6, default 1). The `UnitsWeaponsPerUnit` loop and its sub-zone are deleted; the weaponOffset prefix sum folded into the `UnitsWeaponsPerWeapon` zone reading the (write-through) weaponCount column, no object access. `HashCompletedFrame` keeps the retained legacy gather (slot = -1) per §10.7.

Deviations from this doc, verified at HEAD:

- **§5.1 shieldWeaponEnabled trap DISSOLVED**: since 654dbd0e0d ("Convert shields to use standard collision handling"), `CPlasmaRepulser::IsEnabled()` is the raw `isEnabled` getter and the stunned/beingBuilt fold moved to `IsActive()`. The extraction (and diff gate) read the raw getter, so the column is a plain member mirror; the §12 Q1 decomposition ruling was not needed. All three shield columns are WT with chokes at `SetEnabled`/`SetCurPower` (inline, covers the Lua/COB callers) + the `Update` regen / `IncomingProjectile` / `IncomingBeam` curPower writers. The shield chokes recompute from `owner->shieldWeapon` (not the mutated repulser) so multi-shield units stay correct.
- **§5.1 damages pending list**: pushed at `LuaSyncedCtrl::SetUnitWeaponDamages` (AFTER the key writes — `BumpDamagesVersion` at the top of that function precedes the actual mutations, so a choke at the bump site would capture pre-mutation values) rather than inside `BumpDamagesVersion` itself (its other caller is the CUnit ctor, before the id exists; creation seeding covers it).
- **§5.1 fpsNoFire**: the doc's `FPSUnitController.cpp:95, 102` line refs are stale (file is `rts/Game/FPSUnitController.cpp`); one choke at the end of `RecvStateUpdate` covers both mouse fields via `controllee`.
- **highWaterId stays monotone across `RebuildLiveStore`** (only `Clear()` resets it): pre-load higher ids read as INACTIVE and a slightly larger prefix copy is harmless, versus reasoning about shrink safety.
- `weaponCount` stability verified: `CWeaponLoader::FreeWeapons` runs in `CUnit::PreDestruct`, which `CUnitHandler::DeleteUnit` defers until after `activeUnits.erase` — no ACTIVE (including dying) unit ever has a mutated weapons list.
