function widget:GetInfo()
	return {
		name = "Lua Piece Check", desc = "PR47 one-off: print GetUnitPieceMap returns from draw vs non-draw context",
		author = "test harness", date = "2026", license = "GPL", layer = 999, enabled = true,
	}
end

local doneDraw, doneFrame = false, false

local function report(ctx)
	local units = Spring.GetAllUnits()
	if not units or #units == 0 then return false end
	local uid = units[1]
	local m = Spring.GetUnitPieceMap(uid)
	local l = Spring.GetUnitPieceList(uid)
	local nm = 0
	if type(m) == "table" then for _ in pairs(m) do nm = nm + 1 end end
	Spring.Echo(string.format("[PieceCheck] ctx=%s uid=%d map=%s(%d) list=%s(%d)",
		ctx, uid, type(m), nm, type(l), (type(l) == "table") and #l or -1))
	return true
end

function widget:DrawGenesis()
	if doneDraw then return end
	if Spring.GetGameFrame() >= 3000 then doneDraw = report("DrawGenesis") end
end

function widget:GameFrame(n)
	if doneFrame then return end
	if n >= 3000 then doneFrame = report("GameFrame") end
end
