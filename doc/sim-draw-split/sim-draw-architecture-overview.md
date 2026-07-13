# Recoil sim/draw thread decoupling — architecture overview

This document describes the final architecture of the sim/draw thread split. It assumes familiarity with the classic Recoil/Spring engine architecture (the `CGame::Update`/`Draw` loop, `SimFrame`, synced vs unsynced code, the Lua handles) but nothing about the decoupling project itself. It describes the end state, not the migration path.

## 1. Background: the classic loop

In the original engine, one main thread does everything. Each iteration of the game loop interleaves:

```
main thread:  [net → SimFrame × k] [UpdateUnsynced] [Lua Draw* callins] [render] [swap]
                    "sim"                              "draw"
```

Sim frames are driven by NEWFRAME messages from the (local or remote) game server at 30 frames per game-second. Everything downstream of the sim — drawers, Lua widgets/gadget-unsynced, info textures, the GUI — reads simulation state (`unitHandler`, `losHandler`, `readMap`, per-unit fields) directly, because nothing else can be running at the same time.

That direct-read assumption is the whole problem: sim and draw contend for one thread (a heavy sim frame eats the frame budget and drops draw frames, a heavy scene throttles the sim), and the sim/draw balance machinery (CPU-usage speed throttling, net-consumption budgets) exists only to arbitrate that contention.

## 2. Goals and constraints

The split runs simulation and rendering on separate threads with near-zero synchronization, under three hard constraints:

1. **Sync compatibility.** The synced simulation is untouched. A split client stays in a multiplayer game with unsplit clients, and with the flag off the binary behaves byte-identically to an engine without the feature.
2. **Full Lua API.** Games are built on hundreds of `Spring.*` callouts that read sim state from draw contexts. Every one keeps working, with bit-identical values. No callout was removed or degraded to nil.
3. **No torn reads.** The draw side never observes sim state mid-mutation. There are no per-field locks, no "read mostly-consistent state and hope" — every observable value comes from a consistent point-in-time snapshot.

## 3. The big picture: publish, don't share

The design is a single-producer / single-consumer pipeline. The sim thread **publishes** an immutable snapshot of all observable state — an **epoch** — at its own frame edges. The draw thread consumes the newest complete epoch and works exclusively from it. The threads meet only at a small lock-free ring of epoch slots.

```
  SIM THREAD (producer)                          DRAW THREAD (consumer)
  ─────────────────────                          ──────────────────────
  net → SimFrame × k                             acquire newest epoch  ──┐
        │                                        dispatch its sealed     │ one
        ▼  at a frame edge:                      event batches           │ draw
  extract + seal + publish ──► ┌────────────┐    Lua Draw* callins       │ frame
        │                      │ epoch ring │ ─► (served from epoch)     │
        ▼                      │  slot 0    │    render, swap            │
  next frames…                 │  slot 1    │    release prev epoch   ◄──┘
                               │  slot 2    │         │
                               └────────────┘         ▼
                                    ▲          refcount 0 → slot retires,
                                    └───────── producer may recycle it
```

Key properties:

- A published epoch is **immutable**. The producer only writes into a free (retired) slot; the consumer only reads the slot it holds a reference on.
- The ring is **N = 3** slots, refcount-retired: consumer acquire = ref++, release of the previously held epoch = ref--. A slot recycles only at refcount zero. Acquire/release are a couple of atomics — the draw thread never blocks on the sim thread in gameplay, and vice versa.
- The draw thread renders the world *as of* one consistent sim frame. It may be up to one sim frame behind the live sim — exactly the staleness the classic engine's frame interpolation already tolerates.

## 4. What an epoch contains

An epoch is "everything one draw frame is allowed to observe", as one versioned unit: `{u64 epochId, frame span, channels}`. The channels:

| Channel | Contents | Refresh policy |
|---|---|---|
| Object rows | Flat per-id arrays for units / features / projectiles / teams / players / globals: position, health, LOS status per allyteam, build state, weapon scalars, move-type state, rules-params, … | Full extraction per epoch; expensive sub-payloads (rules-params maps) are version-skipped |
| Row validity | Tri-state per id: `INACTIVE / ACTIVE / DEAD_THIS_BATCH` | Per epoch |
| Sealed render events | Create / destroy / LOS-change / ghost records fired during the epoch's frames, in order | Sealed at publish |
| Sealed deferred dispatches | Unsynced Lua event callins (UnitCreated, UnitDestroyed, …), `SendToUnsynced`, Cob2Lua — as closures, in total fire order | Sealed at publish |
| Map mirrors | LOS maps (7 types × allyteams), blocking map, typemap, metal + extraction maps, smooth mesh, original heightmap, heightmap dirty-rects | Dirty-versioned per layer; blocking map is dirty-rect incremental |
| Command-queue cache | Per-unit command queues + command descriptions | Content-versioned per unit |
| Piece cache | Per-piece matrices/positions, script→piece maps, collision volumes | Read-set-driven: only objects the draw side has actually queried |
| SYNCED mirror | The registered read-set of scalar `SYNCED.*` globals | Per epoch |
| Query replies | Weapon trace tests, build-placement tests, default-command results | Evaluated at the same edge as extraction |
| GPU staging | Model piece transforms for the SSBO upload; effect-container copies | Per epoch |

## 5. The sim thread

The sim thread owns the entire synced simulation plus epoch production:

```
loop:
    clear watchdog; park if the draw side requested quiescence (rare, lifecycle only)
    ProduceEpochAtSimEdge():        ← only if the previous epoch was consumed
        apply queued draw-side writes (see §7)
        extract all row namespaces (+ at-death rows from deletion shells)
        refresh caches (version/dirty/read-set gated)
        evaluate pending queries against the same state
        seal event batches into the slot; publish
    ClientReadNet():                ← consumes net messages, issues SimFrame()s
        runs until the consumer is ready for a fresh epoch (or a hygiene backstop)
    if packets remain and progress was made: continue, else nap ~1ms
```

Two points matter. First, **production happens at frame edges only** — never mid-`SimFrame` — so an epoch is always a consistent end-of-frame state. Second, the pacing gate ("at most one unconsumed epoch") means extraction cost is paid at **min(sim rate, draw rate)**: during fast-forward, one epoch covers many sim frames and the draw side consumes batches.

The classic engine's sim pacing (net-consumption budgets, the server's CPU-usage speed throttle targeting 60–75% load) existed to share one thread between sim and draw. Under the split those are bypassed for local play: the sim runs at whatever the wanted game speed and its own capacity allow, and the leftover core time is genuinely free.

## 6. The draw thread

The draw thread is the original main thread (it keeps the GL context, input, audio):

```
CGame::Draw():
    acquire newest epoch; retire previous       ← the only producer/consumer touch point
    dispatch the epoch's sealed batches:
        render-event records → drawer containers (create/destroy/LOS/ghosts)
        deferred closures    → widget/gadget event callins, in fire order
    UpdateUnsynced: camera, GUI, world-drawer extraction (reads epoch + GPU staging)
    Lua DrawGenesis/DrawWorld/DrawScreen …      ← every Spring.* read served from the epoch
    render, swap
```

Every event callin a game sees (e.g. `widget:UnitDestroyed`) runs on the draw thread, against the epoch that contains that event — so the callin's own reads (`Spring.GetUnitDefID` on the dying unit, etc.) resolve consistently.

## 7. Serving the Lua API

The draw side never dereferences live sim objects. Three mechanisms cover the full API surface:

**Serving twins (reads).** Each `Spring.Get*` callout reachable from draw context has a twin implementation: same argument parsing, same POV/LOS masking rules, but reading the held epoch's rows/mirrors/caches instead of live handlers. Values are bit-identical to what the live body would have returned at the epoch's frame edge. A tripwire (`DenyLiveRead`, plus a strict mode used in testing) turns any unserved live read into a hard error instead of a data race.

**Query/reply channels (reads that must run sim-side).** A few predicates are infeasible to mirror (weapon trace tests against moving targets, build-square tests, the deep default-command resolution under the cursor). These file a query from the draw side; the producer evaluates it at the next frame edge — against exactly the state it is extracting — and the reply rides the next epoch. Cost: one epoch of latency on advisory UI predicates.

**Boundary-queued writes.** Draw-side mutations of sim state (`Spring.SetUnitNoDraw`, team color changes, `GiveOrder`-class actions…) queue and are applied by the producer at its next frame edge, before extraction — last-writer-wins, visible one epoch later.

```
draw thread                      sim thread
───────────                      ──────────
Spring.GetUnitHealth ──────────► (nothing — served from held epoch locally)
Spring.TestBuildOrder ─ query ─► evaluated at next frame edge ─ reply rides epoch N+1
Spring.SetUnitNoDraw ── queue ─► applied at next frame edge, extracted into epoch N+1
```

## 8. Object lifetime across threads

Deletion is the classic hazard: the sim destroys a unit while the draw thread is mid-frame using it. The split solves this with **deferred-deletion shells** keyed to epoch retirement:

- When a sim object dies, its memory becomes a readable "shell" instead of being freed. The destroy *event* is a record in the epoch's sealed batch.
- At the frame edge, the producer extracts the dying object's final state from its shell into the epoch as a `DEAD_THIS_BATCH` row — a genuine at-death snapshot, not stale data.
- The shell is released back to the pools only when every epoch referencing it has retired. So for the whole window in which a draw frame can still dispatch that object's destroy event (and run `widget:UnitDestroyed` handlers that query it), both its row data and its render records remain resolvable.
- An always-on coverage check asserts that every id referenced by an epoch's records resolves in that epoch as `ACTIVE` or `DEAD_THIS_BATCH`.

Id reuse is safe by the same construction: a new object with a recycled id gets `ACTIVE` rows in later epochs, while the old object's `DEAD_THIS_BATCH` validity is scoped to the dispatch of its own epoch.

## 9. Flow control

- **Pacing gate (producer):** at most one unconsumed published epoch. A free-running sim skips *extraction* while the consumer is behind — it never blocks on it.
- **Backpressure (1x play only):** the sim may run at most N−1 = 2 unretired epochs ahead of the consumer. This engages only when the draw thread is slower than the sim at normal speed; fast-forward, catch-up, demo skip and video capture are exempt and free-run.
- **Pool valve (backstop):** if object-pool pressure builds because epochs aren't retiring (a stalled consumer), the sim waits for a retirement before allocating — deadlock-free, and in practice never engages.
- **Parks:** the draw side can still request full sim quiescence, but only lifecycle events do (saves, Lua handle reload, teardown, a handful of rare input-gated paths). Measured: ~20–30 parks per game, ~0.2s total.

## 10. Correctness strategy

Three independent verification pillars, all runnable against full game replays:

1. **Flag-off byte-identity.** With the split disabled, execution is bit-for-bit the stock engine — proven by DESYNC-free re-simulation of recorded games.
2. **The armed differential gate.** A test mode dual-runs every served callout: the live body and the epoch twin execute on the same call, and returns are bit-compared; separately, every row/mirror field is compared against live state at each boundary. A wrong or stale served value is a deterministic test failure, not a subtle visual glitch.
3. **Strict-contract runs.** Full replays with the tripwire escalated: zero live-read denials proves the serving surface is complete (nothing silently fell back to reading live sim).

Where the split's behavior deliberately differs from the classic engine (values are one frame edge old; a first-touch read of a lazily-cached channel is fresher than the epoch; query replies lag one epoch), each deviation is enumerated and reviewed rather than silently accepted — all are advisory-UI-visible only.

## 11. Results

- Draw-thread stalls on the sim went from "the whole sim time, every frame" (classic) to ≈0 in gameplay (lifecycle-only parks).
- Fast-forward (replay/catch-up) with the split on is faster than with it off — the sim thread saturates one core while the draw thread renders independently — and no longer throttled by the classic sim/draw balance machinery.
- Full Spring Lua API served; zero strict-mode denials over full replays; sync compatibility preserved.

---

## Addendum A — what does copying the sim state cost?

**It's paid per draw-consumable epoch, not per sim frame.** The pacing gate means one extraction per draw frame at most; under fast-forward, dozens of sim frames share one extraction. That already amortizes the copy far below "per frame" intuition.

**CPU, measured mid-game on a 8v8 (thousands of units):** naive full extraction was genuinely expensive — the first profiled build spent ~9–13 ms per epoch (map mirrors ~6.7 ms, piece cache ~4 ms, unit rows ~4 ms, command queues ~0.4 ms), which is 3–4× the cost of the sim frames it served. That cost was then attacked channel by channel, with the rule *copy work must be proportional to change or to demand, not to world size*:

- **Blocking map:** whole-map rewalk (~1M squares) replaced by dirty-rect incremental updates from the mutation footprints — milliseconds → tens of microseconds.
- **Rules-params maps:** per-object version stamps; a ring slot recopies an object's params only if they mutated since that slot last copied them. Params mutate rarely relative to 30 Hz, so the ~4 ms unit-row cost collapses toward the flat POD row copy (~0.2–0.5 ms).
- **Piece cache:** demand-driven — only objects the draw side has actually queried are captured (stock games query piece data rarely; first touch is served via a rare counted fallback). ~4 ms → ~0 steady-state.
- **LOS mirrors:** per-(type, allyteam) whole-layer copies, version-gated; the remaining fixed cost of the drain, on the order of a millisecond mid-game, and the current top optimization candidate (dirty-rect granularity is the designed next step if it matters).

Steady-state epoch production after these passes is on the order of **1–2 ms per draw frame** on the profiled hardware, running on the sim thread concurrently with rendering — i.e. it costs sim-thread headroom, not draw latency. The remaining big-ticket items (row extraction, transforms staging) scale linearly with object count and are flat POD copies (memcpy-bound).

**Memory:** resident cost is N=3 × per-slot payload. Per-slot at large-game scale: object rows ~20–25 MB, plus the caches (command queues ~14 MB; the piece cache was ~30 MB when eagerly captured and is now demand-proportional, typically ~0), plus map mirrors (map-size dependent; the LOS layers dominate). Order of magnitude: **~100–150 MB extra** on a big game at the eager-capture worst case, substantially less after the demand-driven changes — accepted as a deliberate trade (flat copies everywhere; no refcount-shared sub-structures) to keep lifetime reasoning trivial. Nothing here touches synced memory, so it has no sync footprint.

## Addendum B — thread configuration

Not 7-sim / 1-render. The split adds **exactly one new full-time thread**. The configuration is:

- **1 × main/draw thread** — the original main thread: GL context, rendering, input, audio dispatch, all Lua draw/event callins.
- **1 × sim thread** — net consumption, all `SimFrame`s (the entire synced simulation), epoch production.
- **The existing worker pool (shared)** — the classic `for_mt` pool used *inside* sim frames (pathing, unit script ticking, LOS updates) and by some draw-side jobs. Under the split these workers are driven mostly by the sim thread. Pool size = auto (per-perf-core policy), minus reservations below.
- Plus the engine's pre-existing auxiliary threads (local game server/net thread, sound, watchdog, GL driver threads).

Pinning: under the default per-performance-core policy, the main thread and each worker get an exclusive performance core (SMT siblings and efficiency cores excluded). The split reserves one core for the sim thread by spawning one fewer worker — an unpinned sim thread would keep preempting whichever pinned worker it landed on, making every `for_mt` wait on that worker's chunk (a measured straggler class). Concretely, on an 8-performance-core CPU: 1 main + 1 sim + 6 workers, each on its own core. An alternative policy (`ThreadPinPolicy=3`) pins all threads to the *group* of performance cores and lets the OS float them within it, which trades cache affinity for resilience to background load.

So the parallelism story is: **two orchestrator threads** (sim, draw) each saturating their own core when loaded, with the data-parallel worker pool accelerating the sim's interior loops — not a many-way partition of the simulation itself, which remains sequential and deterministic by design.

## Addendum C — design concessions forced by the original engine

Very little of this design's complexity comes from threading itself. It comes from the engine never having had a formal boundary between *simulating* and *observing*: thirty years of code assumed "if I can see the pointer, I can read (or write) it right now." The epoch pipeline retrofits that boundary, and the concessions below are the places where the retrofit could not be made invisible. Each entry names the engine trait that forced it, what was conceded, and what the concession costs.

### C.1 Object lifetime is entangled with event delivery

**The trait:** Lua event semantics assume synchronous dispatch. A `widget:UnitDestroyed(unitID)` handler queries the dying unit — its def, position, team — and that worked because the classic engine ran the handler mid-frame while the object still existed. The engine's pooled allocators also zero-on-free and recycle ids aggressively, so once event *firing* is decoupled from event *dispatch*, the object is not merely stale by the time the handler runs — its memory has been reused.

**The concessions:**
- **Deferred-deletion shells.** A dead sim object's memory stays intact (excluded from the pools) until every epoch whose event batch references it has retired. This couples pool lifetime to consumer progress, which in turn forced a **pool valve**: if epochs stop retiring (a stalled consumer), the sim waits for pool headroom before allocating. Deadlock-free by construction and never observed engaging in practice — but it exists solely because shells pin pool slots.
- **At-death row extraction.** The `DEAD_THIS_BATCH` rows are extracted *from the shells* at the frame edge, not taken from the previous snapshot — early prototypes that trusted stale snapshot data for just-died objects served garbage for young units (created and destroyed between two epochs) and crashed on null defs. In effect the split maintains a second, shadow lifetime system layered over the engine's.
- **Event-time value captures.** Some callins observe *event-time* state no snapshot can provide: a `UnitGiven` handler reads the unit's team, but by dispatch time the epoch already shows the post-transfer team. The fix is a hand-enumerated list of (event, field) pairs captured at fire time, with the serving layer consulting scoped per-object overrides during exactly those callins. This is a pure concession — a curated exception list rather than a general mechanism — because the old API contract was implicitly "whatever the C++ happened not to have mutated yet."
- **Death-dependent registrations.** Selection, wait-command icons and tracked lights used the engine's intrusive death-listener lists on synced objects (`CObject` dependences) — unsynced systems holding hooks inside sim-owned objects. Cross-thread that is untenable, so they receive a bespoke boundary notification of the epoch's deaths instead of the general serving mechanism everything else uses.

### C.2 The sim has no change-tracking, anywhere

**The trait:** state mutates in place from scattered call sites — no versions, no dirty flags, no observer hooks. LOS maps are written by circle/raycast painters wherever units move; the ground-blocking map by footprint loops; rules-params are bare string-keyed maps mutated directly by Lua.

**The concessions:**
- **Retrofitted choke points, proven by audit.** Every mirrored layer needed its mutation paths funneled through explicit `Mark*Dirty` chokes, and the only completeness proof is a grep-audit of writers plus the differential gate (Addendum-independent test mode) catching what the audit missed. This is a standing tax on future engine changes: a new writer that bypasses a choke is a silent-staleness bug, detectable only by the gate.
- **Whole-layer copy granularity as the default.** With no rect/delta infrastructure to inherit, mirrors began as "version per layer, copy the whole layer when touched" — correct but brute-force. High-churn layers were then optimized individually (the blocking map now applies per-mutation footprint rects instead of re-walking the map; LOS layers are the next candidate). Copy cost proportional to *change* had to be earned per-channel, not assumed.
- **Version stamps as afterthoughts.** Skip-if-unchanged copying (e.g. per-object rules-params) required bolting version counters *beside* the data they version, with correctness resting on the audited mutation sites bumping them. An engine designed for observation would carry these natively.
- **Type-erased containers.** The blocking map stores bare `CSolidObject*` per cell with no type tag, so the mirror must classify each occupant (unit vs feature, via `dynamic_cast`) at copy time — the draw side can never dereference the pointer itself.

### C.3 Parts of the API are not "reads" at all

**The trait:** several callouts are deep evaluations through half the simulation. Build-placement tests walk yardmaps, the blocking map and move-defs; the default-command resolution runs full command-AI logic under the cursor; the `SYNCED.*` proxy lets unsynced Lua read arbitrary nested tables inside the synced Lua VM.

**The concessions:**
- **Query/reply instead of serving** for weapon traces, placement tests and default commands — the evaluation stays sim-side and the answer arrives one epoch later. Accepted because these are advisory UI predicates; enumerated as a deviation rather than hidden.
- **The SYNCED mirror covers registered scalars only**, with a **lazy park** fallback for nested-table reads: the draw thread briefly pauses the sim at a frame edge and reads live. This is a genuine hole in "the draw thread never stops the sim," tolerable because measured games take dozens of such parks, not thousands — and counted in telemetry precisely because a Lua-heavy game could regress it.
- **A short list of surviving live-read exceptions and lifecycle parks** — game saves (creg walks the entire sim), Lua handle reloads, the pathfinder debug overlay, a handful of rare input-gated GUI paths. Each is a case where full serving was judged not worth the machinery; each is a documented carve-out rather than a clean rule.

### C.4 Copy-model costs

**The trait:** the observable surface is enormous (per-piece matrices for every unit; LOS status per unit *per allyteam*) and visibility masking is evaluated at read time per caller, so a snapshot cannot pre-mask for one viewer.

**The concessions:**
- **Per-allyteam row blowup:** rows like unit LOS status are `numAllyTeams × maxUnits`, because one immutable epoch must be able to serve any handle POV (player, spectator, `/specteam`) without re-extraction.
- **N=3 flat copies of everything:** a deliberate ruling traded memory (order 100 MB at large-game scale, worst case) for trivial lifetime reasoning — no refcount-shared substructures, because the engine's pointer-heavy data makes shared-immutable schemes risky to retrofit.
- **Two capture disciplines instead of one:** most channels are captured eagerly, but the piece cache is demand-driven (only draw-queried objects are captured, first touch served via a counted park) because eager capture of a channel almost nobody reads cost ~4 ms/epoch — and the engine's piece accessors bake unit world transforms into "piece" data, defeating cheap animation-based dirty-gating. Uniformity was conceded for cost.

### C.5 Ownership tangles with the renderer

**The trait:** the classic engine freely mixes ownership across the sim/render line — draw passes iterate sim-owned effect containers, model loading touches GL from sim-side code paths, render transforms derive from synced animation state.

**The concessions:** per-epoch staged copies of the effect containers (swapped at the boundary); model loading split into a sim-side parse half and a queued GL-upload half serviced on the draw thread *before* event dispatch (creation records may reference the model); piece-transform extraction runs serially on the sim thread at the frame edge while the GPU upload stays draw-side. Each is a small custom bridge where a cleaner engine would have had a single ownership boundary.

### C.6 Writes from draw contexts

**The trait:** `LuaUnsyncedCtrl` setters (`Spring.SetUnitNoDraw`, team colors, …) mutate sim-owned fields directly from UI code — legal only under the single thread.

**The concession:** such writes queue and apply at the producer's next frame edge — one frame deferred, last-writer-wins. Read-your-own-write within a frame is gone: a widget that sets a value and immediately reads it back sees the old value until the next epoch. Nothing in the tested games depended on it, but it is a real, enumerated semantic change.

### The pattern

Every concession above is the same story at a different site: the engine had no seam between mutation and observation, so the split had to install one — and where the installation could not be hidden (shells, captures, chokes, parks, deferred writes), a scar shows. The compensating discipline is that every scar is *enumerated*: guarded by a tripwire, counted in telemetry, or bit-checked by the differential gate, rather than left as folklore.

## Addendum D — functionality disabled under the split

Five pieces of draw-pass code walk live sim structures in ways that would be structural races under the split. Rather than build epoch channels for them, they are suppressed when the flag is on (each is byte-identical flag-off). Four are developer debug tools; one is real user-facing functionality.

**The engine's native command-queue drawer (the one real loss).** `CommandDrawer` — the engine-side rendering of selected units' order queues, including the Lua-triggered path (`Spring.DrawUnitCommands` class) — is suppressed whenever the sim thread is live and unparked. It walks `CCommandAI` objects and their command deques directly; a unit dying mid-burst destructs its commandAI on the sim thread, leaving a dangling object under the drawer's `dynamic_cast`s, and the deque mutates under iteration. This is the one disabled feature with a planned restoration: the boundary-copied command-queue caches it needs already exist (the Lua command-queue callouts are served from them) — the drawer just hasn't been converted yet. Games that draw their own command visualization in Lua (as BAR does) are unaffected, because the Lua path is fully served.

**Debug overlays** — each logs `debug overlay unavailable with SimDrawSplit=1` and skips:

- **Pathfinder debug overlays** (QTPFS and HAPFS, the `/debugpath` family) — walk live pathfinder node trees and searches.
- **Collision-volume drawer** (`/debugcolvol`) — dereferences live object collision volumes.
- **QuadField drawer** — iterates the live spatial-partition structure.
- **Smooth-height-mesh drawer** — reads the live mesh.

These were judged not worth serving: developer tools, each of which would need its own epoch channel for data no shipped game reads. (The profiler overlay — `/debug` — is unaffected and gained split-aware sim/draw rows.)

**Latent rather than disabled**, for completeness:

- **Legacy skirmish-AI group callbacks** (the engine-group `AICallback` surface): unexercised by the tested games and neither served nor locked — "probably unsafe if some old AI uses them," an open item that would take a narrow lock or serving if triggered.
- **Checkpoint saves during a pool-valve park** could capture mid-frame sim state (the valve is the never-observed backstop from C.1; the recorded mitigation is a save-side retry).

Everything else that reads sim state from draw contexts kept working — via serving twins, query/reply, or a counted park. These five suppressions are the complete list of outright losses.
