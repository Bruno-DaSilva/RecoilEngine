/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/AttribStateVerify.h"

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Log/ILog.h"

#include <cstring>

constexpr GLenum GL::AttribSnapshot::CAPS[];

void GL::AttribSnapshot::Capture()
{
	for (int i = 0; i < NUM_CAPS; ++i)
		caps[i] = glIsEnabled(CAPS[i]);

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
