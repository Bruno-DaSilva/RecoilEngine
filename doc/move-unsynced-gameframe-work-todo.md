# Move unsynced rendering work out of G:GameFrame into Update/draw frames

This doc covers two layers of the same misplacement pattern: game-side Lua gadget bodies running in `GameFrame` (the original scope, first sections), and the engine-side misplaced-work block inside `CGame::SimFrame` (PR 11a classification, at the end).

## Problem
Some `G:GameFrame` (gadget callin) bodies do unsynced *rendering* work — e.g. "Nano Particles GL4" building/uploading GL4 instance buffers. `GameFrame` fires from `eventHandler.GameFrame` inside `Sim::GameFrame` (under the `Sim` timer in `CGame::SimFrame`) for **both** synced and unsynced gadgets, so this render-prep work inflates sim-frame time and runs at sim rate even though it only needs to be ready at draw time.

## Goal
Relocate the unsynced/rendering portion of these callins to an unsynced per-render-frame callin (`Update`) so it leaves the sim critical path, then measure the sim-frame improvement.

## Status — Nano Particles GL4 (done)
Moved the whole per-tick refresh (emit/cull/homing/clamp + VBO upload) from `gadget:GameFrame(n)` to `gadget:Update()`, gated to run at most once per new sim frame. Works at 1×/3×/6× and under pause after the fixes below. The gadget is unsynced-only (`if gadgetHandler:IsSyncedCode() then return end` at the top), so the move kept the same Lua state — see the **synced gadgets** caveat before copying this to a synced one.

## Status — Fire GL4 + Fire & Smoke GL4 (done, code-side; in-game retest pending)
Both are unsynced-only. Moved `gadget:GameFrame(n)` → `gadget:Update()` with the gate/`bodyTick`/`crossed()` scaffolding on a stable `U` table (both files were near the local limit). `DrawWorld` already drew. Syntax verified with `luajit -bl`; the 1×/3×/6×/pause behaviour + DEBUG-readout retest still needs a running game (no build was run).

What differed from Nano, worth remembering:
- **No cull-deadlock to fix.** Both already range-swept expiry: `removeExpiredParticles` walks `[lastRemovedFrame+1, gameFrame]` via its own `lastRemovedFrame` tracker (caps: fire 600, smoke 300), so it's jump-safe without `prev`. Only the emit/poll scheduling needed converting. (Nano's `cullDead` was exact-frame-keyed and *was* the deadlock — these weren't.)
- **`cachedGameFrame` staleness.** Both stamp emitter/particle timestamps from a cached frame that `GameFrame` used to refresh every sim frame. With the body on `Update` (draw-rate), event callins that create emitters would read a stale frame under fast-forward. Fixed by `cachedGameFrame = spGetGameFrame()` at the top of the engine event callins (`UnitDamaged`/`UnitDestroyed`/`RecvFromSynced`) and the public `GG.Fire*`/`GG.FireSmoke*` API entry points (callable from any other gadget's callin). Hot per-particle reads still use the cached value set once in `Update`.
- **Fire GL4 needed emission integration** (no built-in fast-forward degradation): each emit pass scales its count by `emitScale = n - prev` — the sim frames *that body run* covered — clamped 1..8, threaded through `updateEmitters`/`emitFromEmitter`/`emitTreeFire`, so density holds at 3×/6×. Its `fpsUpdateInterval` emit throttle moved onto `bodyTick`; the wind poll moved to `crossed(10)`; the FPS poll was already a `lastFpsCheckFrame` threshold (jump-safe), left as-is.
  - **Subtlety: use `n - prev`, not `n - lastEmitFrame`.** `n - prev` scales only for the fast-forward jump within one body run; it deliberately does NOT span the body runs the `fpsUpdateInterval` gate skipped. Dropping those skipped frames' share *is* the low-FPS throttle (effective rate stays `rate / fpsUpdateInterval`), so old behaviour is preserved exactly in every regime — full rate per sim frame at good FPS (any speed), and thinned below ~40 FPS. An earlier attempt used `n - lastEmitFrame` (frames since the last actual emit), which integrated across the skipped runs and silently neutralised the throttle (full density at low FPS, more draw cost while already struggling). The FPS throttle's rationale survives the move — unlike a *game-speed* throttle (playbook concern #2), its pressure (low frame rate) is still real in `Update`.
- **Fire & Smoke GL4 intentionally degrades at speed** (forces the lowest quality preset while `fastForward`), so emission was *not* integrated — that would fight the author's intent. Kept per-call trail/emitter semantics (matches old behaviour exactly at 1×/good-FPS; the existing fast-forward thinning is preserved). Converted scheduling only: `updateMaxParticles`/wind/cleanup → `crossed(90)`/`crossed(10)`; quality sampler `% 4` and emit interval → `bodyTick`; removed the now-redundant internal `% K` self-gates from `updateMaxParticles`/`updateWind` (the caller gates them). Its debug readout is `local debugEcho` (flip to retest); Fire GL4 has no debug flag.

## Playbook for future gadgets

The move itself is one line of intent — *run the body from `Update`, gated to once per sim frame*:

```lua
function gadget:Update()
    local n = spGetGameFrame()
    if n <= (U.gateFrame or -1) then return end   -- gate FIRST (pause-safe)
    local prev = U.gateFrame
    if prev == nil or prev < 0 then prev = n - 1 end
    U.gateFrame = n
    ...
end
```

But `Update` differs from `GameFrame` in three ways that silently break a body written for `GameFrame`. All three bit us on Nano:

1. **`Update` runs at draw rate, and `GetGameFrame()` can jump by >1 per body run.**
   `Update` fires once per *draw* frame (your FPS, often 100s/s); the gate collapses that to one body run per new sim frame. But when the sim outruns draw (fast-forward, `/speed`, reconnect catch-up), several sim frames pass between draws, so `n` advances by `dN > 1` per body run instead of `+1`.
   - **Frame-modulo schedules** (`if n % K == 0`) get stepped over and silently stop firing. Fix: fire on boundary *crossings* — `floor(n/K) ~= floor(prev/K)`.
   - **Per-tick accumulation** hardcoded to "one frame's share" undercounts by `dN×`. Fix: integrate over frames actually elapsed. If the work is itself strided/skipped per item, track elapsed *per item* (`frame - item.lastVisitFrame`), not a global `dN`, and clamp it so a long gap can't dump one spike.
   - **Exact-frame-keyed event buckets** (e.g. `deathBuckets[deathFrame]`, timed queues processed as `bucket[frame]`) get stepped over too — and this one *deadlocks*, it doesn't just degrade. Skipped buckets are never drained, so the resource they hold (here: particle VBO slots) is never freed; the pool fills, the saturation gate pins emission off, and everything stalls. Fix: **sweep the whole elapsed range** `(prev, n]`, not just `bucket[n]`. This was the bug that made Nano vanish entirely above 1× — `cullDead` only popped `deathBuckets[frame]`, so under fast-forward dead particles accumulated to the cap and froze the gadget.
   - **Cost-amortization throttles** ("run this expensive pass every Kth frame", "visit every Kth item", strided cosets via `floor(frame/K)`) fire irregularly when `frame` jumps — bunching or skipping the passes. These usually don't break correctness (slot freeing aside), just smoothness/fairness. Fix: key them off a **body-run counter** that increments +1 per body run (a `tick`), not the sim frame `n`. "Every Kth body run" stays an even 1-in-K rhythm at any speed, and is simpler to reason about. Keep `n` for the *content* (timestamps, death frames, per-item elapsed); use `tick` only for *cadence*. On Nano the scan `runEvery`/`stride`, homing re-aim (`% HOMING_RUN_EVERY`), and ground clamp (`% RUN_EVERY`) were all moved onto `tick` this way.

2. **Speed-based throttles written for `GameFrame` become wrong.**
   A throttle that ramped work down at high game speed usually existed because `GameFrame` ran `speedFactor×` per wall-second and threatened the Lua budget. Under `Update` the body is draw-rate-bound regardless of sim speed, so that pressure is gone — but the throttle is still cutting output. Remove/neutralize it; bound resource use with the normal capacity gate instead. (On Nano this throttle was exactly what made particles vanish from ~3× up.)

3. **Pause = 0 sim frames, but `Update` keeps firing.**
   When paused, `GetGameFrame()` is frozen, so the gate's `n <= gateFrame` stays true and the body does nothing — *provided the gate is the first statement*, before any `dN` math. This matches old behavior (`GameFrame` didn't fire while paused). Drive animation off engine time uniforms (`timeInfo` = gameFrame + interp offset) so visuals freeze with the sim rather than vanish; the `Draw*` callin still renders the last state every frame.

### Other rules
- **Synced gadgets:** if the gadget runs in synced code, its upvalue state lives in the *synced* Lua state; `Update` runs in the *unsynced* state with separate upvalues. You can't just rename `GameFrame`→`Update` — either keep minimal synced bookkeeping in `GameFrame` and move only the GL build/upload to `Update`/`Draw*` (re-reading via unsynced-safe queries), or split state explicitly. Nano sidestepped this by being unsynced-only.
- **Confirm nothing synced moved** (no sync-hash effect). Pure visuals only.
- **Diagnose cadence by counting two rates:** callin entries (before the gate) = draw FPS, expected and cheap; body runs (after the gate) = should be ~sim rate. "Update runs too fast" is usually the former (a non-bug); a gate leak shows up as body-runs ≈ draw FPS.

## Benchmark
- [ ] `Sim::GameFrame` / overall `Sim` time before vs after, particle-heavy scene (Tracy + CTimeProfiler).
- [ ] Draw-frame cost added by the moved work (should land in `Update`/`Draw`).
- [ ] Headless fast-forward for a repeatable workload — see [[project_headless_speed_control]].
- [ ] Re-check density/visuals at 1× / 3× / 6× and under pause (the regressions above only show off-1×).

## Notes
- Mostly game-side Lua, but the benchmark target is engine sim-frame time. Engine anchors: `eventHandler.GameFrame` (`Sim::GameFrame`, sim timer) vs the unsynced `eventHandler.Update()` in `CGame::UpdateUnsynced`.
- Aligns with prior profiling: sim-side Lua glue is cheap; the cost is unsynced callout bodies — see [[project_lua_boundary_profiler_findings]].

---

# PR 11a — classification of the SimFrame misplaced-work block (engine-side, audited 2026-07-05)

This is sync-audit item 3 of `sim-draw-thread-decoupling-research.md` ("each §F relocation item needs a touches-synced-state/RNG check before moving"). It classifies every call in the misplaced-work block plus adjacent per-frame work, before any relocation. **Doc only — no code has moved.** PR 11b performs the relocations per this table.

## The block

`rts/Game/Game.cpp:1764-1780` (the research doc's `Game.cpp:2046-2062` citation is stale — that range is now `CGame::StartSkip`). The block sits inside `ENTER_SYNCED_CODE()` (`Game.cpp:1719`), after `gs->frameNum += 1` (`Game.cpp:1729`), and **before** the `SCOPED_SPECIAL_TIMER("Sim")` section (`Game.cpp:1784`), under the comment "everything here is unsynced and should ideally moved to Game::Update()" (`Game.cpp:1765`). That comment is wrong for one item (`eoh->Update`, see below). The whole block is gated on `if (!skipping)`; note `StartSkip` is `#if 0`'d (`Game.cpp:2057`), so `skipping` is constant-false in practice — movers should still preserve the gate.

## Verdict vocabulary

- **SYNCED (stays)** — remains in `SimFrame` (on the sim thread after the split); the "misplaced" comment is corrected instead.
- **UNSYNCED-move-to-boundary** — unsynced effects, but reads (or mutates state consumed by) live sim structures, so post-split it must run at the sim-quiescent extract boundary, once per sim-frame *batch*. In today's single-threaded tree the anchor is the `newSimFrame`-gated section of `CGame::UpdateUnsynced` (`Game.cpp:1275`, `Game.cpp:1384`), which runs after `ClientReadNet` has drained its frame budget.
- **UNSYNCED-move-to-draw-update** — touches only draw/UI-side state; may free-run at draw rate.

Batch-cadence hazard, common to all movers: today each item runs exactly once per sim frame; at the boundary it runs once per *batch* of sim frames (>1 under fast-forward/catch-up). Any frame-modulo or exact-frame-keyed logic must convert to boundary-crossing / range-sweep form — this is the same hazard playbook as the Lua section above (items 1 and "exact-frame-keyed event buckets" especially).

## Classification table — the block itself

| # | Item (call site) | Reads | Mutates / sends | Synced state or gsRNG? | Verdict |
|---|---|---|---|---|---|
| 1 | `waitCommandsAI.Update()` (`Game.cpp:1766`) | `gs->frameNum` cadence gate (`WaitCommandsAI.cpp:106`); wall clock for unacked-wait expiry (`WaitCommandsAI.cpp:122-128`); unit command queues via `unit->commandAI->commandQue` (`WaitCommandsAI.cpp:396-418`, from the per-wait `Update`s at `:575`, `:745`, `:934`, `:1066`); `gs->frameNum` for TimeWait deadlines (`:580-584`) | Own `waitMap`/`unackedMap`; selection save/clear/restore (`WaitCommandsAI.cpp:447-461`); releases waits by sending `CMD_WAIT` through `selectedUnitsHandler.GiveCommand(cmd, false)` (`:442`, `:455`) → `SendCommand` → `clientNet->Send` (`SelectedUnitsHandler.cpp:259`, `:1032-1036`); side effect: unit-reply sound sample (`SelectedUnitsHandler.cpp:261-265`) | **No synced writes, no gsRNG** (file has zero RNG references). Sim influence is exclusively the same net path as user input; commands apply only when the packet round-trips through the server. Reads synced state (queues) read-only | UNSYNCED-**move-to-boundary** |
| 2 | `geometricObjects->Update()` (`Game.cpp:1767`) | `gs->frameNum`, exact-key lookup in `timedGroups` (`GeometricObjects.cpp:135`; groups registered at `frameNum + lifetime`, `:66`, `:126`) | Erases `timedGroups` entry (`:144`); `DeleteGroup` erases `geoGroups` and sets `deleteMe = true` on each `CGeoSquareProjectile` (`:74-82`) | **No.** Geo projectiles are constructed unsynced (`GeoSquareProjectile.cpp:30`, isSynced=false) → unsynced projectile container, no RNG consumed, no synced-ID interaction (`ProjectileHandler.cpp:418-426` — only the synced branch touches RNG/ASSERT_SYNCED). All creators are unsynced/debug-gated: AI debug figures (`AICallback.cpp:1047-1063`), `drawDebugTraceRay` (`MissileLauncher.cpp:165-167`), `ENABLE_PATH_DEBUG` (`HAPFS/PathFinder.cpp:523-525`), `DEBUG_AIRCRAFT` (`StrafeAirMoveType.cpp:550-561`), `DEBUG_DRAWING_ENABLED` (`GroundMoveType.cpp:1949-1953`) | UNSYNCED-**move-to-boundary** |
| 3 | `sound->NewFrame()` (`Game.cpp:1768`) | Nothing (`Sound.cpp:1030-1036`) | Resets `emitsThisFrame = 0` on the four global channels (`IAudioChannel.h:65`); that counter caps sounds started per frame (`AudioChannel.cpp:93-97`; default cap 1000, `IAudioChannel.cpp:8`, games lower it via `SetMaxEmits`) | **No.** Pure audio-engine field, no synced reader, no RNG, no net | UNSYNCED-**move-to-boundary** (gated to sim-frame advance — see constraints) |
| 4 | `eoh->Update()` (`Game.cpp:1769`) | Dispatches `Update(gs->frameNum)` to all locally-hosted skirmish AIs (`EngineOutHandler.cpp:128-131`); AIs synchronously call back into the full `AICallback` read surface over live synced objects; early-out when no AIs (`EngineOutHandler.cpp:80-83`) | AI orders go over the net (`clientNet->Send(...SendAICommand...)`, `AICallback.cpp:351-369`; likewise share/startpos/mapdraw `:127-129`, `:180-219`, `:1404-1416`). **But cheat callbacks mutate synced state directly**: `SetMyIncomeMultiplier` (`AICheats.cpp:53-59`), `GiveMeMetal` — `team->res.metal += amount` (`:61-67`), `GiveMeEnergy` (`:69-75`), `CreateUnit` via `unitLoader->LoadUnit` (`:77-89`) | **Yes (conditionally).** The direct mutations are gated by `OnlyPassiveCheats()` (`AICheats.cpp:34-43`), false only when we are host *and* sole player — so they cannot desync MP, but in single-player they are real synced mutations whose in-frame position is defined by this call site. AIs additionally assume once-per-sim-frame `Update(frame)` cadence (frame-keyed schedulers in AI libs) | **SYNCED (stays in SimFrame)** — the `Game.cpp:1765` "everything here is unsynced" comment must be corrected |
| 5 | `uiGroupHandlers[t].Update()` (`Game.cpp:1771-1772`) | Per group `CGroup::Update()` = `RemoveIfEmptySpecialGroup()` (`Group.h:38`); `changedGroups` list (`GroupHandler.cpp:48-71`) | UI group containers only — `groups`/`changedGroups`/`freeGroups`/`unitGroups`, the latter a `spring::unsynced_map` (`GroupHandler.h:91`); fires `eventHandler.GroupChanged` (`GroupHandler.cpp:67-69`), an `UNSYNCED_BIT` event (`Events.def:128`) | **No.** Group membership has no synced readers: `Spring.GetUnitGroup` exists only in LuaUnsyncedRead (`LuaUnsyncedRead.cpp:273`, `:4371-4387`), nothing group-related in LuaSyncedRead; `CUnit::GetGroup/SetGroup` proxy into the unsynced map (`Unit.cpp:1968-2003`); `noGroup` is commented UNSYNCED (`Unit.h:540`). No net, no RNG | UNSYNCED-**move-to-draw-update** |
| 6 | `fpsController.SendStateUpdate()` (`Game.cpp:1774-1777`) | `gu->fpsMode` early-out (`FPSUnitController.cpp:112`); camera move state (`:115`), mouse buttons (`:116`), camera dir (`:126`) — all draw-side input state | Delta-latch `oldHeading/oldPitch/oldState` (`:128-131`; declared unsynced, `FPSUnitController.h:52-54`); sends `NETMSG_DC_UPDATE` only when input changed (`:133`) | **No.** The synced controller fields (`FPSUnitController.h:41-50`) are written only by the *receive* path (`RecvStateUpdate`, `FPSUnitController.cpp:89-108`, via server rebroadcast `GameServer.cpp:1442-1461` → `NetCommands.cpp:1466-1479`), and the synced sim applies the latched state every frame regardless of send rate (`Player.cpp:147-154` from `Game.cpp:1833`) — send rate affects responsiveness only | UNSYNCED-**move-to-boundary** (draw-rate viable later, see constraints) |
| 7 | `CTeamHighlight::Update(gs->frameNum)` (`Game.cpp:1779`) | `frameNum % TEAM_SLOWUPDATE_RATE` gate (`TeamHighlight.cpp:63`; rate=30, `GlobalConstants.h:67`); config + `gu` flags (`:68-69`, `:83`); synced team fields read-only (`t->gaia/isDead/GetNumUnits/HasLeader`, `:75-79`, `:105`); player ping (`:92-101` — client-divergent, but feeds only unsynced output) | `t->highlight` (`TeamHighlight.cpp:112`) — declared `/// unsynced` (`Team.h:102-103`) and `CR_IGNORED` (`Team.cpp:56`); static `CTeamHighlight::highlight` (`:116`), consumed only by draw-path `Enable/Disable` (`Game.cpp:1526`, `:1574`) | **No.** No synced reader of either mutated field; no RNG, no net. (`t->color` is *not* touched by Update — only by draw-path Enable/Disable) | UNSYNCED-**move-to-boundary** |

## Adjacent per-frame work audited (outside the `!skipping` block)

| Item (call site) | What it is | Verdict |
|---|---|---|
| `spring_lua_alloc_update_stats((gs->frameNum % GAME_SPEED) == 0)` (`Game.cpp:1762`) | Once per sim-second, zeroes the atomic Lua-allocator counters (`LuaUser.cpp:335-339`); diagnostics only | **Stays** — thread-safe atomics, sim-second cadence is the semantic, cost is two stores; nothing to gain by moving |
| `DumpRNG(-1, -1)` (`Game.cpp:1722`) | Sync-debug tooling (gsRNG state dump) | **Stays** — sync tooling by definition sim-side |
| `gu->avgSimFrameTime` update + `DbgTimingInfo(TIMING_SIM, ...)` (`Game.cpp:1837-1841`) | Measures the sim frame just executed | **Stays** — measurement of sim must bracket sim |
| `CTimeProfiler::DumpFrame` + `BoundaryStats::SampleFrame` (`Game.cpp:1846-1848`) | Per-sim-frame sampling for `/profiledump` and `/boundarydump` | **Stays** — per-sim-frame instrumentation by design |
| `CUnitDrawer::UpdateGhostedBuildings()` (`Game.cpp:1829`, comment `:1826-1828` "should probably be split from drawer") | Inside the *Sim* timer block: prunes render-owned dead-ghost containers (`savedData.deadGhostBuildings`) when their position gains LOS; reads `losHandler->InLos` (`UnitDrawerData.cpp:235-255`), mutates only draw-side state | UNSYNCED-**move-to-boundary** — §A-adjacent (draw-owned state currently updated at sim time). Ordering: run after the render-event delta queue drain for the batch, so ghosts created by the batch's destroy events exist before pruning; LOS read at end-of-batch is equivalent to today's post-`losHandler->Update` read (a synced-Lua `SetUnitLosState` in `GameFramePost` could differ by a frame-fraction — visual-only) |

Not misplaced (spot-checked while auditing): everything inside the `SCOPED_SPECIAL_TIMER("Sim")` section other than `UpdateGhostedBuildings` (`helper`, `readMap`, `mapDamage`, `unitHandler`, `pathManager`, `projectileHandler`, `featureHandler`, `losHandler`, `interceptHandler`, team/player `GameFrame`s) is synced simulation and stays.

## Constraints for PR 11b (the movers)

Ordering among movers: **none load-bearing** — the five movers are mutually independent. Two soft notes: (a) keep the current relative order `waitCommandsAI` → `sound->NewFrame` within the boundary step if exact sound-budget accounting is to be preserved (a wait-release plays a unit-reply sample, which today lands in the *previous* frame's emit budget); (b) `UpdateGhostedBuildings` after the render-event queue drain (see its row).

Per-item cadence/gating constraints:

1. **waitCommandsAI** — convert the `(frameNum % 3)` gate (`WaitCommandsAI.cpp:33`, `:106`) to a boundary-crossing check (`floor(n/3) != floor(prev/3)`); keep the `!skipping` gate. Under fast-forward a wait release can go out a few frames later than on master — net-side latency only, same class as user-input latency, no sync impact. The inbound notifications from synced code (`CommandAI.cpp:999`, `:1081`, `:1089`, `:1270`, `:1388`; `FactoryCAI.cpp:195`, `:206`, `:233`; `Factory.cpp:273`; `SelectedUnitsAI.cpp:125`, `:267`) stay where they are — they only mutate waitCommandsAI's own maps; post-split they become cross-thread and need the event-queue treatment (Track-4 scope, not 11b).
2. **geometricObjects** — the expiry map is exact-frame-keyed (`timedGroups.find(gs->frameNum)`, `GeometricObjects.cpp:135`); at batch cadence this *skips* buckets and leaks groups (the "exact-frame-keyed event buckets" deadlock class from the Lua playbook above). The mover must sweep all frames in `(prevFrame, frameNum]` (or switch the container check to `<= frameNum`). `deleteMe` is consumed by `projectileHandler.Update()` inside SimFrame (`Game.cpp:1808`), which is why this is boundary (sim-quiescent), not free-running draw.
3. **sound->NewFrame** — gate the reset to sim-frame advance at the boundary. Raw draw-rate resets would raise the effective sounds/sec wherever a channel's `SetMaxEmits` cap binds (audible change); one reset per batch under fast-forward slightly *tightens* the cap (N frames share one budget) — accepted, visual/audio-only.
4. **uiGroupHandlers** — no frame-keyed logic; safe at draw rate. Sim-side events (unit death/transfer → `SetGroup`) already write into these containers from sim context; post-split those become cross-thread writes needing the event queue (note for Track 4, fine single-threaded).
5. **fpsController.SendStateUpdate** — move to the boundary gated on sim-frame advance for a behavior-preserving 11b (packet cadence stays ≤1 per sim frame). Free-running draw-rate is a viable *improvement* (fresher FPS-mode input, packet rate still bounded by the delta gate `FPSUnitController.cpp:128`) but is a behavior change — separate decision, not 11b. Keep the `!skipping` gate.
6. **CTeamHighlight** — convert the `% TEAM_SLOWUPDATE_RATE` gate to a crossing check (same pattern as item 1).
7. **eoh->Update stays**; 11b should instead split the `Game.cpp:1765` comment so it no longer claims the whole block is unsynced, and leave `eoh->Update()` (plus its `!skipping` gate) in place. If it is ever to move, it needs a snapshot-backed AI callback surface and a net-only cheat path first — out of scope for this track.

Sync-compat argument for the whole of 11b: items 1-3 and 5-7 write no synced state and consume no gsRNG (table above); their only sim-facing effects are net messages, which are timing-equivalent to user input. Item 4 (the one with a synced-mutation path) does not move. Therefore the master-demo resim gate must pass bit-identically — any DESYNC after 11b is a bug in the relocation, not an accepted deviation.
