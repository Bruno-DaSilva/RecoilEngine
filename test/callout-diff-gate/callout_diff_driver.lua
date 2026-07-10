function widget:GetInfo()
	return {
		name      = "Callout Diff Gate Driver",
		desc      = "Headless replay driver for the PR 17 SimSnapshot differential gate: fast-forwards, arms /snapshotdiffgate, keeps the draw-side positions-family callout surface hot each draw frame, dumps the report at game end, then quits.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 1000,
		enabled   = true,
	}
end

-- Knobs come from engine config so the runner can parameterize without editing
-- this file (same pattern as test/s0-measurement):
--
--   DiffGateLabel        : echo prefix / log tag (string)
--   DiffGateArm          : 1 => arm /snapshotdiffgate (0 = plain resim run:
--                          fast-forward + quit-at-end only, gate untouched --
--                          the uncontended wall-time / DESYNC gate)
--   DiffGateFastForward  : 1 => /setspeed 20 + /speedcontrol 0
--   DiffGateQuitFrame    : hard quit at this frame (0 = quit on game over / demo end)
--   DiffGateExercise     : 1 => call the redirected positions/status-family
--                          callouts over all units each DRAW-CALLIN frame. PR 18:
--                          the loop runs from Draw* callins (DrawGenesis +
--                          DrawWorld), not widget:Update -- only Draw* callins
--                          set the draw-context flag the redirect branches on,
--                          so Update-loop calls would exercise the live path
--                          only (and headless never reaches DrawWorld).
--   DiffGatePovFrame     : frame to drop full-view and spectate a single team's
--                          POV (0 = never). Exercises the non-fullRead masking
--                          path (errorVectors, LOS gates) end-to-end through the
--                          armed dual-run comparator. Default 6000.
--   DiffGatePovSpan      : frames to stay in team POV before restoring full
--                          view (default 6000).
--   DiffGatePovTeam      : team to spectate (-1 = auto: team of the first unit
--                          that is not Gaia's at flip time).
--   DiffGateCtrlPokes    : 1 => exercise the PR-27a boundary-queued
--                          LuaUnsyncedCtrl sim pokes (SetUnitNoDraw family)
--                          from a Draw* callin every ~90 draw frames and
--                          verify the one-frame-deferred read-back next
--                          frame. Pokes are toggled back immediately so the
--                          scene stays watchable. Logs
--                          "[DiffGateDriver] ctrl-poke ..." ok/FAIL lines.
--                          Default 0 (headless replays never hit this path).
--
-- Pass criterion: the engine's [SnapshotDiffGate] report prints
--   "PASS (0 mismatches)" and total mismatches=0 over the whole replay.

local label, armGate, fastForward, quitFrame, exercise, ctrlPokes
local povFrame, povSpan, povTeam
local povActive = false
local povDone = false
local armed = false
local finished = false
local gameOverFrame = nil
local lastAdvanceTime = nil

local function announce(msg)
	Spring.Echo("[DiffGateDriver] " .. msg)
end

local function dumpAndQuit(reason)
	if finished then
		return
	end
	finished = true
	announce("finishing (" .. reason .. ") at frame " .. Spring.GetGameFrame())
	if armGate ~= 0 then
		-- print the running totals, then let engine teardown FlushPartial()
		-- print the final report as well
		Spring.SendCommands("snapshotdiffgate dump")
		Spring.SendCommands("snapshotdiffgate disarm")
	end
	Spring.SendCommands("quitforce")
end

function widget:Initialize()
	if not Spring.IsReplay() then
		announce("not a replay - driver disabled")
		widgetHandler:RemoveWidget(self)
		return
	end

	label       = Spring.GetConfigString("DiffGateLabel", "diffgate")
	armGate     = Spring.GetConfigInt("DiffGateArm", 1)
	fastForward = Spring.GetConfigInt("DiffGateFastForward", 1)
	quitFrame   = Spring.GetConfigInt("DiffGateQuitFrame", 0)
	exercise    = Spring.GetConfigInt("DiffGateExercise", 1)
	povFrame    = Spring.GetConfigInt("DiffGatePovFrame", 6000)
	povSpan     = Spring.GetConfigInt("DiffGatePovSpan", 6000)
	povTeam     = Spring.GetConfigInt("DiffGatePovTeam", -1)
	ctrlPokes   = Spring.GetConfigInt("DiffGateCtrlPokes", 0)

	announce(("label=%s ff=%d quit=%d exercise=%d povFrame=%d povSpan=%d povTeam=%d")
		:format(label, fastForward, quitFrame, exercise, povFrame, povSpan, povTeam))

	if fastForward ~= 0 then
		Spring.SendCommands("setspeed 20")
		Spring.SendCommands("speedcontrol 0")
	end
end

local function PickPovTeam()
	if povTeam >= 0 then
		return povTeam
	end
	local gaiaTeam = Spring.GetGaiaTeamID()
	local units = Spring.GetAllUnits()
	for i = 1, #units do
		local t = Spring.GetUnitTeam(units[i])
		if t ~= nil and t ~= gaiaTeam then
			return t
		end
	end
	return nil
end

function widget:GameFrame(f)
	lastAdvanceTime = os.clock()

	-- DiffGateForce1x=1 (env DG_FORCE_1X) pins the demo to 1x even when the
	-- recorded stream carries a setspeed (both gate replays were recorded
	-- fast-forwarded at 20x, so any "1x" visual/timing observation without
	-- this was silently FF'd -- the PR-46 lesson). Re-pinned periodically in
	-- case the stream re-issues a speed change.
	if Spring.GetConfigInt("DiffGateForce1x", 0) ~= 0 and (f % 150) == 30 then
		Spring.SendCommands("setspeed 1")
	end

	if not armed and f >= 1 then
		armed = true
		if armGate ~= 0 then
			Spring.SendCommands("snapshotdiffgate arm")
			announce("armed /snapshotdiffgate at frame " .. f)
		else
			announce("plain resim run (gate not armed) from frame " .. f)
		end
	end

	-- POV segment: drop full view and spectate one team so the redirected
	-- callouts run at a non-fullRead POV (real errorVector/LOS masking) for a
	-- stretch of the replay; restore full view afterwards
	if not povDone and not povActive and povFrame > 0 and f >= povFrame then
		local t = PickPovTeam()
		if t ~= nil then
			povActive = true
			Spring.SendCommands("specfullview 0")
			Spring.SendCommands("specteam " .. t)
			announce(("POV segment: spectating team %d (no fullview) at frame %d"):format(t, f))
		else
			povDone = true
		end
	end
	if povActive and f >= povFrame + povSpan then
		povActive = false
		povDone = true
		Spring.SendCommands("specfullview 3")
		announce("POV segment done: restored fullview at frame " .. f)
	end

	-- let the sim run a short tail past game over so trailing events settle
	if gameOverFrame ~= nil and f >= gameOverFrame + 90 then
		dumpAndQuit("game over")
	end

	if quitFrame > 0 and f >= quitFrame then
		dumpAndQuit("quit frame reached")
	end
end

-- draw-context load generator: hit the redirected positions/status callout
-- family over every unit the local client can see, from a real Draw* callin
-- (widget:Update does NOT set the draw-callin context flag, so the redirect
-- would not fire there). With the gate armed, every one of these calls
-- dual-runs the live body and the snapshot twin and bit-compares the returns
-- (LuaSnapshotServe::Route); CheckBoundary independently sweeps the fields and
-- the per-POV masking math each boundary.
local announced = {}
local function AnnounceOnce(tag)
	if not announced[tag] then
		announced[tag] = true
		announce("exercise loop active (" .. tag .. ")")
	end
end

local mapSizeX = Game.mapSizeX
local mapSizeZ = Game.mapSizeZ

-- PR 27a: the row-backed tail families are wide (~35 unit + ~18 feature
-- callouts), so they are exercised on a rotating 1-in-8 sample per draw frame
-- (full coverage every 8 frames) instead of every object every frame -- the
-- armed dual-run doubles every call, and full-cross would dominate the run
local exerciseTick = 0
local TAIL_STRIDE = 8

local function ExerciseUnitTail(uid, prevUid, fid)
	Spring.ValidUnitID(uid)
	Spring.GetUnitDefID(uid)
	Spring.GetUnitTeam(uid)
	Spring.GetUnitAllyTeam(uid)
	Spring.GetUnitNeutral(uid)
	Spring.GetUnitIsDead(uid)
	Spring.GetUnitIsBeingBuilt(uid)
	Spring.GetUnitVelocity(uid)
	Spring.GetUnitDirection(uid)
	Spring.GetUnitHeading(uid)
	Spring.GetUnitHeading(uid, true)
	Spring.GetUnitVectors(uid)
	Spring.GetUnitRadius(uid)
	Spring.GetUnitHeight(uid)
	Spring.GetUnitMass(uid)
	Spring.GetUnitExperience(uid)
	Spring.GetUnitIsActive(uid)
	Spring.GetUnitIsCloaked(uid)
	Spring.GetUnitMaxRange(uid)
	Spring.GetUnitBuildFacing(uid)
	Spring.GetUnitSensorRadius(uid, "los")
	Spring.GetUnitSensorRadius(uid, "radar")
	Spring.GetUnitSensorRadius(uid, "sonar")
	Spring.GetUnitSeismicSignature(uid)
	Spring.GetUnitSelfDTime(uid)
	Spring.GetUnitArmored(uid)
	Spring.GetUnitResources(uid)
	Spring.GetUnitHarvestStorage(uid)
	Spring.GetUnitCosts(uid)
	Spring.GetUnitCostTable(uid)
	Spring.GetUnitMoveDefID(uid)
	Spring.GetUnitBlocking(uid)
	Spring.GetUnitLeavesGhost(uid)
	Spring.IsUnitInRadar(uid)
	Spring.IsUnitAllied(uid)
	if prevUid ~= nil then
		Spring.GetUnitSeparation(uid, prevUid)
		Spring.GetUnitSeparation(uid, prevUid, true)
	end
	if fid ~= nil then
		Spring.GetUnitFeatureSeparation(uid, fid)
	end
end

local function ExerciseFeatureTail(fid, prevFid)
	Spring.ValidFeatureID(fid)
	Spring.GetFeatureDefID(fid)
	Spring.GetFeatureTeam(fid)
	Spring.GetFeatureAllyTeam(fid)
	Spring.GetFeatureHealth(fid)
	Spring.GetFeatureHeight(fid)
	Spring.GetFeatureRadius(fid)
	Spring.GetFeaturePosition(fid, true, true)
	Spring.GetFeatureMass(fid)
	Spring.GetFeatureDirection(fid)
	Spring.GetFeatureVelocity(fid)
	Spring.GetFeatureHeading(fid)
	Spring.GetFeatureResources(fid)
	Spring.GetFeatureBlocking(fid)
	Spring.GetFeatureNoSelect(fid)
	Spring.GetFeatureResurrect(fid)
	if prevFid ~= nil then
		Spring.GetFeatureSeparation(fid, prevFid)
	end
end

local function ExerciseGlobals()
	Spring.GetGameFrame()
	Spring.GetGameSeconds()
	Spring.GetGameSecondsInterpolated()
	Spring.GetGameSpeed()
	Spring.GetWind()
	Spring.IsCheatingEnabled()
	Spring.IsGodModeEnabled()
	Spring.IsEditDefsEnabled()
	Spring.AreHelperAIsEnabled()
	Spring.IsNoCostEnabled()
	Spring.IsGameOver()
	Spring.GetGroundExtremes()
	Spring.GetGlobalLos()
	local allyTeams = Spring.GetAllyTeamList()
	for i = 1, #allyTeams do
		Spring.GetGlobalLos(allyTeams[i])
	end
end

local function ExerciseFamily()
	exerciseTick = (exerciseTick + 1) % TAIL_STRIDE

	ExerciseGlobals()

	local units = Spring.GetAllUnits()
	local feats = Spring.GetAllFeatures()
	for i = 1, #units do
		local uid = units[i]
		Spring.GetUnitPosition(uid, true, true)
		Spring.GetUnitBasePosition(uid)
		Spring.GetUnitHealth(uid)
		Spring.GetUnitIsStunned(uid)
		Spring.GetUnitViewPosition(uid)
		Spring.GetUnitViewPosition(uid, true)
		Spring.IsUnitVisible(uid)
		Spring.IsUnitIcon(uid)
		Spring.GetUnitLosState(uid)
		Spring.GetUnitLosState(uid, nil, true)
		-- PR 38c: per-unit rules-params (POV/losMask-masked); whole table + miss lookup
		Spring.GetUnitRulesParams(uid)
		Spring.GetUnitRulesParam(uid, "__diffgate_probe_missing__")
	end
	-- PR 38c: per-feature rules-params (POV/losMask-masked); whole table + miss lookup
	for i = 1, #feats do
		local fid = feats[i]
		Spring.GetFeatureRulesParams(fid)
		Spring.GetFeatureRulesParam(fid, "__diffgate_probe_missing__")
	end
	for i = 1 + exerciseTick, #units, TAIL_STRIDE do
		ExerciseUnitTail(units[i], units[i > 1 and (i - 1) or #units], feats[1])
	end
	for i = 1 + exerciseTick, #feats, TAIL_STRIDE do
		ExerciseFeatureTail(feats[i], feats[i > 1 and (i - 1) or #feats])
	end

	-- projectile family (second family): headless projectile-visual widgets
	-- self-disable, so the driver must generate this surface itself
	local projs = Spring.GetProjectilesInRectangle(0, 0, mapSizeX, mapSizeZ)
	for i = 1, #projs do
		local pid = projs[i]
		Spring.GetProjectilePosition(pid)
		Spring.GetProjectileVelocity(pid)
		Spring.GetProjectileDefID(pid)
		Spring.GetProjectileTarget(pid)
		Spring.GetProjectileOwnerID(pid)
		-- PR 27a projectile tail
		Spring.GetProjectileDirection(pid)
		Spring.GetProjectileGravity(pid)
		Spring.GetProjectileTeamID(pid)
		Spring.GetProjectileAllyTeamID(pid)
		Spring.GetProjectileType(pid)
		Spring.GetProjectileTimeToLive(pid)
		Spring.GetProjectileIsIntercepted(pid)
	end

	-- team/player-table family (PR 26): served from the TeamRows/PlayerRows
	-- boundary copy; ids come from the (also redirected) list callouts, which
	-- match live by construction
	Spring.GetGaiaTeamID()
	local allyTeams = Spring.GetAllyTeamList()
	for i = 1, #allyTeams do
		Spring.GetTeamList(allyTeams[i])
	end
	-- PR 38: game rules-params (singleton; no POV). GetGameRulesParams returns the
	-- whole table (twin push vs live push), GetGameRulesParam a single lookup.
	Spring.GetGameRulesParams()
	Spring.GetGameRulesParam("__diffgate_probe_missing__") -- nil both sides (miss)
	local teams = Spring.GetTeamList()
	for i = 1, #teams do
		local tid = teams[i]
		Spring.GetTeamInfo(tid)
		Spring.GetTeamInfo(tid, false)
		Spring.GetTeamAllyTeamID(tid)
		Spring.GetTeamResources(tid, "metal")
		Spring.GetTeamResources(tid, "energy")
		Spring.GetTeamUnitStats(tid)
		Spring.GetTeamResourceStats(tid, "metal")
		Spring.GetTeamResourceStats(tid, "energy")
		Spring.GetTeamDamageStats(tid)
		Spring.GetTeamUnitCount(tid)
		Spring.GetTeamColor(tid)
		Spring.GetTeamOrigColor(tid)
		-- PR 38: per-team rules-params (POV-masked); whole table + single lookup
		Spring.GetTeamRulesParams(tid)
		Spring.GetTeamRulesParam(tid, "__diffgate_probe_missing__")
		Spring.GetPlayerList(tid)
		Spring.GetPlayerList(tid, true)
	end
	local players = Spring.GetPlayerList()
	for i = 1, #players do
		Spring.GetPlayerInfo(players[i])
		Spring.GetPlayerInfo(players[i], false)
		-- PR 38c: per-player rules-params (synced/own-player/fullRead-masked)
		Spring.GetPlayerRulesParams(players[i])
		Spring.GetPlayerRulesParam(players[i], "__diffgate_probe_missing__")
	end
end

-- PR 27a ctrl-poke exercise: under SplitDrawContract the last-wins ctrl
-- setters queue to the next SimDrawBarrier instead of applying in place. The
-- probe is SetTeamColor: it has NO control-permission gate (the SetUnit*
-- family silently no-ops for spectators at ParseCtrlUnit, master behavior)
-- and GetTeamColor is snapshot-served, so one round-trip verifies the whole
-- chain: queue -> barrier drain (before the snapshot publish) -> boundary
-- copy -> serving twin. A poke made during draw frame N must NOT read back
-- at frame N (the deferral itself) and MUST read back at frame N+1. The
-- color is restored one frame later so the scene stays watchable. The
-- permission-gated SetUnit*/SetFeature* setters are still called each round
-- (they queue when the handle has ctrl rights, e.g. /godmode; inventory
-- counters show which path ran).
local pokeState = nil
local pokeCount, pokeFails = 0, 0

local function TeamColorRed(teamID)
	local r = Spring.GetTeamColor(teamID)
	return r
end

local function CtrlPokeCheck(drawFrameTag)
	if pokeState ~= nil then
		-- verify last frame's poke applied at the barrier we just crossed
		local r = TeamColorRed(pokeState.teamID)
		pokeCount = pokeCount + 1
		if r == nil or math.abs(r - pokeState.wantRed) > (1.5 / 255.0) then
			pokeFails = pokeFails + 1
			announce(("ctrl-poke FAIL at drawtag %d: teamColor red=%s want=%.4f")
				:format(drawFrameTag, tostring(r), pokeState.wantRed))
		end
		-- restore (also boundary-queued)
		Spring.SetTeamColor(pokeState.teamID, pokeState.origR, pokeState.origG, pokeState.origB)
		pokeState = nil
		return
	end

	local teams = Spring.GetTeamList()
	if teams == nil or #teams == 0 then
		return
	end
	local teamID = teams[1]
	local origR, origG, origB = Spring.GetTeamColor(teamID)
	if origR == nil then
		return
	end

	-- a distinctive red, alternating so consecutive rounds cannot pass on a
	-- stale value; quantized to the engine's uint8 storage for the compare
	local wantRed = (pokeCount % 2 == 0) and 0.1230 or 0.8770
	Spring.SetTeamColor(teamID, wantRed, origG, origB)
	pokeState = { teamID = teamID, origR = origR, origG = origG, origB = origB,
	              wantRed = math.floor(wantRed * 255) / 255 }

	-- the deferral itself: the write must NOT be visible this frame (it queued)
	local sameFrame = TeamColorRed(teamID)
	if sameFrame ~= nil and math.abs(sameFrame - pokeState.wantRed) <= (0.5 / 255.0)
	   and math.abs(origR - pokeState.wantRed) > (2.0 / 255.0) then
		pokeCount = pokeCount + 1
		pokeFails = pokeFails + 1
		pokeState = nil
		announce("ctrl-poke FAIL: SetTeamColor visible same-frame (queue did not defer)")
		Spring.SetTeamColor(teamID, origR, origG, origB)
		return
	end

	-- the permission-gated family: exercised for coverage; queues only when
	-- the handle can control the unit (godmode), silently no-ops otherwise
	local units = Spring.GetAllUnits()
	if #units > 0 then
		local uid = units[1 + (pokeCount % #units)]
		Spring.SetUnitNoDraw(uid, false)
		Spring.SetUnitNoMinimap(uid, false)
		Spring.SetUnitAlwaysUpdateMatrix(uid, false)
	end
	Spring.SetNanoProjectileParams(1, 1, 0, 0, 0, 0)
end

local drawFrameCount = 0

-- DrawGenesis is the one Draw* callin that fires every draw frame even when
-- rendering is inactive (headless: CGame::Draw returns right after it when
-- !globalRendering->active), so it is the guaranteed-cadence exercise point
function widget:DrawGenesis()
	if not armed or finished then
		return
	end
	drawFrameCount = drawFrameCount + 1
	if ctrlPokes ~= 0 and (pokeState ~= nil or drawFrameCount % 90 == 0) then
		AnnounceOnce("ctrl-pokes")
		CtrlPokeCheck(drawFrameCount)
	end
	if exercise == 0 then
		return
	end
	AnnounceOnce("DrawGenesis")
	ExerciseFamily()
end

-- also exercise from the world pass when it runs (live-GL runs; harmless
-- double coverage, and it is the context real widgets call the family from)
function widget:DrawWorld()
	if not armed or finished or exercise == 0 then
		return
	end
	AnnounceOnce("DrawWorld")
	ExerciseFamily()
end

function widget:Update()
	-- demo-end watchdog: if the sim stops advancing for 15 wall-clock seconds
	-- (demo stream exhausted without a GameOver), finish
	if armed and not finished and lastAdvanceTime ~= nil and (os.clock() - lastAdvanceTime) > 15 then
		dumpAndQuit("no sim advance for 15s (demo end?)")
	end
end

function widget:GameOver()
	if gameOverFrame == nil then
		gameOverFrame = Spring.GetGameFrame()
		announce("game over at frame " .. gameOverFrame)
	end
end

function widget:Shutdown()
	if ctrlPokes ~= nil and ctrlPokes ~= 0 then
		announce(("ctrl-poke checks: %d rounds, %d FAILED"):format(pokeCount, pokeFails))
	end
end
