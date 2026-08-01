/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace Shader {
	struct IProgramObject;
}

namespace GL {
	// A draw with no program bound is drawn by fixed function, and fixed function
	// with lighting off is just `texture * current colour`, optionally fogged --
	// a four-line shader. Standing in for it is what lets such a draw move onto
	// generic vertex attributes and take the client-array family with it.
	//
	// Deliberately NOT a general fixed-function emulator. It reproduces ONE
	// state; the caller checks that state is the live one and keeps its old path
	// otherwise. That is what makes it safe for a game nobody has updated: the
	// fallback is per DRAW, not per run, so the worst case is that a game keeps
	// exactly today's behaviour.
	namespace FFStandIn {
		// Vertex attributes are the modern model VAO's, so both consumers can
		// feed it from their own buffers: 0 = position (vec3), 4 = texCoords[0]
		// (vec4, uv0 in .xy) -- a caller that draws untextured need not feed 4.
		// Uniforms: tex (sampler), uColor (vec4, pre-clamped), uTextured (bool).
		Shader::IProgramObject* GetShader(bool fogged);

		// Null when the live fixed-function state is one GetShader reproduces
		// exactly, else the reason it is not. A rejected draw falls back silently
		// and reads exactly like a converted one, so the reason has to be
		// reportable rather than inferable.
		const char* StateReproducible(bool& textured, bool& fogged);
	}
}
