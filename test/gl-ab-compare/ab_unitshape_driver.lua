function widget:GetInfo()
	return {
		name      = "AB Gate UnitShape Driver",
		desc      = "Draws unit shapes every frame so the engine's legacy model path (S3DModelVAO::BindLegacyVertexAttribsAndVBOs) is actually exercised under the whole-frame A/B compare gate.",
		author    = "test harness",
		date      = "2026",
		license   = "GPL",
		layer     = 100000,
		enabled   = true,
	}
end

-- Active only when config ABUnitShapeDriver=1; inert otherwise.
--
-- WHY THIS EXISTS. gl.UnitShape is the only thing in a BAR run that reaches
-- S3DModelVAO::BindLegacyVertexAttribsAndVBOs (confirmed by backtrace:
-- LuaOpenGL::UnitShape -> GLObjectShape -> S3DModel::DrawStatic), and BAR only
-- calls it when a build menu or ghost placement is on screen. Measured, that is
-- ~30 binds for a whole run of watertest, idletest OR a full replay -- at most
-- ~7 compared frames under the 4-pass harness, which makes any 0/0 on that path
-- vacuous rather than reassuring. Busier content does not help, because the
-- driver of this path is UI state, not combat. So the coverage has to be
-- manufactured deliberately.

local enabled = (Spring.GetConfigInt("ABUnitShapeDriver", 0) == 1)

local shapes = {}
local myTeam = 0

function widget:Initialize()
	if not enabled then
		widgetHandler:RemoveWidget()
		return
	end

	myTeam = Spring.GetMyTeamID() or 0

	-- A spread of defs, so the draws cover several models rather than hammering
	-- one. Sorted by id for run-to-run stability -- pairs() order over UnitDefs
	-- is not deterministic, and the A/B gate compares repeat renders of ONE
	-- frame, so an unstable set would only muddy a divergence, not cause one.
	local ids = {}
	for unitDefID in pairs(UnitDefs) do
		ids[#ids + 1] = unitDefID
	end
	table.sort(ids)
	for i = 1, math.min(8, #ids) do
		shapes[#shapes + 1] = ids[i]
	end

	Spring.Echo(("[AB UnitShape Driver] active, %d defs"):format(#shapes))
end

function widget:DrawScreen()
	if #shapes == 0 then
		return
	end

	local vsx, vsy = Spring.GetViewGeometry()
	local size = math.min(vsx, vsy) * 0.06

	-- Bottom strip, away from the build menu, so this adds draws rather than
	-- overlapping existing UI (overlap would still compare fine, but a clean
	-- band makes a real divergence easy to localise in the diff dump).
	for i = 1, #shapes do
		gl.PushMatrix()
			gl.Translate(size * (i * 1.5), size * 1.2, 0)
			gl.Scale(size, size, size)
			gl.Rotate(25, 1, 0, 0)
			gl.Rotate(180 + i * 15, 0, 1, 0)
			-- alternate useLuaMat: odd i uses BAR's material shader, even i uses
			-- the ENGINE's ModelVertProg/ModelFragProg -- the only shaders that
			-- read gl_LightSource/gl_FrontMaterial, so an FF-lighting experiment
			-- can only be trusted if this path is covered too.
			gl.UnitShape(shapes[i], myTeam, false, (i % 2) == 1, false)
		gl.PopMatrix()
	end
end
