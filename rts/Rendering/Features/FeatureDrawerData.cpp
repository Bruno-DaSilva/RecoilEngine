#include "FeatureDrawerData.h"

#include "System/Config/ConfigHandler.h"
#include "System/StringHash.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Common/ModelDrawerHelpers.h"
#include "Rendering/Common/RenderEventQueue.h"

#include "System/Misc/TracyDefs.h"

CONFIG(float, FeatureDrawDistance)
.defaultValue(6000.0f)
.minimumValue(0.0f)
.description("Maximum distance at which features will be drawn.");

CONFIG(float, FeatureFadeDistance)
.defaultValue(4500.0f)
.minimumValue(0.0f)
.description("Distance at which features will begin to fade from view.");

// id resolution for the drawer-side containers (see ModelDrawerData.h).
// Pending-destroy fallback: same mid-sim-phase container-read window as the
// CUnit resolver (see UnitDrawerData.cpp / RenderEventQueue.h)
template<>
const CFeature* DrawerGetObjectByID<CFeature>(int id)
{
	const CFeature* feature = featureHandler.GetFeature(id);

	if (feature == nullptr)
		feature = renderEventQueue.FindPendingDestroyFeature(id);

	assert(feature != nullptr);
	return feature;
}


void CFeatureDrawerData::RenderFeaturePreCreated(const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (feature->def->drawType != DRAWTYPE_MODEL)
		return;

	UpdateObject(feature, true);

	if (feature->id >= unsyncedTransforms.size())
		unsyncedTransforms.resize(feature->id + 1);

	unsyncedTransforms[feature->id] = CMatrix44f{}; // identity until first drawn, as the old member was

	if (feature->id >= drawAlphas.size())
		drawAlphas.resize(feature->id + 1, 1.0f);

	drawAlphas[feature->id] = 1.0f; // as the old member init was
}

//TODO remove
void CFeatureDrawerData::RenderFeatureCreated(const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(
		feature->def->drawType != DRAWTYPE_MODEL ||
		std::find(unsortedObjects.begin(), unsortedObjects.end(), feature->id) != unsortedObjects.end()
	);
}

void CFeatureDrawerData::RenderFeatureDestroyed(const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	DelObject(feature, feature->def->drawType == DRAWTYPE_MODEL);
	LuaObjectDrawer::SetObjectLOD(feature, LUAOBJ_FEATURE, 0);

	// unlike the other id-keyed slots this one is read for arbitrary features
	// (decal fading), so reset it: a non-model feature reusing the id would
	// otherwise read the previous owner's fade instead of the fresh-member 1.0f
	if (feature->id < drawAlphas.size())
		drawAlphas[feature->id] = 1.0f;
}

CFeatureDrawerData::CFeatureDrawerData(bool& mtModelDrawer_)
	: CFeatureDrawerDataBase("[CFeatureDrawerData]", 313373, mtModelDrawer_)
{
	RECOIL_DETAILED_TRACY_ZONE;
	eventHandler.AddClient(this); //cannot be done in CModelRenderDataConcept, because object is not fully constructed
	configHandler->NotifyOnChange(this, { "FeatureDrawDistance", "FeatureFadeDistance" });

	featureDrawDistance = configHandler->GetFloat("FeatureDrawDistance");
	featureFadeDistance = std::min(configHandler->GetFloat("FeatureFadeDistance"), featureDrawDistance);
}

CFeatureDrawerData::~CFeatureDrawerData()
{
	RECOIL_DETAILED_TRACY_ZONE;
	configHandler->RemoveObserver(this);
}

void CFeatureDrawerData::ConfigNotify(const std::string& key, const std::string& value)
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (hashStringLower(key.c_str())) {
	case hashStringLower("FeatureDrawDistance"): {
		featureDrawDistance = std::strtof(value.c_str(), nullptr);
	} break;
	case hashStringLower("FeatureFadeDistance"): {
		featureFadeDistance = std::strtof(value.c_str(), nullptr);
	} break;
	default: {} break;
	}

	featureDrawDistance = std::max(0.0f, featureDrawDistance);
	featureFadeDistance = std::max(0.0f, featureFadeDistance);
	featureFadeDistance = std::min(featureFadeDistance, featureDrawDistance);

	LOG_L(L_INFO, "[FeatureDrawer::%s] {draw,fade}distance set to {%f,%f}", __func__, featureDrawDistance, featureFadeDistance);
}

void CFeatureDrawerData::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;

	// defined extraction point: snapshot piece/object transforms once per new sim frame
	ExtractTransforms();

	if (mtModelDrawer) {
		for_mt_chunk(0, unsortedObjects.size(), [this](const int k) {
			const CFeature* f = DrawerGetObjectByID<CFeature>(unsortedObjects[k]);
			UpdateDrawPos(f);
			UpdateCommon(f);
			UpdateUnsyncedTransform(f);
		}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
	}
	else {
		for (const int featureID : unsortedObjects) {
			const CFeature* f = DrawerGetObjectByID<CFeature>(featureID);
			UpdateDrawPos(f);
			UpdateCommon(f);
			UpdateUnsyncedTransform(f);
		}
	}
}

bool CFeatureDrawerData::IsAlpha(const CFeature* co) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	return (GetDrawAlpha(co) < 1.0f);
}

void CFeatureDrawerData::UpdateObjectDrawFlags(const CSolidObject* o)
{
	RECOIL_DETAILED_TRACY_ZONE;

	const CFeature* f = static_cast<const CFeature*>(o);
	ResetDrawFlag(f);

	float& drawAlpha = drawAlphas[f->id]; // slot exists for every registered object

	for (uint32_t camType = CCamera::CAMTYPE_PLAYER; camType < CCamera::CAMTYPE_ENVMAP; ++camType) {
		if (camType == CCamera::CAMTYPE_UWREFL && !IWater::GetWater()->CanDrawReflectionPass())
			continue;

		if (camType == CCamera::CAMTYPE_SHADOW && ((shadowHandler.shadowGenBits & CShadowHandler::SHADOWGEN_BIT_MODEL) == 0))
			continue;

		const CCamera* cam = CCameraHandler::GetCamera(camType);

		if (f->noDraw)
			continue;

		if (f->IsInVoid())
			continue;

		if (!f->IsInLosForAllyTeam(gu->myAllyTeam) && !gu->spectatingFullView)
			continue;

		if (!cam->InView(GetDrawMidPos(f), f->GetDrawRadius()))
			continue;

		switch (camType)
			{
			case CCamera::CAMTYPE_PLAYER: {
				const float camDist = (GetDrawPos(f) - cam->GetPos()).Length();

				// special case for non-fading features
				if (!f->alphaFade) {
					SetDrawFlag(f, DrawFlags::SO_OPAQUE_FLAG);
					drawAlpha = 1.0f;
					continue;
				}

				// too far, don't draw at all
				if (camDist > featureDrawDistance) {
					drawAlpha = 0.0f;
					continue;
				}

				// close enough to draw solid
				if (camDist < featureFadeDistance) {
					drawAlpha = 1.0f;
					SetDrawFlag(f, DrawFlags::SO_OPAQUE_FLAG);
					if (f->IsInWater())
						AddDrawFlag(f, DrawFlags::SO_REFRAC_FLAG);

					continue;
				}

				// fading is disabled, just don't draw
				if (featureDrawDistance == featureFadeDistance) {
					drawAlpha = 0.0f;
					continue;
				}

				drawAlpha = std::max(0.0f, 1.0f - (camDist - featureFadeDistance) / (featureDrawDistance - featureFadeDistance));
				SetDrawFlag(f, DrawFlags::SO_ALPHAF_FLAG);
				if (f->IsInWater())
					AddDrawFlag(f, DrawFlags::SO_REFRAC_FLAG);
			} break;

			case CCamera::CAMTYPE_UWREFL: {
				if (drawAlpha <= 0.0f)
					continue;

				if (!HasDrawFlag(f, DrawFlags::SO_OPAQUE_FLAG) && !HasDrawFlag(f, DrawFlags::SO_ALPHAF_FLAG))
					continue;

				if (CModelDrawerHelper::ObjectVisibleReflection(GetDrawMidPos(f), cam->GetPos(), f->GetDrawRadius()))
					AddDrawFlag(f, DrawFlags::SO_REFLEC_FLAG);
			} break;

			case CCamera::CAMTYPE_SHADOW: {
				if (drawAlpha <= 0.0f)
					continue;

				if unlikely(IsAlpha(f))
					AddDrawFlag(f, DrawFlags::SO_SHTRAN_FLAG);
				else
					AddDrawFlag(f, DrawFlags::SO_SHOPAQ_FLAG);
			} break;

			default: { assert(false); } break;
		}
	}

}

const CMatrix44f& CFeatureDrawerData::GetUnsyncedTransformMatrix(const CFeature* f) const
{
	return GetUnsyncedTransformMatrix(f->id);
}

const CMatrix44f& CFeatureDrawerData::GetUnsyncedTransformMatrix(int id) const
{
	static const CMatrix44f identity;
	return (static_cast<size_t>(id) < unsyncedTransforms.size()) ? unsyncedTransforms[id] : identity;
}

float CFeatureDrawerData::GetDrawAlpha(const CFeature* f) const
{
	return (f->id < drawAlphas.size()) ? drawAlphas[f->id] : 1.0f;
}

void CFeatureDrawerData::UpdateUnsyncedTransform(const CFeature* f)
{
	const uint8_t drawFlag = GetDrawFlag(f);
	if (f->alwaysUpdateMat || (drawFlag > DrawFlags::SO_NODRAW_FLAG && drawFlag < DrawFlags::SO_DRICON_FLAG)) {
		unsyncedTransforms[f->id] = f->ComposeMatrix(GetDrawPos(f));
	}
}

void CFeatureDrawerData::UpdateDrawPos(const CFeature* f)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& dp = drawPositions[f->id];
	dp.pos    = f->GetDrawPos(globalRendering->timeOffset);
	dp.midPos = GetMdlDrawMidPos(f);
}