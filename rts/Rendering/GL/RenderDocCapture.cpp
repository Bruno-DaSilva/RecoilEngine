/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RenderDocCapture.h"

#include "Rendering/GlobalRendering.h"
#include "System/Log/ILog.h"

#include <SDL_syswm.h>

#ifndef _WIN32
	#include <dlfcn.h>
#else
	#include <windows.h>
#endif

#include "lib/renderdoc/renderdoc_app.h"

namespace {
	// RTLD_NOLOAD / GetModuleHandle: resolve the API only if RenderDoc already
	// injected itself into this process. Loading the library ourselves would
	// give a handle whose capture machinery was never hooked into the GL driver,
	// so TriggerCapture would silently do nothing.
	RENDERDOC_API_1_4_0* GetAPI()
	{
		static RENDERDOC_API_1_4_0* api = nullptr;
		static bool resolved = false;

		if (resolved)
			return api;

		resolved = true;

		pRENDERDOC_GetAPI getAPI = nullptr;
	#ifndef _WIN32
		if (void* lib = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD); lib != nullptr)
			getAPI = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(lib, "RENDERDOC_GetAPI"));
	#else
		if (HMODULE lib = GetModuleHandleA("renderdoc.dll"); lib != nullptr)
			getAPI = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(lib, "RENDERDOC_GetAPI"));
	#endif

		if (getAPI == nullptr)
			return nullptr;

		if (getAPI(eRENDERDOC_API_Version_1_4_0, reinterpret_cast<void**>(&api)) != 1)
			api = nullptr;

		return api;
	}
}

bool RenderDocCapture::TriggerNextFrame()
{
	RENDERDOC_API_1_4_0* api = GetAPI();

	if (api == nullptr) {
		LOG_L(L_WARNING, "[RenderDoc] not hosting this process -- launch via "
		                 "\"renderdoccmd capture <spring> ...\" to capture");
		return false;
	}

	int major = 0, minor = 0, patch = 0;
	api->GetAPIVersion(&major, &minor, &patch);

	// report the state BEFORE triggering: a second invocation later in the run
	// then says whether the first one actually produced a capture, which is the
	// only way to tell "RenderDoc declined" from "RenderDoc never saw a frame"
	const uint32_t numBefore = api->GetNumCaptures();
	char pathBuf[1024] = {0};
	uint32_t pathLen = sizeof(pathBuf);
	const uint32_t idx = (numBefore > 0) ? (numBefore - 1) : 0;
	const uint32_t got = (numBefore > 0) ? api->GetCapture(idx, pathBuf, &pathLen, nullptr) : 0;

	LOG_L(L_WARNING, "[RenderDoc] API %d.%d.%d, captures so far=%u%s%s, capturing=%d, template=\"%s\"",
	      major, minor, patch, numBefore,
	      (got != 0) ? ", last=" : "", (got != 0) ? pathBuf : "",
	      api->IsFrameCapturing(),
	      api->GetCaptureFilePathTemplate() ? api->GetCaptureFilePathTemplate() : "(null)");

	api->TriggerCapture();
	LOG_L(L_WARNING, "[RenderDoc] triggered: capturing the next frame");
	return true;
}

namespace {
	// RenderDoc's "device pointer" for GLX is the Display*, and the window
	// handle the X11 Window. Naming them explicitly removes any dependence on
	// RenderDoc having tracked an active window of its own.
	void GetGLXHandles(void*& device, void*& window)
	{
		device = nullptr;
		window = nullptr;

		SDL_Window* sdlWindow = (globalRendering != nullptr) ? globalRendering->GetWindow() : nullptr;
		if (sdlWindow == nullptr)
			return;

		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		if (!SDL_GetWindowWMInfo(sdlWindow, &info) || info.subsystem != SDL_SYSWM_X11)
			return;

		device = info.info.x11.display;
		window = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
	}
}

bool RenderDocCapture::BeginExplicit()
{
	RENDERDOC_API_1_4_0* api = GetAPI();

	if (api == nullptr) {
		LOG_L(L_WARNING, "[RenderDoc] not hosting this process");
		return false;
	}

	void* device = nullptr;
	void* window = nullptr;
	GetGLXHandles(device, window);

	if (device != nullptr)
		api->SetActiveWindow(device, window);

	// RenderDoc looks for a GL context current on the CALLING thread; an action
	// dispatched off the render thread finds none and the capture silently
	// never starts, which looks identical to "RenderDoc refused"
	SDL_GLContext curCtx = SDL_GL_GetCurrentContext();

	// Which library actually owns the GL entry points? RenderDoc answers
	// glIsEnabled(GL_DEBUG_TOOL_EXT) from an early-out needing no wrapped
	// context, so its presence proves nothing about hooking; the owner of a
	// real entry point does. Logged once -- it is a property of the process.
	static bool loggedOwners = false;
	if (!loggedOwners) {
		loggedOwners = true;
		for (const char* fn: {"glXMakeCurrent", "glXSwapBuffers", "glXCreateContextAttribsARB"}) {
			void* p = reinterpret_cast<void*>(SDL_GL_GetProcAddress(fn));
			Dl_info info;
			if (p != nullptr && dladdr(p, &info) != 0 && info.dli_fname != nullptr)
				LOG_L(L_WARNING, "[RenderDoc] %-28s -> %s", fn, info.dli_fname);
			else
				LOG_L(L_WARNING, "[RenderDoc] %-28s -> %p (unresolved)", fn, p);
		}
	}

	api->StartFrameCapture(device, window);
	LOG_L(L_WARNING, "[RenderDoc] explicit capture STARTED (device=%p window=%p curGLContext=%p capturing=%d)",
	      device, window, curCtx, api->IsFrameCapturing());
	return true;
}

bool RenderDocCapture::EndExplicit()
{
	RENDERDOC_API_1_4_0* api = GetAPI();

	if (api == nullptr) {
		LOG_L(L_WARNING, "[RenderDoc] not hosting this process");
		return false;
	}

	void* device = nullptr;
	void* window = nullptr;
	GetGLXHandles(device, window);

	const uint32_t ok = api->EndFrameCapture(device, window);
	const uint32_t num = api->GetNumCaptures();

	char pathBuf[1024] = {0};
	uint32_t pathLen = sizeof(pathBuf);
	const uint32_t got = (num > 0) ? api->GetCapture(num - 1, pathBuf, &pathLen, nullptr) : 0;

	LOG_L(L_WARNING, "[RenderDoc] explicit capture ENDED ok=%u, captures=%u%s%s",
	      ok, num, (got != 0) ? ", file=" : "", (got != 0) ? pathBuf : "");
	return ok != 0;
}
