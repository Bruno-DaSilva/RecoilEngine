/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

namespace GL {
	// CPU mirror of the fixed-function RASTERIZATION parameters: the alpha-test
	// function, the line-stipple pattern and the user clip-plane equations.
	//
	// These are not like the colour and fog mirrors. Those hold values a shader
	// reads, which a uniform can supply instead; these configure per-fragment and
	// per-rasterization operations that the pipeline applies to shader draws too,
	// so nothing short of reproducing the operation in the shader retires them.
	//
	// What this does retire is every write made while the operation is DISABLED,
	// and that is most of them: LuaOpenGL::ResetGLState sets an alpha-test
	// function immediately after disabling alpha test, and the attrib-bracket
	// restore writes both the parameter and the enable with no idea whether the
	// parameter can be observed. Skipping a write to state nothing can read
	// cannot change any pixel -- the equivalence is GL semantics, not a
	// measurement -- provided the value is put in place before the operation is
	// switched on, which is what the glEnable hook is for.
	struct FFRasterMirror {
		void SetAlphaFunc(uint32_t func, float ref);
		void SetLineStipple(int32_t factor, uint32_t pattern);
		void SetClipPlane(uint32_t plane, const double* equation);

		// From the glEnable/glDisable wrapper. On the way ON, the mirrored
		// parameter is written first, so the operation never observes the value
		// the skipped writes left behind.
		void NoteCap(uint32_t cap, bool on);

		bool CapEnabled(uint32_t cap) const;

		// glPopAttrib restores the enables without going through glEnable, so the
		// mirror has to re-read them afterwards or it will skip a write to state
		// that has quietly become observable. The engine's own brackets are
		// explicit save/restore and do go through glEnable; this is for
		// gl.PushAttrib and for the shadow-compare verifier's real pop.
		void ResyncCaps();

	private:
		static constexpr int NUM_CLIP_PLANES = 6;

		// the GL defaults, so a mirror that is never written describes GL exactly
		uint32_t alphaFunc = 0x0207; // GL_ALWAYS
		float alphaRef = 0.0f;
		bool alphaTestOn = false;

		int32_t stippleFactor = 1;
		uint32_t stipplePattern = 0xFFFF;
		bool stippleOn = false;

		double clipPlanes[NUM_CLIP_PLANES][4] = {};
		bool clipPlaneOn[NUM_CLIP_PLANES] = {};
	};

	inline FFRasterMirror ffRaster;
}
