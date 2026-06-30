/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 offscreen A/B render harness for the BAR modern-GL migration
// (see doc/bar-gl4-immediate-mode-inventory.md).
//
// This file is the *harness and its self-validation*, not yet the real
// legacy-vs-new-backend primitive cases. Per the doc ("validate the harness
// before trusting it"), a comparator that gates rendering equivalence must be
// proven to BOTH confirm and deny before we rely on it:
//   - positive control: the same legacy primitive rendered twice is
//     byte-identical (proves the offscreen context + FBO + glReadPixels are
//     deterministic),
//   - negative control: two deliberately-different renders compare UNEQUAL
//     (proves the comparator is not a no-op that always passes).
//
// It deliberately depends only on SDL2 + glad + a current GL context: no
// engine globals (globalRendering/configHandler), no CFBO, no
// TypedRenderBuffer. The engine-backed path (CFBO + TypedRenderBuffer "new
// backend" vs raw glBegin "legacy") lands in a later increment, once the
// immediate-mode emitter is factored lua_State-free; the context bring-up,
// the comparator, the readback, the SKIP behavior and the CMake/xvfb recipe
// all carry forward unchanged.
//
// CI recipe: xvfb-run -a -s "-screen 0 64x64x24" env LIBGL_ALWAYS_SOFTWARE=1
//            ctest -R testGLImmediateCompare   (Mesa llvmpipe, deterministic).
// If no GL context can be created (headless CI box), every case SKIPs so the
// suite stays green.

#include <glad/glad.h>
#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <functional>

#include <catch_amalgamated.hpp>


namespace {

constexpr int kSize = 256; // square FBO edge

// ---- offscreen GL context (created once, reused) ---------------------------

struct GLContextHolder {
	SDL_Window*   window = nullptr;
	SDL_GLContext context = nullptr;
	bool ok = false;

	GLContextHolder() { ok = Create(); }

	~GLContextHolder() {
		if (context) SDL_GL_DeleteContext(context);
		if (window)  SDL_DestroyWindow(window);
		if (didInit) SDL_Quit();
	}

	bool didInit = false;

	bool TryCreate(int major, int minor, bool setVersion) {
		if (setVersion) {
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
		}
		// compatibility profile: required so both glBegin (legacy oracle) and
		// GLSL (future new backend) work in the same context.
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

		window = SDL_CreateWindow("recoil-gl-immediate-compare",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
		if (window == nullptr)
			return false;

		context = SDL_GL_CreateContext(window);
		if (context == nullptr) {
			SDL_DestroyWindow(window);
			window = nullptr;
			return false;
		}
		return true;
	}

	bool Create() {
		if (SDL_Init(SDL_INIT_VIDEO) != 0)
			return false;
		didInit = true;

		// llvmpipe/desktop give a high compat context; if an explicit 3.2
		// request is refused, fall back to letting the driver pick.
		if (!TryCreate(3, 2, true) && !TryCreate(0, 0, false))
			return false;

		if (SDL_GL_MakeCurrent(window, context) != 0)
			return false;
		if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0)
			return false;

		return true;
	}
};

GLContextHolder& GL()
{
	static GLContextHolder holder;
	return holder;
}

// ---- raw-GL offscreen render target (RGBA8 + depth) -----------------------

struct RenderTarget {
	GLuint fbo = 0, color = 0, depth = 0;
	int w = 0, h = 0;

	bool Make(int width, int height) {
		w = width; h = height;

		glGenTextures(1, &color);
		glBindTexture(GL_TEXTURE_2D, color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glGenRenderbuffers(1, &depth);
		glBindRenderbuffer(GL_RENDERBUFFER, depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

		const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return complete;
	}

	void Bind() {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, w, h);
	}

	std::vector<uint8_t> ReadBack() {
		std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return buf;
	}

	~RenderTarget() {
		if (fbo)   glDeleteFramebuffers(1, &fbo);
		if (color) glDeleteTextures(1, &color);
		if (depth) glDeleteRenderbuffers(1, &depth);
	}
};

// ---- comparator (the thing the controls validate) -------------------------

struct DiffResult {
	bool   equal = true;
	size_t firstDiffByte = 0;
	int    maxAbsDelta = 0;
	size_t diffBytes = 0;
};

DiffResult Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	DiffResult r;
	if (a.size() != b.size()) {
		r.equal = false;
		r.maxAbsDelta = 255;
		r.diffBytes = (a.size() > b.size()) ? a.size() : b.size();
		return r;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		const int d = std::abs(int(a[i]) - int(b[i]));
		if (d != 0) {
			if (r.equal) { r.equal = false; r.firstDiffByte = i; }
			++r.diffBytes;
			if (d > r.maxAbsDelta) r.maxAbsDelta = d;
		}
	}
	return r;
}

// ---- legacy (fixed-function / immediate-mode) render helpers --------------

void SetupOrtho(int w, int h)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, w, 0.0, h, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

void ClearTo(float r, float g, float b, float a)
{
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void DrawRectLegacy(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
	glColor4f(r, g, b, a);
	glBegin(GL_QUADS);
		glVertex2f(x0, y0);
		glVertex2f(x1, y0);
		glVertex2f(x1, y1);
		glVertex2f(x0, y1);
	glEnd();
}

// Render `scene` into a fresh target and return the RGBA8 readback.
std::vector<uint8_t> RenderToBuffer(const std::function<void()>& scene)
{
	RenderTarget rt;
	REQUIRE(rt.Make(kSize, kSize));
	rt.Bind();
	SetupOrtho(kSize, kSize);
	scene();
	glFinish();
	return rt.ReadBack();
}

bool EnsureGL()
{
	return GL().ok;
}

} // namespace


TEST_CASE("GLImmediateCompare: offscreen context + FBO readback works")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// clear to exact bytes (255,0,0,255) -> no rounding ambiguity
	const auto buf = RenderToBuffer([] { ClearTo(1.0f, 0.0f, 0.0f, 1.0f); });

	REQUIRE(buf.size() == size_t(kSize) * kSize * 4);
	// sample the center texel
	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(buf[c + 0] == 255);
	CHECK(buf[c + 1] == 0);
	CHECK(buf[c + 2] == 0);
	CHECK(buf[c + 3] == 255);
}


TEST_CASE("GLImmediateCompare: positive control — identical renders are byte-identical")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const auto scene = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		DrawRectLegacy(32, 32, 200, 160, 1.0f, 0.5f, 0.25f, 1.0f);
	};

	const auto a = RenderToBuffer(scene);
	const auto b = RenderToBuffer(scene);

	const DiffResult d = Compare(a, b);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.equal);
	CHECK(d.maxAbsDelta == 0);
	CHECK(d.diffBytes == 0);
}


TEST_CASE("GLImmediateCompare: negative control — different renders compare UNEQUAL")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const auto sceneA = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		DrawRectLegacy(32, 32, 200, 160, 1.0f, 0.5f, 0.25f, 1.0f);
	};
	// same geometry, shifted right + recolored: must register as different.
	const auto sceneB = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		DrawRectLegacy(48, 32, 216, 160, 0.25f, 1.0f, 0.5f, 1.0f);
	};

	const auto a = RenderToBuffer(sceneA);
	const auto b = RenderToBuffer(sceneB);

	const DiffResult d = Compare(a, b);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK_FALSE(d.equal);    // the comparator MUST be able to deny
	CHECK(d.maxAbsDelta > 0);
	CHECK(d.diffBytes > 0);
}


TEST_CASE("GLImmediateCompare: comparator denies a single-pixel difference")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// guard against a comparator that only catches large/global differences:
	// a 1-texel delta must still register.
	auto a = RenderToBuffer([] { ClearTo(0.0f, 0.0f, 0.0f, 1.0f); });
	auto b = a;
	b[(size_t(10) * kSize + 10) * 4 + 1] ^= 0xFF; // flip one green byte

	const DiffResult d = Compare(a, b);
	CHECK_FALSE(d.equal);
	CHECK(d.diffBytes == 1);
	CHECK(d.maxAbsDelta == 255);
}
