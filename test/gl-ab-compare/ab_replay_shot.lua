function widget:GetInfo()
	return {
		name      = "AB Replay Shot",
		desc      = "Fast-forwards a replay to a busy frame, screenshots it, and quits -- so a replay can be driven unattended like the startscript contents are.",
		author    = "test harness",
		date      = "2026",
		layer     = 0,
		enabled   = true,
	}
end

-- Replays are locked to 1x playback, and a capture 20s into a game is a capture
-- of two commanders walking. speedcontrol 0 + setspeed lifts that (see the
-- headless speed-control note); everything below is in SIM frames so it does not
-- depend on how fast the fast-forward actually goes.
local enabled   = (Spring.GetConfigInt("ABReplayShot", 0) == 1)
local shotFrame = Spring.GetConfigInt("ABReplayShotFrame", 9000)
local quitFrame = Spring.GetConfigInt("ABReplayQuitFrame", 0) -- 0 = never quit
local shot = false
local slowed = false

-- Deliberately NOT widgetHandler:RemoveWidget() when the knob is off. BAR
-- PERSISTS a self-removal as order=0 in LuaUI/Config/BYAR.lua -- permanently
-- disabled, knob or no knob -- so one run with the knob off silently disables the
-- widget for every run after. Going inert costs a dead callin and cannot do that.
local active = false

function widget:Initialize()
	active = enabled and Spring.IsReplay()

	if not active then
		return
	end

	Spring.SendCommands("speedcontrol 0", "setspeed 20")
	Spring.Echo(("[AB Replay Shot] active, shot at frame %d"):format(shotFrame))
end

function widget:GameFrame(n)
	if not active then
		return
	end

	-- Slow down first and shoot LATER. Screenshotting in the same breath as
	-- setspeed captures a frame the renderer was still skipping through, which
	-- comes out black.
	if not slowed and n >= shotFrame then
		slowed = true
		Spring.SendCommands("setspeed 1")
	end

	if not shot and n >= shotFrame + 60 then
		shot = true
		Spring.SendCommands("screenshot")
		Spring.Echo(("[AB Replay Shot] screenshot at frame %d"):format(n))
	end

	if quitFrame > 0 and n >= quitFrame then
		Spring.SendCommands("quitforce")
	end
end
