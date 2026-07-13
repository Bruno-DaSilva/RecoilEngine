# Sim/draw split — draw-side weapon-trace re-host (plan)

**Status: UNSCHEDULED — evidence-gated, and LOWER priority than the placement plan.** Written 2026-07-10. This is the promotion plan for retiring the PR 35 weapon-trace query/reply channel by re-hosting the trace predicates draw-side over epoch state. Related §8 punch-list items: the "weapon-state/range channel" and the item-6/7 weapon-range narrow parks (`sim-draw-pr45-followups-design.md`). Do not start without an operator ruling.

**Trigger to schedule:** live evidence that the trace tests' one-boundary reply latency is perceptible — e.g. targeting-line / range-check widgets visibly lagging or mis-coloring during combat in the operator's live skirmish, or a game hammering `GetUnitWeaponTryTarget`-family callouts in ways the standing-latest keying mishandles. Absent that, this stays shelved permanently: targeting indicators on moving units are far less latency-sensitive than the build cursor, and the standing-latest model already tracks continuously. Staging step 0 (retiring the GuiHandler weapon-range parks, likely servable from the already-landed PR 31 weapon rows) may be promoted alone on its own trigger (those parks engaging under real input) without the full trace re-host.

**Companion plan:** `sim-draw-placement-rehost-plan.md` — shares the binding design principle (§3 there): ONE implementation parameterized over a live/epoch state view, never a forked recompute; bit-exactness enforced by the armed flag-off dual-run where epoch state == live state; the only flag-on deviation is bounded input staleness. Read that section first; it is not repeated here. Its §0 read-first preamble (worktree, handbook pointers, PATH CORRECTIONS — `SimSnapshot`/`SnapshotHash`/`SnapshotDiffGate`/`DrawMapMirrors` live in `rts/Rendering/Common/`, not the handbook §2's `rts/Sim/Misc/`) applies here verbatim.

Live-code anchors (verified 2026-07-10): the trace channel contract comment at `rts/Lua/LuaSnapshotServe.h` (block starting ~line 604, "PR 35"); `CWeapon::TryTarget/TestTarget/TestRange/HaveFreeLineOfFire` at `rts/Sim/Weapons/Weapon.cpp:954/1020/1087/1112`; the snapshot-backed pick path at `rts/Game/TraceRay.cpp:433-462` (`SnapshotPickGrid::QueryRay` + snapshot rows); the PR 31 per-weapon SoA at `rts/Rendering/Common/SimSnapshot.h` ~lines 582-700.

## 1. What exists today

`Spring.GetUnitWeaponTryTarget` / `TestTarget` / `TestRange` / `HaveFreeLineOfFire` are served by the sim-side trace query/reply channel (PR 35, `LuaSnapshotServe.h` ~line 604 block; the contract-refresh doc's `:561` is a stale line number): flag-on, enqueue + return the last boundary's sim-exact reply; `EvaluateTraceQueries()` evaluates the EXACT live `CWeapon` predicates at the frame edge. Flag-off, the live body runs inline. Keying (PR 43 §7.3 ruling): enemy-form queries keyed exactly; pos-form/cursor queries use the standing-latest-query model (≤1 boundary late, never-default). Deviations carried: ≤1-boundary-old position; same-key same-frame collisions last-wins.

Also today: the `TryTarget` input-gated narrow park and the GuiHandler weapon-range interior parks (`GuiHandler.cpp:4007/4077/4203`, range part) — ≈0 engages in replays, only real input can light them up.

## 2. Scope reality check: who benefits (finding from 2026-07-10 review)

`TraceRay`/`TestCone`/`CollisionHandler` are core engine subsystems, not debug tooling — but almost every draw-side consumer is ALREADY served:

- **Picking** (`GuiTraceRay`, minimap closest-unit, `TraceScreenRay`, the spatial-list twins) already runs epoch-backed via `SnapshotPickGrid` (draw-side broadphase rebuilt per epoch generation from snapshot position rows, geometry matching `CQuadField`'s 128-elmo cells) + `CCollisionHandler::MouseHit` against snapshot-held selection volumes (PR 25). This is the landed existence proof of the dual-use pattern on exactly this code family.
- **Synced consumers** (weapon fire control every slow-update; `ProjectileHandler` → `CollisionHandler::DetectHit` hit detection every frame; `MissileLauncher`; AI callbacks) stay live sim code REGARDLESS of this plan — they run on the sim thread.

So the beneficiary list of this re-host is exactly: the four advisory Lua trace callouts + the weapon-range park sites. That is why this plan is evidence-gated and second in line — the infrastructure delta is modest (§4) but the payoff is narrow.

## 3. What gets parameterized

The weapon-predicate half of the ray subsystem, per the companion plan's state-view principle:

- `CWeapon::TryTarget(tgtPos, trg, preFire)` → `TestRange` → `TestTarget` → `HaveFreeLineOfFire`, plus `GetLeadTargetPos` (target leading) and the ballistic overrides (`CCannon`/`CMissileLauncher` `TryTarget`/trajectory checks) — whatever the four callouts can reach.
- `TraceRay::TraceRay` (avoidFlags scanning: ground/friendly/neutral/feature filtering) and `TraceRay::TestCone` / `TestTrajectoryCone` (spread cones).
- `CCollisionHandler` precise phase: already effectively dual-use (MouseHit precedent); extend to the `DetectHit` paths the traces use, including per-piece hit volumes.
- Ground segment: `CGround::LineGroundCol` over the mirrored heightmap — audit what the GuiTraceRay serving already built and reuse.

The live instantiation must be zero-overhead: these predicates run inside every weapon's aim/auto-target cycle — the hottest synced loops in combat. Same discipline as the placement plan step 2: mechanical lift, flag-off resim gate, no logic edits in the refactor commits.

## 4. State the epoch backend needs

| State read | Epoch status | Work |
|---|---|---|
| Broadphase candidates along a ray/cone | `SnapshotPickGrid.QueryRay` (with width halo) LANDED | Reuse; verify cone queries fit the existing halo model or add a cone gather |
| Object REAL collision volumes + midPos/aimPos (traces do not use selection volumes) | Selection volumes snapshot-held; real colvols NOT eagerly captured | New eager colvol scalars on Unit/FeatureRows (small POD), or read-set/demand capture per the piece-cache precedent — measure both, prefer demand if eager shows up in `/epochstats` |
| Per-piece hit volumes (per-piece hit-detection units) | Piece cache is demand-driven, has colvols | Extend the existing demand-capture read-set to trace candidates |
| Per-weapon state | live `CWeapon` fields | **MOSTLY LANDED (PR 31):** the per-weapon SoA (`SimSnapshot.h` ~582-700, flat arrays indexed `weaponOffset[unitID] + weaponNum`) already carries wRange, wAccuracyExp/wSprayAngleExp/wSalvoError/wMoveErrorExp, wAvoidFlags/wCollisionFlags, wMuzzlePos, wAimFromPosY, wDefMaxFireAngle, wProjectileSpeed, salvo/stockpile/angleGood state, SWeaponTarget rows, wDamages (DamagesSnap) | **§4a becomes a READ-SET DELTA AUDIT, not a new channel:** diff the four predicates' actual reads against the PR 31 arrays. Known candidate gaps to check: heightMod, full aimFromPos/relWeaponMuzzlePos vectors (only Y is captured), mainDir/onlyForward cone params, predictSpeedMod, onlyTargetCategory/badTargetCategory, `GetLeadTargetPos` inputs. Fill only what's missing |
| Target kinematics for lead (`GetLeadTargetPos`): target pos/velocity | In rows | None |
| Alliance/neutral/allyteam filters (avoidFlags semantics) | Team/alliance rows | None |
| Ground ray march | Heightmap mirrored | Reuse the served ground-intersection path (audit) |

Discipline per new/extended channel: choke-point funnel + grep audit, SnapshotHash membership, SnapshotDiffGate field pass (specs §E.2).

## 5. What gets retired on completion

- The trace query/reply channel (`RouteTraceQuery`, `EvaluateTraceQueries`, `ClearTraceQueryChannel`, `TraceKind`) + its keying deviations, replaced by the same swapped deviation as the placement plan: current query args against ≤1-frame-old world state.
- The `TryTarget` input-gated park and the GuiHandler weapon-range interior parks (staging step 0 alone retires the latter).
- If BOTH this and the placement plan land, the query/reply mechanism disappears entirely; `ClosestBuildPos`-class "reads that must run sim-side" text in the architecture overview §7 must be rewritten, and the two query-reply-parked `ScopedLiveException` survivors (contract-refresh §12) get re-audited.

## 6. Staging

0. **Check whether the GuiHandler weapon-range parks are already servable from the LANDED PR 31 rows** (wRange + wMuzzlePos may cover the range-ring reads as-is). If yes, that park retirement is a small standalone landing needing no new state — do it first and independently of the rest of this plan.
1. **§4a read-set delta audit** (see §4 table): diff the four predicates' reads against the PR 31 SoA; add only the missing per-weapon scalars, with diff-gate field passes.
2. Colvol/piece-volume capture extensions (demand-first).
3. State-view refactor of the live predicate stack (live instantiation only; flag-off resim gate on both replays — the synced-hot-path step).
4. Epoch-backed instantiation; armed dual-run of all four callouts (bit-equal, flag-off); flip the flag-on serving; convert the park sites; delete the channel.
5. Docs/deviation bookkeeping; handbook + contract-refresh updates; operator rulings recorded.

## 7. Gates

Per handbook §5: recipe 1 (flag-off byte-identity) after steps 1–4 individually — step 3 is the sync-risk step; recipe 2 (armed diff-gate) with the new field passes + dual-runs; recipe 3 (strict, zero denials, park sites at zero engages); recipe 4 (headful flag-ON full-length, both replays, stack end); TSan segment (recipe 7) if the demand-capture read-set adds cross-thread signaling; live-skirmish re-check of the triggering symptom. `/epochstats` before/after for the weapon-state and colvol channels.

## 8. Risks and open rulings

- **Hot-loop refactor risk** exceeds the placement plan's (weapon aim loops beat builder loops); the zero-overhead live instantiation must be verified in the sim-frame profile, not assumed.
- **Ballistic overrides:** `CCannon`/`CMissileLauncher` override `TryTarget`/LOF with trajectory math; the parameterization must cover the actual virtual dispatch surface the callouts reach — enumerate overrides first, don't discover them in the dual-run.
- **Cone/trajectory gather:** `TestCone` broadphase over the pick grid needs its own conservative-superset argument (the grid never changes the winner, only the candidate set — keep that contract).
- **Ruling needed:** eager vs demand colvol capture (memory/extraction trade).
- **Ruling needed:** promote step 0 (park retirement on landed rows) alone vs the full plan; PR slotting.

## 9. Non-goals

- No change to synced fire control or projectile hit detection — sim-side callers keep the live instantiation by construction.
- No pick-grid redesign; reuse as-is (its determinism contract is load-bearing for picking).
- No serving of `GetDefaultCommand` (separate mechanism, unchanged).
- No AICallback/AICheats work (the legacy-AI latent item in Addendum D stands on its own).
