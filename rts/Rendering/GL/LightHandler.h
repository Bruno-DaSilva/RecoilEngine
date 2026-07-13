/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _GL_LIGHTHANDLER_H
#define _GL_LIGHTHANDLER_H

#include <vector>

#include "Light.h"

namespace Shader {
	struct IProgramObject;
}

namespace GL {
	struct LightHandler {
	public:
		LightHandler(): baseLight(0), maxLights(0), numLights(0), lightHandle(0) {}
		~LightHandler() { Kill(); }

		void Init(unsigned int, unsigned int);
		void Kill() { lights.clear(); }
		void Update(Shader::IProgramObject*);

		unsigned int AddLight(const GL::Light&);
		unsigned int SetLight(unsigned int lgtIndex, const GL::Light&);

		GL::Light* GetLight(unsigned int lgtHandle);

		// PR 27b: under the sim|draw split the Lua track callouts skip the
		// DEPENDENCE_LIGHT registration (cross-thread listener mutation +
		// sim-thread delivery into draw-owned lights); the boundary calls
		// this instead for every drained destroy. The tracked pointer stays
		// readable until then (deferred-deletion shell).
		void DeliverBoundaryDeath(const CWorldObject* obj) {
			for (GL::Light& light: lights) {
				if (light.GetTrackObject() == obj)
					light.DependentDied(nullptr);
			}
		}

		unsigned int GetBaseLight() const { return baseLight; }
		unsigned int GetMaxLights() const { return maxLights; }

	private:
		std::vector<GL::Light> lights;

		unsigned int baseLight;
		unsigned int maxLights;
		unsigned int numLights;
		unsigned int lightHandle;
	};
}

#endif // _GL_LIGHTHANDLER_H
