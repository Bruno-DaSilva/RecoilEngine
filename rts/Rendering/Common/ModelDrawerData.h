#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>

#include <unordered_map>

#include "System/EventClient.h"
#include "System/EventHandler.h"
#include "System/ContainerUtil.h"
#include "System/Config/ConfigHandler.h"
#include "System/Threading/ThreadPool.h"
#include "System/TimeProfiler.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Models/ModelsMemStorage.h"
#include "Rendering/Models/ModelRenderContainer.h"
#include "Rendering/Models/3DModel.hpp"
#include "Rendering/Env/IWater.h"
#include "Map/ReadMap.h"
#include "Game/Camera.h"
#include "Game/GlobalUnsynced.h"
#include "Game/CameraHandler.h"

class CModelDrawerDataConcept : public CEventClient {
public:
	CModelDrawerDataConcept(const std::string& ecName, int ecOrder)
		: CEventClient(ecName, ecOrder, false)
	{};
	virtual ~CModelDrawerDataConcept() {
		eventHandler.RemoveClient(this);
		autoLinkedEvents.clear();
	};
public:
	bool GetFullRead() const override { return true; }
	int  GetReadAllyTeam() const override { return AllAccessTeam; }
protected:
	static constexpr int MT_CHUNK_OR_MIN_CHUNK_SIZE_SMMA = 128;
	static constexpr int MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT = 256;
};


template <typename T>
class CModelDrawerDataBase : public CModelDrawerDataConcept
{
public:
	using ObjType = T;
public:
	CModelDrawerDataBase(const std::string& ecName, int ecOrder, bool& mtModelDrawer_);
	virtual ~CModelDrawerDataBase() override;
public:
	virtual void Update() = 0;
protected:
	virtual bool IsAlpha(const T* co) const = 0;
private:
	void AddObject(const T* co, bool add); //never to be called directly! Use UpdateObject() instead!
protected:
	void DelObject(const T* co, bool del);
	void UpdateObject(const T* co, bool init);
protected:
	void ExtractTransforms();
	void UpdateCommon(T* o);
	virtual void UpdateObjectDrawFlags(CSolidObject* o) const = 0;
private:
	void ExtractObjectTransforms(const T* o);
	void UpdateObjectUniforms(const T* o);
public:
	const std::vector<T*>& GetUnsortedObjects() const { return unsortedObjects; }
	const ModelRenderContainer<T>& GetModelRenderer(int modelType) const { return modelRenderers[modelType]; }

	void ClearPreviousDrawFlags() { for (auto object : unsortedObjects) object->previousDrawFlag = 0; }

	const auto& GetObjectTransformMemAlloc(const T* o) const {
		const auto it = scTransMemAllocMap.find(const_cast<T*>(o));
		return (it != scTransMemAllocMap.end()) ? it->second : ScopedTransformMemAlloc::Dummy();
	}
	auto& GetObjectTransformMemAlloc(const T* o) { return scTransMemAllocMap[const_cast<T*>(o)]; }
private:
	static constexpr int MMA_SIZE0 = 2 << 17;
protected:
	std::array<ModelRenderContainer<T>, MODELTYPE_CNT> modelRenderers;

	std::vector<T*> unsortedObjects;
	spring::unordered_map<const T*, ScopedTransformMemAlloc> scTransMemAllocMap;

	// last sim frame ExtractTransforms() ran for; extraction is due once per new sim frame
	int32_t transformsExtractedFrame = std::numeric_limits<int32_t>::lowest();
	// set on AddObject: new allocations need one extraction outside the sim-frame
	// cadence (e.g. objects spawned before the first sim frame advances)
	bool transformsExtractionPending = true;

	bool& mtModelDrawer;
};

using CUnitDrawerDataBase = CModelDrawerDataBase<CUnit>;
using CFeatureDrawerDataBase = CModelDrawerDataBase<CFeature>;

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

template<typename T>
inline CModelDrawerDataBase<T>::CModelDrawerDataBase(const std::string& ecName, int ecOrder, bool& mtModelDrawer_)
	: CModelDrawerDataConcept(ecName, ecOrder)
	, mtModelDrawer(mtModelDrawer_)
{
	scTransMemAllocMap.reserve(MMA_SIZE0);
	for (auto& mr : modelRenderers) { mr.Clear(); }
}

template<typename T>
inline CModelDrawerDataBase<T>::~CModelDrawerDataBase()
{
	unsortedObjects.clear();
	scTransMemAllocMap.clear();
}

template<typename T>
inline void CModelDrawerDataBase<T>::AddObject(const T* co, bool add)
{
	T* o = const_cast<T*>(co);

	if (o->model != nullptr) {
		modelRenderers[MDL_TYPE(o)].AddObject(o);
	}

	if (!add)
		return;

	unsortedObjects.emplace_back(o);

	const uint32_t numMatrices = ((o->model ? o->model->numPieces : 0) + 1u) * 2;
	scTransMemAllocMap.emplace(o, ScopedTransformMemAlloc(numMatrices));
	transformsExtractionPending = true; //make sure the new allocation is filled at least once before the next sim frame

	modelUniformsStorage.AddObject(co);
}

template<typename T>
inline void CModelDrawerDataBase<T>::DelObject(const T* co, bool del)
{
	T* o = const_cast<T*>(co);

	if (o->model != nullptr) {
		modelRenderers[MDL_TYPE(o)].DelObject(o);
	}

	if (del && spring::VectorErase(unsortedObjects, o)) {
		scTransMemAllocMap.erase(o);
		modelUniformsStorage.DelObject(co);
	}
}

template<typename T>
inline void CModelDrawerDataBase<T>::UpdateObject(const T* co, bool init)
{
	DelObject(co, false);
	AddObject(co, init );
}


/* Transform extraction point (sim/draw decoupling §A; layout/timing feed PR 15's SimSnapshot).
 *
 * When it runs: first drawer-data Update() after >= 1 new sim frame(s) — i.e. once per
 * rendered sim frame, after all SimFrames of the current iteration completed and before
 * any render pass or SSBO upload (transformsUploader.Update() follows worldDrawer.Update()
 * in CGame::UpdateUnsynced) — plus once when objects were added outside the frame cadence.
 * Coverage is eager per the local client's information surface: every registered object
 * in LOS for the local allyteam (all of them for full-view spectators/replays), not just
 * drawFlag-visible ones, so that no consumer after this point (render passes, picking,
 * Lua callouts via GetModelSpaceMatrix) ever recomputes a dirty piece, i.e. mutates
 * sim-side state, mid-draw. Out-of-LOS objects are skipped whole: the storage keeps their
 * last-seen pose and their flags keep accumulating, exactly as under master's drawFlag
 * gate — uploading live transforms of hidden objects would widen the information surface
 * (the storage backs a Lua-shader-readable SSBO), cf. the same LOS gate in
 * UpdateObjectUniforms. LOS only changes inside sim frames, so per-sim-frame extraction
 * leaves no staleness window for anything the client may legitimately draw or pick.
 *
 * What marks a piece dirty / wasUpdated: synced sim code only — UnitScript anim ticks and
 * Move/Turn[Now] (LocalModelPiece::SetPosition/SetRotation -> SetDirty), the TickAllAnims
 * recompute pass (clears dirty, sets wasUpdated), Spring.SetUnitPieceMatrix (SetDirty) and
 * SetScriptVisible. Nothing unsynced mutates piece state, so values cannot change between
 * this extraction and the draw that consumes it: the legacy path (GetModelSpaceMatrix) and
 * the extracted copies in transformsMemStorage render the same, frame-consistent pose.
 *
 * Who clears the flags: dirty is cleared by whoever recomputes the cached transform (sim's
 * TickAllAnims BFS, or GetModelSpaceTransform() here); wasUpdated and the noInterpolation
 * flags are consumed and reset ONLY here (ResetWasUpdated; two-frame handoff so the prev
 * transform gets uploaded one extra frame after animation stops).
 *
 * Per-object layout in transformsMemStorage (allocated in AddObject, (numPieces + 1) * 2):
 *   [0] prev object transform (o->preFrameTra, saved by sim at the start of the frame —
 *       the interpolation base), [1] curr synced object transform;
 *   [2 + 2i] piece i prev model-space transform, [3 + 2i] piece i curr; script-invisible
 *   pieces store Transform::Zero() in both slots.
 */
template<typename T>
inline void CModelDrawerDataBase<T>::ExtractTransforms()
{
	if (!transformsExtractionPending && transformsExtractedFrame >= gs->frameNum)
		return;

	transformsExtractionPending = false;
	transformsExtractedFrame = gs->frameNum;

	SCOPED_TIMER("Update::ExtractTransforms");

	if (mtModelDrawer) {
		for_mt_chunk(0, unsortedObjects.size(), [this](const int k) {
			ExtractObjectTransforms(unsortedObjects[k]);
		}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
	} else {
		for (const T* o : unsortedObjects)
			ExtractObjectTransforms(o);
	}
}

template<typename T>
inline void CModelDrawerDataBase<T>::ExtractObjectTransforms(const T* o)
{
	// keep master's information surface (see comment above); alwaysUpdateMat keeps
	// its master meaning of forcing transform updates for non-visible objects
	if (!gu->spectatingFullView && !o->alwaysUpdateMat && !o->IsInLosForAllyTeam(gu->myAllyTeam))
		return;

	ScopedTransformMemAlloc& stma = GetObjectTransformMemAlloc(o);

	const auto& tmPrev = o->preFrameTra;
	const auto  tmCurr = Transform::FromMatrix(o->GetTransformMatrix(true)); //synced transform

	// conditionally update new and prev synced positions
	stma.UpdateIfChanged(0, tmPrev);
	stma.UpdateIfChanged(1, tmCurr);

	for (int i = 0; i < o->localModel.pieces.size(); ++i) {
		const LocalModelPiece& lmp = o->localModel.pieces[i];

		const auto& lmpTransform = lmp.GetModelSpaceTransform(); //forces dirty / wasUpdated recalculation if no other method called it yet

		if likely(!lmp.GetWasUpdated())
			continue;

		if unlikely(!lmp.GetScriptVisible()) {
			stma.UpdateForced(2 * (1 + i) + 0, Transform::Zero());
			stma.UpdateForced(2 * (1 + i) + 1, Transform::Zero());
			lmp.ResetWasUpdated();
			continue;
		}

		stma.UpdateForced(2 * (1 + i) + 0, lmp.GetEffectivePrevModelSpaceTransform());
		stma.UpdateForced(2 * (1 + i) + 1, lmpTransform);

		lmp.ResetWasUpdated();
	}
}

template<typename T>
inline void CModelDrawerDataBase<T>::UpdateObjectUniforms(const T* o)
{
	auto& uni = modelUniformsStorage.GetObjUniformsArray(o);
	uni.drawFlag = o->drawFlag;

	if (gu->spectatingFullView || o->IsInLosForAllyTeam(gu->myAllyTeam)) {
		uni.id = o->id;
		// TODO remove drawPos, replace with pos
		uni.drawPos = float4{ o->drawPos, o->heading * math::PI / SPRING_MAX_HEADING };
		uni.speed = o->speed;
		uni.maxHealth = o->maxHealth;
		uni.health = o->health;
	}
}

template<typename T>
inline void CModelDrawerDataBase<T>::UpdateCommon(T* o)
{
	assert(o);
	o->previousDrawFlag = o->drawFlag;
	UpdateObjectDrawFlags(o);

	// transforms are no longer updated here: ExtractTransforms() covers every
	// LOS-visible object once per new sim frame, camera-independent (see there)

	UpdateObjectUniforms(o);
}
