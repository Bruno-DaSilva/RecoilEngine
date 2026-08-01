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

-- Band geometry is FIXED, not derived from the viewport: the hash has to describe
-- the same pixels on every run, and a viewport-relative band would silently
-- compare different regions if the window size ever differed.
-- Sized to contain BOTH rows below (shader-bound and rawState), so the
-- cross-build hash covers the client-array/FF-shader path too rather than
-- stopping short of it.
local bandW, bandH = 1280, 400
local hashFrame = Spring.GetConfigInt("ABUnitShapeHashFrame", 0)

-- The rawState row textures half its shapes so the pixel gate covers the
-- textured fixed-function draw. Set ABUnitShapeTextured=0 to suppress that:
-- the OFFENDER METER has to score what BAR itself still reaches, and BAR's
-- only rawState site (unit_icongenerator) draws with GL_TEXTURE_2D disabled,
-- so leaving the amplifier on would report a path alive that BAR does not use.
local texturedRaw = (Spring.GetConfigInt("ABUnitShapeTextured", 1) == 1)

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

	-- Opaque backing so the band contains ONLY these shapes. Without it the world
	-- shows through between them, and the world is not reproducible across
	-- processes -- which would make the cross-build hash below worthless.
	gl.Color(0, 0, 0, 1)
	gl.Rect(0, 0, bandW, bandH)
	gl.Color(1, 1, 1, 1)

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

	-- rawState = TRUE, and nothing bound: the caller owns the state and owns
	-- none, so fixed function draws the model. This is the only thing that
	-- reaches S3DModelVAO's client-array bind, and in BAR the only site is
	-- unit_icongenerator's offscreen atlas -- a LOAD-TIME one-shot that no
	-- pixel gate can see. Drawing it per frame here is what makes the
	-- ModernModelFFShader experiment a measurement instead of a vacuous zero,
	-- exactly as the useLuaMat row above did for the tex-unit experiment.
	-- Half of them with GL_TEXTURE_2D enabled and half without: fixed function
	-- MODULATEs the bound texture only when the target is enabled, and
	-- gl.UnitShapeTextures binds without enabling, so a row that never calls
	-- gl.Texture(true) leaves the textured half of the substitute program
	-- unexercised while still reporting a clean gate.
	for i = 1, #shapes do
		gl.PushMatrix()
			gl.Translate(size * (i * 1.5), size * 2.6, 0)
			gl.Scale(size, size, size)
			gl.Rotate(25, 1, 0, 0)
			gl.Rotate(180 + i * 15, 0, 1, 0)
			gl.UnitShapeTextures(shapes[i], true)
			if texturedRaw and (i % 2) == 1 then
				gl.Texture(true)
			end
			gl.UnitShape(shapes[i], myTeam, true)
			gl.Texture(false)
			gl.UnitShapeTextures(shapes[i], false)
		gl.PopMatrix()
	end

	-- CROSS-BUILD GATE.
	--
	-- The whole-frame A/B harness compares two passes of one frame, so it can only
	-- vary something the engine can switch per pass. A change that moves the engine
	-- AND a shader together -- the attribute contract, say -- is invisible to it:
	-- both passes move, and it reports a clean 0/0 over broken output.
	--
	-- This is the missing half. The band above is deterministic across PROCESSES:
	-- fixed unit defs sorted by id, fixed screen positions, fixed rotations, opaque
	-- backing, no sim or clock input. So its pixels can be hashed and the hash
	-- compared between two builds. Run the same content on each and diff one log
	-- line; no engine support and no file I/O needed, since gl.ReadPixels is already
	-- exposed.
	if hashFrame > 0 and Spring.GetDrawFrame() == hashFrame then
		local px = gl.ReadPixels(0, 0, bandW, bandH)
		-- Rolling multiply-add over the returned components; order is fixed, so any
		-- pixel difference moves the digest. Deliberately NO bitwise operators:
		-- this is Lua 5.1, where `~` is not xor and `&` does not parse at all, and
		-- a syntax error here silently stops the whole widget from loading.
		local h = 2166136261
		local function mix(v)
			local b = math.floor((v or 0) * 255 + 0.5) % 256
			h = (h * 31 + b) % 4294967296
		end
		for _, row in ipairs(px) do
			if type(row) == "table" then
				for _, c in ipairs(row) do
					if type(c) == "table" then
						for _, v in ipairs(c) do mix(v) end
					else
						mix(c)
					end
				end
			else
				mix(row)
			end
		end
		Spring.Echo(("[AB UnitShape Driver] BANDHASH frame=%d %dx%d hash=%.0f")
			:format(hashFrame, bandW, bandH, h))
	end
end
