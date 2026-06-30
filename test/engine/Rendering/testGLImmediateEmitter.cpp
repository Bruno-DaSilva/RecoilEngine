/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// A/B the unified immediate-mode emitter (LuaImmediateBuffer): accumulate ONE
// vertex stream, then flush it both ways — legacy glBegin vs modern
// TypedRenderBuffer+uniform-MVP-shader — into two FBOs and compare. Because the
// same accumulated stream drives both flushes, the two paths receive identical
// input by construction; the only variable is the backend. See
// doc/bar-gl4-immediate-mode-inventory.md.
//
// SKIPs with no GL context. Run under xvfb-run + LIBGL_ALWAYS_SOFTWARE=1.

#include "GLTestHarness.h"

#include "Lua/LuaImmediateBuffer.h"
#include "Rendering/GL/RenderBuffers.h"
#include "System/Matrix44f.h"
#include "System/Color.h"

using namespace gltest;

namespace {

CMatrix44f OrthoMVP()
{
	return CMatrix44f::OrthoProj(0.0f, float(kSize), 0.0f, float(kSize), -1.0f, 1.0f);
}

// the legacy backend reads the fixed-function matrix, so set it to the MVP.
void LoadFFMatrix(const CMatrix44f& mvp)
{
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(static_cast<const float*>(mvp));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

// flush the (already-filled) emitter both ways and compare.
DiffResult CompareBothFlushes(const LuaImmediateBuffer& buf, const CMatrix44f& mvp)
{
	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LoadFFMatrix(mvp);
		buf.FlushLegacy();
	});
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		// modern path: no fixed-function matrix calls; uMVP carries the MVP.
		buf.FlushModern();
	});
	return Compare(legacy, modern);
}

} // namespace


TEST_CASE("GLImmediateEmitter: flat quad flushed both ways matches")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_TRIANGLES);
	buf.Color(0.80f, 0.40f, 0.20f, 1.0f);
	// quad [32,200]x[32,160] as two CCW triangles
	buf.Vertex( 32,  32, 0); buf.Vertex(200,  32, 0); buf.Vertex(200, 160, 0);
	buf.Vertex( 32,  32, 0); buf.Vertex(200, 160, 0); buf.Vertex( 32, 160, 0);
	REQUIRE(buf.GetVerts().size() == 6);

	const DiffResult d = CompareBothFlushes(buf, mvp);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: smooth-colored triangle flushed both ways within 1 LSB")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_TRIANGLES);
	buf.Color(1.0f, 0.0f, 0.0f, 1.0f); buf.Vertex( 40,  40, 0);
	buf.Color(0.0f, 1.0f, 0.0f, 1.0f); buf.Vertex(210,  60, 0);
	buf.Color(0.0f, 0.0f, 1.0f, 1.0f); buf.Vertex(128, 210, 0);

	const DiffResult d = CompareBothFlushes(buf, mvp);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: GL_QUADS flat fill triangulates to match legacy bit-exact")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_QUADS);
	buf.Color(0.70f, 0.30f, 0.55f, 1.0f);
	// one CCW quad; modern triangulates, legacy uses GL_QUADS natively
	buf.Vertex( 40,  40, 0); buf.Vertex(200,  40, 0); buf.Vertex(200, 160, 0); buf.Vertex( 40, 160, 0);

	const DiffResult d = CompareBothFlushes(buf, mvp);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.equal);            // flat fill: triangle union == FF quad coverage
	CHECK(d.maxAbsDelta == 0);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: GL_POLYGON flat pentagon triangulates to match legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_POLYGON);
	buf.Color(0.25f, 0.65f, 0.90f, 1.0f);
	// convex pentagon around (128,128)
	buf.Vertex(128, 208, 0);
	buf.Vertex( 52, 153, 0);
	buf.Vertex( 81,  64, 0);
	buf.Vertex(175,  64, 0);
	buf.Vertex(204, 153, 0);

	const DiffResult d = CompareBothFlushes(buf, mvp);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1); // FF fan vs our fan: flat fill, expect bit-exact

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: GL_LINES (width 1) matches legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_LINES);
	buf.Color(1.0f, 1.0f, 1.0f, 1.0f);
	buf.Vertex( 40,  40, 0); buf.Vertex(210, 150, 0);
	buf.Vertex( 40, 150, 0); buf.Vertex(210,  40, 0);

	const DiffResult d = CompareBothFlushes(buf, mvp);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	// width-1 lines: FF vs core line rasterization should agree on llvmpipe
	CHECK(d.maxAbsDelta <= 1);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: sticky color applies to later vertices")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();

	// set color once, emit a full quad: every vertex must carry that color, so
	// legacy and modern still agree (and the quad is actually drawn).
	const SColor stickyColor(0.20f, 0.85f, 0.55f, 1.0f);

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_TRIANGLES);
	buf.Color(stickyColor);
	buf.Vertex( 50,  50, 0); buf.Vertex(180,  50, 0); buf.Vertex(180, 150, 0);
	buf.Vertex( 50,  50, 0); buf.Vertex(180, 150, 0); buf.Vertex( 50, 150, 0);

	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		buf.FlushModern();
	});

	const DiffResult d = CompareBothFlushes(buf, mvp);
	CHECK(d.maxAbsDelta <= 1);

	// the center (115,100) is inside the quad -> the once-set sticky color
	// reached every vertex and filled it (expected bytes match SColor's
	// float->byte conversion exactly).
	const size_t c = (size_t(100) * kSize + 115) * 4;
	CHECK(modern[c + 0] == stickyColor.r);
	CHECK(modern[c + 1] == stickyColor.g);
	CHECK(modern[c + 2] == stickyColor.b);

	RenderBuffer::KillStatic();
}
