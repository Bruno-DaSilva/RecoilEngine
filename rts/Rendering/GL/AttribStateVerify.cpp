/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/AttribStateVerify.h"

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Log/ILog.h"

#include <cstring>

constexpr GLenum GL::AttribSnapshot::CAPS[];
constexpr GLenum GL::AttribSnapshot::UNIT_CAPS[];
constexpr GLenum GL::AttribSnapshot::TEX_TARGETS[];
constexpr GLenum GL::AttribSnapshot::TEX_BINDINGS[];

void GL::AttribSnapshot::Capture(GLbitfield mask)
{
	capturedMask = mask;

	const bool wantEnables = (mask & (GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_COLOR_BUFFER_BIT |
	                                  GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT | GL_FOG_BIT |
	                                  GL_LINE_BIT | GL_POINT_BIT | GL_SCISSOR_BIT |
	                                  GL_STENCIL_BUFFER_BIT)) != 0;
	const bool wantUnits = (mask & (GL_ENABLE_BIT | GL_TEXTURE_BIT)) != 0;

	if (wantEnables) {
		for (int i = 0; i < NUM_CAPS; ++i)
			caps[i] = glIsEnabled(CAPS[i]);
	}

	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
	for (int u = 0; wantUnits && u < NUM_UNITS; ++u) {
		glActiveTexture(GL_TEXTURE0 + u);
		for (int i = 0; i < NUM_UNIT_CAPS; ++i)
			unitCaps[u][i] = glIsEnabled(UNIT_CAPS[i]);
		for (int t = 0; t < 4; ++t)
			glGetIntegerv(TEX_BINDINGS[t], &texBinding[u][t]);
		if (GL::ffMirror.shadowCompare)
			glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &texEnvMode[u]);
	}
	glActiveTexture(activeUnit);

	if (mask & GL_COLOR_BUFFER_BIT) {
		glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
		glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRGB);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
		glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
		glGetIntegerv(GL_ALPHA_TEST_FUNC, &alphaTestFunc);
		glGetFloatv(GL_ALPHA_TEST_REF, &alphaTestRef);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
		glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
		glGetFloatv(GL_BLEND_COLOR, blendColor);
	}

	if (mask & GL_DEPTH_BUFFER_BIT) {
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
		glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
		glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depthClearValue);
	}

	if (mask & GL_POLYGON_BIT) {
		glGetIntegerv(GL_POLYGON_MODE, polygonModeFB);
		glGetIntegerv(GL_CULL_FACE_MODE, &cullFaceMode);
		glGetIntegerv(GL_FRONT_FACE, &frontFace);
		glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polygonOffsetFactor);
		glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polygonOffsetUnits);
	}

	if (mask & GL_VIEWPORT_BIT) {
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetFloatv(GL_DEPTH_RANGE, depthRange);
	}

	if (mask & GL_LINE_BIT) {
		glGetFloatv(GL_LINE_WIDTH, &lineWidth);
		glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &lineStipplePattern);
		glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &lineStippleRepeat);
	}

	if (mask & GL_POINT_BIT)
		glGetFloatv(GL_POINT_SIZE, &pointSize);

	if (mask & GL_CURRENT_BIT)
		std::copy_n(GL::ffColor.Get(), 4, currentColor);

	if (mask & GL_FOG_BIT) {
		glGetFloatv(GL_FOG_COLOR, fogColor);
		glGetIntegerv(GL_FOG_MODE, &fogMode);
		glGetFloatv(GL_FOG_DENSITY, &fogDensity);
		glGetFloatv(GL_FOG_START, &fogStart);
		glGetFloatv(GL_FOG_END, &fogEnd);
	}

	// detect-only, and cheap enough to always take: it is the tripwire for
	// "nothing writes fixed-function lighting state any more"
	glGetIntegerv(GL_SHADE_MODEL, &shadeModel);

	// verify-only: these getters are unsupported functions themselves
	if (GL::ffMirror.shadowCompare) {
		glGetMaterialfv(GL_FRONT, GL_AMBIENT,   matAmbient);
		glGetMaterialfv(GL_FRONT, GL_DIFFUSE,   matDiffuse);
		glGetMaterialfv(GL_FRONT, GL_SPECULAR,  matSpecular);
		glGetMaterialfv(GL_FRONT, GL_EMISSION,  matEmission);
		glGetMaterialfv(GL_FRONT, GL_SHININESS, &matShininess);
		glGetLightfv(GL_LIGHT1, GL_AMBIENT,  light1Ambient);
		glGetLightfv(GL_LIGHT1, GL_DIFFUSE,  light1Diffuse);
		glGetLightfv(GL_LIGHT1, GL_SPECULAR, light1Specular);
		glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, &lightModelLocalViewer);
		glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &lightModelTwoSide);
	}
}

// Several attrib bits own enables of their own, so restoring a bit means
// restoring those too -- GL_FOG_BIT includes the GL_FOG enable, GL_DEPTH_BUFFER_BIT
// the GL_DEPTH_TEST enable, and so on. Missing that is exactly what the verifier
// reported as "differs from glPopAttrib at enable 0x0b60".
void GL::AttribSnapshot::RestoreCap(GLenum cap) const
{
	for (int i = 0; i < NUM_CAPS; ++i) {
		if (CAPS[i] != cap)
			continue;
		if (caps[i])
			glEnable(cap);
		else
			glDisable(cap);
		return;
	}
}

void GL::AttribSnapshot::Restore(GLbitfield mask) const
{
	// This is the glPopAttrib replacement, so it runs on every draw callin, and
	// restoring unconditionally made it the single largest producer of several
	// RenderDoc-unsupported functions in a run: glAlphaFunc 19,122 calls,
	// glLineStipple 8,465, glFogfv/glFogi 2,664 each -- more than every other
	// site for those functions combined. Writing a state the value it already
	// holds cannot change any rendering, so read the current values back and
	// skip those writes. The reads (glGet*/glIsEnabled) are supported functions;
	// the writes are not, which is what makes the trade worth making.
	//
	// Only the unsupported setters below are guarded. The rest stay
	// unconditional: they cost nothing at the capture gate, and every guard is
	// another chance to mismodel a state.
	AttribSnapshot cur;
	cur.Capture(mask);

	if (mask & GL_ENABLE_BIT) {
		for (int i = 0; i < NUM_CAPS; ++i) {
			if (caps[i])
				glEnable(CAPS[i]);
			else
				glDisable(CAPS[i]);
		}

		// glPushAttrib(GL_ENABLE_BIT) does not save the active unit -- that is
		// GL_TEXTURE_BIT -- so the selector must come back exactly as the CALLER
		// left it. Read it BEFORE the walk: reading it afterwards just returns
		// the last unit the loop selected, which silently parks every subsequent
		// draw on unit 7 (measured: 463/463 frames wrong, max delta 255, and
		// invisible to the state verifier because the active unit is not part of
		// the snapshot being compared).
		GLint callerActive = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &callerActive);

		for (int u = 0; u < NUM_UNITS; ++u) {
			glActiveTexture(GL_TEXTURE0 + u);
			for (int i = 0; i < NUM_UNIT_CAPS; ++i) {
				if (unitCaps[u][i])
					glEnable(UNIT_CAPS[i]);
				else
					glDisable(UNIT_CAPS[i]);
			}
		}

		glActiveTexture(callerActive);
	}

	if (mask & GL_COLOR_BUFFER_BIT) {
		// the enables in this bit are already covered above when both are asked
		// for; when only COLOR_BUFFER is asked for, blend enable still belongs to
		// ENABLE_BIT and is left alone, matching glPopAttrib
		glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
		glBlendEquationSeparate(blendEquationRGB, blendEquationAlpha);
		glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
		if (cur.alphaTestFunc != alphaTestFunc || cur.alphaTestRef != alphaTestRef)
			glAlphaFunc(alphaTestFunc, alphaTestRef);
		RestoreCap(GL_ALPHA_TEST); RestoreCap(GL_BLEND);
		RestoreCap(GL_DITHER);     RestoreCap(GL_COLOR_LOGIC_OP);
		glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
		glBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
		glDrawBuffer(drawBuffer);
	}

	if (mask & GL_DEPTH_BUFFER_BIT) {
		glDepthMask(depthMask);
		glDepthFunc(depthFunc);
		glClearDepth(depthClearValue);
		RestoreCap(GL_DEPTH_TEST);
	}

	if (mask & GL_POLYGON_BIT) {
		// GL_POLYGON_MODE reports {front, back}; the engine only ever sets them
		// together via GL_FRONT_AND_BACK, so restoring the front value covers it
		glPolygonMode(GL_FRONT_AND_BACK, polygonModeFB[0]);
		glCullFace(cullFaceMode);
		glFrontFace(frontFace);
		RestoreCap(GL_CULL_FACE);           RestoreCap(GL_POLYGON_SMOOTH);
		RestoreCap(GL_POLYGON_OFFSET_FILL); RestoreCap(GL_POLYGON_OFFSET_LINE);
		RestoreCap(GL_POLYGON_OFFSET_POINT);
		glPolygonOffset(polygonOffsetFactor, polygonOffsetUnits);
	}

	if (mask & GL_VIEWPORT_BIT) {
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glDepthRange(depthRange[0], depthRange[1]);
	}

	if (mask & GL_TEXTURE_BIT) {
		GLint callerActive = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &callerActive);
		for (int u = 0; u < NUM_UNITS; ++u) {
			glActiveTexture(GL_TEXTURE0 + u);
			for (int t = 0; t < 4; ++t)
				glBindTexture(TEX_TARGETS[t], texBinding[u][t]);

			// GL_TEXTURE_BIT saves the per-unit texture ENABLES as well as the
			// bindings -- they live in both it and GL_ENABLE_BIT. Restoring only
			// the bindings left GL_TEXTURE_2D wrong on unit 0, which the verifier
			// reported 183 times in one run while the pixel gate passed.
			for (int i = 0; i < NUM_UNIT_CAPS; ++i) {
				if (unitCaps[u][i])
					glEnable(UNIT_CAPS[i]);
				else
					glDisable(UNIT_CAPS[i]);
			}
		}
		glActiveTexture(callerActive);
	}

	if (mask & GL_FOG_BIT) {
		if (!std::equal(fogColor, fogColor + 4, cur.fogColor))
			glFogfv(GL_FOG_COLOR, fogColor);
		if (cur.fogMode    != fogMode)    glFogi(GL_FOG_MODE, fogMode);
		if (cur.fogDensity != fogDensity) glFogf(GL_FOG_DENSITY, fogDensity);
		if (cur.fogStart   != fogStart)   glFogf(GL_FOG_START, fogStart);
		if (cur.fogEnd     != fogEnd)     glFogf(GL_FOG_END, fogEnd);
		RestoreCap(GL_FOG);
	}

	if ((mask & GL_CURRENT_BIT) && !std::equal(currentColor, currentColor + 4, cur.currentColor))
		GL::ffColor.Set(currentColor);

	if (mask & GL_LINE_BIT) {
		glLineWidth(lineWidth);
		if (cur.lineStippleRepeat != lineStippleRepeat || cur.lineStipplePattern != lineStipplePattern)
			glLineStipple(lineStippleRepeat, static_cast<GLushort>(lineStipplePattern));
		RestoreCap(GL_LINE_SMOOTH); RestoreCap(GL_LINE_STIPPLE);
	}

	if (mask & GL_POINT_BIT) {
		glPointSize(pointSize);
		RestoreCap(GL_POINT_SMOOTH); RestoreCap(GL_POINT_SPRITE);
	}

	if (mask & GL_SCISSOR_BIT)
		RestoreCap(GL_SCISSOR_TEST);

	if (mask & GL_STENCIL_BUFFER_BIT)
		RestoreCap(GL_STENCIL_TEST);
}

const char* GL::AttribSnapshot::FirstDifference(const AttribSnapshot& o) const
{
	// Enables first: they are what a narrow save most often misses, and naming
	// the cap is what makes the report actionable.
	for (int i = 0; i < NUM_CAPS; ++i) {
		if (caps[i] != o.caps[i]) {
			static char buf[64];
			snprintf(buf, sizeof(buf), "enable 0x%04x", CAPS[i]);
			return buf;
		}
	}

	for (int u = 0; u < NUM_UNITS; ++u) {
		for (int i = 0; i < NUM_UNIT_CAPS; ++i) {
			if (unitCaps[u][i] != o.unitCaps[u][i]) {
				static char buf[64];
				snprintf(buf, sizeof(buf), "unit %d enable 0x%04x", u, UNIT_CAPS[i]);
				return buf;
			}
		}
		for (int t = 0; t < 4; ++t) {
			if (texBinding[u][t] != o.texBinding[u][t]) {
				static char buf[64];
				snprintf(buf, sizeof(buf), "unit %d binding 0x%04x", u, TEX_TARGETS[t]);
				return buf;
			}
		}
		if (texEnvMode[u] != o.texEnvMode[u]) {
			static char buf[64];
			snprintf(buf, sizeof(buf), "unit %d texenv mode", u);
			return buf;
		}
	}

	#define DIFF_SCALAR(field) if (field != o.field) return #field;
	#define DIFF_ARRAY(field, n) if (std::memcmp(field, o.field, sizeof(field[0]) * (n)) != 0) return #field;

	DIFF_SCALAR(blendSrcRGB)   DIFF_SCALAR(blendDstRGB)
	DIFF_SCALAR(blendSrcAlpha) DIFF_SCALAR(blendDstAlpha)
	DIFF_SCALAR(blendEquationRGB) DIFF_SCALAR(blendEquationAlpha)
	DIFF_ARRAY(colorMask, 4)
	DIFF_SCALAR(alphaTestFunc) DIFF_SCALAR(alphaTestRef)
	DIFF_SCALAR(depthMask)     DIFF_SCALAR(depthFunc)
	DIFF_ARRAY(polygonModeFB, 2)
	DIFF_SCALAR(cullFaceMode)  DIFF_SCALAR(frontFace)
	DIFF_ARRAY(viewport, 4)    DIFF_ARRAY(depthRange, 2)
	DIFF_SCALAR(lineWidth)     DIFF_SCALAR(pointSize)
	DIFF_ARRAY(currentColor, 4)
	DIFF_ARRAY(fogColor, 4)
	DIFF_SCALAR(fogMode) DIFF_SCALAR(fogDensity) DIFF_SCALAR(fogStart) DIFF_SCALAR(fogEnd)
	DIFF_SCALAR(shadeModel)
	DIFF_ARRAY(clearColor, 4) DIFF_SCALAR(drawBuffer) DIFF_ARRAY(blendColor, 4)
	DIFF_SCALAR(depthClearValue)
	DIFF_SCALAR(lineStipplePattern) DIFF_SCALAR(lineStippleRepeat)
	DIFF_SCALAR(polygonOffsetFactor) DIFF_SCALAR(polygonOffsetUnits)
	DIFF_ARRAY(matAmbient, 4)  DIFF_ARRAY(matDiffuse, 4)
	DIFF_ARRAY(matSpecular, 4) DIFF_ARRAY(matEmission, 4)
	DIFF_SCALAR(matShininess)
	DIFF_ARRAY(light1Ambient, 4) DIFF_ARRAY(light1Diffuse, 4) DIFF_ARRAY(light1Specular, 4)
	DIFF_SCALAR(lightModelLocalViewer) DIFF_SCALAR(lightModelTwoSide)

	#undef DIFF_SCALAR
	#undef DIFF_ARRAY

	return nullptr;
}

void GL::ShadowPushAttrib(GLbitfield mask)
{
	if (!GL::ffMirror.shadowCompare)
		return;

	glPushAttrib(mask);
}

void GL::VerifyAttribRestore(const char* where)
{
	if (!GL::ffMirror.shadowCompare)
		return;

	// Verification captures EVERYTHING regardless of the bracket's mask -- it
	// only runs under shadowCompare, so its cost does not matter, and a
	// mask-scoped capture would leave uncompared fields holding stale values.
	static constexpr GLbitfield ALL = GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
	                                  GL_POLYGON_BIT | GL_VIEWPORT_BIT | GL_LINE_BIT |
	                                  GL_POINT_BIT | GL_CURRENT_BIT | GL_FOG_BIT | GL_TEXTURE_BIT;
	AttribSnapshot afterExplicit;
	afterExplicit.Capture(ALL);

	glPopAttrib();

	AttribSnapshot afterPop;
	afterPop.Capture(ALL);

	if (const char* diff = afterExplicit.FirstDifference(afterPop)) {
		LOG_L(L_ERROR, "[AttribVerify] %s: explicit restore differs from glPopAttrib at %s",
			where, diff);
	}
}
