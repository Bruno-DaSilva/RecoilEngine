function widget:GetInfo()
	return {
		name      = "AB Gate Replay Driver",
		desc      = "Drives a replay for the whole-frame A/B compare gate: playback speed, camera waypoints (water coverage), PiP open, timed quit.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 100000,
		enabled   = true,
	}
end

-- Config knobs (engine config, so the runner can parameterize without editing this file):
--   ABDriverSpeed     : /setspeed value after start          (default 10)
--   ABDriverQuitFrame : sim frame at which to /quitforce     (default 30000)
--   ABDriverCamPeriod : seconds between camera waypoints     (default 15)
--   ABDriverPip       : 1 => enable the Picture-in-Picture widget (default 1)

local speedTarget, quitFrame, camPeriod, wantPip
local started = false
local pipDone = false
local timeAcc = 0
local wpIndex = 0

-- Fractions of map size; cycles through mid-map and flank positions so large
-- water bodies (e.g. Supreme Isthmus sea flanks) enter the view regardless of
-- map orientation.
local waypoints = {
	{ 0.50, 0.50 },
	{ 0.30, 0.50 },
	{ 0.70, 0.50 },
	{ 0.50, 0.30 },
	{ 0.50, 0.70 },
	{ 0.25, 0.25 },
	{ 0.75, 0.75 },
}

local function announce(msg)
	Spring.Echo("[ABDriver] " .. msg)
end

function widget:Initialize()
	if not Spring.IsReplay() then
		announce("not a replay - driver disabled")
		widgetHandler:RemoveWidget(self)
		return
	end
	speedTarget = Spring.GetConfigInt("ABDriverSpeed", 10)
	quitFrame   = Spring.GetConfigInt("ABDriverQuitFrame", 30000)
	camPeriod   = Spring.GetConfigInt("ABDriverCamPeriod", 15)
	wantPip     = Spring.GetConfigInt("ABDriverPip", 1) == 1
	announce(("active: speed=%d quitFrame=%d camPeriod=%ds pip=%s"):format(
		speedTarget, quitFrame, camPeriod, tostring(wantPip)))
end

function widget:GameFrame(f)
	if not started and f >= 30 then
		started = true
		Spring.SendCommands("setspeed " .. speedTarget, "speedcontrol 0")
		announce("playback speed set to " .. speedTarget)
	end
	if started and wantPip and not pipDone and f >= 90 then
		pipDone = true
		local ok
		if widgetHandler.EnableWidgetRaw then
			ok = widgetHandler:EnableWidgetRaw("Picture-in-Picture")
		elseif widgetHandler.EnableWidget then
			ok = widgetHandler:EnableWidget("Picture-in-Picture")
		end
		announce("EnableWidget(Picture-in-Picture) => " .. tostring(ok))
	end
	if f >= quitFrame then
		announce("quit frame " .. f .. " reached - quitforce")
		Spring.SendCommands("quitforce")
	end
end

function widget:Update(dt)
	if not started then
		return
	end
	timeAcc = timeAcc + dt
	if timeAcc >= camPeriod then
		timeAcc = 0
		wpIndex = (wpIndex % #waypoints) + 1
		local wp = waypoints[wpIndex]
		local x = wp[1] * Game.mapSizeX
		local z = wp[2] * Game.mapSizeZ
		local y = Spring.GetGroundHeight(x, z)
		Spring.SetCameraTarget(x, y, z, 1)
		announce(("camera -> waypoint %d (%.0f, %.0f)"):format(wpIndex, x, z))
	end
end
