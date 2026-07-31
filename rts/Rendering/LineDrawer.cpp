/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// TODO: move this out of Sim, this is rendering code!

#include "LineDrawer.h"

#include <cmath>

#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Game/UI/CommandColors.h"

CLineDrawer lineDrawer;


CLineDrawer::CLineDrawer()
	: lineStipple(false)
	, useColorRestarts(false)
	, useRestartColor(false)
	, restartAlpha(0.0f)
	, restartColor(NULL)
	, lastPos(ZeroVector)
	, lastColor(NULL)
	, stippleTimer(0.0f)
{
	lines.reserve(32);
	stippled.reserve(32);
}


void CLineDrawer::UpdateLineStipple()
{
	stippleTimer += (globalRendering->lastFrameTime * 0.001f * cmdColors.StippleSpeed());
	stippleTimer = std::fmod(stippleTimer, (16.0f / 20.0f));
}


void CLineDrawer::SetupLineStipple()
{
	const unsigned int stipPat = (0xffff & cmdColors.StipplePattern());
	if ((stipPat != 0x0000) && (stipPat != 0xffff)) {
		lineStipple = true;
	} else {
		lineStipple = false;
		return;
	}
	const unsigned int fullPat = (stipPat << 16) | (stipPat & 0x0000ffff);
	const int shiftBits = 15 - (int(stippleTimer * 20.0f) % 16);
	glLineStipple(cmdColors.StippleFactor(), (fullPat >> shiftBits));
}


// Draws the queued lines through a VA_TYPE_C4 RenderBuffer. This replaced a
// glColorPointer/glVertexPointer client-array path, which was the last reachable
// glColorPointer site (RenderDoc rejects all of them; see
// doc/bar-gl4-immediate-mode-inventory.md).
//
// The colours stay float. VA_TYPE_C's 8-bit SColor would round each component
// by up to 0.5 LSB, and these vertices come straight from the float arrays the
// legacy glColorPointer(4, GL_FLOAT, ...) stream fed, so the float type is what
// keeps the conversion pixel-identical. Measured: GLFFRemovalExperiment 2 ran
// both paths in the same frame, 788 frames, 0 pixels different.
//
// The transform is exact for the same structural reason: the RenderBuffer shader
// reads the compatibility gl_ModelViewProjectionMatrix builtin (uUseMVP unset),
// so the driver composes the MVP exactly as it does for the fixed-function draw.
// That is what makes this conversion safe where a CPU-composed uniform MVP would
// not be -- see the dense-MV note in doc/bar-gl4-immediate-mode-inventory.md.
//
// Line stipple is untouched: it is a rasterization state, applies to shader
// draws in a compatibility context, and glLineStipple is a separate offender
// with its own call sites.
void CLineDrawer::DrawAllBuffered()
{
	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C4>();
	auto& sh = rb.GetShader();

	const auto drawGroup = [&rb, &sh](const std::vector<LinePair>& group) {
		for (const LinePair& p : group) {
			const size_t numVerts = p.colors.size() / 4;
			if (numVerts == 0)
				continue;

			for (size_t i = 0; i < numVerts; ++i) {
				rb.AddVertex({
					float3(p.verts[i * 3 + 0], p.verts[i * 3 + 1], p.verts[i * 3 + 2]),
					float4(p.colors[i * 4 + 0], p.colors[i * 4 + 1], p.colors[i * 4 + 2], p.colors[i * 4 + 3])
				});
			}

			sh.Enable();
			rb.DrawArrays(p.type);
			sh.Disable();
		}
	};

	glPushAttrib(GL_ENABLE_BIT);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LINE_STIPPLE);

	drawGroup(lines);

	if (!stippled.empty()) {
		glEnable(GL_LINE_STIPPLE);
		drawGroup(stippled);
		glDisable(GL_LINE_STIPPLE);
	}

	glPopAttrib();
}

void CLineDrawer::DrawAll()
{
	if (lines.empty() && stippled.empty())
		return;

	DrawAllBuffered();

	lines.clear();
	stippled.clear();
}
