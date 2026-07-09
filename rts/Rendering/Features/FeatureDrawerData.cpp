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
#include "Rendering/Features/FeatureDrawer.h"
#include "System/SimDrawSplit.h"

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
const CFeature* DrawerResolveLiveObjectByID<CFeature>(int id)
{
	const CFeature* feature = featureHandler.GetFeature(id);

	if (feature == nullptr)
		feature = renderEventQueue.FindPendingDestroyFeature(id);

	assert(feature != nullptr);
	return feature;
}

template<>
const CFeature* DrawerGetObjectByID<CFeature>(int id)
{
	// SCOPE-1: see the CUnit resolver. Non-model features are registered in the
	// record by RegisterNonModelFeatureRecords (replacing RegisterExtraSplitResolveIDs).
	if (SimDrawSplit::Enabled()) {
		if (const CFeature* feature = CFeatureDrawer::GetRenderRecord(id).obj)
			return feature;
	}

	return DrawerResolveLiveObjectByID<CFeature>(id);
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

	// SCOPE-1: freeze the render record's immutable header; mutable fields are
	// filled by UpdateRenderRecord (here + each Update pass) before any draw
	auto& rr = RenderRecordRef(feature);
	rr = {};
	rr.obj   = feature;
	rr.model = feature->model;
	rr.def   = feature->def;
	rr.localModel = &feature->localModel;
	UpdateRenderRecord(feature);
}

// SCOPE-1: register the deferred-safe handle for non-model features (trees/geo-
// vents). RenderFeaturePreCreated early-returns for them (no model-drawer slot),
// yet the Lua draw-payload resolvers (LuaSnapshotServe::ResolveDrawFeature) must
// still resolve their ids to a live pointer. Producer-side sweep (Update, sim
// quiescent) — the exact sanctioned featureHandler read RegisterExtraSplitResolveIDs
// did, now writing the record's handle instead of the deleted splitResolveCache.
// Dead ids drop out at RenderFeatureDestroyed (record cleared); this only ADDS
// missing live non-model handles, so it is idempotent per frame.
void CFeatureDrawerData::RegisterNonModelFeatureRecords()
{
	for (const int id: featureHandler.GetActiveFeatureIDs()) {
		if (static_cast<size_t>(id) < renderRecords.size() && renderRecords[id].obj != nullptr)
			continue;

		const CFeature* feature = DrawerResolveLiveObjectByID<CFeature>(id);
		if (feature->def->drawType == DRAWTYPE_MODEL)
			continue; // model features register via RenderFeaturePreCreated

		auto& rr = RenderRecordRef(feature);
		rr = {};
		rr.obj = feature;
		rr.def = feature->def;
		// model/localModel stay null (never drawn); scalar fields unused for
		// non-model features (their Lua serving dereferences obj live)
	}
}

const CFeatureDrawerData::FeatureRenderRecord& CFeatureDrawerData::GetRenderRecord(const CFeature* f) const
{
	return GetRenderRecord(f->id);
}

CFeatureDrawerData::FeatureRenderRecord& CFeatureDrawerData::RenderRecordRef(const CFeature* f)
{
	if (f->id >= renderRecords.size())
		renderRecords.resize(f->id + 1);

	return renderRecords[f->id];
}

// SCOPE-1: refresh the sim-owned mutable fields the draw-window passes read.
// Runs producer-side (once per new sim frame, in Update), sim quiescent.
void CFeatureDrawerData::UpdateRenderRecord(const CFeature* feature)
{
	FeatureRenderRecord& rr = RenderRecordRef(feature);
	rr.team           = feature->team;
	rr.engineDrawMask = feature->engineDrawMask;
	rr.luaDraw        = feature->luaDraw;
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

	// PR 43 (3b, retires the IdToObject shell fallback): under the split the
	// dead record is RETAINED -- its obj stays the deferred-deletion shell,
	// readable until the step-8 ack -- so a died-in-batch id keeps resolving
	// through DrawerGetObjectByID for the whole deferred-dispatch window
	// (gl.SetFeatureBufferUniforms from deferred RecvFromSynced forwarders:
	// unit_healthbars_widget_forwarding, the exact class that blocked the
	// SCOPE-1 retirement). obj/def are (re)assigned unconditionally: a
	// non-model feature created-and-died in the SAME batch never registered a
	// record (RenderFeaturePreCreated early-returns for it, and the
	// RegisterNonModelFeatureRecords sweep only sees live ids), and this is
	// the only at-death registration point that covers it -- the record's
	// cold-miss (FindPendingDestroyFeature) cannot, because the destroy
	// record just popped its shell. ClearDeadRetainedRecords (barrier step 8
	// / valve service) clears it before the ack poisons the shell. Flag-off
	// keeps the SCOPE-1 immediate clear (byte-identical).
	if (SimDrawSplit::Enabled()) {
		FeatureRenderRecord& rr = RenderRecordRef(feature);
		rr.obj = feature; // ensure the retained record IS this (latest) dead generation
		rr.def = feature->def;
		deadRetainedRecords.emplace_back(feature->id, feature);
	} else if (feature->id < renderRecords.size()) {
		renderRecords[feature->id] = {};
	}
}

// PR 43 (3b): see RenderFeatureDestroyed -- clear the dead-retained records
// at the end of the dispatch window, before the ack poisons their shells.
// The obj==shell guard skips records a same-batch id reuse already overwrote.
void CFeatureDrawerData::ClearDeadRetainedRecords()
{
	for (const auto& [id, shell] : deadRetainedRecords) {
		if (static_cast<size_t>(id) < renderRecords.size() && renderRecords[id].obj == shell)
			renderRecords[id] = {};
	}

	deadRetainedRecords.clear();
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

	// defined extraction point: snapshot piece/object transforms once per new
	// sim frame. PR 44a: under the running flip the PRODUCER extracts at the
	// sim frame edge (ExtractTransformsAtSimEdge); this consume-side call
	// degrades to the targeted catch-up for objects added by this consume's
	// record dispatch (sim parked here, so the live reads stay legal).
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning())
		ExtractPendingNewObjectTransforms();
	else
		ExtractTransforms();

	// SCOPE-1: keep non-model-feature handles registered for the Lua resolvers
	// (replaces RegisterExtraSplitResolveIDs). Producer-side, sim quiescent.
	RegisterNonModelFeatureRecords();

	if (mtModelDrawer) {
		for_mt_chunk(0, unsortedObjects.size(), [this](const int k) {
			const CFeature* f = DrawerGetObjectByID<CFeature>(unsortedObjects[k]);
			UpdateDrawPos(f);
			UpdateCommon(f);
			UpdateUnsyncedTransform(f);
			UpdateRenderRecord(f); // SCOPE-1: refresh draw-window record (producer-side)
		}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
	}
	else {
		for (const int featureID : unsortedObjects) {
			const CFeature* f = DrawerGetObjectByID<CFeature>(featureID);
			UpdateDrawPos(f);
			UpdateCommon(f);
			UpdateUnsyncedTransform(f);
			UpdateRenderRecord(f); // SCOPE-1: refresh draw-window record (producer-side)
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
	dp.drawRadius = f->GetDrawRadius(); // sim|draw PR 40: extract for the frustum twins
}