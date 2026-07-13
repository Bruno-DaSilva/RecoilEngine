# PR 44c — RequestPause demoted to lifecycle-only + valve/backpressure rekey: as-landed design record

Status: landed 2026-07-10 on `epoch-integration` (3 sub-commits). Contract: `doc/sim-draw-pr43-44-contract-refresh.md` §3.4 (valve + backpressure), §3.5 (the RequestPause demotion), §5.13 (free-run carve-outs), §9 (the 44b ruling that deferred the full valve rekey + N-1 backpressure to 44c). Builds on the PR-43/44a/44b records and closes their 44c handoff items. This is the FINAL engine PR of the sim/draw decoupling program; what remains is the operator's live windowed skirmish (§7.8) and the end-of-stack TSan/DEBUG/multi-run matrix.

## 1. The valve conversion (§3.4) — sim waits for epoch retirement

Pre-44c the pool valve was main-thread-serviced: the sim parked mid-frame (`ParkAtValve`), and either `AcquireSimPause` or the Draw-top `ServicePoolValve` ran an in-place consume + force-produce + `ReleaseAcked` on the MAIN thread, then resumed the sim. PR 44c DELETES that service entirely (`ServicePoolValve`/`ServicePoolValveOnce`, the `AcquireSimPause` valve loop, `ParkAtValve`/`ParkedAtValve`/`ResumeFromValve`/`ResumeFromValveNoWait`). The new shape (`DeferredObjectDeleter::WaitForEpochRetirementAtValve`, sim thread, mid-frame):

1. `ServiceRetiredReleases()` — reclaim the pages of every epoch the draw side retired since the last check (pools are sim-owned; this IS the sim thread).
2. Break when headroom is back (`FreePoolHeadroom(kind) >= EMERGENCY_HEADROOM_PAGES`) or when nothing reclaimable remains anywhere in the pipeline (`outstandingShells == 0` — the pool is near cap with LIVE objects; the flag-off branch proceeds identically after its flush).
3. Otherwise publish the mid-frame tail as a NORMAL producer epoch (`CGame::ProduceEpochForPoolValve` → `ProduceEpochAtSimEdge(force)`; the §3.2 pacing gate still applies, so repeated rounds publish at the draw side's consume rate). When the tail is already sealed, the forced pass publishes an EMPTY retirement-advancing epoch — that is what un-sticks the "acked shells wait for their epoch's retirement, retirement needs a successor acquire" tailcase.
4. `SimDrawSplit::ValveParkWait()` — one park round: present as `PARK_VALVE` (satisfying any concurrent `RequestPause`), nap ~1ms, then HOLD parked while a pause is pending; `parkKind` returns to `PARK_NONE` before the loop goes active again. Only the sim itself ever resumes from the valve.

**The deadlock argument (§3.4), as landed:** the draw side never waits on the sim — the epoch acquire is non-blocking, and `RequestPause` is satisfied by an edge OR valve park. The sim's only waits are (a) the ring backpressure gate, released by the draw side's acquire retiring an epoch; (b) the valve wait, released by the draw side's ordinary consume → ack → next-acquire-retires flow returning pool pages; (c) the pause park, released by `ReleasePause`. (a) and (b) need nothing from the sim. The valve's progress chain is: forced publish → next Draw consumes + acks the batch → the Draw after that acquires the successor and RETIRES it → `ReleaseRetired` marks the shells releasable → the waiting sim frees them.

**New machinery:** `outstandingShells` (cross-stage atomic: ++ at `Park`, -- at `ReleaseSlot`) — the valve's reclaimable check; `pending`/`poisoned` are split across threads under the flip and the backlog can sit in sealed slot batches or the releasable list, so container emptiness is neither thread-safe nor complete. `[PoolValveStats]` teardown telemetry (engages / rounds / producePasses / totalWaitMs).

**Semantics notes (all 44b-precedented, not new deviation classes):** the valve's tail epochs are value-consistent mid-frame states dispatched through the NORMAL barrier with full epoch serving and id coverage (44b's valve already did this; only the producing thread changed, sim instead of main-with-parked-sim — strictly cleaner). Ctrl pokes drained by a forced mid-frame produce apply mid-frame (same class as 44b's valve). The 43-era "valve drops coverage refs unchecked" limitation is GONE — there is no valve dispatch path anymore.

**Enumerated deviation (flagged for the operator):** a lifecycle park that catches a valve-parked sim gets MID-frame quiescence. Pre-44c, `AcquireSimPause` serviced the valve in place and waited for the frame edge, so lifecycle parks (saves/checkpoint serialize included) always saw a frame-edge sim — except when engaged inside a dispatch window, where the same mid-frame form already applied (44b's lazy park took valve-parked quiescence as-is). Waiting for the edge instead would deadlock (the sim cannot reach its edge without pages; pages need Draw frames; the pause holder blocks Draw). Exposure is rare²: a near-pool-cap game (the valve has never engaged in any gate replay) AND a concurrent lifecycle event; a checkpoint serialized in that window would capture a mid-frame state. If this ever matters, the fix is a save-side retry (release, wait one draw frame, re-park), not a valve redesign.

**Forced-valve smoke evidence (uncommitted `DG_VALVE_TEST` hack, headless Rosetta flag-ON to f=4010):** `FreePoolHeadroom` forced to 0 while `outstandingShells > 800` → `[PoolValveStats] engages=80 rounds=391 producePasses=391 totalWaitMs=476.4`, rc=0, 0 DESYNC, full FF speed, no hang — the retirement-wait loop makes progress under real load, concurrent with the draw thread's consumes. (The real gate replays never engage the valve; this smoke is the mechanism's only direct coverage, matching the 44b precedent.)

## 2. The backpressure rekey (§3.4 + §5.13)

`CGame::CanConsumeSimFrameNow` (NetCommands.cpp) replaces `gs->frameNum < SimDrawSplit::LastBoundaryFrame() + 1` with:

```cpp
if (simSnapshot.UnretiredEpochCount() < SimSnapshot::EPOCH_RING_SLOTS - 1)
    return true;
```

`UnretiredEpochCount() = epochCounter - retiredEpochId` (two monotone relaxed atomics; `retiredEpochId` is stamped in `AcquireNewestEpoch` when the consumer's release drops a slot's refcount to 0 — in-order, single consumer; teardown-reset in `SimSnapshot::Clear`). The `LastBoundaryFrame`/`PublishBoundaryFrame` plumbing is deleted; `ReleasePause` loses its boundary-frame parameter; `SignalEpochConsumeComplete` stamps pacing only.

**1× cadence:** publish E+1 (unretired = {held E, newest E+1} = 2 = N-1) → NEWFRAME consumption blocked → the draw side's next acquire retires E (unretired = 1) → unblocked. Same publish→acquire rhythm as the old gate. **Looser cases (blessed "unchanged-or-looser", §5.13):** between an acquire and the next publish (e.g. while the consumer is mid-consume or a draw stall), the sim may consume frames at the server's arrival rate rather than freezing at boundary+1 — under a draw stall at 1× the sim now tracks real time instead of falling behind and then free-running through `IsSimLagging` catch-up, which is behaviorally equivalent-or-better.

**Carve-out preservation (§5.13):** the FF/catch-up/skip/capture free-run checks (`gs->speedFactor > 1.01f || skipping || IsSimLagging() || videoCapturing->AllowRecord()`) are verbatim and evaluated after the ring gate — a free-running sim is extraction-SKIPPED by the producer's §3.2 pacing gate, never ring-BLOCKED. Evidence: `[BackpressureStats]` (new teardown counter of ring denials) is ABSENT (= 0 denials) in every DG_FF=1 gate run, and the full-length FF runs complete in minutes (see the gate table) — no stalled/ring-blocked fast-forward.

## 3. The RequestPause demotion (§3.5) — survivor audit

After 44b (no per-frame consume-park) and 44c (no valve park via the pause machinery), `SimDrawSplit::RequestPause` has exactly TWO callers: `CGame::AcquireSimPause` (every `ScopedExternalSimPause` bracket) and `CGame::AcquireLazyDispatchPark` (the §9 dispatch-scoped SYNCED-read fallback). Complete grep table of every `ScopedExternalSimPause` site on the landed tree, with its §3.5 class:

| Site | file:line | Class | Why it survives |
|------|-----------|-------|-----------------|
| Game save/load serialize | LoadSaveHandler.cpp:45 | LIFECYCLE | whole-sim serialize (saves/checkpoints) |
| Lua handle create/reload/kill brackets | LuaDefs.h:94/110/126 (`DECL_*_HANDLER`, used by LuaUI/LuaRules/LuaGaia/LuaIntro/LuaMenu) | LIFECYCLE | handle (re)load/kill mutates cross-VM state |
| `Reload`/`Restart`/`Start` executors | LuaUnsyncedCtrl.cpp:5735/5756/5781 | LIFECYCLE | process-lifecycle teardown paths |
| `CPathTexture::Update` | InfoTexture/Modern/Path.cpp:208 | OPERATOR-RULED PARK | display-gated (path infotex overlay), §4.5 ruling: stays |
| `TryTarget` | GuiHandler.cpp:1195 | ACCEPTED input-gated park | attack-command-held only; 0 in replays (pre-44c audit) |
| `TestUnitBuildSquare` minimap proxy | GuiHandler.cpp:1117 | ACCEPTED input-gated park | build-cmd + minimap proxy; 0 in replays |
| `GetDefaultCommand` non-cursor fallback | GuiHandler.cpp:1667/1759 | ACCEPTED input-gated park (Gap B/Prereq D residual) | cursor path is served; fallback is input/pregame only |
| `GetCommand` | GuiHandler.cpp:2429 | ACCEPTED input-gated park | mouse-release order build |
| `GetBuildPositions` | GuiHandler.cpp:2764 | ACCEPTED input-gated park | build drag |
| `DrawMapStuff` interior residuals | GuiHandler.cpp:4007/4077/4203 | ACCEPTED input-gated narrow parks (Prereq D residual) | weapon-range/build-preview interior reads; whole-pass park dropped by Prereq D |
| `MouseHandler::MouseRelease` | MouseHandler.cpp:484 | ACCEPTED input-gated park | selection box |
| `GiveOrder` family | LuaUnsyncedCtrl.cpp:3817 | ACCEPTED park with measured frequency (§4.5 ruling) | 0 in replays; op-capture design deferred |
| `SendCommands` sim-touching executors | via `touchesSimState` (UnsyncedGameCommands) | ACCEPTED rare park (Prereq E) | DRAW-UI/NET-SEND majority is park-free; ~1/game headful |
| Lazy dispatch park | LuaSyncedTable.cpp:51 → AcquireLazyDispatchPark | §9-RULED fallback | SYNCED first-touch/non-scalar dispatch-window reads; ~30/game |

No armed-test-gate `RequestPause` caller exists on the tree (the armed diff-gate runs flag-OFF where there is no sim thread; the LuaSnapshotServe armed dual-run brackets are `ScopedLiveException`s, not parks). There is NO per-frame `RequestPause` caller left; `[SimParkStats]`/`[SimPauseSurvey]` in the gate table below confirm parks ≈ lifecycle-only (tens per full game, all classes above).

**Ctrl-poke queue pre-extraction consumption** (the 44c prompt's fourth bullet): already landed by 44a — `LuaSplitContract::DrainBoundaryApplies()` is producer step p1, immediately before the due-check/extraction (freshness by construction). No 44c work remained; noted here for completeness.

## 4. Gate results (lean policy, operator-ratified)

| Leg | Result |
|-----|--------|
| `ninja engine-legacy` (each sub-commit) | rc=0 |
| Forced-valve smoke (headless Rosetta f=4010, uncommitted pressure hack) | rc=0, 0 DESYNC, engages=80 / rounds=391 / producePasses=391 / waitMs=476, no hang |
| Armed diff-gate ATG flag-OFF full-length (`DG_ARM=1`, headless, bar-data2) | **PASS (0 mismatches)**, rc=0, 0 DESYNC, full length (f=44867), wallclock 5:33 — flag-off byte-identity proven |
| Strict Rosetta flag-ON full-length headful (`SplitDrawContract=2`, bar-data3, concurrent with the armed leg) | rc=0, full length (f=44543), wallclock 3:26 (FF healthy — faster than 44b's 5:58), **0 DESYNC, 0 denials, idCoverage checked=186,036 violations=0, 0 lua errors, 0 `Invalid Feature id`**; `[SimParkStats]` parks=29 totalMs=193.8 avgMs=6.7 (lifecycle-only); `[SimPauseSurvey]` LUA_SEND_COMMANDS=5 only; mirror-served 44,537 / lazy-park 35; 0 ring-blocks (`[BackpressureStats]` absent — §5.13 carve-out held), 0 valve engages |
| Serial headful flag-ON ATG full-length (quiet machine) | rc=0, full length (f=44863), wallclock 4:21, **0 DESYNC, 0 denials, idCoverage checked=275,152 violations=0, 0 lua errors, 0 `Invalid Feature id`, 0 zombie echoes (cleaner than 44b's accepted 2), 0 no-park VBO-resize warnings** (the 44b watch class, absent); `[SimParkStats]` parks=21 totalMs=160.1 avgMs=7.6 maxMs=63.9 (parked ≈ 0, lifecycle-only); `[SimPauseSurvey]` LUA_SEND_COMMANDS=5 only; mirror-served 44,855 / lazy-park 29; 0 ring-blocks, 0 valve engages |

SKIPPED per the operator directive: TSan (end-of-stack), DEBUG (waived), extra headful repeats (end-of-stack matrix).

**FF/catch-up verification (gate item 4):** every full-length leg ran `DG_FF=1` and finished in 3:26–5:33 wallclock (minutes, not hours — no ring-blocked crawl), with `[BackpressureStats]` absent (= zero ring denials) in all flag-ON runs: the free-running sim was extraction-skipped, never ring-blocked, exactly the §5.13 contract.

## 5. Deviations / notes for PR 45+

- The lifecycle-park-during-valve mid-frame quiescence deviation (§1 above) — rare², flagged.
- Backpressure looseness under consumer stalls (§2 above) — §5.13-blessed.
- The valve wait can hold the sim for multiple draw frames under genuine pool pressure (vs one main-thread service round pre-44c) — the engaged-warning LOG line still fires per engage; `[PoolValveStats]` quantifies.
- `InvalidateStagedEffectContainers` (dead since the 44b valve iteration) removed.
- The end-of-stack items remain: TSan segment (the valve wait + ring backpressure are new cross-thread machinery), DEBUG-headless, the multi-run headful matrix, and the operator's live windowed skirmish (§7.8) — the final coverage of interactive input→park paths.
