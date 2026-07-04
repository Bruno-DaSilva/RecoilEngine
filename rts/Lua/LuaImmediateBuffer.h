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
class LuaImmediateBuffer {
public:
	enum class Backend { Legacy, Modern };

	void SetBackend(Backend b) { backend = b; }
	Backend GetBackend() const { return backend; }

	// MVP used by the modern backend's uMVP uniform (ignored by the legacy
	// backend, which reads the fixed-function matrix).
	void SetMVP(const CMatrix44f& m) { mvp = m; }

	void Begin(uint32_t glMode) {
		mode = glMode; verts.clear(); vertColorsF.clear();
		textured = false; curS = curT = 0.0f;
		sawColor = false; multiColor = false;
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
		for (int i = 0; i < 4; ++i) { curColorF[i] = rgba[i]; seedColorF[i] = rgba[i]; singleColorF[i] = rgba[i]; }
	}
	void Color(float r, float g, float b, float a) {
		curColorF[0] = r; curColorF[1] = g; curColorF[2] = b; curColorF[3] = a;
		lastColorF[0] = r; lastColorF[1] = g; lastColorF[2] = b; lastColorF[3] = a;
		if (verts.empty()) {
			// color set before any vertex replaces the seed as the (potential)
			// whole-stream color
			for (int i = 0; i < 4; ++i) singleColorF[i] = curColorF[i];
		} else if (curColorF[0] != singleColorF[0] || curColorF[1] != singleColorF[1] ||
		           curColorF[2] != singleColorF[2] || curColorF[3] != singleColorF[3]) {
			multiColor = true;
		}
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

	// textured quad (gl.TexRect). The texture is bound by the caller (as in
	// gl.Texture); the shading is MODULATE = texture * color (exact float via
	// the uColor uniform). As with BeginEnd, one SetTexRect can be flushed
	// either backend for an A/B compare.
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
	// the single color the whole stream uses (exact float) -- valid while
	// !multiColor; lets the modern flush route the color through a float
	// uniform instead of the quantized 8-bit vertex attribute, which keeps
	// animated fade alphas bit-exact vs the legacy float pipeline
	float singleColorF[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool multiColor = false;
	bool sawColor = false; // any Color() (as opposed to SeedColor) since Begin()
	std::vector<float> vertColorsF; // 4 floats per vertex; exact legacy replay colors
	float curS = 0.0f, curT = 0.0f;
	bool textured = false; // any TexCoord seen this Begin()
	CMatrix44f mvp;
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
