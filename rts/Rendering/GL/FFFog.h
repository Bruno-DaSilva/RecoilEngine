/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

namespace GL {
	// CPU mirror of the fixed-function fog parameters, and -- with the shader
	// rewrite on -- the only place they live.
	//
	// glFogf/glFogfv/glFogi are three of RenderDoc's unsupported functions, and
	// unlike the current colour the values are not consumed by a draw at all:
	// every reader is a shader reading the `gl_Fog` builtin, which the rewrite
	// redirects to a uniform fed from here (GL::PushFFUniforms). The exception is
	// a draw that falls back to fixed function, which reads fog out of GL like it
	// always did -- MaterializeIntoGL() is for exactly those, and they are issuing
	// glBegin or client-array calls anyway.
	//
	// `scale` is derived rather than stored, matching the builtin: GLSL defines
	// gl_Fog.scale as 1/(end - start) whichever mode is set.
	//
	// Unlike GL::ffColor there is no Verify against GL, because GL is not an
	// oracle for this: an attrib bracket's glPopAttrib restores GL's copy without
	// telling the mirror, and a glFog* issued inside a gl.CreateList body is
	// swallowed by the command-list recorder so GL never sees it at all. Both are
	// correct behaviour and both make the two copies differ. What the conversion
	// is proven by instead is FFExperiment 9, which compiles the builtin in
	// alongside the uniform and has the A/B harness read one on the candidate
	// pass and the other everywhere else -- a pixel comparison, which does not
	// care what GL's unused copy holds.
	struct FFFogMirror {
		void SetColor(const float* rgba);
		void SetMode(int32_t m);
		void SetStart(float f);
		void SetEnd(float f);
		void SetDensity(float f);

		const float* Color() const { return color; }
		int32_t Mode() const { return mode; }
		float Start() const { return start; }
		float End() const { return end; }
		float Density() const { return density; }
		float Scale() const { return (end != start) ? 1.0f / (end - start) : 0.0f; }

		// Bumped by every setter; a program whose uniforms were last fed at this
		// generation needs no re-upload, which is what keeps the per-bind feed free
		// in the overwhelmingly common case of fog not having moved.
		uint32_t Generation() const { return generation; }

		void MaterializeIntoGL() const;

	private:
		// the GL defaults, so a mirror that is never written describes GL exactly
		float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int32_t mode = 0x0800; // GL_EXP
		float start = 0.0f;
		float end = 1.0f;
		float density = 1.0f;
		uint32_t generation = 1;
	};

	inline FFFogMirror ffFog;
}
