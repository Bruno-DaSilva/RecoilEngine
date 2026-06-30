/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 A/B: legacy fixed-function (glBegin) vs a modern, RenderDoc-clean
// draw (VAO + uniform-MVP GLSL shader), BOTH fed the *identical* MVP built by
// the Phase-1 GLMatrixStateTracker. See doc/bar-gl4-immediate-mode-inventory.md.
//
// Why this shape: the engine's stock TypedRenderBuffer shader transforms via
// gl_ModelViewProjectionMatrix (the deprecated fixed-function builtin, set by
// the glMatrixMode/glLoadMatrixf calls we must remove for RenderDoc). The real
// modern path therefore needs a *uniform* MVP fed from the matrix tracker.
// This test proves that path reproduces legacy output pixel-for-pixel and
// exercises the tracker end-to-end on the GPU. It deliberately uses a minimal
// hand-rolled VAO+shader rather than the engine TypedRenderBuffer (whose
// bring-up needs globalRendering/configHandler and whose stock shader is not
// the target); swapping in TypedRenderBuffer-with-a-uniform-MVP-shader is the
// follow-on increment.
//
// To isolate the *backend* (FF pipeline vs shader) from matrix-source
// differences, the SAME CMatrix44f MVP is used for both sides: loaded into the
// FF stack for the legacy draw and set as the uniform for the modern draw.
//
// SDL2 + glad + the matrix tracker (header-only) + CMatrix44f. SKIPs with no
// GL context. Run under xvfb-run + LIBGL_ALWAYS_SOFTWARE=1 (llvmpipe).

#include "GLTestHarness.h"

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Matrix44f.h"

using namespace gltest;

namespace {

struct Vtx {
	float x, y, z;
	float r, g, b, a;
};

// ---- modern path: VAO + uniform-MVP GLSL 150 shader -----------------------

const char* kVS =
	"#version 150\n"
	"in vec3 aPos;\n"
	"in vec4 aCol;\n"
	"uniform mat4 uMVP;\n"
	"out vec4 vCol;\n"
	"void main() { vCol = aCol; gl_Position = uMVP * vec4(aPos, 1.0); }\n";

const char* kFS =
	"#version 150\n"
	"in vec4 vCol;\n"
	"out vec4 oColor;\n"
	"void main() { oColor = vCol; }\n";

GLuint CompileShader(GLenum type, const char* src)
{
	const GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = GL_FALSE;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = {0};
		glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
		INFO("shader compile failed: " << log);
		CHECK(ok == GL_TRUE);
	}
	return s;
}

struct ModernRenderer {
	GLuint prog = 0, vao = 0, vbo = 0;
	GLint uMVP = -1, aPos = -1, aCol = -1;
	bool ok = false;

	bool Init() {
		const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVS);
		const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFS);
		prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glBindFragDataLocation(prog, 0, "oColor");
		glLinkProgram(prog);
		glDeleteShader(vs);
		glDeleteShader(fs);

		GLint linked = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &linked);
		if (!linked) {
			char log[1024] = {0};
			glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
			INFO("program link failed: " << log);
			return false;
		}

		uMVP = glGetUniformLocation(prog, "uMVP");
		aPos = glGetAttribLocation(prog, "aPos");
		aCol = glGetAttribLocation(prog, "aCol");

		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		ok = (uMVP >= 0 && aPos >= 0 && aCol >= 0);
		return ok;
	}

	void Draw(const CMatrix44f& mvp, GLenum mode, const std::vector<Vtx>& v) {
		glUseProgram(prog);
		glUniformMatrix4fv(uMVP, 1, GL_FALSE, static_cast<const float*>(mvp));

		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vtx), v.data(), GL_STREAM_DRAW);

		glEnableVertexAttribArray(aPos);
		glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx),
			reinterpret_cast<void*>(0));
		glEnableVertexAttribArray(aCol);
		glVertexAttribPointer(aCol, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
			reinterpret_cast<void*>(3 * sizeof(float)));

		glDrawArrays(mode, 0, static_cast<GLsizei>(v.size()));

		glBindVertexArray(0);
		glUseProgram(0);
	}

	~ModernRenderer() {
		if (vbo)  glDeleteBuffers(1, &vbo);
		if (vao)  glDeleteVertexArrays(1, &vao);
		if (prog) glDeleteProgram(prog);
	}
};

// ---- legacy path: same MVP loaded into the FF stack, glBegin --------------

void LegacyDraw(const CMatrix44f& mvp, GLenum mode, const std::vector<Vtx>& v)
{
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(static_cast<const float*>(mvp));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glBegin(mode);
	for (const Vtx& x : v) {
		glColor4f(x.r, x.g, x.b, x.a);
		glVertex3f(x.x, x.y, x.z);
	}
	glEnd();
}

// quad as two triangles (CCW), single color
std::vector<Vtx> FlatQuad(float x0, float y0, float x1, float y1,
                          float r, float g, float b, float a)
{
	const Vtx tl{x0, y1, 0, r, g, b, a};
	const Vtx tr{x1, y1, 0, r, g, b, a};
	const Vtx br{x1, y0, 0, r, g, b, a};
	const Vtx bl{x0, y0, 0, r, g, b, a};
	return { bl, br, tr,  bl, tr, tl };
}

// the MVP both sides share, built via the Phase-1 tracker
CMatrix44f OrthoMVP()
{
	GLMatrixStateTracker tr;
	tr.SetMatrixMode(GL_PROJECTION);
	tr.LoadIdentity();
	tr.Ortho(0.0, kSize, 0.0, kSize, -1.0, 1.0);
	return tr.GetMatrix(GL_PROJECTION);
}

} // namespace


TEST_CASE("GLMatrixDrawCompare: modern shader can be built")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	ModernRenderer mr;
	CHECK(mr.Init());
}


TEST_CASE("GLMatrixDrawCompare: flat axis-aligned quad — legacy vs modern bit-exact")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const CMatrix44f mvp = OrthoMVP();
	const auto quad = FlatQuad(32, 32, 200, 160, 0.80f, 0.40f, 0.20f, 1.0f);

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDraw(mvp, GL_TRIANGLES, quad);
	});

	ModernRenderer mr;
	REQUIRE(mr.Init());
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		mr.Draw(mvp, GL_TRIANGLES, quad);
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.equal);          // same MVP, same positions, flat color => bit-exact
	CHECK(d.maxAbsDelta == 0);
}


TEST_CASE("GLMatrixDrawCompare: smooth-colored triangle — legacy vs modern within 1 LSB")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const CMatrix44f mvp = OrthoMVP();
	const std::vector<Vtx> tri = {
		{ 40,  40, 0, 1.0f, 0.0f, 0.0f, 1.0f },
		{ 210, 60, 0, 0.0f, 1.0f, 0.0f, 1.0f },
		{ 128, 210, 0, 0.0f, 0.0f, 1.0f, 1.0f },
	};

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDraw(mvp, GL_TRIANGLES, tri);
	});

	ModernRenderer mr;
	REQUIRE(mr.Init());
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		mr.Draw(mvp, GL_TRIANGLES, tri);
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1); // color interpolation may differ by <=1 LSB
}


TEST_CASE("GLMatrixDrawCompare: transformed quad (tracker translate+rotate) — within 1 LSB")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// MVP = ortho(P) * model(M), with M built from tracker Translate+Rotate.
	// Same combined matrix loaded into FF for legacy and as uniform for modern,
	// so any delta is purely backend (and rotation puts edges off-axis).
	GLMatrixStateTracker tr;
	tr.SetMatrixMode(GL_PROJECTION);
	tr.LoadIdentity();
	tr.Ortho(0.0, kSize, 0.0, kSize, -1.0, 1.0);
	const CMatrix44f P = tr.GetMatrix(GL_PROJECTION);

	tr.SetMatrixMode(GL_MODELVIEW);
	tr.LoadIdentity();
	tr.Translate(128.0f, 128.0f, 0.0f);
	tr.Rotate(20.0f, 0.0f, 0.0f, 1.0f);
	const CMatrix44f M = tr.GetMatrix(GL_MODELVIEW);

	const CMatrix44f mvp = P * M;
	const auto quad = FlatQuad(-70, -45, 70, 45, 0.30f, 0.65f, 0.90f, 1.0f);

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDraw(mvp, GL_TRIANGLES, quad);
	});

	ModernRenderer mr;
	REQUIRE(mr.Init());
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		mr.Draw(mvp, GL_TRIANGLES, quad);
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1); // rotated edges: <=1 LSB at the diagonal
}
