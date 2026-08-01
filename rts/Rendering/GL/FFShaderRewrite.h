/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>

// Rewrites the fixed-function builtins out of the GLSL the engine
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
// The position and texcoord attributes are selected per draw by
// `recoil_ff_useAttrs`, a uniform that defaults to false, so a program compiled
// through here renders exactly as it does today until a caller feeds it a
// stream. gl_Color has no such switch: it is a global CURRENT value rather than
// something a draw supplies, so it is replaced unconditionally and fed through
// the pinned attribute's current value instead (see GL::ffColor).
//
// #define of a gl_ name is illegal in GLSL, so this is textual substitution of
// the identifier plus a generated declaration block.
namespace GL {
	// The colour attribute's location is PINNED, unlike the other two. An
	// attribute's current value lives in a context slot, not in the program, so
	// the one thing a per-program queried location cannot express is "set the
	// colour now, for whichever program is bound later" -- which is exactly what
	// gl.Color does. 15 is the last slot GL guarantees; the engine binds up to 6
	// and BAR up to 10.
	constexpr int FF_COLOR_ATTRIB_LOC = 15;
	constexpr const char* FF_COLOR_ATTRIB_NAME = "recoil_ff_aColor";

	// The struct instance gl_Fog is substituted with. A struct rather than an
	// accessor so that member access carries over unchanged.
	constexpr const char* FF_FOG_UNIFORM_NAME = "recoil_ff_FogU";
	// selects the uniform over the builtin; see the declaration block for why
	// both are compiled in
	constexpr const char* FF_FOG_SELECT_NAME = "recoil_ff_useFogU";

	// The composed modelview-projection, and its selector. Same both-sources
	// shape as the fog pair, for the same reason: an engine-side substitution is
	// invisible to a gate that compares passes of one build unless the builtin
	// stays reachable.
	constexpr const char* FF_MVP_UNIFORM_NAME = "recoil_ff_MVPu";
	constexpr const char* FF_MVP_SELECT_NAME = "recoil_ff_useMVP";

	// Pin FF_COLOR_ATTRIB_NAME in `prog` ahead of linking it. Binding a name the
	// program does not declare is a no-op, so this is unconditional at each link
	// site rather than conditional on the rewrite having fired.
	void BindFFColorAttribLocation(uint32_t prog);

	// `stage` is the GL shader type. The vertex-input channels are vertex-stage
	// only -- in a fragment shader gl_Color is the interpolated varying rather
	// than the attribute, and gl_MultiTexCoord0 does not exist -- while gl_Fog is
	// fixed-function state read identically in both.
	//
	// Returns true when `src` was rewritten. The POSITION and TEXCOORD channels
	// are declined -- left as the builtins -- when the shader reads a
	// fixed-function vertex builtin this does not feed (gl_Normal,
	// gl_SecondaryColor, gl_FogCoord, gl_MultiTexCoord1..7): half-feeding a
	// stream would silently zero one of them, so such a shader keeps the
	// fixed-function path and its draws keep the legacy fallback. gl_Color is
	// replaced either way, since its channel is complete on its own.
	//
	// glslVersion is the effective #version (0 when none was given, i.e. 110),
	// which decides `attribute` vs `in`.
	bool RewriteFFBuiltins(std::string& src, int glslVersion, uint32_t stage);

	// Parses a leading "#version <n>" out of `text`; 0 when absent.
	int ParseGlslVersion(const std::string& text);

	// Where the rewritten program takes its vertex inputs from. Position and
	// texcoord locations are QUERIED rather than pinned, so an injected attribute
	// cannot collide with one the shader declares itself; colour is the pinned
	// one and is here only so a stream draw can fill the same slot it reads.
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

	// Feed the uniforms the rewrite introduced into `prog`, which is about to be
	// (or has just been) bound. Locations are looked up once per program and the
	// upload is skipped while the mirrored state has not moved since this
	// program last saw it, so a bound-but-unchanged program costs one map lookup.
	void PushFFUniforms(uint32_t prog);

	// Wrap glUseProgram so PushFFUniforms runs on every bind, and glLinkProgram /
	// glDeleteProgram so the location cache cannot outlive the layout it
	// describes. glad has one pointer per entry point, so engine and Lua binds
	// both come through it -- there is no second path to miss.
	void InstallFFUniformFeed();

	// True while a gl.CreateList body runs. Two things must not happen there:
	// uniform uploads (RECORDED by a real compile, and a MATERIALIZER for a
	// capture -- one glUniform1i turns a capture into a real display list and
	// every legacy call in it into a real GL call), and materializing mirrored
	// fixed-function state for the same reason. Both are re-done at replay time,
	// which is when the program is re-bound and the draw actually happens.
	inline bool ffListBodyOpen = false;

	// Config FFVertexAttribRewrite. Off leaves every shader source untouched.
	bool FFRewriteEnabled();
}
