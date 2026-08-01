/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CglFont;

// Phase-2 modern-GL migration: a CAPTURED gl.CreateList body (config
// LuaCommandLists). Instead of compiling a GL display list, the recordable GL
// entry points are POINTER-SWAPPED for recorders while the body executes once
// (exactly glNewList record semantics, at GL-call granularity, with zero
// Lua-level parsing changes); gl.CallList replays the commands through live
// GL + the FF matrix mirror + the modern immediate backend, so
//   - matrix ops keep the mirror in sync (no more taint on CallList),
//   - captured geometry flushes through the CURRENT backend,
//   - nothing records glUseProgram/glUniform (the uniform-cache class dies),
//   - glNewList/glCallList disappear for captured lists (RenderDoc path).
// A body that emits a recordable-but-unsupported call (glUseProgram,
// glPushAttrib, glNormal3f, ...) is MATERIALIZED mid-capture: a real GL list
// is opened, the already-captured commands are replayed into the compile, and
// the body continues recording legacy-style -- single body execution,
// per-list hybrid fallback.
struct LuaCommandList {
	enum class Op : uint8_t {
		Enable,            // u0 = cap
		Disable,           // u0 = cap
		BlendFunc,         // u0 = src, u1 = dst
		BlendFuncSeparate, // u0..u3 = srcRGB, dstRGB, srcA, dstA
		BlendEquation,     // u0 = mode
		BlendColor,        // f0..f3
		BindTexture,       // u0 = target, u1 = texID
		ActiveTexture,     // u0 = unit enum
		LineWidth,         // f0
		PointSize,         // f0
		LineStipple,       // u0 = factor, u1 = pattern
		DepthMask,         // u0 = flag
		DepthFunc,         // u0 = func
		CullFace,          // u0 = face
		AlphaFunc,         // u0 = func, f0 = ref
		PolygonMode,       // u0 = face, u1 = mode
		PolygonOffset,     // f0 = factor, f1 = units
		ColorMask,         // u0..u3 = r,g,b,a flags
		Scissor,           // u0..u3 = x,y,w,h (ints)
		ShadeModel,        // u0 = mode
		Fogf,              // u0 = pname, f0
		Fogi,              // u0 = pname, u1 = param (int)
		Fogfv,             // u0 = pname, f0..f3 (GL_FOG_COLOR is the 4-float case)
		Color,             // f0..f3 (also seeds subsequent stream vertices)
		TexCoord,          // f0, f1 (out-of-Begin/End current-texcoord set)
		MatrixMode,        // u0 = mode
		PushMatrix,
		PopMatrix,
		LoadIdentity,
		Translate,         // f0..f2
		Scale,             // f0..f2
		Rotate,            // f0 = deg, f1..f3 = axis
		MultMatrix,        // 16 floats in ext
		LoadMatrix,        // 16 floats in ext
		Rect,              // f0..f3 (glRectf)
		CallGLList,        // u0 = raw GL list id (nested real display list)
		ImmStream,         // u0 = index into streams
		Font,              // u0 = index into fontCmds
	};

	struct Cmd {
		Op op;
		uint32_t u0 = 0;
		uint32_t u1 = 0;
		uint32_t u2 = 0;
		uint32_t u3 = 0;
		float f[4] = { 0.0f };
		std::vector<float> ext; // MultMatrix/LoadMatrix payload
	};

	// a captured glBegin..glEnd run, replayable through LuaImmediateBuffer
	// (either backend). Vertices emitted before the LIST's first glColor are
	// seed-class: real display lists give them the CALL-time current color, so
	// the replay substitutes the replay-time color into the first seedVertCount
	// entries. texSeedVertCount marks vertices before the STREAM's first
	// glTexCoord (materialize emission omits texcoords for them, real-list
	// inherit semantics; the live flushes give them s=t=0 exactly like the live
	// modern gl.BeginEnd capture, gate-proven).
	struct ImmStreamData {
		uint32_t mode = 0;
		bool textured = false;         // any glTexCoord within this stream
		bool sawColor = false;         // any glColor within this stream
		uint32_t seedVertCount = 0;
		uint32_t texSeedVertCount = 0;
		std::vector<float> posUV;      // 5 floats per vertex (pos3 + st2)
		std::vector<float> colorsF;    // 4 floats per vertex (exact floats)
		float lastColorF[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float lastS = 0.0f, lastT = 0.0f;
	};

	// a captured font call (gl.Text / font:Print / font:Begin / ...): fonts
	// hit glUseProgram or glPushAttrib at draw time, which would otherwise
	// MATERIALIZE every text-bearing list (measured: ~180 real glNewList
	// compiles per frame of BAR UI). Captured at the Lua-call level instead
	// and replayed through the LIVE font renderer -- the shared_ptr keeps the
	// font alive for the lifetime of the list. FONT_BUFFERED prints are NOT
	// captured (their draw happens at SubmitBuffered time; they execute
	// normally and take the materialize path).
	struct FontCmd {
		enum class Kind : uint8_t {
			Print,            // text, x, y, size, options
			Begin,            // flag = userDefinedBlending
			End,
			TextColor,        // color
			OutlineColor,     // color
			AutoOutlineColor, // flag
		};
		std::shared_ptr<CglFont> font;
		Kind kind = Kind::Print;
		std::string text;
		float x = 0.0f, y = 0.0f, size = 0.0f;
		int options = 0;
		bool flag = false;
		float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	std::vector<Cmd> cmds;
	std::vector<ImmStreamData> streams;
	std::vector<FontCmd> fontCmds;

	// "file:line" of the gl.CreateList that captured this, for the fallback
	// census only (empty unless LuaImmediateFallbackStats). A replayed stream
	// reaches the flush with no Lua stack of its own, so without this its
	// fallbacks are attributed to whichever live draw ran last.
	std::string createSite;

	bool Empty() const { return cmds.empty(); }
};

// capture hooks for call sites outside LuaOpenGL.cpp (LuaFonts.cpp): active
// only while a gl.CreateList command-list capture is running
namespace LuaCmdListCapture {
	bool CapturingFonts();
	void RecordFont(LuaCommandList::FontCmd&& fc);

	// Lets GL-OBJECT CONSTRUCTION run for real inside a gl.CreateList body.
	//
	// A list body is a recording of drawing, but a first gl.Texture("...") in
	// one also CONSTRUCTS the texture, and construction is not drawing: under a
	// real display list glTexImage2D and glTexParameteri are compiled rather
	// than executed, so the object stays undefined until the first gl.CallList
	// and its parameters land on whatever happens to be bound then. The capture
	// inherited that, and materialized the list rather than record calls it
	// cannot replay -- 12 of the 14 real display lists in a BAR run come from
	// exactly this, none of them from Lua asking for it.
	//
	// Scope construction with this instead. It is the same treatment glGen*
	// and glBindBuffer already get by being left unwrapped, just applied where
	// the calls are too far from LuaOpenGL.cpp to leave unwrapped.
	//
	// Only suspends a live capture. Once a capture has materialized a real
	// glNewList is open and the calls compile into it, which is the legacy
	// path's own long-standing behaviour and not this scope's to change.
	struct SuspendScope {
		SuspendScope();
		~SuspendScope();
		SuspendScope(const SuspendScope&) = delete;
		SuspendScope& operator=(const SuspendScope&) = delete;
	private:
		bool suspended = false;
	};
}
