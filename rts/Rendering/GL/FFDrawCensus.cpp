/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFDrawCensus.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

#if defined(__unix__)
	#include <dlfcn.h>
	#include <execinfo.h>
	#define FFDRAW_CENSUS_SUPPORTED 1
#else
	#define FFDRAW_CENSUS_SUPPORTED 0
#endif

CONFIG(bool, GLFFDrawCensus).defaultValue(false).safemodeValue(false)
	.description("Development instrument: count the draws that still run with no program bound (the fixed-function pipeline), keyed by call site, and report them at shutdown. Costs a backtrace per such draw.");

#if FFDRAW_CENSUS_SUPPORTED

namespace {
	// Every entry point that can rasterize, plus glUseProgram to track what is
	// bound. X(gladName, (params), (args)); all of these return void.
	#define FFDRAW_ENTRIES(X) \
		X(glDrawArrays,               (GLenum a, GLint b, GLsizei c), (a, b, c)) \
		X(glDrawElements,             (GLenum a, GLsizei b, GLenum c, const void* d), (a, b, c, d)) \
		X(glDrawRangeElements,        (GLenum a, GLuint b, GLuint c, GLsizei d, GLenum e, const void* f), (a, b, c, d, e, f)) \
		X(glDrawElementsBaseVertex,   (GLenum a, GLsizei b, GLenum c, const void* d, GLint e), (a, b, c, d, e)) \
		X(glDrawArraysInstanced,      (GLenum a, GLint b, GLsizei c, GLsizei d), (a, b, c, d)) \
		X(glDrawElementsInstanced,    (GLenum a, GLsizei b, GLenum c, const void* d, GLsizei e), (a, b, c, d, e)) \
		X(glMultiDrawArrays,          (GLenum a, const GLint* b, const GLsizei* c, GLsizei d), (a, b, c, d)) \
		X(glMultiDrawElements,        (GLenum a, const GLsizei* b, GLenum c, const void* const* d, GLsizei e), (a, b, c, d, e)) \
		X(glDrawArraysIndirect,       (GLenum a, const void* b), (a, b)) \
		X(glDrawElementsIndirect,     (GLenum a, GLenum b, const void* c), (a, b, c)) \
		X(glMultiDrawElementsIndirect,(GLenum a, GLenum b, const void* c, GLsizei d, GLsizei e), (a, b, c, d, e)) \
		X(glBegin,                    (GLenum a), (a)) \
		X(glRectf,                    (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
		X(glCallList,                 (GLuint a), (a)) \
		X(glCallLists,                (GLsizei a, GLenum b, const void* c), (a, b, c)) \
		X(glDrawPixels,               (GLsizei a, GLsizei b, GLenum c, GLenum d, const void* e), (a, b, c, d, e)) \
		X(glBitmap,                   (GLsizei a, GLsizei b, GLfloat c, GLfloat d, GLfloat e, GLfloat f, const GLubyte* g), (a, b, c, d, e, f, g))

	#define FFDRAW_DECL_ORIG(name, params, args) decltype(glad_##name) orig_##name = nullptr;
	struct OrigPtrs {
		FFDRAW_ENTRIES(FFDRAW_DECL_ORIG)
		decltype(glad_glUseProgram) orig_glUseProgram = nullptr;
	} orig;
	#undef FFDRAW_DECL_ORIG

	// What glUseProgram last bound. Tracked rather than queried: glGetIntegerv on
	// every draw would dominate the run, and the wrapper below sees every bind
	// (glad has one pointer, so engine and Lua binds both come through it).
	uint32_t boundProgram = 0;

	constexpr int FRAMES = 6;

	struct Site {
		const char* fn;
		void* frames[FRAMES];
		uint64_t count;
	};
	std::vector<Site> sites;

	void Note(const char* fn)
	{
		if (boundProgram != 0)
			return;

		void* frames[FRAMES + 2];
		const int n = backtrace(frames, FRAMES + 2);

		// frames[0] is Note, frames[1] the wrapper; the caller starts at 2
		Site probe = { fn, {}, 1 };
		for (int i = 0; i < FRAMES; ++i)
			probe.frames[i] = (i + 2 < n) ? frames[i + 2] : nullptr;

		for (Site& s : sites) {
			if (s.fn != fn || std::memcmp(s.frames, probe.frames, sizeof(probe.frames)) != 0)
				continue;
			++s.count;
			return;
		}

		if (sites.size() < 4096)
			sites.push_back(probe);
	}

	#define FFDRAW_DEFINE(name, params, args) \
		void APIENTRY Cen_##name params { \
			Note(#name); \
			orig.orig_##name args; \
		}
	FFDRAW_ENTRIES(FFDRAW_DEFINE)
	#undef FFDRAW_DEFINE

	void APIENTRY Cen_glUseProgram(GLuint program)
	{
		boundProgram = program;
		orig.orig_glUseProgram(program);
	}

	bool installed = false;
}

void GL::FFDrawCensus::Install()
{
	if (installed || !configHandler->GetBool("GLFFDrawCensus"))
		return;

	installed = true;

	#define FFDRAW_SWAP(name, params, args) \
		orig.orig_##name = glad_##name; \
		glad_##name = &Cen_##name;
	FFDRAW_ENTRIES(FFDRAW_SWAP)
	#undef FFDRAW_SWAP

	orig.orig_glUseProgram = glad_glUseProgram;
	glad_glUseProgram = &Cen_glUseProgram;

	LOG_L(L_WARNING, "[FFDrawCensus] ACTIVE: counting draws issued with no program bound");
}

void GL::FFDrawCensus::Dump()
{
	if (!installed)
		return;

	std::stable_sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.count > b.count; });

	uint64_t total = 0;
	for (const Site& s : sites)
		total += s.count;

	LOG_L(L_WARNING, "[FFDrawCensus] %d distinct fixed-function draw sites, %llu draws total",
		static_cast<int>(sites.size()), static_cast<unsigned long long>(total));

	// module+offset, the same shape rdoc_site_census.sh already resolves
	for (const Site& s : sites) {
		std::string trace;
		for (void* f : s.frames) {
			Dl_info info;
			if (f == nullptr || dladdr(f, &info) == 0 || info.dli_fname == nullptr)
				continue;

			char buf[512];
			snprintf(buf, sizeof(buf), " %s+0x%lx", info.dli_fname,
				static_cast<unsigned long>(static_cast<const char*>(f) - static_cast<const char*>(info.dli_fbase)));
			trace += buf;
		}
		LOG_L(L_WARNING, "[FFDRAW-SITE] %llu %s%s", static_cast<unsigned long long>(s.count), s.fn, trace.c_str());
	}
}

#else // FFDRAW_CENSUS_SUPPORTED

void GL::FFDrawCensus::Install() {}
void GL::FFDrawCensus::Dump() {}

#endif
