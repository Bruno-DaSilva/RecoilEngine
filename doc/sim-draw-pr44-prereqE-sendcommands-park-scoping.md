# PR 44 prereq E — `SendCommands` per-action park scoping — LANDED

Status: LANDED on `epoch-integration`. Governing contract: `sim-draw-pr43-44-contract-refresh.md` (§4.5 pause inventory, §7 operator rulings) and `sim-draw-pr44-prerequisites.md` (Prereq E). Companion analysis: `sim-draw-pre44c-pause-surface-clearing.md` §4.5-SendCommands (the per-action net/UI/sim classification this PR implements).

## Problem

`Spring.SendCommands` (LuaUnsyncedCtrl.cpp) wrapped its whole console-action batch in one `ScopedExternalSimPause` (tag `LUA_SEND_COMMANDS`). The pre-44c pause-surface audit measured this park at **~10k engages/game HEADFUL** (vs 22 headless) — stock BAR widgets call `Spring.SendCommands` ~once per draw frame (verified NOT the diff-gate driver), so the conservative full-batch park is effectively a **per-frame** sim re-park. A per-frame park moves the *parked ≈ 0* headline that PR 44b must hit, so it gates 44b.

The audit's classification (§4.5-SendCommands) established that the DRAW-UI and NET-SEND action families (the large majority — camera, rendering/asset config, sound, chat/echo, net-send, pure UI toggles, async redirect-to-synced) touch **no** live sim, and that only a small SIM-POKE minority does (`DumpState`/`DumpRNG`, `ViewSelection`/selection/group, `Give`/`Destroy`/`Remove`, `MaxParticles`/`MaxNanoParticles`, team/spectator/ally/speed/pause/control-unit reads). Deadlock-safety is established: no reachable unsynced executor blocks/waits synchronously on the sim thread, so `ScopedExternalSimPause` (a one-way "sim parks at its own frame edge" request) can be scoped or dropped without deadlock risk.

## Mechanism (conservative-by-default per-action scoping)

Preferred mechanism from the prereq spec: a `touchesSimState` flag on the unsynced action executor, **defaulting TRUE (park)** so anything unclassified stays conservative.

1. **`IUnsyncedActionExecutor::touchesSimState`** (UnsyncedActionExecutor.h) — new bool member, default `true`, with `TouchesSimState()` / `SetTouchesSimState()`. Only unsynced executors are dispatched inline on the draw thread (synced actions are net-sent and consumed asynchronously by the sim), so the flag lives on the unsynced base.

2. **Central classification** (UnsyncedGameCommands.cpp `AddDefaultActionExecutors`) — a file-local `SimSafe(e)` helper flips the flag to `false`; the safe DRAW-UI / NET-SEND registrations are wrapped in `SimSafe(...)`, the SIM-POKE minority (and genuinely-ambiguous / diagnostic / lifecycle executors) are left at the default `true`. Classification lives in one place; no executor class was touched. Notably-left-parked: `Select*`/`Deselect`, `ViewSelection`, `Group*`, `Team`/`Spectator*`/`Ally`, `AI*`, `Track` (follows selection), `Pause`/`Speed*`/`SetGamespeed`/`SpeedControl`, `ControlUnit`, `Mouse1..5` (can drive an order build), `Give`/`Destroy`/`Remove`, `MaxParticles`/`MaxNanoParticles`, `DumpState`/`DumpRNG`, `Save`, `Reload`, `ShareDialog` (opens `CShareBox`, reads teams), and the sim-walking diagnostics (`SnapHashDump`/`BoundaryDump`/`ProfileDump`/`CalloutCensus`/`SplitContractDump`/`SnapshotDiffGate`/`DebugInfo`).

3. **Per-action park in `CGuiHandler::RunCustomCommands`** — the batch dispatch loop resolves each action's unsynced executor and parks only when it touches sim: `park = (executor == nullptr) || executor->TouchesSimState()`. Unresolved actions (guihandler-local UI, server commands, Lua-widget chat actions handled by `GotChatMsg`) keep the park (conservative default — an unclassified path stays safe). A `std::optional<ScopedExternalSimPause>` is constructed only when `park` is true, so sim-safe actions dispatch with no `AcquireSimPause` at all. The recursion-depth guard is untouched; nested `RunCustomCommands` parks are nest-safe no-ops.

4. **The full-batch park in `LuaUnsyncedCtrl::SendCommands` is removed** — scoping now lives entirely in `RunCustomCommands` (which serves both `SendCommands` and the interactive `cd.params`/`cmdDesc.params` callers; the interactive callers already had their own narrow per-site parks — `GUI_GET_COMMAND`/`GUI_GET_BUILDPOS` etc. — so adding a per-action park there is at most more-correct, never a regression). The `LUA_SEND_COMMANDS` telemetry tag is kept on the remaining per-action parks.

Flag-off: `ScopedExternalSimPause` is inert (returns early when the split is off / game null), so flag-off behaviour is byte-identical to before — same as the old batch park, which was also a flag-off no-op. Order is preserved: actions dispatch in sequence; the sim may advance a frame edge between two parked actions, which is safe (console actions are independent, deadlock-safe).

## Gate results

Engine built in the worktree (`ninja -C build engine-legacy` / `engine-headless`, both rc=0). Config `SimDrawSplit=1`, `SplitWindowShrink=1`, `SPRING_DATADIR=/www/projects/bar-data2`.

**Flag-OFF resim (headless, `DG_ARM=0`, full-length, `build/spring-headless`)** — proves no synced-ordering change:
- Rosetta → **SYNC clean (0 DESYNC)**, f=44447, RC=0.
- All That Glitters → **SYNC clean (0 DESYNC)**, f=44867, RC=0.

**Flag-ON headful ×2 per replay** (`build/spring`, `DISPLAY=:0`, `DG_ARM=0 DG_FF=1`, full-length):

| Run | rc | last frame | DESYNC | in-game Lua errors | `LUA_SEND_COMMANDS` parks |
|-----|----|-----------|--------|--------------------|--------------------------|
| rosetta_hf1 | 0 | 44543 | 0 | 0 | **1** |
| rosetta_hf2 | 0 | 44542 | 0 | 0 | **1** |
| atg_hf1 | 0 | 44858 | 0 | 0 | **1** |
| atg_hf2 | 0 | 44861 | 0 | 0 | **1** |

**`[SimPauseSurvey]` `LUA_SEND_COMMANDS` before → after: ~10055/10376 (rosetta), ~9722/9654 (atg) → 1 in all four runs.** The per-frame headful driver was a registered DRAW-UI/NET-SEND executor now marked sim-safe; a temporary per-action census (log any action driving ≥1000 parks) reported **no remaining per-frame parker** in any run, confirming the classification is complete. The residual `1` is a rare SIM-POKE action firing once, correctly still parked. Census + nil-storm counts were 0 across all four runs.

No 38b widget-error nil-storm (`gl.SetFeatureBufferUniforms() Invalid Feature id` from `unit_healthbars_widget_forwarding.lua`); the only `healthbars.*forwarding` log lines are the pregame gadget-load banners.

## Deferred / not in scope

- The temporary per-action park census (a `LOG_L` when an action crosses 1000 parks) was used only to prove no per-frame parker remains; it is **removed before commit** (pure logging, zero behavioural effect).
- **Interactive executor-park behaviour** (a widget/keybind issuing a SIM-POKE action interactively, and the `SendCommands` executor-park deadlock check) is not replay-reachable → covered by the operator's final live skirmish (contract §7.8). The classification's deadlock argument (no executor blocks on the sim) is the static guarantee.
- Under 44b the residual SIM-POKE parks become real sim-thread parks that fire ≈0 times in steady-state gameplay (input-gated), consistent with the *parked ≈ 0* headline; the remaining always-served path for them would be boundary-apply of the pokes (`DumpState`/particle-limits) — a further optimization, not required for the headline.
