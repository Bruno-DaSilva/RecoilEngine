function widget:GetInfo()
	return {
		name      = "Lua Section Probe",
		desc      = "PR 47 measurement instrument: replicates the diff-gate exercise loop section by section over all units for a window of draw frames, attributing LuaUI allocation churn (numLuaAllocs delta) per section.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 999,
		enabled   = true,
	}
end

local START_FRAME = 3000
local FRAMES = 200 -- draw frames to accumulate

local sections = {
	"posfam", "rulesparams", "unittail", "featrp", "feattail", "projs", "teams", "globals",
}
local acc = {}
for _, s in ipairs(sections) do acc[s] = 0 end
local frames = 0
local reported = false

local function kallocs()
	local _, a = Spring.GetLuaMemUsage()
	return a
end

local exerciseTick = 0
local TAIL_STRIDE = 8

local function UnitTail(uid)
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
end

local function FeatTail(fid)
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
end

local mapSizeX = Game.mapSizeX
local mapSizeZ = Game.mapSizeZ

local function RunSections()
	exerciseTick = (exerciseTick + 1) % TAIL_STRIDE
	local units = Spring.GetAllUnits()
	local feats = Spring.GetAllFeatures()

	local t = kallocs()
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
	local t2 = kallocs(); acc.posfam = acc.posfam + (t2 - t); t = t2
	for i = 1, #units do
		local uid = units[i]
		Spring.GetUnitRulesParams(uid)
		Spring.GetUnitRulesParam(uid, "__section_probe_missing__")
	end
	t2 = kallocs(); acc.rulesparams = acc.rulesparams + (t2 - t); t = t2
	for i = 1 + exerciseTick, #units, TAIL_STRIDE do
		UnitTail(units[i])
	end
	t2 = kallocs(); acc.unittail = acc.unittail + (t2 - t); t = t2
	for i = 1, #feats do
		Spring.GetFeatureRulesParams(feats[i])
		Spring.GetFeatureRulesParam(feats[i], "__section_probe_missing__")
	end
	t2 = kallocs(); acc.featrp = acc.featrp + (t2 - t); t = t2
	for i = 1 + exerciseTick, #feats, TAIL_STRIDE do
		FeatTail(feats[i])
	end
	t2 = kallocs(); acc.feattail = acc.feattail + (t2 - t); t = t2
	local projs = Spring.GetProjectilesInRectangle(0, 0, mapSizeX, mapSizeZ)
	for i = 1, #projs do
		local pid = projs[i]
		Spring.GetProjectilePosition(pid)
		Spring.GetProjectileVelocity(pid)
		Spring.GetProjectileDefID(pid)
		Spring.GetProjectileTarget(pid)
		Spring.GetProjectileOwnerID(pid)
		Spring.GetProjectileDirection(pid)
		Spring.GetProjectileGravity(pid)
		Spring.GetProjectileTeamID(pid)
		Spring.GetProjectileAllyTeamID(pid)
		Spring.GetProjectileType(pid)
		Spring.GetProjectileTimeToLive(pid)
		Spring.GetProjectileIsIntercepted(pid)
	end
	t2 = kallocs(); acc.projs = acc.projs + (t2 - t); t = t2
	local teams = Spring.GetTeamList()
	for i = 1, #teams do
		local tid = teams[i]
		Spring.GetTeamInfo(tid)
		Spring.GetTeamResources(tid, "metal")
		Spring.GetTeamResources(tid, "energy")
		Spring.GetTeamUnitStats(tid)
		Spring.GetTeamResourceStats(tid, "metal")
		Spring.GetTeamResourceStats(tid, "energy")
		Spring.GetTeamDamageStats(tid)
		Spring.GetTeamUnitCount(tid)
		Spring.GetTeamColor(tid)
		Spring.GetTeamRulesParams(tid)
		Spring.GetTeamRulesParam(tid, "__section_probe_missing__")
		Spring.GetPlayerList(tid)
	end
	local players = Spring.GetPlayerList()
	for i = 1, #players do
		Spring.GetPlayerInfo(players[i])
		Spring.GetPlayerRulesParams(players[i])
	end
	t2 = kallocs(); acc.teams = acc.teams + (t2 - t); t = t2
	Spring.GetGameFrame()
	Spring.GetGameSeconds()
	Spring.GetGameSecondsInterpolated()
	Spring.GetGameSpeed()
	Spring.GetWind()
	Spring.IsCheatingEnabled()
	Spring.IsGodModeEnabled()
	Spring.GetGameRulesParams()
	Spring.GetGameRulesParam("__section_probe_missing__")
	Spring.GetGroundExtremes()
	Spring.GetGlobalLos()
	t2 = kallocs(); acc.globals = acc.globals + (t2 - t)
end

function widget:DrawGenesis()
	if reported then
		return
	end
	local f = Spring.GetGameFrame()
	if f < START_FRAME then
		return
	end
	if frames < FRAMES then
		frames = frames + 1
		RunSections()
		return
	end
	reported = true
	for _, s in ipairs(sections) do
		Spring.Echo(string.format("[LuaSectionProbe] %-12s %10.1f kallocs over %d draw frames", s, acc[s], frames))
	end
	Spring.Echo("[LuaSectionProbe] done")
end
