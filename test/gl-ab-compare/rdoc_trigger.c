// Trigger a real RenderDoc frame capture from inside the engine process.
//
// The offender meter (run_rdoc_offenders.sh) answers "would RenderDoc refuse
// this frame", which is the burn-down score, but it never produces a .rdc: the
// patched librenderdoc it uses is a headless meter build, and a capture still
// has to be ASKED for. Normally that is the UI or F12; neither exists here, so
// this shim asks through RenderDoc's in-application API instead.
//
// It is a shim rather than an engine change on purpose. TriggerCapture has to
// be called from inside the process being captured, but nothing about it needs
// to be in the engine -- LD_PRELOAD puts it there for one run and leaves no
// trace, and an engine-side debug action would be a permanent dependency on a
// capture tool for a thing only this harness does.
//
// Preload order matters: librenderdoc.so must come FIRST so that it, not this
// library, wins the GL entry points it hooks. This one only needs the symbol
// RENDERDOC_GetAPI to be somewhere in the global scope by the time its thread
// runs, which preloading guarantees.
//
// Timing. A capture is worth having only if it lands on a representative frame,
// so the thread waits for a marker line to appear in the engine's infolog (the
// game is up) and then a further delay (the game is actually playing) before
// triggering. Wall-clock alone would be at the mercy of load time, which varies
// by minutes across cold and warm content caches.
//
// build:
//   cc -shared -fPIC -O2 -I<renderdoc-src>/renderdoc/api/app
//      -o rdoc_trigger.so rdoc_trigger.c -lpthread
//
// env:
//   RDOC_TRIGGER_INFOLOG   file to watch for the marker (required for marker mode)
//   RDOC_TRIGGER_MARKER    substring meaning "the game is up"
//   RDOC_TRIGGER_DELAY     seconds to wait after the marker           (default 15)
//   RDOC_TRIGGER_FRAMES    consecutive frames to capture              (default 1)
//   RDOC_TRIGGER_CAPFILE   capture path template
//   RDOC_TRIGGER_WAIT      seconds to wait for the capture to appear  (default 120)

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "renderdoc_app.h"

#define LOG(...) do { fprintf(stderr, "[rdoc-trigger] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

static int EnvInt(const char* name, int fallback)
{
	const char* v = getenv(name);
	return (v != NULL && *v != '\0') ? atoi(v) : fallback;
}

static void SleepMs(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

// Poll rather than inotify: the engine truncates and rewrites the infolog during
// startup, and a watch on the old inode would simply never fire again.
static int WaitForMarker(const char* path, const char* marker, int timeoutSec)
{
	char buf[64 * 1024];

	for (int waited = 0; waited < timeoutSec * 10; ++waited) {
		FILE* f = fopen(path, "rb");

		if (f != NULL) {
			// The marker is a startup line, so it stays within the first chunk;
			// reading the whole file every 100ms would be the more expensive
			// thing here by far.
			while (fgets(buf, sizeof(buf), f) != NULL) {
				if (strstr(buf, marker) != NULL) {
					fclose(f);
					return 1;
				}
			}

			fclose(f);
		}

		SleepMs(100);
	}

	return 0;
}

static void* TriggerThread(void* unused)
{
	(void)unused;

	pRENDERDOC_GetAPI getAPI = (pRENDERDOC_GetAPI)dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI");

	if (getAPI == NULL) {
		LOG("FAIL: RENDERDOC_GetAPI not in the global scope -- is librenderdoc.so preloaded BEFORE this library?");
		return NULL;
	}

	// Descending, because the struct is append-only: asking for an older version
	// against a newer library is safe, the reverse is not.
	static const RENDERDOC_Version versions[] = {
		eRENDERDOC_API_Version_1_7_0, eRENDERDOC_API_Version_1_6_0,
		eRENDERDOC_API_Version_1_5_0, eRENDERDOC_API_Version_1_4_2,
		eRENDERDOC_API_Version_1_1_2,
	};

	RENDERDOC_API_1_7_0* rdoc = NULL;

	for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]) && rdoc == NULL; ++i)
		getAPI(versions[i], (void**)&rdoc);

	if (rdoc == NULL) {
		LOG("FAIL: RENDERDOC_GetAPI refused every version this shim knows");
		return NULL;
	}

	{
		int major = 0, minor = 0, patch = 0;
		rdoc->GetAPIVersion(&major, &minor, &patch);
		LOG("in-application API %d.%d.%d", major, minor, patch);
	}

	const char* capfile = getenv("RDOC_TRIGGER_CAPFILE");

	if (capfile != NULL && *capfile != '\0') {
		rdoc->SetCaptureFilePathTemplate(capfile);
		LOG("capture path template: %s", rdoc->GetCaptureFilePathTemplate());
	}

	const char* infolog = getenv("RDOC_TRIGGER_INFOLOG");
	const char* marker  = getenv("RDOC_TRIGGER_MARKER");

	if (infolog != NULL && marker != NULL && *infolog != '\0' && *marker != '\0') {
		LOG("waiting for \"%s\" in %s", marker, infolog);

		if (!WaitForMarker(infolog, marker, 600)) {
			LOG("FAIL: marker never appeared -- not triggering, a capture of the loading screen would not mean anything");
			return NULL;
		}

		LOG("marker seen");
	}

	const int delay = EnvInt("RDOC_TRIGGER_DELAY", 15);
	LOG("waiting %ds for the frame to become representative", delay);
	SleepMs((long)delay * 1000);

	const int frames = EnvInt("RDOC_TRIGGER_FRAMES", 1);
	const uint32_t before = rdoc->GetNumCaptures();

	LOG("triggering a %d-frame capture (captures so far: %u)", frames, before);

	if (frames > 1)
		rdoc->TriggerMultiFrameCapture((uint32_t)frames);
	else
		rdoc->TriggerCapture();

	const int wait = EnvInt("RDOC_TRIGGER_WAIT", 120);

	for (int waited = 0; waited < wait * 10; ++waited) {
		if (rdoc->GetNumCaptures() > before)
			break;

		SleepMs(100);
	}

	const uint32_t after = rdoc->GetNumCaptures();

	if (after <= before) {
		// The interesting failure. RenderDoc refuses silently when the context
		// has used a function on its unsupported list, which is the whole thing
		// the burn-down exists to prevent, so say so rather than just exiting.
		LOG("FAIL: no capture was produced. RenderDoc accepted the trigger and then declined,");
		LOG("FAIL: which is what it does when the context used an unsupported function.");
		return NULL;
	}

	for (uint32_t i = before; i < after; ++i) {
		char path[4096] = { 0 };
		uint32_t pathLen = 0;
		uint64_t timestamp = 0;

		if (rdoc->GetCapture(i, path, &pathLen, &timestamp))
			LOG("CAPTURED: %s", path);
	}

	LOG("done, %u capture(s)", after - before);
	return NULL;
}

__attribute__((constructor))
static void Install(void)
{
	pthread_t tid;

	if (pthread_create(&tid, NULL, TriggerThread, NULL) != 0) {
		LOG("FAIL: could not start the trigger thread");
		return;
	}

	pthread_detach(tid);
}
