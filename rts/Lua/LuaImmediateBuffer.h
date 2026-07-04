/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_IMMEDIATE_BUFFER_H
#define LUA_IMMEDIATE_BUFFER_H

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

	void Begin(uint32_t glMode) { mode = glMode; verts.clear(); textured = false; curS = curT = 0.0f; sawColor = false; }
	// the color the stream INHERITS from outside the Begin/End body (the FF
	// current color, which legacy glBegin picks up implicitly). Unlike Color()
	// this does not count as a body color: legacy leaves the FF current color
	// UNCHANGED when the body never calls glColor, and the flushes replicate
	// exactly that (see FlushModern's trailing glColor).
	void SeedColor(const SColor& c) { curColor = c; seedColor = c; }
	void Color(float r, float g, float b, float a) { Color(SColor(r, g, b, a)); }
	void Color(const SColor& c) { curColor = c; lastColor = c; sawColor = true; }
	void TexCoord(float s, float t) { curS = s; curT = t; textured = true; }
	void Vertex(float x, float y, float z) { verts.push_back(VA_TYPE_TC{float3{x, y, z}, curS, curT, curColor}); }

	// flush via the active backend and reset the vertex list.
	void End() { Flush(backend); verts.clear(); }

	// flush a given backend WITHOUT clearing, so the same accumulated scene can
	// be rendered both ways for an A/B compare.
	void Flush(Backend b) const { (b == Backend::Legacy) ? FlushLegacy() : FlushModern(); }
	void FlushLegacy() const;
	void FlushModern() const;

	// textured quad (gl.TexRect). The texture is bound by the caller (as in
	// gl.Texture); the shading is MODULATE = texture * color. As with BeginEnd,
	// one SetTexRect can be flushed either backend for an A/B compare.
	void SetTexRect(float x0, float y0, float x1, float y1,
	                float s0, float t0, float s1, float t1, const SColor& c) {
		texRect = TexRectData{x0, y0, x1, y1, s0, t0, s1, t1, c, true};
	}
	void FlushTexRect(Backend b) const { (b == Backend::Legacy) ? FlushTexRectLegacy() : FlushTexRectModern(); }
	void FlushTexRectLegacy() const;
	void FlushTexRectModern() const;

	void Clear() { verts.clear(); }
	bool Empty() const { return verts.empty(); }
	bool IsTextured() const { return textured; }
	uint32_t GetMode() const { return mode; }
	const std::vector<VA_TYPE_TC>& GetVerts() const { return verts; }

private:
	struct TexRectData {
		float x0, y0, x1, y1;
		float s0, t0, s1, t1;
		SColor c;
		bool set = false;
	};

	Backend backend = Backend::Legacy;
	uint32_t mode = 0;
	SColor curColor = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
	SColor seedColor = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
	SColor lastColor = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
	bool sawColor = false; // any Color() (as opposed to SeedColor) since Begin()
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
