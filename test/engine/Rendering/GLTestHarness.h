/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Shared offscreen-GL test harness for the BAR modern-GL migration Tier-2
// pixel-equivalence tests (see doc/bar-gl4-immediate-mode-inventory.md).
//
// Header-only: a hidden SDL2 compatibility GL context (created once, reused),
// a raw-GL RGBA8 + depth render target, the A/B comparator, and a couple of
// legacy fixed-function render helpers. Depends only on SDL2 + glad — no
// engine globals. Consumers: testGLImmediateCompare (harness controls) and
// testGLMatrixDrawCompare (modern uniform-MVP vs legacy glBegin).
//
// NOTE: include <glad/glad.h> (here) NOT the engine's myGL.h, which #errors
// under UNIT_TEST (this whole test dir is built with -DUNIT_TEST).

#ifndef TEST_GL_TEST_HARNESS_H
#define TEST_GL_TEST_HARNESS_H

#include <glad/glad.h>
#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <functional>

#include <catch_amalgamated.hpp>

namespace gltest {

inline constexpr int kSize = 256; // square FBO edge

// ---- offscreen GL context (created once, reused) ---------------------------

struct GLContextHolder {
	SDL_Window*   window = nullptr;
	SDL_GLContext context = nullptr;
	bool didInit = false;
	bool ok = false;

	GLContextHolder() { ok = Create(); }

	~GLContextHolder() {
		if (context) SDL_GL_DeleteContext(context);
		if (window)  SDL_DestroyWindow(window);
		if (didInit) SDL_Quit();
	}

	bool TryCreate(int major, int minor, bool setVersion) {
		if (setVersion) {
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
		}
		// compatibility profile: both glBegin (legacy oracle) and GLSL (modern
		// path) must work in the same context.
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

		window = SDL_CreateWindow("recoil-gl-test",
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

		// GLSL 150 needs a >=3.2 context; if an explicit 3.2 request is
		// refused, fall back to letting the driver pick.
		if (!TryCreate(3, 2, true) && !TryCreate(0, 0, false))
			return false;

		if (SDL_GL_MakeCurrent(window, context) != 0)
			return false;
		if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0)
			return false;

		return true;
	}
};

inline GLContextHolder& GL()
{
	static GLContextHolder holder;
	return holder;
}

inline bool EnsureGL() { return GL().ok; }

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

// Render `scene` (which is responsible for its own clear + matrix + draw) into
// a fresh target and return the RGBA8 readback.
inline std::vector<uint8_t> RenderToBuffer(const std::function<void()>& scene)
{
	RenderTarget rt;
	REQUIRE(rt.Make(kSize, kSize));
	rt.Bind();
	scene();
	glFinish();
	return rt.ReadBack();
}

// ---- comparator (the thing the controls validate) -------------------------

struct DiffResult {
	bool   equal = true;
	size_t firstDiffByte = 0;
	int    maxAbsDelta = 0;
	size_t diffBytes = 0;
};

inline DiffResult Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
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

inline void ClearTo(float r, float g, float b, float a)
{
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

inline void SetupOrtho(int w, int h)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, w, 0.0, h, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

inline void DrawRectLegacy(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
	glColor4f(r, g, b, a);
	glBegin(GL_QUADS);
		glVertex2f(x0, y0);
		glVertex2f(x1, y0);
		glVertex2f(x1, y1);
		glVertex2f(x0, y1);
	glEnd();
}

} // namespace gltest

#endif // TEST_GL_TEST_HARNESS_H
