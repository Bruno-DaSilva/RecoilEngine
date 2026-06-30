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

#include <utility>
#include <vector>

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

// a small distinctive 2x2 RGBA texture (NEAREST, CLAMP) for the TexRect A/B.
GLuint MakeTestTexture()
{
	const uint8_t px[4 * 4] = {
		255,   0,   0, 255,    0, 255,   0, 255, // red,    green
		  0,   0, 255, 255,  255, 255,   0, 255, // blue,   yellow
	};
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
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


TEST_CASE("GLImmediateEmitter: TexRect (textured, MODULATE) matches legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();
	const GLuint tex = MakeTestTexture();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	// white color -> MODULATE shows the texture directly; full 0..1 texcoords
	buf.SetTexRect(40, 40, 200, 160, 0.0f, 0.0f, 1.0f, 1.0f,
		SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255)));

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LoadFFMatrix(mvp);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glEnable(GL_TEXTURE_2D);          // FF needs the texture target enabled
		buf.FlushTexRectLegacy();
		glDisable(GL_TEXTURE_2D);
	});
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex); // shader samples unit 0; no FF enable
		buf.FlushTexRectModern();
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	// the four quadrants should carry the four texel colors (NEAREST): sample
	// the lower-left quadrant center -> red texel (modern path actually textured)
	const size_t ll = (size_t(70) * kSize + 80) * 4;
	CHECK(modern[ll + 0] == 255);
	CHECK(modern[ll + 1] == 0);
	CHECK(modern[ll + 2] == 0);

	glDeleteTextures(1, &tex);
	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: textured BeginEnd captures texcoords, falls back to legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();
	const GLuint tex = MakeTestTexture();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_TRIANGLES);
	buf.Color(1.0f, 1.0f, 1.0f, 1.0f);
	// textured quad [40,200]x[40,160] as two CCW triangles
	buf.TexCoord(0, 0); buf.Vertex( 40,  40, 0);
	buf.TexCoord(1, 0); buf.Vertex(200,  40, 0);
	buf.TexCoord(1, 1); buf.Vertex(200, 160, 0);
	buf.TexCoord(0, 0); buf.Vertex( 40,  40, 0);
	buf.TexCoord(1, 1); buf.Vertex(200, 160, 0);
	buf.TexCoord(0, 1); buf.Vertex( 40, 160, 0);
	REQUIRE(buf.IsTextured());

	const auto bindTex = [&] {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glEnable(GL_TEXTURE_2D); // FlushModern falls back to FlushLegacy (FF texturing)
	};
	const auto legacy = RenderToBuffer([&] { ClearTo(0, 0, 0, 1); bindTex(); buf.FlushLegacy(); glDisable(GL_TEXTURE_2D); });
	const auto modern = RenderToBuffer([&] { ClearTo(0, 0, 0, 1); bindTex(); buf.FlushModern(); glDisable(GL_TEXTURE_2D); });

	// textured BeginEnd is not modernized yet -> modern falls back to legacy
	CHECK(Compare(legacy, modern).maxAbsDelta == 0);
	// and it actually sampled the texture (lower-left quadrant -> red texel)
	const size_t c = (size_t(70) * kSize + 80) * 4;
	CHECK(modern[c + 0] == 255);
	CHECK(modern[c + 1] == 0);
	CHECK(modern[c + 2] == 0);

	glDeleteTextures(1, &tex);
	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: LuaGLCompare::CompareDraws confirms and denies")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// CompareDraws renders both lambdas with the current FF matrices into its
	// own FBOs; set up an ortho so glRectf lands inside.
	glViewport(0, 0, kSize, kSize);
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, kSize, 0, kSize, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();

	const auto rectA = [] { glColor4f(1, 1, 1, 1); glRectf(40, 40, 200, 160); };
	const auto rectB = [] { glColor4f(1, 1, 1, 1); glRectf(60, 40, 220, 160); }; // shifted

	// identical draws -> zero delta (comparator confirms)
	CHECK(LuaGLCompare::CompareDraws(kSize, kSize, rectA, rectA) == 0);
	// different draws -> nonzero delta (comparator denies)
	CHECK(LuaGLCompare::CompareDraws(kSize, kSize, rectA, rectB) > 0);
}


TEST_CASE("GLImmediateEmitter: more primitive modes match legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	const auto deltaFor = [&](uint32_t mode, const std::vector<std::pair<float, float>>& pts) {
		LuaImmediateBuffer buf;
		buf.SetMVP(mvp);
		buf.Begin(mode);
		buf.Color(0.70f, 0.70f, 0.70f, 1.0f);
		for (const auto& p : pts)
			buf.Vertex(p.first, p.second, 0.0f);
		return CompareBothFlushes(buf, mvp).maxAbsDelta;
	};

	CHECK(deltaFor(GL_TRIANGLE_FAN,   {{128,128},{60,60},{196,60},{196,196},{60,196}}) <= 1);
	CHECK(deltaFor(GL_TRIANGLE_STRIP, {{50,50},{50,150},{120,50},{120,150},{190,50},{190,150}}) <= 1);
	CHECK(deltaFor(GL_QUAD_STRIP,     {{50,50},{50,150},{120,50},{120,150},{190,50},{190,150}}) <= 1);
	CHECK(deltaFor(GL_LINE_STRIP,     {{40,40},{200,60},{80,180},{210,200}}) <= 1);
	CHECK(deltaFor(GL_LINE_LOOP,      {{50,50},{200,60},{180,190},{60,180}}) <= 1);
	CHECK(deltaFor(GL_POINTS,         {{60,60},{120,90},{180,150},{90,200}}) <= 1);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: translucent draw-order composites identically (C4)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	// two overlapping half-transparent quads, drawn A then B
	LuaImmediateBuffer a, b;
	a.SetMVP(mvp);
	a.Begin(GL_TRIANGLES); a.Color(1.0f, 0.0f, 0.0f, 0.5f);
	a.Vertex(40, 40, 0); a.Vertex(160, 40, 0); a.Vertex(160, 160, 0);
	a.Vertex(40, 40, 0); a.Vertex(160, 160, 0); a.Vertex(40, 160, 0);
	b.SetMVP(mvp);
	b.Begin(GL_TRIANGLES); b.Color(0.0f, 1.0f, 0.0f, 0.5f);
	b.Vertex(90, 90, 0); b.Vertex(210, 90, 0); b.Vertex(210, 210, 0);
	b.Vertex(90, 90, 0); b.Vertex(210, 210, 0); b.Vertex(90, 210, 0);

	const auto blendOn  = [] { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); };
	const int d = LuaGLCompare::CompareDraws(kSize, kSize,
		[&] { blendOn(); a.FlushLegacy(); b.FlushLegacy(); glDisable(GL_BLEND); },
		[&] { blendOn(); a.FlushModern(); b.FlushModern(); glDisable(GL_BLEND); });

	INFO("blend/draw-order maxAbsDelta=" << d);
	CHECK(d >= 0);
	CHECK(d <= 1); // identical compositing of the overlap region

	RenderBuffer::KillStatic();
}


TEST_CASE("GLImmediateEmitter: empty and degenerate primitives don't crash (C5)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();

	RenderTarget rt;
	REQUIRE(rt.Make(kSize, kSize));
	rt.Bind();
	while (glGetError() != GL_NO_ERROR) {}

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);

	// empty BeginEnd: nothing accumulated
	buf.Begin(GL_TRIANGLES);
	CHECK(buf.Empty());
	buf.FlushLegacy();
	buf.FlushModern();
	CHECK(glGetError() == GL_NO_ERROR);

	// single vertex with TRIANGLES: not enough for a primitive, must not crash
	buf.Begin(GL_TRIANGLES);
	buf.Color(1.0f, 1.0f, 1.0f, 1.0f);
	buf.Vertex(50, 50, 0);
	buf.FlushLegacy();
	buf.FlushModern();
	CHECK(glGetError() == GL_NO_ERROR);

	// degenerate (zero-area) triangle
	buf.Begin(GL_TRIANGLES);
	buf.Vertex(80, 80, 0); buf.Vertex(80, 80, 0); buf.Vertex(80, 80, 0);
	buf.FlushModern();
	CHECK(glGetError() == GL_NO_ERROR);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	RenderBuffer::KillStatic();
}


namespace {

// observable, restorable GL state that a draw must not silently change (C3).
// FF-only state the modern path intentionally abandons (current color, the
// matrix stack) is deliberately excluded -- that's an expected divergence, not
// a leak.
struct GLState {
	GLint program = 0, vao = 0, arrayBuf = 0;
	GLint blendSrc = 0, blendDst = 0, activeTex = 0, boundTex2D = 0;
	GLboolean blend = GL_FALSE, depthTest = GL_FALSE, depthMask = GL_TRUE;

	bool operator==(const GLState& o) const {
		return program == o.program && vao == o.vao && arrayBuf == o.arrayBuf &&
		       blendSrc == o.blendSrc && blendDst == o.blendDst &&
		       activeTex == o.activeTex && boundTex2D == o.boundTex2D &&
		       blend == o.blend && depthTest == o.depthTest && depthMask == o.depthMask;
	}
};

GLState CaptureGLState()
{
	GLState s;
	glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
	glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrc);
	glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDst);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.boundTex2D);
	s.blend = glIsEnabled(GL_BLEND);
	s.depthTest = glIsEnabled(GL_DEPTH_TEST);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
	return s;
}

} // namespace


TEST_CASE("GLImmediateEmitter: modern flush leaves observable GL state intact (C3)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();
	const CMatrix44f mvp = OrthoMVP();
	const GLuint tex = MakeTestTexture();

	RenderTarget rt;
	REQUIRE(rt.Make(kSize, kSize));
	rt.Bind();

	// establish a distinctive fixed-function context (no shader / VAO / VBO
	// bound, as in real immediate-mode use) with non-default blend/depth/texture
	// state the modern flush must not disturb.
	glUseProgram(0);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ZERO);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	while (glGetError() != GL_NO_ERROR) {} // drain

	const GLState before = CaptureGLState();

	LuaImmediateBuffer buf;
	buf.SetMVP(mvp);
	buf.Begin(GL_TRIANGLES);
	buf.Color(0.5f, 0.5f, 0.5f, 1.0f);
	buf.Vertex(40, 40, 0); buf.Vertex(200, 40, 0); buf.Vertex(120, 160, 0);
	buf.FlushModern();

	const GLState after = CaptureGLState();

	CHECK(glGetError() == GL_NO_ERROR);
	CHECK(before == after);     // nothing observable leaked
	CHECK(after.program == 0);  // shader disabled
	CHECK(after.vao == 0);      // VAO unbound
	CHECK(after.blend == GL_TRUE);          // blend left enabled
	CHECK(after.depthMask == GL_FALSE);     // depth mask left as set
	CHECK(after.boundTex2D == GLint(tex));  // bound texture untouched

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &tex);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
