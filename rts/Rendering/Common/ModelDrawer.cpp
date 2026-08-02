#include "ModelDrawer.h"
#include "Rendering/GL/FFShaderRewrite.h"

#include "Map/Ground.h"
#include "Rendering/GL/LightHandler.h"
#include "System/Config/ConfigHandler.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/LuaObjectDrawer.h"

#include "System/Misc/TracyDefs.h"

void CModelDrawerConcept::InitStatic()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (initialized)
		return;

	cubeMapHandler.Init();
	wireFrameMode = false;

	lightHandler.Init(2U, configHandler->GetInt("MaxDynamicModelLights"));

	deferredAllowed = configHandler->GetBool("AllowDeferredModelRendering");

	// shared with FeatureDrawer!
	geomBuffer = LuaObjectDrawer::GetGeometryBuffer();
	deferredAllowed &= geomBuffer->Valid();

	// The legacy state's constructor compiles pre-core GLSL that RenderDoc cannot
	// reflect, at load, whether or not anything draws with it. Under the migration
	// knob and only when the GL4 path is available to take instead, construct it on
	// demand: the fallback still exists, but a run that never falls back never
	// creates those shaders. Knob-gated, so an unupdated game builds what it builds
	// today.
	const bool deferLegacy = GL::FFRewriteEnabled() && globalRendering->haveGL4;
	IModelDrawerState::InitInstance<CModelDrawerStateGLSL>(MODEL_DRAWER_GLSL, deferLegacy);
	IModelDrawerState::InitInstance<CModelDrawerStateGL4 >(MODEL_DRAWER_GL4 );

	initialized = true;
}

void CModelDrawerConcept::KillStatic(bool reload)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!initialized)
		return;

	cubeMapHandler.Free();
	geomBuffer = nullptr;

	for (int t = ModelDrawerTypes::MODEL_DRAWER_GLSL; t < ModelDrawerTypes::MODEL_DRAWER_CNT; ++t) {
		IModelDrawerState::KillInstance(t);
	}

	initialized = false;
}