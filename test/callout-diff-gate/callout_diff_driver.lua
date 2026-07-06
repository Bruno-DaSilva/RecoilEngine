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
--
-- Pass criterion: the engine's [SnapshotDiffGate] report prints
--   "PASS (0 mismatches)" and total mismatches=0 over the whole replay.

local label, armGate, fastForward, quitFrame, exercise
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
		Spring.GetPlayerList(tid)
		Spring.GetPlayerList(tid, true)
	end
	local players = Spring.GetPlayerList()
	for i = 1, #players do
		Spring.GetPlayerInfo(players[i])
		Spring.GetPlayerInfo(players[i], false)
	end
end

-- DrawGenesis is the one Draw* callin that fires every draw frame even when
-- rendering is inactive (headless: CGame::Draw returns right after it when
-- !globalRendering->active), so it is the guaranteed-cadence exercise point
function widget:DrawGenesis()
	if not armed or finished or exercise == 0 then
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
