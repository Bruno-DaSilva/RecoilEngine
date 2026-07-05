function widget:GetInfo()
	return {
		name      = "S0 Stats Driver",
		desc      = "Headless replay driver for the S0 boundary-size measurement pass: fast-forwards, arms /boundarydump (+optional /profiledump window), writes /calloutcensus at game end, then quits.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 1000,
		enabled   = true,
	}
end

-- Knobs come from engine config so the runner can parameterize without
-- editing this file (same pattern as test/replay-rewind):
--
--   S0DumpEndFrame : /boundarydump end frame (default 200000 = "whole game";
--                    a partial dump flushes at game end regardless)
--   S0ProfStart    : arm /profiledump at this frame (-1 = off)
--   S0ProfLen      : /profiledump window length in frames
--   S0QuitFrame    : hard quit at this frame (0 = quit on game over / demo end)
--   S0FastForward  : 1 => /setspeed 20 + /speedcontrol 0
--
-- The output label is read from S0Label (string) and prefixes all CSVs,
-- which land in the write data-dir.

local label, armBoundary, dumpEndFrame, profStart, profLen, quitFrame, fastForward
local started = false
local profStarted = false
local censusDone = false
local gameOverFrame = nil
local lastFrame = 0
local lastAdvanceTime = nil

local function announce(msg)
	Spring.Echo("[S0Driver] " .. msg)
end

local function writeCensusAndQuit(reason)
	if censusDone then
		return
	end
	censusDone = true
	announce("finishing (" .. reason .. ") at frame " .. Spring.GetGameFrame())
	Spring.SendCommands("calloutcensus " .. label .. "_callouts.csv")
	-- /boundarydump flushes partially at engine shutdown if its end frame was
	-- not reached; census is written synchronously by the command above
	Spring.SendCommands("quitforce")
end

function widget:Initialize()
	if not Spring.IsReplay() then
		announce("not a replay - driver disabled")
		widgetHandler:RemoveWidget(self)
		return
	end

	label        = Spring.GetConfigString("S0Label", "s0")
	armBoundary  = Spring.GetConfigInt("S0Boundary", 1)
	dumpEndFrame = Spring.GetConfigInt("S0DumpEndFrame", 200000)
	profStart    = Spring.GetConfigInt("S0ProfStart", -1)
	profLen      = Spring.GetConfigInt("S0ProfLen", 900)
	quitFrame    = Spring.GetConfigInt("S0QuitFrame", 0)
	fastForward  = Spring.GetConfigInt("S0FastForward", 1)

	announce(("label=%s dumpEnd=%d profStart=%d profLen=%d quit=%d ff=%d")
		:format(label, dumpEndFrame, profStart, profLen, quitFrame, fastForward))

	if fastForward ~= 0 then
		Spring.SendCommands("setspeed 20")
		Spring.SendCommands("speedcontrol 0")
	end
end

function widget:GameFrame(f)
	lastFrame = f
	lastAdvanceTime = os.clock()

	if not started and f >= 1 then
		started = true
		if armBoundary ~= 0 then
			Spring.SendCommands(("boundarydump 1 %d %s_boundary.csv"):format(dumpEndFrame, label))
			announce("armed /boundarydump 1 " .. dumpEndFrame)
		end
	end

	if profStart >= 0 and not profStarted and f >= profStart then
		profStarted = true
		Spring.SendCommands(("profiledump %d %d %s_profile.csv"):format(f + 1, f + profLen, label))
		announce(("armed /profiledump %d %d"):format(f + 1, f + profLen))
	end

	-- let the sim run a short tail past game over so trailing events settle,
	-- then write the census and quit
	if gameOverFrame ~= nil and f >= gameOverFrame + 90 then
		writeCensusAndQuit("game over")
	end

	if quitFrame > 0 and f >= quitFrame then
		writeCensusAndQuit("quit frame reached")
	end
end

function widget:GameOver()
	if gameOverFrame == nil then
		gameOverFrame = Spring.GetGameFrame()
		announce("game over at frame " .. gameOverFrame)
	end
end

function widget:Update()
	-- demo-end watchdog: if the sim stops advancing for 15 wall-clock seconds
	-- (demo stream exhausted without a GameOver, or playback stalled), finish
	if started and lastAdvanceTime ~= nil and (os.clock() - lastAdvanceTime) > 15 then
		writeCensusAndQuit("no sim advance for 15s (demo end?)")
	end
end
