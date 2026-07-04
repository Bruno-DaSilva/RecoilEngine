function widget:GetInfo()
	return {
		name      = "AB Gate Idle Driver",
		desc      = "Drives an idle base-building start for the whole-frame A/B compare gate: selects the commander (buildmenu/gridmenu visible), issues a few build orders.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 100000,
		enabled   = true,
	}
end

-- Active only when config ABIdleDriver=1 and not a replay; inert otherwise.

local myTeam
local comID
local ordersGiven = 0
local camPeriod, timeAcc, wpIndex = 0, 0, 0

-- fractions of map size; mid-map + flanks so water bodies enter the view on
-- water-flanked maps (Supreme Isthmus)
local waypoints = {
	{ 0.50, 0.50 },
	{ 0.30, 0.50 },
	{ 0.70, 0.50 },
	{ 0.50, 0.30 },
	{ 0.50, 0.70 },
}

local function announce(msg)
	Spring.Echo("[ABIdleDriver] " .. msg)
end

function widget:Initialize()
	if Spring.IsReplay() or Spring.GetConfigInt("ABIdleDriver", 0) ~= 1 then
		widgetHandler:RemoveWidget(self)
		return
	end
	myTeam = Spring.GetMyTeamID()
	camPeriod = Spring.GetConfigInt("ABDriverCamPeriod", 0) -- 0 = no camera cycling
	announce("active (team " .. myTeam .. ", camPeriod " .. camPeriod .. ")")
end

function widget:Update(dt)
	if camPeriod <= 0 or Spring.GetGameFrame() < 60 then
		return
	end
	timeAcc = timeAcc + dt
	if timeAcc >= camPeriod then
		timeAcc = 0
		wpIndex = (wpIndex % #waypoints) + 1
		local wp = waypoints[wpIndex]
		local x = wp[1] * Game.mapSizeX
		local z = wp[2] * Game.mapSizeZ
		Spring.SetCameraTarget(x, Spring.GetGroundHeight(x, z), z, 1)
		announce(("camera -> waypoint %d (%.0f, %.0f)"):format(wpIndex, x, z))
	end
end

local function findCommander()
	for _, uid in ipairs(Spring.GetTeamUnits(myTeam)) do
		local udid = Spring.GetUnitDefID(uid)
		if udid and UnitDefs[udid] and UnitDefs[udid].customParams and UnitDefs[udid].customParams.iscommander then
			return uid
		end
	end
end

function widget:GameFrame(f)
	if f == 60 then
		comID = findCommander()
		if comID then
			Spring.SelectUnitArray({ comID })
			announce("selected commander " .. comID .. " (buildmenu visible)")
		else
			announce("no commander found")
		end
	end
	-- keep it selected so the buildmenu stays up, and queue a few buildings for
	-- build-queue ghost/nanoframe rendering
	if comID and f > 60 and f % 300 == 0 and ordersGiven < 4 then
		Spring.SelectUnitArray({ comID })
		local x, y, z = Spring.GetUnitPosition(comID)
		if x then
			local mexDef = UnitDefNames["armmex"] and UnitDefNames["armmex"].id
			local solarDef = UnitDefNames["armsolar"] and UnitDefNames["armsolar"].id
			local defID = (ordersGiven % 2 == 0) and solarDef or mexDef
			if defID then
				local bx = x + 120 + 80 * ordersGiven
				local bz = z + 120
				local by = Spring.GetGroundHeight(bx, bz)
				Spring.GiveOrder(-defID, { bx, by, bz, 0 }, { "shift" })
				ordersGiven = ordersGiven + 1
				announce("queued build order " .. ordersGiven)
			end
		end
	end
end
