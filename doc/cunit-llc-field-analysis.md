# CUnit field-level L3-miss analysis (2026-07-21)

Data-driven field access analysis of the unit memory pool, to inform CUnit / CGroundMoveType field reordering for cache-locality (X3D-class win investigation).

## Method

- Hardware: Ryzen 9 9955HX (Zen 5). Sampling via AMD IBS: `perf record -e ibs_op/l3missonly=1,swfilt=1/u -d`, which records the exact data address of sampled L3-missing loads/stores, user-space only, no sudo (perf_event_paranoid=1).
- Workload: `fightertest corak armpw 650 10 2040` startscript (650v650 ground bots), spring-headless, frames 15..2100 at 20x. Capture covered the whole fight (~34s wall).
- Units live in `unitMemPool`, a `StaticMemPool<32000, 4496>` static BSS array: any sampled address in its range folds to a slot offset via `(addr - base) % 4496`, then to a field via gdb `ptype /o` layout tables (CUnit chain + the placement-new member buffers: `amtMemBuffer` -> CGroundMoveType, `caiMemBuffer` -> CMobileCAI, `usMemBuffer` -> CCobInstance).
- Tooling: `tools/cunit-llc-analysis/` (capture script, layout parser, fold script). Raw artifacts (perf.data, reports) in the session scratchpad; re-run takes ~3 min.

## Engine-wide context

279,107 L3-miss samples total; **36,079 (12.9%) hit the unit pool**. The unit pool is the largest *single object pool*, but not the largest miss source overall:

| subsystem (top symbols) | approx share of all L3-miss samples |
|---|---|
| LocalModelPiece transform updates (UpdateModelSpaceTransform, SetDirty, PieceSpace, ResetWasUpdated) | ~23% |
| unit pool (this report) | ~13% |
| CSimpleParticleSystem::Update | ~11% |
| CProjectileDrawer::UpdateDrawFlags | ~7% |
| threadpool idle/dispatch (wait_for, WorkerLoop, ConcurrentQueue) | ~13% |

So CUnit reordering attacks ~13% of misses; piece-transform storage is a bigger (separate) target for later.

## Field heat (top, % of in-pool misses)

| field | offset | % | notes |
|---|---|---|---|
| mtTempNum | WorldObject+284 (128B) | 12.4% | per-thread visited-markers, WRITTEN by every QuadField scan |
| CAI::commandQue | u+2960 | 9.0% | read every frame by GMT FollowPath/ChangeSpeed |
| pos | WorldObject+224 | 5.2% | QuadField scans |
| GMT::nextPathId | u+2240 | 5.1% | UpdateTraversalPlan (MT pathing mailbox) |
| GMT::jobId | u+2008 | 4.8% | read from threadpool job lambda (MT dispatch) |
| GMT::lastAvoidanceDir | u+2084 | 3.6% | GetObstacleAvoidanceDir |
| physicalState | SolidObject+464 | 3.3% | scans + collisions + UpdateUnitPosition |
| localModel | SolidObject+504 | 3.2% | COB anim writes via LocalModelPiece::SetFloat3 |
| moveType (ptr) | Unit+1272 | 2.3% | job lambda double-hop |
| GMT::avoidingUnits | u+2307 | 2.3% | avoidance |
| speed | WorldObject+236 | 2.3% | UpdateOwnerPos |
| GMT::pushResistant / owner / currentSpeed / waypointDir / atGoal / atEndOfPath / earlyCurrWayPoint / numIdlingSlowUpdates | lines 31-35 | ~10% combined | per-frame GMT core |
| los | Unit+3408 | 1.8% | CLosHandler::Update |
| frontdir/rightdir/updir | SolidObject+1000.. | ~3.6% | weapon vectors, avoidance |
| isDead/beingBuilt/activated/stunned | Unit+4244.. (tail line 66) | ~2.5% | status bools read by LOS + GMT, live on the far tail |

Hottest 64B lines in the 4496B slot: line 35 (GMT pathing-mailbox + idling counters, 5103 samples), line 4 (radius/height/model/drawFlag, 3638), line 3 (pos/speed, 2834), lines 31-33 (GMT waypoint/speed core, ~6600 combined), lines 46-47 (CAI commandQue, ~3250).

## Access-pattern findings (field x function)

1. **QuadField scans** (`GetUnitsExact`, `GetSolidsExact`; ~18% of pool misses): read `pos` (line 3) + `radius` (line 4), then **write** `mtTempNum[thread]` (lines 5-6). Three-plus lines touched per candidate unit, and the write dirties lines adjacent to `pos`.
2. **GMT per-frame core** (`FollowPath`, `ChangeSpeed`, `UpdateObstacleAvoidance`, ~28%): waypoint state (line 31-32), speed params (line 33), goal/idling/flags (line 34-35) - spread over 5 lines that are all touched every frame per unit.
3. **MT pathing mailbox on sim-hot lines**: `jobId` (line 31) is read by threadpool job lambdas (workers) while the same line holds `currWayPoint`/`earlyCurrWayPoint` (main-thread per-frame state); `nextPathId`/`deletePathId` (line 35) share their line with `numIdlingUpdates`/`wantRepath` counters. Cross-thread traffic and per-frame sim state are interleaved on the same lines.
4. **Cross-object read**: GMT consults `CAI::commandQue` (1.3KB away in the slot) every frame (~9%) - the deque header/front command, mostly to answer "is there a follow-up order".
5. **Status bools on the tail**: `isDead`, `beingBuilt`, `activated`, `stunned` sit on line 66 (4224+) but are read by LOS update, GMT update, and collision code that otherwise touch only front-of-slot lines.
6. **LOS update** reads `los` (u+3408), `midPos` (1060), `allyteam` (476), plus the tail bools - four scattered lines per unit per LOS pass.

## Recommendations (ranked by expected impact)

1. **Move `mtTempNum` out of the objects entirely** (design change, not reorder): a per-thread dense array indexed by object id (32000 x 4B = 128KB/thread, cache-resident) instead of 128B inside every unit slot. Removes ~12% of pool misses *and* stops scans from dirtying the pos/radius neighborhood. Applies to features too (CWorldObject).
2. **Pack a "spatial scan header" into one 64B line** near slot start: `id, pos, radius, sqRadius, height, physicalState, collidableState, team, allyteam, heading` (+the status bools, see 4). QuadField scans + collision filters + LOS then touch 1 line per unit instead of 3-4.
3. **Reorder CGroundMoveType**: (a) one line for the per-frame waypoint/speed core (currWayPoint, waypointDir, flatFrontDir, currentSpeed, wantedSpeed, atGoal/atEndOfPath/idling counters); (b) an `alignas(64)` mailbox line for the MT pathing fields (jobId, nextPathId, deletePathId, wantRepath) so worker-thread traffic stops sharing lines with sim state; (c) cold config (turnRate/accRate/skidRot*, goalRadius family) to the tail.
4. **Hoist the tail status bools** (isDead, beingBuilt, activated, stunned, plus transport ids if possible) into the front header line(s).
5. **Cache the CAI answer GMT needs** (e.g. a "hasMoreCommands/progressState" byte maintained by CommandAI) inside the GMT hot line, replacing the 1.3KB-away commandQue peek.

Sanity notes: reordering is sync-safe (checksums accumulate in execution order) and demo-safe; creg savegame layout shifts are accepted churn. Verify each step with the per-zone Self-LLC-fills columns (Sim zones) + sim wall time on this same benchmark, and a re-capture of this analysis.

## Late-game validation run (56-player real game)

Second capture on a real 52-min 56-player teams replay ("Why did I let PtaQ talk me into this", engine 2026.06.11 demo on 2026.06.12 build via `DisableDemoVersionCheck=1` - played back cleanly, 0 desync warnings), fast-forwarded headless to f=75600 (~42 min game time) and sampled 150s: 1.82M samples, **14.0% in unit pool** (vs 12.9% in fightertest).

Confirms the fightertest ranking - and re-ranks two items:

- `mtTempNum` still #1 (10.9%), QuadField scans unchanged. Recommendation 1 is scenario-robust.
- GMT pathing mailbox (line 35: `nextPathId` 5.7% + `jobId` 3.3%) and the avoidance core (line 32: `flatFrontDir` 5.3%, `lastAvoidanceDir` 2.9%, `useRawMovement` 1.8%) are the two hottest lines. Recommendation 3 robust.
- **Tail line 66 is much hotter in real games** (#5 line): besides LOS reading `isDead`/`beingBuilt`, `CUnit::UpdateWeaponVectors` reads `onTempHoldFire`/`forceUseWeapons`/`allowUseWeapons` there, and `iconRadius` joins via icon updates. Recommendation 4 gains priority; include the weapon-enable bools in the hoist.
- `CAI::commandQue` drops to 1.2% (vs 9% in fightertest - constant re-ordering exaggerated it). Recommendation 5 demoted to opportunistic.
- **New late-game costs**: script animation state - `COB::anims` 4.7% (`TickAllAnims`, lines 21-22), `COB::pieces` 2.6% (weapon aim-piece reads), `localModel` 6.9% (`LocalModelPiece::SetFloat3` writes through it), plus draw-adjacent `preFrameTra` 3.6% / `drawPos`/`drawMidPos`/`iconRadius`. Piece/anim data is the biggest engine-wide miss source in late game too (TickAllAnims + SetDirty + SetFloat3 + transform updates + std::function dispatch ~ 30% of ALL samples), reinforcing that piece-transform storage is the next target after CUnit.

## Relating IBS objects to the per-zone fills table (doc/l3-cache-fills.txt)

The zone table (RECOIL_HW_ZONE_COUNTERS) and the IBS field data compose: zones say *where per-thread fills accrue*, IBS says *which objects miss inside which functions*. Key join rule: **per-thread counting means `for_mt` body misses land in `ThreadPool::RunTask` self, not in the sim zone that brackets the fan-out** (the zone timer lives on the sim thread; workers charge their own `RunTask` timer). That is why e.g. `Sim::Unit::MoveType::1::UpdateTraversalPlan` shows ~0 self fills while IBS ranks `CGroundMoveType::UpdateTraversalPlan` among the top unit-pool missers.

Zone-by-zone attribution (self fills/s from the headful session table; IBS from the late-game capture):

- `ThreadPool::RunTask` self 4.76M/s - the largest row; decomposes per IBS into: piece/anim transform family (LocalModelPiece SetDirty/SetFloat3/UpdateModelSpaceTransform, TickAllAnims MT share, UpdateList::SetUpdate, CQuaternion::Rotate: ~25% of all samples), projectile drawflag scan (~9%), particle updates (~13%), LOS update lambda (los/allyteam/isDead/midPos: ~4%), MoveType MT phases 1+3 (GMT::nextPathId, physicalState), QuadField MT queries (mtTempNum writes), and std::function task dispatch (~10%, hits GMT::jobId + Unit::moveType ptr). A tid split shows **mtTempNum misses are ~100% worker-side** (27,660 of 27,717 samples; GetUnitsExact + GetSolidsExact) - the #1 unit-pool field is entirely invisible to the Sim::* zone rows and lives in RunTask self.
- `Sim::Unit::UpdatePreFrame` self 1.83M/s - the hottest ST sim zone; = CUnit::UpdatePreFrame writing `preFrameTra` (line 3, shared with pos/speed). One dirtied line per unit per frame.
- `Sim::Unit::Update` self 1.07M/s - CUnit::Update body: tail-line-66 status bools, speed/physicalState touches (aggregated small accesses; no single dominant IBS function).
- `Sim::Unit::MoveType` self 0.02M/s + `::5::Update` self 0.35M/s - the ST GMT portions: untimed UpdateUnitPosition/UpdatePreCollisions block accrues to the parent's self; phase 5 = GMT::Update -> FollowPath/ChangeSpeed/UpdateOwnerPos (waypoint/speed core lines 31-33, CAI::commandQue peek, speed).
- `Sim::Unit::UpdateWeaponVectors` total 0.06M (MT -> RunTask) - CUnit::UpdateWeaponVectors (weapons vector + onTempHoldFire/forceUseWeapons/allowUseWeapons tail bools) and CWeapon::UpdateWeaponVectors (COB::pieces aim-piece reads + frontdir/rightdir/updir).
- `CUnitScriptEngine::Tick` self 0.12M/s + `Sim::Unit::UpdatePostAnimation` 0.08M/s - COB::anims ticking + localModel piece writes (the ST share; the MT share is in RunTask).
- `Sim::Los` (MT -> RunTask) - los + allyteam + isDead + midPos + beingBuilt, 4 scattered lines per unit per LOS pass.
- `CUnitDrawerBase::Update` / `Update::EventHandler` - UpdateDrawPos (reads preFrameTra -> writes drawMidPos/drawPos) and UpdateUnitIconStateScreen (iconRadius/health/losStatus/noDraw).
- `TransformsUploader::Update` self 0.48M/s - reads LocalModel piece transforms for the SSBO upload (heap piece data, not unit pool).
- `Lua::Callins::Unsynced` self 2.39M/s - headful-only widget callin bodies (matches the earlier Lua boundary-profiler finding); outside IBS headless coverage, not unit-pool.

Cross-check: summing the unit-pool-heavy rows (RunTask share + UpdatePreFrame + Unit::Update + MoveType + SlowUpdate) against the ~14% unit-pool share of all IBS samples is consistent.

## Caveats

- Single scenario: 650v650 ground bots, headless. No air (StrafeAirMoveType), no builders/factories economy load; draw-side unit access is underrepresented. Re-run with other scenarios before finalizing a layout.
- `l3missonly` sampling has period skew (hardware resets period on non-miss tags), so counts are relative weights, not absolute miss counts.
- No latency weights captured (add `-W` to perf record for that); counts treat all misses equally, but dependent-chain misses cost more than overlapped ones.

## Reproduction

```
# 1. layout tables (once per layout change)
gdb -batch -ex "ptype /o CUnit" -ex "print sizeof(CUnit)" build/spring-headless > cunit_layout.txt
gdb -batch -ex "ptype /o CSolidObject" -ex "ptype /o CWorldObject" -ex "ptype /o CObject" build/spring-headless > base_layout.txt
gdb -batch -ex "ptype /o CGroundMoveType" -ex "ptype /o AMoveType" -ex "ptype /o CMobileCAI" \
    -ex "ptype /o CCommandAI" -ex "ptype /o CCobInstance" -ex "ptype /o CUnitScript" \
    build/spring-headless > embedded_layout.txt
python3 tools/cunit-llc-analysis/parse_layout.py cunit_layout.txt base_layout.txt   # -> fields.json

# 2. capture (runs the fightertest benchmark headless under IBS)
tools/cunit-llc-analysis/run_capture.sh    # -> perf.data, maps.txt

# 3. fold
perf script -i perf.data -F tid,ip,sym,addr > samples.txt
python3 tools/cunit-llc-analysis/fold2.py  # heat + lines + affinity report

# NB: update POOL_SYM_OFF in fold2.py from `nm build/spring-headless | grep unitMemPool`
# after relinking, and SLOT_STRIDE/sizes after layout changes.
```
