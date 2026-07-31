/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RenderDocCapture.h"

#include "System/Log/ILog.h"

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
