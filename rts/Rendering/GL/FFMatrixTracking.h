/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

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
}
