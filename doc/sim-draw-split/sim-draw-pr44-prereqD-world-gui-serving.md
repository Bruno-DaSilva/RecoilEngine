# PR 44 prereq D — world-GUI serving (unpark DrawMapStuff + MiniMap frustum + non-cursor GetDefaultCommand)

Status: LANDED on `epoch-integration`. Governing contract: `sim-draw-pr43-44-contract-refresh.md` §8 / §7 operator rulings; companion `sim-draw-pr44-prerequisites.md` "NEW 44b prerequisites … Prereq D"; disposition source `sim-draw-pre44c-pause-surface-clearing.md` §4.5. Base HEAD: `a22b710ce5` (§8.1 FPS direct-control camera write).

Goal: the pre-44c audit measured three mid-gameplay `ScopedExternalSimPause` sites that re-park the running sim **every draw frame** (`GUI_DRAW_MAPSTUFF`, `GUI_GET_DEFAULT_CMD`, `MINIMAP_FRUSTUM`-by-construction). They move the *parked ≈ 0* headline, so they gate 44b. This PR serves the per-frame reads so those sites drop to ~0. It is a wiring PR (Wave-5 style, flag-off byte-identical).

## What the audit premise got right, and the two corrections found while wiring

The audit escalation said: route DrawMapStuff's queue reads → command-queue cache, traces → "PR-35 trace channel", blocking/yardmap → blocking mirror + placement channel. Mapping the landed machinery corrected two load-bearing premises:

1. **`TraceRay::GuiTraceRay` is already snapshot-backed and draw-safe.** It reads `SimSnapshot` rows + `snapshotPickGrid` and returns a hit distance + snapshot hit-ids; the only live touch is a `Valid`-gated id→pointer resolve for pointer-taking callers (TraceRay.cpp:587). There is **no** pick-carrying query/reply channel — the "PR-35 trace channel" (`WeaponTraceQuery`/`EvaluateTraceQueries`) serves only four **boolean weapon predicates**, not picks. So a GuiTraceRay whose result is consumed as *distance + ids* needs **no park** (the `TraceScreenRay` precedent, LuaUnsyncedRead.cpp:3551, already calls it park-free from draw context). A GuiTraceRay whose returned **live pointer is then dereferenced** for weapon/unit state still needs the sim quiescent.

2. **The command-queue and placement channels have no draw-side C++ read path for what DrawMapStuff needs.** The command-queue cache exposes exactly one C++ accessor (`GetServedAvailableCommands`, descs+page) — there is **no** served `GetOverlapQueued` twin and no raw-queue accessor. The placement channel (`RoutePlacementQuery`) is a `lua_State*` router keyed on raw float world-pos (cursor-follow keying is a documented persistent-miss), and `TestUnitBuildSquare`/`ShowUnitBuildSquare` have **no** blocking-mirror accessor (the mirror gives cell[0] `BlockedAt` only, not the `BUILDSQUARE_*` verdict). So DrawMapStuff's build-preview and weapon-range interior reads are the **residual with no channel**.

Net: the per-frame *driver* of both `GUI_DRAW_MAPSTUFF` and `GUI_GET_DEFAULT_CMD` is the unconditional context-cursor `GetDefaultCommand` call, which **is** servable (the Gap-B single-slot reply). Everything else in DrawMapStuff that reads live sim is **input-gated** (attack/build command active, shift-hover, or a mouse drag) — it measured 0 in both replays — so it keeps a **narrow** park scoped to just that block. This is exactly the PARTIAL-LAND expectation the task named ("TryTarget's weapon-state / range-ring reads as the likely residual").

## Changes

### 1. Non-cursor `GetDefaultCommand` → served (removes the per-frame `GUI_GET_DEFAULT_CMD` park)

`CGuiHandler::GetDefaultCommandServed(x, y[, camPos, dir])` (GuiHandler.h/.cpp): under the running split (draw runs post-`ReleaseSimPause`, sim live-not-parked) AND when `(x,y)==(mouse->lastx,lasty)`, it reads the **existing Gap-B single standing reply slot** (`defaultCmdReplyCmd`/`Valid`) and requests a re-eval (`defaultCmdQueryPending=true`); otherwise it runs the live parking `GetDefaultCommand` (byte-identical flag-off / sim-parked / pregame / non-cursor point). All three per-frame draw-path callers query the default command at the *current mouse ray* — the same logical query SetCursorIcon's cursor path already registers — so they share the one slot:

- `CGuiHandler::DrawMapStuff` (the unconditional range-circle default-cmd read, GuiHandler.cpp).
- `CGuiHandler::DrawCentroidCursor`.
- `LuaUnsyncedRead::GetDefaultCommand` (`Spring.GetDefaultCommand`, polled ~per frame by widgets).

Semantics: the `DefaultCommand` widget-override event fires once per barrier (in `EvaluateDefaultCmdQuery`) instead of once per draw-path call; every reader gets the same overridden value. This is the tradeoff Gap-B already made for the cursor path (§7.2 ruling A′), extended to its non-cursor siblings. The input-path callers (`MousePress`, `GetCommand`) keep the live parking `GetDefaultCommand` (input-gated, not per-frame).

### 2. `MiniMap::DrawCameraFrustumAndMouseSelection` → park dropped

The frustum GuiTraceRay consumes **only** the returned hit distance (to pick the frustum draw height) and ignores the resolved unit/feature pointers. Snapshot-backed + distance-only ⇒ draw-safe under the running split (the `TraceScreenRay` park-free precedent). Park removed; the selection box below reads only draw-owned mouse/map state. `MINIMAP_FRUSTUM` enum retired (kept for telemetry-id stability, no longer counted).

### 3. `CGuiHandler::DrawMapStuff` → whole-pass park dropped; residual interior blocks keep narrow parks

- Whole-pass `ScopedExternalSimPause(GUI_DRAW_MAPSTUFF)` at the top removed.
- The `activeMousePress` drag block runs **park-free** — it reads only `CGround` (draw-safe unsynced heightmap) + draw-owned mouse/command state (`DrawFormationFrontOrder`/`DrawArea`/`DrawSelectBox` verified to touch no unit/handler state).
- Three residual interior blocks keep a **narrow `GUI_DRAW_MAPSTUFF` park** (nest-safe, no-op flag-off / when already parked), each **input-gated**:
  - pointee weapon-range rings (`GetQueueKeystate()`, i.e. shift-hover): live `unit->weapons/maxRange/decloak/stockpileWeapon` derefs, no channel.
  - build preview (`inCommand` is a building command): builder-CAI walk + `TestUnitBuildSquare`/`ShowUnitBuildSquare` + `GetOverlapQueued` build-overlap — none has a draw-side served accessor; the nested `GetBuildPositions`/`GetCommand` parks nest no-op under it.
  - attack range rings (`playerAttackCmd || defaultAttackCmd`): live selected-unit `weapons/maxRange/pos` derefs.
- `pointeeUnit` (resolved+dereferenced only inside the shift-hover park) is *pointer-compared* only in the later blocks; a stale-slot equality compare is cosmetic (a one-frame ring flicker), no deref, §C-class, and doubly input-gated.

## Residual + escalation

Fully unparking DrawMapStuff's interior needs machinery this PR does not build:
- a **weapon-state / range channel** (served `CUnit::weapons[0]`/`maxRange`/`decloakDistance`/`stockpileWeapon`) — the range rings' hard residual;
- a **C++ served command-queue read** (`GetServedCommandQueue`/`GetServedOverlapQueued`) — the cache holds the flattened queue but exposes only descs+page to C++;
- a **draw-side `TestUnitBuildSquare` verdict** (placement channel given a C++ query path, or a fuller blocking/terrain mirror).

All three are input-gated (0 in the replay gates), so they do not move the *parked ≈ 0* headline; they are follow-up serving work, not 44b blockers. Interactive frequency of these residual parks is covered only by the operator's final live skirmish (§7.8).

## Gate results

Built in the worktree (`ninja -C build engine-legacy` / `engine-headless`, both rc=0). Config `SimDrawSplit=1`, `SplitWindowShrink=1`, `SPRING_DATADIR=/www/projects/bar-data2`.

**Armed diff-gate** (headless, `DG_ARM=1 DG_FF=1`, full-length, `build/spring-headless`):
- Rosetta → **GATE PASS (0 mismatches** across every channel incl. `unit:rules`/`feature:rules`/`cq:lastPage`/mirrors/callouts), **SYNC clean (0 DESYNC)**, f=44539.
- All That Glitters → **GATE PASS (0 mismatches), SYNC clean (0 DESYNC)**, f=44860.
- (`defCmd checked=0` under split-on is expected — the defCmd armed dual-run is the flag-*off* leg, disabled while the sim thread runs; this PR added no new served field, so no new field-pass. The served `GetDefaultCommandServed` is flag-off byte-identical (it calls the live parking `GetDefaultCommand`), so the pre-existing SetCursorIcon `defCmd` flag-off check is unchanged.)

**Flag-ON headful matrix** (`build/spring`, `DISPLAY=:0`, `DG_ARM=0 DG_FF=1`, full-length, 2 replays × 2):

| Run | last frame | DESYNC | in-game Lua errors | 38b nil-storm |
|-----|-----------|--------|--------------------|---------------|
| rosetta_hf1 | 44719 | 0 | 0 | 0 |
| rosetta_hf2 | 44542 | 0 | 0 | 0 |
| atg_hf1 | 44857 | 0 | 0 | 0 |
| atg_hf2 | 44860 | 0 | 0 | 0 |

No `gl.SetFeatureBufferUniforms() Invalid Feature id` nil-storm from `unit_healthbars_widget_forwarding.lua`; no crash, no DESYNC.

**[SimPauseSurvey] dump** — the three target sites dropped to **0 engages** in all four headful runs (a site absent from the survey = 0):

| Site | pre-PR (audit, headful) | this PR (headful ×4) |
|------|------------------------:|---------------------:|
| `GUI_DRAW_MAPSTUFF` | 12418 / 12786 / 13304 / 13139 | **0 / 0 / 0 / 0** |
| `GUI_GET_DEFAULT_CMD` | 10011 / 10333 / 9719 / 9650 | **0 / 0 / 0 / 0** |
| `MINIMAP_FRUSTUM` | 0 (config-dependent) | **0** (park retired) |
| `LUA_SEND_COMMANDS` (prereq E, unchanged) | ~10k | 11265 / 11543 / 11201 / 11253 |

The only per-frame headful park left is `LUA_SEND_COMMANDS` (prereq E, a separate PR). The residual DrawMapStuff interior narrow parks (`GUI_DRAW_MAPSTUFF` around the weapon-range / build-preview blocks) engaged **0** times — they are input-gated, and replays drive no interactive input (§7.8), so their steady-state count is ~0 by construction; their true interactive frequency is covered only by the operator's final live skirmish.

**All gates GREEN.**
</content>
</invoke>
