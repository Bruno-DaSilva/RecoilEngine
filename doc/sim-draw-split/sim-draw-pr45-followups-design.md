# PR 45 — follow-ups / cleanup (program close-out): as-landed design record

Status: landed 2026-07-10 on `epoch-integration`. The FINAL PR of the sim/draw thread-decoupling program. The engine stack (PRs 42, 43, 44a, 44b, 44c) is complete and gated; this PR does the cleanup, closes the 44a FF open question, consolidates the docs, and compiles the program's deferred-items punch list. Contract: `doc/sim-draw-epoch-publish-plan.md` (the PR-45 row + "Machinery this plan deletes"), `doc/sim-draw-pr43-44-contract-refresh.md` §5/§7/§8/§9. Builds on the four Wave-7 design records.

Scope discipline: this PR is deliberately near-zero-code. The plan's "deletion list" turned out to be already handled by the landing PRs (verified below, not re-deleted); the FF re-measure resolved to a DROP (no knob); the only substantive artifacts are docs. Flag-off byte-identity is preserved by construction (no engine code changed).

## 1. Dead-machinery audit (plan "Machinery this plan deletes" → landed status)

The plan (2026-07-06) listed machinery it expected PR 45 to delete. Landed reality already handled all of it — either DELETED by the landing PR (leaving only documentary comments), or INTENTIONALLY RETAINED as the flag-off/lockstep degenerate form (deleting these would break flag-off byte-identity — explicitly out of scope). Grep + build verified. NOTHING was deleted in PR 45: every item is provably either gone-to-comments or load-bearing.

| Mechanism (plan deletion list) | Landed status | Evidence |
|---|---|---|
| `sanctionedLive` table + strict-mode deny path | RETAINED as blessed EMPTY tripwire (contract §5.1) — zero real entries; `DenyLiveRead` survives to catch future unclassified callouts, should never fire | `LuaSplitContract.cpp:101` set is comment-only; §5.1 |
| `ScopedLiveException` on barrier/valve/a0 dispatches | DELETED down to the 8 audited survivors — `barrierLive` gone (44b), `valveLive` gone (44b §7); survivors = pregame `prePublishFallback` ×3 + armed dual-run `liveLeg` ×3 + query-reply-parked `simParkedInline` ×2, ALL in `LuaSnapshotServe.cpp` | grep: exactly 8 construct sites; the class + depth counter stay |
| `SetBoundaryShellWindow` + `ShellFallbackUnit/Feature` + RenderEventQueue dispatch-time id→shell maps | `ShellFallbackUnit/Feature` DELETED (44b §2, comment-only in `LuaUtils.h:499`). `SetBoundaryShellWindow` RETAINED as the lockstep/valve degenerate form (the flip uses `OpenEpochDispatchWindow`/`CloseEpochDispatchWindow`, 44b §1c); dead-shell maps RETAINED (they feed the §7.7 producer extraction + 44b §3.8 `BoundaryUnitAliveAtDrain`) | `Game.cpp:1727/1998` (lockstep); `SimDrawSplit.cpp:48-77` |
| `BuildSplitResolveCache` / `splitResolveCache` (units/features/projectiles) | DELETED (SCOPE-1 units/features + Projectile-pass PR). Comment-only everywhere; also gone: `RegisterExtraSplitResolveIDs`, `SplitResolveCacheBuilt`, `ResolveSplitCachedObject` | grep: 0 code lines, comments only |
| `MarkMutatedOutsideFrame` freshness pokes | REPURPOSED, not deleted: the plan meant "ctrl pokes give freshness by construction" (true — the poke queue drains on the sim thread pre-extraction, 44a p1), but the SAME method is now the producer's net-mutation republish trigger (NetCommands re-marks the pending epoch, 44a §3.3). Retained + live | `NetCommands.cpp` ×7 (producer re-mark), `Game.cpp` (lockstep/valve degenerate) |
| End-of-Draw `ReleaseAcked` special case + valve in-place drain | End-of-Draw `ReleaseAcked` RETAINED flag-off-only (`Game.cpp:2665`, gated `if (!SimDrawSplit::Enabled())`); the split rekeyed release to epoch retirement (43 §2.2). Valve in-place drain FULLY DELETED (44c: `ServicePoolValve`/`Once`, `ParkAtValve`, `ResumeFromValve*` — grep empty). `InvalidateStagedEffectContainers` DELETED (44c) | grep: valve-drain symbols return 0 hits |
| `LastBoundaryFrame` plumbing | DELETED (44c) — `LastBoundaryFrame()`/`PublishBoundaryFrame` gone; backpressure is `UnretiredEpochCount()`. Comment-only references remain | grep: 0 code lines, comments only |

Stale-TODO sweep: no TODO/FIXME points at a now-landed PR (the one `PR 27b` HACK note in `Player.cpp:144` is a live deferral rationale, correct as-is). No orphaned functions/members found. Conclusion: the plan's deletion payoff was collected incrementally by the landing PRs; PR 45 deletes nothing (conservative + correct).

## 2. FF wall-time re-measure (closes the 44a open question)

44a flagged that full-length FF wall-clock regressed under the flip (extraction paid at the draw-consume rate on the sim thread; 44a headful Rosetta 5:58 vs PR-43 4:05), with a producer min-publish-interval knob pre-scoped as the candidate fix (`doc/sim-draw-pr44a-producer-flip-design.md`). 44c's backpressure rekey appeared to largely recover it (strict Rosetta 3:26). PR 45 measures it properly: flag-ON vs flag-OFF, SAME replay (Rosetta), SAME mode (headful), FF full-length, serial + quiet, apples-to-apples via the diff-gate runner.

- **flag-OFF (headful, armed diff-gate, Rosetta FF full-length):** **205.6 s (3:26)** wall, f=44537.
- **flag-ON (headful, strict `SplitDrawContract=2`, Rosetta FF full-length):** **177.6 s (2:58)** wall, f=44544.
- **ratio ON/OFF = 0.864** → flag-ON FF is **14% FASTER than flag-OFF**, i.e. not merely "within ~15% of" — the 44a regression is fully recovered and inverted (the sim thread's extraction overlaps the draw loop instead of serializing with it). **DECISION: DROP the producer min-publish-interval pacing knob — resolved by 44c's backpressure rekey.** The knob idea (44a design record, "FF wall-time note") is retired; if FF throughput ever regresses again the knob's pre-scoped design remains in that record, and the alternative lever (MT extraction with a fork-join fence) is punch-list item 15.

Trajectory for the record (headful Rosetta FF full-length, flag-ON): PR 43 ≈ 4:05 → 44a 5:58 (the regression: extraction paid at draw-consume rate, both loops serialized by the residual park) → 44c 3:26 → **PR 45 re-measure 2:58 flag-ON vs 3:26 flag-OFF on the same binary/day/machine**.

Method note (why Rosetta headful for BOTH legs, not the gate's default ATG-headless flag-OFF): item 2 requires a same-replay flag-ON-vs-flag-OFF comparison, and headless drops the GL4 draw entirely (the flip's draw thread does no real work headless), so a headless flag-OFF would not be comparable to a headful flag-ON. Byte-identity of the (empty) deletions is replay-agnostic and already proven ATG-flag-OFF by 44c on this exact tree; the armed flag-OFF Rosetta leg re-confirms it (PASS 0 mismatches).

Why 44c recovered it: 44a's residual consume-park serialized the producer against the whole consume window every draw frame (extraction at draw rate, ADDED to the draw frame); 44b removed the park (overlap begins) and 44c's N-1-epochs backpressure plus retirement-stamped pacing let the free-running sim consume NEWFRAMEs while extraction is skip-gated — extraction still happens at consume rate, but concurrently with rendering instead of inside it.

## 3. Config / telemetry polish

- **Epoch-ring depth N** is a named, commented constant: `SimSnapshot::EPOCH_RING_SLOTS = 3` (`SimSnapshot.h:1342-1345`), with the rationale comment. Making it a runtime config knob is NOT trivial (the six row-namespace buffers + slot-meta are fixed-size C arrays `[EPOCH_RING_SLOTS]`, plus per-slot cmd/piece/mirror caches — a runtime N touches allocation, sizing and the backpressure gate) → left as the constant; noted as future work below, not plumbed.
- **Telemetry survives and is documented in one place:** the consolidated "Telemetry" subsection of `doc/sim-draw-thread-decoupling-research.md` (Wave 7 section) lists `/epochstats` + `[EpochStats]`, `[SimParkStats]`, `[SimPauseSurvey]`, `[PoolValveStats]`, `[BackpressureStats]` with what each measures and when it fires. All verified present on the tree (`Game.cpp:2437-2473`, `SimSnapshot.cpp:618-692`, `DeferredObjectDeleter.cpp:387`, `UnsyncedGameCommands.cpp:1562`).

## 4. Doc consolidation

Appended a "Wave 7 / epoch flip — LANDED" section to `doc/sim-draw-thread-decoupling-research.md` (the program architecture doc) — the final producer/consumer/ring/valve/backpressure/serving-surface architecture, the headline numbers, pointers to the four Wave-7 design records + the §7/§8/§9 rulings, the telemetry index, and the deferred/watch items. The historical sections were left intact (appended, not rewritten). The wave7 prompts file's dependency-order status lines were updated to mark PR 45 and the program close.

## 5. Deferred-items punch list (the program's open items — operator reads this)

Every explicitly deferred / non-blocking / accepted-park / future-work item still open across the program docs, with its trigger condition. None blocks the stack; each fires only under its condition.

### A. End-of-stack verification (operator directive — deferred to whole-stack level, all landing PRs skipped these per the lean-gate ruling)
1. **TSan segment** over the new cross-thread machinery (producer, epoch ring acquire/release, valve retirement-wait, SYNCED-globals mirror). *Trigger: end-of-stack, before merge.* Build `USE_TSAN=ON USE_MIMALLOC=OFF`, sandbox off, DISPLAY :0 for resim/diff-gate (per the TSan-segment recipe); ~100× slow.
2. **DEBUG-headless** replay leg (waived during sessions; run once at stack end).
3. **Multi-run headful matrix** — both replays × ≥2 flag-ON runs, event-time classes repeated, orchestrator-instrumented. *Trigger: end-of-stack.* The 38b widget-error class is now deterministic by construction (id-coverage gate + twins), so this is statistical confirmation.
4. **Operator's live windowed skirmish (§7.8)** — the sole coverage of interactive input→GUI→sim-poke paths replays cannot drive: the `SendCommands` executor-park deadlock check, placement cursor-tracking, and FPS/direct-control camera follow (the §8.1 `AddFPSDirectControlRotY` accumulator). *Trigger: after the whole stack, operator-run.*

### B. Accepted parks / narrow residuals (measured ≈0 in replays; interactive-only)
5. **`GiveOrder` family park** (`LuaUnsyncedCtrl.cpp:3817`) — reviewer-confirmed live reads; kept as an input-gated park (0 in replays). *Trigger: if profiling shows it engaging under real input → the op-capture / id-set-capture net-send design (plan PR 37 class).*
6. **`DrawMapStuff` interior narrow parks** (`GuiHandler.cpp:4007/4077/4203`) — weapon-range / build-preview interior reads with no landed channel (Prereq D residual); whole-pass park already dropped. *Trigger: a per-frame engage (would need its own serving decision) — see item 8.*
7. **Other input-gated parks** — `TryTarget`, `TestUnitBuildSquare` minimap proxy, `GetDefaultCommand` non-cursor fallback, `GetCommand`, `GetBuildPositions`, `MouseRelease`, sim-touching `SendCommands` executors, the operator-ruled `CPathTexture::Update` display-gated park. All lifecycle/input-gated, ≈0 in replays. *Trigger: the operator's skirmish surfaces a hot one.*

### C. Serving follow-ups (channels not built; demand trace-level or absent in replays)
8. **Weapon-state / range-ring channel** for the residual narrow `TryTarget`/`DrawMapStuff` parks (item 6) — weapon range/target state is not currently served. *Trigger: interactive attack-move/range-ring widgets shown to re-park per frame under real input.*
9. **Nested-table `SYNCED` serving** — the §9 mirror serves SCALAR globals; nested-table / non-scalar first-touch reads take the dispatch-scoped lazy park (~28–35/game, `gui_awards`/`system_info` class). *Trigger: a game that HAMMERS nested `SYNCED` tables per frame (the lazy-park engage count climbs) → the deep-copy serving sub-project (44b §7 option-3 nested half).*
10. **AICallback engine-group callbacks** — the deprecated `CAICallback` engine-group callbacks were scoped OUT of Gap A. *Trigger: an AI/skirmish path using them under the split → a possible narrow group lock (noted in the Gap A spec).*
11. **`SendSkirmishAIMessage`** (plan decision-5) — the last synchronous sim-side AI entry from unsynced; the recommended disposition is boundary-apply + deferred (nil-now) result. *Trigger: a widget calling it at draw time under the split.*
12. **Unsynced `RequestPath` / `PathFinder` object API** (plan decision-1) — trace-level demand; recommended boundary-deferred async (path handle valid one boundary later). *Trigger: a pathing widget exercising it.*
13. **`GetUnitMoveTypeData` full-table** (plan decision-2) — served as the common-subset rows; the full table is interest-flagged / not fully served. *Trigger: a widget reading the uncovered moveType fields.*

### D. Memory / performance revisits (non-gating standing TODOs)
14. **§7.1 memory-sharing revisit** — epochs are flat 3× copies by operator ruling (rules-params full map copies × N, TeamRows strings/maps, moveType blocks). *Trigger: `/epochstats` / `[EpochStats]` shows a channel's per-epoch bytes or resident share is DOMINANT → convert that channel to refcount-shared dirty-versioned blocks (pre-approved by §7.1).*
15. **Extraction cost budget** — summed per-boundary extraction; if it approaches ~1.5–2 ms revisit SoA layout / for_mt extraction on the sim thread (with a fork-join fence vs draw-side pool use — also a candidate FF knob if §2 had gone the other way). *Trigger: per-boundary avg/max telemetry crossing the budget.*

### E. Known deviations / watch classes (flagged, non-blocking)
16. **Lifecycle-park-during-valve mid-frame quiescence** (44c §1) — rare²: a near-pool-cap game AND a concurrent lifecycle event (e.g. checkpoint serialize) would capture a mid-frame state. *Trigger: if it ever matters, fix = save-side retry (release, wait one draw frame, re-park), not a valve redesign.*
17. **No-park-only Point-Light-VBO resize warning class** (`invalid unit/featureID while resizing, Unit Point Light VBO`, gfx_deferred_rendering_gl4) — non-fatal widget diagnostic, no downstream lua errors, absent flag-OFF and on Rosetta. Same lights family as the operator-accepted ≤4 instancevbo zombie-echo budget. *Trigger: the end-of-stack matrix — confirm it stays diagnostic-only.*
18. **Backpressure looseness under consumer stalls** (44c §2, §5.13-blessed) — the sim may consume at the server arrival rate rather than freezing at boundary+1 during a draw stall (equivalent-or-better). *No action; documented.*
19. **BAR upstream `gfx_decals_gl4.lua:564 table index is NaN`** (~f=39535, flag-OFF Rosetta) + the `gui_build_eta.lua:151` feature-loop nil guard gap — pre-existing STOCK BAR widget bugs, not branch-caused. *Trigger: report upstream to BAR; the decals-NaN is the flag-OFF baseline the gates subtract.*

## 6. Gate results

| Leg | Result |
|-----|--------|
| `ninja -C build -j28 engine-legacy` | rc=0 (clean HEAD, no code change) |
| Armed diff-gate flag-OFF full-length, Rosetta headful FF (`DG_ARM=1`, `SimDrawSplit=0`) | **PASS (0 mismatches)**, rc=0, full length (f=44537), **0 DESYNC**; the only lua error is the KNOWN flag-OFF baseline (`gfx_decals_gl4.lua:564 table index is NaN` at f=39685, exactly 1×); wall 3:26 |
| Strict flag-ON full-length, Rosetta headful FF (`SplitDrawContract=2`, `SimDrawSplit=1`) | rc=0, full length (f=44544), **0 DESYNC, 0 lua errors** (decals NaN correctly absent flag-ON), **0 `Invalid Feature id`, 0 denials, idCoverage checked=186,036 violations=0**; `[SimParkStats]` parks=29 totalMs=200.8 avgMs=6.9 maxMs=40.0 (lifecycle-only — matches 44c's 29/193.8 baseline); `[SimPauseSurvey]` LUA_SEND_COMMANDS=5 only; mirror-served 44,535 / lazy-park 36; `[BackpressureStats]`/`[PoolValveStats]` absent (0 ring blocks, 0 valve engages); 0 zombie echoes, 0 VBO-resize warnings; wall 2:58 |

Both legs serial on the quiet machine, bar-data2, binary = worktree HEAD `f9c4f02a94` (`Spring Engine Version: 2026.06.10-112-gb40d5ef epoch-integration` in both infologs). GATE-SHAPE NOTE: the lean-gate spec named ATG for the armed flag-OFF leg; it ran on ROSETTA instead so the same two runs double as the item-2 FF timing source (per the "reuse those runs' wall-times" directive) — item 2 requires same-replay same-mode legs, and headless drops the GL4 draw entirely, so a headless/ATG flag-OFF leg would not be FF-comparable to a headful flag-ON one. Byte-identity risk is nil regardless: PR 45 changes ZERO engine code, and the ATG armed flag-OFF PASS on this exact HEAD binary is 44c's own gate (its design record §4).

SKIPPED per the operator lean-gate directive (end-of-stack items, list §5.A): TSan, DEBUG, the multi-run matrix, the live skirmish.
