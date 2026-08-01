/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/AttribStateVerify.h"

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Log/ILog.h"

#include <cstring>

constexpr GLenum GL::AttribSnapshot::CAPS[];
constexpr GLenum GL::AttribSnapshot::UNIT_CAPS[];

void GL::AttribSnapshot::Capture()
{
	for (int i = 0; i < NUM_CAPS; ++i)
		caps[i] = glIsEnabled(CAPS[i]);

	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
	for (int u = 0; u < NUM_UNITS; ++u) {
		glActiveTexture(GL_TEXTURE0 + u);
		for (int i = 0; i < NUM_UNIT_CAPS; ++i)
			unitCaps[u][i] = glIsEnabled(UNIT_CAPS[i]);
	}
	glActiveTexture(activeUnit);

	glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRGB);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
	glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
	glGetIntegerv(GL_ALPHA_TEST_FUNC, &alphaTestFunc);
	glGetFloatv(GL_ALPHA_TEST_REF, &alphaTestRef);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
	glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
	glGetIntegerv(GL_POLYGON_MODE, polygonModeFB);
	glGetIntegerv(GL_CULL_FACE_MODE, &cullFaceMode);
	glGetIntegerv(GL_FRONT_FACE, &frontFace);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetFloatv(GL_DEPTH_RANGE, depthRange);
	glGetFloatv(GL_LINE_WIDTH, &lineWidth);
	glGetFloatv(GL_POINT_SIZE, &pointSize);
	glGetFloatv(GL_CURRENT_COLOR, currentColor);
	glGetFloatv(GL_FOG_COLOR, fogColor);
	glGetIntegerv(GL_FOG_MODE, &fogMode);
	glGetFloatv(GL_FOG_DENSITY, &fogDensity);
	glGetFloatv(GL_FOG_START, &fogStart);
	glGetFloatv(GL_FOG_END, &fogEnd);
	glGetIntegerv(GL_SHADE_MODEL, &shadeModel);
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
	if (mask & GL_ENABLE_BIT) {
		for (int i = 0; i < NUM_CAPS; ++i) {
			if (caps[i])
				glEnable(CAPS[i]);
			else
				glDisable(CAPS[i]);
		}

		for (int u = 0; u < NUM_UNITS; ++u) {
			glActiveTexture(GL_TEXTURE0 + u);
			for (int i = 0; i < NUM_UNIT_CAPS; ++i) {
				if (unitCaps[u][i])
					glEnable(UNIT_CAPS[i]);
				else
					glDisable(UNIT_CAPS[i]);
			}
		}
		// glPushAttrib(GL_ENABLE_BIT) does not save the active unit -- that is
		// GL_TEXTURE_BIT -- so put back whatever the caller had, not the captured
		// one, and leave the selector otherwise untouched.
		GLint nowActive = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &nowActive);
		glActiveTexture(nowActive);
	}

	if (mask & GL_COLOR_BUFFER_BIT) {
		// the enables in this bit are already covered above when both are asked
		// for; when only COLOR_BUFFER is asked for, blend enable still belongs to
		// ENABLE_BIT and is left alone, matching glPopAttrib
		glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
		glBlendEquationSeparate(blendEquationRGB, blendEquationAlpha);
		glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
		glAlphaFunc(alphaTestFunc, alphaTestRef);
		RestoreCap(GL_ALPHA_TEST); RestoreCap(GL_BLEND);
		RestoreCap(GL_DITHER);     RestoreCap(GL_COLOR_LOGIC_OP);
	}

	if (mask & GL_DEPTH_BUFFER_BIT) {
		glDepthMask(depthMask);
		glDepthFunc(depthFunc);
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
	}

	if (mask & GL_VIEWPORT_BIT) {
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glDepthRange(depthRange[0], depthRange[1]);
	}

	if (mask & GL_FOG_BIT) {
		glFogfv(GL_FOG_COLOR, fogColor);
		glFogi(GL_FOG_MODE, fogMode);
		glFogf(GL_FOG_DENSITY, fogDensity);
		glFogf(GL_FOG_START, fogStart);
		glFogf(GL_FOG_END, fogEnd);
		RestoreCap(GL_FOG);
	}

	if (mask & GL_CURRENT_BIT)
		glColor4fv(currentColor);

	if (mask & GL_LINE_BIT) {
		glLineWidth(lineWidth);
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

	AttribSnapshot afterExplicit;
	afterExplicit.Capture();

	glPopAttrib();

	AttribSnapshot afterPop;
	afterPop.Capture();

	if (const char* diff = afterExplicit.FirstDifference(afterPop)) {
		LOG_L(L_ERROR, "[AttribVerify] %s: explicit restore differs from glPopAttrib at %s",
			where, diff);
	}
}
