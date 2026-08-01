/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

namespace GL {
	// CPU mirror of the fixed-function CURRENT COLOR, and the only writer of it.
	//
	// glColor* is on RenderDoc's unsupported list, and one call to any function on
	// it permanently and silently disables capture for the whole process. The
	// colour itself is still needed: shaders read it as gl_Color, and the engine
	// reads it back to hand to its own stand-in shaders. So keep the value and
	// stop keeping it in GL.
	//
	// Where it goes instead is a GENERIC vertex attribute's current value, which
	// has exactly the semantics gl_Color has -- per-vertex when an array is
	// enabled for that slot, one global value otherwise -- provided every
	// rewritten program agrees on the slot. GL::RewriteFFVertexBuiltins pins it
	// (FF_COLOR_ATTRIB_LOC) for that reason: a queried location differs per
	// program and cannot carry a value set before the program was bound.
	//
	// With the rewrite off this still calls glColor4fv, so a game that never
	// enables it is byte-for-byte unaffected, mirror and GL agree, and Verify()
	// can prove the mirror sees every writer.
	struct FFColorMirror {
		// Write the colour into GL as well, because a fixed-function draw may
		// still read it there. Set from LuaOpenGL::SetModernImmediate: the legacy
		// immediate backend transforms and shades through fixed function, so it
		// is the one remaining reader, and it is also what the whole-frame A/B
		// gate renders on three of its four passes. Those paths issue glBegin
		// anyway, so the glColor4fv costs a capture that was already lost.
		//
		// The engine's own glBegin drawers (HUDDrawer, HAPFSPathDrawer, DynWater,
		// GuiHandler's build menu) would need the same, and do not get it: the
		// fixed-function draw census reaches none of them, and any that is ever
		// reached blocks capture through glBegin first, so the meter names it
		// before the colour matters.
		bool writeThrough = true;

		void Set(float r, float g, float b, float a = 1.0f);
		void Set(const float* rgba) { Set(rgba[0], rgba[1], rgba[2], rgba[3]); }
		// the glColor3ub/glColor4ub spellings: unsigned bytes are the [0,1] range
		// in 255ths, which is the conversion GL does for them. Named apart from
		// Set so an integer literal cannot pick it by accident.
		void SetUB(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { Set(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

		// Replaces glGetFloatv(GL_CURRENT_COLOR) -- itself supported, but it reads
		// the state this no longer writes.
		const float* Get() const { return current; }

		// glPushAttrib(GL_CURRENT_BIT) saves and restores the colour with no
		// set-call for the mirror to observe. The engine's own brackets are
		// explicit save/restore and go through Set(), so this is for gl.PushAttrib,
		// which still pushes for real.
		void NotePushAttrib(unsigned int mask); // GLbitfield
		void NotePopAttrib();

		// Only meaningful with the rewrite off, where Set() also writes GL: it
		// reports any writer that bypassed the mirror, which is the one way this
		// can be wrong once the rewrite is on and GL stops being the oracle.
		void Verify(const char* where) const;

	private:
		static constexpr int MAX_ATTRIB_DEPTH = 16;

		float current[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float stack[MAX_ATTRIB_DEPTH][4] = {};
		bool savesColor[MAX_ATTRIB_DEPTH] = {};
		int depth = 0;
	};

	inline FFColorMirror ffColor;
}
