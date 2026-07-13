# WS-9 `epoch-cadence-decimation.md` — RULING: publish-rate ceiling

Status: RULING DOC. No code lands until the operator rules. This is the parent program's WS-9 (`doc/sim-draw-split-optimization/00-program-overview.md` §4). It is orthogonal to WS-1..8 (§2/§3/§6 of the overview are binding) and, per the sequencing note (overview §5), should be **decided after WS-1..7 land**: those workstreams shrink the per-epoch produce cost; WS-9 caps how many times per wall-second that (now smaller) cost is paid. It bounds whatever residue remains, independent of world size.

The mechanism is a publish-rate **ceiling**. The §3.2 pacing gate already has a *floor* fallback (a 250 ms `timeDue` republish while idle) but **no ceiling**: while frames flow the producer publishes as fast as the draw side consumes. This doc analyses current cadence, designs two ceiling mechanisms, enumerates the fidelity costs the operator must weigh (the heart of the ruling), designs a gate-able knob, and lists the telemetry that would validate it.

## 1. Current behaviour (verified against the branch)

### 1.1 The producer and its pacing gate

`CGame::ProduceEpochAtSimEdge` (`rts/Game/Game.cpp:2104`) is the sole publish path. It is called at the top of the sim-thread loop, `CGame::SimThreadProc` (`rts/Game/Game.cpp:2307`), i.e. at a sim frame edge only (the inter-packet yield point does not produce). Its decision has three gates in order:

- **(p2) pacing / skip-if-unconsumed** — `if (!simSnapshot.NewestEpochConsumed()) return;` (`rts/Game/Game.cpp:2132-2133`). `NewestEpochConsumed()` (`rts/Rendering/Common/SimSnapshot.h:1314`) is true iff the consumer has stamped `consumedEpochId == epochCounter`; the consumer stamps it in `MarkNewestEpochConsumed()` (`SimSnapshot.h:1324`, called from the barrier at `Game.cpp:1767/1848`). Effect: **at most one unpublished-then-unconsumed epoch outstanding**; produce cost is paid at `min(sim rate, draw acquire rate)`.
- **(p3) due-check** — `rts/Game/Game.cpp:2147-2178`. `framesDue` (`SimSnapshot::FramesDue()`, `rts/Rendering/Common/SimSnapshot.cpp:309`: newest published `simFrame != gs->frameNum` OR the active-unit count changed) publishes **unconditionally**. The non-frame classes (`mutationDue`, `recsDue`, `timeDue`) publish only while `frameIdle` (no new frames for ≥100 ms), to avoid same-frame republishes tripping BAR's zombie recovery.
- **the floor fallback** — `const bool timeDue = (now - lastEpochPublishTime).toMilliSecsf() >= 250.0f;` (`rts/Game/Game.cpp:2158`). This is a **floor** (republish at *least* every 250 ms while idle/paused, so paused team/player/global rows and between-frames mutation marks refresh at ≥4 Hz). It is not a ceiling and never suppresses a publish; it only *adds* one.

There is no branch anywhere in `ProduceEpochAtSimEdge` that throttles the *upper* publish rate. The only upper bound is emergent: the draw side's acquire rate (via p2) and the sim rate (via `framesDue`).

### 1.2 Cadence at 1× play

At 1× the sim runs at `GAME_SPEED` (30 Hz) and the draw side is normally faster (e.g. 60 FPS). Every sim frame flips `framesDue` true, and the draw consumes each published epoch before the next sim edge, so p2 does not skip. **Publish cadence at 1× = the sim frame rate (30 Hz), one epoch per sim frame** — until late game, where the sim frame itself slows below 30 Hz (and, per the §1 baseline, the ~13 ms produce is now *added* to each of those already-slow frames). So at 1× the cost is already self-limiting to the sim rate, but that rate is exactly where produce hurts most.

### 1.3 Cadence under fast-forward (PR 46)

Under full throttle (`CGame::SplitFullThrottleConsume`, `doc/sim-draw-pr46-full-throttle-design.md` §1) the consumption loop's exit is **epoch-consumed**: `ClientReadNet` returns when a NEWFRAME was consumed this pass AND `NewestEpochConsumed()` is true (`rts/Game/Game.cpp:2333-2338`), letting the next loop-top `ProduceEpochAtSimEdge` publish immediately. The sim runs many frames per publish only when the draw side is slow to acquire; otherwise **one publish per consumed epoch ≈ one publish per draw acquire**. PR 46's own telemetry note (pr44a design §Telemetry "FF wall-time note"; pr46 §D3) records the consequence: extraction cost is paid at **draw rate on the sim thread**, which becomes the FF throughput bottleneck. The escape hatch PR 46 shipped-without (pr46 §Escape hatch: "require ≥K frames consumed before honoring `NewestEpochConsumed()`") **is exactly WS-9's knob** — this doc formalises it.

### 1.4 Multi-frame epochs are already a first-class shape

The ring already carries a frame *span*, not a single frame. `EpochSlotMeta` holds `int32_t firstSimFrame / lastSimFrame` (`rts/Rendering/Common/SimSnapshot.h:1444-1447`); the producer stamps `firstSimFrame = prev-slot.lastSimFrame + 1`, `lastSimFrame = buffers[target].simFrame` (`rts/Rendering/Common/SimSnapshot.cpp:446-447`), and `[EpochStats]` logs the span per slot (`SimSnapshot.cpp:639`). Under FF catch-up an epoch already routinely covers a multi-frame batch, and the sealed record/closure/shell batches (`Game.cpp:2245-2247`) already cover exactly one such batch. **Decimation therefore reuses existing machinery** — a wider ceiling just makes multi-frame spans the common case at 1× too. No new span concept is needed; only a gate that *withholds* a publish that current logic would emit.

### 1.5 Deferred-event dispatch is already epoch-granular

`renderEventQueue.SealEpochBatch(slot)` at the producer (`Game.cpp:2245`) seals the pending render-event records into the slot; the consumer dispatches exactly that batch at the barrier via `renderEventQueue.DispatchSealedBatch(HeldSlot())` (`Game.cpp:1747, 1768`). The same seal/dispatch pairing exists for `UnsyncedBoundaryQueue` closures and `DeferredObjectDeleter` shells (`Game.cpp:2246-2247`). **Events already dispatch at epoch boundaries, once per consumed epoch.** Coarser epochs ⇒ coarser event dispatch directly — this is the second-largest fidelity cost (§3.2).

## 2. Mechanism options

Both options add a single early-return inside `ProduceEpochAtSimEdge`, after the p2 pacing gate but **guarded so it can never suppress the floor**: the return is taken only when `framesDue` is the sole reason to publish AND the game is actively running (not paused, not pregame). It must fall through whenever `mutationDue || recsDue || timeDue` would fire under `frameIdle`, so paused/idle liveness (the §1.1 floor) is untouched. When the ceiling withholds a publish, the sim frame(s) still run; the withheld frames simply fold into the next published epoch's span (§1.4) exactly as an FF catch-up batch does today.

### Option A — fixed ceiling (`SimDrawMaxEpochRate`, Hz)

A wall-clock minimum inter-publish interval. Add, alongside `lastEpochPublishTime` (`Game.cpp:2092`):

```
const float maxRateHz = SimDrawSplit::MaxEpochRate();   // 0 = uncapped (default)
if (maxRateHz > 0.0f && framesDue && !frameIdle) {
    const float minIntervalMs = 1000.0f / maxRateHz;
    if ((now - lastEpochPublishTime).toMilliSecsf() < minIntervalMs)
        return;   // fold these frames into the next epoch's span
}
```

- **Pros:** trivial, predictable, easy to reason about and gate; the withheld-frames-fold behaviour is identical to FF catch-up so it exercises a proven path; one number the operator sets.
- **Cons:** a fixed Hz is world-size-blind — 20 Hz early game wastes the cap's headroom (produce is cheap there), 20 Hz late game may still be too fast if produce grew past `1000/20 = 50 ms`. It caps *rate*, not *cost fraction*, so it does not self-tune to the thing WS-9 exists to bound (produce as a share of sim-thread time).

### Option B — adaptive interval (target produce ≤ X % of sim-thread time, floor ~10–15 Hz)

Measure per-epoch produce cost and the sim-thread busy time, and stretch the inter-publish interval so produce stays under a budget fraction, clamped to a floor rate so cadence never collapses to a slideshow.

```
// EWMA of the last produce durations (the SCOPED_TIMER("Sim::EpochProduce")
// span, Game.cpp:2111 -- already measured; also the TIMING_EPOCH_PRODUCE slice)
const float produceMs   = avgProduceMs;              // maintained across publishes
const float budgetFrac  = SimDrawSplit::MaxEpochBudgetFrac();  // e.g. 0.15 = 15%
const float floorHz     = SimDrawSplit::MinEpochRate();        // e.g. 12
// produce should be <= budgetFrac of the interval:  interval >= produceMs / budgetFrac
const float wantIntervalMs = std::min(produceMs / std::max(budgetFrac, 1e-3f),
                                      1000.0f / floorHz);
if (budgetFrac > 0.0f && framesDue && !frameIdle) {
    if ((now - lastEpochPublishTime).toMilliSecsf() < wantIntervalMs)
        return;
}
```

Worked example against the §1 baseline (~13.3 ms produce post-damages-gate, pre-WS-1..7): at 15 % budget the interval floors to `13.3/0.15 ≈ 89 ms` ⇒ ~11 Hz, near the floor. After WS-1..7 bring produce to ~3–4 ms, the same 15 % budget gives `~23 ms` ⇒ ~43 Hz, i.e. the cap barely engages at 1× and only bites under FF — which is the desired shape (spend the produce budget only when it is actually large). The floor (`MinEpochRate`) guarantees ≥12 Hz regardless, so the staleness/event-latency ceiling is bounded at ≤~83 ms even if produce blows out.

- **Pros:** self-tuning; bounds the *actual* quantity of interest (produce as a fraction of sim-thread time); degrades gracefully as WS-1..7 land (auto-loosens); one budget number is intuitive ("produce may cost at most 15 % of the sim thread").
- **Cons:** needs a maintained produce-cost EWMA and a sim-thread-busy denominator (or the simpler produce-fraction-of-wall proxy above); more moving parts to validate; the effective rate is not a round number, complicating A/B repro (mitigated by the histogram telemetry, §5).

### Recommendation: Option B, with Option A's fixed cap available as a debugging/repro override

Ship the adaptive budget as the operator-facing default-off knob because it bounds the right quantity and auto-loosens as the other workstreams shrink produce. Keep a fixed `SimDrawMaxEpochRate` as a secondary override (0 = ignore) for deterministic A/B measurement and as a hard safety cap layered over B (`interval = max(adaptive, fixed)`). Both share the single guard site and the fold-into-next-span behaviour.

## 3. Fidelity cost enumeration (the ruling)

This is what the operator is deciding. Decimating to a floor of 10–15 Hz means epochs 66–100 ms apart. Everything the split serves at epoch granularity gets that much staler / coarser.

### 3.1 Served callouts up to ~one epoch-interval staler

All six SimSnapshot row namespaces + the per-slot cmd-queue / piece / mirror channels are captured at the epoch edge and served until the next epoch (the §3.3 staleness contract in pr44a already makes rows ≤1 consume-interval old; decimation widens that interval to ≤66–100 ms). Widget classes that read these and visibly care:

- **Healthbars / build progress / stun/para state** — `Spring.GetUnitHealth`, `GetUnitIsBeingBuilt`, para state: values step at 10–15 Hz instead of 30 Hz. A unit taking rapid damage shows a bar that lurches in bigger increments; a unit dying between epochs shows full health until the next boundary. Visible but arguably tolerable (the value is monotone and the error is ≤100 ms).
- **Unit trackers / minimap blips / icon state** — position and existence read from rows; a fast unit's tracker dot and off-screen icon update at the epoch rate. Note **model draw position is interpolated separately** (§3.3) so the 3D model stays smoother than its healthbar/icon — a model-vs-overlay offset up to the epoch interval (pr44a §"Enumerated deviations" already notes a ≤1-frame version of this; decimation widens it to ≤100 ms).
- **Selection info / unit-stats panel / command tooltip** — `GetUnitCommands`, `GetUnitWeaponState`, rules params: the selected-unit HUD refreshes at 10–15 Hz. Users hovering values will see them tick coarsely. Command-queue draw (the queued-order lines) redraws at the epoch rate — see §3.4.
- **LOS / radar / jammer state** (WS-7's block) — `Spring.GetUnitLosState`: an enemy entering LOS becomes visible up to one epoch later. At 12 Hz that is ≤83 ms of "I saw it late". Competitively perceptible at the extreme floor but bounded.

### 3.2 Deferred events dispatch at coarser boundaries — the worst cost

Because render-event records, unsynced closures, and destroy shells dispatch **once per consumed epoch** (§1.5), decimation directly coarsens *event latency*, and events are where humans notice latency most:

- **`UnitCreated` / `UnitFinished`** → the build-complete flash, the "unit ready" sound, factory exit effects fire up to one epoch (≤100 ms) late and **batched** — several units finishing across the decimated window all pop on the same draw frame instead of spread out. This reads as a visible hitch/clump, not just a delay.
- **`UnitDestroyed`** → death explosions, wreck/heap spawns, and the disappearance of the unit's overlays all land on the epoch boundary. Batched deaths (a bombing run, an area-effect) all resolve on one frame — a perceptible stutter of effects.
- **LOS-driven events (`UnitEnteredLos` / `UnitLeftLos`, ghost updates)** → ghost-unit position updates and icon pops for units crossing the LOS edge fire at the epoch rate. Ghost "teleport" on update becomes chunkier.
- **General "icon pops"** — any widget reacting to a boundary event (selection changes, cloak toggles, nano-spray start/stop) inherits the epoch granularity.

This is the cost to weigh hardest: staleness of a *value* (§3.1) is a small monotone error, but batched/late *events* produce a visible clumping that the eye reads as a frame hitch even when average FPS is fine. The 10–15 Hz floor exists precisely to keep this ≤~83–100 ms; going below it is not recommended at any budget.

### 3.3 Interpolation smoothness — a design trap, not just a cost

Draw position is interpolated: `GetDrawPos(float t) = mix(preFrameTra.t, pos, t)` (`rts/Sim/Objects/WorldObject.h:74`), with `t = globalRendering->timeOffset = (currentTime - lastFrameTime) * weightedSpeedFactor`, `weightedSpeedFactor = 0.001f * gu->simFPS` (`rts/Game/Game.cpp:1362-1364`). The interpolation **base** is `preFrameTra`, saved by the sim at the *start of each sim frame* (`rts/Sim/Objects/SolidObject.cpp:494`), and the transform-extraction pass captures exactly the pair `[preFrameTra (frame F−1), tmCurr (frame F)]` at the epoch edge (`rts/Rendering/Common/ModelDrawerData.h:419-424`).

The trap: **the interpolation base spans a single sim frame (F−1→F), not the epoch.** If the sim runs frames F−1, F, F+1, F+2 but only publishes an epoch at F+2, the extracted pair is `[F+1, F+2]` — the draw interpolates only the *last* sim frame of motion. The intermediate frames' motion is not interpolated; the unit reaches its F+2 pose over one frame's worth of `timeOffset` (which clamps to 1.0 fast, since `weightedSpeedFactor` is scaled by the true `simFPS`) and then **teleports** one epoch-step at the next publish. Naïve decimation therefore makes units **judder at the epoch rate** — smooth motion collapses to a 10–15 Hz stop-motion with per-epoch position jumps, which is far more objectionable than stale healthbars.

To keep motion smooth under decimation the design must additionally:

1. **Widen the interpolation base to the previous *epoch's* pose** (chain `preFrameTra` per-epoch instead of per-sim-frame), so `mix` spans the whole decimated interval; AND
2. **Scale `weightedSpeedFactor` (or `timeOffset`) by the epoch span** so `t` traverses 0→1 over the epoch's wall-gap, not one sim frame's.

Both are real changes to the interpolation machinery (`Game.cpp:1360-1440` and the transform-extraction base), and they trade the judder for **increased visual lag / rubber-banding** (the model now lerps linearly across 66–100 ms, so a unit that changes direction mid-epoch cuts the corner). This is a genuine design cost that must be scoped into any WS-9 implementation PR — the ceiling knob is not "one early-return" if smooth motion is required; it is "one early-return **plus** an epoch-span interpolation rework". The ruling should decide whether smooth-motion-under-cap is required (implies the rework) or whether the cap is FF-only (where judder during fast-forward scrub is acceptable and the rework can be skipped).

### 3.4 Command-queue / selection UI staleness

`RefreshCommandQueues` (per-slot, `Game.cpp:2185`) and the piece cache feed the drawn command lines, formation ghosts, and build-queue overlays. These redraw at the epoch rate: a player issuing a fast sequence of orders sees the queue lines update at 10–15 Hz. The default-command cursor query (`EvaluateDefaultCmdQueryAtSimEdge`, `Game.cpp:2170/2215`) is evaluated at the epoch edge too, so the context cursor under the mouse updates at the epoch rate — perceptible when sweeping the cursor over mixed terrain/units. Bounded and low-severity, but it is the interaction-latency channel most directly under the player's hand.

### 3.5 Interaction with the §3.2 floor and with FF

- **Floor (250 ms `timeDue`) vs ceiling:** complementary and non-conflicting *by construction* of the §2 guard — the ceiling returns only on the `framesDue && !frameIdle` path, which the floor never touches (the floor fires under `frameIdle`). Paused/idle liveness (team/player/global at ≥4 Hz, cursor tracking, pending closures) is unaffected. The ceiling only stretches the *active-play* frame-driven cadence.
- **FF (PR 46):** the ceiling is the escape hatch PR 46 deferred (pr46 §Escape hatch; pr44a §Telemetry "candidate knobs … a producer min-publish-interval under free-run"). Under FF it makes multiple consumed epochs coalesce into one publish, so extraction cost stops tracking draw rate and FF throughput recovers. This is arguably WS-9's *primary* payoff: FF scrubbing tolerates coarse events/judder (§3.3 rework may be skipped for FF-only), and the wall-time regression PR 46 flagged (strict ATG 12:35 vs 6:57) is what the cap directly attacks.
- **Backpressure (PR 44c) is unaffected:** `CanConsumeSimFrameNow` (kept, pr46 §"Explicitly KEPT") bounds the sim to N−1 unretired epochs ahead. The ceiling reduces publish *frequency*, not ring occupancy, so it composes with backpressure without new interaction.

## 4. Knob design (gate-able)

Register in `rts/System/SimDrawSplit.cpp` beside the existing `CONFIG(int, SimDrawSplit)` (`rts/System/SimDrawSplit.cpp:16`), following the `SplitDrawContract` / `SplitWindowShrink` precedent (`rts/Lua/LuaSplitContract.cpp:20`, `rts/Game/Game.cpp:177`). Accessors on the `SimDrawSplit` namespace (`rts/System/SimDrawSplit.h:38`), cached like `Enabled()`:

- **`CONFIG(float, SimDrawMaxEpochBudgetFrac).defaultValue(0.0f)`** — Option B. `0 = uncapped (default OFF)`. `0.15` = produce may consume ≤15 % of the inter-publish interval. Only consulted when `Enabled() && SimThreadRunning()`.
- **`CONFIG(int, SimDrawMinEpochRate).defaultValue(12)`** — Option B floor (Hz). Ignored when the budget knob is 0. Clamps the adaptive interval so cadence never drops below this even if produce blows out. Range guidance 10–15.
- **`CONFIG(float, SimDrawMaxEpochRate).defaultValue(0.0f)`** — Option A / hard override. `0 = ignore`. A fixed Hz cap layered as `interval = max(adaptiveInterval, 1000/MaxEpochRate)`. Primary use: deterministic A/B repro.

**Defaults keep the feature entirely off** (all zero except the inert floor), so flag-on-but-uncapped behaviour is byte-identical to today and every existing gate stays valid without re-baselining. Interaction with FF is automatic (the knobs read the same `Enabled()/SimThreadRunning()` predicate PR 46 uses); no separate FF knob is needed, though the ruling may choose to *only* honour the cap under the FF carve-out (guard the return additionally on `gs->speedFactor > 1.01 || IsSimLagging()`) if the §3.3 interpolation rework is declined and judder is acceptable only during scrub.

## 5. Telemetry to validate

Extend the existing teardown lines; no new subsystem needed:

- **Produce-time fraction** — the `SCOPED_TIMER("Sim::EpochProduce")` span (`Game.cpp:2111`) and the `TIMING_EPOCH_PRODUCE` frame-grapher slice (`Game.cpp:2263`) already measure per-publish produce cost. Add a teardown aggregate: total produce ms / total sim-thread wall ms = the fraction the budget knob targets. This is the headline "did the cap hold the budget" number.
- **Epoch-rate histogram** — bucket the inter-publish interval (`now - lastEpochPublishTime`) per publish and dump at teardown (beside `[EpochStats]`, `rts/Rendering/Common/SimSnapshot.cpp:634`). Confirms the achieved cadence distribution vs the configured floor/cap and exposes whether the cap engages at 1× (it should not, post-WS-1..7) vs FF (it should).
- **Withheld-publish count** — a counter incremented at the §2 guard return (mirror `g_ffBackstopExitCount` / `[BackpressureStats]`, pr46 §4). Zero at 1× with a loose budget; large under FF = the cap doing its job. A large count at 1× is the warning sign that produce is still heavy (WS-1..7 incomplete) or the budget is too tight.
- **Span width** — `[EpochStats]` already logs `span=[first,last]` (`SimSnapshot.cpp:639`); under the cap, spans widen from 1 frame to N. Track max/avg span to confirm frames fold rather than drop.
- **Event-batch size** — the sealed-batch record counts per epoch (already the seal path, `Game.cpp:2245-2247`); a rising per-epoch event count under the cap quantifies the §3.2 clumping cost directly.

Validation protocol (per program §6): flag-off byte-identity is trivial (knobs default off). Flag-on-uncapped byte-identity confirms the guard never fires when the knobs are zero. Flag-on-capped is verified by eyeball (per program rule: no A/B pixel gate for decoupling work) for the §3.2/§3.3 visual costs, plus the histogram/fraction telemetry for the numeric target, plus a demo-resim to confirm synced state is untouched (the cap decides *when* to publish, never packet order — same argument as PR 46's flag-off determinism, pr46 §Verification).

## 6. Decision points

1. **Ship WS-9 at all?** It is orthogonal to WS-1..8 and only bounds residual produce cost per wall-second. If WS-1..7 (and WS-8) land produce at ~2–4 ms, at 1× that is already ≤sim-frame cost and the cap barely engages; the case for WS-9 then rests almost entirely on **FF throughput** (§3.3/§3.5). **Recommendation: yes, but FF-first (see #4).**
2. **Mechanism: fixed (A) or adaptive (B)?** **Recommendation: B (adaptive budget) as the primary knob, A (fixed Hz) as an override/repro cap.** B bounds the actual quantity (produce fraction) and auto-loosens as WS-1..7 land.
3. **Floor rate?** **Recommendation: 12 Hz default (`SimDrawMinEpochRate`), range 10–15.** Below 10 Hz the §3.2 event-clumping becomes a visible hitch regardless of budget.
4. **Does the cap apply at 1× play, or FF-only?** The interpolation trap (§3.3) means smooth motion under a 1× cap requires an epoch-span interpolation **rework** (widen the base to the previous epoch + scale `timeOffset` by span), which trades judder for corner-cutting lag. FF-only capping avoids the rework (scrub judder is acceptable). **Recommendation: land FF-only first** (guard the cap on the FF carve-out), gated behind the budget knob; defer the 1× interpolation rework to a follow-up decision only if late-game 1× produce cost proves it necessary after WS-1..7.
5. **Default state?** **Recommendation: OFF (all knobs zero except the inert 12 Hz floor).** Preserves every existing gate baseline; the operator opts in per-measurement.
6. **Sequencing?** **Recommendation: decide and land after WS-1..7** (overview §5): their produce reduction changes whether the cap ever engages at 1× and thus whether the §3.3 rework is even in scope.

---

### Appendix — key file:line index

- Producer / gates: `rts/Game/Game.cpp:2104` (`ProduceEpochAtSimEdge`), `:2132-2133` (p2 pacing skip-if-unconsumed), `:2147-2178` (p3 due-check), `:2158` (250 ms floor `timeDue`), `:2245-2247` (seal batches), `:2253` (publish), `:2263` (`TIMING_EPOCH_PRODUCE`), `:2307` (called from `SimThreadProc`), `:2333-2338` (FF epoch-consumed exit).
- Pacing predicate: `rts/Rendering/Common/SimSnapshot.h:1314` (`NewestEpochConsumed`), `:1324` (`MarkNewestEpochConsumed`), `rts/Rendering/Common/SimSnapshot.cpp:309` (`FramesDue`).
- Multi-frame span: `rts/Rendering/Common/SimSnapshot.h:1444-1447` (`EpochSlotMeta` span), `rts/Rendering/Common/SimSnapshot.cpp:446-447` (span stamp), `:639` (`[EpochStats]`).
- Event dispatch: `rts/Game/Game.cpp:1747, 1768` (`DispatchSealedBatch`), `:2245-2247` (`SealEpochBatch`).
- Interpolation: `rts/Sim/Objects/WorldObject.h:74` (`GetDrawPos` mix), `rts/Sim/Objects/SolidObject.cpp:494` (`preFrameTra` save), `rts/Rendering/Common/ModelDrawerData.h:419-424` (extract `[preFrameTra, tmCurr]`), `rts/Game/Game.cpp:1362-1364` (`timeOffset` / `weightedSpeedFactor`).
- Knob precedent: `rts/System/SimDrawSplit.cpp:16` (`CONFIG(int, SimDrawSplit)`), `rts/Lua/LuaSplitContract.cpp:20` (`SplitDrawContract`), `rts/Game/Game.cpp:177` (`SplitWindowShrink`), `rts/System/SimDrawSplit.h:38-46` (namespace accessors).
</content>
</invoke>
