# PR 46 — epoch-consistent draw interpolation (the 30 Hz model/shadow jitter)

Status: landed on `epoch-integration`. Operator report (post-PR-45, live flag-ON game): unit models and their shadows step at sim rate (~30 Hz) while unit icons are smooth; for air units the shadow sometimes looks smooth while the model jitters. Invisible to every log-based gate — root-caused with a temporary draw-frame probe, fixed with a one-uniform correction in `UniformConstants`.

## 1. How model interpolation actually works (branch AND master)

The orchestrator's initial hypothesis was that PR 44a moved per-draw-frame interpolated matrix production to the sim thread, freezing every SSBO consumer at frame edges. The code says otherwise, on both sides:

- The transforms SSBO (`transformsMemStorage`, binding 0) stores, per object, a **lerp pair**: slot 0 = the previous frame-edge transform (`preFrameTra`, saved by `UpdatePreFrame` at the top of every `SimFrame`), slot 1 = the current synced transform (`GetTransformMatrix()` at extraction). Piece slots hold the same prev/curr pairs (`GetEffectivePrevModelSpaceTransform` / `GetModelSpaceTransform`).
- Every GL4 consumer interpolates **in the vertex shader**: `Lerp(transforms[base+0], transforms[base+1], timeInfo.w)`, with `Lerp` clamping the factor to [0,1]. `timeInfo.w = globalRendering->timeOffset`, uploaded once per draw frame by `UniformConstants::UpdateParamsImpl`.
- Master (merge-base `74e145c707`) extracts the SAME pair at the SAME cadence — `UpdateObjectTrasform` is due-gated `lastUploadFrame >= gs->frameNum`, i.e. once per **sim** frame — and the shaders are byte-identical to the branch's. Master was never "SSBO produced at draw rate"; it was always frame-edge pairs + shader lerp.

So frame-edge pair production on the sim thread (44a §7.6A) is NOT inherently wrong. What 44a broke is the **atomicity between the pair and the lerp clock**.

## 2. Root cause (confirmed by probe, not hypothesis)

Master, single-threaded, per draw iteration: run any due `SimFrame` (which sets `gs->frameNum += 1; lastFrameTime = now` at its top) → `UpdateUnsynced` computes `timeOffset = (now - lastFrameTime) * wsf`, extracts the fresh pair in `worldDrawer.Update`, uploads it, uploads `timeInfo.w` → renders. The pair and the clock always describe the same frame edge — by sequential construction.

Under the flip (44a/44b):

1. The SIM thread executes frame N at wall time T_N, rebasing the clock (`lastFrameTime = T_N`) at the frame's TOP.
2. The pair for frame N is extracted only after frame N fully executes, inside the next `ProduceEpochAtSimEdge` (frame execution + ~2-3 ms extraction later), and reaches the GPU only at the draw thread's next upload seam (`transformsUploader.Update()` in `UpdateUnsynced`, once per draw iteration).
3. Any draw frame rendered in that window uses the NEW clock against the OLD pair: `Lerp(pair(N-2→N-1), timeOffset≈0.1)` ≈ pose(N-2) — after the previous draw frame showed ≈pose(N-1).

The rendered pose regresses by ~0.8 of a frame's motion, then leaps ~1.2 frames forward when the fresh pair arrives — once per sim frame: a 30 Hz sawtooth on every SSBO consumer. CPU-side consumers (`drawPositions` via `UpdateDrawPos`: `mix(preFrameTra.t, pos, timeOffset)` on live state) read pos and clock from the same live source AND use an unclamped `mix` — they stay smooth, which is exactly why icons/healthbars didn't jitter.

### Probe evidence (pre-fix, flag-ON, forced 1x, Rosetta, unit 22430)

Temporary `DG_TRA_PROBE=1` probe in `UpdateUnsynced` (removed after verification), one line per draw frame: raw `timeOffset` (`to`), the SSBO pair (`prev`/`curr` positions), the held-epoch frame (`heldSf`), live `sf`, and the CPU `drawPos`.

```
df=7874 sf=744 heldSf=744 to=0.8562  pair 743→744  rendered≈743.86
df=7875 sf=745 heldSf=744 to=0.0744  pair 743→744  rendered≈743.07   <- BACKWARD 0.79 frames
df=7876 sf=745 heldSf=745 to=0.2886  pair 744→745  rendered≈744.29   <- forward leap 1.22 frames
...
df=7907 sf=751 heldSf=751 to=0.7629  pair 750→751  rendered≈750.76
df=7908 sf=752 heldSf=751 to=0.0055  pair 750→751  rendered≈750.01   <- BACKWARD 0.75 frames
df=7909 sf=752 heldSf=752 to=0.2062  pair 751→752  rendered≈751.21   <- forward leap 1.2 frames
```

4 of 5 sim edges in the sampled window produced exactly one such mismatched draw frame (the exceptions are edges where `SmoothTimeOffset=2` carried the offset over ≥1.0, which the shader clamp turns into a benign hold). The `drawPos` column is strictly monotonic throughout — the icon path never regresses. Under FF (the gate replays carry a recorded `setspeed 20`) the same mismatch is grosser: the pair lags 5-8 frames behind `sf` and `to` saturates >1 for long runs — models advance only per epoch upload.

## 3. Consumer/source/rate table

| Consumer (pass / surface) | Transform source | Rate / clock | Pre-fix behavior |
|---|---|---|---|
| Engine GL4 opaque+alpha model pass (`ModelVertProgGL4.glsl`) | transforms SSBO pair | shader `Lerp(pair, timeInfo.w)`, clamped | 30 Hz sawtooth (pair/clock mismatch) |
| Engine GL4 shadow pass (`ShadowGenVertProgGL4.glsl`) | transforms SSBO pair | same `timeInfo.w` | identical sawtooth (same source — passes cannot disagree with each other) |
| BAR CUS GL4 model/shadow materials (`modelmaterials_gl4/templates/cus_gl4.vert.glsl`) | transforms SSBO pair | same `timeInfo.w`; also uses `simFrame = timeInfo.x + timeInfo.w` as continuous time | identical sawtooth |
| Piece/bone transforms (walk anims, turrets) — all passes above | SSBO piece pairs (same extraction) | same `timeInfo.w` | same sawtooth on top of sim-rate script anim |
| Legacy GLSL/FFP model+shadow path (`DrawUnitTrans` → `GetUnsyncedTransformMatrix`) | CPU `drawPositions[id]` | per draw frame, live `pos`+`timeOffset` (documented §C torn class) | smooth |
| Unit icons (world + screen), minimap icons | CPU `drawPositions[id]` (`UpdateDrawPos`) | per draw frame, unclamped `mix` | smooth (extrapolates when `to>1`) |
| Healthbars / selection / Lua `GetUnitViewPosition`-family | served drawPos twins (same `drawPositions`) | per draw frame | smooth |
| `ModelUniformsStorage.uni.drawPos` (GL4 widget instance data) | `GetDrawPos` copy, draw-written storage | per draw frame upload | smooth |

The operator's "air shadow smooth vs model jittery" is salience, not a source split: model and shadow read the same SSBO+factor and cannot disagree; a high flyer's diffuse ground blob just hides the sawtooth that its model shows. Post-fix both read the identical corrected factor by construction.

## 4. The fix

Rebase `timeInfo` onto the pair actually on the GPU, flag-ON-running only (`SimDrawSplit::Enabled() && SimThreadRunning()`):

- `CGame::UpdateUnsynced`, at the flip upload seam (right after `transformsUploader.Update()`, before `SignalEpochConsumeComplete()` — pacing guarantees the storage content is exactly the held epoch's extraction there): stamp `SimDrawSplit::SetUploadedTransformFrame(simSnapshot.SlotMeta(HeldSlot()).lastSimFrame)`.
- `UniformConstants::UpdateParamsImpl`: with `pairFrame >= 0`,
  - `timeInfo.w = timeOffset + max(0, gs->frameNum - pairFrame)` (uncapped; the shader `Lerp` clamps),
  - `timeInfo.x = pairFrame`.

Properties:

- **Pair-fresh case (`delta = 0`, the steady state)**: byte-identical to master's values.
- **Mismatch window (`delta = 1`, one draw frame per sim edge)**: factor = `to + 1` ≥ 1 → shader clamp holds the pair's edge pose (continuous with the previous draw frame) until the fresh pair arrives ≤1 draw frame later. The backward jump is gone; the residual is a sub-draw-frame hold (~7 ms at 144 fps), the information-theoretic best a consumer of late data can do.
- **`timeInfo.x + timeInfo.w` is invariant** (`pairFrame + to + (sf - pairFrame) = sf + to`) — BAR's continuous sim-time base (tread scroll, wind phases) is unchanged. `timeInfo.z` keeps master's formula (same continuous value).
- **`timeInfo.x` alone** becomes the held-epoch frame instead of the live frame (≤1 behind at 1×) — MORE consistent with the rest of the served draw state (`Spring.GetGameFrame()` draw-side already serves the epoch frame); enumerated as a deviation below.
- **FF catch-up**: `delta` large → factor clamps → the newest available edge pose, monotonic per epoch (pre-fix it oscillated inside a stale span).
- **Flag-OFF / flip-not-running**: the branch is dead, values byte-identical (armed diff-gate + full flag-OFF replay below).
- Model, shadow, and CUS all consume the SAME corrected uniform — the passes cannot disagree.

Files: `rts/System/SimDrawSplit.{h,cpp}` (stamp holder, reset in `Clear()`), `rts/Game/Game.cpp` (stamp at the upload seam), `rts/Rendering/UniformConstants.cpp` (corrected `timeInfo`). Test infra: `run_diff_gate.sh`/`callout_diff_driver.lua` gained `DG_FORCE_1X=1`/`DiffGateForce1x` (default 0) to pin gate replays to 1× — the gate demos carry a recorded `setspeed 20`, which had made every "1x" observation silently fast-forwarded.

### Probe evidence (post-fix, same replay/window/unit)

`w` = the corrected factor now uploaded, `posed` = the pose the GL4 passes render (`Lerp(pair, clamp(w))`):

```
df=7875 sf=747 heldSf=747 to=0.8275 w=0.8275  posed z=1495.34
df=7876 sf=748 heldSf=747 to=0.0323 w=1.0323  posed z=1495.13  <- mismatch window: clamp -> HOLD at pair edge, still forward
df=7877 sf=748 heldSf=748 to=0.2379 w=0.2379  posed z=1494.85  <- fresh pair, monotonic continuation
```

Across the full post-fix sample the `posed` column is strictly monotonic (zero backward jumps; pre-fix: one ~0.8-frame regression per sim edge) and tracks the smooth CPU `drawPos` column to ~0.1 elmo in steady state. Every `heldSf < sf` window saturates `w` to exactly `to + 1` as designed.

## 5. Alternatives considered

- **(a) full draw-side re-interpolation (CPU rewrite of the SSBO per draw frame)**: NOT master's mechanism (master uploads pairs at sim rate and lerps in-shader); would re-upload every transform every draw frame (the SSBO is tens of MB under load) for strictly less fidelity than the factor fix. Rejected.
- **(b) shader-side velocity extrapolation**: new data channel + every consumer shader; master never extrapolates models (clamped lerp). Rejected.
- **Arrival-clock (key `to` to epoch-arrival time)**: kills the sub-frame hold ripple but introduces a new clock with draw-scheduling phase noise and diverges the model clock from the icon/healthbar clock. Kept as an escalation option if the operator still perceives residual ripple (see §7).

## 6. Verification

- Mechanism probe (`DG_TRA_PROBE`, temporary, removed): pre-fix samples show the per-sim-edge backward jump (§2); post-fix samples show the rendered pose (`posed` column) strictly monotonic across the same mismatch windows, with `w = to + delta` saturating exactly during the ≤1-draw-frame pair lag. Model and shadow source identity is static (one SSBO, one uniform — §3).
- `ninja -C build -j28 engine-legacy` (and `engine-headless`) rc=0.
- Armed diff-gate, flag-OFF, full-length ATG (headless, bar-data3): `PASS (0 mismatches)` on both report lines, 0 DESYNC, reached f=44856 — byte-identity holds (the fix is dead code flag-off).
- Serial headful flag-ON full-length Rosetta (bar-data2): rc=0, reached f=44544, 0 DESYNC, 0 `attempt to`/`Error in` lua errors, 0 ZOMBIE / 0 `Invalid Feature id` / 0 decals-NaN, `[EpochStats] idCoverage: checked=186036 violations=0`, `[SimParkStats] parks=30 totalMs=178.1 avgMs=5.9 maxMs=40.6` (lifecycle-only).
- FF wall-time: engine clock 2:52 for the full Rosetta replay — IDENTICAL to the pre-fix 44b-final headful gate (also 2:52); the fix adds a handful of float ops per draw frame, no regression.

## 7. Flagged follow-ups

- **Piece-level animation**: covered by this fix (piece pairs ride the same SSBO and the same `timeInfo.w`); script animations advance at sim rate by design, exactly as master.
- **Residual sub-frame hold**: if the operator still perceives micro-ripple at low draw FPS (hold fraction grows as draw FPS approaches sim rate), escalate to the arrival-clock variant (§5) — a contained change in the same two functions.
- **`timeInfo.x` epoch-keyed under the flip**: enumerated deviation (advisory-only, ≤1 frame at 1×); any Lua shader comparing `timeInfo.x` against a LIVE frame number it smuggled in via uniforms would see a ≤1 off-by — none found in BAR's templates (they use `x+w`, invariant).
- **Gate replays are recorded fast-forwarded**: every prior "visual" observation on these demos ran at 20×; use `DG_FORCE_1X=1` for any future smoothness verification.

## 8. What the operator should look at

Flag-ON live game or 1× replay, ground camera on moving units:
- Unit models and their shadows should now track as smoothly as their icons/healthbars (compare zoomed-in model motion vs the healthbar above it — pre-fix the healthbar visibly led/steadied while the model stuttered).
- Walking animations (legs) and turret traverse should be smooth (piece pairs).
- Air units: model and shadow should agree.
- Under fast-forward/catch-up, models advance in epoch steps — expected, pre-existing, matches the consume cadence.
