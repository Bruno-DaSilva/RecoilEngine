/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 A/B against the REAL engine renderer: GL::TypedRenderBuffer vs legacy
// glBegin, same MVP. Unlike testGLMatrixDrawCompare (hand-rolled VAO+shader),
// this links the actual engine rendering path (RenderBuffers/Shader/VAO/VBO/
// StreamBuffer) so we validate what the migration will really run.
//
// This test lives in a subdirectory that removes -DUNIT_TEST, because the
// engine GL headers (myGL.h) intentionally #error under UNIT_TEST. The few
// engine globals the render path needs (globalRendering, configHandler) are
// provided as minimal stubs here to avoid dragging in the GL window system /
// VFS / config file machinery — see GLEngineStubs.cpp.
//
// SKIPs with no GL context. Run under xvfb-run + LIBGL_ALWAYS_SOFTWARE=1.

#include "GLTestHarness.h"

#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GL/VertexArrayTypes.h"
#include "System/Matrix44f.h"
#include "System/Color.h"

using namespace gltest;

namespace {

// quad as two CCW triangles, single color
std::vector<VA_TYPE_C> FlatQuadC(float x0, float y0, float x1, float y1, SColor col)
{
	const VA_TYPE_C tl{ {x0, y1, 0}, col };
	const VA_TYPE_C tr{ {x1, y1, 0}, col };
	const VA_TYPE_C br{ {x1, y0, 0}, col };
	const VA_TYPE_C bl{ {x0, y0, 0}, col };
	return { bl, br, tr,  bl, tr, tl };
}

CMatrix44f OrthoMVP()
{
	return CMatrix44f::OrthoProj(0.0f, float(kSize), 0.0f, float(kSize), -1.0f, 1.0f);
}

void LoadFFMatrix(const CMatrix44f& mvp)
{
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(static_cast<const float*>(mvp));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

void LegacyDrawC(const CMatrix44f& mvp, const std::vector<VA_TYPE_C>& v)
{
	LoadFFMatrix(mvp);
	glBegin(GL_TRIANGLES);
	for (const VA_TYPE_C& x : v) {
		glColor4ub(x.c.r, x.c.g, x.c.b, x.c.a);
		glVertex3f(x.pos.x, x.pos.y, x.pos.z);
	}
	glEnd();
}

void EngineDrawC(const CMatrix44f& mvp, const std::vector<VA_TYPE_C>& v)
{
	// the stock RenderBuffer shader transforms by gl_ModelViewProjectionMatrix,
	// so feed the same MVP through the fixed-function stack.
	LoadFFMatrix(mvp);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	for (const VA_TYPE_C& vert : v)
		rb.AddVertex(VA_TYPE_C{vert});

	auto& shader = rb.GetShader();
	shader.Enable();
	rb.DrawArrays(GL_TRIANGLES);
	shader.Disable();
}

} // namespace


TEST_CASE("GLRenderBufferCompare: engine TypedRenderBuffer vs legacy glBegin (flat quad)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();
	const auto quad = FlatQuadC(32, 32, 200, 160, SColor(uint8_t(204), uint8_t(102), uint8_t(51), uint8_t(255)));

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDrawC(mvp, quad);
	});
	const auto engine = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		EngineDrawC(mvp, quad);
	});

	const DiffResult d = Compare(legacy, engine);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	// guard against a false pass where BOTH sides draw nothing: the engine
	// buffer must actually contain the quad color at the FBO center (128,128),
	// which lies inside the [32,200]x[32,160] quad.
	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(engine[c + 0] == 204);
	CHECK(engine[c + 1] == 102);
	CHECK(engine[c + 2] == 51);

	RenderBuffer::KillStatic();
}
