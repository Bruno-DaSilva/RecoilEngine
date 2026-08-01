/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFRasterState.h"

#include "Rendering/GL/myGL.h"

#include "Rendering/GL/FFShaderRewrite.h"

#include <algorithm>

namespace {
	// The measured-inert half; see the header. Off leaves every write in place.
	bool WritesGL() { return !GL::FFRewriteEnabled(); }

	int ClipPlaneIndex(uint32_t cap)
	{
		const int i = static_cast<int>(cap) - GL_CLIP_PLANE0;
		return (i >= 0 && i < 6) ? i : -1;
	}
}

void GL::FFRasterMirror::SetAlphaFunc(uint32_t func, float ref)
{
	alphaFunc = func;
	alphaRef = ref;

	if (alphaTestOn && WritesGL())
		glAlphaFunc(alphaFunc, alphaRef);
}

void GL::FFRasterMirror::SetLineStipple(int32_t factor, uint32_t pattern)
{
	stippleFactor = factor;
	stipplePattern = pattern;

	if (stippleOn && WritesGL())
		glLineStipple(stippleFactor, static_cast<GLushort>(stipplePattern));
}

void GL::FFRasterMirror::SetClipPlane(uint32_t plane, const double* equation)
{
	const int i = ClipPlaneIndex(plane);
	if (i < 0)
		return;

	std::copy(equation, equation + 4, clipPlanes[i]);

	if (clipPlaneOn[i] && WritesGL())
		glClipPlane(plane, clipPlanes[i]);
}

void GL::FFRasterMirror::NoteCap(uint32_t cap, bool on)
{
	if (cap == GL_ALPHA_TEST) {
		// the write the setter skipped while this was off, put in place before
		// anything can observe it
		if (on && !alphaTestOn && WritesGL())
			glAlphaFunc(alphaFunc, alphaRef);
		alphaTestOn = on;
		return;
	}

	if (cap == GL_LINE_STIPPLE) {
		if (on && !stippleOn && WritesGL())
			glLineStipple(stippleFactor, static_cast<GLushort>(stipplePattern));
		stippleOn = on;
		return;
	}

	if (const int i = ClipPlaneIndex(cap); i >= 0) {
		if (on && !clipPlaneOn[i] && WritesGL())
			glClipPlane(cap, clipPlanes[i]);
		clipPlaneOn[i] = on;
	}
}

void GL::FFRasterMirror::MaterializeIntoGL() const
{
	// the caller is about to draw through fixed function after all, so give the
	// rasterizer the parameters the writes above skipped
	if (alphaTestOn)
		glAlphaFunc(alphaFunc, alphaRef);
	if (stippleOn)
		glLineStipple(stippleFactor, static_cast<GLushort>(stipplePattern));

	for (int i = 0; i < NUM_CLIP_PLANES; ++i) {
		if (clipPlaneOn[i])
			glClipPlane(GL_CLIP_PLANE0 + i, clipPlanes[i]);
	}
}

void GL::FFRasterMirror::ResyncCaps()
{
	// glIsEnabled is supported by RenderDoc; this runs only after a real pop.
	alphaTestOn = (glIsEnabled(GL_ALPHA_TEST) == GL_TRUE);
	stippleOn = (glIsEnabled(GL_LINE_STIPPLE) == GL_TRUE);

	for (int i = 0; i < NUM_CLIP_PLANES; ++i)
		clipPlaneOn[i] = (glIsEnabled(GL_CLIP_PLANE0 + i) == GL_TRUE);
}

bool GL::FFRasterMirror::CapEnabled(uint32_t cap) const
{
	if (cap == GL_ALPHA_TEST)
		return alphaTestOn;
	if (cap == GL_LINE_STIPPLE)
		return stippleOn;
	if (const int i = ClipPlaneIndex(cap); i >= 0)
		return clipPlaneOn[i];

	return false;
}
