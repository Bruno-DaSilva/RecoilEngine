/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/Matrix44f.h"

namespace GL {
	// Feed GL::ffMirror from the glad entry points, so it mirrors the WHOLE
	// fixed-function matrix state rather than the part LuaOpenGL happens to
	// replay into it.
	//
	// That gap is what stands between the matrix family and retirement. The
	// mirror is fed today only by the gl.* matrix callouts and the per-callin
	// scaffolding's seeds; every matrix call the engine makes for itself --
	// CCamera::Update, CModelDrawerHelper::PushTransform, and the 130k
	// glMultMatrixf a run from S3DModelPiece::DrawStaticLegacy -- is invisible to
	// it. A mirror that cannot describe a model draw cannot replace the builtin
	// for one.
	//
	// glad has one pointer per entry point, so wrapping there sees every call
	// from anywhere, which is the same property that made the draw census and the
	// uniform feed exact. It also makes LuaOpenGL's own replay redundant, and
	// FFMirrorOps() stands down while this is installed -- applying each op twice
	// would be worse than not applying it at all.
	//
	// Installed only under the migration knob, since it changes what the mirror
	// contains and anything reading it.
	void InstallFFMatrixTracking();
	bool FFMatrixTrackingInstalled();

	// Compare the mirrored matrix for the active mode against glGetFloatv, EXACTLY
	// -- the existing shadow-compare in LuaOpenGL uses a 1e-4 relative tolerance,
	// which is not the bar for replacing a builtin. Runs only under
	// ffMirror.shadowCompare (glGetFloatv per matrix op is not free) and reports
	// the op that diverged, since which one it is decides where the tracker is
	// wrong.
	void VerifyFFMatrixMirror(const char* op);

	// The fixed-function matrices, from wherever they currently live: GL while
	// the set-calls still reach it, the mirror once they no longer do. Every
	// glGetFloatv(GL_*_MATRIX) in the engine goes through here, because after
	// suppression that query returns whatever GL was last left holding -- an
	// identity, in most cases -- and reads exactly like a working one.
	void ReadFFMatrix(unsigned int mode, CMatrix44f& out);
	void ReadFFMatrices(CMatrix44f& proj, CMatrix44f& modelView);

	// Stop issuing the fixed-function matrix calls entirely. Only under the
	// migration knob, and only once every consumer above is on the mirror: with
	// it on, the ten matrix functions are the last RenderDoc-unsupported calls a
	// BAR frame makes, and this is what removes them.
	bool FFMatrixSuppressed();

	// Load the mirrored matrices back into the fixed-function stack, for a draw
	// that fell back to fixed function and will be transformed by it. Those draws
	// are issuing glBegin or client-array calls anyway, so the matrix calls this
	// costs are on a path the capture had already lost.
	void MaterializeFFMatrices();
}
