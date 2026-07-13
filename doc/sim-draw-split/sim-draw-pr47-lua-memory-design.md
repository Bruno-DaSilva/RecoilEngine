# PR 47 — Lua memory regression under the split: measurement, root cause, fix

Operator report (2026-07-10, flag-ON headful games): "uncontrolled Lua memory growth causing emergency GCs again". Operator confirmed the symptom does NOT occur on the base branch — this was ours to root-cause and fix. This record documents the measurement methodology, the growth curves (numbers), the root cause, the fix, and verification.

Commits: `bc6d335bf0` (1/3, the GC-pacing fix + measurement widgets), `c6e2f55e61` (2/3, the flag-off piece-serving NIL fix + armed coverage), this record (3/3). Base: `5ab6391ac2` (PR 46).

## 1. What "emergency GC" is

BAR's `widgetHandler:Update` (luaui/barwidgets.lua) checks `collectgarbage("count") > 1200000` every 30 Update calls and, when tripped, echoes `Warning: Emergency garbage collection due to exceeding 1.2GB LuaRAM` and runs a FULL `collectgarbage("collect")`. It measures the LuaUI lua_State's own heap. A full collect on a >1GB heap takes O(0.5s) — the emergency valve is itself a visible stutter, on top of the memory growth.

Engine-side context: `SLuaAllocLimit::MAX_ALLOC_BYTES` (default 1536 MB, `rts/Lua/LuaAllocState.h`) is the global hard OOM valve across ALL lua_States; `spring_lua_alloc_skip_gc` (rts/lib/lua/include/LuaUser.cpp) makes each `CLuaHandle::CollectGarbage(false)` call a probabilistic no-op with run-probability `LuaGarbageCollectionMemLoadMult (1.33) x globalAllocedBytes / MAX_ALLOC_BYTES` — i.e. collection frequency scales with heap load, per COLLECTOR CALL.

## 2. Measurement methodology

Instruments (all under `test/callout-diff-gate/`, installed as LuaUI widgets in the gate datadirs; follow the diff-gate driver registration pattern):

- `lua_mem_sampler.lua` — every 300 game frames echoes `[LuaMemSampler] f= df= wall= luauiHeapKB= handleKB= handleKAllocs= globalKB= unsyncedKB= syncedKB=` from `collectgarbage("count")` + `Spring.GetLuaMemUsage()`. `handleKAllocs` (the engine's per-handle `numLuaAllocs`, kilo-allocs) is monotonic gross allocation count — the churn metric, independent of GC. `df` = `Spring.GetDrawFrame()` for draw-rate normalization (added mid-investigation; earliest runs lack it).
- `lua_gc_probe.lua` — at f=3000, from DrawGenesis (draw-context so the serving twins are engaged flag-ON), calls each of 74 callouts N=10000x on a fixed live unit/feature/team and echoes the per-call delta. Two variants used: garbage bytes with the GC stopped, then `numLuaAllocs` per call (the committed version; the GC-stopped variant trips the 1.5GB valve on big-table callouts at N=10000).
- `lua_section_probe.lua` — replicates the diff-gate exercise loop section-by-section (positions family / rules-params / strided unit tail / feature rules-params / feature tail / projectiles / teams+players / globals) over ALL units for 200 real draw frames, attributing `numLuaAllocs` per section. Catches cold-id/epoch-advance effects the single-unit probe cannot.
- `lua_piece_check.lua` — one-off value check: prints `GetUnitPieceMap`/`List` return types from a Draw* callin vs `GameFrame` (the instrument that confirmed the §6.35 flag-off rehearsal NIL).

All measurement runs: Rosetta replay, headless, `run_diff_gate.sh`, `DG_ARM=0`, fast-forward, flag-ON in bar-data2 / flag-OFF in bar-data3, concurrently.

## 3. The curves (pre-fix, HEAD = 5ab6391ac2 + instruments)

Full-length Rosetta headless (DG_EXERCISE=1, the serving-load generator on — a proxy for BAR's ~27k callout calls/draw-frame headful):

| metric (game end, f≈44400) | flag-ON | flag-OFF |
|---|---|---|
| LuaUI gross allocations (handleKAllocs) | 231.6 M | 43.3 M (5.35x) |
| LuaUI heap sawtooth peaks | repeatedly 1.15–1.20 GB | 100–712 MB |
| BAR emergency collects | **42** | **0** |
| DESYNC | 0 | 0 |

Emergency collects fire every 1–5 s continuously from t=50s on — the heap refills to 1.2GB between BAR's 30-Update valve checks.

Segment runs to f=12000 (same replay, same widgets):

| run | LuaUI kallocs @f=12000 | draw frames @f=12000 |
|---|---|---|
| ON, exercise=1 | 34 556 | 34 671 |
| OFF, exercise=1 | 7 786 | 28 825 |
| ON, exercise=0 | 6 516 | 30 936 |
| OFF, exercise=0 | 4 589 | 28 583 |

Cumulative df is dominated by pregame (both free-run thousands of df during load); the IN-GAME rates diverge massively — see §4.

## 4. Root cause

Three-step elimination:

1. **Not per-call twin allocation.** The per-callout probe: 74 callouts x 10000 calls, per-call garbage bytes AND per-call allocation counts are IDENTICAL flag-ON vs flag-OFF, to the second decimal, for every probed callout (GetUnitPosition 0.0, GetUnitLosState 2.0/2.0, GetUnitRulesParams 2.0/2.0, GetUnitVectors 6.0/6.0, GetUnitStates ON 704B = OFF 704B, ...). The serving twins are line-by-line mirrors and allocate exactly like the live bodies.
2. **Not the loop composition.** The section probe over 200 real draw frames: every section's per-draw-frame allocation count is equal or slightly LOWER flag-ON (posfam 63.4k vs 77.0k kallocs/200df, rulesparams 63.4 vs 77.0, teams 25.5 vs 25.5 — OFF's 200 df span more sim time, hence more units).
3. **It is the draw-frame RATE.** In the same 300-sim-frame window mid-replay (f=3900→4200 under FF), flag-ON ran **1773** draw frames (~2200 df/s) vs flag-OFF **41** (~50 df/s) — **~40x**. Master's mainloop, sharing one thread, draws rarely while simming at speed 20; the split's consumer free-runs by design.

The garbage per draw frame is unchanged (BAR widget work, master-faithful); the garbage per SECOND is multiplied by the free-running draw rate. Meanwhile the collector for the unsynced states did NOT scale:

- Master: `CGame::SimFrame` calls `eventHandler.CollectGarbage(false, GC_ALL)` — once per SIM frame, i.e. from the same loop that runs the (synchronous) unsynced dispatches, with per-call budget divided by `gcSpeedFactor` (clamped ≤50). At FF speed-20 that is ~600 collector calls/s.
- Split (PR 27b): the sim thread collects only the synced states (per sim frame); the unsynced states' ONLY collector became the fixed **30 Hz** main-thread timed job (`CGame::AddTimedJobs`), budget ≤5 ms per non-skipped call (`fixedRateCaller`, PR 27b comment in `CLuaHandle::CollectGarbage`).

Under FF headless: garbage production x40, collection calls/s ÷20 vs master. The skip-gate (`spring_lua_alloc_skip_gc`) scales the run-probability with heap load but can never exceed 30 calls/s x 5 ms = 150 ms/s of collection — the heap climbs until BAR's 1.2GB valve does the collector's job. Headful at uncapped/high fps: same mechanism, smaller multiplier, slower climb — "the lua heap grows quickly over time" (operator).

Why this appeared "again": PR 27b had already hit the imbalance once (windowed-dogfood LUA_ERRMEM, fixed by making the 30Hz job collect unsynced unconditionally, commit comment at `Game.cpp:AddTimedJobs`). That fix set the collector's CADENCE right for ~master-rate draw loops but not for the post-44b free-running consumer, whose draw rate is unbounded.

## 5. The fix

`rts/Game/Game.cpp`, top of `CGame::Draw()` (before `SimDrawBarrier()`, i.e. OUTSIDE the draw window, no dispatch or draw callin running), flag-gated:

```cpp
if (SimDrawSplit::Enabled())
    eventHandler.CollectGarbage(false, CEventHandler::GC_UNSYNCED_ONLY);
```

This restores master's invariant — the GC is stepped from the loop that produces the garbage, so collection calls/s scale 1:1 with draw frames/s. The per-call cost self-balances exactly as on master: `spring_lua_alloc_skip_gc` skips with probability `1 − 1.33·load`, so a low heap makes the call a cheap RNG check; the budget stays the PR 27b `fixedRateCaller` semantics (≤5 ms, early-exit when a full cycle frees nothing). The 30 Hz timed job is kept unchanged as the backstop for non-drawing periods (menus, parked saves, minimized — the reason it exists since PR 27b).

Placement notes: top-of-Draw covers every Draw exit path (the headless `!globalRendering->active` early-return after DrawGenesis, the UpdateUnsynced early-outs, and the full path); it runs on the draw thread which owns every unsynced state (same thread the 30 Hz job used — no new threading); flag-OFF takes the unchanged legacy path (SimFrame GC_ALL + paused-gated job), byte-identical.

## 6. Verification (fixed binary bc6d335bf0)

### 6.1 Stress load (DG_EXERCISE=1, the serving-load generator — ~10x BAR's ambient headless churn)

Full-length Rosetta headless FF, concurrent A/B:

| metric (game end f≈44400) | flag-ON (fixed) | flag-OFF | flag-ON (pre-fix) |
|---|---|---|---|
| LuaUI gross allocations | 227.3 M | 43.4 M | 231.6 M |
| LuaUI max heap | 965 MB | 680 MB | 1.20 GB (valve-pinned) |
| LuaUI end heap | 504 MB | 666 MB | sawtooth at valve |
| BAR emergency collects | **0** | 0 | **42** |
| FF wall time | 162 s | 186 s | 161 s |
| DESYNC | 0 | 0 | 0 |

Gross allocation count is unchanged by design — the churn is legitimate per-draw-frame widget work; the collector now paces it. FF wall time unaffected (the GC runs on the otherwise-idle-spinning consumer). Split telemetry nominal: parks=32 lifecycle-only, idCoverage 0 violations, SYNCED mirror lazy engages 31, denials 0.

### 6.2 Matched ambient load (DG_EXERCISE=0 — BAR's own headless widgets only)

| metric (game end f≈44400) | flag-ON (fixed) | flag-ON (fixed, rerun) | flag-OFF |
|---|---|---|---|
| LuaUI gross allocations | 11.9 M | 11.4 M | 6.8 M |
| LuaUI max heap | **52.6 MB** | 37.9 MB (end) | 137.7 MB |
| BAR emergency collects | 0 | 0 | 0 |

Flag-ON now runs a SMALLER LuaUI footprint than flag-OFF (collection is paced by the free-running draw loop, which runs more often than master's), while the draw rate stays unthrottled when the heap is low (skip-gate ≈ no-op). The step-2 criterion — flag-ON curve tracks flag-OFF within noise, emergency count ON ≤ OFF — holds with margin.

The PRE-FIX binary could not complete this configuration at all: by f≈17.7k its LuaUI heap had pushed process RSS to 4.1 GB and the run wedged in a continuous full-collect spiral (log stalled 8+ minutes, 410% CPU) — the undamped version of exactly the operator's headful symptom.

### 6.3 Gate battery (lean, per the operator-ratified §7 gate economy)

- build rc=0 (spring-headless + engine-legacy), both commits.
- Armed diff-gate, flag-OFF, full-length Rosetta: **PASS (0 mismatches)**, 0 DESYNC — run twice: on the pacing-fix binary (commit 1), and re-run on the final binary with the new (throttled) piece-family dual-run coverage after the §6.35 fix.
- Headful flag-ON full-length ATG (FF, exercise=1, commit-1 binary; commit 2 is flag-ON inert by construction — `Armed()`/`Enabled()` predicate falls through to the unchanged `Route`): rc=0, reached f=44700, 0 DESYNC, 0 lua errors beyond the §4 baselines (exactly the accepted deterministic airjets ZOMBIE pair at gf≈12050), idCoverage checked=275152 violations=0, parks=22 all lifecycle.

### 6.3b Headful memory A/B (ATG, FF, exercise=1 — measured because the operator's symptom is headful)

| | flag-ON (fixed) | flag-OFF |
|---|---|---|
| emergency collects | 8 (every ~20–25 s from t≈2:10) | **0** |
| max LuaUI heap | 1.21 GB | 397 MB |
| gross allocations (f=44700) | 193.5 M @ df=13583 | 36.8 M @ df=6154 |

The pacing fix moved headful FF from the pre-fix continuous-valve regime to periodic trips, but headful is NOT yet at parity: at the SAME game frame (f=17100) flag-ON had churned 21.2M allocs over 6894 df (≈3.1k/df) vs flag-OFF 7.2M over 5179 df (≈1.4k/df) — a REAL ~2.2x per-draw-frame allocation divergence that exists only headful (headless per-frame churn is equal, §4 step 2). This is a second, headful-only component — most plausibly a serving path exercised only by the GL4 widget set (which headless drops on shader-compile failure) — and is still open; see §6.5.

## 6.35 Second find + fix: flag-OFF draw-callin piece reads served NIL (pre-existing since PR 33)

Hunting the headful churn residual with the extended per-callout probe surfaced an inconsistency instead of a leak: headful `GetUnitPieceMap`/`GetUnitPieceList` measured 2 allocs/call flag-ON but **0** flag-OFF — impossible for a table-returning callout. Direct check (one-off widget): flag-OFF, `Spring.GetUnitPieceMap(uid)` returns a real 40-piece table from `widget:GameFrame` but **nil from `widget:DrawGenesis`**.

Cause: `EnsurePieceCacheCaptured` (LuaSnapshotServe.cpp) skips the piece-cache capture flag-off ("flag-off never serves these twins") — but `Route`'s flag-off draw-callin REHEARSAL (ShouldServe via `InDrawCallin()`, PR 27a) still picks the twins, which then find an empty per-slot cache and nil out. Every piece-slot-cache-backed callout (24 wrappers: unit/feature Root/Map/List/Info/Position/Direction/PosDir/Matrix pieces, ScriptPiece/ScriptNames, LastAttackedPiece, CollisionVolumeData, PieceCollisionVolumeData) was nil-broken in flag-off draw callins — an invariant-1 violation vs base, hidden because (a) BAR's widgets read piece maps outside Draw* callins, and (b) the diff-gate exercise loop never called the family, so the armed dual-run had no coverage.

Fix (`LuaSnapshotServe::RoutePieceCache` + the 24 wrapper swaps in LuaSyncedRead.cpp): serve the LIVE leg under exactly the capture-skip predicate (`!LuaSplitContract::Enabled() && !snapshotDiffGate.Armed()`), so twin-picking and capture can never disagree; flag-on and armed behavior unchanged by construction. Verified with `lua_piece_check.lua` headful: flag-OFF DrawGenesis now returns the real piece tables (was nil); flag-ON unchanged (twin-served in both contexts).

The exercise loop now covers the family on a deliberate 1-in-64-frame cadence (`ExercisePieceFamily`: unit RootPiece/PieceMap/PieceList/PieceMatrix/PiecePosDir + feature PieceMap/PieceList): a per-frame version livelocked the armed FF run mid-game, because ANY piece-twin call lazily triggers `EnsurePieceCacheCaptured` for the held epoch — a maxUnits resize + per-unit piece copy — and per-frame exercise makes that a per-draw-frame full re-capture (gate-found; multi-GB C++ churn at peak unit count). Two lessons recorded: (a) the piece-cache capture cost is per-epoch-×-consumer-rate when any piece callout is hot — a game hammering piece callouts from draw context every frame flag-ON would pay the same cost (watch `[EpochStats]` if that ever shows up); (b) the armed dual-run of this family is expensive by construction (double table builds + compares).

Note: this pre-existing nil-break is NOT the memory regression (the nil path allocates less, not more); it is a correctness find made by the memory probes.

## 6.4 ESCALATION — one non-reproducing flag-ON DESYNC in a previously-ungated configuration

During verification, ONE of three full-length flag-ON `DG_EXERCISE=0` runs (fixed binary, Rosetta, headless FF) desynced from the demo stream at f=32040 (checksum divergence vs all recorded players; 3717 warning lines to game end; run otherwise nominal — parks lifecycle-only, idCoverage 0, no lua errors beyond §4 baselines). The frame follows a demo-recorded mid-game spectator rejoin (f=31018–31398), which every other run replays cleanly.

Facts bearing on attribution:

- The same binary + identical configuration, rerun: CLEAN through game end (desync=0). Non-deterministic.
- All `DG_EXERCISE=1` full-length flag-ON runs — including this session's pre-fix and fixed stress runs and every PR 43–46 gate — are clean. `DG_EXERCISE=0` full-length flag-ON appears to be a configuration the program never gated (the runner defaults DG_EXERCISE=1): it is the maximum-consumer-rate regime (the draw thread free-runs at ~1.8–5k fps against a 600 fps producer, vs ~150–600 fps with the exercise loop loading each frame).
- A pre-fix A/B was attempted and is UNOBTAINABLE: the pre-fix binary cannot complete this configuration at all (§6.2 — it wedges in the memory spiral by f≈17.7k).
- The fix itself only changes WHEN `CLuaHandle::CollectGarbage` runs for draw-owned unsynced states (same thread, same states, same code path as the existing 30Hz job; unsynced pool is per-side since PR 27b) — no mechanism by which it can write synced state has been found in audit.

Assessment: most plausibly a pre-existing timing-dependent race in the split machinery exposed by the extreme consumer acquire/release rate of the ungated exercise-free regime, but NOT PROVEN — per the program rule (any flag-ON DESYNC is real; single occurrence counts), this is escalated to the operator rather than closed. Suggested follow-up: repeated `DG_EXERCISE=0` full-length flag-ON runs on both this and the parent commit binary (statistical attribution), and/or a TSan segment bracketing a high-consumer-rate window.

## 6.5 Open follow-up — headful FF emergency parity

With the pacing fix, headful FF flag-ON still trips BAR's valve periodically (§6.3b: 8/game vs 0 flag-OFF) for two rate reasons the fix does not (and should not silently) change: (a) the free-running consumer draws ~2.2x master's frames AND runs the FULL draw (DrawWorld included) every frame where master's FF loop skips most redraws — so per-SECOND widget garbage is a multiple of master's even at equal per-frame churn; (b) under FF the deferred sim-event dispatch garbage scales with SIM rate (600 fps of GameFrame batches folded into ~150 draw frames), while the collector's per-call budget stays master's ≤5 ms. At 1x (the operator's live scenario) neither multiplier applies with anything like FF magnitude. Candidate refinement if 1x parity turns out to be insufficient in operator dogfood: scale the per-draw-frame GC budget by the number of sim frames consumed in that draw frame (master-faithful: K sim frames ↔ K collect calls). Left open pending operator observation on real games — escalate-not-improvise per the task guardrail.

## 7. Attributable to pre-existing behavior (reported, not fixed)

- BAR's per-draw-frame widget allocation volume itself (the ~110–800 allocs/df measured flag-OFF, LuaUI heap routinely in the 100–700 MB band with the stock collector) is baseline BAR behavior; `doc/lua-gc-sweep-prefetch-todo.md` (main checkout) already documents the resulting sweep cost. Nothing was changed about it.
- The 1.2GB emergency valve (full collect on the draw thread = visible stutter) is BAR's own mechanism.
- UPSTREAM ENGINE CRASH BUG (base branch): `Spring.GetFeatureRootPiece(fid)` on a feature without an instantiated localModel (e.g. map rocks) null-derefs in the live body (`GetSolidObjectRootPiece`: `o->localModel.GetRoot()->GetLModelPieceIndex()`, LuaSyncedRead.cpp:9806, gdb-verified). Found by the new exercise coverage; the driver deliberately skips exercising it (comment at the site). Should be reported upstream; not fixed here (flag-off behavior change out of scope).

## 8. Instruments kept

`test/callout-diff-gate/lua_mem_sampler.lua`, `lua_gc_probe.lua`, `lua_section_probe.lua`, `lua_piece_check.lua` are committed as reusable measurement widgets (inert unless registered/enabled in a datadir's LuaUI config — same lifecycle as the diff-gate driver). No engine-side instrumentation was added; the fix itself carries no logging (the existing `[LuaMemSampler]` widget + BAR's emergency echo are the observability story). The gate datadirs' registrations were reset to disabled after verification.
