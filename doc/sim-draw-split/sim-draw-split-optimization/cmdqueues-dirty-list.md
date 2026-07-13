# WS-5 `cmdqueues-dirty-list.md` — CmdQueues dirty-list (923 µs → ~0.1–0.3 ms)

Status: DESIGN. Child doc of `doc/sim-draw-split-optimization/00-program-overview.md` (§4 WS-5). Binding shared context lives in the parent's §2 (core insights), §3 (strategy taxonomy — this is a **DL** dirty-list workstream), and §6 (verification protocol). This doc does not re-litigate those.

Paths are the `bruno/poc-split-sim-draw` main checkout: the command-queue serving cache and its producer are in `rts/Lua/LuaSnapshotServe.cpp`; SimSnapshot is at `rts/Rendering/Common/SimSnapshot.{h,cpp}`. (The handbook's worktree paths are stale.)

## 1. Problem

`LuaSnapshotServe::RefreshCommandQueues(int ringSlot, uint64_t targetEpoch)` (`rts/Lua/LuaSnapshotServe.cpp:6584`) is the producer that refreshes one epoch-ring slot's per-unit command-queue copies at the sim frame edge. Its body is an unconditional **O(maxUnits) scan** (`rts/Lua/LuaSnapshotServe.cpp:6611`, `for (size_t id = 0; id < maxUnits; ++id)`): for every unit id it does `unitHandler.GetUnit(id)`, and for every *present* unit it additionally does a `commandQue.GetVersion()` compare, a `GetCmdDescVersion()` compare, and — outside any version gate — `ResolveWorkerTask(unit, slot)` (a `dynamic_cast<CBuilder>` + `dynamic_cast<CFactory>` chain, `rts/Lua/LuaSnapshotServe.cpp:6247`), a `lastSelectedCommandPage` copy (`:6680`), and the factory bugger-off scalar copies for factory units (`:6663`).

So the cost floor is paid *even on a completely quiet epoch where no queue changed*: the `GetUnit` stride over `maxUnits` (late game ~32k slots), plus a per-present-unit (~few-thousand) `ResolveWorkerTask` double-`dynamic_cast` and page copy. That floor is the measured **923 µs** (`Sim::EpochProduce::CmdQueues`, parent §1) — and it is paid once per epoch-ring slot, in the exact serial span that sets the sim rate under the split. The few queues that actually changed are a tiny fraction of that walk.

The mutation-side version chokes that would let us skip the scan already exist: `CCommandQueue::BumpVersion()` bumps a globally-unique version on every queue mutation (`rts/Sim/Units/CommandAI/CommandQueue.h:56`), and `CCommandAI::BumpCmdDescVersion()` does the same for the command-description surface (`rts/Sim/Units/CommandAI/CommandAI.h:192`). The scan re-derives "what changed" by *reading* those versions for all ids; a dirty-list lets the mutation *tell us* instead.

## 2. Mechanism

Mutation-side dirty-list (strategy **DL**): the mutation chokes push the affected unit id into a producer-drained dirty structure; `RefreshCommandQueues` drains that structure for the slot it is producing instead of scanning all ids. The per-unit copy logic inside the loop body is unchanged — only the *iteration domain* changes from `[0, maxUnits)` to "the ids dirtied since this slot was last produced".

The dirty push **rides the existing version bump**. Rather than a separate hand-audited push at each mutation site, the push lives *inside* `BumpVersion()` and `BumpCmdDescVersion()` — the two functions every queue/desc mutation already funnels through. This makes the push set **exactly equal** to the bump set by construction: it is structurally impossible to have a version bump without a matching dirty push, so WS-5 inherits precisely the completeness guarantee the *current* per-slot version cache already depends on, and adds no new correctness axis (see §5 for the version-bump-gap analysis). The only extra chokes WS-5 must add are for the fields the current producer copies **unconditionally, with no version to ride** (§4.3–§4.5).

## 3. THE RING SUBTLETY (full)

### 3.1 Why it exists

The epoch ring has `EPOCH_RING_SLOTS = 3` physical slots (`rts/Rendering/Common/SimSnapshot.h:1439`). PR 44a made the command-queue cache **truly per-slot** — `std::array<std::vector<UnitCmdQueueSlot>, EPOCH_RING_SLOTS> cmdQueueCaches` (`rts/Lua/LuaSnapshotServe.cpp:6149`). The producer refreshes the slot being produced (`slot = simSnapshot.BeginEpochProduction()`, `rts/Game/Game.cpp:2181`, `:2185`); serving reads the consumer-held slot (`ServedCmdCache()` → `cmdQueueCaches[simSnapshot.HeldSlot()]`, `:6154`).

The current scan gets a critical property **for free**: each slot stores its *own* last-copied `cmdQueVersion` per unit (`UnitCmdQueueSlot::cmdQueVersion`, `:6094`), and compares it against the live queue version every produce (`:6625`). So a queue that changed once at epoch E is recopied **independently into each of the 3 slots** as the ring rotates through them over the next 3 produces — because each slot's stored version still differs from live until *that* slot is next produced. This is the "changed queue is recopied into each slot as the ring rotates — correctness by construction" note already in the code (`:6592`–`:6597`).

A naive dirty-list breaks exactly this property. If the push records "unit u changed at epoch E" into a single list that is drained-and-cleared at the very next produce (which writes only *one* slot), then the other two slots — produced at E+1 and E+2 — never learn u changed, and serve **stale** pre-E copies until u happens to change again. A queue dirtied once must reach **every** ring slot.

### 3.2 Two candidate solutions

**Option A — per-slot dirty sets (fan-out on dirty, drain per slot).** Maintain, conceptually, `EPOCH_RING_SLOTS` dirty sets, one per ring slot. A mutation adds the id to **all N** sets. Producing slot S drains only S's set (recopy/clear each id, then clear S's set). Because the id was added to all N sets, and each slot drains its own set exactly when it is produced, the mutation reaches every slot regardless of rotation order or timing.

The compact encoding of Option A — the one this doc recommends implementing — is a **per-id pending-slot bitmask** plus one active-id vector:

- `std::vector<uint8_t> cmdDirtyPending;` sized `maxUnits` (bit `s` set = ring slot `s` still owes a recopy of this id). N=3 fits in a byte with room to spare.
- `std::vector<int> cmdDirtyActive;` — the ids with any pending bit set (the drain domain).
- Dirty push `MarkCmdDirty(id)`: `if (cmdDirtyPending[id] == 0) cmdDirtyActive.push_back(id); cmdDirtyPending[id] = ALL_SLOTS_MASK;` (`ALL_SLOTS_MASK = (1u << EPOCH_RING_SLOTS) - 1`).
- Drain in `RefreshCommandQueues(slot, …)`: walk `cmdDirtyActive`; for each id with bit `slot` set, run the existing per-unit copy body, then clear bit `slot`; if the mask reached 0, swap-pop the id out of `cmdDirtyActive`.

This bitmask *is* "N per-slot sets" with the per-id, per-slot membership stored as N bits instead of N containers; the single active vector avoids N id-copies and gives O(active-set) drains.

**Option B — persistent dirty-serial per id + per-slot cursor.** Give each id a monotonically-increasing `dirtySerial` stamped from a global counter at every mutation, and append `(id, serial)` to a retained dirty log. Each slot stores the highest serial it has drained. Producing slot S replays log entries with `serial > slot[S].servedSerial`, recopies those ids, and advances S's cursor. The log must retain each entry until the *slowest* slot has consumed it (a ring/redo-log with compaction against `min` over the per-slot cursors).

### 3.3 Decision: Option A (per-id pending-slot bitmask)

Reasons:

1. **Slot-rotation correctness is by construction and trivially provable** (§3.4), with no dependence on produce ordering, log retention, or serial wraparound.
2. **Automatic dedup** across an epoch *and* across epochs (§6): the "was the mask zero?" guard means a queue hammered 50×/frame appends to the active vector at most once, and stays a single entry until fully drained.
3. **Fixed, tiny memory**: one `uint8_t`/unit + a small active vector, versus Option B's unbounded-until-compacted log and its per-entry serial storage.
4. **It composes with the existing per-slot version compare as an idempotency backstop** (§3.5): the drain keeps the `slot.cmdQueVersion != live` / `GetCmdDescVersion` compares, so an over-push (spurious or duplicate id) simply re-verifies and no-ops. Option B's cursor model makes double-processing an id after a serial-cursor bug much harder to reason about.

Option B's only theoretical edge — not touching ids that are pending for *other* slots during a drain — is marginal here: the active set is bounded by "ids dirtied within the last N=3 epochs", which late game is the order-issue rate over ~3 frames, not `maxUnits`. The bitmask's skip test on an other-slot-only entry is a single byte read. Option B is rejected.

(If profiling ever shows the other-slot skips matter, Option A degrades cleanly to *N literal per-slot vectors* — append the id to all N on dirty, drain vector[S] fully when producing S — same correctness proof, no bitmask scan. Noted as the fallback, not the plan.)

### 3.4 Worked example — N=3, one dirty, three successive publishes

Ring slots `{0,1,2}`; produce order rotates `0,1,2,0,…`. Unit `u`'s queue is mutated **once** at epoch E (say a `push_back`, which calls `BumpVersion()` → `MarkCmdDirty(u)`). No further mutation of `u`.

- At the mutation (sim update, epoch E): `cmdDirtyPending[u] == 0` → append `u` to `cmdDirtyActive`, set `cmdDirtyPending[u] = 0b111`.
- **Produce E into slot 0**: drain slot 0. `u` has bit 0 → recopy `u`'s queue into `cmdQueueCaches[0][u]`, clear bit 0 → mask `0b110`. `u` stays active.
- **Produce E+1 into slot 1**: drain slot 1. `u` has bit 1 → recopy into `cmdQueueCaches[1][u]`, clear bit 1 → mask `0b100`. `u` stays active.
- **Produce E+2 into slot 2**: drain slot 2. `u` has bit 2 → recopy into `cmdQueueCaches[2][u]`, clear bit 2 → mask `0b000` → swap-pop `u` out of `cmdDirtyActive`.

After the third publish, **all three slots hold u's E-state copy**, and `u` is no longer walked. Whichever slot the consumer holds, it serves the correct post-E queue — bit-identical to what the old full-scan-with-per-slot-version-compare produced. The naive single-frame list would have written only slot 0 at E and left slots 1 and 2 serving pre-E data at E+1/E+2: the exact bug §3.1 names, fixed by the fan-out-to-all-slots.

### 3.5 The per-slot version compare stays (as an idempotency backstop, not the driver)

The drain retains the existing `if (!slot.present || slot.cmdQueVersion != cmdQueVersion)` guard (`:6625`) and the `slot.cmdDescVersion != descVersion` guard (`:6648`). With the dirty-list driving iteration, these are no longer needed to *find* changes, but they make the mechanism **safe against over-pushing**: a duplicate id, or an id pushed for a desc-only change whose queue is unchanged, re-reads live, finds the stored version equal, and skips the copy. This is what lets the extra tail chokes (§4.3–§4.5) share one `MarkCmdDirty` without worrying about which sub-field changed.

Note the asymmetry, stated plainly: the version-compare backstop protects against **over**-pushing (harmless no-op), **not** under-pushing. Under-push completeness rides bump-completeness (§5) for the versioned fields, and rides the explicit tail chokes for the unversioned fields (§4). A missing push means a slot is never revisited for that id — a staleness bug the armed diff gate catches by construction (§7).

## 4. Exact choke inventory (file:line)

Every site below calls `MarkCmdDirty(unitID)` (the §3.3 push). The versioned families ride the single bump funnel; the unversioned tail needs explicit pushes.

### 4.1 Queue-content version → rides `CCommandQueue::BumpVersion()`

Single funnel: `rts/Sim/Units/CommandAI/CommandQueue.h:56`. Every structural mutator already routes through it — `push_back` (`:167`), `push_front` (`:176`), `emplace_back` (`:63`), `emplace_front` (`:69`), `pop_back` (`:79`), `pop_front` (`:85`), `erase` (`:92`, `:98`), `clear` (`:104`), `insert` (`:186`), and the ctor's fresh-version draw (`:133`, `:151` counter). The documented in-place-edit sites that must bump manually all do, and thus all push: `rts/Sim/Units/CommandAI/MobileCAI.cpp:506`, `:1035`, `:2006`; `rts/Sim/Units/CommandAI/FactoryCAI.cpp:267`, `:332`; `rts/Sim/Units/CommandAI/BuilderCAI.cpp:1157`; `rts/Sim/Units/CommandAI/CommandAI.cpp:1842`; `rts/Sim/Units/UnitTypes/Builder.cpp:474`; `rts/Game/WaitCommandsAI.cpp:321`, `:399`.

**The unit-id plumbing problem.** `BumpVersion()` lives on `CCommandQueue`, which does not know its owning unit id. Fix: add a `int ownerId = -1;` member to `CCommandQueue` and set it via friend access — `CCommandAI` sets `commandQue.ownerId` in its ctor, `CFactoryCAI` sets its `newUnitCommands.ownerId` (a factory's second queue) to the same owner id. `BumpVersion()` then calls `MarkCmdDirty(ownerId)` through a header-inline global registry declared alongside the existing `static inline uint64_t nextGlobalVersion` (`:151`) so `CommandQueue.h` stays link-clean for the test executables that reach it without a dedicated `.cpp`. Because `newUnitCommands.ownerId` is the factory's unit id, factory `newUnitCommands` mutations (`:6655`) push the same id and are served by the same drain entry.

### 4.2 Command-description version → rides `CCommandAI::BumpCmdDescVersion()`

Single funnel: `rts/Sim/Units/CommandAI/CommandAI.h:192`. `CCommandAI` has `owner` (`:158`), so the push here is `MarkCmdDirty(owner->id)`. Callers (all of them): the four description-update methods `UpdateCommandDescription`×2 / `InsertCommandDescription` / `RemoveCommandDescription` at `rts/Sim/Units/CommandAI/CommandAI.cpp:392`, `:423`, `:445`, `:465`; `AddStockpileWeapon` at `:1755`; and `CFactoryCAI`'s build-count-badge update at `rts/Sim/Units/CommandAI/FactoryCAI.cpp:477`. Construction-time `possibleCommands.push_back`s in the subclass ctors need no bump (the ctor push in §4.6 already forces a from-scratch copy).

### 4.3 Worker-task inputs (UNVERSIONED — explicit chokes required)

`ResolveWorkerTask` (`:6247`) reads `curBuild/curCapture/curResurrect/curReclaim/terraforming/helpTerraform` off `CBuilder`/`CFactory`. **These transitions do not bump the command queue** — e.g. `CBuilder::SetRepairTarget` sets `curBuild = target` (`rts/Sim/Units/UnitTypes/Builder.cpp:586`) while the BUILD command stays at the queue front unmutated. Today the producer masks this by re-resolving worker-task for *every present unit every epoch*; under a queue-version-only dirty-list it would go stale (and trip the gate's `CQ_WORKER`, §7). So the worker-task transition sites push explicitly. They funnel through a small, enumerable set of `CBuilder`/`CFactory` methods, all sim-thread:

- `CBuilder::StopBuild` (`rts/Sim/Units/UnitTypes/Builder.cpp:695`; clears all cur* at `:709`–`:713`, `:720`) — the common "cleared" transition, called first by all the setters.
- `CBuilder::SetRepairTarget` (`:577`), `SetReclaimTarget` (`:606`), `SetResurrectTarget` (`:631`), `SetCaptureTarget` (`:647`), `StartRestore` (`:663`), `StartBuild` (`:729`, sets `curBuild` at `:804`/`:860`).
- `CBuilder::UpdateTerraform` terraforming toggles (`:230`, `:262`) — covered by their adjacent `StopBuild` calls, but push directly if the toggle can flip without a StopBuild.
- `CFactory` `curBuild` transitions: set at `rts/Sim/Units/UnitTypes/Factory.cpp:203`, cleared at `:76`/`:350`/`:366`.

A single `owner->id` (or `this->id` for the CFactory, itself a CUnit) push at the tail of each covers it. Alternative considered and rejected: keep a persistent "active builder/factory id set" and re-resolve only those each epoch — that reintroduces a bounded scan and a second data structure for no benefit over the push.

### 4.4 Factory bugger-off scalars (UNVERSIONED — explicit chokes)

`boPerform/boOffset/boRadius/boRelHeading/boSherical/boForced` are copied unconditionally for factory units (`:6663`–`:6672`). Writers, all sim-thread: `Spring.SetFactoryBuggerOff` (synced Lua) at `rts/Lua/LuaSyncedCtrl.cpp:4256`–`:4261`, and the `CFactory` ctor defaults at `rts/Sim/Units/UnitTypes/Factory.cpp:91`–`:92`. Push the factory unit id at those sites. Factories are few, so the fallback (unconditionally re-copy BO for a persistent factory-id set) is also acceptable if a choke there is awkward; the push is preferred for uniformity.

### 4.5 `lastSelectedCommandPage` (UNVERSIONED, MAIN-THREAD write — remove from the epoch cache)

`lastSelectedCommandPage` is copied unconditionally (`:6680`) and served to `GetServedAvailableCommands` → `CSelectedUnitsHandler::GetAvailableCommands`. Its **only** writers are `CSelectedUnitsHandler::GetAvailableCommands` (`rts/Game/SelectedUnitsHandler.cpp:1239`) and the CommandAI ctors (`rts/Sim/Units/CommandAI/CommandAI.cpp:98`, `:113`) — verified: the sim never writes it. The write at `:1239` runs on the **main/draw thread** (GUI LayoutIcons), so routing it through a sim-thread dirty push would be the one cross-thread hazard (§8).

Recommendation: **stop routing the page through the per-epoch producer copy**; read it live off `commandAI->lastSelectedCommandPage` on the main thread inside `GetServedAvailableCommands`. This is race-free (main-thread write, main-thread read; sim never touches it), removes a field from the hot copy, and eliminates the only cross-thread candidate. The diff-gate's `CQ_LASTPAGE` compare (`rts/Rendering/Common/SnapshotDiffGate.cpp:1820`) then compares live-vs-live for this field (drop it from the slot compare, or leave it tautological). If keeping the page in the slot is preferred for minimal churn, the alternative is a tiny main-thread-owned page-dirty list merged at the barrier (where sim is parked) — heavier and only justified if a later WS needs the page snapshotted; not recommended for WS-5.

### 4.6 Unit creation and death / id reuse

- **Creation**: push at `CCommandAI`'s ctor (after `owner`/`ownerId` are set). A fresh queue already draws a unique version (`CommandQueue.h:133`) and a fresh `cmdDescVersion` (`CommandAI.cpp:100`/`:115`), so the from-scratch copy is correct; the ctor push is what puts the new id into the drain domain (all N slots) so every slot captures it.
- **Death**: push at the universal removal funnel `CUnitHandler::DeleteUnit(CUnit* delUnit)` (`rts/Sim/Units/UnitHandler.cpp:291`) — `MarkCmdDirty(delUnit->id)`. This is **required for completeness**: a unit that dies without a push would leave `slot.present == true` with a stale queue in every unrevisited slot. The drain body already re-reads `unitHandler.GetUnit(id)` and, on `nullptr`, calls `ClearCmdQueueSlot` → `present = false` → the nil shape (`:6616`–`:6620`); the death push is simply what makes each slot revisit the id so that clear runs. Because the drain always re-reads *live* state, "dead ids serve the nil shape, never stale copies" is preserved exactly.
- **Id reuse**: a reused id gets a new `CCommandAI` ctor → creation push (mask OR'd back to all-slots) → each slot recopies from live. Queue/desc versions are globally unique across lifetimes (`CommandQueue.h:46`–`:53`, `CommandAI.h:99`–`:104`), so a reused id can never alias the previous owner's cached version even if a slot had a residual pending bit. The drain's live re-read is the authority in all interleavings.

## 5. Version-bump-gap analysis

The task asked whether any mutation bumps the queue version incompletely (a missing bump = a blocker). Key finding: **WS-5 does not introduce dependence on bump completeness — it inherits it.** The current per-slot cache *already* keys queue recopy on `commandQue.GetVersion()` (`:6623`–`:6625`) and desc recopy on `GetCmdDescVersion()` (`:6647`–`:6648`). If any queue mutation failed to bump, today's cache would *already* serve stale data and the armed `SnapshotDiffGate::CheckCmdQueueRows` (`rts/Rendering/Common/SnapshotDiffGate.cpp:1791`, fields `CQ_QUEUE`/`CQ_DESCS`) would *already* be flagging it. By placing the dirty push *inside* `BumpVersion()`/`BumpCmdDescVersion()`, push-completeness ≡ bump-completeness identically — no worse than the status quo.

I spot-audited the in-place-edit sites (the class the header warns about — `front()`/`at()`/`operator[]`/iterator mutation that must bump manually, `CommandQueue.h:45`–`:53`). Every in-place *write* site I found is paired with a co-located manual `BumpVersion()` (`MobileCAI.cpp:1034`→`:1035`; `FactoryCAI.cpp:255/262`→`:267`, iterator overwrite→`:332`; `CommandAI.cpp:1836`→`:1842`; `Builder.cpp:450`→`:474`; `WaitCommandsAI.cpp:320`→`:321`, `:398`→`:399`; `MobileCAI.cpp` attack-front edits→`:506`/`:2006`). The many other `Command& c = commandQue.front();` grabs (e.g. `MobileCAI.cpp:370`, `BuilderCAI.cpp:513`, `FactoryCAI.cpp:388`, `CommandAI.cpp:1584`) are read/dispatch handles feeding `Execute*` — not mutations of the queued element. **No missing-bump blocker found.** Should one ever surface, it is a pre-existing bug in the current cache, not a WS-5 regression, and the same gate names it.

The genuine gap WS-5 must handle is *not* a bump gap: it is the three **unversioned** fields the current producer copies unconditionally (worker-task §4.3, factory-BO §4.4, page §4.5), which have no bump to ride. Those are addressed by the explicit chokes / read-live above.

## 6. Duplicate-push suppression

A queue mutated 50× in one frame (bulk order edits, `FactoryCAI` STOP storms) must not push 50 active-list entries. The per-id pending-slot mask is the dedup: `MarkCmdDirty` appends to `cmdDirtyActive` **only when `cmdDirtyPending[id]` was 0** (fully drained); otherwise it just re-OR's the all-slots mask (a no-op on an already-set mask). So the id occupies at most one active-vector slot from its first dirty until all N ring slots have drained it — deduped both within a frame and across frames. The drain's version-compare backstop (§3.5) makes the resulting single recopy idempotent regardless of how many mutations coalesced into it. No per-mutation allocation, no set/hash lookup — a byte read + conditional push.

## 7. Verification plan

- **Existing diff-gate family, unchanged.** `SnapshotDiffGate::CheckCmdQueueRows` (`rts/Rendering/Common/SnapshotDiffGate.cpp:1791`, driven from `:884`) already scans **all** ids and bit-compares each cached slot against live via `LuaSnapshotServe::CompareCmdQueueSlot` (`:6968`), reporting `CQ_QUEUE`/`CQ_DESCS`/`CQ_WORKER`/`CQ_FACTORY`/`CQ_LASTPAGE` (`:1811`–`:1820`). This is exactly the oracle §6/insight §2.4 asks for: any missed dirty push (a slot never revisited for an id) surfaces as a *named field* mismatch on that id, not silent staleness. The gate itself stays an O(maxUnits) armed-only compare — that is fine; it is a test path, not the hot producer.
- **Armed dual-run**: run the established late-game replay with the diff gate armed (flag-off dual-run, sim==draw single-threaded — chokes and producer on one thread) and confirm zero `CQ_*` mismatches across the run, including boundaries with heavy order churn, factory build storms, and mass unit death (the id-reuse path). The worker-task and page changes are the two to watch (they exercise §4.3/§4.5, the parts that don't ride a queue bump).
- **Demo-resim + headless replay gate**: served values are unsynced, so this is a serving-correctness change only; resim confirms no sim perturbation. Per program rule, no A/B pixel gate.
- **Threading**: TSan not required (single-writer/single-reader, same thread — §8), but state the argument in the PR.

## 8. Threading argument

All dirty pushes run in **sim context on the sim thread**: the queue/desc bumps are sim-only (`CommandQueue.h:52`, `CommandAI.h:190`), the worker-task and factory-BO chokes are sim update / synced-Lua, and the `WaitCommandsAI` boundary path routes its one sim write through `LuaSplitContract::QueueEngineBoundaryApply` so the `BumpVersion` lands on the sim thread at its next produce edge (`rts/Game/WaitCommandsAI.cpp:378`–`:401`). The producer drains at the sim frame edge / lockstep barrier on the same sim-state-owning thread (`rts/Game/Game.cpp:2181`–`:2186`). Single writer (chokes), single reader (drain), same thread ⇒ the dirty structures (`std::vector<uint8_t>` mask + `std::vector<int>` active list) need **no atomics and no locks**. The one field written off-thread — `lastSelectedCommandPage` (main thread, §4.5) — is deliberately removed from the dirty path and read live, so no cross-thread access remains. `ClearCaches()` (`:8620`) already resets the per-slot caches and epoch stamps on game teardown; extend it to clear the dirty mask/active-list too (generation-safety, same rationale as the existing cache clear at `:8632`).

## 9. Creg / checkpoint-load behavior

The dirty structures are runtime-only (like the version counters, which are `CR_IGNORED` — `CommandQueue.h:146`, `CommandAI.cpp:83`). After a checkpoint load, every command queue is reconstructed with a fresh globally-unique version and every `cmdDescVersion` is ctor-fresh, but **no dirty pushes fire for the loaded units** unless we force them. The current full-scan producer captures them implicitly on the next produce (stored slot version 0 ≠ fresh). The dirty-list must reproduce "all queues captured after load": on load completion, enumerate `unitHandler`'s active units and `MarkCmdDirty(id)` each (all-slots mask). This is a bounded one-time O(active units) sweep, and it is robust whether or not creg deserialization runs the normal `CCommandAI` ctor path. Wire it at the same post-load hook that other serving caches use, alongside the `ClearCaches`/epoch-reset on a *new* game.

## 10. Staging — 1 small PR

Single PR:

1. Dirty structures + `MarkCmdDirty` + `ClearCaches` extension in `rts/Lua/LuaSnapshotServe.cpp` (file-static, next to `cmdQueueCaches`).
2. `CCommandQueue::ownerId` + push inside `BumpVersion()` (header-inline registry hook in `CommandQueue.h`); push inside `BumpCmdDescVersion()` (`CommandAI.h`).
3. Explicit tail chokes: worker-task setters (`Builder.cpp`/`Factory.cpp`), factory-BO writers (`LuaSyncedCtrl.cpp`/`Factory.cpp`), creation (`CCommandAI` ctor), death (`UnitHandler::DeleteUnit`), post-load sweep.
4. `lastSelectedCommandPage` → read live in `GetServedAvailableCommands`; drop/tautologize `CQ_LASTPAGE`.
5. `RefreshCommandQueues` body: replace the `for id in [0,maxUnits)` scan with the per-slot drain of `cmdDirtyActive`; keep the per-unit copy body and the version-compare backstop verbatim.

No sim behavior changes; served values stay bit-identical (the diff gate is the contract).

## 11. Expected numbers

The drain cost is O(ids dirtied in the last `EPOCH_RING_SLOTS` epochs) × the (unchanged) per-unit copy, replacing the O(maxUnits) `GetUnit` stride and the per-present-unit `ResolveWorkerTask` double-`dynamic_cast` + page-copy floor. On a quiet epoch the active set is near-empty and the producer does ~no work (the early `gen == cmdQueueCacheEpochs[ringSlot]` skip at `:6600` still short-circuits unchanged republishes). On a busy late-game epoch the active set is the order-issue + builder-transition rate over ~3 frames — small.

Target (parent §1): **923 µs → ~0.1–0.3 ms**. The residual is the genuine per-boundary copy volume (already telemetered via `BoundaryStats::cmdBlocksCopied`/`cmdBlockBytes`, `:6202`) plus the bounded drain bookkeeping; report the updated `Sim::EpochProduce::CmdQueues` zone against the parent baseline table.
