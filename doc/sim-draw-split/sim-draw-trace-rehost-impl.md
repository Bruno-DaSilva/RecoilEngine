# Sim/draw split — trace re-host IMPLEMENTATION audit + plan

## STATUS (2026-07-11)
- **Stage 1 LANDED** (`977d10b072`): read-set delta rows. Armed diff-gate PASS (unit:traceOcc 654586/0, wpn:trace 284598/0, 0 DESYNC).
- **Stage 3 LANDED** (`79da6d9cfd`): `WeaponPredicates.h` templated stack + LiveView + member-fn wrappers. Flag-off resim BYTE-IDENTICAL both replays (Rosetta 0 DESYNC; ATG == pre-existing f=44760 baseline). Two bugs caught: (a) Missile/Starburst also override GetAimFromPos→muzzle; (b) single-arg TryTarget forwards preFire=FALSE not true.
- **Stage 2b LANDED** (`85957dea4b`): dormant draw-side backend utilities — object-free `CCollisionHandler::DetectHit(midPos,relMidPos,isInVoid,v,m,p0,p1,cq,forceTrace)` + `SnapshotPickGrid::QueryCone`. Zero callers (grep-confirmed) ⇒ flag-off byte-identical by construction; recipe-1 Rosetta resim clean (0 DESYNC, game over f=44537). Bit-exactness deferred to 4b's armed dual-run.
- **Stage 2a COLLAPSED (code-verified, not a separate commit):** feature real colVol is ALREADY demand-captured in the piece cache (`ObjectPieceSlot::colVol`, `captureColVol=true` at both unit+feature call sites; per-piece `pd.pieceColVol` too). Feature transform is reconstructable from `FeatureRows` (`transMatrix = ComposeMatrix(pos) = CMatrix44f(pos,-rightdir,updir,frontdir)`; the matXdir/matYdir/matZdir columns ARE those). Unit transform = `CUnit::GetTransformMatrix() = ComposeMatrix(pos)`, NO interpolation, derivable from UnitRows. ⇒ no standalone capture/diff-gate commit; the only capture-adjacent work left is REGISTERING trace-candidate ids into the existing piece read-set (`pending*PieceRegs` + `RefreshPieces`), done inside the Stage-4b primitives (first-touch lazy-park covers the same-frame miss).
- **Stage 4a LANDED** (`37573673cf`): `TraceEpochView.h` (trace::EpochView, full View interface from SoA+UnitRows; collision/cone/ground primitives STUBBED to "clear LOF") + `EvaluateTraceQueryEpoch` (mirrors EvaluateTraceQueryLive) + armed snap leg rewired Live→Epoch + `ExerciseTrace()` driver. Armed dual-run (Rosetta flag-off, f=44537, 0 DESYNC): **TestTarget 0-mismatch (scalar half exact), every other family 0** (no regression); TestRange 46 / TryTarget 110 / HFLOF 14578 mismatches = clean `live=false snap=true` boolean diffs, exactly the stubbed-primitive surface. flag-ON splitRunning leg still on the PR-35 channel (deferred to 4b).
- **Stage 4b-1 LANDED** (`9126be6ebc`): ground primitives (CGround synced=false + TraceRayGroundDist). HFLOF 14578→9858.
- **Stage 4b-2 LANDED** (`543cfd54e5`): TargetBorderPos + base TraceRay/TestCone (out-of-line in LuaSnapshotServe.cpp; demand piece-cache colvols + reconstructed synced transform; `GetPointSurfaceDistance(mv,p)` made public). Armed: TryTarget 108→**4**, HFLOF 9858→**7168**, TestTarget **0**. Piece-tree candidates deferred. NOTE: the ≤2 `GetUnitPiece*` diff-gate flaky (live=1 snap=0, POV window) is PRE-EXISTING (reproduced with ExerciseTrace off) — see [[project_piece_callout_pov_flaky]], don't chase.
- **Stage 4b-3 LANDED** (`20c3e026ef`): Cannon TrajectoryGroundCol (CGround synced param) + TestTrajectoryCone + MissileTrajectoryLOF (pursuit-curve). Armed: HFLOF 7168→**474**, TestTarget **0**, TestRange 48, TryTarget 6, 0 DESYNC. All HFLOF weapon classes wired; **99.8% bit-exact**.
- **RESIDUAL characterized (instrumented run):** ~518 mismatches (0.19%) dominated by PIECE-TREE colvol deferral — TARGETDIAG breakdown: HFLOF variant 6/3 (lead-pos forms) on BeamLaser (332) + MissileLauncher (110); Cannon TestRange enemy-form (48); both directions (327 over-block / 194 under-block). Root: `TargetBorderPos` returns rawPos for piece-tree targets (wrong `GetUnitLeadTargetPos` → variant 3/6 HFLOF + TestRange-enemy diverge) + TraceRay/cone missing piece-tree obstacles. TestTarget=0 (pure scalar) proves the scalar half perfect.
- **Stage 4b-4 LANDED** (`6a9fdde8e4`, ruling = build per-piece): per-piece `IntersectPieceTree` draw-side over the demand per-piece cache (`pd.pieceColVol` + `pd.modelSpaceMat` + new `pd.scriptVisible`); `EpochPieceTreeIntersect` + `EpochDetectHit` dispatcher; `CCollisionHandler::Intersect(v,m,p0,p1,cq)` made public. Result: **TestTarget/TestRange/TryTarget all 0 (bit-exact)**, HFLOF 474→328.
- **Stage 4b-5 LANDED** (`2d45046edc`): exact-ray trace broadphase — replaced `QueryCone` (wrong model) with `SnapshotPickGrid::QueryRayExact` (halo=0, == `GetQuadsOnRay`; live's cone/parabola widening is per-candidate, not broadphase). HFLOF unchanged (~316) — proving the residual is NOT the gather.
- **RESIDUAL fully diagnosed:** HFLOF ~316 (0.11%), HFLOF-only. Two experiments RULED OUT broadphase (QueryRayExact halo=0 + a GetQuads radius-membership filter both left it unchanged). A `synced=true` ground-read test dropped it **316→68**, proving **~248 (78%) is the unsynced-heightmap ground-ray-march deviation** (`LineGroundCol`/`TrajectoryGroundCol`/missile `GetApproximateHeight` read the draw-side heightmap, ≤1-frame stale vs live's synced during terrain deformation — INHERENT to the split; `synced=true` is a sync violation flag-ON, reverted). The remaining ~68 (0.024%) is the smaller pick-grid-broadphase / off-map-cone inherent deviation. ALL residual is inherent advisory-UI draw-side approximation (same classes placement/pick-grid already carry); TestTarget/TestRange/TryTarget are fully bit-exact.
- **Stage 4b-final LANDED (retirement):** flipped the `RouteTraceQuery` splitRunning leg from `ServeTraceQuery` (PR-35 queue) to a synchronous `EvaluateTraceQueryEpoch` (mirror `RoutePlacementQuery`). DELETED the whole PR-35 channel: `WeaponTraceQueryEq/Hash`, `StandingTraceKey`, the queue+mutex+reply-map data members, `EvaluateTraceQueryLive`, `ServeTraceQuery`, `EvaluateTraceQueries`, `ClearTraceQueryChannel`, `EvaluateQueriesAtSimEdge`, `CommitStagedQueryReplies` (both now trace-only after the placement retirement); the 4 Game.cpp barrier calls (lockstep 3c/3d, flip commit, 2× sim-edge — surrounding blocks that keep default-cmd/synced-mirror work preserved); the `ClearTraceQueryChannel()` teardown call; header decls; the LuaSplitContract.cpp sanction note. KEPT `WeaponTraceQuery` POD (now just the parsed-args carrier), `BuildTraceQuery`, `TraceKind`, `RouteTraceQuery`, `EvaluateTraceQueryEpoch`. Net −393/+108 lines across 5 files. GCC build clean; armed Rosetta dual-run (flag-off, f=44537, **0 DESYNC**): **TestTarget/TestRange/TryTarget 0 mismatched (bit-exact), HFLOF 310/273040 (0.11%)** = the unchanged inherent residual → retirement introduced NO regression (matches 4b-5). Remaining: strict/headful flag-ON gate + operator live skirmish; Stage 0 (GuiHandler weapon-range park) interactive-only, last.
- **Stage 0 DONE — all 3 GuiHandler sim parks retired (2026-07-11; user "plan it and complete them", reversing the earlier same-day defer).** Two gated commits:
  - `5f3c6ccd63` **Stage 0 (1/N)**: Site 3 (`:4203` attack rings) + Site 1 (`:4007` hover rings). glBallisticCircleImpl factored into a range-functor core + a served overload; `SplitServedWeaponRange2D` = GetLiveRange2D over `trace::EpochView` (GetRange2DT, TestRange-gated); 2 net-new UnitRows scalars `decloakDistance`+`stockpileIsInterceptor` (+`WeaponCount` accessor) wired resize/extract/dead-shell/accessor/diff-gate(unitScalars,wpn:unit)/hash(w[103]+wpnAcc); `DrawUnitDefRanges` takes served sensor scalars not a `CUnit*`; splitRunning branches read rows, flag-off keeps the live path under the now-no-op park, cheat+debug `DrawWeaponArc` in a narrow nested park.
  - `4cc449650d` **Stage 0 (2/N)**: Site 2 (`:4077` build preview). Builder circles via a UnitRows my-team-builder scan; `GetServedOverlapQueued` twin over the barrier command-queue cache (`CCommandAI::GetOverlapQueued(c,q)` made static + body factored into a file-local template shared by the CCommandQueue and new std::vector<Command> overloads — no fork); build-square verdict via `placement::TestUnitBuildSquareUIT<EpochView>` (reuses the TestBuildOrder-gated `TestBuildSquareT` + EpochView `HasNearbyGeoFeature`/`BuildHeight`), called by `CUnitDrawer::ShowUnitBuildSquare` under the split. Outer park dropped; `GetBuildPositions` keeps its own separate nest-safe `GUI_GET_BUILDPOS` park (a distinct input-gated circle-build item, not part of Stage 0).
  - **Gates:** GCC build; armed Rosetta diff-gate flag-off (f=44537) 0 DESYNC on both commits, every placement/row/callout pass 0 mismatch (only the inherent HFLOF residual + the pre-existing `GetUnitPiece*` POV flaky non-zero). All 3 blocks are INPUT-GATED (shift-hover / attack-cmd / build-cmd) so no replay exercises the flag-ON served draws — the per-square/range predicates are already gated (TestBuildOrder / TestRange), the sim-side refactors are flag-off byte-identical, and end-to-end validation is the operator's **live skirmish** (hover → range/sensor/decloak/interceptor rings; attack-cmd+selection → attack rings; build-cmd over terrain → build-square coloring/overlap/builder circles).
- **Stage 4b (superseded)**: wire the primitives bit-exact — ground mirror (GroundHeightReal/ApproxHeight/HeightAboveWater), TargetBorderPos (demand colvol + 2b DetectHit + derived transform), TraceRay×2 + TestCone + TrajectoryGroundCol + TestTrajectoryCone (port over QueryRay/QueryCone + per-candidate DetectHit + ground march), MissileTrajectoryLOF (pursuit-curve scan). Then flip splitRunning leg + retire channel. Stage 0 interactive last.
- Ruling: full re-host, staged; demand-capture for colvols.
- Build note: `build/` was left configured for clang (desync trap); reconfigured to the GCC toolchain (`toolchain/gcc_x86_64-pc-linux-gnu.cmake`) — pure GCC 13.3.0 binary required for every determinism gate.

---


Working doc for executing `sim-draw-trace-rehost-plan.md` directly on `bruno/poc-split-sim-draw`.
Written 2026-07-11 after the full read-set + channel + virtual-surface mapping. The
companion `sim-draw-placement-rehost-*` series (commits `1feb4e172e`..`bc6093e852`) is the
binding template: stage 1 rows → stage 2a/2b templatize (LiveView) → stage 3 EpochView +
serve → stage 4 retire channel.

## A. What the four callouts reach (verified)

Entry points `rts/Lua/LuaSyncedRead.cpp:6146/6198/6242/6286`, each a thin wrapper over
`LuaSnapshotServe::RouteTraceQuery(L, __func__, &<Live>, TraceKind::<K>)`. The channel to
retire is `rts/Lua/LuaSnapshotServe.cpp:9492-10024` (+ producer/consumer `10475-10537`, 4
`Game.cpp` barrier call sites `1893/1898/2168/2205`, decls `LuaSnapshotServe.h:604-656`).
The single function doing real work against live objects is `EvaluateTraceQueryLive`
(`9693-9747`): it calls the **virtual** `weapon->TryTarget/TestTarget/TestRange/
HaveFreeLineOfFire` + non-virtual `GetUnitLeadTargetPos`/virtual `GetAimFromPos` against
`unitHandler.GetUnit(...)`. The re-host replaces exactly this with an EpochView instantiation.

Predicate call graph (all `rts/Sim/Weapons/`):
- `TryTarget` (non-virtual) → `TestTarget`→`TestRange`→`GetAimFromPos`→`HaveFreeLineOfFire`
  (+ a `CGround::GetHeightReal` muzzle-below-ground check).
- `TestTarget` reads `weaponDef`(manualfire/canAttackGround/interceptor/interceptSolo/
  waterweapon), `onlyTargetCategory`, `owner`(unitDef->canManualFire/allyteam/fireState/
  IsUnderWater), target(`category`/`isDead`/`IsCrashing`/`losStatus[allyteam]`/`IsNeutral`/
  `allyteam`/`GetTransporter`/`pos`), globals `teamHandler.Ally`, `modInfo.*`,
  `CGround::GetHeightReal`. Overrides: BombDropper, TorpedoLauncher, NoWeapon.
- `TestRange` reads `aimFromPos`, `weaponDef`(cylinderTargeting/heightmod), `range`,
  `mainDir`, `owner->GetObjectSpaceVec`; `GetRange2D` (virtual: Cannon/Starburst),
  `CheckTargetAngleConstraint`(onlyForward/maxForwardAngleDif/maxMainDirAngleDif/frontdir).
  Overrides: BeamLaser, LightningCannon, BombDropper.
- `HaveFreeLineOfFire` reads `avoidFlags`, `damages->damageAreaOfEffect`,
  `AccuracyExperience()`+`SprayAngleExperience()` (→ `owner->limExperience`,
  `weaponDef->ownerExpAccWeight`), `owner->allyteam`; calls `TraceRay::TraceRay`/`TestCone`.
  Overrides: **Cannon/MissileLauncher/StarburstLauncher** (ballistic — trajectory cone),
  BombDropper/Melee/PlasmaRepulser (return true).
- `GetLeadTargetPos` (non-virtual) → `GetUnitLeadTargetPos` → `GetUnitPositionWithError`
  (owner->allyteam, errorVector, `unit->GetErrorPos`, aimPos, pos, speed.w, `MoveErrorExperience`) +
  `GetLeadVec` (predictSpeedMod, weaponDef->predictBoost/leadLimit/leadBonus, accurateLeading,
  unit->speed/pos, owner->experience, virtual `GetPredictedImpactTime`: BeamLaser/BombDropper/
  Rifle/LightningCannon) + `GetTargetBorderPos` (weaponDef->targetBorder, weaponMuzzlePos,
  `targetUnit->collisionVolume`, `targetUnit->GetTransformMatrix()`, `CCollisionHandler::DetectHit`).

Split-sensitive gotchas: (1) `CCannon::GetAimFromPos` overrides to always return
`weaponMuzzlePos`; (2) `CMissileLauncher::HaveFreeLineOfFire` has an unsynced
`globalRendering->drawDebugTraceRay`/`geometricObjects` debug block (MissileLauncher.cpp:165-169)
that must be excluded from the epoch path; (3) `HaveFreeLineOfFire`'s ground trace uses
`CGround::LineGroundCol(...synced=true)` by default — draw-side must route the unsynced
heightmap mirror (the placement `BuildHeight(synced=false)` deviation precedent).

## B. Stage 1 — read-set delta vs the landed PR 31 per-weapon SoA (`SimSnapshot.h:607-723`)

PR 31 already carries (reusable as-is): `wRange`, `wProjectileSpeed`, `wSalvoSize`,
`wSalvoDelay`, `wAvoidFlags`, `wMuzzlePos`, `wWeaponDir`, `wAccuracyExp`, `wSprayAngleExp`,
`wMoveErrorExp`, `wSalvoError`, `wDamages` (→ damageAreaOfEffect), `wProjectileType`,
`wTargetType/UnitID/GroundPos`. Per-unit `UnitRows` already carries: `fireState`,
`physicalState` (IsUnderWater/IsInWater/IsMoving), `frontdir/rightdir/updir` (GetObjectSpaceVec),
`heading`, `experience`, `limExperience`, `useHighTrajectory`, `speed`, `pos`, `aimPos`,
`midPos`, `posErrorVector`, `losStatusAll`, `allyTeam`, `neutral`, `isDead`, `maxRange`,
`selVol` (selection volume only), `moveDefID`, `DefID`.

Per-weapon fields MISSING (add to the SoA, with SnapshotHash + a SnapshotDiffGate field pass):
- `wWeaponClass` (uint8 discriminator: Base/Cannon/MissileLauncher/StarburstLauncher/BeamLaser/
  LightningCannon/BombDropper/TorpedoLauncher/Melee/PlasmaRepulser/NoWeapon/Rifle) — drives the
  draw-side virtual dispatch.
- `wWeaponDefID` (int32) — to read the immutable `weaponDef` scalars draw-side directly
  (thread-safe, like `unitDefHandler` in placement EpochView): manualfire, canAttackGround,
  interceptor, interceptSolo, waterweapon, heightmod, cylinderTargeting, targetBorder,
  predictBoost, leadLimit, leadBonus, targetMoveError, ownerExpAccWeight, myGravity,
  trajectoryHeight, projectilespeed, startvelocity, weaponacceleration, fixedLauncher,
  highTrajectory, beamburst, heightBoostFactor, rangeBoostFactor, damageAreaOfEffect (also in wDamages).
- `wAimFromPos` (float3 — currently only `wAimFromPosY` is captured), `wRelAimFromPos`,
  `wRelWeaponMuzzlePos`, `wMainDir`, `wOnlyForward` (u8), `wMaxForwardAngleDif`,
  `wMaxMainDirAngleDif`, `wOnlyTargetCategory` (u32), `wPredictSpeedMod`, `wAccurateLeading`,
  `wDoTargetGroundPos` (u8), `wCurrentTargetPos` (float3), `wErrorVector` (float3),
  `wHeightBoostFactor`.
- Subclass-mutable members: `wCannonGravity`, `wCannonHighTrajectory` (u8),
  `wBombDropTorpedoes` (u8), `wBombTorpMoveRange`, `wSalvoWindup` (present as `wSalvoWindup`).

Per-unit fields MISSING: `category` (u32), `crashing` (u8, `CUnit::IsCrashing()`),
`underFirstPersonControl` (u8), `transporterID` (already present as PR 32 `transporterID`).
`GetErrorPos`/error handling reuses `posErrorVector` + the LOS gate (mirror `ErrorVector`).

## C. Stage 2 — collision / trajectory backend (the hard part; NOT covered by rows)

1. **Real collision volumes.** Traces use the object's REAL `collisionVolume`, not `selVol`
   (only `selVol` is snapshot-held, PR 25). Add `colVol` (+ real `relMidPos`/`midPos` already
   present) to UnitRows/FeatureRows, OR demand-capture per the piece-cache precedent. Ruling
   needed (plan §8): eager vs demand — measure via `/epochstats`.
2. **Object transforms.** `GetTargetBorderPos` + `TraceRay`/`DetectHit` need
   `GetTransformMatrix()` per candidate. The landed pick path (`GuiTraceRay`, TraceRay.cpp:433-500)
   reads the DRAWER's unsynced transform + `MouseHit(relMidPos,isInVoid,...)` snapshot overload
   for the precise phase — reuse that; audit whether arbitrary trace targets (not just picked)
   are all drawer-registered.
3. **Per-piece hit volumes — GENUINE GAP.** `CCollisionHandler::DefaultToPieceTree` volumes are
   **deferred to a live read even in the landed GuiTraceRay path** (TraceRay.cpp:492-500). Per-piece
   hit-detection units need `IntersectPieceTree` draw-side. Extend the demand-driven piece cache
   (which already has colvols) to trace candidates. This is net-new infra, the largest Stage-2 item.
4. **Broadphase.** `SnapshotPickGrid::QueryRay` (with width halo) is landed and reused for the ray
   forms. `TestCone`/`TestTrajectoryCone` need a cone gather — verify the halo model is a
   conservative superset or add `QueryCone` (the grid must never change the winner, only widen the
   candidate set — keep that contract).
5. **Ground march.** `CGround::LineGroundCol` + `TrajectoryGroundCol` over the **unsynced**
   heightmap mirror (DrawMapMirrors), a documented ≤1-boundary staleness deviation.

## D. Stage 3 — templatize the predicate stack (LiveView), mirroring PlacementPredicates.h

New `rts/Sim/Weapons/WeaponPredicates.h`: `namespace trace`, templated pure functions
`TryTargetT/TestTargetT/TestRangeT/HaveFreeLineOfFireT/GetLeadTargetPosT<V>(const V& view, ...)`,
plus the per-subclass override bodies as `TestRangeCannonT` etc. dispatched on `wWeaponClass`.
`CWeapon`/subclass member functions become thin wrappers calling the `LiveView` instantiation
(byte-identical — these run in the hottest synced aim loops). `LiveView` (globals + live
`CWeapon*`/`CUnit*`) defined at the bottom of the header. **This is the sync-risk step:
flag-off resim byte-identity on BOTH replays gates it before anything else.**

Virtual-dispatch handling: `TestTarget`/`TestRange`/`HaveFreeLineOfFire`/`GetRange2D`/
`GetPredictedImpactTime`/`GetAimFromPos` become `switch(wWeaponClass)` in the templated body
(one templated free function per override). Enumerate the surface (§A) up front — do NOT discover
overrides in the dual-run.

## E. Stage 4 — trace::EpochView + serve + retire (mirror the placement stage 3/4)

New `rts/Rendering/Common/TraceEpochView.h`: `trace::EpochView` reading the PR 31 SoA (+ Stage 1
additions), UnitRows/FeatureRows, real colvols (Stage 2), DrawMapMirrors ground, SnapshotPickGrid.
Add `EvaluateTraceQueryEpoch(q)` next to `EvaluateTraceQueryLive` (switch on TraceKind, build the
EpochView, call the templated predicates). Rewire `RouteTraceQuery`: the `splitRunning` leg →
build query + `EvaluateTraceQueryEpoch` synchronously (drop `ServeTraceQuery`); the armed flag-off
dual-run snap leg → `EvaluateTraceQueryEpoch` (was `...Live`) so the gate proves epoch == live.
Add an `ExerciseTrace()` driver in `callout_diff_driver.lua` (replays never call these — the
placement rehost's `ExercisePlacement()` precedent). Then delete `ServeTraceQuery`,
`EvaluateTraceQueries`, `ClearTraceQueryChannel`, `EvaluateQueriesAtSimEdge`/
`CommitStagedQueryReplies` (trace parts), the `WeaponTraceQuery` queue/reply maps/mutex/
`StandingTraceKey`, and the 4 `Game.cpp` barrier call sites. Keep `TraceKind` (the query
discriminator, like `PlacementKind`). Convert the GuiHandler weapon-range parks (Stage 0).

Deviation swap (mirrors placement): the reply reflects the CURRENT query args against
≤1-boundary-old world state, replacing the old standing-latest keying + first-frame-default.

## F. Gates (handbook §5)

Recipe 1 (flag-off byte-identity) after each of stages 1–4 — stage 3 is the sync-risk step.
Recipe 2 (armed diff-gate, new field passes + the four callouts' dual-run). Recipe 3 (strict,
zero denials, park sites at zero engages). Recipe 4 (headful flag-ON full-length, both replays,
stack end). TSan segment if the demand piece/colvol capture adds cross-thread signalling.
`/epochstats` before/after for the weapon-state + colvol channels. Operator live-skirmish
re-check of the triggering symptom + the input-gated park sites (replays cannot cover these).

## H. Stage 2 + Stage 4 EXECUTION BLUEPRINT (mapped 2026-07-11, ready to execute)

### Stage 4 serving glue — MECHANICAL, mirror the placement rehost (LuaSnapshotServe.cpp)
- **RouteTraceQuery splitRunning leg** (`~9902-9906`): replace `return ServeTraceQuery(...)` with
  `WeaponTraceQuery q; if (BuildTraceQuery(L,caller,kind,q)==0) return 0; lua_pushboolean(L, EvaluateTraceQueryEpoch(q)); return 1;` (mirror RoutePlacementQuery `10376-10386`).
- **armed flag-off snap leg** (`~9942-9947`): change the one call `EvaluateTraceQueryLive(q)` → `EvaluateTraceQueryEpoch(q)` (mirror `10429`; makes the dual-run prove epoch==live).
- **EvaluateTraceQueryEpoch(q)** — NEW, mirrors `EvaluateTraceQueryLive` (`9693-9747`) control flow but builds a `trace::EpochView view(q.ownerID, q.weaponNum)` + a parallel POD `Target{type,isManualFire,isUserTarget,isAutoTarget,groundPos,unitID}` (from `SWeaponTarget(enemy,pos,true)` semantics: type=enemyForm?Unit:Pos, isUserTarget=true, groundPos=enemyForm?pos:Zero) and calls `trace::TryTargetT/TestTargetT/TestRangeT/HaveFreeLineOfFireT/GetLeadTargetPosT/GetUnitLeadTargetPosT/GetAimFromPosT` (WeaponPredicates.h). `UnitRef=int32` (row id). Target_Intercept accessors assert-unreachable (callouts never build them).
- **KEEP**: `WeaponTraceQuery` struct + `BuildTraceQuery` (now the parsed-args carrier, like PlacementQuery); `TraceKind` enum + `RouteTraceQuery` decl.
- **DELETE** (channel retirement): `WeaponTraceQueryEq/Hash`, `StandingTraceKey`, the data-members block (`9584-9601`), `EvaluateTraceQueryLive`, `ServeTraceQuery`, `EvaluateTraceQueries`, `ClearTraceQueryChannel`, `EvaluateQueriesAtSimEdge`, `CommitStagedQueryReplies`, the `ClearTraceQueryChannel()` call at `8017`. Game.cpp barrier calls at `1893/1898/2168/2205` (each inside a block that keeps other work — delete just the call+comment). Header decls: `LuaSnapshotServe.h` DELETE `EvaluateTraceQueries`/`ClearTraceQueryChannel`/`EvaluateQueriesAtSimEdge`/`CommitStagedQueryReplies`; KEEP `TraceKind`/`RouteTraceQuery`.
- **#includes** into LuaSnapshotServe.cpp: `Rendering/Common/TraceEpochView.h` + `Sim/Weapons/WeaponPredicates.h`.
- **ExerciseTrace()** driver in callout_diff_driver.lua (mirror ExercisePlacement): call the 4 GetUnitWeapon* callouts across units×weapons×(enemy,pos) forms so the armed dual-run + strict have coverage.

### Stage 2 backend — the NET-NEW, bit-exact-critical, LARGE part
`trace::EpochView` (rts/Rendering/Common/TraceEpochView.h, mirror PlacementEpochView.h) needs ALL the View
accessors LiveView has. Scalar/vector half is MECHANICAL (read the PR31+Stage1 weapon SoA + UnitRows;
`GetWeaponClass()` from `wWeaponClass`; `Def()` from `wWeaponDefID` via weaponDefHandler). The trace
primitives are the hard part:
- **Unit synced transform**: DERIVABLE draw-side, no new capture — `CMatrix44f(pos, -rightdir, updir, frontdir)` from UnitRows (== `CSolidObject::ComposeMatrix`).
- **Unit real colvol**: ALREADY captured — demand piece-cache `ObjectPieceSlot::colVol` (LuaSnapshotServe.cpp ~7217/7329). Register trace-candidate ids through the same `pending*Regs` mailbox + `RefreshPieces` walk.
- **Feature real colvol + transMatrix**: NET-NEW capture (features cache a precomputed `transMatrix`).
- **Scalar `CCollisionHandler::DetectHit` overload** (NET-NEW): mirror the scalar `MouseHit(relMidPos,isInVoid,m,p0,p1,v,cq)` (CollisionHandler.cpp:227-257) but add the Collision (non-ray) branch + `forceTrace` — GetTargetBorderPos uses forceTrace=false (Collision) then a ray.
- **`SnapshotPickGrid::QueryCone`** (NET-NEW): no cone query exists (only ray+rect+radius). Interim: QueryRay widened by max cone half-width. Also the grid inserts by `max(radius, selVol.boundingRadius)` — collision traces may want the collision-volume radius (widen insertion or accept selVol superset — DECISION).
- **Ground mirror for the cone/trajectory ray-leg** (TraceRay ground + TrajectoryGroundCol): reuse DrawMapMirrors heightmap (synced→unsynced deviation, the placement precedent).
- The primitives to port bit-exact: `TargetBorderPos`, base HFLOF `TraceRay`(×2)/`TestCone`, Cannon `TrajectoryGroundCol`/`TestTrajectoryCone`, `MissileTrajectoryLOF` — ~500 lines of TraceRay/CollisionHandler logic re-expressed over the snapshot. Each must equal live (flag-off dual-run).

### Sequencing + gates
1. Stage 2a: feature real-colvol + transform demand-capture + diff-gate field pass (armed-diff-gate-able, "no consumer"). 2. Stage 2b: scalar DetectHit overload + QueryCone (dormant utilities). 3. Stage 4a: TraceEpochView scalar half + serving glue + EvaluateTraceQueryEpoch, with trace primitives stubbed → serve the pos-form callouts, gate. 4. Stage 4b: the trace/cone/colvol primitives, iterate to bit-exact via armed dual-run. 5. Retire channel + ExerciseTrace + strict/headful. Final: operator live skirmish.

## G. Open rulings for the operator (plan §8)

1. Scope: full trace re-host vs promote Stage 0 (weapon-range park retirement) alone. Stage 0 is
   input-gated (shift-hover) so ONLY a live skirmish can gate it; it needs `glBallisticCircle`'s
   `CWeapon*` ballistic params (range/projectileSpeed/heightBoostFactor) served — a small subset
   of Stage 1.
2. Colvol capture: eager scalars on Unit/FeatureRows vs demand-capture (memory vs extraction cost).
3. Per-piece hit volumes draw-side (Stage 2.3) is net-new infra — accept the scope, or accept a
   documented deviation where per-piece-hit trace targets fall back (they cannot, under the split).
