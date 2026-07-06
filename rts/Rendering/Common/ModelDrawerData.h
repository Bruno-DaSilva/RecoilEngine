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
#include "Sim/Objects/WorldObject.h" // DrawFlags
#include "Rendering/Env/IWater.h"
#include "Map/ReadMap.h"
#include "Game/Camera.h"
#include "Game/GlobalUnsynced.h"
#include "Game/CameraHandler.h"

// id -> live object resolution for the drawer-side containers (PR 14: the
// containers hold IDs; ids are looked up through the object's handler at
// use time). Post-drain invariant: every id in a drawer container resolves
// to a live object -- destroy records erase the id at the boundary drain
// before any draw-side consumer iterates. Explicit specializations for
// CUnit (UnitDrawerData.cpp) and CFeature (FeatureDrawerData.cpp).
template<typename T>
const T* DrawerGetObjectByID(int id);

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
	void UpdateCommon(const T* o);
	// authors the drawer-owned draw-flag storage (sim/draw §A, PR 4); the
	// object itself is read-only (draw code cannot mutate sim state, PR 10)
	virtual void UpdateObjectDrawFlags(const CSolidObject* o) = 0;
private:
	void ExtractObjectTransforms(const T* o);
	void UpdateObjectUniforms(const T* o);
public:
	// object ids; resolve via DrawerGetObjectByID<T> (see above)
	const std::vector<int>& GetUnsortedObjects() const { return unsortedObjects; }
	const ModelRenderContainer<T>& GetModelRenderer(int modelType) const { return modelRenderers[modelType]; }

	// render-owned draw-visibility flags (sim/draw §A: these were drawFlag/previousDrawFlag
	// fields on the sim objects, authored and consumed only by draw code — evicted to
	// drawer storage). Keyed by object id; SO_NODRAW_FLAG on AddObject (matching the old
	// member init), stale after death until id reuse, and unregistered objects read
	// SO_NODRAW_FLAG — each matching the old member semantics exactly.
	uint8_t GetDrawFlag(const T* o) const { return GetDrawFlagState(o).flag; }
	uint8_t GetPreviousDrawFlag(const T* o) const { return GetDrawFlagState(o).prev; }
	bool HasDrawFlag(const T* o, DrawFlags f) const { return (GetDrawFlag(o) & f) == f; }

	// id-keyed variants for snapshot-serving consumers (PR 18 Lua callout twins)
	uint8_t GetDrawFlag(int id) const { return GetDrawFlagState(id).flag; }
	bool HasDrawFlag(int id, DrawFlags f) const { return (GetDrawFlag(id) & f) == f; }

	// mutators; only valid for registered objects (id slot exists), as UpdateObjectDrawFlags
	// and the icon-state pass are the sole writers — matching the old member's always-present
	void ResetDrawFlag(const T* o) { DrawFlagRef(o).flag = DrawFlags::SO_NODRAW_FLAG; }
	void SetDrawFlag(const T* o, DrawFlags f) { DrawFlagRef(o).flag  =  f; }
	void AddDrawFlag(const T* o, DrawFlags f) { DrawFlagRef(o).flag |=  f; }
	void DelDrawFlag(const T* o, DrawFlags f) { DrawFlagRef(o).flag &= ~f; }

	void ClearPreviousDrawFlags() { for (const int id : unsortedObjects) drawFlags[id].prev = 0; }

	const auto& GetObjectTransformMemAlloc(const T* o) const {
		const auto it = scTransMemAllocMap.find(o->id);
		return (it != scTransMemAllocMap.end()) ? it->second : ScopedTransformMemAlloc::Dummy();
	}
	auto& GetObjectTransformMemAlloc(const T* o) { return scTransMemAllocMap[o->id]; }

	// lazy registration for mid-sim-phase queries (PR 12/14): Lua handlers
	// driven by synced events (FeatureCreated, selection changes) can query a
	// just-created object's transform offset before the boundary drain runs
	// AddObject -- pre-PR-12 registration was synchronous with creation, so
	// those queries always hit. Allocates the same block AddObject would (its
	// emplace then no-ops on the existing entry); the uniforms storage has
	// auto-added on first query since before the split work, this is the
	// transforms-side symmetric behavior.
	const ScopedTransformMemAlloc& GetOrCreateTransformMemAlloc(const T* o) {
		const auto it = scTransMemAllocMap.find(o->id);

		if (it != scTransMemAllocMap.end())
			return it->second;

		const uint32_t numMatrices = ((o->model ? o->model->numPieces : 0) + 1u) * 2;
		transformsExtractionPending = true; // fill the new block before the next sim frame
		return scTransMemAllocMap.emplace(o->id, ScopedTransformMemAlloc(numMatrices)).first->second;
	}

	// render-owned interpolated draw positions (sim/draw decoupling §A: these were
	// fields on the sim objects, authored at draw rate — evicted to drawer storage).
	// Keyed by object id (ids are dense and bounded); slots are zeroed on AddObject,
	// matching the old member initialization, and written once per draw frame by the
	// derived UpdateDrawPos(). Values for deleted objects go stale until id reuse,
	// exactly like the old members went stale on the freed object. Objects never
	// registered with the drawer (non-model features) read zero, as their members
	// permanently did.
	const float3& GetDrawPos(const T* o) const { return GetDrawPosition(o).pos; }
	const float3& GetDrawMidPos(const T* o) const { return GetDrawPosition(o).midPos; }

	// id-keyed variant for snapshot-serving consumers (PR 18 Lua callout twins)
	// that hold no object pointer; same unregistered/stale-id semantics as above
	const float3& GetDrawPos(int id) const { return GetDrawPosition(id).pos; }

	// these transform a point or vector to object-space, based at the draw position
	float3 GetObjectSpaceDrawPos(const T* o, const float3& p) const { return (GetDrawPos(o) + o->GetObjectSpaceVec(p)); }

	// unsynced mid-positions (drawPos-based counterparts of relMidPos/localModel mid)
	float3 GetMdlDrawMidPos(const T* o) const { return (GetObjectSpaceDrawPos(o, WORLD_TO_OBJECT_SPACE * o->localModel.GetRelMidPos())); }
	float3 GetObjDrawMidPos(const T* o) const { return (GetObjectSpaceDrawPos(o, WORLD_TO_OBJECT_SPACE * o->relMidPos)); }
private:
	static constexpr int MMA_SIZE0 = 2 << 17;
protected:
	struct DrawPosition {
		float3 pos;
		float3 midPos;
	};

	const DrawPosition& GetDrawPosition(const T* o) const { return GetDrawPosition(o->id); }
	const DrawPosition& GetDrawPosition(int id) const {
		static const DrawPosition zero = {};
		return (static_cast<size_t>(id) < drawPositions.size()) ? drawPositions[id] : zero;
	}

	struct DrawFlagState {
		uint8_t flag = DrawFlags::SO_NODRAW_FLAG;
		uint8_t prev = DrawFlags::SO_NODRAW_FLAG;
	};

	const DrawFlagState& GetDrawFlagState(const T* o) const { return GetDrawFlagState(o->id); }
	const DrawFlagState& GetDrawFlagState(int id) const {
		static const DrawFlagState zero = {};
		return (static_cast<size_t>(id) < drawFlags.size()) ? drawFlags[id] : zero;
	}
	DrawFlagState& DrawFlagRef(const T* o) { return drawFlags[o->id]; }

	std::array<ModelRenderContainer<T>, MODELTYPE_CNT> modelRenderers;

	std::vector<int> unsortedObjects; // object ids (see GetUnsortedObjects)
	std::vector<DrawPosition> drawPositions; // indexed by object id
	std::vector<DrawFlagState> drawFlags;    // indexed by object id
	spring::unordered_map<int, ScopedTransformMemAlloc> scTransMemAllocMap; // keyed by object id

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
inline void CModelDrawerDataBase<T>::AddObject(const T* o, bool add)
{
	if (o->model != nullptr) {
		modelRenderers[MDL_TYPE(o)].AddObject(o);
	}

	if (!add)
		return;

	unsortedObjects.emplace_back(o->id);

	if (o->id >= drawPositions.size())
		drawPositions.resize(o->id + 1);

	drawPositions[o->id] = {}; // zero until the first UpdateDrawPos, as the old members were

	if (o->id >= drawFlags.size())
		drawFlags.resize(o->id + 1);

	drawFlags[o->id] = {}; // SO_NODRAW_FLAG until the first UpdateObjectDrawFlags, as the old members were

	const uint32_t numMatrices = ((o->model ? o->model->numPieces : 0) + 1u) * 2;
	scTransMemAllocMap.emplace(o->id, ScopedTransformMemAlloc(numMatrices));
	transformsExtractionPending = true; //make sure the new allocation is filled at least once before the next sim frame

	modelUniformsStorage.AddObject(o);
}

template<typename T>
inline void CModelDrawerDataBase<T>::DelObject(const T* o, bool del)
{
	if (o->model != nullptr) {
		modelRenderers[MDL_TYPE(o)].DelObject(o);
	}

	if (del && spring::VectorErase(unsortedObjects, o->id)) {
		scTransMemAllocMap.erase(o->id);
		modelUniformsStorage.DelObject(o);
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
			ExtractObjectTransforms(DrawerGetObjectByID<T>(unsortedObjects[k]));
		}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
	} else {
		for (const int id : unsortedObjects)
			ExtractObjectTransforms(DrawerGetObjectByID<T>(id));
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
	const auto  tmCurr = Transform::FromMatrix(o->GetTransformMatrix()); //synced transform

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
	uni.drawFlag = GetDrawFlag(o);

	if (gu->spectatingFullView || o->IsInLosForAllyTeam(gu->myAllyTeam)) {
		uni.id = o->id;
		// TODO remove drawPos, replace with pos
		uni.drawPos = float4{ GetDrawPos(o), o->heading * math::PI / SPRING_MAX_HEADING };
		uni.speed = o->speed;
		uni.maxHealth = o->maxHealth;
		uni.health = o->health;
	}
}

template<typename T>
inline void CModelDrawerDataBase<T>::UpdateCommon(const T* o)
{
	assert(o);
	DrawFlagRef(o).prev = GetDrawFlag(o);
	UpdateObjectDrawFlags(o);

	// transforms are no longer updated here: ExtractTransforms() covers every
	// LOS-visible object once per new sim frame, camera-independent (see there)

	UpdateObjectUniforms(o);
}
