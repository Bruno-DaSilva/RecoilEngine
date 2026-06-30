/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef GL_MATRIX_STATE_TRACKER_H
#define GL_MATRIX_STATE_TRACKER_H

#include <array>
#include <vector>

#include "System/Matrix44f.h"
#include "System/MathConstants.h"
#include "System/Log/ILog.h"

#ifndef UNIT_TEST
	#include "Rendering/GL/myGL.h"
#else
	// myGL.h is deliberately unavailable to unit tests (it pulls in the GL
	// loader). The tracker's value-stack math is pure CPU and only needs the
	// three matrix-mode enum values; provide them locally so the Tier-1 unit
	// tests can exercise it without a GL context.
	#ifndef GL_MODELVIEW
		#define GL_MODELVIEW  0x1700
		#define GL_PROJECTION 0x1701
		#define GL_TEXTURE    0x1702
	#endif
#endif

struct SMatrixStateData {
	SMatrixStateData(): mode(GL_MODELVIEW),
						modelView(0),
						projection(0),
						texture(0) {}
	int mode;
	int modelView;
	int projection;
	int texture;
};

struct GLMatrixStateTracker {
public:
	SMatrixStateData matrixData; // [>0] = stack depth for mode, [0] = matrix mode
	bool listMode; // if creating display list

private:
	// CPU mirror of the fixed-function matrix values (Phase 1 of the modern-GL
	// migration, see doc/bar-gl4-immediate-mode-inventory.md). One current
	// matrix plus a push/pop stack per mode; indexed by ModeToIndex(). The
	// depth counters in matrixData remain the canonical balance check and are
	// kept in sync with these stacks.
	struct ModeStack {
		CMatrix44f current;
		std::vector<CMatrix44f> stack;
	};
	std::array<ModeStack, 3> modeStacks;

	static int ModeToIndex(unsigned int mode) {
		switch (mode) {
			case GL_MODELVIEW:  return 0;
			case GL_PROJECTION: return 1;
			case GL_TEXTURE:    return 2;
			default:            return 0;
		}
	}

	ModeStack& CurStack() { return modeStacks[ModeToIndex(GetMode())]; }
	const ModeStack& CurStack() const { return modeStacks[ModeToIndex(GetMode())]; }

public:
	GLMatrixStateTracker() : listMode(false) {}

	SMatrixStateData PushMatrixState() {
		SMatrixStateData md;
		std::swap(matrixData, md);
		return md;
	}

	SMatrixStateData PushMatrixState(bool lm) {
		listMode = lm;
		return PushMatrixState();
	}

	void PopMatrixState(SMatrixStateData& md) {
		std::swap(matrixData, md);
	}

	void PopMatrixState(SMatrixStateData& md, bool lm) {
		listMode = lm;
		PopMatrixState(md);
	}

	unsigned int GetMode() const {
		return matrixData.mode;
	}

	int& GetDepth(unsigned int mode) {
		switch (mode) {
			case GL_MODELVIEW: return matrixData.modelView;
			case GL_PROJECTION: return matrixData.projection;
			case GL_TEXTURE: return matrixData.texture;
			default:
				LOG_L(L_ERROR, "unknown matrix mode = %u", mode);
				abort();
				break;
		}
	}

	bool PushMatrix() {
		unsigned int mode = GetMode();
		int& depth = GetDepth(mode);
		if (!listMode && depth >= 255)
			return false;
		depth += 1;
		CurStack().stack.push_back(CurStack().current);
		return true;
	}

	bool PopMatrix() {
		unsigned int mode = GetMode();
		int& depth = GetDepth(mode);
		if (listMode) {
			depth -= 1;
			RestoreTopMatrix();
			return true;
		}
		if (depth == 0)
			return false;
		depth -= 1;
		RestoreTopMatrix();
		return true;
	}

	bool SetMatrixMode(int mode) {
		if (mode == GL_MODELVIEW || mode == GL_PROJECTION || mode == GL_TEXTURE) {
			matrixData.mode = mode;
			return true;
		}
		return false;
	}

	// --- matrix value ops (OpenGL fixed-function semantics) ---
	// All transform ops post-multiply the active mode's current matrix
	// (M' = M * Op), so a vertex transforms as M*Op*v, matching glTranslatef
	// et al. The legacy GL calls in LuaOpenGL still run; these keep a CPU copy
	// so a re-backed primitive can hand its shader an MVP.

	const CMatrix44f& GetMatrix() const { return CurStack().current; }
	const CMatrix44f& GetMatrix(unsigned int mode) const { return modeStacks[ModeToIndex(mode)].current; }

	void LoadIdentity() { CurStack().current.LoadIdentity(); }
	void LoadMatrix(const CMatrix44f& m) { CurStack().current = m; }
	void MultMatrix(const CMatrix44f& m) { CurStack().current = CurStack().current * m; }

	void Translate(float x, float y, float z) { CurStack().current.Translate(x, y, z); }
	void Scale(float x, float y, float z) { CurStack().current.Scale(float3(x, y, z)); }

	void Rotate(float angleDeg, float x, float y, float z) {
		float3 axis(x, y, z);
		axis.Normalize(); // glRotatef normalizes its axis
		CMatrix44f rot;
		rot.Rotate(angleDeg * (math::PI / 180.0f), axis);
		CurStack().current = CurStack().current * rot;
	}

	void Ortho(double l, double r, double b, double t, double n, double f) {
		CurStack().current = CurStack().current * CMatrix44f::OrthoProj(
			float(l), float(r), float(b), float(t), float(n), float(f));
	}

	void Frustum(double l, double r, double b, double t, double n, double f) {
		CurStack().current = CurStack().current * CMatrix44f::PerspProj(
			float(l), float(r), float(b), float(t), float(n), float(f));
	}


	int ApplyMatrixState(SMatrixStateData& m) {
		// validate
#define VALIDATE(modeName) \
		{ \
			int newDepth = m.modeName + matrixData.modeName; \
			if (newDepth < 0) \
				return -1; \
			if (newDepth >= 255) \
				return 1; \
		}

		VALIDATE(modelView)
		VALIDATE(projection)
		VALIDATE(texture)

#undef VALIDATE

		// apply
		matrixData.mode = m.mode;
		matrixData.modelView += m.modelView;
		matrixData.projection += m.projection;
		matrixData.texture += m.texture;

		return 0;
	}

	const SMatrixStateData& GetMatrixState() const {
		return matrixData;
	}

	bool HasMatrixStateError() const {
		return matrixData.mode != GL_MODELVIEW ||
			matrixData.modelView != 0 ||
			matrixData.projection != 0 ||
			matrixData.texture != 0;
	}

#ifndef UNIT_TEST
	void HandleMatrixStateError(int error, const char* errsrc) {
		unsigned int mode = GetMode();

		// dont complain about stack/mode issues if some other error occurred
		// check if the lua code did not restore the matrix mode
		if (error == 0 && mode != GL_MODELVIEW)
			LOG_L(L_ERROR, "%s: OpenGL state check error, matrix mode = %d, please restore mode to GL.MODELVIEW before end", errsrc, mode);


#define CHECK_MODE(modeName, glMode) \
		assert(matrixData.modeName >= 0); \
		if (matrixData.modeName != 0) {\
			if (error == 0){ \
				LOG_L(L_ERROR, "%s: OpenGL stack check error, matrix mode = %s, depth = %d, please make sure to pop all matrices before end", errsrc, #glMode, matrixData.modeName); \
			} \
			glMatrixMode(glMode); \
			for (int p = 0; p < matrixData.modeName; ++p) { \
				glPopMatrix(); \
			} \
			matrixData.modeName = 0;\
		}

		CHECK_MODE(modelView, GL_MODELVIEW)
		CHECK_MODE(projection, GL_PROJECTION)
		CHECK_MODE(texture, GL_TEXTURE)

#undef CHECK_MODE

		glMatrixMode(GL_MODELVIEW);
	}
#endif // !UNIT_TEST

private:
	void RestoreTopMatrix() {
		ModeStack& ms = CurStack();
		if (!ms.stack.empty()) {
			ms.current = ms.stack.back();
			ms.stack.pop_back();
		}
	}
};

#endif
