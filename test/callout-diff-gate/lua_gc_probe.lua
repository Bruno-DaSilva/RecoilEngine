function widget:GetInfo()
	return {
		name      = "Lua GC Probe",
		desc      = "PR 47 measurement instrument: per-callout garbage-bytes probe. At a fixed frame, stops the GC, calls each callout N times on a live unit, and echoes the heap delta per callout.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 999,
		enabled   = true,
	}
end

local PROBE_FRAME = 3000
local N = 10000

local done = false

-- churn metric: the engine's per-handle numLuaAllocs counter (2nd return of
-- GetLuaMemUsage, in kilo-allocs) is monotonic gross allocation count --
-- unaffected by GC, no risk of tripping the 1.5GB valve on big-table callouts
local function probeOne(name, fn, n)
	n = n or N
	local _, before = Spring.GetLuaMemUsage()
	for i = 1, n do
		fn()
	end
	local _, after = Spring.GetLuaMemUsage()
	-- allocations per call
	return (after - before) * 1000.0 / n
end

local function runProbe()
	local units = Spring.GetAllUnits()
	if (not units) or (#units < 2) then
		Spring.Echo("[LuaGCProbe] no units, skipping")
		return
	end
	local uid = units[1]
	local uid2 = units[2]
	local features = Spring.GetAllFeatures()
	local fid = features and features[1] or nil

	local teams = Spring.GetTeamList()
	local tid = teams[1]
	local players = Spring.GetPlayerList()
	local pid = players and players[1] or nil
	local msx, msz = Game.mapSizeX, Game.mapSizeZ

	local probes = {
		-- headful-only / GL4-widget-path suspects
		{ "GetVisibleUnits",      function() Spring.GetVisibleUnits() end, 200 },
		{ "GetVisibleUnitsAll",   function() Spring.GetVisibleUnits(-1, nil, false) end, 200 },
		{ "GetVisibleFeatures",   function() Spring.GetVisibleFeatures() end, 200 },
		{ "GetVisibleProjectiles",function() if Spring.GetVisibleProjectiles then Spring.GetVisibleProjectiles() end end, 200 },
		{ "GetSelectedUnits",     function() Spring.GetSelectedUnits() end, 2000 },
		{ "GetCameraState",       function() Spring.GetCameraState() end, 2000 },
		{ "GetCameraPosition",    function() Spring.GetCameraPosition() end, 2000 },
		{ "GetUnitPieceMap",      function() Spring.GetUnitPieceMap(uid) end, 2000 },
		{ "GetUnitPieceList",     function() Spring.GetUnitPieceList(uid) end, 2000 },
		{ "GetUnitPieceMatrix",   function() Spring.GetUnitPieceMatrix(uid, 1) end },
		{ "GetUnitPiecePosDir",   function() Spring.GetUnitPiecePosDir(uid, 1) end },
		{ "GetUnitPiecePosition", function() Spring.GetUnitPiecePosition(uid, 1) end },
		{ "GetUnitTransformMatrix",function() Spring.GetUnitTransformMatrix(uid) end },
		{ "WorldToScreenCoords",  function() Spring.WorldToScreenCoords(1000, 100, 1000) end },
		{ "GetGroundHeight",      function() Spring.GetGroundHeight(1000, 1000) end },
		{ "GetGroundNormal",      function() Spring.GetGroundNormal(1000, 1000) end },
		{ "GetGroundInfo",        function() Spring.GetGroundInfo(1000, 1000) end },
		{ "GetMouseState",        function() Spring.GetMouseState() end, 2000 },
		{ "GetUnitWeaponState",   function() Spring.GetUnitWeaponState(uid, 1) end },
		{ "GetUnitWeaponVectors", function() Spring.GetUnitWeaponVectors(uid, 1) end },
		{ "GetUnitShieldState",   function() Spring.GetUnitShieldState(uid) end },
		{ "GetUnitEstimatedPath", function() Spring.GetUnitEstimatedPath(uid) end, 2000 },
		{ "GetUnitLastAttacker",  function() Spring.GetUnitLastAttacker(uid) end },
		{ "GetUnitNearestEnemy",  function() Spring.GetUnitNearestEnemy(uid) end, 2000 },
		{ "GetAllUnits",          function() Spring.GetAllUnits() end, 200 },
		{ "GetAllFeatures",       function() Spring.GetAllFeatures() end, 200 },
		{ "GetUnitBasePosition",  function() Spring.GetUnitBasePosition(uid) end },
		{ "GetUnitIsStunned",     function() Spring.GetUnitIsStunned(uid) end },
		{ "GetUnitViewPosition",  function() Spring.GetUnitViewPosition(uid) end },
		{ "GetUnitViewPosition2", function() Spring.GetUnitViewPosition(uid, true) end },
		{ "IsUnitVisible",        function() Spring.IsUnitVisible(uid) end },
		{ "IsUnitIcon",           function() Spring.IsUnitIcon(uid) end },
		{ "GetUnitLosState",      function() Spring.GetUnitLosState(uid) end },
		{ "GetUnitLosStateRaw",   function() Spring.GetUnitLosState(uid, nil, true) end },
		{ "GetUnitRulesParams",   function() Spring.GetUnitRulesParams(uid) end },
		{ "GetUnitRulesParamMiss",function() Spring.GetUnitRulesParam(uid, "__gcprobe_missing__") end },
		{ "GetGameRulesParams",   function() Spring.GetGameRulesParams() end },
		{ "GetGameRulesParamMiss",function() Spring.GetGameRulesParam("__gcprobe_missing__") end },
		{ "GetTeamList",          function() Spring.GetTeamList() end },
		{ "GetTeamInfo",          function() Spring.GetTeamInfo(tid) end },
		{ "GetTeamResources",     function() Spring.GetTeamResources(tid, "metal") end },
		{ "GetTeamUnitStats",     function() Spring.GetTeamUnitStats(tid) end },
		{ "GetTeamResourceStats", function() Spring.GetTeamResourceStats(tid, "metal") end },
		{ "GetTeamDamageStats",   function() Spring.GetTeamDamageStats(tid) end },
		{ "GetTeamUnitCount",     function() Spring.GetTeamUnitCount(tid) end },
		{ "GetTeamColor",         function() Spring.GetTeamColor(tid) end },
		{ "GetTeamRulesParams",   function() Spring.GetTeamRulesParams(tid) end },
		{ "GetPlayerList",        function() Spring.GetPlayerList() end },
		{ "GetPlayerInfo",        pid and function() Spring.GetPlayerInfo(pid) end or function() end },
		{ "GetPlayerRulesParams", pid and function() Spring.GetPlayerRulesParams(pid) end or function() end },
		{ "GetAllyTeamList",      function() Spring.GetAllyTeamList() end },
		{ "GetGlobalLos",         function() Spring.GetGlobalLos() end },
		{ "GetWind",              function() Spring.GetWind() end },
		{ "GetGameSpeed",         function() Spring.GetGameSpeed() end },
		{ "GetGameSeconds",       function() Spring.GetGameSeconds() end },
		{ "GetGroundExtremes",    function() Spring.GetGroundExtremes() end },
		{ "GetProjsInRect",       function() Spring.GetProjectilesInRectangle(0, 0, msx, msz) end, 200 },
		{ "GetUnitPosition",      function() Spring.GetUnitPosition(uid) end },
		{ "GetUnitPosition(3)",   function() Spring.GetUnitPosition(uid, true, true) end },
		{ "ValidUnitID",          function() Spring.ValidUnitID(uid) end },
		{ "GetUnitDefID",         function() Spring.GetUnitDefID(uid) end },
		{ "GetUnitTeam",          function() Spring.GetUnitTeam(uid) end },
		{ "GetUnitAllyTeam",      function() Spring.GetUnitAllyTeam(uid) end },
		{ "GetUnitNeutral",       function() Spring.GetUnitNeutral(uid) end },
		{ "GetUnitIsDead",        function() Spring.GetUnitIsDead(uid) end },
		{ "GetUnitIsBeingBuilt",  function() Spring.GetUnitIsBeingBuilt(uid) end },
		{ "GetUnitHealth",        function() Spring.GetUnitHealth(uid) end },
		{ "GetUnitVelocity",      function() Spring.GetUnitVelocity(uid) end },
		{ "GetUnitDirection",     function() Spring.GetUnitDirection(uid) end },
		{ "GetUnitHeading",       function() Spring.GetUnitHeading(uid) end },
		{ "GetUnitVectors",       function() Spring.GetUnitVectors(uid) end },
		{ "GetUnitRadius",        function() Spring.GetUnitRadius(uid) end },
		{ "GetUnitMass",          function() Spring.GetUnitMass(uid) end },
		{ "GetUnitExperience",    function() Spring.GetUnitExperience(uid) end },
		{ "GetUnitIsActive",      function() Spring.GetUnitIsActive(uid) end },
		{ "GetUnitIsCloaked",     function() Spring.GetUnitIsCloaked(uid) end },
		{ "GetUnitMaxRange",      function() Spring.GetUnitMaxRange(uid) end },
		{ "GetUnitBuildFacing",   function() Spring.GetUnitBuildFacing(uid) end },
		{ "GetUnitSensorRadius",  function() Spring.GetUnitSensorRadius(uid, "los") end },
		{ "GetUnitSelfDTime",     function() Spring.GetUnitSelfDTime(uid) end },
		{ "GetUnitArmored",       function() Spring.GetUnitArmored(uid) end },
		{ "GetUnitResources",     function() Spring.GetUnitResources(uid) end },
		{ "GetUnitHarvestStorage",function() Spring.GetUnitHarvestStorage(uid) end },
		{ "GetUnitCosts",         function() Spring.GetUnitCosts(uid) end },
		{ "GetUnitCostTable",     function() Spring.GetUnitCostTable(uid) end },
		{ "GetUnitMoveDefID",     function() Spring.GetUnitMoveDefID(uid) end },
		{ "GetUnitBlocking",      function() Spring.GetUnitBlocking(uid) end },
		{ "GetUnitLeavesGhost",   function() Spring.GetUnitLeavesGhost(uid) end },
		{ "IsUnitInRadar",        function() Spring.IsUnitInRadar(uid) end },
		{ "IsUnitAllied",         function() Spring.IsUnitAllied(uid) end },
		{ "GetUnitSeparation",    function() Spring.GetUnitSeparation(uid, uid2) end },
		{ "GetUnitCommands0",     function() Spring.GetUnitCommands(uid, 0) end },
		{ "GetUnitCurrentCommand",function() Spring.GetUnitCurrentCommand(uid) end },
		{ "GetUnitStates",        function() Spring.GetUnitStates(uid) end },
		{ "noop",                 function() end },
	}
	if fid then
		probes[#probes + 1] = { "GetFeaturePosition", function() Spring.GetFeaturePosition(fid) end }
		probes[#probes + 1] = { "GetFeatureHealth",   function() Spring.GetFeatureHealth(fid) end }
		probes[#probes + 1] = { "GetFeatureResurrect",function() Spring.GetFeatureResurrect(fid) end }
	end

	Spring.Echo(string.format("[LuaGCProbe] probing %d callouts x %d calls, unit=%d", #probes, N, uid))
	for _, p in ipairs(probes) do
		local bpc = probeOne(p[1], p[2], p[3])
		Spring.Echo(string.format("[LuaGCProbe] %-24s %8.2f allocs/call", p[1], bpc))
	end
	Spring.Echo("[LuaGCProbe] done")
end

-- run from a Draw callin so the draw-context serving path is engaged (the
-- same path BAR widget draw-phase reads take); DrawGenesis runs headless
function widget:DrawGenesis()
	if done then
		return
	end
	local f = Spring.GetGameFrame()
	if f >= PROBE_FRAME then
		done = true
		runProbe()
	end
end
