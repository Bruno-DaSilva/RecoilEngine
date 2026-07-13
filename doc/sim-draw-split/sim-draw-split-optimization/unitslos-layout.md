# WS-7 — LOS block layout + jammer reuse (`UnitsLos`: 691 µs → ~0.4 ms)

Child doc of `doc/sim-draw-split-optimization/00-program-overview.md` (§4 WS-7). Binding context: overview §2 (core insights), §3 (strategy taxonomy — this is a plain locality/compute quick win, not a WT/DS/DG mechanism), §6 (verification protocol). Paths are the main checkout; `SimSnapshot` lives at `rts/Rendering/Common/SimSnapshot.{h,cpp}`.

This is a purely mechanical change: the served VALUES are bit-identical before and after, only the storage index arithmetic (and one redundant recompute) changes. Because it is mechanical, completeness of the indexing-site inventory is the entire deliverable — a single missed site is a silent wrong-index read of neighbouring units' LOS bytes. §3 below is that exhaustive inventory.

## 1. Problem

The `Update::SimSnapshot::UnitsLos` pass (`SimSnapshot.cpp:1253-1270`) fills SIX per-allyteam byte arrays, one byte per `(allyTeam, unitID)` pair, all laid out **row-major by allyteam**: `[at * maxUnits + unitID]` (allocated `numAllyTeams * maxUnits`, `Resize` at `SimSnapshot.cpp:917-919, 974-976`). The six arrays (every one confirmed to share this stride and sizing):

1. `losStatusAll` — `u->losStatus[at]` (the raw LOS bitmask; masking input)
2. `posErrorBits` — `u->GetPosErrorBit(at)` (masking input)
3. `inRadarAll` — `losHandler->InRadar(u, at)` (computed answer)
4. `unitInLosAll` — `losHandler->InLos(u, at)` (computed answer)
5. `unitInAirLosAll` — `losHandler->InAirLos(u, at)` (computed answer)
6. `unitInJammerAll` — `losHandler->InJammer(u, at)` (computed answer)

The producer loop is unit-outer, allyteam-inner:

```
for (u : activeUnits) {
    id = u->id;
    for (at = 0; at < numAllyTeams; ++at) {
        losStatusAll  [at * maxUnits + id] = ...;   // each ++at jumps maxUnits bytes
        posErrorBits  [at * maxUnits + id] = ...;
        inRadarAll    [at * maxUnits + id] = ...;
        unitInLosAll  [at * maxUnits + id] = ...;
        unitInAirLosAll[at * maxUnits + id] = ...;
        unitInJammerAll[at * maxUnits + id] = ...;
    }
}
```

Two costs:

**(a) Write scatter.** For one unit, the `numAllyTeams` writes into a given array land at `id, maxUnits+id, 2·maxUnits+id, …` — one cache line apart per allyteam, striding the *entire* extent of the array. `maxUnits` is `unitHandler.MaxUnits()` (tens of thousands of slots), so consecutive-allyteam bytes for the same unit are ~`maxUnits` bytes (hundreds of KB) apart; across the six arrays a single unit dirties up to `6 × numAllyTeams` distinct, mutually-cold cache lines spanning megabytes of the six allocations. Every one is a cold write-allocate. This is the §2.1 "gather-bound, pointer-chasing cold memory" signature applied to the store side.

**(b) Redundant jammer recompute.** `unitInJammerAll` stores `InJammer(u, at)`; `inRadarAll` stores `InRadar(u, at)`, and `CLosHandler::InRadar(const CUnit*, int)` internally calls `InJammer(unit, allyTeam)` with the identical arguments (verified below). So per `(unit, allyTeam)` we compute `InJammer(u, at)` up to twice — once explicitly, once inside `InRadar` — even though we store the answer anyway.

### 1.1 InRadar/InJammer verification

`CLosHandler::InRadar(const CUnit* unit, int allyTeam)` (`rts/Sim/Misc/LosHandler.cpp:955-975`):

```
if (unit->IsInWater()) {
    if ((!unit->sonarStealth || unit->beingBuilt) &&
        sonar.InSight(unit->pos, allyTeam) &&
        !InJammer(unit, allyTeam))          // <-- InJammer call #1 (surface-in-water branch)
        return true;
}
if (unit->IsUnderWater())  return false;    // returns WITHOUT calling InJammer
if (unit->stealth && !unit->beingBuilt) return false; // ditto
return (radar.InSight(unit->pos, allyTeam) && !InJammer(unit, allyTeam)); // <-- InJammer call #2
```

Confirmed: `InRadar(unit, at)` recomputes `InJammer(unit, at)` (`LosHandler.cpp:962` and `:974`), with the **exact same** `(unit, allyTeam)` arguments as the separate `unitInJammerAll` store. Both internal call sites pass identical args, so a single precomputed `InJammer(u, at)` value is valid for either branch. `InJammer(const CUnit*, int)` (`LosHandler.cpp:990-1004`) is a pure read (allyteam short-circuit + one `jammer`/`sonarJammer` map `InSight`), no mutation, so computing it once and reusing changes nothing observable. Note InRadar can also *return early* (underwater / radar-stealth) without ever calling InJammer — in those cases there was no redundant call to save, but we already had to compute `unitInJammerAll` regardless, so reuse is never a net loss.

## 2. Mechanism

Two independent, additive changes, both value-preserving:

### 2.1 Interleave the layout to `[id * numAllyTeams + at]`

Change all six arrays from allyteam-major `[at * maxUnits + id]` to **unit-major** `[id * numAllyTeams + at]`. Allocation is unchanged (`numAllyTeams * maxUnits` total either way, max index `(maxUnits-1)·numAllyTeams + (numAllyTeams-1) = maxUnits·numAllyTeams - 1`); only the index arithmetic changes.

After interleaving, a unit's `numAllyTeams` bytes in each array are **contiguous** (`numAllyTeams` bytes, one to two cache lines), so the producer's inner allyteam loop writes sequentially instead of striding. The consumer side is unaffected on the hot path: every serving consumer does single-`(id, at)` random access and pays one cache line either way (§3 shows the read sites are single-id). The two full-sweep consumers (hash, diff-gate) already iterate **allyteam-innermost per unit**, so interleaving makes them *more* cache-friendly, not less — no loop-nest restructuring is needed anywhere, only the index formula.

The `maxUnits` operand for reads is `MaxUnits()` (`= valid.size()`); the `numAllyTeams` operand is the public member `rows.numAllyTeams`, already in scope at every read site (the accessors already read it for bounds checks). So the substitution is uniform: replace `at * MaxUnits() + id` with `id * numAllyTeams + at` (and `at * maxUnits + i` with `i * numAllyTeams + at` in the extraction/gate loops where `maxUnits`/`numAllyTeams` are locals).

### 2.2 Compute jammer once, reuse in the radar check

In the producer loop compute `InJammer(u, at)` first, store it into `unitInJammerAll`, and feed it into the radar computation instead of letting `InRadar` recompute it:

```
const bool inJammer = losHandler->InJammer(u, at);
urows.unitInJammerAll[id * numAllyTeams + at] = inJammer;
urows.inRadarAll     [id * numAllyTeams + at] = losHandler->InRadar(u, at, inJammer);
```

Recommended shape: add a 3-arg overload `bool CLosHandler::InRadar(const CUnit* unit, int allyTeam, bool inJammer) const` in `LosHandler.{h,cpp}` that is the current body with both `InJammer(unit, allyTeam)` calls replaced by the `inJammer` parameter; the existing 2-arg `InRadar(unit, allyTeam)` becomes `return InRadar(unit, allyTeam, InJammer(unit, allyTeam));`. This keeps the branch logic in one place (single source of truth for synced callers) and is a pure factoring — the 2-arg form, which every synced sim caller uses, is behaviourally identical. The diff-gate live oracle (§4) keeps calling the 2-arg `InRadar(u, at)`, so the stored value is still compared against the canonical formula. (Alternative — inlining InRadar's body at the extraction site — is rejected: it duplicates synced LOS branch logic into the draw-side extractor and would drift.)

This must land in both extraction sites (§3, writes W1 and W2). It is optional-but-recommended relative to the layout change; the layout change alone yields most of the win.

## 3. EXHAUSTIVE indexing-site inventory

Every code site that indexes one of the six arrays with the stride formula, `grep`-verified across the whole tree (`grep -rn` for each array name; only `.size()`/`.resize()`/comments were excluded, and those are listed in §3.4 so an implementer knows to leave them alone). No consumer exists outside `rts/`. `LuaSyncedRead.cpp:9691`, `LuaSnapshotServe.h:154`, `LuaSnapshotServe.cpp:1689`, and `LuaSplitContract.cpp:127` mention these arrays in **comments only** — `Spring.IsUnitInRadar` routes through the `InRadar()` accessor (R2 below), not a direct index.

### 3.1 Writes (12 sites, all in `SimSnapshot.cpp`)

W1 — the live `UnitsLos` pass, `SimSnapshot.cpp:1260-1267` (locals `maxUnits`, `numAllyTeams`, `id`):
- `:1260` `losStatusAll[at * maxUnits + id]`
- `:1261` `posErrorBits[at * maxUnits + id]`
- `:1262` `inRadarAll[at * maxUnits + id]`
- `:1265` `unitInLosAll[at * maxUnits + id]`
- `:1266` `unitInAirLosAll[at * maxUnits + id]`
- `:1267` `unitInJammerAll[at * maxUnits + id]`

W2 — `ExtractDeadRowsFromShells` DEAD_THIS_BATCH unit rows, `SimSnapshot.cpp:1964-1969` (locals `maxUnits`, `numAllyTeams`, `id`; writes off the deferred-deletion shell):
- `:1964` `losStatusAll[at * maxUnits + id]`
- `:1965` `posErrorBits[at * maxUnits + id]`
- `:1966` `inRadarAll[at * maxUnits + id]`
- `:1967` `unitInLosAll[at * maxUnits + id]`
- `:1968` `unitInAirLosAll[at * maxUnits + id]`
- `:1969` `unitInJammerAll[at * maxUnits + id]`

Both W1 and W2 also get the §2.2 jammer-reuse edit. Both are unit-outer/allyteam-inner loops, so both become sequential writes after interleaving.

### 3.2 Reads — accessors, `SimSnapshot.h` (5 sites; formula `argAllyTeam * MaxUnits() + unitID`)

- R1 `:493` `LosStatus(unitID, argAllyTeam)` → `losStatusAll`
- R2 `:497` `InRadar(unitID, argAllyTeam)` → `inRadarAll`
- R3 `:574` `UnitInLos(unitID, argAllyTeam)` → `unitInLosAll`
- R4 `:578` `UnitInAirLos(unitID, argAllyTeam)` → `unitInAirLosAll`
- R5 `:582` `UnitInJammer(unitID, argAllyTeam)` → `unitInJammerAll`

### 3.3 Reads — serving-layer helpers, `SimSnapshot.cpp` (5 sites; formula `readAllyTeam/argAllyTeam * MaxUnits() + unitID`)

- R6 `:215` `PovUnitVisible` → `losStatusAll`
- R7 `:232` `PovUnitInLos` → `losStatusAll`
- R8 `:250` `PovUnitTyped` → `losStatusAll`
- R9 `:263` `ErrorVector` → `posErrorBits` (`argAllyTeam * MaxUnits() + unitID`)
- R10 `:271` `ErrorVector` → `losStatusAll`

(R6–R8, R10 each read the row conditionally — the `SimSnapshotLosEvent::Active` ternary at `:213-215`, `:230-232`, `:248-250`, `:269-271` selects the event override OR the row byte; only the row-byte branch indexes the array and must change. The event-override branch reads a scalar, untouched.)

### 3.4 Reads — Lua serving, `LuaSnapshotServe.cpp` (2 sites)

- R11 `:831` `losStatusAll[allyTeamID * rows.MaxUnits() + unitID]` (the LOS-state serving twin)
- R12 `:9630` `posErrorBits[argAllyTeam * rows.MaxUnits() + unitID]` (`GetUnitPosErrorParams`)

### 3.5 Reads — verification passes (armed-only; full `unit × allyTeam` sweeps)

`SnapshotHash.cpp` — `HashUnitRow`, per-unit function looping over `at` (formula `at * r.MaxUnits() + id`):
- R13 `:84` `losStatusAll`
- R14 `:85` `posErrorBits`
- R15 `:87` `inRadarAll`
- R16 `:253` `unitInLosAll`
- R17 `:254` `unitInAirLosAll`
- R18 `:255` `unitInJammerAll`

`SnapshotDiffGate.cpp` — `CheckBoundary`, per-unit outer (`i`) / per-allyteam inner (`at`) loop (formula `at * maxUnits + i`). Note the three "with a log" sites write the index expression **twice** (compare + `LOG_L` argument), so they are ~9 raw expressions across these 6 logical sites:
- R19 `:844` + `:846` `losStatusAll`
- R20 `:848` + `:850` `posErrorBits`
- R21 `:852` + `:854` `inRadarAll`
- R22 `:858` `unitInLosAll`
- R23 `:859` `unitInAirLosAll`
- R24 `:860` `unitInJammerAll`

**Total: 36 distinct read/write sites across 5 files** (12 writes + 24 reads; ~42 raw index-expression occurrences counting the diff-gate compare/log duplication). The `hashScratch`/`hashProjScratch`/… scratch buffers reuse the same `Extract`/`HashUnitRow` code paths (`SimSnapshot.cpp:734`, and `hashScratch` is itself a `UnitRows`, `SimSnapshot.h:1496`), so they inherit the new layout automatically — no separate site.

### 3.6 Bookkeeping that must NOT change (leave as-is)

These reference the arrays via `.size()`/`.resize()` and depend only on the total element count, which is invariant under interleaving:
- `Resize` sizing: `SimSnapshot.cpp:917-919` (`losStatusAll`/`posErrorBits`/`inRadarAll`) and `:974-976` (`unitIn*All`) — `size_t(numAllyTeams) * maxUnits`, unchanged.
- `LogEpochStats` byte accounting: `SimSnapshot.cpp:660-661`.
- `Clear` byte accounting: `SimSnapshot.cpp:757, 792`.

## 4. Allocation / sizing analysis

- **Allocation is unchanged.** Both layouts occupy `numAllyTeams * maxUnits` bytes per array per slot; interleaving only reinterprets which dimension is the stride. `Resize` (`:917-919, :974-976`) needs no edit. No growth, no reallocation, no bounds change (max linear index identical, §2.1).
- **Per-game allyteam count.** `numAllyTeams` is fixed for a game and captured into `rows.numAllyTeams` at `Resize` (`SimSnapshot.cpp:846`). Under interleaving it becomes the array *stride* (was `maxUnits`); it is a public member available at every read site, so the multiply is always in scope. A game with more allyteams gets a larger stride but the same total allocation, exactly as today. Checkpoint-load / replay-rewind re-`Resize` with whatever `numAllyTeams` the loaded game has, identical to current behaviour.
- **`maxUnits` is the allocation dimension in the new layout** (`id` ranges `[0, maxUnits)`, `at` the stride `[0, numAllyTeams)`), the reverse of today. Both are known at `Resize` and at every write, so nothing dynamic is introduced.

## 5. Ring / slot analysis

- **Per-ring-slot, single-buffer per slot.** Each of the `EPOCH_RING_SLOTS = 3` (`SimSnapshot.h:1439`) slots holds its own `UnitRows` (`buffers[EPOCH_RING_SLOTS]`, `SimSnapshot.h:1476`), each owning independent copies of all six arrays. Extraction fills a free slot in place (`PickFreeSlot` → `Extract(buffers[target])`); there is **no slot-to-slot copy** of the LOS arrays. The PR-43 LOCKSTEP-EXCEPTION in-place refresh (`SimSnapshot.cpp:347-350`) only re-extracts team/player/global channels — it never touches the unit LOS arrays. So the layout change is confined to per-slot in-place writes (§3.1) and single-slot reads (§3.2-3.5); there is no cross-slot `memcpy` whose element order could be disturbed. All three slots (and the hash scratch) are written by the same `Extract` code, so they stay mutually consistent by construction.
- No interaction with acquire/release/retire: those rotate slot *indices* and refcounts, never the byte contents of the LOS arrays.

## 6. Byte-identity argument

The served values are unchanged; only their storage index and one redundant recompute differ.

- **Layout (§2.1):** for every `(id, at)`, the byte stored/read is the same value; `[at·maxUnits+id]` and `[id·numAllyTeams+at]` are two addressings of the same logical `(id, at)` cell. As long as **every** site in §3 flips together (the reason the inventory must be exhaustive), reads observe exactly what writes stored. No value is computed differently.
- **Jammer reuse (§2.2):** `InJammer(u, at)` is a pure read; computing it once and passing it into `InRadar(u, at, inJammer)` yields the identical bool `InRadar(u, at)` would return (§1.1 — both internal call sites use identical args). `unitInJammerAll` stores the same bool as before. So both `inRadarAll` and `unitInJammerAll` are bit-identical.
- **Hash:** `HashUnitRow` (R13-R18) XOR-folds the six arrays in a fixed sequence over `at`. Keeping the same loop with the new index formula preserves the fold order and every folded byte, so `SnapshotHash` output is unchanged frame-for-frame — the demo-stream sync hash and any hash-compare gate see no difference.
- **Diff-gate:** the four LOS field passes (`F_LOSSTATUS`, `F_POSERRORBIT`, `F_INRADAR`, `D_LOSVARIANTS`; R19-R24) compare the stored byte against a fresh live recompute (`u->losStatus[at]`, `u->GetPosErrorBit(at)`, `losHandler->InRadar/InLos/InAirLos/InJammer(u, at)`). Since stored values are unchanged, these passes must remain **0-mismatch** — that is the pass criterion for this WS.

## 7. Verification plan

Per overview §6 (bit-identity is the contract; no A/B pixel gate for decoupling work):

1. **Armed diff-gate, LOS field passes = 0 mismatches.** Run the armed `SnapshotDiffGate` over the established late-game replay and confirm `F_LOSSTATUS`, `F_POSERRORBIT`, `F_INRADAR`, and `D_LOSVARIANTS` report zero mismatches across the run (these are R19-R24, recomputing the live oracle every boundary). This is the primary gate — a missed §3 site or a broken index flip surfaces here as a named field failure, not a silent stale read.
2. **Hash identity.** With `SnapshotHash` armed, confirm the per-frame unit-row hash is unchanged versus a pre-change baseline on the same replay (guards the fold-order preservation, §6).
3. **Flag-OFF resim / headless replay gate.** These arrays are draw-side reads and do not enter the synced sim, so sync is not at risk; still run the DEBUG headless replay gate (flag-OFF and flag-ON) to confirm no assert/poison regression and identical demo-stream sync, per the standing gate discipline.
4. **Benchmark the `Update::SimSnapshot::UnitsLos` Tracy/`/debug` sub-zone** on the late-game replay before/after; report into the overview §1 table.

Threading note (overview §6): no new cross-thread access. The six arrays are producer-owned; writes happen only in the single-threaded producer (`Extract` / `ExtractDeadRowsFromShells`), reads only from consumers holding a slot. The new `InRadar(unit, at, inJammer)` overload is a `const` read of the same `losHandler` state the existing 2-arg form already reads from the extractor. No atomics, no `for_mt` (that is WS-8).

## 8. Staging

**One small PR.** Scope:
- Flip the index formula at all 36 sites in §3 (`SimSnapshot.{h,cpp}`, `LuaSnapshotServe.cpp`, `SnapshotHash.cpp`, `SnapshotDiffGate.cpp`).
- Add the `CLosHandler::InRadar(const CUnit*, int, bool)` overload (`LosHandler.{h,cpp}`) and apply the jammer-reuse in W1 + W2.
- No `Resize` change, no allocation change, no consumer signature change.

The jammer-reuse could be split into a second trivial PR if a reviewer prefers isolating the sim-side `LosHandler` overload from the draw-side layout change, but both are small and independently gate-clean; landing them together keeps the `UnitsLos`-zone benchmark delta attributable to one commit.

## 9. Expected numbers

Baseline `Update::SimSnapshot::UnitsLos` = **691 µs** (overview §1). The layout change removes the per-unit write scatter (from up to `6 × numAllyTeams` cold lines/unit to ~6 sequential runs/unit); the jammer reuse removes up to one `jammer`/`sonarJammer` map `InSight` lookup per `(unit, allyTeam)`. Combined target **~0.4 ms (~300-400 µs)**, roughly a 40-45% reduction, consistent with the overview's `→ ~0.4 ms`. This is a locality/compute win with a hard bit-identity guarantee — no fidelity or serving-latency cost, orthogonal to every other workstream.
