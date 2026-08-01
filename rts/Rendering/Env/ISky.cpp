/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include <algorithm>
#include "ISky.h"
#include "NullSky.h"
#include "SkyBox.h"
#include "ModernSky.h"
#include "Game/Camera.h"
#include "Game/TraceRay.h"
#include "Map/MapInfo.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Env/DebugCubeMapTexture.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/FFStateTracker.h"
#include "System/Config/ConfigHandler.h"
#include "System/Exceptions.h"
#include "System/SafeUtil.h"
#include "System/Log/ILog.h"

#include "System/Misc/TracyDefs.h"

CONFIG(bool, AdvSky).deprecated(true);

ISky::ISky()
	: skyColor(mapInfo->atmosphere.skyColor)
	, sunColor(mapInfo->atmosphere.sunColor)
	, cloudColor(mapInfo->atmosphere.cloudColor)
	, fogColor(mapInfo->atmosphere.fogColor)
	, skyAxisAngle(mapInfo->atmosphere.skyAxisAngle)
	, fogStart(mapInfo->atmosphere.fogStart)
	, fogEnd(mapInfo->atmosphere.fogEnd)
	, cloudDensity(mapInfo->atmosphere.cloudDensity)
	, skyLight(nullptr)
	, wireFrameMode(false)
	, updated(true)
{
	skyLight = new ISkyLight();
}

ISky::~ISky()
{
	RECOIL_DETAILED_TRACY_ZONE;
	spring::SafeDelete(skyLight);
}

std::unique_ptr<ISky> ISky::sky = nullptr;



void ISky::SetupFog() {
	RECOIL_DETAILED_TRACY_ZONE;

	if (globalRendering->drawFog) {
		glEnable(GL_FOG);
	} else {
		glDisable(GL_FOG);
	}

	// Every glFog* is on RenderDoc's unsupported list and the queries that read
	// the state back are not, so only write what actually moved -- this runs per
	// draw pass (5,601 times a run) while the values change at most once a frame,
	// and mostly never. Writing a state the value it already holds cannot change
	// rendering. Same trade as GL::AttribSnapshot::Restore.
	const float fogStartDist = camera->GetFarPlaneDist() * fogStart;
	const float fogEndDist   = camera->GetFarPlaneDist() * fogEnd;

	float curFogColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	GLint curFogMode = 0;
	float curFogStart = 0.0f, curFogEnd = 0.0f, curFogDensity = 0.0f;
	glGetFloatv(GL_FOG_COLOR, curFogColor);
	glGetIntegerv(GL_FOG_MODE, &curFogMode);
	glGetFloatv(GL_FOG_START, &curFogStart);
	glGetFloatv(GL_FOG_END, &curFogEnd);
	glGetFloatv(GL_FOG_DENSITY, &curFogDensity);

	if (!std::equal(curFogColor, curFogColor + 4, static_cast<const float*>(fogColor)))
		glFogfv(GL_FOG_COLOR, fogColor);
	// The candidate pass has to hold GL_EXP -- the value a deleted call would
	// leave behind -- not merely skip the write. GL_FOG_MODE is sticky, so with
	// the write skipped the pass would inherit the GL_LINEAR an earlier pass
	// already set and the experiment would report a vacuous zero. The guard
	// above restores GL_LINEAR on the next pass, so this needs no teardown.
	const GLint wantFogMode = GL::ffExperiment.Active(GL::FFExperiment::FogMode) ? GL_EXP : GL_LINEAR;
	if (curFogMode != wantFogMode)
		glFogi(GL_FOG_MODE, wantFogMode);
	if (curFogStart != fogStartDist)
		glFogf(GL_FOG_START, fogStartDist);
	if (curFogEnd != fogEndDist)
		glFogf(GL_FOG_END, fogEndDist);
	if (curFogDensity != 1.0f)
		glFogf(GL_FOG_DENSITY, 1.0f);
}

void ISky::SetSky()
{
	RECOIL_DETAILED_TRACY_ZONE;
	sky = nullptr; //break before make

	try {
		if (globalRendering->drawDebugCubeMap) {
			int2 dims = debugCubeMapTexture.GetDimensions();
			sky = std::make_unique<CSkyBox>(debugCubeMapTexture.GetId(), dims.x, dims.y);
		}
		else if (!mapInfo->atmosphere.skyBox.empty()) {
			sky = std::make_unique<CSkyBox>("maps/" + mapInfo->atmosphere.skyBox);
		}
		else {
			sky = std::make_unique<CModernSky>();
		}
	} catch (const content_error& ex) {
		LOG_L(L_ERROR, "[ISky::%s] error: %s (falling back to NullSky)", __func__, ex.what());
		sky = std::make_unique<CNullSky>();
	}

	if (!sky->IsValid()) {
		LOG_L(L_ERROR, "[ISky::%s] error creating %s (falling back to NullSky)", __func__, sky->GetName().c_str());
		sky = std::make_unique<CNullSky>();
	}
}

void ISky::SetSkyAxisAngle(const float4& skyAxisAngleRaw)
{
	auto axis = float3{ skyAxisAngleRaw.x, skyAxisAngleRaw.y, skyAxisAngleRaw.z };
	const float axisNorm = axis.Length();
	if (axisNorm < float3::nrm_eps())
		axis = FwdVector;
	else
		axis /= axisNorm;

	skyAxisAngle = float4{ axis, ClampRad(skyAxisAngleRaw.w) };
}

bool ISky::SunVisible(const float3 pos) const {
	RECOIL_DETAILED_TRACY_ZONE;
	const CUnit* hitUnit = nullptr;
	const CFeature* hitFeature = nullptr;

	// cast a ray *toward* the sun from <pos>
	// sun is visible if no terrain blocks it
	const float3& sunDir = skyLight->GetLightDir();
	const float sunDist = TraceRay::GuiTraceRay(pos, sunDir, camera->GetFarPlaneDist(), nullptr, hitUnit, hitFeature, false, true, false);

	return (sunDist < 0.0f || sunDist >= camera->GetFarPlaneDist());
}

