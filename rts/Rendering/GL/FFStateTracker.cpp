/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/FFStateTracker.h"

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Log/ILog.h"

void GL::FFResetState::Verify(const char* where) const
{
	if (!GL::ffMirror.shadowCompare)
		return;

	GLint shadeModel = 0;
	glGetIntegerv(GL_SHADE_MODEL, &shadeModel);
	if ((shadeModel == GL_SMOOTH) != shadeModelIsSmooth) {
		LOG_L(L_ERROR, "[FFResetState::%s] shade-model mirror desync at %s: GL=0x%x mirror=%s",
			__func__, where, shadeModel, shadeModelIsSmooth ? "SMOOTH" : "not-SMOOTH");
	}

	// Only the clean claim needs checking -- once a family is marked touched the
	// unconditional restore runs and the mirror asserts nothing about it.
	if (!texEnvTouched) {
		GLint envMode = 0;
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envMode);
		if (envMode != GL_MODULATE) {
			LOG_L(L_ERROR, "[FFResetState::%s] tex-env mirror desync at %s: GL=0x%x mirror=MODULATE",
				__func__, where, envMode);
		}
	}

	if (!materialTouched) {
		static constexpr float defAmbient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
		static constexpr float defDiffuse[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
		static constexpr float defBlack[4]   = { 0.0f, 0.0f, 0.0f, 1.0f };

		const struct { GLenum pname; const float* def; int n; } checks[] = {
			{ GL_AMBIENT,   defAmbient, 4 },
			{ GL_DIFFUSE,   defDiffuse, 4 },
			{ GL_EMISSION,  defBlack,   4 },
			{ GL_SPECULAR,  defBlack,   4 },
		};

		for (const auto& c : checks) {
			float got[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			glGetMaterialfv(GL_FRONT, c.pname, got);
			for (int i = 0; i < c.n; ++i) {
				if (got[i] != c.def[i]) {
					LOG_L(L_ERROR, "[FFResetState::%s] material mirror desync at %s: pname=0x%x [%d] GL=%f mirror=%f",
						__func__, where, c.pname, i, got[i], c.def[i]);
					break;
				}
			}
		}

		float shininess = -1.0f;
		glGetMaterialfv(GL_FRONT, GL_SHININESS, &shininess);
		if (shininess != 0.0f) {
			LOG_L(L_ERROR, "[FFResetState::%s] material mirror desync at %s: shininess GL=%f mirror=0",
				__func__, where, shininess);
		}
	}
}
