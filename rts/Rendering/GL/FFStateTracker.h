/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "Rendering/GL/myGL.h"

namespace GL {
	// CPU mirror of the fixed-function state LuaOpenGL::ResetGLState restores to
	// its GL default ahead of every draw callin.
	//
	// Those restores are why glShadeModel, glTexEnvi and glMaterial* appear in
	// every BAR frame, and a single call to any function on RenderDoc's
	// unsupported list permanently and silently disables capture for the whole
	// process (see doc/bar-gl4-immediate-mode-inventory.md). Content that never
	// moves a family off its GL default needs no restore for it -- and BAR moves
	// none of them: gl.Material, gl.ShadeModel and gl.TexEnv have zero call sites
	// in the game.
	//
	// Skipping a write of the value a state already holds cannot change any
	// rendering, so the equivalence here is a GL-semantics argument rather than a
	// pixel measurement. What has to hold is the tracking itself: that the mirror
	// says "clean" only when the real state is its default. Verify() checks
	// exactly that against GL and is driven by the same shadowCompare switch the
	// matrix mirror uses.
	struct FFResetState {
		// Shade model is a single global enum, so it can be tracked by value and
		// cleared again when a user restores GL_SMOOTH itself (CShadowHandler
		// brackets its pass with GL_FLAT/GL_SMOOTH and would otherwise leave the
		// next reset dirty forever).
		bool shadeModelIsSmooth = true;

		// Texture-env mode is per texture unit and materials are a 17-float tuple,
		// so a value mirror would have to model far more than the reset writes
		// back (the reset only ever touches the ACTIVE unit and GL_FRONT_AND_BACK).
		// Once anything writes either family, fall back to the original
		// unconditional restore for the rest of the process: that reproduces the
		// old behaviour exactly for content that uses them, and costs BAR nothing
		// because BAR never does.
		bool texEnvTouched = false;
		bool materialTouched = false;

		void NoteShadeModel(GLenum mode) { shadeModelIsSmooth = (mode == GL_SMOOTH); }
		void NoteTexEnv() { texEnvTouched = true; }
		void NoteMaterial() { materialTouched = true; }

		// glPushAttrib/glPopAttrib save and restore the shade model under
		// GL_LIGHTING_BIT, so a pop can change it with no set-call for the mirror
		// to observe. Mirroring the save-stack keeps it exact instead of merely
		// instrumented, and costs no GL calls. Every push records an entry --
		// including ones without the bit -- so pops stay paired with their own
		// push. The touched latches need no such handling: they are one-way, and a
		// pop can only restore a value some set-call already latched.
		//
		// Every attrib bracket that can carry GL_LIGHTING_BIT has to report here,
		// which includes the engine's own: LuaOpenGL::EnableCommon wraps each draw
		// callin in glPushAttrib(AttribBits), and AttribBits contains
		// GL_LIGHTING_BIT. Miss that one and a callin entered while the shade
		// model is GL_FLAT (CShadowHandler's pass) leaves the mirror claiming
		// GL_SMOOTH after the matching pop restores GL_FLAT.
		void NotePushAttrib(GLbitfield mask) {
			if (attribDepth < MAX_ATTRIB_DEPTH) {
				attribStack[attribDepth].savedIsSmooth = shadeModelIsSmooth;
				attribStack[attribDepth].savesShadeModel = ((mask & GL_LIGHTING_BIT) != 0);
			}
			++attribDepth;
		}
		void NotePopAttrib() {
			if (attribDepth == 0)
				return; // unbalanced; leave the mirror as-is and let Verify report
			--attribDepth;
			if (attribDepth >= MAX_ATTRIB_DEPTH) {
				shadeModelIsSmooth = false; // deeper than tracked: assume dirty, the restore runs
				return;
			}
			if (attribStack[attribDepth].savesShadeModel)
				shadeModelIsSmooth = attribStack[attribDepth].savedIsSmooth;
		}

		// The modern Lua immediate backend gates on GL_MODULATE; while no TexEnv
		// call has happened every unit is still at the GL default, which spares it
		// a glGetTexEnviv -- itself an unsupported function, so the gate was a
		// capture blocker in its own right.
		bool TexEnvIsKnownModulate() const { return !texEnvTouched; }

		// Only under shadowCompare: the queries below (glGetTexEnviv,
		// glGetMaterialfv) are unsupported functions too, so this must never run
		// in a capture or meter run.
		void Verify(const char* where) const;

	private:
		// GL guarantees only 16 attribute stack levels; anything past that is a
		// GL error in the caller, not a case to mirror.
		static constexpr int MAX_ATTRIB_DEPTH = 16;

		struct AttribEntry {
			bool savedIsSmooth = true;
			bool savesShadeModel = false;
		};
		AttribEntry attribStack[MAX_ATTRIB_DEPTH];
		int attribDepth = 0;
	};

	inline FFResetState ffResetState;
}
