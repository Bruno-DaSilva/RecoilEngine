// Mesa bug: the GLSL disk cache is not keyed on GL profile.
//
// A "#version 150 compatibility" shader compiled in a COMPATIBILITY context is
// served back from the shader cache to a CORE context, where the front end would
// otherwise reject it with "the compatibility profile is not supported". The
// cached compile reports success, and linking it as a separable program then
// SEGFAULTS inside libgallium.
//
// This matters here because it is what a RenderDoc capture of the engine walks
// into. RenderDoc replays GL captures on a core-profile context and rebuilds
// every captured shader as a separable program to reflect it; the capturing run
// has just populated the cache with the engine's compatibility shaders from its
// own compatibility context. So the capture opens, crashes, and looks for all
// the world like a bad capture -- it is not. With MESA_SHADER_CACHE_DISABLE=true
// the same file opens and replays fine, which is why run_rdoc_capture.sh sets it.
//
// The engine-side way out is to stop declaring the compatibility profile in
// shaders that no longer use it, which the FF builtin rewrite makes possible but
// does not yet do: the rewrite keeps the builtin compiled in behind a selector
// uniform so the A/B gate can compare the two sources within one build, and that
// reference is what forces the profile declaration.
//
// build: cc -O0 -o mesa_compat_cache_repro mesa_compat_cache_repro.c -lGL -lX11
// run:   ./mesa_compat_cache_repro          # compat context first: populates cache
//        ./mesa_compat_cache_repro core     # core context: cache hit, then crash
//        MESA_SHADER_CACHE_DISABLE=true ./mesa_compat_cache_repro core   # clean reject

#define GL_GLEXT_PROTOTYPES
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <string.h>

// Anything unique enough not to collide with a cache entry from another run; the
// point of the shader is only that it is a valid compatibility-profile one.
static const char* SRC =
	"#version 150 compatibility\n"
	"#extension GL_ARB_explicit_attrib_location : enable\n"
	"layout (location = 0) in vec3 pos;\n"
	"out Data { vec4 vCol; };\n"
	"uniform bool sel; uniform mat4 m;\n"
	"void main() { vCol = vec4(0.5); gl_Position = (sel ? m : gl_ModelViewProjectionMatrix) * vec4(pos, 1.0); }\n";

int main(int argc, char** argv)
{
	const int core = (argc > 1) && strcmp(argv[1], "core") == 0;

	Display* dpy = XOpenDisplay(NULL);

	if (dpy == NULL) {
		printf("no display\n");
		return 2;
	}

	int fbAttribs[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
	                    GLX_DOUBLEBUFFER, False, None };
	int numConfigs = 0;
	GLXFBConfig* cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), fbAttribs, &numConfigs);

	if (cfgs == NULL || numConfigs == 0) {
		printf("no fbconfig\n");
		return 2;
	}

	int ctxAttribs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, core ? 4 : 3,
	                     GLX_CONTEXT_MINOR_VERSION_ARB, core ? 6 : 2,
	                     GLX_CONTEXT_PROFILE_MASK_ARB, core ? GLX_CONTEXT_CORE_PROFILE_BIT_ARB
	                                                        : GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
	                     None };
	GLXContext ctx = glXCreateContextAttribsARB(dpy, cfgs[0], NULL, True, ctxAttribs);

	if (ctx == NULL) {
		printf("no context\n");
		return 2;
	}

	int pbAttribs[] = { GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None };
	GLXPbuffer pb = glXCreatePbuffer(dpy, cfgs[0], pbAttribs);

	if (!glXMakeContextCurrent(dpy, pb, pb, ctx)) {
		printf("could not make the context current\n");
		return 2;
	}

	printf("%s\n", (const char*)glGetString(GL_VERSION));

	GLuint sh = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(sh, 1, &SRC, NULL);
	glCompileShader(sh);

	GLint ok = 0;
	char log[2048] = { 0 };
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
	printf("compile: %s  %s\n", ok ? "ok" : "rejected", log);

	if (!ok) {
		printf("(this is the CORRECT result for a core context -- the cache was cold or off)\n");
		return 1;
	}

	GLuint prog = glCreateProgram();
	glProgramParameteri(prog, GL_PROGRAM_SEPARABLE, GL_TRUE);
	glAttachShader(prog, sh);

	printf("linking as separable...\n");
	fflush(stdout);
	glLinkProgram(prog);

	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	printf("link: %s\n", ok ? "ok" : "failed");
	return ok ? 0 : 1;
}
