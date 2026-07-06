# PiP minimap engine-API offload — Option 0 + Option 2

## Why

BAR's `gui_pip.lua` reimplements the engine minimap in Lua because the engine minimap can only render the whole map, not an arbitrary world sub-rect. Midgame this costs ~3ms per R2T re-render, dominated by per-unit Lua↔C++ boundary crossings (`GetUnitBasePosition`, `GetUnitLosState`, `GetUnitDefID`) and a from-scratch instance-VBO rebuild + upload, repeated for every visible unit every update frame.

The engine already holds all this data contiguously on `CUnit` and its own `DrawUnitMiniMapIcons` reads it with zero Lua crossings. These two APIs let the widget keep its custom shader/effects while dropping the gather+upload cost.

Not yet measured: confirm the cost split with `config.showPipTimers = true` in a real midgame replay before building. If the time is in icon fill/draw rather than CPU gather, this is the wrong fix (that needs native sub-rect minimap-to-FBO instead — tracked separately).

## Option 0 — bulk LOS export (do first, low risk, standalone)

Goal: replace the per-unit `Spring.GetUnitLosState` storm with one call. LOS is the one per-unit field that genuinely changes every frame and can't be cached widget-side.

Proposed Lua API: `Spring.GetUnitsLosState(allyTeamID, unitIDs)` → flat array of bitmask bytes aligned to the input order (or rect-based variant `Spring.GetUnitsLosStateInRectangle(allyTeamID, xmin, zmin, xmax, zmax)` returning `{unitID, losBits}` pairs).

Engine seam: `rts/Lua/LuaSyncedRead.cpp` next to `GetUnitLosState` (~:8343) and `GetUnitsInRectangle` (~:2993). `losStatus` is `std::array<uint8_t,255>` per unit (`rts/Sim/Units/Unit.h:321`) — read `unit->losStatus[allyTeamID]` in a tight loop. Respect the same allyTeam/spectator permission checks `GetUnitLosState` already enforces.

Widget change: in `GL4DrawIcons` / the keysort+process passes, fetch LOS once into a table keyed by unitID instead of calling per unit inside `processUnit`.

## Option 2 — engine fills the instance VBO (the big cut)

Goal: one engine call walks the quadfield for a world rect and writes the per-unit instance data directly into the widget's GPU buffer in its packed format, eliminating both the per-unit gather and the Lua-table→buffer upload marshal.

Proposed Lua API: `Spring.GetMinimapUnitData(allyTeamID, xmin, zmin, xmax, zmax, vboHandle, attribID)` → count written. Engine fills `{posX, posZ, team, losBits, iconIdx, flags}` (final layout must match the widget's `INSTANCE_STEP` / 12-float instance format — pin this down with the widget before coding).

Engine seam:
- Spatial query: `quadField.GetUnitsExact` (cheap; same path `GetUnitsInRectangle` uses, `rts/Sim/Misc/QuadField.h:116`).
- Per-unit fields already contiguous on `CUnit`: `pos`, `team`, `losStatus`, `currentIconIndex` (`rts/Sim/Units/Unit.h`). Use `GetObjDrawErrorPos(allyTeam)` for fog-of-war-correct position, matching what `DrawUnitMiniMapIcons` does (`rts/Rendering/Units/UnitDrawer.cpp:449`).
- Buffer write target: follow `LuaVBOImpl::InstanceDataFromUnitIDs` (`rts/Lua/LuaVBOImpl.cpp:~1299`, struct fill at :1009-1085) for how to write into a Lua-owned VBO from C++.

Open questions:
- Icon index mapping: widget uses its own `$icons` atlas UVs (`gl4Icons.atlasUVs`), keyed by unitDefID; engine `currentIconIndex` points at `icon::iconHandler`'s atlas. Decide whether the engine returns engine icon idx (and widget maps it) or unitDefID (and widget keeps its current UV lookup). unitDefID is the lower-coupling choice.
- LOS-derived visibility (ghosts, radar wobble, typed/untyped enemy defID hiding) currently lives in `processUnit`. Either return raw `losBits` and keep that logic in Lua, or replicate it engine-side. Start with raw bits in Lua — keeps engine API dumb.
- Selection/damage-flash/tracking/selfD overlays stay widget-side (they're applied on top of the cached block already). Engine fills base data only.

Widget change: replace the keysort + `processUnit` + `vbo:Upload` path in `GL4DrawIcons` with a single `Spring.GetMinimapUnitData(...)` into `gl4Icons.vbo` (and bldg/slow VBOs), then apply the existing dynamic overlays. The mobile/building block caches (>5000 units) become unnecessary for the gather cost and can be simplified.

## Sequencing

1. Land Option 0; wire widget to it; measure delta with `showPipTimers`.
2. Pin the instance-format contract with the widget author (Floris).
3. Land Option 2 base-data fill; keep LOS-derived logic in Lua initially.
4. Re-measure; decide whether native sub-rect minimap-to-FBO (separate todo) is still worth it.
