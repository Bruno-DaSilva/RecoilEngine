/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFFog.h"

#include "Rendering/GL/myGL.h"

#include <algorithm>

#include "Rendering/GL/FFShaderRewrite.h"
#include "Rendering/GL/FFStateTracker.h"

namespace {
	// Writing to GL is what the rewrite exists to stop; it survives with the
	// rewrite off (so an unenabled game is byte-identical and Verify has an
	// oracle) and while a fixed-function draw is still possible.
	bool WritesGL() { return !GL::FFRewriteEnabled() || GL::ffDrawsPossible; }
}

void GL::FFFogMirror::SetColor(const float* rgba)
{
	std::copy(rgba, rgba + 4, color);
	++generation;

	if (WritesGL())
		glFogfv(GL_FOG_COLOR, color);
}

void GL::FFFogMirror::SetMode(int32_t m)
{
	mode = m;
	++generation;

	if (WritesGL())
		glFogi(GL_FOG_MODE, mode);
}

void GL::FFFogMirror::SetStart(float f)
{
	start = f;
	++generation;

	if (WritesGL())
		glFogf(GL_FOG_START, start);
}

void GL::FFFogMirror::SetEnd(float f)
{
	end = f;
	++generation;

	if (WritesGL())
		glFogf(GL_FOG_END, end);
}

void GL::FFFogMirror::SetDensity(float f)
{
	density = f;
	++generation;

	if (WritesGL())
		glFogf(GL_FOG_DENSITY, density);
}

void GL::FFFogMirror::MaterializeIntoGL() const
{
	glFogfv(GL_FOG_COLOR, color);
	glFogi(GL_FOG_MODE, mode);
	glFogf(GL_FOG_START, start);
	glFogf(GL_FOG_END, end);
	glFogf(GL_FOG_DENSITY, density);
}
