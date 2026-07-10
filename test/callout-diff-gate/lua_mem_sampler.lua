function widget:GetInfo()
	return {
		name      = "Lua Mem Sampler",
		desc      = "PR 47 measurement instrument: samples Spring.GetLuaMemUsage + collectgarbage('count') every N game frames and echoes [LuaMemSampler] lines for offline growth-curve analysis.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 999,
		enabled   = true,
	}
end

-- Sample cadence in game frames (300 = 10s of game time)
local SAMPLE_EVERY = 300

local t0 = nil
local gcCycles0 = nil

local function sample(frame)
	if t0 == nil then
		t0 = Spring.GetTimer()
	end
	local wallSecs = Spring.DiffTimers(Spring.GetTimer(), t0)

	-- LuaUI state heap (what BAR's 1.2GB emergency valve measures), KB
	local luauiHeapKB = collectgarbage("count")

	-- engine allocator stats: this handle, global, unsynced-total, synced-total
	local hKB, hAllocs, gKB, gAllocs, uKB, uAllocs, sKB, sAllocs = Spring.GetLuaMemUsage()

	Spring.Echo(string.format(
		"[LuaMemSampler] f=%d df=%d wall=%.1f luauiHeapKB=%.0f handleKB=%.0f handleKAllocs=%.1f globalKB=%.0f unsyncedKB=%.0f syncedKB=%.0f",
		frame, Spring.GetDrawFrame(), wallSecs, luauiHeapKB, hKB, hAllocs, gKB, uKB, sKB))
end

function widget:GameFrame(n)
	if (n % SAMPLE_EVERY) == 0 then
		sample(n)
	end
end

function widget:Initialize()
	Spring.Echo("[LuaMemSampler] initialized, sampling every " .. SAMPLE_EVERY .. " frames")
end
