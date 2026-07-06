# Replay-checkpoint rewind — feature-queue fix + QTPFS accumulation residual (TODO)

Branch: `codex/replay-rewind-2025.06.24`. Handoff note from the 80k-validation pass. Self-contained so the session can be cleared.

## TL;DR

Two distinct checkpoint-rewind bugs were found while extending validation from frame 40000 to 80000 (in previously-untested territory beyond 40k):

1. **Feature update-queue restore — FIXED.** A checkpoint load re-queues *every* active feature into `CFeatureHandler::updateFeatures`. The old `RebuildUpdateQueueAfterLoad` filtered the queue back down with a `wantsUpdate()` predicate, which wrongly dropped genuinely-updating features (e.g. a wreck perpetually settling under gravity: it moves a sub-unit amount each frame so `CFeature::Update` keeps it queued, but its instantaneous state is `speed==0 / !IsMoving / no fire-smoke-geo`, so every predicate clause is false). A dropped feature loses its per-frame synced physics ops, so the running sync checksum diverges while every state digest stays identical ("checksum-only divergence"). Fix = serialize the genuine queue (`savedUpdateQueue`, captured in `SnapshotUpdateQueueForSave()` at save) and restore it exactly. Verified: single-rewind repro at the original failing frame is clean (op-streams identical, queue restored to the genuine set).

2. **QTPFS rewind-accumulation residual — OPEN, scoped out.** Needs the full ~18-rewind accumulation to repro; manifests as desync/drift/crash on the longer demos. See "Bug #2".

3. **Smooth-height-mesh restore re-blur — OPEN, root-caused, fix is deep.** The aircraft-attitude divergence (single-rewind reproducible) traces to the checkpoint restore inducing a spurious, non-idempotent smooth-mesh re-blur. See "Bug #3".

## Bug #2: QTPFS rewind-accumulation residual (OPEN)

### Symptom

Demo: `replays/2026-06-13_21-02-54-238_All That Glitters v2.2.3_2025.06.24.sdfz` (60210 frames).

Full multi-rewind run (`MULTI=1 MULTI_BASE=600 MULTI_STEP=3000 MULTI_WINDOW=600`, checkpoint every 3000 frames) diverges at the last checkpoint window. First divergence from the **demo stream** (ground truth) at frame ~57060; within-window `[RewindSimPhaseAudit]` divergence at frame 57608, phase `after-units`. Diverging digests at 57608: `sync`, `unitDigest`, `unitKinematicsDigest`, `unitMoveDigest`, `changeHeadingOrderDigest`, `changeHeadingValueDigest`. Everything else matches (rng, teams, commands, features, projectiles, paths, `unitIDPoolDigest`, `unitIdentityDigest`, all *move*-order ECS digests, `changeHeadingEntityOrderDigest`). So: the same units in the same order compute **different `ChangeHeadingEvent.deltaHeading`** values — a unit-movement/pathing divergence, not a feature or id-pool issue.

### Root nature (why it's hard)

It is **cumulative and accumulation-dependent**, not a single-restore bug:

- A single rewind at frame 57600 reproduces **nothing** — op-history forward-vs-resim is byte-identical (23401 == 23401 ops).
- One prior rewind (2-checkpoint MULTI, cp@54600 then cp@57600) is also clean — 0 phase divergences, 0 demo-stream desyncs.
- The full 18-prior-rewind MULTI diverges.

Comparing the full-MULTI forward state at frame 57600 against a clean-forward run (both with the #1 fix) shows the MULTI sim has already **silently drifted**: different unit/feature/projectile counts (e.g. 3055 vs 3060 active units), and crucially `GetNumQueuedUpdates()` (the QTPFS dirty-path update schedule `updateDirtyPathRate` / `updateDirtyPathRemainder`) differs (e.g. remainder 1 vs 5). The drift is **invisible** — the run matches the demo stream perfectly until ~57060, then suddenly diverges, consistent with an unserialized counter/queue crossing a threshold or a deferred dirty-path refresh firing on a different frame.

The within-window audit (forward-vs-resim of the same checkpoint window) cannot detect this drift because both sides share the residual; only comparing against the demo stream or a clean-forward baseline reveals it.

### Where to look

`QTPFS::PathManager` is **not** creg-serialized (`grep CR_MEMBER rts/Sim/Path/QTPFS/PathManager.cpp` is empty). Its state is restored through the bespoke `QueueLoadSaveRestore` / `ApplyLoadSaveRestore` snapshot path (driven from `CGame::FinalizeLoadSavePostLoad`, with `TerrainChange(0,0,mapx,mapy)` + `PostFinalizeRefresh` when no full snapshot is queued, plus `RebuildPathAfterLoadSaveRestore` per ground-mover). The dirty-path update scheduling (`updateDirtyPathRate` / `updateDirtyPathRemainder`, `PathManager.h:309-310`) and the dirty/damage queue (`mapChangeTrack.damageQueue`) are reconstructed by that custom logic and are the prime suspects for non-idempotent restore. The bug is that this restore is not bit-idempotent across *repeated* rewinds — each restore leaves a tiny residual that compounds.

### Repro / diagnosis harness

- `test/replay-rewind/run_rewind_test.sh` with the MULTI env vars above; `AUDIT_LABEL` names the audit dir under `$SPRING_DATADIR/rewind_audit/<label>/audit.log`.
- For op-level localization, build with `-DSYNC_HISTORY=ON` and set `HISTFRAME=<frame>` + `AUDIT_FRAMES=<frame+1>` to dump that frame's per-op sync history (forward pass and post-rewind resim both append a `history-begin/​end frame=<frame>` block; diff them for the first diverging op index). NB: this only reproduces in the full MULTI context, so each cycle is a ~20-minute run, and op indices still need code markers to map to a callsite.
- Ground-truth signal = `[DESYNC WARNING] ... from demo player N` against the recorded stream (earliest real divergence). Trust it over the within-window `[RewindSimPhaseAudit]`, which lags because it is blind to shared drift.

### 80k sweep findings (2026-06-23): one root cause, three manifestations

A full 9-demo 80k sweep with the feature-queue fix in place showed the QTPFS rewind-accumulation residual surfaces in more than one way, on different demos, at different points:

- **Drift → desync.** `2026-06-13_21-02-54-238 ATG`: silent drift, demo-stream desync at f≈57060 (idx 19, ~18 rewinds), heading-value divergence (the canonical case above).
- **Earlier drift.** `2026-06-22_04-11-57-148 ATG`: diverges at f≈24606 (idx 8, ~8 rewinds), signature `units` digest differs while `sync` matches — i.e. a non-sync-hashed unit-state (vertical / `pos.y`) difference, demo-stream desync at f≈25260. (Possibly the same root tripping earlier on this demo; not separately confirmed.)
- **Drift → crash.** `2026-06-13_19-24-05-786 Supreme Isthmus`: SIGSEGV during the checkpoint *save* (terrain/QTPFS capture) at f63600 (idx 21, ~20 rewinds). Unsymbolized libc-only trace (heap-corruption style). **Confirmed accumulation-dependent**: a single `/cp` at f63600 from clean-forward state (no prior rewinds) creates+restores the checkpoint cleanly with no crash. So the cumulative restore non-idempotency eventually corrupts state enough to fault the save path.

The feature-queue fix (#1) is independently sound — clean on single-rewind and single-`/cp` tests. These three are all downstream of the un-fixed QTPFS accumulation residual and are scoped out together.

## Bug #3: aircraft-attitude divergence — single-rewind reproducible (OPEN, distinct from #2)

Found in the 80k sweep on the 06-22 demos (`...04-44-42 SI`, `...04-11-57 ATG`). **Unlike #2, this reproduces with a SINGLE rewind** (no accumulation) — so it is more tractable and is a genuinely distinct bug.

### Symptom

Single `/cp` at frame 24600 (`MULTI=0 CHECKPOINT_FRAME=24600 TRIGGER_FRAME=25200 TARGET_SECONDS=820`), rewind, resim. First `[RewindSimPhaseAudit]` divergence at frame 24652, phase `after-units`. Diverging digests: `sync`, `unitDigest`, `unitKinematicsDigest`, `unitMoveDigest` (NOT the changeHeading digests of #2). Per-unit detail: the first diverging units are all the same aircraft type (`def=506`, airborne, maneuvering), diverging in `pos, speed, frontDir, rightDir, upDir` — i.e. aircraft attitude/altitude. Demo-stream desync follows at ~f25260.

### Op-level localization

Build `-DSYNC_HISTORY=ON`, `HISTFRAME=24652 AUDIT_FRAMES=24653`, diff the forward vs resim op blocks: both have the **same op count (11636)**; first diverging op is **#6108** (~52% into the frame, in unit processing). Same count + value difference = a synced computation produces a different *result* from *identical synced inputs* — so op 6108 consumes a **non-synced value** that differs post-restore and feeds a synced computation. Frames 24601-24651 and ops 0-6107 of 24652 are bit-identical.

### Ruled out (all fully creg-serialized, verified by inspection)

`AMoveType` (goalPos/oldPos/progressState/maxSpeed…), `AAirMoveType` (aircraftState/oldGoalPos/wantedHeight/collide…), `CStrafeAirMoveType` (maneuver*/last{Rudder,Elevator,Aileron}Pos…), `CHoverAirMoveType` (currentBank/currentPitch/wantedSpeed/randomWind/wantedHeading…), `EnvResourceHandler` global wind (curWindVec/curWindDir/newWindVec/oldWindVec/windDirTimer), `SmoothHeightMesh` (custom Serialize covers mesh + mapChangeTrack). `CSolidObject` pos/speed/heading/frontdir/midPos are serialized too.

### Root cause (fully localized, 2026-06-23)

Op 6108 was tracked to the unit being a `CStrafeAirMoveType` (def=506 fighter). In `UpdateFlying` the only diverging input to `GetControlSurfaceAngles` (which sets the elevator) is `groundHeight = amtGetGroundHeightFuncs[5*UseSmoothMesh()](pos.x,pos.z)` — the **smooth-height-mesh** height (`UseSmoothMesh()==1`). Probing `smoothGround.GetHeight(aircraftPos)` per frame, forward vs resim:

- Forward: smooth-mesh queues are empty (`UpdateSmoothMeshRequired==false`); height is static at the converged value (e.g. 48.3694801).
- Resim (post-restore): a spurious batch of damaged quads is released at frame ≈ restore+`SMOOTH_MESH_UPDATE_DELAY` (=`GAME_SPEED`=30), processed through `BlurHorizontal`/`BlurVertical`, and shifts the height (e.g. to 48.2423401). The aircraft samples the shifted height → different elevator → divergent attitude.

The smooth mesh IS fully serialized (`SmoothHeightMesh::Serialize` covers `mesh`/`tempMesh`/`origMesh`/`maximaMesh` + the blur queues + `mapChangeTrack`), and `PostLoad` re-applies it. The problem is the **checkpoint restore re-derives the heightmap, and `CBasicMapDamage` then calls `smoothGround.MapChanged()` for terrain that did not actually change** (it was restored to the saved state). That enqueues damage; `MapChanged` sets `queueReleaseOnFrame = frame + 30`, so it releases ~30 frames after the restore (NOT at load time). Re-blurring those regions does NOT reproduce the original `MakeSmoothMesh` value (the incremental blur path and the initial full-mesh build are not value-identical for the same terrain), so the height drifts. Forward never re-blurs these undamaged regions, so it keeps the original value.

### Why the obvious fixes don't work (three attempts, all failed byte-identically)

Tried, each produced the *identical* divergence (phaseAuditDiv=7 @ f24652):
1. Re-apply the saved smooth-mesh snapshot in `CGame::FinalizeLoadSavePostLoad` after the terrain restore.
2. Re-apply it once at the first post-load `UpdateSmoothMesh` (the queue consumer).
3. Make `SmoothHeightMesh::MapChanged` a no-op while the post-load snapshot is pending (suppress the restore's re-derivation notifications), clearing the flag at the first `UpdateSmoothMesh`.

Why they fail: the whole-map `MapChanged` is issued from `CReadMap::PostLoad` (line ~538, `mapDamage->RecalcArea(0,mapx,0,mapy)`) which enqueues damage; `queueReleaseOnFrame = frame + SMOOTH_MESH_UPDATE_DELAY(=30)`, so the blur work releases ~30 frames AFTER the restore and processes one quad/frame — reaching the aircraft's quad ~f24652. A first-frame reset clears the queue too early (the deferred batch repopulates after), and the `hasPendingPostLoadRestore` suppression apparently doesn't cover the offending call — likely a **creg PostLoad ordering** issue: `CReadMap::PostLoad`'s whole-map `RecalcArea→smoothGround.MapChanged` may run relative to `SmoothHeightMesh::Serialize` setting the pending flag such that the guard isn't active, AND/OR the double-buffer (`damageQueue[2]` + `queueReleaseOnFrame` deferred swap) means clearing the active buffer once doesn't stop the already-staged batch. The reset/suppression demonstrably DID clear the queue at the first frame (verified vBlur=0, H correct through ~f24633) yet `dmg0=8` reappeared at f24634 — a second staged batch. Pinning that requires understanding the creg PostLoad order of `readMap` vs `smoothGround` and the buffer-swap release timing.

A cleaner fix likely needs one of: (i) make `CReadMap::PostLoad`'s whole-map `RecalcArea` skip the smooth-mesh (and feature/LOS) re-notification on a checkpoint restore (those subsystems are restored from their own serialized state); or (ii) make the incremental blur value-identical to `MakeSmoothMesh` so any re-blur of unchanged terrain is a no-op.

### Where the real fix lives

Stop the restore from telling the smooth mesh that unchanged terrain changed. The spurious `smoothGround.MapChanged()` comes from the restore's heightmap re-derivation / `RecalcArea` path (`ReadMap` "reapplied exact derived terrain" + `CBasicMapDamage`), firing ~`GAME_SPEED` frames after the restore (deferred release). Note `CBasicMapDamage` is **not** creg-`CR_MEMBER`-serialized — it has a custom `Serialize` (`mapDamage->Serialize` in `CregLoadSaveHandler`); audit whether its `explosionUpdateQueue`/`explUpdateQueueIdx` and the derived-terrain re-notification are restored without re-notifying the smooth mesh. Fix options: (a) suppress `smoothGround.MapChanged()` for terrain that the checkpoint merely restored (not deformed); or (b) make the incremental `UpdateSmoothMesh` blur value-identical to the initial `MakeSmoothMesh` so re-blurring unchanged terrain is a no-op (the two blur paths currently differ, which is what makes the spurious re-blur shift heights). This is a terrain/map-damage/`SmoothHeightMesh` restore-consistency issue, deeper than a missing-serialize; comparable in depth to #2. Repro/diagnosis: single `/cp` at f24600 on `2026-06-22_04-44-42 SI`, `SYNC_HISTORY` op-history diff at f24652, plus per-frame `smoothGround.GetHeight(7087.34,1623.25)` + queue-size logging in `UpdateSmoothMesh`.

### UPDATE — suppressing the notification is NOT enough; it's blur restore-determinism (2026-06-23)

Implemented an `ignoreMapChanges` flag set by `CReadMap::PostLoad` around its whole-map `RecalcArea`, and verified by probe that the whole-map `MapChanged` at f24602 is suppressed (`ignore=1`). The divergence was **byte-identical** (phaseAuditDiv=7 @ f24652) — so the whole-map re-notification is NOT the corrupting input. With the suppression in place, probing `smoothGround.GetHeight(7087,1623)` forward-vs-resim: forward stays 48.3694801 with `verticalBlurQueue` empty (no smooth work in this window), but **resim still builds a `verticalBlurQueue` (vB grows 5→9)** and shifts the height to 48.2423401 at f24653. That blur work is not from the suppressed whole-map call, and not from the small localized `MapChanged` at f24603 (region (921,285)-(933,297), nowhere near the aircraft). It comes from the saved pending blur work + the double-buffer (`damageQueue[2]` / `queueReleaseOnFrame`) **swap/release timing differing across the restore**, so the same queue is processed in a different order / from different intermediate buffers, and the (not order-independent) blur converges to a different height. The forward run's queue at the `/cp` instant drained into the converged 48.369 before f24648; the resim's restored queue re-drains to 48.242.

**Conclusion:** the real bug is **smooth-mesh incremental-blur restore non-determinism**, not the terrain re-notification. SIX approaches were tried and all failed: reapply-saved-state in `FinalizeLoadSavePostLoad`; reapply at first `UpdateSmoothMesh`; no-op `MapChanged` while pending; gate `MapChanged` at the `CReadMap::PostLoad` source; the prior feature-style snapshot; **and a deterministic full rebuild (`MakeSmoothMesh`) on restore** — that last one *changed* the first-divergent frame from f24652 to **f24603** (immediate), proving the **full-rebuild `MakeSmoothMesh` and the incremental `UpdateSmoothMesh` blur are NOT value-identical for the same terrain**. So both clean directions are blocked:
- *Deterministic rebuild* (option ii/c) diverges immediately — the full and incremental blur paths disagree.
- *Incremental replay* (option i/a) diverges later — the queue processing order/timing isn't reproduced across restore.

The fix therefore requires **reconciling the two blur code paths so `MakeSmoothMesh` == the converged incremental result** (then rebuild-on-restore works), OR making the incremental `UpdateSmoothMesh` queue processing byte-deterministic across a restore (exact double-buffer swap state + order + order-independent blur). Either is a deliberate `SmoothHeightMesh` redesign, not a patch. Not landed.

### UPDATE 2 — precise root localized to the horizontal-blur input (`maximaMesh` neighbor), 2026-06-23

Seventh attempt: force-drain the restored queue to convergence on load (`queueReleaseOnFrame=0` + loop `UpdateSmoothMesh` until all queues empty). Still diverged at f24652 (same as no fix). Then a decisive probe interpolating `maximaMesh` / `tempMesh` / `mesh` at the aircraft point (7087,1623), forward vs resim, over f24648–24654:

```
forward:  maxima=47.6222267  temp=48.3694801  mesh=48.3694801   (converged, vB=0)
resim:    maxima=47.6222267  temp=47.3684578  mesh=48.3694801   (vB=5..9; at f24653 temp & mesh -> 48.2423401)
```

So: **`maxima` MATCHES** (the maxima value AND the terrain it reads are correct — consistent with units/paths not diverging), but **`tempMesh` DIFFERS** (forward 48.369 vs resim 47.368). `tempMesh` is the **horizontal-blur output** (`BlurHorizontal: maximaMesh -> tempMesh`) and is serialized; forward's evolved 47.368→48.369 as it processed the region's pending horizontal blur between the save (f24600) and f24648, while resim restored the mid-blur f24600 value and re-blurs to a **different** result (48.242, not 48.369). Since the aircraft's own `maximaMesh` cell matches but the windowed horizontal blur output differs, a **neighboring `maximaMesh` cell inside the blur window diverges after restore** — the maxima are serialized but recomputed per damage region by `UpdateSmoothMeshMaximas`, and the post-restore processing recomputes a neighbor region's maxima / processes regions in a different order/state than the live run, so the windowed blur reads different neighbor maxima.

**Actionable root:** the incremental maxima-recompute + blur pipeline (`UpdateSmoothMeshMaximas` → `BlurHorizontal` → `BlurVertical`, one queue item/frame, double-buffered) is not reproduced byte-exactly across a restore at the *neighbor-cell* granularity that the windowed blur depends on. Fix must make that pipeline deterministic across restore (exact per-region recompute + processing order + window inputs), or replace the per-region incremental maxima/blur with a deterministic whole-region recompute that the live run also uses. Force-drain alone does NOT fix it (tried), so it's not merely a timing/lag issue — the per-region maxima recompute order/state itself must be reproduced. Deep `SmoothHeightMesh` work.

### Build note

Must be a GCC build (`toolchain/gcc_x86_64-pc-linux-gnu.cmake`); GCC-only `-frounding-math` (`CMakeLists.txt:~499`) is required or demos desync at ~frame 300 regardless of checkpoints. Verify with `readelf -p .comment build/spring-headless` (GCC, no clang).
