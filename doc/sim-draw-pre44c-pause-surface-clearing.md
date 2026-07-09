# Pre-44c pause-surface clearing — disposition audit (§4.5 / §4.6 / §4.8)

Status: LANDED on `epoch-integration`. Governing contract: `sim-draw-pr43-44-contract-refresh.md` (§4.5 pause inventory, §4.6 ctrl remainders, §4.8 known races, §7 operator rulings). Base HEAD: `7e8895c312` (PR 40 projectile pass; SCOPE-1 + Gap A + Gap B landed).

Goal: clear the mid-gameplay sim-park surface so PR 44c can demote `RequestPause` to lifecycle-only. This is an audit PR: it (a) closes the cheap, landed-machinery items (§4.6 ctrl remainders via the id→owner seam; §4.8 Race 2), (b) lands per-site pause-surface telemetry to measure the remaining parks, and (c) dispositions every mid-gameplay `ScopedExternalSimPause` site as *served*, *converted*, or *accepted rare park*, escalating the per-frame sites that need dedicated serving PRs before 44b.

The 44 sub-split (operator ruling §7.5): 44a = extraction on the sim thread with a residual consume-time park; 44b = remove the park + the §3.6 creation/event-time deferred serving; **44c = demote RequestPause to lifecycle-only + rekey valve/backpressure**. So by the time 44c runs, 44b has already removed the barrier park. "Lifecycle-only" is the headline invariant *parked ≈ 0 outside lifecycle events*: it tolerates rare input/display-gated parks that fire ≈0 times in steady-state gameplay (this is exactly why `Path.cpp` and `GiveOrder` are ruled acceptable parks), but per-frame parks would move the headline and must be served first.

---

## §4.5 — mid-gameplay `ScopedExternalSimPause` inventory + disposition

### How the sites relate to the current (pre-44b) park

The draw-side barrier park spans `AcquireSimPause()` (Game.cpp Draw top) → `ReleaseSimPause()` (inside `UpdateUnsynced`). The world render (`worldDrawer.Draw()` → `DrawMapStuff`) and the minimap draw (`minimap` in `DrawInputReceivers`) run **after** `ReleaseSimPause` — so those sites already RE-PARK the running sim every frame today. `guihandler->Update` (`SetCursorIcon` → `TryTarget`/`TestUnitBuildSquare`/default-cmd query) was moved post-release by Gap B/`SplitWindowShrink`, so it too re-parks for real when it fires. Under 44b the barrier park is gone entirely, so every one of these sites becomes a real mid-gameplay park unless served.

### Telemetry landed with this PR

`CGame::SimPauseSite` (Game.h) tags each mid-gameplay site; `ScopedExternalSimPause(site)` counts a park **only when it actually engaged** (re-parked the running sim — a nested no-op is not counted). `CGame::DumpSimPauseSurvey()` logs the per-site totals at game shutdown (`[SimPauseSurvey] …`). Flag-off inert (the park never engages, `acquired=false`). This is the "measured frequency" instrument; it stays as permanent pause-surface telemetry for the 44b serving PRs.

### Disposition table

Line numbers are current HEAD. "Fires" = when the enclosing code path runs with the sim running and not already parked (i.e. a real re-park under 44b).

| # | Site | file:line | Fires | Live reads under the park | Disposition |
|---|------|-----------|-------|---------------------------|-------------|
| 1 | `DrawMapStuff` | GuiHandler.cpp:3622 | **per draw frame** (world + minimap GUI overlay) | selected-unit command queues, weapon ranges, GuiTraceRay, blocking/yardmap | **ESCALATE → dedicated serving PR before 44b.** Per-frame; cannot be an accepted park. Not cheaply servable: needs the command-queue cache (landed) + trace channel + range data wired into the whole world-GUI overlay. |
| 2 | `MiniMap::DrawCameraFrustumAndMouseSelection` | MiniMap.cpp:1432 | **per draw frame** the minimap draws | `GuiTraceRay` hover pick | **ESCALATE → dedicated serving PR before 44b.** Per-frame; needs the trace channel. |
| 3 | `TryTarget` | GuiHandler.cpp:1189 | per frame **while an attack command is active** (else not) | `GuiTraceRay` + selected-unit weapon state | **SERVE via trace channel (deferred to the trace-channel PR).** Command-gated: 0 in a no-input replay, per-frame while a command is held. Predates the PR-35 trace channel; that channel is the served fix. |
| 4 | `TestUnitBuildSquare` (minimap build-proxy) | GuiHandler.cpp:1112 | rare: build command active **and** minimap proxy | `Pos2BuildPos` + `TestUnitBuildSquare` (blocking/heightmap) | **ACCEPTED rare park** (Gap-B narrow park). Served fix = blocking mirror + placement channel (placement-channel PR). |
| 5 | `GetCommand` | GuiHandler.cpp:2241 | on mouse-release order build (input); nested no-op inside `DrawMapStuff` preview | queue/trace reads | **ACCEPTED rare park** (input-gated). |
| 6 | `GetBuildPositions` | GuiHandler.cpp:2576 | build drag (input); nested no-op inside `DrawMapStuff` | blocking/yardmap, `GuiTraceRay` | **ACCEPTED rare park** (input-gated). Served fix = blocking mirror + placement channel. |
| 7 | `GetDefaultCommand` (fallback) | GuiHandler.cpp:1726 | non-cursor callers: `DrawMapStuff` preview, `DrawCentroidCursor`, `LuaUnsyncedRead` | deep-CAI `GetDefaultCmd`, `GuiTraceRay` | **LEAVE (Gap B).** The every-frame cursor path is already served by the default-command query/reply (a8c90334ce). Under 44b the query hook must be serviced while paused — carried in the Gap-B landed note. |
| 8 | `MouseHandler::MouseRelease` | MouseHandler.cpp:484 | on mouse-button release (input) | selection-box `GuiTraceRay` + unit walk | **ACCEPTED rare park** (input-gated). |
| 9 | `SendCommands` | LuaUnsyncedCtrl.cpp:603 | on `Spring.SendCommands` (Lua) | console-action executors (see §4.5-SendCommands) | **ACCEPTED park, kept conservative** — deadlock-safe (no executor blocks on the sim); per-action scoping is a deferred optimization. |
| 10 | `GiveOrder` family | LuaUnsyncedCtrl.cpp:3818 | on `Spring.GiveOrder` (Lua) | `GiveCommand` local side effects (wait-AI inserts, ok-sound live unit read) | **ACCEPTED rare park with measured frequency** (operator §4.5 ruling: "stays a park with measured frequency"). |
| — | `CPathTexture::Update` | InfoTexture/Modern/Path.cpp:208 | only when the path infotex overlay is toggled on | path cost map | **LEAVE — operator-ruled park (display-gated).** |

Measured park-engage counts (`[SimPauseSurvey]` at shutdown; a site absent from the list = 0):

| Site | Rosetta headless (f=44539) | ATG headless (f=44864) | Rosetta headful ×2 (f≈44540) | ATG headful ×2 (f≈44900) |
|------|---------------------------:|-----------------------:|-----------------------------:|-------------------------:|
| `GUI_DRAW_MAPSTUFF` | 222166 | 159683 | 12418 / 12786 | 13304 / 13139 |
| `GUI_GET_DEFAULT_CMD` | 222166 | 159683 | 10011 / 10333 | 9719 / 9650 |
| `LUA_SEND_COMMANDS` | 22 | 6 | 10055 / 10376 | 9722 / 9654 |
| `MINIMAP_FRUSTUM` | 0 | 0 | 0 | 0 |
| `GUI_TRY_TARGET` / `GUI_GET_COMMAND` / `GUI_GET_BUILDPOS` / `GUI_TEST_BUILDSQUARE` / `MOUSE_RELEASE` / `LUA_GIVE_ORDER` | 0 | 0 | 0 | 0 |

The measurement CONFIRMS the partition and adds one reclassification:
- **Per-frame parks** (must be served before 44b): `GUI_DRAW_MAPSTUFF` engages ~once per *draw* frame (headless renders far more draw frames than headful, hence 222k vs 12k — either way, per-frame). `GUI_GET_DEFAULT_CMD` engages ~identically — a finding: **Gap B unparked only the every-frame *cursor* path (via the default-command query/reply); the `GetDefaultCommand` fallback's *non-cursor* callers (`DrawMapStuff` / `DrawCentroidCursor`) still park per frame.** Folds into the DrawMapStuff serving PR.
- **`LUA_SEND_COMMANDS` — RECLASSIFIED to per-frame headful** (≈10k/game vs 22 headless). Headless loads no LuaUI widgets; headful stock-BAR widgets call `Spring.SendCommands` ~per draw frame (verified NOT the diff-gate driver, which only issues setup commands). So the full-batch park is a *per-frame* park headful → it moves the *parked ≈ 0* headline and the per-action scoping (§4.5-SendCommands classification) becomes a **needed 44b prerequisite**, not merely an optimization. See the escalation.
- **`MINIMAP_FRUSTUM` = 0 even headful** in both replays — the frustum/hover park did not engage under these replays' minimap state; its per-frame risk is config-dependent, not exercised here. Stays an accepted park pending the trace channel.
- **Zero in a no-input replay**: every remaining input-gated site. Per operator §7.8 replays don't drive interactive input, so ≈0 is the *evidence* these are input-gated (accepted parks), not steady-state; their true interactive frequency is covered only by the operator's final live skirmish.

### Escalation to the operator (§4.5)

Two sites fire **per draw frame** and therefore cannot be accepted as parks under the *parked ≈ 0* headline, but are also too large to serve inside this audit PR:

- **`DrawMapStuff` (GuiHandler.cpp:3622)** — the entire world-space GUI overlay reads live sim (command queues, ranges, traces, build previews). Serving it is a dedicated PR: route the queue reads through the landed command-queue cache, the traces through the PR-35 trace channel, and the blocking/yardmap reads through the blocking mirror + placement channel.
- **`MiniMap::DrawCameraFrustumAndMouseSelection` (MiniMap.cpp:1432)** — per-frame `GuiTraceRay` hover; needs the trace channel.

Both are **hard 44b prerequisites** (they precede 44c). This PR does not attempt them (ESCALATION rule: narrowest flag-off-safe change; serving them is unlanded-machinery work). `TryTarget` (#3) is the same trace-channel dependency but command-gated. Recommendation: a "world-GUI serving" PR (trace channel + queue/range reads) + a "placement channel" PR (blocking/yardmap: #4/#6) land before 44b; #5/#8/#9/#10 stay accepted parks.

---

## §4.5 — `SendCommands` per-action net/UI/sim classification (Batch-1 amendment)

`Spring.SendCommands` → `CGuiHandler::RunCustomCommands` → per action `ProcessLocalActions` else `game->ProcessAction` → `ActionPressed` → unsynced executor registry (`UnsyncedGameCommands.cpp`) / `clientNet->Send` / console. **Synced actions are never executed inline on the draw thread** — they are net-sent (`clientNet->Send`, `RedirectToSyncedActionExecutor`) and consumed asynchronously by the sim; the synced executors run only on the network-receive path (`CGame::ActionReceived`).

Deadlock check (the Batch-1 amendment's core owed item): **no reachable executor blocks/waits synchronously on the sim thread** (no `JoinSimThread`/join/condition-wait anywhere in the unsynced executor path). `ScopedExternalSimPause` is a one-way "sim parks at its own frame edge" request — the sim never waits on the draw thread — and is reentrant (nested re-park is a no-op). **Therefore the current full-batch park is deadlock-safe, and removing it under 44c cannot deadlock.** (The runtime confirmation of interactive `SendCommands` executor-park behaviour is deferred to the operator's final live skirmish per §7.8.)

Classification (bucketed):

| Family | Bucket | Touches live sim? | Blocks on sim? |
|--------|--------|-------------------|----------------|
| Camera / view, config & GUI toggles, renderer/assets, sound, render-only debug drawers | DRAW-UI | No | No |
| Chat / echo / net-ping | NET-SEND | No | No |
| Cheat / give / destroy / take / atm / redirect-to-synced | NET-SEND (async `clientNet->Send`) | Give reads `CGround`; Destroy/Remove read `selectedUnits` | No |
| Team / spectator / ally / speed / pause / control-unit | NET-SEND + sim-read (`teamHandler`/`gs->`) | Yes (read then net-send) | No |
| Selection, `ViewSelection`, group | SIM-POKE (read) | `selectedUnitsHandler`, `unitHandler…->midPos` | No |
| `MaxParticles`/`MaxNanoParticles` | SIM-POKE (write) | `projectileHandler.SetMaxParticles` | No |
| `DumpState`/`DumpRNG` | SIM-POKE (heavy read) | walks the whole live synced world | No |
| Save / Quit / Reload lifecycle | deferred flag-set | No inline sim touch | No |

Disposition: **keep the conservative full-batch park this PR** (correct, deadlock-safe), but the headful measurement (≈10k engages/game — stock BAR widgets call `SendCommands` ~per draw frame) **reclassifies this from "rare" to a per-frame headful park**. It is over-conservative: the DRAW-UI and NET-SEND families (the large majority) need no park; only the SIM-POKE readers (`DumpState`/`ViewSelection`/`Give`/particle-limits) justify one. **ESCALATION:** because it is per-frame headful, the per-action park scoping (skip the park for DRAW-UI/NET actions; per-action park or boundary-apply for the SIM-POKE minority) is a **needed 44b prerequisite**, not merely a deferred optimization — it moves the *parked ≈ 0* headline. The classification above is the analysis that owed work required (Batch-1 amendment); implementing the scoping is a follow-up PR. No behavioural code change this PR beyond the telemetry tag.

---

## §4.6 — PR-41-escalated ctrl remainders (id→owner seam) — LANDED

The seam: `ParseCtrlUnitID` (LuaUnsyncedCtrl.cpp:450, landed `875d6e2058`) resolves identity + `CanControlTeam` from `SimSnapshot::UnitRows::Valid/Team` under the split — never a live `unit->team` deref of a pool-recycled slot — and delegates to `ParseCtrlUnit`→`->id` flag-off (byte-identical). Draw-owned icon state is an id-keyed vector; group/CUnit-field writes boundary-apply (drain at the parked barrier); the deferred-safe id→object handle is `DrawerGetObjectByID<T>`.

| Callout | Was | Now | Notes |
|---------|-----|-----|-------|
| `SetUnitNoGroup` (LuaUnsyncedCtrl.cpp:2407) | `ParseCtrlUnit` (live `->team`) + `unit->SetGroup(nullptr)` (live CUnit* → `uiGroupHandlers[team]`) | `ParseCtrlUnitID` + `QueueBoundaryApply(id){ noGroup + SetGroup(nullptr) }` (drained sim-parked) + flag-off live path | mirrors `SetUnitNoSelect`; `noGroup` is a sim-owned CUnit field so it boundary-applies |
| `SetUnitIconDraw` (LuaUnsyncedCtrl.cpp:2834) | `ParseCtrlUnit` + `SetUnitDrawIcon(unit,…)` | `ParseCtrlUnitID` + id-keyed `SetUnitDrawIcon(id,…)` | icon state draw-owned; new id overloads on UnitDrawerData/UnitDrawer |
| `SetUnitIcon` (LuaUnsyncedCtrl.cpp:2852) | `ParseCtrlUnit` + `SetUnitCustomIcon(unit,…)` + `UpdateCurrentUnitIcon(unit)` | `ParseCtrlUnitID` + id-keyed `SetUnitCustomIcon(id,…)` + `UpdateCurrentUnitIcon(DrawerGetObjectByID<CUnit>(id))` (null-guarded) | matches PlayerChanged's use of the same call; `UpdateCurrentUnitIcon`'s `losStatus` read is the pre-existing draw-reads-LOS surface, unchanged |
| `TraceScreenRay` minimap branch (LuaUnsyncedRead.cpp:3511) | `minimap->GetSelectUnit(pos)->id` (live CUnit* deref) | `minimap->GetSelectUnitID(pos)` (id-only pick) | mirrors the main branch's `GuiTraceRay` hit-id out-params; deref encapsulated in the draw-owned pick helper |

New API: `CUnitDrawerData::SetUnitCustomIcon(int,size_t)` / `SetUnitDrawIcon(int,bool)` / `IconStateRef(int)`; `CUnitDrawer::SetUnitCustomIcon(int,size_t)` / `SetUnitDrawIcon(int,bool)`; `CMiniMap::GetSelectUnitID(const float3&)`. All flag-off byte-identical.

---

## §4.8 — known races: clear / re-triage

Source: `pr27b-implementation-notes.md` TSan triage (Rosetta flag-ON segment).

### Race 1 — `RenderEventQueue::Push` append race — ALREADY CLEARED (documented)

Container `std::vector<Record> records` (RenderEventQueue.h), append at RenderEventQueue.cpp:46. The only path that reaches `Push` off the pure sim thread is the MT unsynced projectile pass (`UpdateProjectilesImpl<false>` → CEG/wreck spawns). **Every reachable MT spawn site already takes `CProjectile::mut`** before it can reach `Push` (`CExpGenSpawner::Update`→`Explosion(withMutex=true)`, `CWreckProjectile::Update` `scoped_lock(mut)`); the synced and unsynced passes run sequentially, so there is no synced/unsynced overlap either. The invariant is asserted in `Push` and `AddProjectile`. **No code change** — the triage's suggested "append under `CProjectile::mut`" is in place. Residual discipline note: a future unsynced projectile that spawns from its MT `Update()` without taking `mut` would re-open it; the assert catches it.

### Race 2 — `guRNG` shared unsynced stream (draw vs sim) — FIXED

The served draw-thread twin `LuaSnapshotServe::GetTeamUnitsByDefs` shuffled with the global `guRNG` (LuaSnapshotServe.cpp:4118), which the sim thread concurrently RMWs from particle/CEG spawns — a genuine split-only data race on one PCG32 state (UB; the shuffle order itself is only cosmetic anti-leak). **Fix:** the served twin now shuffles with its own draw-owned `CGlobalUnsyncedRNG` (default PCG32 state is a valid stream; no seed needed). Flag-off inert (the served twin never runs — the Live sibling on the main thread is used); flag-on the callout is compared order-insensitively (ID set) by the armed dual-run. Clears the TSan finding.

### Race 3 — sim-thread camera read vs draw-thread camera write — RE-TRIAGED (benign) + one escalation

`float3 camVect = camera->GetPos() - pos;` at ExplosionGenerator.cpp:453 (`CStdExplosionGenerator::Explosion`, sim thread) reads the active camera while the draw thread rewrites its `pos/dir/…` — a torn `float3` read. **Benign/cosmetic**: at worst a slightly-off spawn offset for one unsynced heat-cloud particle for one frame; no sync impact (unsynced particle, `guRNG` not `gsRNG`). Effectively headful-only (no windowed camera controller headless). **Disposition: tolerated §C torn-read** (matches the triage's "keep an eye on it"). A real fix (publish the camera position at the barrier and read the published copy sim-side) is a channel addition deferred to the flip work; not warranted for a benign cosmetic read.

**Sim-thread camera WRITE — FIXED (operator ruling A, 2026-07-09).** A *distinct* sim-thread camera write — `camera->SetRotY(camera->GetRot().y + …)` in `CGroundMoveType::UpdateDirectControl` (FPS/direct-unit-control) — is an RMW racing the draw thread's every-frame camera use. Operator ruled direct-control IS supported under the split, so the write is deferred draw-side: the sim accumulates the additive rot-Y delta via `CCameraHandler::AddFPSDirectControlRotY` (relaxed atomic, no live camera touch); the draw side drains + applies it (`ApplyPendingFPSDirectControlRotY`) before `camHandler->UpdateController` in `UpdateUnsynced`. Additive → the summed delta reproduces master's per-sim-frame inline writes; flag-off keeps the exact inline `SetRotY` (byte-identical). Camera is unsynced → zero synced impact. The FPS-camera follow behaviour is not replay-reachable (no local direct-control in a replay), so it is verified in the operator's final live skirmish; the replay gates cover sync byte-identity (flag-off resim) + no-regression (flag-ON headful, drain runs clean). See contract-refresh §8.1.

---

## Gate results

Engine built in the worktree (`ninja -C build engine-legacy` / `engine-headless`, both rc=0). Config `SimDrawSplit=1`, `SplitWindowShrink=1`, `SPRING_DATADIR=/www/projects/bar-data2`.

**Armed diff-gate** (headless, `DG_ARM=1 DG_FF=1`, full-length, `build/spring-headless`):
- Rosetta → **GATE PASS (0 mismatches** across all channels incl. `unit:rules`/`feature:rules`/`cq:lastPage`/mirrors/callouts), **SYNC clean (0 DESYNC)**, f=44539.
- All That Glitters → **GATE PASS (0 mismatches), SYNC clean (0 DESYNC)**, f=44864.
- (Pre-game headless-only Lua errors — `gui_pip`/`r2thelper` GL-shader callouts absent in headless — are environmental, at f=-1, not branch-caused.)

**Flag-ON headful ×2 per replay** (`build/spring`, `DISPLAY=:0`, `DG_ARM=0 DG_FF=1`, full-length):

| Run | rc | last frame | DESYNC | in-game Lua errors |
|-----|----|-----------|--------|--------------------|
| rosetta_hf1 | 0 | 44541 | 0 | 0 |
| rosetta_hf2 | 0 | 44538 | 0 | 0 |
| atg_hf1 | 0 | 44933 | 0 | 0 |
| atg_hf2 | 0 | 44862 | 0 | 0 |

No 38b widget-error nil-storm (`gl.SetFeatureBufferUniforms() Invalid Feature id` from `unit_healthbars_widget_forwarding.lua`); the only `healthbars.*forwarding` log lines are the pregame gadget-load banners. No crash, no DESYNC.

**TSan segment** (flag-ON headful, `build-tsan/spring` USE_TSAN=ON/USE_MIMALLOC=OFF, watchdog off `HangTimeout=-1`, `TSAN_OPTIONS=allocator_may_return_null=1 halt_on_error=0`; a build-tsan-only `aligned_alloc` round-up was applied for the run and **reverted before commit**, per the PR40 precedent). Rosetta, full load + 600 gameplay frames, quit at **f=601, RC=0**. 3162 data-race warnings — **all in pre-existing families**: load-time parallel infra (S3OParser/IModelParser/ArchiveScanner/ThreadPool/Futex) and the tolerated §C gameplay torn-reads (GlobalRNG `PCG32`, `LosHandler`, `BeamLaser`, `CUnit::UpdateWeaponVectors`, `SolidObject::Move`/`float3`, `Camera`/`ExplosionGenerator` = Race 3, `GroundMoveType`, `CProjectileDrawer::UpdateDrawFlags`, libgallium). **Zero warnings reference any symbol this PR changed** — `simPauseSiteCounts`/`DumpSimPauseSurvey`/`ScopedExternalSimPause` ctor (Game.cpp:2057/2081/2092), `drawUnsyncedRNG`, `IconStateRef`/`SetUnit*Icon`, `GetSelectUnitID`, `GetTeamUnitsByDefs` are all absent. The Race-2 fix is confirmed: the `GetTeamUnitsByDefs` draw-twin no longer appears in any race. Conclusion: **no new races introduced; the telemetry counters (relaxed atomics), the draw-owned RNG (draw-thread-only), the id-keyed icon ops (draw-thread-only), and the boundary-applied `SetUnitNoGroup` (sim-parked drain) are all race-free by construction and confirmed unracing at runtime.**

**All gates GREEN.**
