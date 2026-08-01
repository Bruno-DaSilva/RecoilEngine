/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace GL {
	// Which draws still reach the FIXED-FUNCTION pipeline, i.e. run with no
	// program bound. That set is what decides whether the remaining
	// fixed-function state families can be retired at all: alpha test, fog, clip
	// planes, line stipple and the current color are read by the rasterizer
	// itself on those draws, so no amount of shader-side substitution reaches
	// them. Nothing static answers it -- a draw is fixed-function or not
	// depending on who bound what earlier -- so this counts them live, keyed by
	// call site, the same way rdoc_site_census.sh does for the unsupported
	// functions themselves.
	//
	// Development instrument, off unless config GLFFDrawCensus: it swaps the
	// glad pointers for the draw entry points and takes a backtrace on every
	// fixed-function draw.
	namespace FFDrawCensus {
		void Install();
		void Dump();
	}
}
