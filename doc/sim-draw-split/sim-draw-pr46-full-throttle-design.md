# PR 46 — sim-thread full throttle (pacing-limit removal under the split)

Written 2026-07-10. Status: LANDED on `bruno/poc-split-sim-draw` (main checkout), build-verified; gates per the operator's ruling deferred (build-only verification requested).

## Problem

Profiling showed the split sim thread idling between bursts. Three pacing layers survived from master, all designed for the pre-split world where sim and draw shared one thread:

1. **Client consumption budget** — `msgProcTimeLeft` (each NEWFRAME costs 1000, `NetCommands.cpp`) plus the per-call wall cap `GetNetMessageProcessingTimeLimit()` ("balance the time spent in simulation & drawing"). Exhaustion → 1 ms `SimIdleWait` naps between bursts.
2. **Server CPU-usage speed controller** (`CGameServer::LagProtection`) — throttles `internalSpeed` to hold the reference client's reported CPU at 0.75 (speedcontrol max policy) / 0.60 (median), plus the `#ifndef DEDICATED` `maxSimFrameRate` clamp derived from `minSimDrawBalance`. The sim was *designed* to idle 25–40% of wall time.
3. **Client CPU report** (`CGame::SendClientProcUsage`) — folds draw usage into the reported number (`Draw%/FPS × minDrawFPS`, badly inflated at low FPS), making servers throttle sim speed on draw load that no longer competes with the sim.

Under the split the sim owns its own core; these limits buy nothing and cap fast-forward well below sim capacity.

## Operator rulings (this session)

- **Bypass scope: SCOPED**, not unconditional. Full throttle applies when the split sim thread runs AND (local/replay server `gameServer != nullptr`, OR `gs->speedFactor > 1.01`, OR `IsSimLagging()`) — the same carve-out family as `CanConsumeSimFrameNow`'s free-run set. Remote-server 1x play keeps master's budget pacing (the net-smoothing trailing buffer absorbs link jitter; the sim is supply-bound at 30 frames/s there, so bypassing gains nothing).
- **CPU report change: INCLUDED** (the only MP-visible piece; deviation D1 below).

## What changed

### 1. Client consumption loop (`CGame::SplitFullThrottleConsume`, Game.h / NetCommands.cpp / Game.cpp)

Under full throttle, `ClientReadNet`'s budget/wall exits are replaced by:

- **Epoch-consumed exit**: break when a NEWFRAME was consumed this pass AND `simSnapshot.NewestEpochConsumed()` — returning then lets `SimThreadProc`'s `ProduceEpochAtSimEdge` publish immediately (the §3.2 pacing gate cannot skip), so epoch publish cadence tracks the draw acquire rate with zero sim idle.
- **250 ms hygiene backstop** (matches the epoch `timeDue` fallback): guarantees the `SimThreadProc` loop top runs — `Watchdog::ClearTimer(WDT_SIM)` (60 s HangTimeout vs ≤250 ms cadence: no per-packet clear needed), `YieldIfPauseRequested`, and `deferredObjectDeleter.ServiceRetiredReleases()` (p0 — otherwise retired pool pages starve and the valve engages). Counted (`ffBackstopExits`, below).
- **Deficit pin**: `msgProcTimeLeft = max(msgProcTimeLeft, 0)` each bypassed iteration. NEWFRAME still costs 1000; without the pin a long FF burst drives the budget thousands of frames negative and the sim stalls for seconds when dropping back to master pacing (accrual is ~30 units/ms).

`SimThreadProc`'s hot-continue drops the budget term under full throttle but requires **frame progress** across the pass (`gs->frameNum` advanced): a ring-blocked exit at 1x (draw slower than sim, PR 44c backpressure) or a dry queue naps 1 ms instead of busy-spinning a core against the gate.

The split-only per-packet exits (PauseRequested, `CanConsumeSimFrameNow`) are untouched — they are correctness gates, not pacing. Pre-spawn lockstep (flag on, sim thread not yet running) keeps master pacing: `ClientReadNet` still shares the main thread with rendering there, so master's caps ARE the draw-starvation protection.

### 2. Server speed controller (`CGameServer::HasRemotePacedClient`, GameServer.h/.cpp)

Inside `LagProtection`'s speed-adjust block (AFTER the per-player `SendPlayerInfo` broadcast and `isReconn` maintenance, which keep running): when `SimDrawSplit::Enabled() && !HasRemotePacedClient()`, snap `internalSpeed` to `userSpeedFactor` and return — skipping both the CPU-based reduction and the `maxSimFrameRate` clamp.

- `HasRemotePacedClient()` mirrors LagProtection's counting rule (`INGAME && !isLocal && (demoReader ? !isFromDemo : !spectator)`) — a live spectator joining a hosted replay flips it true and protection resumes, both ways.
- The explicit snap-up is required: `UserSpeedChange`'s insta-raise does not cover raising speed while already throttled below the OLD `userSpeedFactor`. Worst-case restore latency: one LagProtection tick (2 s).
- `SimDrawSplit.h` is self-contained (`<atomic>`/`<cstdint>`); in DEDICATED builds the flag is constant-false (only `CGame`'s ctor sets it), so dedicated behavior is untouched.

### 3. CPU report (`SendClientProcUsage`, NetCommands.cpp)

Under `Enabled() && SimThreadRunning()`, send `simProcUsage` (the "Sim" scope's wall fraction ≈ sim-thread busy share under the split) instead of `totalProcUsage` (which folds in draw). Flag-off value bit-identical.

### 4. Telemetry

`SimDrawSplit::g_ffBackstopExitCount` (beside `g_ringBlockCount`), teardown-logged as `[BackpressureStats] ffBackstopExits=N` (zero-suppressed). Small counts under FF are normal (draw hitches); huge counts mean the draw side stopped acquiring epochs — the first thing to check if FF draw-FPS regresses.

### 5. Sim-thread core reservation (the for_mt straggler fix)

Diagnosed from the operator's Windows Tracy capture: `CUnitScriptEngine::Tick(MT)` fanned out normally, then ONE worker ran a single ~22ms `TickAllAnims` every sim frame (≈ a Windows scheduler quantum). Root cause: the pool topology predates the split — Main gets a dedicated core and every remaining core in the mask gets a pinned worker, but the split's sim thread has NO affinity: it floats over the pinned workers' cores (Linux: inherits the spawning main thread's mask) and preempts whichever worker it lands on. `for_mt` distributes chunks via an atomic counter, so the whole group (and the sim frame — the caller spins in `WaitForFinished`) waits on the preempted worker's in-flight chunk. Tracy's own client thread is a second unpinned floater, worsening it while profiling.

Fix (`ThreadPool.cpp::SetDefaultThreadCount`, `Threading::{Set,Get}ReservedSimAffinityMask`, `SimThreadProc`): when the split is configured on AND the per-perf-core pin policy is active AND the worker count is AUTO (`WorkerThreadCount < 0`): spawn one fewer worker, size the cache-group mask for busy-threads+1, carve the top core out of `workerAvailCores`, store it as the reserved sim mask, and the sim thread pins itself to it at spawn (`[ThreadPool] Sim thread affinity reserved as 0x…` / `[Threading] Sim thread CPU affinity mask set: 0x…` in the infolog). An explicit `WorkerThreadCount` is respected verbatim (no reservation — the old layout). Flag-off: no reservation, byte-identical topology.

No-rebuild mitigation on any older split build: set `WorkerThreadCount` to (current effective − 2), freeing cores for the sim + Tracy threads.

## Explicitly KEPT (bounds that are not CPU pacing)

- PR 44c epoch-ring backpressure (`CanConsumeSimFrameNow`: sim ≤ N−1 unretired epochs ahead at interpolated 1x; free-run carve-outs under FF/catch-up/capture).
- The §3.2 one-unconsumed-epoch extraction gate (extraction economy, not frame pacing).
- Demo queue-depth gate (`modGameTime` halts when the local client is ≥ `GAME_SPEED` frames behind, GameServer.cpp `Update`) — bounds replay backlog to ~1 s of frames; with the controller skipped, effective FF speed = min(wanted speed, sim capacity).
- Live `maxNewFrames` backlog cap in `CreateNewFrame` (host-local queue-depth bound).
- Setup min/max user speed clamps.
- `/skip` is dead code (`#if 0` bodies) — untouched.

## Enumerated deviations (advisory)

- **D1 (MP-visible):** split clients report sim-only CPU to servers — remote/dedicated servers throttle less for draw-bound split clients. Flag-off report unchanged.
- **D2:** local/replay/SP FF speed is now bounded by sim capacity + queue-depth gates instead of the 60/75% CPU target and the `maxSimFrameRate` clamp.
- **D3:** FF epoch publish cadence tracks the draw acquire rate (the "one extraction per draw frame" cost model was already the design; this makes it exact with zero sim idle between).

## Escape hatch

If headful FF draw-FPS regresses (publish-per-draw-acquire making extraction truly per-draw-frame), the knob is the exit predicate — require ≥K frames consumed before honoring `NewestEpochConsumed()`. Shipped without it.

## Producer-cost work (same session, after the first FF measurements)

With the limits removed, epoch production became the visible sim-thread cost: measured per epoch MapMirrors 6.65ms, Pieces 1.65ms, CmdQueues 0.36ms — 3–4× the sim frames served. Two changes landed:

**Blocking-mirror rect-incremental (the MapMirrors bulk).** The blocking-map mirror re-walked every map square (mapx×mapy, per-occupied-cell dynamic_cast, two whole-map assigns) on every drain, since blocking churns every frame. Now: `MarkBlockingDirty(x1,z1,x2,z2)` — the three GroundBlockingObjectMap choke points pass their exact footprint rects into a serial-numbered, producing-thread-owned rect log (`DrawMapMirrors::blockingRects`); each ring slot's drain re-scans only the rects since its cursor; the fully-applied prefix prunes once all slots pass it. The whole-map walk survives as the fallback (slot's first drain, map resize, log overflow ≥65536 rects → `blockingVersion` bump). Rect-granularity was pre-sanctioned by the PR-28 header ("a later cost optimisation"); the armed `SnapshotDiffGate` mirror memcmp remains the deterministic detector for any missed rect.

**Producer instrumentation (Tracy zones + /debug timers, one per step).** `Sim::EpochProduce` (whole producer) with sub-timers `::PoolService`, `::CmdQueues`, `::Pieces`, `::MapMirrors` (+ `::MirrorLos`, `::MirrorBlocking` inside), `::Transforms`, `::Queries`, `::SyncedMirror`, `::EffectStage`, `::SealPublish`; row extraction split into `Update::SimSnapshot::{Units,Projectiles,Features,TeamsPlayersGlobals,DeadRows}`. The /debug frame grapher gained a cyan **Epoch** slice on the sim-thread row (`TIMING_EPOCH_PRODUCE`, emitted per publish) so producer time no longer reads as idle.

**Open (data-driven next steps):** if `MirrorLos` remains hot → LOS rect-granularity (AddCircle/AddRaycast bboxes, same log pattern) or read-set-driven layer serving (needs a first-touch staleness ruling).

## Producer-cost round 2 (post blocking fix; measured Pieces ~4ms, Units ~4ms MTPC mid-game)

**Rules-params version-skip (Units/Features row extraction).** The per-object FULL `modParams` map copies dominated `Update::SimSnapshot::Units` (string-keyed maps, per-node allocations). Now: `CSolidObject::modParamsVersion` — set to a fresh globally-unique serial by `BumpModParamsVersion()` at every mutation (choke: the `SetRulesParam` call sites in `LuaSyncedCtrl::SetUnitRulesParam` / `SetFeatureRulesParam`, audited as the only runtime writers); each ring slot stores the version its copy reflects (`unitRulesParamsVersion` / `featureRulesParamsVersion`) and skips the copy when unchanged. Version 0 (creation / creg-load default, deliberately unserialized — `CR_IGNORED`) always copies; global uniqueness makes id-reuse aliasing impossible. The unit pass moved into its own loop under `Update::SimSnapshot::UnitsRules` for attribution. Dead-row shell copies stay unconditional. The armed diff-gate rules-params field passes are the detector for a missed choke.

**Read-set-driven piece capture (operator-ruled).** `RefreshPieces` captured every piece of every active object per epoch (~45k piece captures; world-space fields defeat animation gating) while stock games query piece callouts rarely. Now: the producer captures ONLY registered ids (`unitPieceReadSet`/`featurePieceReadSet`, drained at each edge from a mutex-guarded draw→producer mailbox); dead ids prune, reused ids re-register. First touch of a valid-but-unregistered object serves via a counted lazy park (`SimPauseSite::PIECE_FIRST_TOUCH` in `[SimPauseSurvey]`) + inline live capture into the HELD slot (safe: the producer never writes the held slot; serving is main-thread-only). Rendering is unaffected (drawer poses flow through `ExtractTransformsAtSimEdge`, not this cache).
Enumerated deviations: (D4) a first-touch value is live-fresh rather than epoch-stale (SYNCED-mirror precedent); (D5) a unit that died after the epoch edge and is FIRST queried in that dispatch window serves a miss where the eager cache would have served its edge-time capture for one window. A game that hammers piece callouts over many objects converges to eager-capture cost (the read-set grows with real demand) — `[SimPauseSurvey] PIECE_FIRST_TOUCH` counts expose it.

**Thread-affinity straggler (TickAllAnims):** no code change — `ThreadPinPolicy = 3` ("Share Performance Cores", `THREAD_PIN_POLICY_ANY_PERF_CORE`) pins threads to the perf-core group instead of one core each; the per-core policy's sim-core reservation does not engage under it (everything floats, which is the point). If floating regresses cache locality, the hybrid (pin sim+main, float workers) is a small `ThreadPool.cpp` `AffinityFunc` change — not implemented.

## Verification status

Build-only per the operator's instruction ("don't worry about testing, just make sure it builds"). The full gate matrix for this change, when wanted: flag-OFF byte-identity (both replays, 0 DESYNC — pacing decides *when* consumption stops, not packet order, so synced state is untouched by construction); flag-ON headful full-length; FF wall-time A/B (expect sim-thread utilization →~100%); telemetry checks (`[BackpressureStats]` ringBlocked=0 under FF, `[PoolValveStats]`=0, ffBackstopExits sane); remote FF→1x transition smoke (deficit pin); replay + live spectator join (predicate flips both ways).
