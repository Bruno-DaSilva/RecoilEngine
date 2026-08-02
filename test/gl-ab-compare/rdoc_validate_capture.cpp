// Prove a .rdc is a capture, not just a file.
//
// run_rdoc_capture.sh can only see that a file appeared with the right magic
// bytes, and that is a weak claim: what is being asserted is that RenderDoc can
// OPEN this frame and walk its draws, which only RenderDoc's replay side can
// answer. That side lives in librenderdoc itself, so this links against it and
// asks -- no Qt, and no renderdoccmd (which needs X11/Xlib-xcb.h, a dev package
// this machine does not have).
//
// Reports the driver the file identifies as, whether local replay is supported,
// and -- if the replay device comes up -- the action tree actually replayed,
// which is the strongest form of the claim available offline.
//
// build:
//   c++ -O2 -std=c++17 -DRENDERDOC_PLATFORM_LINUX -I<renderdoc-src>/renderdoc/api
//       -o rdoc_validate_capture rdoc_validate_capture.cpp
//       -L<renderdoc-build>/lib -lrenderdoc -Wl,-rpath,<renderdoc-build>/lib
//
// usage: rdoc_validate_capture <capture.rdc> [thumbnail.png]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <replay/renderdoc_replay.h>

// Without this, librenderdoc treats its own host as an app to CAPTURE, hooks GLX
// inside it, and then fails an IsReplayApp() assertion the moment replay tries to
// create a context -- which reads exactly like "the capture cannot be replayed".
// It must be exported from the executable, so -rdynamic (or an export list).
REPLAY_PROGRAM_MARKER()

namespace {
	struct ActionStats {
		uint32_t total = 0;
		uint32_t draws = 0;
		uint32_t dispatches = 0;
		uint32_t clears = 0;
	};

	void WalkActions(const rdcarray<ActionDescription>& actions, ActionStats& stats)
	{
		for (const ActionDescription& a: actions) {
			stats.total++;

			const uint32_t flags = (uint32_t)a.flags;

			if (flags & (uint32_t)ActionFlags::Drawcall)  stats.draws++;
			if (flags & (uint32_t)ActionFlags::Dispatch)  stats.dispatches++;
			if (flags & (uint32_t)ActionFlags::Clear)     stats.clears++;

			WalkActions(a.children, stats);
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <capture.rdc> [thumbnail.png]\n", argv[0]);
		return 2;
	}

	// RenderDoc reports why replay failed only through its diagnostic log, so
	// route that somewhere readable before anything can fail.
	const char* debugLog = getenv("RDOC_VALIDATE_LOG");

	if (debugLog != NULL && *debugLog != '\0')
		RENDERDOC_SetDebugLogFile(debugLog);

	GlobalEnvironment env;
	rdcarray<rdcstr> initArgs;
	RENDERDOC_InitialiseReplay(env, initArgs);

	ICaptureFile* file = RENDERDOC_OpenCaptureFile();

	if (file == NULL) {
		fprintf(stderr, "FAIL: RENDERDOC_OpenCaptureFile returned NULL\n");
		return 1;
	}

	if (!file->OpenFile(argv[1], "rdc", NULL).OK()) {
		fprintf(stderr, "FAIL: RenderDoc could not open %s as a capture\n", argv[1]);
		return 1;
	}

	printf("driver:        %s\n", file->DriverName().c_str());
	printf("local replay:  %s\n",
		file->LocalReplaySupport() == ReplaySupport::Supported ? "supported" : "NOT supported");

	// The thumbnail is the frame RenderDoc saw, rendered by the engine, stored in
	// its own section of the file -- so it is also a cheap check that the capture
	// is of something and not of a black window.
	if (argc >= 3) {
		const Thumbnail thumb = file->GetThumbnail(FileType::PNG, 0);

		if (thumb.data.empty()) {
			printf("thumbnail:     none embedded\n");
		} else {
			FILE* out = fopen(argv[2], "wb");

			if (out != NULL) {
				fwrite(thumb.data.data(), 1, thumb.data.size(), out);
				fclose(out);
				printf("thumbnail:     %ux%u -> %s (%zu bytes)\n",
					thumb.width, thumb.height, argv[2], (size_t)thumb.data.size());
			}
		}
	}

	rdcpair<ResultDetails, IReplayController*> opened = file->OpenCapture(ReplayOptions(), NULL);

	if (!opened.first.OK() || opened.second == NULL) {
		// Not necessarily a bad capture -- replay needs a GPU and a display, and
		// this machine may have neither at the time. Say which claim failed.
		printf("replay:        could not open a replay device (code %d)\n", (int)opened.first.code);
		// ResultDetails::Message() stringises the code through DoStringise, which
		// librenderdoc does not export, so the code number is all there is here.

		// Why it failed is in RenderDoc's own diagnostic log and nowhere else,
		// and "could not replay" without a reason is not a usable result.
		rdcstr diag;
		RENDERDOC_GetLogFileContents(0, diag);
		printf("               log: %s\n", RENDERDOC_GetLogFile());
		printf("%s\n", diag.c_str());

		printf("\nPARTIAL: the file is a valid capture, but it was not replayed here.\n");
		file->Shutdown();
		RENDERDOC_ShutdownReplay();
		return 3;
	}

	IReplayController* replay = opened.second;

	const APIProperties props = replay->GetAPIProperties();
	printf("replay:        opened, api=%d degraded=%d\n", (int)props.pipelineType, (int)props.degraded);

	ActionStats stats;
	WalkActions(replay->GetRootActions(), stats);

	printf("actions:       %u total, %u draws, %u dispatches, %u clears\n",
		stats.total, stats.draws, stats.dispatches, stats.clears);

	replay->Shutdown();
	file->Shutdown();
	RENDERDOC_ShutdownReplay();

	if (stats.draws == 0) {
		fprintf(stderr, "FAIL: the capture replays but contains no draws\n");
		return 1;
	}

	printf("\nOK: RenderDoc opened and replayed this capture.\n");
	return 0;
}
