/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>

// Rewrites the fixed-function vertex-input builtins out of the GLSL the engine
// compiles, so a draw path with no fixed-function arrays to bind can still feed
// a shader it did not author.
//
// That is the unlock for retiring glBegin/glEnd/glVertex/glTexCoord: those
// survive only where a game-supplied shader is bound and expects gl_Vertex,
// gl_Color and gl_MultiTexCoord0 to arrive through fixed function, which leaves
// immediate mode as the only way to deliver them. Rewriting the source the
// engine compiles reaches a game's own shaders too, so an unmodified game gets
// the modern feed without touching its content or the gl.* API.
//
// The attributes are selected per draw by `recoil_ff_useAttrs`, a uniform that
// defaults to false. A program compiled through here therefore renders exactly
// as it does today until a caller opts in, which is what keeps every existing
// draw path -- and every game nobody has updated -- unchanged.
//
// #define of a gl_ name is illegal in GLSL, so this is textual substitution of
// the identifier plus a generated declaration block.
namespace GL {
	// Vertex shaders only: in a fragment shader gl_Color is the interpolated
	// varying rather than the attribute, and gl_MultiTexCoord0 does not exist.
	//
	// Returns true when `src` was rewritten. Declines -- leaving `src` untouched
	// -- when the shader reads a fixed-function vertex builtin this does not
	// feed (gl_Normal, gl_SecondaryColor, gl_FogCoord, gl_MultiTexCoord1..7).
	// Half-feeding one of those would silently zero it, so such a shader keeps
	// the fixed-function path and its draws keep the legacy fallback.
	//
	// glslVersion is the effective #version (0 when none was given, i.e. 110),
	// which decides `attribute` vs `in`.
	bool RewriteFFVertexBuiltins(std::string& src, int glslVersion);

	// Parses a leading "#version <n>" out of `text`; 0 when absent.
	int ParseGlslVersion(const std::string& text);

	// Where the rewritten program takes its vertex inputs from. Locations are
	// QUERIED rather than pinned, so an injected attribute cannot collide with
	// one the shader declares itself.
	struct FFAttribBinding {
		int32_t vertex = -1;
		int32_t color = -1;
		int32_t texCoord0 = -1;
		int32_t useAttrs = -1; // the uniform; -1 means "not a rewritten program"

		bool Usable() const { return useAttrs >= 0 && vertex >= 0; }
	};

	// Introspects `prog` once and caches the result. Returns nullptr for 0, for
	// a program that was never rewritten, and for one whose rewrite the linker
	// optimized away.
	const FFAttribBinding* GetFFAttribBinding(uint32_t prog);

	// GL_CURRENT_PROGRAM.
	uint32_t CurrentProgram();

	// Draw an interleaved pos3/uv2/color4 float stream (9 floats per vertex)
	// through the bound rewritten program, from a VAO of its own so nothing the
	// caller's content set up is disturbed. `recoil_ff_useAttrs` is raised for
	// the draw and lowered again afterwards, because the next draw through the
	// same program may well be one fixed function still feeds.
	void DrawFFAttribStream(uint32_t drawMode, const float* data, size_t vertCount, const FFAttribBinding& b);

	// Config FFVertexAttribRewrite. Off leaves every shader source untouched.
	bool FFRewriteEnabled();
}
