// Isolate ONE variable: does RenderDoc capture a GL context that differs from
// the engine's only by profile mask? Same SDL2, same driver, same trigger path.
//   ./rdoc_profile_test compat|core
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <initializer_list>
#include <thread>
#include <vector>
#include <atomic>

#include "renderdoc_app.h"
#ifdef WITH_MIMALLOC
#include <mimalloc-new-delete.h>
#endif

static RENDERDOC_API_1_4_0* GetRDoc(void)
{
	void* lib = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
	if (!lib) { printf("[test] renderdoc not hosting\n"); return NULL; }
	pRENDERDOC_GetAPI getAPI = (pRENDERDOC_GetAPI)dlsym(lib, "RENDERDOC_GetAPI");
	if (!getAPI) { printf("[test] no RENDERDOC_GetAPI\n"); return NULL; }
	RENDERDOC_API_1_4_0* api = NULL;
	if (getAPI(eRENDERDOC_API_Version_1_4_0, (void**)&api) != 1) return NULL;
	return api;
}

int main(int argc, char** argv)
{
	const int core = (argc > 1 && strcmp(argv[1], "core") == 0);

	// argv[3] == "threads": spawn a thread pool BEFORE any GL init, as the
	// engine does, in case RenderDoc's per-thread context tracking is upset
	std::vector<std::thread> pool;
	std::atomic<bool> stop{false};
	if (argc > 3 && strcmp(argv[3], "threads") == 0) {
		for (int i = 0; i < 32; ++i)
			pool.emplace_back([&stop]{ while (!stop.load()) SDL_Delay(5); });
		printf("[test] spawned %zu threads before GL init\n", pool.size()); fflush(stdout);
	}

	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	// the engine's exact request, bar the profile mask
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		core ? SDL_GL_CONTEXT_PROFILE_CORE : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, core ? 3 : 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, core ? 3 : 0);

	// argv[2] == "engine" -> the engine's exact window: borderless, resizable,
	// desktop-sized
	const int engineWin = (argc > 2 && strcmp(argv[2], "engine") == 0);
	Uint32 flags = SDL_WINDOW_OPENGL;
	int w = 320, h = 240;
	if (engineWin) {
		flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS;
		w = 2560; h = 1440;
	}
	printf("[test] window flags=0x%x %dx%d\n", flags, w, h); fflush(stdout);
	SDL_Window* win = SDL_CreateWindow("rdoc profile test", 0, 0, w, h, flags);
	if (!win) { printf("[test] no window: %s\n", SDL_GetError()); return 1; }
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	if (!ctx) { printf("[test] no context: %s\n", SDL_GetError()); return 1; }

	printf("[test] requested %s -> GL_VERSION = %s\n", core ? "core" : "compat",
	       (const char*)((const unsigned char*(*)(unsigned int))SDL_GL_GetProcAddress("glGetString"))(0x1F02));
	fflush(stdout);

	// argv[4] == "detach": mimic the engine's LoadLock, which repeatedly
	// detaches the GL context (SDL_GL_MakeCurrent(win, NULL)) and re-attaches it
	if (argc > 4 && strcmp(argv[4], "detach") == 0) {
		for (int i = 0; i < 5; ++i) {
			SDL_GL_MakeCurrent(win, NULL);
			SDL_GL_MakeCurrent(win, ctx);
		}
		printf("[test] did 5 detach/reattach cycles\n"); fflush(stdout);
	}

	RENDERDOC_API_1_4_0* rdoc = GetRDoc();

	for (const char* fn : {"glXMakeCurrent", "glXSwapBuffers", "glXCreateContextAttribsARB"}) {
		void* p2 = (void*)SDL_GL_GetProcAddress(fn);
		Dl_info di;
		printf("[test] %-28s -> %s\n", fn,
		       (p2 && dladdr(p2, &di) && di.dli_fname) ? di.dli_fname : "(unresolved)");
	}
	fflush(stdout);

	void (*glClearColor)(float, float, float, float) = (void(*)(float,float,float,float))SDL_GL_GetProcAddress("glClearColor");
	void (*glClear)(unsigned int) = (void(*)(unsigned int))SDL_GL_GetProcAddress("glClear");

	for (int f = 0; f < 120; ++f) {
		if (f == 30 && rdoc) {
			printf("[test] frame %d: TriggerCapture (captures so far=%u)\n", f, rdoc->GetNumCaptures());
			fflush(stdout);
			rdoc->TriggerCapture();
		}
		glClearColor((f % 60) / 60.0f, 0.2f, 0.4f, 1.0f);
		glClear(0x00004000); // GL_COLOR_BUFFER_BIT
		SDL_GL_SwapWindow(win);
		SDL_Delay(8);
	}

	if (rdoc) {
		printf("[test] RESULT: captures = %u\n", rdoc->GetNumCaptures());
		fflush(stdout);
	}
	stop.store(true);
	for (auto& t : pool) t.join();
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
