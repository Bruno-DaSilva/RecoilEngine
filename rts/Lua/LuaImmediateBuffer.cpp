/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Lua/LuaImmediateBuffer.h"

#include "Lua/LuaOpenGL.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/FFStateTracker.h"
#include "Rendering/GL/FFFog.h"
#include "Rendering/GL/FFMatrixTracking.h"
#include "Rendering/GL/FFShaderRewrite.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/UnorderedMap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {
	// uniform-MVP shaders for the modern backend: transform by a uniform mat4,
	// NOT gl_ModelViewProjectionMatrix, so the path makes no fixed-function
	// matrix calls. Vertex layout is the emitter's own interleaved FLOAT stream
	// (pos=0 vec3, uv=1 vec2, color=2 vec4): float vertex colors carry the
	// exact values the legacy float pipeline interpolates -- an 8-bit color
	// attribute rounds each component by up to 0.5 LSB, which stacked additive
	// glow quads amplified to visible deltas under the whole-frame A/B gate.
	// Fog: legacy glBegin/glEnd draws get fixed-function fog applied; the
	// modern shader must replicate it or distant world draws diverge by a few
	// LSB (replay residual class: translucent ground quads, thin world lines).
	// LINEAR mode only (the engine sets linear map fog; other modes take the
	// legacy fallback), fog coordinate = |eye z| via uMV, exact state values
	// from the compatibility gl_Fog builtin.
	// FOUR program variants: {untextured, textured} x {fogless, fogged}. The
	// fogless variants are the byte-parity-proven shaders and stay the default;
	// the fogged ones (fog factor per vertex, classic FF semantics, exact state
	// via the compatibility gl_Fog builtin) are selected ONLY when the FF fog
	// state actually bites the stream (see FogIsEffective). Touching the proven
	// shader source perturbed the compiled gl_Position math enough to shift
	// thin-primitive edge pixels under the A/B gate, so fog lives in separate
	// programs rather than a uniform branch.
	std::string MakeFragmentSrc(bool textured, bool fogged)
	{
		std::string s = "#version 150" + std::string(fogged ? GL::FFCompatToken() : "") + "\n";
		if (textured) {
			s += "uniform sampler2D tex;\n";
			s += "in vec2 vuv;\n";
		}
		s += "in vec4 vcolor;\n";
		s += "out vec4 outColor;\n";
		if (fogged)
			s += "in float vFogF;\n";

		const char* base = textured ? "texture(tex, vuv) * vcolor" : "vcolor";
		if (fogged) {
			s += std::string("void main() { vec4 c = ") + base + "; "
				 "outColor = vec4(mix(gl_Fog.color.rgb, c.rgb, clamp(vFogF, 0.0, 1.0)), c.a); }\n";
		} else {
			s += std::string("void main() { outColor = ") + base + "; }\n";
		}
		return s;
	}

	std::string MakeVertexSrc(bool explicitAttribLoc, bool textured, bool fogged, bool builtinMVP)
	{
		std::string s = "#version 150" + std::string(GL::FFCompatToken()) + "\n";
		if (explicitAttribLoc) {
			s += "#extension GL_ARB_explicit_attrib_location : require\n";
			s += "layout(location = 0) in vec3 apos;\n";
			s += "layout(location = 1) in vec2 auv;\n";
			s += "layout(location = 2) in vec4 acolor;\n";
		} else {
			s += "in vec3 apos;\n";
			s += "in vec2 auv;\n";
			s += "in vec4 acolor;\n";
		}
		// uMVP: CPU-composed float P*MV. Composition can differ from the
		// driver's own by final-bit ULPs on DENSE matrices (no arithmetic
		// recipe provably matches an undocumented driver ordering; measured:
		// two-step P*(MV*v) and double-composed variants both flip pixels),
		// so the flushes only go modern under screen-aligned MV, where the
		// composition is measured exact across all gate content. uMV (fog
		// variant only) feeds the eye-space fog coordinate.
		// builtinMVP: transform by the compatibility gl_ModelViewProjectionMatrix
		// instead of a CPU-composed uniform. The driver composes P*MV itself, so
		// the result is exact for ANY matrices -- which is what lets dense-MV and
		// perspective-projection streams go modern at all (see the gates in
		// FlushModern). Reading a builtin is not a GL call, so it costs nothing at
		// the capture gate; only the FF matrix SET-calls do, and those are a
		// separate group.
		if (!builtinMVP)
			s += "uniform mat4 uMVP;\n";
		if (fogged)
			s += "uniform mat4 uMV;\n";
		if (textured)
			s += "out vec2 vuv;\n";
		s += "out vec4 vcolor;\n";
		if (fogged)
			s += "out float vFogF;\n";
		s += "void main() { ";
		if (textured)
			s += "vuv = auv; ";
		s += "vcolor = acolor; ";
		if (fogged)
			s += "vFogF = (gl_Fog.end - abs((uMV * vec4(apos, 1.0)).z)) * gl_Fog.scale; ";
		s += builtinMVP ? "gl_Position = gl_ModelViewProjectionMatrix * vec4(apos, 1.0); }\n"
		                : "gl_Position = uMVP * vec4(apos, 1.0); }\n";
		return s;
	}

	Shader::IProgramObject* GetModernShaderImpl(const char* poName, bool textured, bool fogged, bool builtinMVP)
	{
		Shader::IProgramObject* shader = shaderHandler->GetProgramObject("[LuaImmediateBuffer]", poName);
		if (shader != nullptr && shader->IsValid())
			return shader;

		const bool eal = globalRendering->supportExplicitAttribLoc;

		shader = shaderHandler->CreateProgramObject("[LuaImmediateBuffer]", poName);
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeVertexSrc(eal, textured, fogged, builtinMVP), "", GL_VERTEX_SHADER));
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeFragmentSrc(textured, fogged), "", GL_FRAGMENT_SHADER));

		if (!eal) {
			shader->BindAttribLocation("apos", 0);
			shader->BindAttribLocation("auv", 1);
			shader->BindAttribLocation("acolor", 2);
		}

		shader->Link();
		return shader;
	}

	// EIGHT variants: {untextured, textured} x {fogless, fogged} x {uniform MVP,
	// builtin MVP}. Separate programs rather than a uniform branch, because
	// touching the parity-proven shader source perturbs the compiled gl_Position
	// math enough to shift thin-primitive edge pixels (the same reason fog is a
	// variant).
	Shader::IProgramObject* GetModernShader(bool fogged, bool builtinMVP) {
		if (builtinMVP)
			return fogged ? GetModernShaderImpl("IMM_F_FOG_BMVP", false, true, true)
			              : GetModernShaderImpl("IMM_F_BMVP", false, false, true);
		return fogged ? GetModernShaderImpl("IMM_F_FOG", false, true, false)
		              : GetModernShaderImpl("IMM_F", false, false, false);
	}
	Shader::IProgramObject* GetModernTexShader(bool fogged, bool builtinMVP) {
		if (builtinMVP)
			return fogged ? GetModernShaderImpl("IMM_TEX_F_FOG_BMVP", true, true, true)
			              : GetModernShaderImpl("IMM_TEX_F_BMVP", true, false, true);
		return fogged ? GetModernShaderImpl("IMM_TEX_F_FOG", true, true, false)
		              : GetModernShaderImpl("IMM_TEX_F", true, false, false);
	}

	// True when the modelview has NO rotation/shear terms: a screen-aligned
	// translate+scale transform. Only such draws go modern while the legacy
	// pipeline coexists: composing P*MV on the CPU can differ from the
	// driver's own composition by final-bit ULPs on dense matrices, which
	// flips edge pixels on thin primitives (measured classes: gl.Rotate'd
	// loading-spinner arcs, world-camera lines/quads in PiP and minimap
	// views). Dense-MV draws keep the exact legacy replay until the FF
	// pipeline is deleted and bit-parity against it stops being a
	// requirement.
	bool ScreenAlignedMV(const CMatrix44f& mv)
	{
		return mv.m[1] == 0.0f && mv.m[2] == 0.0f &&
		       mv.m[4] == 0.0f && mv.m[6] == 0.0f &&
		       mv.m[8] == 0.0f && mv.m[9] == 0.0f;
	}

	// The screen-aligned composition-exactness measurement (see ScreenAlignedMV)
	// only holds under an ORTHO-like projection (w row = {0,0,0,*}). Under a
	// PERSPECTIVE projection (BAR's tilted top-bar UI draws through one inside
	// display lists) the CPU-composed P*MV can ULP-differ from the driver's own
	// composition, and the perspective divide amplifies that into subpixel
	// vertex shifts -- invisible on flat fills, but LINEAR-sampled
	// high-frequency textures (glyph caches, icons) diverge by whole shades
	// (command-list gate, 2026-07-04: ~4-7k px/frame, max delta ~200, textured
	// streams only). Perspective draws take the exact legacy replay.
	bool OrthoProjection(const CMatrix44f& p)
	{
		return p.m[3] == 0.0f && p.m[7] == 0.0f && p.m[11] == 0.0f;
	}

	// True when the current FF LINEAR fog state can tint any vertex of the
	// stream: the fog factor (end - |eye z|) * scale dips below 1 for the
	// farthest vertex. When every factor saturates at 1 fog is inert and the
	// byte-parity-proven fogless program handles the draw.
	bool FogIsEffective(const CMatrix44f& mv, const std::vector<VA_TYPE_TC>& verts)
	{
		const GLfloat fogStart = GL::ffFog.Start();
		const GLfloat fogEnd = GL::ffFog.End();
		if (fogEnd <= fogStart)
			return true; // degenerate scale; be conservative

		float maxAbsZ = 0.0f;
		for (const VA_TYPE_TC& v : verts)
			maxAbsZ = std::max(maxAbsZ, std::fabs((mv * v.pos).z));

		return ((fogEnd - maxAbsZ) / (fogEnd - fogStart)) < 1.0f;
	}

	// The emitter's own streaming VAO/VBO for the interleaved float vertex
	// (pos3 uv2 color4 = 9 floats). Orphaned with glBufferData on EVERY draw so
	// consecutive flushes within a frame never overwrite in-flight data; the
	// buffer NAME stays stable, so the VAO's recorded attribute bindings remain
	// valid (re-specifying the store is not TypedRenderBuffer's Resize, which
	// creates a NEW buffer object and orphans the VAO -- that bug class).
	struct ImmFloatStream {
		static constexpr GLsizei STRIDE = 9 * sizeof(float);

		GLuint vao = 0;
		GLuint vbo = 0;

		void Draw(uint32_t drawMode, const std::vector<float>& data) {
			const size_t vertCount = data.size() / 9;
			if (vertCount == 0)
				return;

			if (vao == 0) {
				glGenVertexArrays(1, &vao);
				glGenBuffers(1, &vbo);
				glBindVertexArray(vao);
				glBindBuffer(GL_ARRAY_BUFFER, vbo);
				glEnableVertexAttribArray(0);
				glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>( 0));
				glEnableVertexAttribArray(1);
				glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(12));
				glEnableVertexAttribArray(2);
				glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(20));
				glBindVertexArray(0);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
			}

			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STREAM_DRAW);
			glBindBuffer(GL_ARRAY_BUFFER, 0);

			glBindVertexArray(vao);
			glDrawArrays(drawMode, 0, static_cast<GLsizei>(vertCount));
			glBindVertexArray(0);
		}
	};

	ImmFloatStream immStream;

	// A rejected draw falls back silently and reads exactly like a converted
	// one, so say which happened -- once per outcome, since these fire
	// thousands of times a run.
	void ReportBoundShaderOutcome(bool active, uint32_t prog)
	{
		static bool saidActive = false, saidRejected = false;
		bool& said = active ? saidActive : saidRejected;
		if (said)
			return;

		said = true;
		LOG_L(L_WARNING, "[FFAttribFeed] %s: immediate stream into bound program %u",
			active ? "ACTIVE" : "REJECTED (program not rewritten -- keeping the legacy replay)", prog);
	}

	// GL_QUADS / GL_QUAD_STRIP / GL_POLYGON are not in the core profile, so the
	// modern path triangulates them (as an index remap into the captured
	// stream); other modes pass through unchanged. For a planar convex
	// quad/polygon the triangle union (hence coverage) is the same as the
	// fixed-function decomposition, so flat-colored fills are bit-exact either
	// way -- but a SMOOTH-shaded fill interpolates along the split, so the
	// diagonal has to be the one the driver picks or the quad's interior differs.
	//
	// It is {0,1,3},{1,2,3}, not the {0,1,2},{0,2,3} this used to assume.
	// Measured, not reasoned: with the wrong diagonal the whole-frame gate
	// diverged on ~14% of frames by a few interior pixels at delta 16-29 (BAR's
	// gradient-filled UI bars), and flipping it took the same gate to 0/722.
	// That difference stayed hidden for as long as the gradient-heavy lists
	// compiled into display lists, which rendered identically in both passes.
	std::pair<std::vector<uint32_t>, uint32_t> TriangulateIndicesForModern(uint32_t mode, size_t n)
	{
		std::vector<uint32_t> out;

		switch (mode) {
			case GL_QUADS: {
				out.reserve((n / 4) * 6);
				for (size_t i = 0; i + 3 < n; i += 4)
					out.insert(out.end(), {uint32_t(i), uint32_t(i + 1), uint32_t(i + 3),
					                       uint32_t(i + 1), uint32_t(i + 2), uint32_t(i + 3)});
				return {std::move(out), GL_TRIANGLES};
			}
			case GL_QUAD_STRIP: {
				// quad k spans verts {2k, 2k+1, 2k+3, 2k+2}
				for (size_t i = 0; i + 3 < n; i += 2)
					out.insert(out.end(), {uint32_t(i), uint32_t(i + 1), uint32_t(i + 3),
					                       uint32_t(i), uint32_t(i + 3), uint32_t(i + 2)});
				return {std::move(out), GL_TRIANGLES};
			}
			case GL_POLYGON: {
				// triangle fan from the first vertex (matches FF for convex polys)
				for (size_t i = 1; i + 1 < n; ++i)
					out.insert(out.end(), {uint32_t(0), uint32_t(i), uint32_t(i + 1)});
				return {std::move(out), GL_TRIANGLES};
			}
			default: {
				out.reserve(n);
				for (size_t i = 0; i < n; ++i)
					out.push_back(uint32_t(i));
				return {std::move(out), mode};
			}
		}
	}

	// FF samples an INCOMPLETE texture as if texturing were disabled while a
	// GLSL sampler2D returns (0,0,0,1), so no modern draw may SAMPLE such a
	// texture. Detects the practical case on the unit-0 GL_TEXTURE_2D binding:
	// mipmapping min filter, MAX_LEVEL not clamped to 0, and no level-1 image
	// (or no base image at all).
	bool MipIncompleteTexture2D()
	{
		GLint minFilter = 0;
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
		const bool mipFilter =
			(minFilter == GL_NEAREST_MIPMAP_NEAREST) || (minFilter == GL_LINEAR_MIPMAP_NEAREST) ||
			(minFilter == GL_NEAREST_MIPMAP_LINEAR)  || (minFilter == GL_LINEAR_MIPMAP_LINEAR);
		if (!mipFilter)
			return false;

		GLint maxLevel = 1000;
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
		GLint w0 = 0, h0 = 0, w1 = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH,  &w1);
		if (w0 == 0)
			return true; // no base image
		return (maxLevel != 0) && (w0 > 1 || h0 > 1) && (w1 == 0);
	}

	// Any FF texture unit above 0 with an enabled target: that engages FF
	// multitexture combining the single-sampler shader does not implement.
	// (GL_MAX_TEXTURE_UNITS is the FF unit count, typically 4.)
	bool AnyOtherFFUnitEnabled()
	{
		GLint maxFFUnits = 0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &maxFFUnits);
		maxFFUnits = std::min(maxFFUnits, GLint(8));

		bool enabled = false;
		for (GLint u = 1; u < maxFFUnits; ++u) {
			glActiveTexture(GL_TEXTURE0 + u);
			enabled = enabled ||
				(glIsEnabled(GL_TEXTURE_2D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_1D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_3D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE);
		}
		glActiveTexture(GL_TEXTURE0);
		return enabled;
	}

	// The FF texturing configurations the modern textured shader reproduces
	// EXACTLY: active unit 0 with plain GL_TEXTURE_2D sampling (no
	// higher-priority target enabled, no texgen, no other enabled units),
	// MODULATE env, identity texture matrix, and a texture whose base format
	// modulates like GLSL sampling does. Anything else falls back to the exact
	// legacy replay: legacy is the parity oracle, so unsupported state costs
	// only modern coverage, never correctness. This gate is why the earlier
	// ungated attempt diverged on gui_pip's textured overlays (255-class).
	// Returns the rejecting sub-gate, or REASON_NONE when the state is
	// reproducible. Two of those sub-gates -- TEX_2D_DISABLED and
	// TEX_MIP_INCOMPLETE -- report that FF samples NOTHING, which the
	// untextured program reproduces exactly; both are checked with the "and
	// nothing else textures this fragment" guards the caller needs to convert
	// rather than fall back.
	LuaImmFallback::Reason PlainModulateTexturing()
	{
		using namespace LuaImmFallback;

		GLint activeUnit = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
		if (activeUnit != GL_TEXTURE0)
			return REASON_TEX_ACTIVE_UNIT;

		// No 2D sampling. If nothing else can texture this fragment either --
		// no other FF target on unit 0, no other enabled unit -- then texturing
		// is simply OFF and FF rasterizes the plain interpolated vertex color,
		// which is what the (parity-proven) UNTEXTURED program already does; the
		// caller converts rather than falling back. Otherwise FF samples that
		// other target/unit and only the legacy replay is exact.
		if (glIsEnabled(GL_TEXTURE_2D) != GL_TRUE) {
			if (glIsEnabled(GL_TEXTURE_1D) == GL_TRUE ||
			    glIsEnabled(GL_TEXTURE_3D) == GL_TRUE ||
			    glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE ||
			    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE)
				return REASON_TEX_TARGET_PRIORITY;

			return AnyOtherFFUnitEnabled() ? REASON_TEX_MULTI_UNIT : REASON_TEX_2D_DISABLED;
		}

		// FF target priority: an enabled cube/rect/3D target overrides 2D
		// (1D is BELOW 2D, so an enabled 1D loses and is fine)
		if (glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_3D) == GL_TRUE)
			return REASON_TEX_TARGET_PRIORITY;

		// An INCOMPLETE texture disables the whole unit (GL 2.1 3.8.15), so FF
		// rasterizes the plain vertex color and every sampling gate below is
		// moot: this is the "texturing is off" case reached by a second route,
		// and the caller converts it the same way -- provided nothing else
		// textures the fragment. The draft-spot octagons in the Supreme Isthmus
		// replay bind a mip-filtered texture with no mip chain, so legacy drew
		// white fills while a sampling shader drew black. An enabled 1D target
		// goes down with the unit per that spec rule, but the corner is
		// untestable on BAR content and refusing it costs no coverage.
		if (MipIncompleteTexture2D()) {
			if (glIsEnabled(GL_TEXTURE_1D) == GL_TRUE)
				return REASON_TEX_TARGET_PRIORITY;

			return AnyOtherFFUnitEnabled() ? REASON_TEX_MULTI_UNIT : REASON_TEX_MIP_INCOMPLETE;
		}

		// texgen replaces the captured per-vertex texcoords
		if (glIsEnabled(GL_TEXTURE_GEN_S) == GL_TRUE || glIsEnabled(GL_TEXTURE_GEN_T) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_GEN_R) == GL_TRUE || glIsEnabled(GL_TEXTURE_GEN_Q) == GL_TRUE)
			return REASON_TEX_TEXGEN;

		// glGetTexEnviv is itself on RenderDoc's unsupported list, so this gate
		// was a capture blocker in its own right. While nothing has written a
		// texture env every unit still holds the GL default, GL_MODULATE, and the
		// query can be skipped entirely -- which is the whole of BAR, since the
		// game has no gl.TexEnv call sites.
		if (!GL::ffResetState.TexEnvIsKnownModulate()) {
			GLint envMode = 0;
			glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envMode);
			if (envMode != GL_MODULATE)
				return REASON_TEX_ENV_MODE;
		}

		// FF transforms texcoords by the texture matrix; the shader does not
		static const CMatrix44f identity;
		CMatrix44f texMat;
		GL::ReadFFMatrix(GL_TEXTURE, texMat);
		for (int i = 0; i < 16; ++i) {
			if (texMat.m[i] != identity.m[i])
				return REASON_TEX_MATRIX;
		}

		// GL_ALPHA-format MODULATE passes the fragment RGB through untouched,
		// while GLSL texture() samples (0,0,0,A) and would zero it
		GLint intFormat = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &intFormat);
		switch (intFormat) {
			case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8:
			case GL_ALPHA12: case GL_ALPHA16: case GL_COMPRESSED_ALPHA:
				return REASON_TEX_ALPHA_FORMAT;
			default:
				break;
		}

		return AnyOtherFFUnitEnabled() ? REASON_TEX_MULTI_UNIT : REASON_NONE;
	}
}

CONFIG(bool, LuaImmediateFallbackStats).defaultValue(false).safemodeValue(false)
	.description("Tally why modern Lua immediate-mode flushes fall back to the legacy replay; dump with \"/luaimmfallback\".");

namespace LuaImmFallback {
	// [reason][drawMode]; drawMode indexes LuaOpenGL::DrawMode (0..DRAW_LAST_MODE)
	static constexpr int NUM_DRAW_MODES = 9;
	static constexpr int SITE_RANK_LIMIT = 15;
	static uint64_t flushCount[REASON_COUNT][NUM_DRAW_MODES] = {{0}};
	static uint64_t vertCount [REASON_COUNT][NUM_DRAW_MODES] = {{0}};

	static const char* reasonNames[REASON_COUNT] = {
		"DENSE_MV", "PERSPECTIVE_P", "TEX_ACTIVE_UNIT", "TEX_2D_DISABLED",
		"TEX_TARGET_PRIORITY", "TEX_TEXGEN", "TEX_ENV_MODE", "TEX_MATRIX",
		"TEX_MIP_INCOMPLETE", "TEX_ALPHA_FORMAT", "TEX_MULTI_UNIT", "FOG_MODE",
		"BOUND_SHADER",
	};
	static const char* drawModeNames[NUM_DRAW_MODES] = {
		"NONE", "GENESIS", "WORLD", "WORLD_SHADOW", "WORLD_REFLECTION",
		"WORLD_REFRACTION", "SCREEN", "MINIMAP", "MINIMAP_BACKGROUND",
	};

	bool Enabled()
	{
		static const bool enabled = configHandler->GetBool("LuaImmediateFallbackStats");
		return enabled;
	}

	// [reason][call site]; the site is whatever the gl.* dispatch last tagged
	struct SiteTally { uint64_t flushes = 0; uint64_t verts = 0; };
	static std::string curSite = "<untagged>";
	static spring::unordered_map<std::string, SiteTally> siteTally[REASON_COUNT];

	void SetCallSite(const char* site) { curSite = site; }

	void Count(Reason r, int drawMode, size_t numVerts)
	{
		if (r >= REASON_COUNT || drawMode < 0 || drawMode >= NUM_DRAW_MODES)
			return;

		flushCount[r][drawMode] += 1;
		vertCount [r][drawMode] += numVerts;

		SiteTally& t = siteTally[r][curSite];
		t.flushes += 1;
		t.verts   += numVerts;
	}

	void Dump()
	{
		if (!Enabled()) {
			LOG_L(L_WARNING, "[LuaImmFallback] disabled -- set LuaImmediateFallbackStats=1");
			return;
		}

		LOG_L(L_WARNING, "[LuaImmFallback] modern->legacy fallbacks by reason x draw mode (flushes / vertices)");

		uint64_t grandFlushes = 0;
		uint64_t grandVerts = 0;

		for (int r = 0; r < REASON_COUNT; ++r) {
			uint64_t rowFlushes = 0;
			uint64_t rowVerts = 0;
			for (int d = 0; d < NUM_DRAW_MODES; ++d) {
				rowFlushes += flushCount[r][d];
				rowVerts   += vertCount [r][d];
			}
			if (rowFlushes == 0)
				continue;

			grandFlushes += rowFlushes;
			grandVerts   += rowVerts;

			std::string perMode;
			for (int d = 0; d < NUM_DRAW_MODES; ++d) {
				if (flushCount[r][d] == 0)
					continue;
				perMode += "  " + std::string(drawModeNames[d]) + "=" +
				           std::to_string(flushCount[r][d]) + "/" + std::to_string(vertCount[r][d]);
			}
			LOG_L(L_WARNING, "[LuaImmFallback]   %-20s %8llu / %-10llu %s",
			      reasonNames[r],
			      static_cast<unsigned long long>(rowFlushes),
			      static_cast<unsigned long long>(rowVerts),
			      perMode.c_str());
		}

		LOG_L(L_WARNING, "[LuaImmFallback]   %-20s %8llu / %llu", "TOTAL",
		      static_cast<unsigned long long>(grandFlushes),
		      static_cast<unsigned long long>(grandVerts));

		// per-reason call-site ranking, by VERTICES: converting content is paid
		// for per draw site, and one site emitting 400 verts a flush is worth
		// far more than a hundred sites emitting four
		for (int r = 0; r < REASON_COUNT; ++r) {
			if (siteTally[r].empty())
				continue;

			std::vector<std::pair<std::string, SiteTally>> sites(siteTally[r].begin(), siteTally[r].end());
			std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) {
				return a.second.verts > b.second.verts;
			});

			LOG_L(L_WARNING, "[LuaImmFallback] %s by call site (flushes / vertices), top %d of %d:",
			      reasonNames[r], std::min(SITE_RANK_LIMIT, int(sites.size())), int(sites.size()));
			for (int i = 0; i < int(sites.size()) && i < SITE_RANK_LIMIT; ++i) {
				LOG_L(L_WARNING, "[LuaImmFallback]   %8llu / %-12llu %s",
				      static_cast<unsigned long long>(sites[i].second.flushes),
				      static_cast<unsigned long long>(sites[i].second.verts),
				      sites[i].first.c_str());
			}
		}
	}
}

void LuaImmediateBuffer::CountFallback(LuaImmFallback::Reason r, size_t numVerts) const
{
	if (LuaImmFallback::Enabled())
		LuaImmFallback::Count(r, LuaOpenGL::GetDrawMode(), numVerts);
}

void LuaImmediateBuffer::FlushLegacy() const
{
	if (verts.empty())
		return;

	// transforms via the fixed-function matrix the caller has already set;
	// replays texcoords when the stream was textured, and the ORIGINAL float
	// colors (not the quantized SColor), so it is an exact legacy equivalent
	// either way.
	assert(vertColorsF.size() == verts.size() * 4);
	GL::MaterializeFFState();

	glBegin(mode);
	for (size_t i = 0; i < verts.size(); ++i) {
		const VA_TYPE_TC& v = verts[i];
		if (textured)
			glTexCoord2f(v.s, v.t);
		GL::ffColor.Set(&vertColorsF[i * 4]);
		glVertex3f(v.pos.x, v.pos.y, v.pos.z);
	}
	glEnd();

	// exact legacy end-state: current color = last body glColor, or UNCHANGED
	// (= the inherited seed) when the body never called one -- the per-vertex
	// glColor replay above would otherwise leave the last vertex's quantized
	// color. Float precision so unclamped/overbright current colors round-trip.
	GL::ffColor.Set(sawColor ? lastColorF : seedColorF);
}

// Persistent twin of ImmFloatStream: same attribute layout, GL_STATIC_DRAW, and
// uploaded once. Kept here rather than in LuaCommandList.cpp so the layout can
// only ever be changed in one place alongside the shaders that consume it.
void LuaCommandList::StreamBake::Upload(uint32_t mode, const std::vector<float>& data)
{
	constexpr GLsizei STRIDE = 9 * sizeof(float);

	if (vao == 0) {
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>( 0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(12));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(20));
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	drawMode = mode;
	vertCount = static_cast<int32_t>(data.size() / 9);
}

void LuaCommandList::StreamBake::Draw() const
{
	if (vertCount <= 0)
		return;

	glBindVertexArray(vao);
	glDrawArrays(drawMode, 0, static_cast<GLsizei>(vertCount));
	glBindVertexArray(0);
}

void LuaCommandList::StreamBake::Release()
{
	// Runs from ~LuaCommandList, i.e. when the owning Lua handle drops the list.
	// That is on the draw thread with a live context; a null vao means the list
	// was never replayed and nothing was ever created.
	if (vao == 0)
		return;

	glDeleteVertexArrays(1, &vao);
	glDeleteBuffers(1, &vbo);
	vao = vbo = 0;
	vertCount = -1;
}

void LuaImmediateBuffer::FlushModern() const
{
	if (verts.empty())
		return;

	// A game-supplied shader is bound: it is what must draw the primitive, so
	// feed it rather than picking a program of this backend's own.
	if (GL::CurrentProgram() != 0) {
		if (!FlushIntoBoundShader(false)) {
			CountFallback(LuaImmFallback::REASON_BOUND_SHADER, verts.size());
			FlushLegacy();
		}
		return;
	}

	// A CPU-composed P*MV can ULP-differ from the driver's own on dense or
	// perspective matrices, which used to force these streams down the exact
	// legacy replay. They now take the builtin-MVP shader variant instead: the
	// driver composes the matrix, so the transform is exact for any matrices and
	// no legacy fixed-function replay is needed.
	const bool builtinMVP = !ScreenAlignedMV(mvMat) || !OrthoProjection(projMat);

	// Textured streams flush through the MODULATE shader when the FF texture
	// state is one it reproduces exactly (see PlainModulateTexturing); any
	// other texture-unit/texenv setup falls back to the exact legacy replay
	// (which carries the texcoords).
	// A stream can carry texcoords while FF texturing is OFF -- no enabled 2D
	// target, or one bound to an INCOMPLETE texture -- in which case FF ignores
	// them and rasterizes the plain vertex color. The untextured program
	// reproduces that exactly, so sample nothing instead of falling back (see
	// PlainModulateTexturing, which has already established that nothing else
	// textures the fragment). The FF current-texcoord end state below still
	// follows `textured`, because the legacy replay would have issued the
	// coords either way.
	bool sampleTexture = textured;
	if (textured) {
		const LuaImmFallback::Reason texReason = PlainModulateTexturing();
		if (texReason == LuaImmFallback::REASON_TEX_2D_DISABLED ||
		    texReason == LuaImmFallback::REASON_TEX_MIP_INCOMPLETE) {
			sampleTexture = false;
		} else if (texReason != LuaImmFallback::REASON_NONE) {
			CountFallback(texReason, verts.size());
			FlushLegacy();
			return;
		}
	}

	// Perspective projection: the divide amplifies CPU-vs-driver P*MV
	// composition ULPs into subpixel vertex shifts. Measured (command-list
	// gate, 2026-07-04) those shifts are invisible on flat fills but LINEAR
	// -sampled high-frequency textures (glyph caches, icons) turn them into
	// whole-shade deltas -- so the gate is on SAMPLING, not on the projection
	// alone: a stream that samples nothing may go modern under perspective.
	// This is checked after the texture gate because it needs sampleTexture,
	// so a stream that is both perspective and texenv-rejected now reports the
	// texenv reason rather than PERSPECTIVE_P.

	// the fogged shader variant replicates LINEAR fog only (the engine's map
	// fog); other modes keep the exact legacy replay. Fog that saturates at
	// factor 1 for the whole stream is inert and stays on the proven fogless
	// program.
	bool fogged = (glIsEnabled(GL_FOG) == GL_TRUE);
	if (fogged) {
		const GLint fogMode = GL::ffFog.Mode();
		if (fogMode != GL_LINEAR) {
			CountFallback(LuaImmFallback::REASON_FOG_MODE, verts.size());
			FlushLegacy();
			return;
		}
		fogged = FogIsEffective(mvMat, verts);
	}

	assert(vertColorsF.size() == verts.size() * 4);

	auto [idx, drawMode] = TriangulateIndicesForModern(mode, verts.size());
	if (idx.empty())
		return;

	// interleaved float stream: exact positions/uvs, and the exact float
	// colors legacy interpolates -- clamped to [0,1] per component as the FF
	// pipeline does before interpolation (overbright current colors stay
	// unclamped only in the CURRENT-color state, restored below)
	const auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
	std::vector<float> data;
	// already uploaded by an earlier replay of this same captured stream: the
	// interleave below is the expensive part for a big list, and its input is
	// immutable, so skip straight to the draw
	const bool haveBaked = (activeBake != nullptr) && activeBake->IsBuilt();
	if (!haveBaked) {
		data.reserve(idx.size() * 9);
		for (const uint32_t k : idx) {
			const VA_TYPE_TC& v = verts[k];
			data.insert(data.end(), {
				v.pos.x, v.pos.y, v.pos.z, v.s, v.t,
				c01(vertColorsF[k * 4 + 0]), c01(vertColorsF[k * 4 + 1]),
				c01(vertColorsF[k * 4 + 2]), c01(vertColorsF[k * 4 + 3]),
			});
		}
	}

	const CMatrix44f mvp = projMat * mvMat;

	Shader::IProgramObject* shader = sampleTexture ? GetModernTexShader(fogged, builtinMVP)
	                                               : GetModernShader(fogged, builtinMVP);
	shader->Enable();
	if (!builtinMVP)
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	if (fogged)
		shader->SetUniformMatrix4x4("uMV", false, static_cast<const float*>(mvMat));
	if (sampleTexture)
		shader->SetUniform("tex", 0);
	if (activeBake != nullptr) {
		if (!haveBaked)
			activeBake->Upload(drawMode, data);
		activeBake->Draw();
	} else {
		immStream.Draw(drawMode, data);
	}
	shader->Disable();

	// Exact legacy end-state. glBegin/glEnd leaves the FF current color at the
	// body's last glColor (or UNCHANGED when the body issued none), and the
	// compatibility profile exposes that as gl_Color to LATER shader draws --
	// BAR's gui_pip minimap shader reads its alpha, display-list replays
	// without recorded glColor inherit it. FlushModern draws via a shader and
	// never touches glColor, so replicate the exact legacy side effect
	// (apitrace-confirmed classes: the gui_pip minimap wash, the minimap
	// camera-box blue-channel divergence). Float precision so unclamped/
	// overbright current colors round-trip. The FF current texcoord went the
	// other way: priming it here was the last thing keeping glTexCoord2f alive.
	// Every immediate-mode emitter in the engine sets its own coords (audited),
	// so the only draw that could inherit this one is an UNTEXTURED legacy
	// replay sampling an enabled texture -- and the modern flush already draws
	// that case as a flat fill, so the two have never agreed on it anyway. The
	// one real inheritor left is CmdListEmitStreamIntoCompile's texSeed
	// vertices, inside a real display-list compile, which this configuration no
	// longer produces. Hence the knob: with it off, nothing moves at all.
	if (textured && !GL::FFRewriteEnabled())
		glTexCoord2f(verts.back().s, verts.back().t);
	GL::ffColor.Set(sawColor ? lastColorF : seedColorF);
}

bool LuaImmediateBuffer::FlushIntoBoundShader(bool isTexRect) const
{
	// investigation aid: declining here sends every bound-shader stream to the
	// exact legacy replay, which isolates this flush as the divergence source.
	// Env-gated; not for shipping.
	static const bool abDecline = getenv("AB_BOUNDSHADER_LEGACY") != nullptr;
	if (abDecline)
		return false;

	const uint32_t prog = GL::CurrentProgram();
	const GL::FFAttribBinding* ffb = GL::GetFFAttribBinding(prog);

	ReportBoundShaderOutcome(ffb != nullptr, prog);

	if (ffb == nullptr)
		return false;

	// No texture, fog or matrix gate here, unlike the no-shader flushes. Those
	// exist because this backend's OWN program has to reproduce what fixed
	// function would have done to the fragment; here the game's shader does its
	// own texturing, fogging and transform, exactly as it did when immediate
	// mode delivered the same vertices. All that has to match is the stream.
	const auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };

	std::vector<float> data;
	uint32_t drawMode = GL_TRIANGLES;

	if (isTexRect) {
		if (!texRect.set)
			return true;

		const float r = c01(texRect.cf[0]), g = c01(texRect.cf[1]);
		const float b = c01(texRect.cf[2]), a = c01(texRect.cf[3]);
		const float quad[4][9] = {
			{ texRect.x0, texRect.y0, 0.0f, texRect.s0, texRect.t0, r, g, b, a },
			{ texRect.x1, texRect.y0, 0.0f, texRect.s1, texRect.t0, r, g, b, a },
			{ texRect.x1, texRect.y1, 0.0f, texRect.s1, texRect.t1, r, g, b, a },
			{ texRect.x0, texRect.y1, 0.0f, texRect.s0, texRect.t1, r, g, b, a },
		};

		data.reserve(6 * 9);
		for (const int k : {0, 1, 2, 0, 2, 3})
			data.insert(data.end(), quad[k], quad[k] + 9);
	} else {
		if (verts.empty())
			return true;

		assert(vertColorsF.size() == verts.size() * 4);

		auto [idx, mode] = TriangulateIndicesForModern(this->mode, verts.size());
		if (idx.empty())
			return true;

		drawMode = mode;
		data.reserve(idx.size() * 9);
		for (const uint32_t k : idx) {
			const VA_TYPE_TC& v = verts[k];
			data.insert(data.end(), {
				v.pos.x, v.pos.y, v.pos.z, v.s, v.t,
				c01(vertColorsF[k * 4 + 0]), c01(vertColorsF[k * 4 + 1]),
				c01(vertColorsF[k * 4 + 2]), c01(vertColorsF[k * 4 + 3]),
			});
		}
	}

	GL::DrawFFAttribStream(drawMode, data.data(), data.size() / 9, *ffb);

	// same exact-legacy end state the no-shader flushes replicate: the fixed
	// function current color a glBegin/glEnd body would have left behind, which
	// later inheriting draws still read. This path only exists under the source
	// rewrite, so the current texcoord is never primed here -- see FlushModern.
	GL::ffColor.Set(isTexRect ? texRect.cf : (sawColor ? lastColorF : seedColorF));

	return true;
}

void LuaImmediateBuffer::FlushTexRectLegacy() const
{
	if (!texRect.set)
		return;

	GL::MaterializeFFState();

	// caller has bound the texture and enabled GL_TEXTURE_2D; FF MODULATE
	// gives texture * glColor (exact float color).
	GL::ffColor.Set(texRect.cf);
	glBegin(GL_QUADS);
		glTexCoord2f(texRect.s0, texRect.t0); glVertex2f(texRect.x0, texRect.y0);
		glTexCoord2f(texRect.s1, texRect.t0); glVertex2f(texRect.x1, texRect.y0);
		glTexCoord2f(texRect.s1, texRect.t1); glVertex2f(texRect.x1, texRect.y1);
		glTexCoord2f(texRect.s0, texRect.t1); glVertex2f(texRect.x0, texRect.y1);
	glEnd();
}

void LuaImmediateBuffer::FlushTexRectModern() const
{
	if (!texRect.set)
		return;

	// a TexRect carries no accumulated stream; it is always the one quad
	static constexpr size_t TEX_RECT_VERTS = 4;

	if (GL::CurrentProgram() != 0) {
		if (!FlushIntoBoundShader(true)) {
			CountFallback(LuaImmFallback::REASON_BOUND_SHADER, TEX_RECT_VERTS);
			FlushTexRectLegacy();
		}
		return;
	}

	// Dense modelview or perspective projection takes the builtin-MVP variant,
	// where the driver composes P*MV, rather than the exact legacy quad -- see
	// the same switch in FlushModern.
	const bool builtinMVP = !ScreenAlignedMV(mvMat) || !OrthoProjection(projMat);

	// incomplete texture: FF draws the flat current color, the shader would
	// With FF texturing OFF -- no GL_TEXTURE_2D enable, or one bound to an
	// INCOMPLETE texture, which disables the whole unit (GL 2.1 3.8.15) -- the
	// legacy quad rasterizes the flat current colour and samples nothing. The
	// UNTEXTURED program reproduces exactly that, so these cases go modern
	// instead of falling back; only the texcoords become irrelevant.
	const bool sampleTexture = (glIsEnabled(GL_TEXTURE_2D) == GL_TRUE) && !MipIncompleteTexture2D();

	// fogged shader variant for LINEAR fog only; other modes keep legacy (see
	// FlushModern); saturated fog stays on the proven fogless program
	bool fogged = (glIsEnabled(GL_FOG) == GL_TRUE);
	if (fogged) {
		const GLint fogMode = GL::ffFog.Mode();
		if (fogMode != GL_LINEAR) {
			CountFallback(LuaImmFallback::REASON_FOG_MODE, TEX_RECT_VERTS);
			FlushTexRectLegacy();
			return;
		}
		static constexpr SColor white = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
		const std::vector<VA_TYPE_TC> corners = {
			{ {texRect.x0, texRect.y0, 0.0f}, 0.0f, 0.0f, white },
			{ {texRect.x1, texRect.y1, 0.0f}, 0.0f, 0.0f, white },
		};
		fogged = FogIsEffective(mvMat, corners);
	}

	// caller has bound the texture to unit 0; the shader samples it (no
	// GL_TEXTURE_2D enable needed). Quad as two CCW triangles; the exact float
	// modulation color rides the float vertex-color attribute, clamped to
	// [0,1] per component as the FF pipeline does before interpolation.
	const auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
	const float r = c01(texRect.cf[0]), g = c01(texRect.cf[1]), b = c01(texRect.cf[2]), a = c01(texRect.cf[3]);

	const float quad[4][9] = {
		{ texRect.x0, texRect.y0, 0.0f, texRect.s0, texRect.t0, r, g, b, a }, // bl
		{ texRect.x1, texRect.y0, 0.0f, texRect.s1, texRect.t0, r, g, b, a }, // br
		{ texRect.x1, texRect.y1, 0.0f, texRect.s1, texRect.t1, r, g, b, a }, // tr
		{ texRect.x0, texRect.y1, 0.0f, texRect.s0, texRect.t1, r, g, b, a }, // tl
	};

	std::vector<float> data;
	data.reserve(6 * 9);
	for (const int k : {0, 1, 2, 0, 2, 3})
		data.insert(data.end(), quad[k], quad[k] + 9);

	const CMatrix44f mvp = projMat * mvMat;

	Shader::IProgramObject* shader = sampleTexture ? GetModernTexShader(fogged, builtinMVP)
	                                               : GetModernShader(fogged, builtinMVP);
	shader->Enable();
	if (!builtinMVP)
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	if (fogged)
		shader->SetUniformMatrix4x4("uMV", false, static_cast<const float*>(mvMat));
	if (sampleTexture)
		shader->SetUniform("tex", 0);
	immStream.Draw(GL_TRIANGLES, data);
	shader->Disable();

	// FlushTexRectLegacy sets glColor(texRect.cf); the compatibility profile
	// carries that FF current color into LATER draws (gl_Color). Replicate it so a
	// modern gl.TexRect is state-identical to legacy -- otherwise a following text
	// draw inherits a stale color (apitrace class: the minimap wash, here on the
	// countdown text). The current texcoord the legacy quad would have left at
	// (s0, t1) is no longer primed here -- see FlushModern.
	if (!GL::FFRewriteEnabled())
		glTexCoord2f(texRect.s0, texRect.t1);
	GL::ffColor.Set(texRect.cf);
}


namespace LuaGLCompare {
	static GLuint fboA = 0, texA = 0, fboB = 0, texB = 0, depthRB = 0;
	static int fbW = 0, fbH = 0;

	static void DeleteFBOs()
	{
		if (fboA)    glDeleteFramebuffers(1, &fboA);
		if (fboB)    glDeleteFramebuffers(1, &fboB);
		if (texA)    glDeleteTextures(1, &texA);
		if (texB)    glDeleteTextures(1, &texB);
		if (depthRB) glDeleteRenderbuffers(1, &depthRB);
		fboA = texA = fboB = texB = depthRB = 0;
	}

	static GLuint MakeColorTex(int w, int h)
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		return t;
	}

	static bool EnsureFBOs(int w, int h)
	{
		if (w == fbW && h == fbH && fboA != 0)
			return true;

		DeleteFBOs();
		if (w <= 0 || h <= 0)
			return false;

		texA = MakeColorTex(w, h);
		texB = MakeColorTex(w, h);

		glGenRenderbuffers(1, &depthRB);
		glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

		const auto makeFBO = [&](GLuint tex) {
			GLuint f = 0;
			glGenFramebuffers(1, &f);
			glBindFramebuffer(GL_FRAMEBUFFER, f);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
			return f;
		};
		fboA = makeFBO(texA);
		fboB = makeFBO(texB);

		const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		fbW = w; fbH = h;

		if (!ok) {
			DeleteFBOs();
			fbW = fbH = 0;
			return false;
		}
		return true;
	}

	int CompareDraws(int w, int h, const std::function<void()>& drawLegacy, const std::function<void()>& drawModern)
	{
		if (!EnsureFBOs(w, h))
			return -1;

		GLint prevFBO = 0;
		GLint prevVP[4] = {0, 0, 0, 0};
		GLfloat prevClear[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
		glGetIntegerv(GL_VIEWPORT, prevVP);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

		std::vector<uint8_t> a(static_cast<size_t>(w) * h * 4);
		std::vector<uint8_t> b(static_cast<size_t>(w) * h * 4);

		glViewport(0, 0, w, h);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		glBindFramebuffer(GL_FRAMEBUFFER, fboA);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		drawLegacy();
		glFinish(); // ensure the draw completes before readback (threaded sw rasterizers)
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, a.data());

		glBindFramebuffer(GL_FRAMEBUFFER, fboB);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		drawModern();
		glFinish(); // ensure the draw completes before readback (threaded sw rasterizers)
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, b.data());

		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
		glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
		glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);

		int maxDelta = 0;
		for (size_t i = 0; i < a.size(); ++i)
			maxDelta = std::max(maxDelta, std::abs(int(a[i]) - int(b[i])));

		// investigation aid (env-gated, not for shipping): dump the two arms of
		// the first few diverging compares as PPMs, so what each arm actually
		// drew is inspectable instead of inferred from a delta.
		static const bool dump = getenv("AB_COMPARE_DUMP") != nullptr;
		if (dump && maxDelta > 2) {
			static int dumped = 0;
			if (dumped < 6) {
				const auto writePPM = [&](const char* tag, const std::vector<uint8_t>& px) {
					char name[64];
					snprintf(name, sizeof(name), "abcmp_%d_%s.ppm", dumped, tag);
					FILE* f = fopen(name, "wb");
					if (f == nullptr)
						return;
					fprintf(f, "P6\n%d %d\n255\n", w, h);
					// glReadPixels rows are bottom-up; flip so the image views upright
					for (int y = h - 1; y >= 0; --y) {
						for (int x = 0; x < w; ++x)
							fwrite(&px[(static_cast<size_t>(y) * w + x) * 4], 1, 3, f);
					}
					fclose(f);
				};
				writePPM("legacy", a);
				writePPM("modern", b);
				LOG_L(L_WARNING, "[LuaGLCompare] dumped abcmp_%d_{legacy,modern}.ppm (max delta %d)", dumped, maxDelta);
				++dumped;
			}
		}
		return maxDelta;
	}
}
