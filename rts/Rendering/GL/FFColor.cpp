/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFColor.h"

#include "Rendering/GL/myGL.h"

#include <algorithm>

#include "Rendering/GL/FFShaderRewrite.h"
#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Log/ILog.h"

void GL::FFColorMirror::Set(float r, float g, float b, float a)
{
	current[0] = r;
	current[1] = g;
	current[2] = b;
	current[3] = a;

	if (!FFRewriteEnabled() || writeThrough)
		glColor4fv(current);

	if (!FFRewriteEnabled())
		return;

	// The pinned slot's current value, which is what every rewritten shader reads
	// in place of gl_Color. Not clamped: the fixed-function CURRENT colour is not
	// either (BAR widgets set overbright line colours and read them back), and the
	// clamp that does apply happens at rasterization, on both paths alike.
	glVertexAttrib4fv(GL::FF_COLOR_ATTRIB_LOC, current);
}

void GL::FFColorMirror::NotePushAttrib(unsigned int mask)
{
	if (depth < MAX_ATTRIB_DEPTH) {
		std::copy(current, current + 4, stack[depth]);
		savesColor[depth] = ((mask & GL_CURRENT_BIT) != 0);
	}
	++depth;
}

void GL::FFColorMirror::NotePopAttrib()
{
	if (depth == 0)
		return; // unbalanced; leave the mirror as-is and let Verify report

	--depth;

	if (depth >= MAX_ATTRIB_DEPTH || !savesColor[depth])
		return;

	std::copy(stack[depth], stack[depth] + 4, current);

	// the pop restored GL's own copy, so only the attribute needs catching up
	if (FFRewriteEnabled())
		glVertexAttrib4fv(GL::FF_COLOR_ATTRIB_LOC, current);
}

void GL::FFColorMirror::Verify(const char* where) const
{
	if (!GL::ffMirror.shadowCompare || FFRewriteEnabled())
		return;

	// a verifier that never ran reads exactly like one that found nothing
	static bool reported = false;
	if (!reported) {
		reported = true;
		LOG_L(L_WARNING, "[FFColorMirror] ACTIVE: checking the mirror against GL_CURRENT_COLOR");
	}

	float live[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glGetFloatv(GL_CURRENT_COLOR, live);

	if (std::equal(live, live + 4, current))
		return;

	static int logged = 0;
	if (logged++ < 16) {
		LOG_L(L_WARNING, "[FFColorMirror] %s: mirror (%g %g %g %g) != GL (%g %g %g %g) -- a writer bypassed GL::ffColor",
			where, current[0], current[1], current[2], current[3], live[0], live[1], live[2], live[3]);
	}
}
