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
--   DiffGateFastForward  : 1 => /setspeed 20 + /speedcontrol 0
--   DiffGateQuitFrame    : hard quit at this frame (0 = quit on game over / demo end)
--   DiffGateExercise     : 1 => call the positions-family callouts over all units
--                          each draw frame (keeps the draw-side surface PR 18 will
--                          redirect hot; the gate itself verifies via CheckBoundary)
--
-- Pass criterion: the engine's [SnapshotDiffGate] report prints
--   "PASS (0 mismatches)" and total mismatches=0 over the whole replay.

local label, fastForward, quitFrame, exercise
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
	-- print the running totals, then let engine teardown FlushPartial() print
	-- the final report as well
	Spring.SendCommands("snapshotdiffgate dump")
	Spring.SendCommands("snapshotdiffgate disarm")
	Spring.SendCommands("quitforce")
end

function widget:Initialize()
	if not Spring.IsReplay() then
		announce("not a replay - driver disabled")
		widgetHandler:RemoveWidget(self)
		return
	end

	label       = Spring.GetConfigString("DiffGateLabel", "diffgate")
	fastForward = Spring.GetConfigInt("DiffGateFastForward", 1)
	quitFrame   = Spring.GetConfigInt("DiffGateQuitFrame", 0)
	exercise    = Spring.GetConfigInt("DiffGateExercise", 1)

	announce(("label=%s ff=%d quit=%d exercise=%d"):format(label, fastForward, quitFrame, exercise))

	if fastForward ~= 0 then
		Spring.SendCommands("setspeed 20")
		Spring.SendCommands("speedcontrol 0")
	end
end

function widget:GameFrame(f)
	lastAdvanceTime = os.clock()

	if not armed and f >= 1 then
		armed = true
		Spring.SendCommands("snapshotdiffgate arm")
		announce("armed /snapshotdiffgate at frame " .. f)
	end

	-- let the sim run a short tail past game over so trailing events settle
	if gameOverFrame ~= nil and f >= gameOverFrame + 90 then
		dumpAndQuit("game over")
	end

	if quitFrame > 0 and f >= quitFrame then
		dumpAndQuit("quit frame reached")
	end
end

-- draw-context load generator: hit the positions/visibility callout families the
-- gate exists to verify, over every unit the local (spectator full-view) client
-- can see, once per draw frame. In PR 17 these still read live sim; in PR 18 they
-- will be snapshot-served and the gate's CheckBoundary is what actually verifies
-- them. Kept here so the draw surface matches the future load.
function widget:Update()
	if not armed or finished or exercise == 0 then
		return
	end

	local units = Spring.GetAllUnits()
	for i = 1, #units do
		local uid = units[i]
		Spring.GetUnitPosition(uid, true, true)
		Spring.GetUnitViewPosition(uid)
		Spring.IsUnitVisible(uid)
	end

	-- demo-end watchdog: if the sim stops advancing for 15 wall-clock seconds
	-- (demo stream exhausted without a GameOver), finish
	if armed and lastAdvanceTime ~= nil and (os.clock() - lastAdvanceTime) > 15 then
		dumpAndQuit("no sim advance for 15s (demo end?)")
	end
end

function widget:GameOver()
	if gameOverFrame == nil then
		gameOverFrame = Spring.GetGameFrame()
		announce("game over at frame " .. gameOverFrame)
	end
end
