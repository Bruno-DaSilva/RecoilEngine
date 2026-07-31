/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_IMMEDIATE_BUFFER_H
#define LUA_IMMEDIATE_BUFFER_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "Rendering/GL/VertexArrayTypes.h"
#include "System/Color.h"
#include "System/Matrix44f.h"

// lua_State-free immediate-mode emitter for the modern-GL migration
// (doc/bar-gl4-immediate-mode-inventory.md).
//
// It accumulates a pos+color immediate-mode vertex stream (current color is
// sticky, OpenGL-style), then flushes it one of two ways:
//   - Legacy: glBegin/glColor/glVertex (the deprecated path; transforms via the
//     fixed-function matrix the caller has set).
//   - Modern: GL::TypedRenderBuffer + a uniform-mat4-MVP shader that makes NO
//     fixed-function matrix calls (RenderDoc-clean); the MVP is supplied via
//     SetMVP (in the engine, from GLMatrixStateTracker).
//
// Because the SAME accumulated stream drives both flushes, the two paths can be
// A/B compared with identical input by construction — that is the whole point:
// it makes "the modern path renders the same" a measured property, and lets the
// production code pick a backend by flag with the other as the oracle/fallback.
//
// This first iteration handles pos+color (VA_TYPE_C) and the core primitive
// modes that map straight to the core profile; QUADS/POLYGON triangulation,
// textures (TexRect), normals, and the lua-side wiring come in later iterations.
// Why a modern flush took the exact-legacy replay instead, tallied per Lua draw
// mode (config LuaImmediateFallbackStats, dumped by "/luaimmfallback"). The
// census in doc/bar-gl4-immediate-mode-inventory.md counts the residual legacy
// calls but cannot say WHICH gate rejected a stream; these do.
// [*] marks the SAMPLES-NOTHING reasons: FF textures the fragment with nothing
// at all, which the untextured program reproduces, so the caller draws those
// modern rather than falling back and they never reach the census.
namespace LuaImmFallback {
	enum Reason {
		REASON_DENSE_MV = 0,        // modelview has rotation/shear terms
		REASON_PERSPECTIVE_P,       // projection is not ortho-like
		REASON_TEX_ACTIVE_UNIT,     // active texture unit is not unit 0
		REASON_TEX_2D_DISABLED,     // GL_TEXTURE_2D not enabled [*]
		REASON_TEX_TARGET_PRIORITY, // cube/rect/3D target outranks 2D
		REASON_TEX_TEXGEN,          // texgen replaces the captured texcoords
		REASON_TEX_ENV_MODE,        // texenv mode is not MODULATE
		REASON_TEX_MATRIX,          // non-identity texture matrix
		REASON_TEX_MIP_INCOMPLETE,  // mip-filtered texture with no mip chain [*]
		REASON_TEX_ALPHA_FORMAT,    // GL_ALPHA internal format
		REASON_TEX_MULTI_UNIT,      // another FF texture unit is enabled
		REASON_FOG_MODE,            // fog mode is not LINEAR
		REASON_NONE,                // gate passed (not counted)
		REASON_COUNT = REASON_NONE
	};

	bool Enabled();
	// drawMode is a LuaOpenGL::DrawMode; taken as int to keep this header free
	// of the LuaOpenGL dependency
	void Count(Reason r, int drawMode, size_t numVerts);
	void Dump();
}

class LuaImmediateBuffer {
public:
	enum class Backend { Legacy, Modern };

	void SetBackend(Backend b) { backend = b; }
	Backend GetBackend() const { return backend; }

	// Projection and modelview for the modern backend's uP/uMV uniforms
	// (ignored by the legacy backend, which reads the fixed-function matrix).
	// Kept SEPARATE: the fixed-function pipeline transforms P * (MV * v) in two
	// steps, which differs from a CPU-precomposed (P*MV) * v by final-bit ULPs
	// on world-scale coordinates -- enough to flip pixels on thin primitives
	// and polygon edges (apitrace-verified on the whole-frame A/B gate). The
	// shaders replicate the exact FF operation order.
	void SetMatrices(const CMatrix44f& p, const CMatrix44f& mv) { projMat = p; mvMat = mv; }
	// single-matrix convenience (tests, callers with a premade transform):
	// uP*(uMV*v) with MV=identity == m*v exactly
	void SetMVP(const CMatrix44f& m) { projMat = m; mvMat = CMatrix44f{}; }

	void Begin(uint32_t glMode) {
		mode = glMode; verts.clear(); vertColorsF.clear();
		textured = false; curS = curT = 0.0f;
		sawColor = false;
	}
	// The FF pipeline clamps vertex colors to [0,1] at rasterization while the
	// CURRENT-color state stays unclamped (glGetFloatv returns e.g. 1.15 -- BAR
	// widgets use overbright line colors); SColor's raw float->uint8 conversion
	// would WRAP such components (1.15*255 = 293 -> 37, seen as the minimap
	// camera-box blue-channel divergence under the A/B gate). Always quantize
	// vertex colors through this clamp, rounding to nearest as FF does.
	static SColor ClampedColor(float r, float g, float b, float a) {
		const auto c = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		return SColor(c(r), c(g), c(b), c(a));
	}
	// the color the stream INHERITS from outside the Begin/End body (the FF
	// current color, which legacy glBegin picks up implicitly). Unlike Color()
	// this does not count as a body color: legacy leaves the FF current color
	// UNCHANGED when the body never calls glColor, and the flushes replicate
	// exactly that (see the trailing glColor4fv -- kept in FLOAT precision so
	// unclamped/unquantizable current colors survive the round-trip).
	void SeedColor(const float* rgba) {
		for (int i = 0; i < 4; ++i) { curColorF[i] = rgba[i]; seedColorF[i] = rgba[i]; }
	}
	void Color(float r, float g, float b, float a) {
		curColorF[0] = r; curColorF[1] = g; curColorF[2] = b; curColorF[3] = a;
		lastColorF[0] = r; lastColorF[1] = g; lastColorF[2] = b; lastColorF[3] = a;
		sawColor = true;
	}
	void Color(const SColor& c) { Color(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f); }
	void TexCoord(float s, float t) { curS = s; curT = t; textured = true; }
	void Vertex(float x, float y, float z) {
		verts.push_back(VA_TYPE_TC{float3{x, y, z}, curS, curT,
		                ClampedColor(curColorF[0], curColorF[1], curColorF[2], curColorF[3])});
		vertColorsF.insert(vertColorsF.end(), {curColorF[0], curColorF[1], curColorF[2], curColorF[3]});
	}

	// flush via the active backend and reset the vertex list.
	void End() { Flush(backend); verts.clear(); }

	// flush a given backend WITHOUT clearing, so the same accumulated scene can
	// be rendered both ways for an A/B compare.
	void Flush(Backend b) const { (b == Backend::Legacy) ? FlushLegacy() : FlushModern(); }
	void FlushLegacy() const;
	void FlushModern() const;

	// tally one modern->legacy fallback against the current Lua draw mode
	void CountFallback(LuaImmFallback::Reason r, size_t numVerts) const;

	// textured quad (gl.TexRect). The texture is bound by the caller (as in
	// gl.Texture); the shading is MODULATE = texture * color (exact float via
	// the float vertex-color attribute). As with BeginEnd, one SetTexRect can
	// be flushed either backend for an A/B compare.
	void SetTexRect(float x0, float y0, float x1, float y1,
	                float s0, float t0, float s1, float t1, const float* rgba) {
		texRect = TexRectData{x0, y0, x1, y1, s0, t0, s1, t1,
		                      {rgba[0], rgba[1], rgba[2], rgba[3]}, true};
	}
	void FlushTexRect(Backend b) const { (b == Backend::Legacy) ? FlushTexRectLegacy() : FlushTexRectModern(); }
	void FlushTexRectLegacy() const;
	void FlushTexRectModern() const;

	void Clear() { verts.clear(); vertColorsF.clear(); }
	bool Empty() const { return verts.empty(); }
	bool IsTextured() const { return textured; }
	uint32_t GetMode() const { return mode; }
	const std::vector<VA_TYPE_TC>& GetVerts() const { return verts; }

	// command-list capture support (gl.CreateList body baking): expose the
	// exact state a later LoadCaptured must restore for the flushes to behave
	// identically to a live End().
	bool GetSawColor() const { return sawColor; }
	const float* GetSeedColorF() const { return seedColorF; }
	const float* GetLastColorF() const { return lastColorF; }
	const std::vector<float>& GetVertColorsF() const { return vertColorsF; }

	// rebuild the accumulated stream from baked data (posUV = 5 floats/vertex
	// pos3+st2, colors = 4 floats/vertex); after this a Flush(backend) renders
	// exactly what the captured body accumulated
	void LoadCaptured(uint32_t glMode, bool tex, size_t n,
	                  const float* posUV, const float* colors,
	                  bool saw, const float* seed, const float* last) {
		Begin(glMode);
		textured = tex;
		sawColor = saw;
		for (int i = 0; i < 4; ++i) { seedColorF[i] = seed[i]; lastColorF[i] = last[i]; }
		verts.reserve(n);
		vertColorsF.assign(colors, colors + n * 4);
		for (size_t i = 0; i < n; ++i) {
			const float* v = posUV + i * 5;
			const float* c = colors + i * 4;
			verts.push_back(VA_TYPE_TC{float3{v[0], v[1], v[2]}, v[3], v[4],
			                ClampedColor(c[0], c[1], c[2], c[3])});
		}
	}

private:
	struct TexRectData {
		float x0, y0, x1, y1;
		float s0, t0, s1, t1;
		float cf[4];
		bool set = false;
	};

	Backend backend = Backend::Legacy;
	uint32_t mode = 0;
	float curColorF[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
	float seedColorF[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
	float lastColorF[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool sawColor = false; // any Color() (as opposed to SeedColor) since Begin()
	// 4 floats per vertex: the EXACT colors the legacy pipeline sees. Both
	// flushes consume these -- the modern flush streams them as a float vertex
	// attribute (clamped to [0,1] as FF does pre-interpolation), so vertex
	// colors are bit-exact by construction; the quantized 8-bit VA_TYPE_TC
	// color is kept only for GetVerts() introspection. (8-bit vertex colors
	// were the last quantization class: per-quad-colored glow stacks under
	// additive blending amplified the +-0.5 LSB rounding to visible deltas.)
	std::vector<float> vertColorsF;
	float curS = 0.0f, curT = 0.0f;
	bool textured = false; // any TexCoord seen this Begin()
	CMatrix44f projMat;
	CMatrix44f mvMat;
	std::vector<VA_TYPE_TC> verts; // superset; s/t unused when !textured
	TexRectData texRect;
};

// LuaGLCompareMode support: render two draws (legacy vs modern of the same
// primitive) into matching offscreen RGBA8 FBOs and report how far apart they
// are. Validates the wired modern path against legacy over real frames.
namespace LuaGLCompare {
	// Render drawLegacy and drawModern into two w*h FBOs (the caller's viewport)
	// over a common cleared background, read both back, and return the max abs
	// per-byte delta; returns -1 if the FBOs could not be created. Saves and
	// restores the bound framebuffer, viewport and clear color.
	int CompareDraws(int w, int h, const std::function<void()>& drawLegacy, const std::function<void()>& drawModern);
}

#endif // LUA_IMMEDIATE_BUFFER_H
