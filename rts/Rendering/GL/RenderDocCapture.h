/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Trigger a RenderDoc frame capture from inside the engine, via RenderDoc's
// in-application API (rts/lib/renderdoc/renderdoc_app.h, MIT).
//
// RenderDoc's own trigger is a keypress, which a scripted run cannot produce --
// and the modern-GL migration's whole acceptance test is "capture an in-match
// BAR frame", something that has to be reachable from a startscript's
// debugcommands to be part of any gate. Hence the "/renderdoccapture" action.
//
// Does nothing (and says so) unless the process was launched under RenderDoc:
// the library is looked up with RTLD_NOLOAD, never loaded by us.
namespace RenderDocCapture {
	// capture the NEXT frame; false if RenderDoc is not hosting this process
	bool TriggerNextFrame();

	// Explicitly bracket a capture instead of asking RenderDoc to pick the next
	// frame itself. TriggerNextFrame depends on RenderDoc recognising this
	// process's swap/window as capturable; when it silently does not (measured:
	// it accepts the trigger and then never starts), the explicit form still
	// works and says so. Everything drawn between the two is one capture.
	bool BeginExplicit();
	bool EndExplicit();
}
