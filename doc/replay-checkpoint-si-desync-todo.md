# Replay-checkpoint restore desync — Supreme Isthmus residual (TODO)

Branch: `codex/replay-rewind-2025.06.24`. Handoff note for the *second* checkpoint-restore
desync, still open after the COB fix below. Self-contained so the session can be cleared.

## TL;DR

**RESOLVED 2026-06-21.** Root cause: the `CBuilderCaches` reclaim/resurrect tracking sets
(`reclaimers`/`featureReclaimers`/`resurrecters`) are static, not creg-serialized, and are
cleared by `CUnit::InitStatic()` on every load — so after a checkpoint restore they start
empty while the restored cons still hold active reclaim/resurrect commands. Fix =
`CBuilderCaches::RepopulateAfterLoad()`, called from `CGame::FinalizeLoadSavePostLoad`. Both
demos are now fully clean (SI + ATG, 10 checkpoints to f30000, 0 RewindAudit divergences,
0 DESYNC warnings). See "What is already fixed" item 3. The original report below is kept for
history.

---

Restoring a replay checkpoint and resuming the demo desyncs on **Supreme Isthmus**
(first divergence ~frame **24630**, after a checkpoint at **24600**). Straight playback
(no rewind) is perfectly sync-clean to 30000 on both demos. All That Glitters multi-rewind
is now clean too — only SI still fails, via a *different* root cause than the COB bug.

**Re-confirmed 2026-06-21 on the current branch** (fresh GCC RelWithDebInfo build). Multi-rewind
walk, 10 checkpoints at 600/3600/.../27600 to f30000, both demos:
- **ATG: fully clean** — 0 RewindAudit divergences, 0 DESYNC warnings.
- **SI: only the f24600 checkpoint diverges.** Checkpoints at 600, 3600, 6600, 9600, 12600,
  15600, 18600, 21600 all restore clean; 24600 diverges at f24630 (= checkpoint **+30**, exactly
  one `GAME_SPEED` read-ahead window), then cascades into DESYNC warnings f25263→f28149.
- First-diverging detail at f24630 decoded (cmd IDs): three `def=803` builder units (1655, 5871,
  25341) go `cmds=1 firstCmd=16` (FIGHT) in the reference → `cmds=3 firstCmd=90` (RECLAIM) after
  restore. **90=RECLAIM is a *front sub-order* a FIGHT-queued builder spawns when it perceives
  reclaimable wreckage** — i.e. those units saw a feature/target the reference timeline didn't.
  So the phantom "commands" are a *symptom* of an upstream perception divergence (feature state or
  gsRNG), not net-stream command double-application. unit 9923 genuinely re-paths; corca 23670 is
  the gsRNG/aircraft cascade. The +30 timing pins it to the moment the restored read-ahead queue
  is exhausted and live demo reading resumes.

## What is already fixed (context, do not re-investigate)

1. **clang build was the original red herring.** `build/` had been configured with
   `clang++-19`, ignoring the GCC toolchain file, so its FP codegen diverged from the
   GCC-recorded demos (first diff was a 1-ULP aim-piece rotation at f98). Rebuilding with
   the canonical GCC command fixed straight playback. **Always build the engine with GCC**
   (`-DCMAKE_TOOLCHAIN_FILE=toolchain/gcc_x86_64-pc-linux-gnu.cmake`) and check
   `readelf -p .comment build/spring-headless` before suspecting source. See
   `doc/clang-windows-pdb-symbols-issue.md` / memory `project_clang_build_desyncs`.

2. **COB `waitingThreadIDs` was not serialized.** `rts/Sim/Units/Scripts/CobEngine.cpp`
   had `CR_IGNORED(waitingThreadIDs)` ("always null/empty when saving" — false).
   `WakeSleepingThreads()` runs *after* the `std::swap(runningThreadIDs, waitingThreadIDs)`
   in `TickRunningThreads()`, so sleep-woken threads that reschedule as `Run` sit in
   `waitingThreadIDs` at the end-of-frame save point (`FlushPendingReplayCheckpointSave()`
   at the tail of `CGame::SimFrame`). Unserialized → orphaned in `threadInstances` on
   restore → scripts froze + threads leaked → desync. **Fix = `CR_MEMBER(waitingThreadIDs)`**
   (already applied). Verified: All That Glitters, 10 checkpoints / 16 rewinds to f30000,
   went 4933 → **0** DESYNC warnings, RewindAudit 35 → **0**.

3. **`CBuilderCaches` reclaim/resurrect caches were not rebuilt after load** — the SI bug
   this doc was opened for. `reclaimers`/`featureReclaimers`/`resurrecters` are *static*
   sets, deliberately not in creg ("should repopulate itself"), and `CUnit::InitStatic()`
   (called from `CGame::PostLoadSimulation` on every load) clears them. A checkpoint restore
   does not re-issue commands, so the sets start **empty** while the restored cons still hold
   active CMD_RECLAIM/CMD_RESURRECT commands. Until each builder's next SlowUpdate re-runs
   `ExecuteReclaim`/`ExecuteResurrect` and re-adds itself, `IsFeatureBeingReclaimed` /
   `IsFeatureBeingResurrected` / `IsUnitBeingReclaimed` wrongly return `false`, so
   `FindReclaimTarget`/`FindResurrectTarget` (BuilderCAI.cpp:1548/1477) stop skipping
   already-targeted features and multiple cons re-target the same wreck → phantom duplicate
   `CMD_RECLAIM`(90) front orders, interrupted resurrects, divergent feature `reclaimLeft`,
   and the gsRNG cascade. Diagnosed with `RewindAudit=1 RewindAuditPeriod=1`: the
   `[RewindSimPhaseAudit]` log put the first divergence at **f24606 phase=after-units**, with
   `commands`+`features`+`rng` digests differing together while `units`/`paths`/`projectiles`
   still matched — ruling out the net-stream and QTPFS-pathID hypotheses below. **Fix =
   `CBuilderCaches::RepopulateAfterLoad()`** (rebuilds the three sets from the restored command
   queues), called from `CGame::FinalizeLoadSavePostLoad` next to the other replay-checkpoint
   rebuilds. Verified: Supreme Isthmus 10 checkpoints to f30000 went 1180 → **0** DESYNC
   warnings, RewindAudit 20 → **0**; ATG stayed **0**/**0**.

## The remaining bug (this doc) — RESOLVED, see item 3 above; kept for history

Supreme Isthmus multi-rewind still produces DESYNC warnings (first demo-checksum mismatch
at f25260; RewindAudit re-sim divergence at f24630, 30 frames after the f24600 checkpoint).

### Evidence (straight vs restore-from-24600, dumped via DumpState)

At frame 24615 (15 frames after restore), comparing the GCC straight (= demo-correct) dump
against the post-restore dump:

- **`genState` (gsRNG synced RNG state) differs.** This is the broad cascade driver —
  once any unit draws a different number of `gsRNG` values, the global synced RNG diverges
  and everything downstream (aircraft `randomWind`, weapon spread, etc.) follows.
- **Command-queue over-population.** Several units of the same def (`def=803`, static,
  `heading=-16384`) have `commandQue.size(): 1` in straight but `3` after restore, with
  extra `commandID: 90` and `commandID: 16` (16 = CMD_FIGHT; 90 = likely a Lua/custom cmd).
  i.e. the restored timeline gains phantom commands.
- **QTPFS `pathID` mismatches.** Many units differ only in `pathID` while movement is
  identical — path `70` belongs to unit 1761 in the reference but to unit 9923 after
  restore; others get huge handles (`2097256`, `6291510`, `1048744`) that look like entt
  entity IDs with **bumped version bits**. A few units (e.g. **9923**) then genuinely
  re-path: reference `atGoal=1 path=0` (arrived) vs restore `path=70` moving toward a
  *different* `goal` → real behavioral divergence.
- **First divergent moving unit in dump order:** `corca` (unitID 23670), a Cortex
  aircraft (airborne, height ~344) — symptom of the gsRNG cascade (hover/strafe air move
  types draw `gsRNG` every frame).

COB thread count is off by only 1 at f24615 (724 vs 723), growing later — secondary, not
the SI root (the big COB bug is already fixed).

### RewindAudit at f24630 (per-unit ref-vs-current, the cleanest signal)

`/www/projects/bar-data/rewind_audit/<label>/audit.log` `detail` lines show, e.g.:
- `unit=1761 diff=pathID` (ref path=70, current path=2097256) — movement identical.
- `unit=1655/5871/25341 diff=cmdCount,firstCmd` (cmds 1→3, firstCmd 16→90) — phantom cmds.
- `unit=9923 diff=progress,pathID,pos,speed,goal,...` — genuinely different goal/path.
- `unit=23670 (corca) diff=pos,speed,goal,dirs,heading` — aircraft, gsRNG cascade.

## Leads / hypotheses to chase (in rough priority)

1. **Command-queue / order restoration** (most concrete). Why do 3 same-def units gain
   identical phantom commands (90,16) after restore? Candidates:
   - **Demo/replay net-stream re-applies commands** after restore. The branch restores the
     demo read position (`[ReplayRewind] restored replay net state: demoFilePos=... q0=...`,
     `[GameServer] preserved checkpoint replay server read-ahead`). If `demoFilePos` /
     server read-ahead is even slightly behind where it should be, command packets between
     the checkpoint and resume get replayed twice. Look at GameServer.cpp + DemoReader.cpp
     net-state restore (both heavily changed on this branch).
   - **Synced-Lua (gadget) state restore.** If synced Lua VM state isn't restored
     correctly, a gadget re-inits and re-issues commands (FIGHT etc.) to those units.
     Check whether the checkpoint serializes/restores the synced Lua state.
   - **CCommandAI creg** restoring a stale/duplicated `commandQue`.

2. **QTPFS path-handle restoration.** `QTPFS::PathManager::ApplyLoadSaveRestore()`
   (rts/Sim/Path/QTPFS/PathManager.cpp ~2233) uses
   `entt::basic_snapshot_loader<QTPFS::entity>{registry}.entities(archive)` to recreate
   path entities, but only restores a *subset* of components in the snapshot; `IPath` and
   the shared-path chains are reattached separately from `pendingLoadSaveState.paths`. The
   unit's `pathID` (entt handle) is creg-serialized on `CGroundMoveType` independently. If
   the snapshot loader does not reproduce the exact same entt handles (index **and**
   version) that the units' `pathID`s reference — e.g. when `registryEntitySnapshot` is
   empty and the fallback destroy/recreate path runs — units' `pathID`s point at the wrong
   (or revived-with-new-version) entities. Note `CGroundMoveType::PostLoad` deliberately
   `return`s early when `gameSetup->replayCheckpoint` so it does NOT re-request, so a
   mismatched `pathID` will silently point at the wrong path.
   - Open question: is `pathID` itself sync-hashed, or only behaviorally relevant when a
     unit re-paths? If purely behavioral, fixing the command/goal divergence may be enough
     and the pathID diffs are benign noise. Confirm by checking whether unit `pathID` /
     `currWayPoint` feed CSyncChecker.

3. **gsRNG ordering.** Confirm `gsRNG`/`CGlobalSynced` is itself correctly restored at the
   checkpoint frame (it appears to be — divergence develops over ~15-30 frames rather than
   being immediate, implying restored state is *almost* right and drifts once one unit
   draws a different count). So treat gsRNG divergence as a *symptom*, fix the upstream
   command/path divergence.

## Reproduce

GCC build required. Binary: `build/spring-headless` (verify GCC via `readelf -p .comment`).

Multi-rewind walk (10 checkpoints across the replay, reports DESYNC + RewindAudit):
```
# /tmp/multirewind.sh in the prior session; equivalently set these env on
# test/replay-rewind/run_rewind_test.sh:
REWIND_AUDIT=1 MULTI=1 MULTI_BASE=600 MULTI_STEP=3000 MULTI_MAX=30000 MULTI_WINDOW=600 \
AUDIT_LABEL=si SPRING_BIN=build/spring-headless \
  test/replay-rewind/run_rewind_test.sh \
  "2026-06-13_19-24-05-786_Supreme Isthmus v2.1_2025.06.24.sdfz"
```

Single restore + DumpState diff to pinpoint (straight = demo-correct reference):
```
# straight reference at frame N:
DUMPSTATE=1 DUMPFRAME=24615 DUMPWINDOW=0 TRIGGER_FRAME=-1 QUIT_FRAME=24635 REWIND_AUDIT=0 \
  test/replay-rewind/run_rewind_test.sh "<SI demo>"
# restore from checkpoint CPF then dump frame N:
DUMPSTATE=1 DUMPFRAME=24615 DUMPWINDOW=0 CHECKPOINT_FRAME=24600 \
  TRIGGER_FRAME=24630 TARGET_SECONDS=820 QUIT_FRAME=24635 REWIND_AUDIT=0 \
  test/replay-rewind/run_rewind_test.sh "<SI demo>"
# DumpState writes /www/projects/bar-data/ServerGameState-*.txt ; diff with `sed '9d'`
# to drop the syncVer line. NOTE: DumpState only writes its *min* frame, so dump one
# frame per run and bisect.
```

Run with `--isolation --write-dir /www/projects/bar-data` (the runner already does this).
Config knobs of interest: `ReplayCheckpointSyncCheck=1` (re-enabled demo-stream desync
comparison), `RewindAudit=1`, `AllowDumpStateNoCheat=1`. Headless fast-forward via the
driver widget (`/setspeed 20 + /speedcontrol 0`).

## Files most likely involved

- `rts/Net/GameServer.cpp` / `.h` — replay net-state restore, server read-ahead.
- `rts/System/LoadSave/DemoReader.cpp` / `.h` — demo seek / read position on rewind.
- `rts/Sim/Path/QTPFS/PathManager.cpp` (`CaptureLoadSaveState`, `ApplyLoadSaveRestore`,
  `QueueLoadSaveRestore`) + `NodeLayer.cpp` / `Node.cpp` — QTPFS path entt snapshot.
- `rts/Sim/MoveTypes/GroundMoveType.cpp` (`PostLoad`, `RebuildPathAfterLoadSaveRestore`,
  `NeedsLoadSavePathReinit`) — pathID reconciliation, early-return on replayCheckpoint.
- `rts/Game/Game.cpp` (`FinalizeLoadSavePostLoad`, `MaybeCreateAutoReplayCheckpoint`,
  `SaveReplayCheckpoint`) — checkpoint save/load orchestration + `AuditRewindSimPhase`.
- Synced-Lua state restore (if gadgets re-issue commands): wherever the checkpoint
  serializes the synced Lua handle.

## Debug instrumentation still in the tree (clean up when done)

Real fixes to KEEP: `ReplayCheckpointSyncCheck` config (GameServer), async checkpoint-write
race fix (`CregLoadSaveHandler` `WaitForPendingCheckpointWrite`), `RebuildUpdateQueueAfterLoad`
/ `RebuildAnimatingAfterLoad`, and the COB `waitingThreadIDs` `CR_MEMBER` fix.

Debug-only to REMOVE later: DumpState/DumpHistory cheat bypass (`AllowDumpStateNoCheat`),
`FeatQueueDbg`/`FeatUpdDbg`/`AnimQueueDbg` logs, LoadGame position logs in
`CregLoadSaveHandler`, the RewindAuditHistoryFrame op-history dump, and the
`test/replay-rewind/` driver's DumpState arming knobs (`RewindTestDumpFrame/Window`).
`AuditRewindSimPhase` / `AuditRewindMovePhase` are useful diagnostics gated behind
`RewindAudit` config — keep until this bug is fixed, then decide.
