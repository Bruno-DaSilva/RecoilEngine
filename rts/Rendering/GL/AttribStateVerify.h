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
			GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2,
			GL_CLIP_PLANE3, GL_CLIP_PLANE4, GL_CLIP_PLANE5,
			GL_LIGHT0, GL_LIGHT1,
		};
		static constexpr int NUM_CAPS = sizeof(CAPS) / sizeof(CAPS[0]);

		// PER TEXTURE UNIT. glIsEnabled/glEnable only ever address the active
		// unit, so these have to be walked unit by unit or a restore writes one
		// unit's value onto another -- which broke 456/456 frames when this helper
		// first shipped without it.
		static constexpr GLenum UNIT_CAPS[] = {
			GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
			GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q,
		};
		static constexpr int NUM_UNIT_CAPS = sizeof(UNIT_CAPS) / sizeof(UNIT_CAPS[0]);

		static constexpr GLenum TEX_TARGETS[]  = { GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP };
		static constexpr GLenum TEX_BINDINGS[] = { GL_TEXTURE_BINDING_1D, GL_TEXTURE_BINDING_2D,
		                                           GL_TEXTURE_BINDING_3D, GL_TEXTURE_BINDING_CUBE_MAP };

		// The engine's legacy paths reach unit 6 at most (S3DModelVAO's tangent
		// channels); 8 covers that with headroom without paying for 32.
		static constexpr int NUM_UNITS = 8;

		GLboolean caps[NUM_CAPS] = {};
		GLboolean unitCaps[NUM_UNITS][NUM_UNIT_CAPS] = {};

		// GL_TEXTURE_BIT: the bindings are what its users here actually protect
		// (both remaining brackets wrap texture creation and want the caller's
		// binding back). Texture-env is NOT restored -- that would need glTexEnvi,
		// already retired, and nothing writes texenv any more; it is captured and
		// diffed under shadowCompare instead so the verifier says so if that
		// changes. Texture-object parameters are not modelled either: glPushAttrib
		// only saves them for objects bound AT PUSH TIME, and a texture created
		// inside the bracket is not among them, which is why these call sites work.
		GLint texBinding[NUM_UNITS][4] = {};
		GLint texEnvMode[NUM_UNITS] = {};
		GLint activeUnit = GL_TEXTURE0;
		// what Capture() was asked for; FirstDifference only compares within it
		GLbitfield capturedMask = 0;

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
		GLfloat fogColor[4] = {};
		GLint fogMode = 0;
		GLfloat fogDensity = 0.0f, fogStart = 0.0f, fogEnd = 0.0f;
		GLint shadeModel = 0;
		GLfloat clearColor[4] = {};
		GLint drawBuffer = 0;
		GLfloat blendColor[4] = {};
		GLfloat depthClearValue = 0.0f;
		GLint lineStipplePattern = 0, lineStippleRepeat = 0;
		GLfloat polygonOffsetFactor = 0.0f, polygonOffsetUnits = 0.0f;

		// GL_LIGHTING_BIT content, captured ONLY under shadowCompare: the getters
		// (glGetMaterialfv, glGetLightfv) are unsupported functions, so they must
		// not run in a normal frame. Detect-only -- never restored, because the
		// setters are four functions this programme already retired. Their whole
		// purpose is to let the verifier prove the "nothing writes lighting state
		// any more" assumption instead of asserting it.
		GLfloat matAmbient[4] = {}, matDiffuse[4] = {}, matSpecular[4] = {}, matEmission[4] = {};
		GLfloat matShininess = 0.0f;
		GLfloat light1Ambient[4] = {}, light1Diffuse[4] = {}, light1Specular[4] = {};
		GLint lightModelLocalViewer = 0, lightModelTwoSide = 0;

		// Capture only what `mask` covers. glPushAttrib was ONE call; a full
		// capture is ~146 queries plus ~94 sets on restore, and doing that
		// unconditionally made a meter run take 7 minutes instead of 1. Scoping
		// to the mask takes the common cases back down -- GL_VIEWPORT_BIT is two
		// queries, and RenderToTexture alone does 34k brackets a run.
		void Capture(GLbitfield mask);

		// Restores exactly the states covered by `mask`, mirroring glPopAttrib
		// semantics. Capturing everything and restoring by mask is what makes a
		// conversion exact without per-site analysis of the bracket's dynamic
		// extent -- the failure mode that a narrow hand-picked save invites.
		//
		// GL_CURRENT_BIT and GL_FOG_BIT ARE handled, but note what that costs:
		// restoring them needs glColor4fv and glFog*, which are unsupported
		// functions themselves. That is a deliberate trade -- both are already
		// reachable elsewhere (gl.Color, ISky::SetupFog), so using them here does
		// not keep any function alive that was not alive anyway, and it buys the
		// retirement of the attrib pair. When those families are eventually
		// retired, these restores become blockers and have to go with them.
		//
		// GL_LIGHTING_BIT is NOT restored: shade model, materials and light
		// parameters would need glShadeModel/glMaterial*/glLight*, all of which
		// this programme has already retired, so restoring them would resurrect
		// four dead functions. Nothing in the engine writes that state any more,
		// so it cannot differ across a bracket -- and shadeModel is captured and
		// diffed (never restored) precisely so the verifier says so if that
		// assumption ever breaks.
		//
		// Per-unit texture enables ARE handled (see UNIT_CAPS): the first version
		// of this helper walked only the active unit and broke 456/456 frames when
		// applied to a bracket that switches units. FirstDifference was blind to
		// the same thing, so the pixel gate caught it, not the verifier.
		void Restore(GLbitfield mask) const;

		// restores one cap from the snapshot; several attrib bits own enables
		void RestoreCap(GLenum cap) const;

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
