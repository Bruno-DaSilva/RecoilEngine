# Research: decoupling sim and draw into two threads

Status: research only (2026-07-04). No code changes. Companion docs: `ENGINE_PERFORMANCE.md`, `move-unsynced-gameframe-work-todo.md`, `replay-seeking-architecture.md`.

## Update 2026-07-05: decisions and refinements

Outcome of a deep-dive on the Lua boundary and the strategic sequencing. The compatibility invariants and the concrete PR plan are at the end of this doc; the seeking-specific design (stream format, stages S0–S4, scrub UX) lives in `replay-seeking-architecture.md`.

1. **Driving use case: replay seeking.** The boundary work ships as a product (instant replay scrubbing) instead of an invisible refactor. Stage order: extraction v1 + stream format (S1) → stream-fed drawers (= Phases 0–1 here) → seek UX (S3) → sim thread slots in later as background-resim engine. The thread split becomes the capstone, not the driver.
2. **Lua mechanism settled: redirect the callout glue, not the VM.** No sim state lives in any `lua_State`; the entire sim-touching surface is the C callout layer (~250 `LuaSyncedRead` + ~177 `LuaUnsyncedRead` + ~169 `LuaUnsyncedCtrl` entry points). Snapshot-backed reads = flat arrays keyed by unitID (IDs are already dense and already the Lua-facing handle). No interpreter changes; API surface byte-compatible.
3. **No ECS prerequisite.** Extraction is a hand-written field copy running at the quiescent barrier — it walks live pointers and emits values; no pointers cross the boundary, so the sim graph stays pointer-rich. ECS remains an optional follow-up: once all draw-side consumers of a field read the extracted array, moving the authoritative copy to SoA is a small per-field step (the boundary is the incremental migration path, not the obstacle).
4. **Unbounded structures (command queues, paths): dirty-versioning + immutable shared blocks.** Copy cost is bounded by mutation rate (order-rate, not frame-rate); snapshot generations share unchanged blocks via refcount. `GetUnitCommands` already walks the queue per call today, so one boundary copy costs ~one current call. Interest tracking is an optimization only, not load-bearing. Spatial queries are boundary-computed against a draw-side grid.
5. **DECISION — no lock fallback, boundary must be complete.** Both blanket sim-locking (old option E-1) and a cold-miss lock are rejected: they convert functional gaps into load- and widget-dependent timing edges (unattributable hitches in the field). The boundary serves everything it claims to serve; the residual tail gets a documented deterministic contract (stale/nil, identical every time, dev-mode warning naming the callout). Consequence: full classification of the callout surface, driven by call-count telemetry, is a *precondition* of enabling the split — not lazy per-miss work.
6. **Claims corrected after review.** (a) Snapshot hashing localizes desyncs in *space* (which unit/field), not time — per-frame detection already exists (`NETMSG_SYNCRESPONSE`), and deep-state divergence can precede observable divergence by many frames, so root-causing still needs deep tools; hierarchical hash ring (~unit-level, ~25 MB for a ~10 s window) is the always-on tier, field-level values via opt-in ring or offline deterministic resim. (b) Render-replay is a byproduct, not a pillar: useful for GL-migration parity, sim-noise-free draw benchmarks, and crash-repro iteration (share the demo + frame; regenerate the stream locally; clip a few seconds around the crash for hand-off). (c) The observable stream **cannot** serve reconnect-to-live-game — reconnect needs bit-identical deep sim state; the stream is a viewing artifact only.
7. **GL4 migration will not come first and is not a dependency.** The boundary serves the current widget call pattern at full fidelity; migration, if/when it happens, shrinks demand as a bonus only.
8. **Size estimates are gated on measurement.** Two known holes in the Fermi numbers: per-piece pose data (~10–20 MB/frame raw at 10k units; `transformsMemStorage` is the in-tree proxy to instrument) and projectile lifecycle churn (mitigation: ballistic projectiles re-derived from spawn params; guided ones need updates). Hard gate: instrument sizes and dirty rates over 2–3 late-game BAR replays before designing the stream format. **GATE CLEARED 2026-07-05** — S0 measured over two full-length replays (Wave-3 PRs 1–2): piece-pose raw was a 4–8× overestimate (~0.5–0.8 MB/frame at 2–3k units, 33–35% per-frame churn), projectile churn is a non-issue (synced spawns ~2/frame, ≥96% re-derivable, guided 2–4%); measured numbers now in `replay-seeking-architecture.md` "Sizes".

## TL;DR / verdict

It is feasible, but it is a large multi-phase project, and the hard part is not the threading — it is the ~8 places where draw and sim alias the same objects, plus Lua. The engine already contains about half of the required "extraction layer" (transform/uniform SSBO copies, heightmap synced/unsynced split, one-frame `preFrameTra` interpolation base). The blockers, in increasing order of pain:

1. Draw code **writes into sim objects** every frame (`drawPos`, `drawFlag`, icon state, `drawAlpha`, `losStatus &= ~LOS_PREVLOS`, localModel dirty flags) — must move to render-owned side structs.
2. Sim fires **synchronous render events** mid-sim-frame (`RenderUnitCreated` etc. mutate drawer containers) — must become a queued delta applied at a frame boundary.
3. The **CPU draw pass and picking read live sim objects** directly (localModel, collision/selection volumes, quadfield) — needs extraction (locks rejected, see decisions above).
4. **Lua draw callins** (`DrawWorld` etc.) call `Spring.GetUnitPosition`-class callouts that dereference live sim objects arbitrarily — the single biggest obstacle, with no cheap fix.

The realistic architecture is **pipelined frames with a synchronized extract boundary** (the Factorio model), not free-running threads: sim thread simulates frame N+1 while the main thread renders frame N; they meet once per draw frame at a short "extract" barrier. GL stays on the main thread (mandatory), so it is the *sim* that moves off-main.

Recommendation (updated 2026-07-05): execute Phases 0–1 as the PR plan at the end of this doc, shipped as the replay-seeking product (`replay-seeking-architecture.md`); the thread split (Phase 2+) is the capstone, gated on boundary completeness for the draw-time callout surface. The BAR GL4 migration is explicitly **not** a prerequisite and will not come first — the boundary must serve the current widget call pattern as-is; any future migration only shrinks demand as a bonus.

## Prior art

- **GML / Spring MT (zerver fork, ~91–94 era)**: threaded GL wrapper + partially-threaded sim ("ASIM") + async pathfinding ("APATH"). Never merged; removed. Failure modes: licensing, fragility, and — decisive — Lua-driven rendering made the GL translation layer slower than single-threaded for the games people actually played. The docs literally warned "LUA-BASED GRAPHICS WILL CAUSE HIGH CPU LOAD AND SEVERE SLOWDOWNS". Nothing of it remains in the tree (grep is clean); this is a clean slate, not an untangling job.
- **Current maintainer direction** (issues #2316 ECS, #1210 MT loading, threadpool/affinity work): keep one main thread for deterministic sim + GL, push data-parallel work onto the worker pool (`for_mt`), run QTPFS in the background, move render-prep off the sim frame. #1210 states the two invariants directly: *Lua is not multi-thread safe* and *OpenGL calls must occur on the main thread*. No open issue proposes a sim/render split.
- **Factorio** (FFF #215, #281): same lockstep-determinism constraints; their landed design is exactly the pipelined model — render runs parallel to update, synchronizing only a short "prepare" step, leaning heavily on interpolation. That is the shape to copy.

## Current architecture (what the seam looks like)

One thread does everything, in this order per iteration (`SpringApp::Run` loop, `rts/System/SpringApp.cpp:905,919`):

```
SpringApp::Update (SpringApp.cpp:871)
├─ CGame::Update (Game.cpp:1370)            [SIM SIDE]
│   └─ ClientReadNet (NetCommands.cpp:259)
│       └─ 0..N × NETMSG_NEWFRAME → SimFrame (Game.cpp:2000)
│            (budgeted: msgProcTimeLeft, capped by minDrawFPS so catch-up
│             can't starve rendering — NetCommands.cpp:239-241)
├─ CGame::Draw (Game.cpp:1640)              [DRAW SIDE]
│   ├─ UpdateUnsynced (Game.cpp:1418)
│   │   ├─ timeOffset = fraction of a sim frame elapsed (Game.cpp:1472)
│   │   └─ worldDrawer.Update → DrawerData extraction, drawPos interpolation
│   └─ drawOnePass(): shadows, DrawWorld, DrawScreen
└─ SwapBuffers
```

Useful properties already in place:

- **Draw FPS is already decoupled from sim rate logically** — draw interpolates between the last two sim states via `preFrameTra` (snapshotted in `UpdatePreFrame`, `Game.cpp:2070`) and `globalRendering->timeOffset`. The one-frame transform double-buffer a sim thread needs *for positions* already exists.
- **GameServer already runs on its own "netcode" thread** (`GameServer.cpp:263`) and flow-controls NEWFRAMEs against the client's consumption (`GameServer.cpp:2694-2700`). Sim pacing needs no changes; the sim thread just becomes the consumer of the net queue instead of the main thread.
- **The heightmap is the reference pattern**: synced heightmap owned by sim, unsynced copy owned by draw, dirty-rect queue drained at draw time (`ReadMap.cpp:69-70,96,496-514`). This is what "done right" looks like for every other shared structure.
- **The GPU data path is already an extraction layer**: `UpdateObjectUniforms`/`UpdateObjectTrasform` copy sim fields into `transformsMemStorage`/`modelUniformsStorage`, streamed to triple-buffered persistent SSBOs with dirty-lists (`ModelDrawerData.h:161-218`, `ModelsDataUploader.cpp`), guarded by `CModelsLock`, deduped per sim frame via `lastSyncedFrameUpload`.

## Threading infrastructure status

- There is **no sim-thread concept**: thread registry is `MAIN/LOAD/SND/VFSI/WDOG` only (`Threading.h:30-37`). Adding `THREAD_IDX_SIM` + `SetSimThread/IsSimThread` + a watchdog slot is trivial and mechanical.
- The **game-load thread** (`GameLoadThread.cpp`, `LoadLock.h`, `WrappedSync.h`) is the working in-tree template for "second thread that shares state with main under a handoff lock", including streflop FPU init on the new thread and the runtime `SetThreadSafety` toggle to make the locks free when the feature is off. A sim thread should copy this pattern (minus the GL-context handoff — the sim thread must never touch GL).
- **ThreadPool is a shared blocking fork-join pool** used by *both* sim (`LosHandler`, QTPFS/HAPFS, `GroundMoveSystem`, `UnitHandler`) and rendering (`UnitDrawerData`, `FeatureDrawerData`, `RoamMeshDrawer`, `ProjectileDrawer`). Today these serialize through the main thread; with two threads calling `for_mt` concurrently they contend, and `GetThreadNum()`-indexed scratch buffers (`ThreadPool.h:475,494,538,553`) assume one fork-join in flight. Needs either two pools, pool partitioning, or making the per-thread scratch fork-join-instance-local. Sim correctness asserts (`assert(!ThreadPool::IsInMultiThreadedSection())`) keep working unchanged since they are thread-local.
- **One GL context, main thread only.** That is fine for this design — the sim thread does no GL. The `IsMainThread() || IsGameLoadThread()` asserts in `IModelParser.cpp:238,262,467` and `ExplosionGenerator.cpp:1090` mark places where *sim code triggers GL-resource creation* (model loading on unit build, explosion generators); these become cross-thread requests that must be queued to the main thread (model preload already has machinery pointing this direction).

## The coupling inventory (what actually has to change)

### A. Draw→sim write-backs (must move to render-owned storage first)

These are fields that live inside `CUnit`/`CFeature`/`CProjectile` but are authored by draw code every draw frame. With a sim thread they are races on day one:

| Field | Written at | Fix |
|---|---|---|
| `drawPos`/`drawMidPos` | `UnitDrawerData.cpp:367-379`, `FeatureDrawerData.cpp:212-217`, `ProjectileDrawer.cpp:379` | render-side array keyed by drawer slot; sim never reads these (verify — `slowUpdate` icon distance does) |
| `drawFlag`/`previousDrawFlag` | `ModelDrawerData.h:224-225` + cull passes | render-side visibility bitset |
| `SetIsIcon`/`currentIconIndex`/`iconRadius` | `UnitDrawerData.cpp:263-364` | mostly render-side, **but `iconRadius` is used for click-picking** — keep in the picking snapshot |
| `CFeature::drawAlpha` fade | `FeatureDrawerData.cpp:150-176` | render-side |
| `p->SetSortDist` | `ProjectileDrawer.cpp:403` | render-side sort key array |
| `LocalModelPiece` dirty-flag reset/recompute | `ModelDrawerData.h:182-197` | **DONE 2026-07-05 (PR 8)** — transform upload is now a defined extraction pass (`CModelDrawerDataBase::ExtractTransforms`, full contract in the comment block there): runs once per new sim frame at the first drawer-data `Update()` after the sim burst and covers every object in LOS for the local allyteam (everything under full-view/replay), camera-independent — the old per-object `lastSyncedFrameUpload` dedup and the drawFlag gate are gone — so no post-extraction consumer ever recomputes a dirty piece in place; `wasUpdated`/`noInterpolation` are consumed and reset only at the extraction point. The LOS (not drawFlag) gate is deliberate: extracting *hidden* objects would push live enemy poses into the Lua-shader-readable SSBO, widening master's information surface (same gate as `UpdateObjectUniforms`); out-of-LOS objects keep master's accumulate-until-seen behavior, and per-allyteam extraction is the boundary contract a player-POV client will need anyway. Key audit result: every LMP mutator is synced-sim-side (UnitScript ticks, `Move/TurnNow`, `Spring.SetUnitPieceMatrix`, `SetScriptVisible`), so nothing can mutate between extraction and draw — legacy `GetModelSpaceMatrix()` reads and the extracted SSBO copies are frame-consistent by construction. Layout for PR 15: per-object slots `[0]`=prev root (preFrameTra), `[1]`=curr root, `[2+2i]/[3+2i]`=piece i prev/curr model-space, script-invisible pieces = `Transform::Zero()` |
| `losStatus[allyTeam] &= ~LOS_PREVLOS` | `UnitDrawerData.cpp:637,720-721` | **DONE 2026-07-05 (PR 7)** — audit found the doc premise wrong: the clears fire at sim-event time (not draw rate) and LOS_PREVLOS has synced readers (GetErrorVector→weapon aim, GenerateWeaponTargets, synced GetUnitLosState), so a render-side shadow would desync; the clears moved *into* sim (`CUnit::SetLeavesGhost`, bit-identical ordering) and the dying-unit clear was dropped as provably unobservable — drawer is now a pure losStatus reader |
| `tempNum`/`mtTempNum` quadfield scratch | `WorldObject.h:82,104` stamped by draw-time `GuiTraceRay` | dedicated scratch id space for draw-thread queries, or picking runs at the boundary |

Each row is an independently landable refactor with zero behavior change. This is Phase 0.

### B. Synchronous sim→render events

`RenderUnitPreCreated/Created/Destroyed`, `RenderFeatureCreated/...`, LOS-transition events fire from inside the sim frame and synchronously mutate drawer containers, ghost lists, decals, and the SSBO alloc maps (`Unit.cpp:381-404`, `UnitHandler.cpp:292`, `UnitDrawerData.cpp:662-723`, `GroundDecalHandler.cpp:1737-1753`). These must become a per-sim-frame **delta queue** (create/destroy/LOS-change records) drained by the draw side at the extract boundary. The heightmap's `unsyncedHeightMapUpdates` queue is the in-tree precedent. Deletion is the sharp edge: drawer containers hold raw `CUnit*`/`CFeature*` (`ModelDrawerData.h:82`), so object lifetime must be extended past the boundary (deferred-delete list owned by sim, freed only after the draw side acks the destroy delta).

**DONE 2026-07-05 (PR 13)** — deferred-deletion epoch, landed as `DeferredObjectDeleter` (`rts/Sim/Objects/DeferredObjectDeleter.{h,cpp}`; full contract in the class comment). The destroy sharp edge is resolved by splitting sim-object destruction: the sync-observable destructor half moved verbatim into a virtual `PreDestruct()` (`CObject` severing, `~CUnit` wreck/weapon/script/AI teardown, `~CBuilding`/`~CExtractorBuilding` unblock/extraction, `~CFeature` unblock/quadfield/geotherm, `~CProjectile` quadfield, nano-counter, damage-array decref) that still runs at the exact old `pool.free()` call sites — sim timing bit-identical to master by construction — while the undestructed shell (plain fields + localModel) parks in the deleter so every queued record, including the destroy itself and any earlier creation/LOS records of the same object, dispatches against valid memory in exact fire order at the drain. The three `Render*Destroyed` methods became the plain `Push` the PR-12 handoff note promised; the mid-sim flushes are gone. Ack point is a single explicit function: `AckDrainedDestroys()`, called right after `renderEventQueue.Drain()` in `CGame::Draw`, runs the real (body-skipping, guarded by `CObject::detached`) destructors and overwrites each slot with `0xDE` poison; `ReleaseAcked()` at the end of the same Draw returns the slots to the pools (which zero on free — the poison is the detector for the pooled-UAF bug class ASan cannot see, since any draw-side dereference after the ack faults on `0xDEDE..` instead of reading a zeroed/recycled slot; a DEBUG-only scan at release also catches stray writes). Deferring member teardown and slot reuse is sync-safe by address-blindness; verified by both master-demo resim gates. Pool-pressure valve: the pools are fixed-size, so `Defer()` checks headroom and below 64 free pages flushes the queue in place (the pre-PR-13 destroy semantics) and releases everything — under the future split this becomes "sim waits for the boundary". Post-drain invariant handlers may now rely on: drawer containers hold live objects only. Enumerated Lua deviation (doc'd tier (a)): the `RenderUnitDestroyed` callin now fires at the boundary drain, so callouts querying the dead unitID inside it get nil (the pushed id/defID/team args are read from the shell and unchanged).

**DONE 2026-07-05 (PR 12)** — landed as `RenderEventQueue` (`rts/Rendering/Common/RenderEventQueue.{h,cpp}`; full semantics in the class comment). Creation and LOS-transition events append plain-data records during the sim phase (window opened by `CGame::Update`) and drain — everything, in exact fire order, once — at the top of `CGame::Draw`; catch-up bursts therefore apply N frames of records at one boundary. Facts that sim overwrites before the drain are captured into the record at fire time: the LOS records carry the event-time `leavesGhost` bit, and `UnitLeavesGhostChanged` (the direct `unitDrawer->` call from `CUnit::SetLeavesGhost`, which reads `losStatus` that sim clears immediately after firing) is now also a queued record carrying a per-allyteam dead-ghost mask — with every ghost-container mutation flowing through the queue in order, drawer end-state replays master op-for-op. Destruction stays synchronous behind the same API per the PR-13 gate: `Render*Destroyed` flushes the queue (an early drain, order-exact) then dispatches in place, because all three destroy sites free the object immediately after; the PR-13 handoff point is exactly those three methods (flush+dispatch → plain Push once the deferred-deletion epoch extends lifetimes). Outside the sim phase the queue dispatches immediately (empty-queue invariant preserves order): game-load creation and Lua draw-callin CEG spawns keep master timing. The record types are the seed of the recorded-stream event schema (`replay-seeking-architecture.md`); the embedded object pointer is a transitional dispatch handle that PR 14 (ID-keyed drawer containers) removes.

### C. Direct sim reads during the draw pass

Beyond the Update-phase extraction, the actual render pass still dereferences live objects: `u->drawFlag`, `GetIsIcon()`, `luaDraw`, `localModel.Draw()`, `GetTransformMatrix()`, minimap's `losStatus`/radar radii reads (`UnitDrawer.cpp:188-451`, `MiniMap.cpp:862,1943-1963`), health-bar reads, `CUnitTracker`/`DollyController`/`DynWater` position reads, info textures reading sim LOS/metal state, `SmoothHeightMeshDrawer` reading the sim mesh. Under the pipelined model these all read **frame-N state while sim mutates frame N+1** — every one must either (a) read from the extracted snapshot, or (b) be tolerable-if-torn (some are: health bar off by a frame is invisible; LOS texture a frame late is invisible) with the field made atomic-relaxed. An explicit torn-read policy per field beats trying to snapshot everything.

### D. Picking / TraceRay

`GuiTraceRay` walks the sim quadfield and reads live collision/selection volumes + transforms (`TraceRay.cpp:352,426-432,453,495`). Called from mouse, GuiHandler, minimap, and Lua `TraceScreenRay` — i.e. from the draw/input side, potentially many times per frame. DECIDED: picking runs draw-side against the boundary snapshot — positions, radii, collision-volume params, piece transforms (if it renders, it raycasts) — with box-select served by a draw-side grid over snapshot positions; a shared read lock on the quadfield is rejected (no-lock decision). Synchronous callers like Lua `TraceScreenRay` read the previous boundary's snapshot (≤1 draw frame of pick latency, same staleness as everything else they see).

### E. Lua — the big one

- `LuaRules`/`LuaGaia` already have **separate synced and unsynced `lua_State`s** (`LuaHandleSynced.h:180-268`) and LuaUI is its own state — the state-separation groundwork exists. But `GetSyncedHandle(L)` cross-hops exist and must be barred cross-thread.
- The killer: unsynced draw callins (`DrawWorld`, `DrawUnit`, `DrawScreen`, … `LuaHandle.cpp:2727-2909`) freely call `LuaSyncedRead` callouts that dereference live sim objects (`LuaSyncedRead.cpp:537-544,2948,3260`). BAR calls these tens of thousands of times per frame (see `project_lua_boundary_profiler_findings`). Resolution (2026-07-05 decisions above):
  1. **Locking is rejected** — both blanket sim-locking during draw callins (the GML failure mode) and a cold-miss lock (widget/load-dependent timing edges). The boundary must be complete for everything it serves before the split can enable.
  2. **Snapshot-backed reads are the mechanism**: on the draw thread, `Spring.GetUnitPosition`-class callouts read the extracted frame-N snapshot. Requires classifying the LuaSyncedRead surface (hundreds of callouts) into snapshot-served / boundary-computed / contracted stale-nil — driven by call-count telemetry, completed *before* the flip. Start extraction with the ~30 callouts that dominate call counts.
  3. **GL4 migration is not a dependency** (decided 2026-07-05: it will not come first). The boundary must serve today's call pattern — tens of thousands of per-unit getters per frame — at full fidelity. If widgets later migrate to engine-side instancing they stop calling per-unit getters at draw time, which shrinks demand as a bonus, but no serving work is deferred in anticipation of that.
  Note `gl.CreateList`-era semantics and the A/B parity work assume draw-time sim reads are frame-consistent — the snapshot actually *improves* consistency (today a mid-draw sim frame can't happen, but mid-*Update* Lua sees half-updated frames during catch-up).

### E.1 S0 draw-time callout census (measured 2026-07-05)

The callout-context telemetry (PR 2, `/calloutcensus`: every callout invocation bucketed as draw-callin / sim-phase / other) run full-length over the two S0 replays (headless resim, engine-2026.06.10 demos; see `replay-seeking-architecture.md` "Sizes" for the setup). Top 30 callouts by draw-context demand, normalized per draw frame:

| Callout | Rosetta draw calls | /draw frame | ATG draw calls | /draw frame | sim-phase share |
|---|---|---|---|---|---|
| `GetMouseState` | 3,158,161 | 8.00 | 2,310,579 | 8.00 | 0% |
| `IsGUIHidden` | 2,763,187 | 7.00 | 2,021,551 | 7.00 | 0% |
| `GetUnitViewPosition` | 736,767 | 1.87 | 1,627,994 | 5.64 | 0% |
| `Blending` | 2,003,458 | 5.07 | 1,476,881 | 5.11 | 0% |
| `GetGameFrame` | 1,269,128 | 3.21 | 938,543 | 3.25 | 39% |
| `Translate` | 918,431 | 2.33 | 703,041 | 2.43 | 0% |
| `CallList` | 894,141 | 2.26 | 665,857 | 2.31 | 0% |
| `PopMatrix` | 845,655 | 2.14 | 608,842 | 2.11 | 0% |
| `PushMatrix` | 845,655 | 2.14 | 608,842 | 2.11 | 0% |
| `GetActiveCommand` | 819,404 | 2.08 | 610,654 | 2.11 | 0% |
| `GetModKeyState` | 789,482 | 2.00 | 577,586 | 2.00 | 0% |
| `CreateTexture` | 784,333 | 1.99 | 573,745 | 1.99 | 0% |
| `Begin` | 761,249 | 1.93 | 546,178 | 1.89 | 0% |
| `End` | 761,249 | 1.93 | 546,178 | 1.89 | 0% |
| `GetMapDrawMode` | 759,331 | 1.92 | 544,287 | 1.88 | 0% |
| `IsUnitVisible` | 269,416 | 0.68 | 414,253 | 1.43 | 0% |
| `Color` | 445,969 | 1.13 | 327,965 | 1.14 | 0% |
| `GetCameraPosition` | 424,493 | 1.08 | 321,708 | 1.11 | 0% |
| `Scale` | 420,786 | 1.07 | 286,755 | 0.99 | 0% |
| `SetTextColor` | 396,201 | 1.00 | 290,222 | 1.00 | 0% |
| `GetMouseCursor` | 394,741 | 1.00 | 288,793 | 1.00 | 0% |
| `GetSelectionBox` | 394,741 | 1.00 | 288,793 | 1.00 | 0% |
| `SetOutlineColor` | 394,741 | 1.00 | 288,793 | 1.00 | 0% |
| `Print` | 366,966 | 0.93 | 257,847 | 0.89 | 0% |
| `GetGameSeconds` | 364,826 | 0.92 | 255,739 | 0.89 | 4% |
| `GetUnitPosition` | 125,108 | 0.32 | 63,489 | 0.22 | 26% |
| `GetFeaturePosition` | 12,517 | 0.03 | 90,599 | 0.31 | 5% |
| `AlphaTest` | 59,504 | 0.15 | 65,830 | 0.23 | 0% |
| `WorldToScreenCoords` | 69,141 | 0.18 | 32,459 | 0.11 | 0% |
| `IsUnitIcon` | 55,967 | 0.14 | 31,030 | 0.11 | 0% |

Classification takeaways for the boundary contract:

- **The draw-context surface splits cleanly into three serving classes.** (1) GL/font calls (`Blending`, `Translate`, `CallList`, `Begin/End`, `Print`, ...) never touch sim state — free on a draw thread. (2) Input/UI/camera state (`GetMouseState`, `IsGUIHidden`, `GetActiveCommand`, `GetModKeyState`, `GetSelectionBox`, `GetCameraPosition`) is unsynced main-thread state — also free. (3) Sim-state reads that need snapshot serving are concentrated in a *small* family: `GetUnitViewPosition`, `IsUnitVisible`, `GetUnitPosition`, `GetFeaturePosition`, `IsUnitIcon`, `WorldToScreenCoords`(-adjacent), plus low-rate `GetUnitDefID`-class lookups — the positions family predicted by decision 2 dominates exactly as assumed.
- **Family shares of draw-context demand**: `GetUnit*` is 3.9% (Rosetta) / 9.6% (ATG) of all draw-context calls — and essentially the *only* synced-state family with volume; `GetFeature*` ≤0.5%, `GetProjectile*` literally zero in draw context, `GetTeam*`/`GetPlayer*` ≤0.02%. Snapshot-serving the positions/visibility family covers the demand cliff; the long tail (hundreds of callouts) is candidate stale/nil-contract territory at these volumes.
- **Context split is real and matters**: `GetGameFrame` is 39% sim-phase (gadget event handlers), `GetUnitPosition` 26% — the same callout name serves both sides, so the redirect must be per-*handle* (which lua_State/thread), not per-name; counts here justify per-callin glue redirection (decision 2) rather than global function swaps.
- **Headless caveats** (this census is a classification set + ranking, not an absolute-load forecast): GL-stubbed widgets self-disable some render paths, so real-GL per-frame demand is higher — the live-game boundary profiler measured ~27k callouts/sim-frame (`project_lua_boundary_profiler_findings`) vs the ~2.4k/draw-frame seen here; the fast-forward draw:sim ratio (6–9 draws/sim frame) inflates "other"-context totals; per-draw-frame columns are ratio-independent. A live-GL census (one `/calloutcensus` in a real game with `LuaTrackCalloutCounts=1`) should be captured before finalizing the PR-18 family order, but the *shape* — positions family on top, projectiles absent, teams/players negligible — is robust.

### F. Misplaced work (cheap, do first)

`SimFrame()` runs a block of unsynced work (`waitCommandsAI`, `geometricObjects`, `sound->NewFrame`, `eoh->Update`, group handlers — `Game.cpp:2046-2062`, already commented as misplaced) and `UpdateUnsynced` computes stuff sim needs nothing of. `move-unsynced-gameframe-work-todo.md` already tracks the pattern. Everything in that block must be classified: stays with sim thread, moves to draw thread, or becomes boundary work.

## Proposed architecture (Phase 2 target)

Pipelined, one-frame-deep, barrier-synchronized:

- **Main thread** (unchanged identity): SDL input, all GL, `CGame::Draw`, Lua unsynced states, swap.
- **Sim thread** (new): `ClientReadNet` → `SimFrame`s, `ENTER_SYNCED_CODE`, synced Lua, streflop FPU init at spawn (copy `GameLoadThread.cpp`), watchdog slot `WDT_SIM`.
- **Boundary (once per draw frame, short)**: sim pauses at a frame edge (never mid-SimFrame); drain render-event delta queue; run transform/uniform extraction (`worldDrawer.Update` extraction half) + heightmap `UpdateDraw`; snapshot the small hot-field set (health, losStatus copy for minimap/icons, radar radii); release sim; render frame from snapshot while sim consumes more NEWFRAMEs.
- **Backpressure**: sim runs at most far enough ahead that interpolation stays within one frame (it already targets ~2 frames behind server, `NetCommands.cpp:204-208`); during catch-up/replays sim free-runs and draw just renders the latest boundary — this is where the split pays off most (fast-forward no longer fights rendering for the thread, cf. `project_headless_speed_control`).
- **Feature flag**: config var (à la `LoadingMT`), single-threaded path preserved via `WrappedSync`-style no-op locks. Headless keeps the current model.

Determinism note: the sim code itself is unchanged and stays on one thread, so lockstep sync is not inherently at risk — the risks are indirect (FPU state on the new thread, any sim code that accidentally reads draw-authored fields — which Phase 0 eliminates, and event *timing* of unsynced observers). `ASSERT_SYNCED`/sync-checker machinery is thread-agnostic.

## What it buys

Main-thread frame time today is `Σ(simFrames) + draw`. The split makes it `max(sim, draw)` plus the boundary cost (Amdahl on the boundary). Concretely:

- Steady 30 Hz play: draw stutter from expensive sim frames disappears; sim spikes (big battles, path floods) no longer eat draw frames and vice versa.
- Catch-up / replays / fast-forward: currently capped by `minDrawFPS` budgeting sharing one thread; split, sim saturates a core while draw stays smooth.
- It does **not** speed up sim itself. Under the no-lock decision the split does not enable at all until the draw-time callout surface is served/contracted (§E), so there is no degraded lock-mode intermediate — the flip is all-or-nothing per the completeness precondition.

## Phasing and effort

| Phase | Content | Effort | Standalone value |
|---|---|---|---|
| 0 | Move all §A write-backs to render-owned storage; split `tempNum` scratch; relocate §F misplaced work | ~8 independent medium refactors | Yes — cleans aliasing, enables `const` sim during draw, helps current MT asserts |
| 1 | Render-event delta queue + deferred object deletion (§B); formalize the extract boundary as one function; torn-read policy for §C fields; boundary-deferred picking (§D) | Large; the deletion-lifetime work is the riskiest part | Partial — queue also fixes today's "sim frame mutates drawer containers mid-catch-up" ordering quirks |
| 2 | Sim thread itself: `THREAD_IDX_SIM`, spawn/join, boundary barrier, feature flag, ThreadPool partitioning, GL-resource request queue (model loads), watchdog | Medium once 0–1 are done | The actual payoff, behind a default-off flag; cannot enable before Phase 3's classification is complete (no-lock decision) |
| 3 | Lua: cross-hop bans, snapshot-backed callout serving, per-callout contract (served / boundary-computed / stale-nil) | Scope bounded by the callout census | Precondition of the Phase-2 flip, not a follow-up |

Rough total: Phases 0–2 are on the order of a couple of engine-dev months of focused work; Phase 3's scope is set by the callout census (the GL4 migration is explicitly not a prerequisite — see decisions).

Mapping to the PR plan below: Tracks 1–5 refine Phases 0–1 plus the telemetry/classification half of Phase 3; Track 6 is the seeking product built on them; Track 7 is Phase 2.

## Compatibility invariants

**Sync compatibility with master is a hard invariant for Tracks 1–6** (a branch client must not desync in a game with a master client). Structural argument: only synced-code changes can desync; every PR below is draw/unsynced-side or read-only. §A evictions are sync-safe *by construction* — those fields are authored at draw rate, which already differs across machines on master, so sync is provably blind to them (else master would already desync between a 60 and a 240 fps player). Deferred deletion is safe by address-blindness: cross-machine sync working today proves synced code cannot observe object addresses or allocation order.

**Enforcement, per PR:** demos recorded on master must resim clean on the branch with matching per-frame checksums (the `test/replay-rewind` harness runs exactly this). A demo is "same net input" by definition, so a clean resim *is* the sync-compat proof. Live same-lobby dogfood vs a master client as end-to-end confirmation. (Lobby version pinning may prevent actual mixed-version play; the invariant's practical value is master demos as regression corpus, bisectability, and dogfooding.)

**Build & gate runbook** (how the per-PR gates are actually run on this branch):

- Build: plain non-docker `build/` (`cd build && ninja engine-headless`; `engine-legacy` and `engine-dedicated` should also link before committing). Use the GCC toolchain — clang without `-frounding-math` desyncs replays around f=300.
- Resim gate: `SPRING_BIN=$REPO/build/spring-headless S0_BOUNDARY=0 S0_CALLOUTS=0 test/s0-measurement/run_s0.sh <demo.sdfz> <label>` per demo. The gate demos are the two S0 replays (Rosetta 1.4.4 and All That Glitters Extended, engine-2026.06.10 demos, `replays/` in the repo root); data dir defaults to `/www/projects/bar-data` (`--isolation --write-dir`, BAR content must be in the pool — see the restore notes in the S0 README).
- Speed: gates run fast-forwarded (`/setspeed 20` + `/speedcontrol 0`, the driver widget's default) — this is the catch-up stress case and the standard; 1x runs add nothing sim-side (resim is speed-invariant) and take ~25 min each vs ~3 min.
- Verdict: `run_s0.sh` greps the infolog for `DESYNC` and copies it to `<label>_infolog.txt`. A clean full-length run (Rosetta f=44537, ATG f=44856) with zero DESYNC is a pass. **Always confirm the `Spring Engine Version:` line in the infolog names the branch/commit you think you built** — a stale binary under `build*/install/` once hijacked a gate run and produced a convincing-looking "desync" (the script now excludes install trees from its binary search, but the version check stays mandatory).
- Environment: stash the A/B driver widgets out of `<write-dir>/LuaUI/Widgets/` before gate runs (one of them force-quits at f=30000 and they don't reflect stock BAR); they currently live in `Widgets.ab-stash/` next to it.

**Sync audit list** — the four places the structural argument has an asterisk; each is the review focus of its PR:
1. Does `slowUpdate` icon-distance logic read `drawPos`? If any synced path reads a draw-authored field, that's a latent master desync bug — find and fix (read sim `pos`) before/with the eviction.
2. `LOS_PREVLOS`: eviction stops draw code clearing a bit inside synced `losStatus`; needs an explicit no-synced-readers audit. **DONE 2026-07-05 (PR 7)** — synced readers DO exist (GetErrorVector, GenerateWeaponTargets, synced Lua LOS queries), which killed the shadow design; resolution and full reader table in the PR-7 commit message.
3. Each §F relocation item needs a touches-synced-state/RNG check before moving (e.g. confirm `waitCommandsAI` is genuinely unsynced-side despite the engine's "misplaced" comment); anything synced stays put.
4. Track-1 instrumentation must be side-effect-free reads (no streflop perturbation).

**Lua API compatibility tiers:** signatures identical by construction (same registered C functions). Synced Lua bit-identical (non-negotiable). Draw-callin reads: snapshot-served values are the same frame's values as today (draw already only ever sees the last completed sim frame) — semantics preserved, consistency improved. Enumerated deviations only: (a) unsynced event handlers deferred from mid-sim-frame to the boundary; (b) the contracted sim-only tail returns deterministic stale/nil on the draw thread (chosen via telemetry to be what games don't call at draw time, dev-mode warning on trip); (c) unsynced writes apply at the boundary. Verified by the callout differential gate (PR 17) + the single-threaded feature-flag path preserving exact legacy behavior.

## PR plan (individually reviewable, each standalone-valuable)

Ordered **Fable-first**: Fable work is front-loaded to the maximum the dependency graph allows, because every Fable PR emits the artifact (pattern, audit table, contract, inventory) that converts its followers into Opus-grade pattern-following. Opus work runs *behind* the Fable frontier in parallel — the frontier never waits for it. Original track labels (T1–T7) kept for cross-reference; PR numbers are stable identifiers, not order.

Model tiers — **[F] = Fable**, **[O] = Opus**. Rule of thumb: Fable where the work is design-setting (first of a pattern), proof-carrying (sync audits), or semantic-edge-heavy (lifetime, ordering, lazy→eager changes); Opus where a pattern already exists, the spec here is complete, and the mechanical gates (demo-resim, diff gate) catch errors. Nothing before PR 19 changes game-visible behavior. Verification: master-demo resim gate (above), benchmark suite. (Decided 2026-07-05: the GL4-parity A/B pixel gate is not used for these PRs — it lives on the gl4 harness branch and compares render backends, not sim/draw refactors; visual checks here are manual where relevant.)

**Wave 1 — Fable frontier (no upstream deps; start immediately, mutually parallel)**
3. [F] (T2) `drawPos`/`drawMidPos` → render-side array. Sets the Track-2 eviction pattern; includes sync audit item 1. Unblocks PRs 4–6, 9.
7. [F] (T2) `LOS_PREVLOS` clearing → render-side prev-LOS shadow (audit item 2; synced-reader audit + ghost-bookkeeping semantics). **DONE 2026-07-05** — audit overturned the shadow design (synced readers exist; clears are sim-event-time, not draw-rate); landed as sim-side ownership move instead, full audit table in the commit message (`Move LOS_PREVLOS ghost accounting from draw code into sim`). See §A row.
8. [F] (T2) `LocalModelPiece` dirty-flag recompute → extraction-time snapshot of piece transforms (lazy→eager semantic change; animation-correctness and perf edges). Feeds PR 15's piece-transform layout. **DONE 2026-07-05** — landed as `CModelDrawerDataBase::ExtractTransforms` (defined once-per-new-sim-frame pass over all objects); see §A row and the extraction-site comment block for the contract and PR-15 layout.
11a. [F] (T3) Synced-state/RNG classification table for every §F item (audit item 3) — lands as reviewable doc/table before any code moves. Unblocks 11b.
12. [F] (T4) Render-event delta queue — ordering semantics and catch-up edge cases are the substance. With 13, defines the rules PR 14 follows. **DONE 2026-07-05** — landed as `RenderEventQueue`; see the §B DONE note for the semantics (event-time capture for ghost bookkeeping, destroy-flush PR-13 handoff, immediate mode outside the sim phase).
13. [F] (T4) Deferred-deletion epoch + freed-slot poisoning — the sharpest lifetime edge in the plan; wants the strongest model *and* the strongest review. **DONE 2026-07-05** — landed as `DeferredObjectDeleter` (PreDestruct split keeps all sync-observable teardown at master timing; shells stay readable until the boundary drain; slots poisoned `0xDE` between ack and release); see the §B DONE note for the full design.
15. [F] (T5) `SimSnapshot` v1 + extraction pass with one converted consumer — sets the contract every later consumer/callout PR follows.
21a. [F] (T6) Draw-side accumulator inventory for the reset hook — doc-only, completeness-critical (a missed accumulator = subtle post-seek corruption); no code deps, can start anytime.

**Wave 2 — Opus follow-through (each consumes a Wave-1 artifact; runs behind the frontier)**
4. [O] (T2) `drawFlag`/`previousDrawFlag` → render-side visibility bitset (PR-3 pattern).
5. [O] (T2) Icon state → render-side; `iconRadius` kept picking-visible (PR-3 pattern).
6. [O] (T2) `drawAlpha` fade + `SetSortDist` → render-side (PR-3 pattern; two trivial rows, one PR).
9. [O] (T2) Draw-time `tempNum` scratch → dedicated scratch id space (design settled here; execution mechanical).
10. [O] (T2) Capstone: constify draw-pass access to sim objects — compiler-driven churn; requires 3–9 landed.
11b. [O] (T3) The §F relocations per 11a's table. Value now: honest profiler buckets.
14. [O] (T4) Drawer containers `CUnit*` → ID, one container family per PR (rules from 12/13).
16. [O] (T5) Hierarchical snapshot hashing + debug command, wired into `test/replay-rewind` (consumes 15; design spec'd above).
17. [O] (T5) Lua callout differential gate (test-only harness; consumes 15).

**Wave 3 — Fable integration (dependency-gated; in order)**
Just-in-time telemetry (Opus, deferred from the start of the plan — nothing in Waves 1–2 needs it):
1. [O] (T1) Boundary-size instrumentation: `transformsMemStorage`/`modelUniformsStorage` sizes + dirty rates, command-queue mutation rate, projectile churn; surfaced via `/boundarydump` (a `/profiledump` sibling — its own fixed-width per-frame row buffer, so full-game dumps stay small). The S0 dataset; must land before 19. **DONE 2026-07-05** (`BoundaryStats`, commit on `bruno/poc-split-sim-draw`). S0 numbers in `replay-seeking-architecture.md` "Sizes"; harness `test/s0-measurement/`. Verification: both S0 replays (44.5k frames each) resim sync-clean with instrumentation compiled in and armed (audit item 4); overhead over a fixed 8k-frame fast-forward segment: base 1.933 ms/frame, instrumented-idle 1.932 (counters unmeasurable), dump-active 1.946 (+0.7%, sampler self-measure 0.02–0.05 ms/frame).
2. [O] (T1) Draw-time callout census: per-callout call counts from draw callins (extends existing per-callin/callout profiler). Classification dataset for the boundary contract; must land before 18. **DONE 2026-07-05** (draw-callin context axis + `/calloutcensus` + `count_draw/count_sim` dump columns; same sync/overhead gates as PR 1). Census + classification takeaways in §E.1; a live-GL census remains recommended before finalizing PR-18 family order.

18. [F first family / O rest] (T5) Snapshot-serve hot callout families — positions family establishes the glue-redirection pattern (Fable; needs 15 + 17); remaining families follow (Opus), each validated by 17. With GL4 not coming first, the positions/piece-transform families carry the full draw-time load permanently — highest-priority serving work.
19. [F] (T6) Stream format + headless index-pass writer — encoding decisions (piece-pose quantization, projectile spawn-param re-derivation) are open design problems; gated on Wave-0 S0 numbers. (S0–S4 stages defined in `replay-seeking-architecture.md`.)
20. [F] (T6) Stream-fed render mode — the big integration; needs Track 2 complete + 12/13/14.
21b. [O] (T6) Reset-hook implementation from 21a's inventory.
22. [F] (T6) Seek UX + background resim + feed swap — interpolation continuity across the swap is the subtle part; needs 19/20/21b.
23. [O] (T6) Live ring-buffer rewind (reuses everything above).

**Later — the thread split itself (T7, out of scope here).** Sync-compatible in principle (sim code unchanged, single-threaded); two risk points: streflop/FPU init on the sim thread must match master (copy `GameLoadThread` pattern), and net-message consumption order preserved bit-for-bit. Same demo gate applies; feature flag drops back to the single-threaded path.


## Key files

`rts/System/SpringApp.cpp` (871-932), `rts/Game/Game.cpp` (1370, 1418, 1640, 2000-2138), `rts/Net/NetCommands.cpp` (161-267, 593-618), `rts/Net/GameServer.cpp` (263, 2630-2724), `rts/System/Platform/Threading.{h,cpp}` (30-37, 461-506), `rts/System/{GameLoadThread.cpp,LoadLock.h,Threading/WrappedSync.h,Threading/ThreadPool.h}`, `rts/Rendering/Common/ModelDrawerData.h` (82, 116-225), `rts/Rendering/Units/UnitDrawerData.cpp` (179-451, 589-723), `rts/Rendering/Features/FeatureDrawerData.cpp` (94-217), `rts/Rendering/Env/Particles/ProjectileDrawer.cpp` (377-421), `rts/Rendering/ModelsDataUploader.cpp`, `rts/Map/ReadMap.cpp` (69-96, 496-514), `rts/Game/TraceRay.cpp` (352-495), `rts/Lua/LuaHandleSynced.h` (180-268), `rts/Lua/LuaHandle.cpp` (2727-2909), `rts/Lua/LuaSyncedRead.cpp` (537-544), `rts/Sim/Objects/{WorldObject.h,SolidObject.h}` (67-104, 431-434).
