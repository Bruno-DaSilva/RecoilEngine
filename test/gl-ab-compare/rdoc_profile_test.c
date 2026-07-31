// Isolate ONE variable: does RenderDoc capture a GL context that differs from
// the engine's only by profile mask? Same SDL2, same driver, same trigger path.
//   ./rdoc_profile_test compat|core
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "renderdoc_app.h"

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

	SDL_Window* win = SDL_CreateWindow("rdoc profile test", 0, 0, 320, 240, SDL_WINDOW_OPENGL);
	if (!win) { printf("[test] no window: %s\n", SDL_GetError()); return 1; }
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	if (!ctx) { printf("[test] no context: %s\n", SDL_GetError()); return 1; }

	printf("[test] requested %s -> GL_VERSION = %s\n", core ? "core" : "compat",
	       (const char*)((const unsigned char*(*)(unsigned int))SDL_GL_GetProcAddress("glGetString"))(0x1F02));
	fflush(stdout);

	RENDERDOC_API_1_4_0* rdoc = GetRDoc();

	void (*glClearColor)(float, float, float, float) = SDL_GL_GetProcAddress("glClearColor");
	void (*glClear)(unsigned int) = SDL_GL_GetProcAddress("glClear");

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
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
