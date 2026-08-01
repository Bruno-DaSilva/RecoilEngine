/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "Rendering/GL/myGL.h"

namespace GL {
	// Oracle-checker for glPushAttrib -> explicit save/restore conversions.
	//
	// The attrib stack is on RenderDoc's unsupported list and is the largest
	// engine-side family left, but the whole-frame A/B gate is nearly blind to
	// these conversions: a missed state corrupts all four passes identically, so
	// the compare cancels it out and reads 0/0. The font bracket was caught only
	// by luck -- its recordable path runs on some passes and not others -- and
	// what it missed (GL_TEXTURE_2D, disabled by a callee three frames down the
	// call graph) is exactly the kind of thing eyeballing the bracket misses.
	//
	// So verify against the real thing. Under shadowCompare a converted bracket
	// ALSO issues the original glPushAttrib; at the end it snapshots the state its
	// explicit restore produced, calls glPopAttrib, snapshots again, and reports
	// any difference. That turns "I read the code and think this is the full set"
	// into a measurement, and it names the exact state that was missed.
	//
	// Only ever active under shadowCompare: glPushAttrib/glPopAttrib are
	// unsupported functions, so this must never run in a capture or meter run.
	struct AttribSnapshot {
		// GL_ENABLE_BIT members the engine or the Lua API can actually reach.
		// A cap absent here is one nothing in the process touches, so it cannot
		// differ across a bracket.
		static constexpr GLenum CAPS[] = {
			GL_ALPHA_TEST, GL_BLEND, GL_COLOR_LOGIC_OP, GL_COLOR_MATERIAL,
			GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER, GL_FOG, GL_LIGHTING,
			GL_LINE_SMOOTH, GL_LINE_STIPPLE, GL_MULTISAMPLE, GL_NORMALIZE,
			GL_POINT_SMOOTH, GL_POINT_SPRITE, GL_POLYGON_OFFSET_FILL,
			GL_POLYGON_OFFSET_LINE, GL_POLYGON_OFFSET_POINT, GL_POLYGON_SMOOTH,
			GL_RESCALE_NORMAL, GL_SCISSOR_TEST, GL_STENCIL_TEST,
			GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
			GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q,
			GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2,
			GL_CLIP_PLANE3, GL_CLIP_PLANE4, GL_CLIP_PLANE5,
			GL_LIGHT0, GL_LIGHT1,
		};
		static constexpr int NUM_CAPS = sizeof(CAPS) / sizeof(CAPS[0]);

		GLboolean caps[NUM_CAPS] = {};

		// non-enable state from the bits the engine's brackets actually push
		GLint blendSrcRGB = 0, blendDstRGB = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
		GLint blendEquationRGB = 0, blendEquationAlpha = 0;
		GLboolean colorMask[4] = {};
		GLint alphaTestFunc = 0;
		GLfloat alphaTestRef = 0.0f;
		GLboolean depthMask = GL_FALSE;
		GLint depthFunc = 0;
		GLint polygonModeFB[2] = { 0, 0 };
		GLint cullFaceMode = 0;
		GLint frontFace = 0;
		GLint viewport[4] = { 0, 0, 0, 0 };
		GLfloat depthRange[2] = { 0.0f, 0.0f };
		GLfloat lineWidth = 0.0f;
		GLfloat pointSize = 0.0f;
		GLfloat currentColor[4] = {};

		void Capture();
		// Names the first differing state, or nullptr when identical.
		const char* FirstDifference(const AttribSnapshot& o) const;
	};

	// Call at the point the converted bracket has finished its explicit restore.
	// Compares that result against what the real glPopAttrib produces, and
	// reports the mismatching state by name. Inert unless shadowCompare is on.
	void VerifyAttribRestore(const char* where);

	// Call where the converted bracket used to push. Inert unless shadowCompare.
	void ShadowPushAttrib(GLbitfield mask);
}
