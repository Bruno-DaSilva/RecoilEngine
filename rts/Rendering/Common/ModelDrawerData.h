#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>

#include <unordered_map>

#include "System/EventClient.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
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
#include "System/SimDrawSplit.h" // PR 44a: flip predicate (AddObject catch-up queue)
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

// the raw handler(+pending-destroy-shell) lookup; only valid while the sim is
// quiescent (single-threaded, parked, or in the sim phase itself) -- the
// split-aware DrawerGetObjectByID wraps it (PR 27b)
template<typename T>
const T* DrawerResolveLiveObjectByID(int id);

// sim|draw WS-2 (doc/sim-draw-split-optimization/transforms-dirty-skip.md §4):
// the object half of the whole-object transform-skip key is a bitwise value-
// shadow of exactly the values ExtractObjectTransforms consumes, NOT a
// mutation-choke version -- the dir vectors are public members mutated by
// reference-aliasing arithmetic all over the movetypes, so no finite choke
// set exists for them. Both structs are padding-free (memcmp-comparable);
// the feature matrix is stored as raw floats because CMatrix44f's alignas(64)
// would introduce 52 B of indeterminate tail padding.
struct UnitTransformSkipKey {
	float3 pos;
	float3 frontdir;
	float3 rightdir;
	float3 updir;
};
static_assert(sizeof(UnitTransformSkipKey) == 4 * sizeof(float3));

struct FeatureTransformSkipKey {
	float transMat[16]; // CFeature::transMatrix (what GetTransformMatrix() serves)
	float3 pos;         // keyed separately: preFrameTra.t is taken from pos, not the matrix
};
static_assert(sizeof(FeatureTransformSkipKey) == 16 * sizeof(float) + sizeof(float3));

// the preFrameTra shadow is memcmp-compared; Transform must stay padding-free
static_assert(sizeof(Transform) == sizeof(CQuaternion) + sizeof(float3) + sizeof(float));

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
public:
	// PR 44a: the flip producer's entry (sim thread, frame edge, forced-serial)
	void ExtractTransformsAtSimEdge() { ExtractTransforms(false); }
protected:
	// PR 44a: allowMT=false forces the serial walk -- the flip producer runs
	// this on the SIM thread at its frame edge and must not fork-join the
	// shared thread pool concurrently with main-thread draw work
	void ExtractTransforms(bool allowMT = true);
	// PR 44a consumer catch-up: extract ONLY the objects added by this
	// consume's record dispatch (the producer covers objects registered as of
	// the previous consume; without this a new object's first rendered frame
	// would have a zero/stale pose). Runs under the park. No-op when empty.
	// resolveRecordObj resolves an id via the drawer's RENDER RECORD ONLY --
	// live pointer | readable retained shell | nullptr, NEVER a released
	// slot: DrawerGetObjectByID's live-resolve fallback can return null (its
	// assert is NDEBUG-dead) or outlive an ack for ids whose lifetime ended
	// between the push and this call (the pr44a_strict_atg f=11826 SIGSEGV).
	template<typename RecordObjFn>
	void ExtractPendingNewObjectTransforms(RecordObjFn&& resolveRecordObj);
	void UpdateCommon(const T* o);
	// authors the drawer-owned draw-flag storage (sim/draw §A, PR 4); the
	// object itself is read-only (draw code cannot mutate sim state, PR 10)
	virtual void UpdateObjectDrawFlags(const CSolidObject* o) = 0;

private:
	void ExtractObjectTransforms(const T* o);
	void RunTransformSkipOracle(const T* o);
	void UpdateObjectUniforms(const T* o);
public:
	// object ids; resolve via DrawerGetObjectByID<T> (see above)
	const std::vector<int>& GetUnsortedObjects() const { return unsortedObjects; }

	// SCOPE-1 (plan PR 39): the PR-27b splitResolveCache + BuildSplitResolveCache
	// are DELETED. Draw-window id->object resolution now goes through the drawer-
	// owned render record's deferred-safe handle (UnitRenderRecord::obj /
	// FeatureRenderRecord::obj), captured producer-side at creation, so no draw
	// pass walks the sim-owned handler tables and no per-barrier cache rebuild is
	// needed. See DrawerGetObjectByID<T> (UnitDrawerData.cpp / FeatureDrawerData.cpp).

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
	// id-keyed stored draw-midpos / draw-radius (sim|draw PR 40 frustum twins:
	// GetVisibleUnits/Features read the SAME stored DrawPosition the pointer
	// forms return -- GetDrawMidPos(o) is drawPositions[o->id].midPos, and
	// GetDrawRadius is the localModel bounding radius captured in UpdateDrawPos
	// at extraction time; both carry the same stale-until-first-update semantics)
	const float3& GetDrawMidPos(int id) const { return GetDrawPosition(id).midPos; }
	float GetDrawRadius(int id) const { return GetDrawPosition(id).drawRadius; }

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
		// sim|draw PR 40: the object's GetDrawRadius() (localModel bounding
		// radius) captured at UpdateDrawPos, so the frustum twins can InView-test
		// by id without a live localModel read; zeroed with pos/midPos on
		// AddObject, updated once per draw frame alongside them
		float drawRadius = 0.0f;
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

	// sim|draw WS-2: whole-object dirty-skip record for ExtractObjectTransforms
	// (transforms-dirty-skip.md §3). Dense id-indexed like drawPositions; slots
	// go stale on death and are reset by the id's next AddObject. Owned by the
	// extraction's single caller context (sim thread at the frame edge under
	// the split; main thread otherwise -- the split-off for_mt walk writes
	// disjoint per-id slots, the same discipline as the storage writes).
	using TransformSkipKey = std::conditional_t<std::is_same_v<T, CUnit>, UnitTransformSkipKey, FeatureTransformSkipKey>;

	struct TransformSkipState {
		TransformSkipKey key = {};     // value-shadow of the last completed walk
		Transform preFrameTra = Transform{}; // slot-[0] input shadow (see predicate leg 1: preFrameTra is a frame-START sample the edge key does not cover)
		uint64_t pieceTreeVersion = 0; // WS-1 LocalModel counter seen at that walk
		int32_t lastWalkFrame = std::numeric_limits<int32_t>::lowest(); // sim frame of that walk (pending re-extractions can re-walk the same frame; those must not double-count a quiescent edge)
		uint8_t quiescentEdges = 0;    // consecutive frame-distinct walks that were unchanged AND wrote no piece slots, saturating at 2
		bool everWalked = false;       // false until the first completed walk primes the record
	};

	static TransformSkipKey CurrentTransformSkipKey(const T* o) {
		TransformSkipKey key;
		if constexpr (std::is_same_v<T, CUnit>) {
			key.pos = o->pos;
			key.frontdir = o->frontdir;
			key.rightdir = o->rightdir;
			key.updir = o->updir;
		} else {
			std::memcpy(key.transMat, o->GetTransformMatrixRef().m, sizeof(key.transMat));
			key.pos = o->pos;
		}
		return key;
	}

	// SCOPE-1: splitResolveCache/splitResolveCacheBuilt DELETED — id->object
	// resolution now uses the drawer-owned render record's deferred-safe handle.
	std::vector<DrawPosition> drawPositions; // indexed by object id
	std::vector<DrawFlagState> drawFlags;    // indexed by object id
	std::vector<TransformSkipState> transformSkipStates; // indexed by object id (WS-2)

	// WS-2 skip-rate counters (always-on; atomics only for the split-off
	// for_mt walk) + the oracle's teardown counters, logged in the dtor
	std::atomic<uint64_t> traObjGated{0};
	std::atomic<uint64_t> traObjWalked{0};
	std::atomic<uint64_t> traObjSkipped{0};
	std::atomic<uint64_t> traSkipOracleChecked{0};
	std::atomic<uint64_t> traSkipOracleMismatched{0};
	// SimDrawTransformSkipOracle cadence, latched once per extraction pass
	uint64_t transformSkipOracleEpochs = 0;
	bool transformSkipOracleThisEpoch = false;
	spring::unordered_map<int, ScopedTransformMemAlloc> scTransMemAllocMap; // keyed by object id

	// last sim frame ExtractTransforms() ran for; extraction is due once per new sim frame
	int32_t transformsExtractedFrame = std::numeric_limits<int32_t>::lowest();
	// PR 44a: ids added by the current consume's record dispatch (flip only;
	// see ExtractPendingNewObjectTransforms)
	std::vector<int> pendingNewObjectTransformIds;
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
	// WS-2 teardown telemetry (skip-rate + oracle verdict for the gate report)
	if (traObjWalked.load(std::memory_order_relaxed) > 0 || traObjSkipped.load(std::memory_order_relaxed) > 0) {
		LOG("[EpochStats] transformSkip(%s): gated=%llu walked=%llu skipped=%llu oracleChecked=%llu oracleMismatched=%llu",
			GetName().c_str(),
			(unsigned long long)traObjGated.load(std::memory_order_relaxed),
			(unsigned long long)traObjWalked.load(std::memory_order_relaxed),
			(unsigned long long)traObjSkipped.load(std::memory_order_relaxed),
			(unsigned long long)traSkipOracleChecked.load(std::memory_order_relaxed),
			(unsigned long long)traSkipOracleMismatched.load(std::memory_order_relaxed));
	}

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

	if (o->id >= transformSkipStates.size())
		transformSkipStates.resize(o->id + 1);

	transformSkipStates[o->id] = TransformSkipState{}; // everWalked=false: the id's first extraction always walks and primes the key (covers id reuse and creg reload)

	const uint32_t numMatrices = ((o->model ? o->model->numPieces : 0) + 1u) * 2;
	scTransMemAllocMap.emplace(o->id, ScopedTransformMemAlloc(numMatrices));
	transformsExtractionPending = true; //make sure the new allocation is filled at least once before the next sim frame

	// PR 44a: under the running flip the record dispatch (sim parked) adds
	// objects the producer's last extraction predates -- queue them for the
	// consumer's targeted catch-up (ExtractPendingNewObjectTransforms)
	if (SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning())
		pendingNewObjectTransformIds.push_back(o->id);

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
template<typename RecordObjFn>
inline void CModelDrawerDataBase<T>::ExtractPendingNewObjectTransforms(RecordObjFn&& resolveRecordObj)
{
	for (const int id : pendingNewObjectTransformIds) {
		// created-and-died in the same batch: DelObject already dropped the
		// transform allocation, nothing to fill
		if (scTransMemAllocMap.find(id) == scTransMemAllocMap.end())
			continue;

		// record-only resolution (see the declaration comment); a null record
		// handle means the id's object is gone -- nothing to pose
		const T* o = resolveRecordObj(id);

		if (o == nullptr)
			continue;

		ExtractObjectTransforms(o);
	}

	pendingNewObjectTransformIds.clear();
}

template<typename T>
inline void CModelDrawerDataBase<T>::ExtractTransforms(bool allowMT)
{
	if (!transformsExtractionPending && transformsExtractedFrame >= gs->frameNum)
		return;

	transformsExtractionPending = false;
	transformsExtractedFrame = gs->frameNum;

	SCOPED_TIMER("Update::ExtractTransforms");

	// WS-2 §8 value-equivalence oracle cadence (SimDrawTransformSkipOracle=N,
	// 0 = off): every N extraction passes, re-extract each skipped object into
	// scratch and compare against the storage it claims is current
	const int oracleN = configHandler->GetInt("SimDrawTransformSkipOracle");
	transformSkipOracleThisEpoch = (oracleN > 0) && ((++transformSkipOracleEpochs % oracleN) == 0);

	if (mtModelDrawer && allowMT) {
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
	if (!gu->spectatingFullView && !o->alwaysUpdateMat && !o->IsInLosForAllyTeam(gu->myAllyTeam)) {
		traObjGated.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	/* sim|draw WS-2 whole-object dirty-skip (transforms-dirty-skip.md §3-§5).
	 * Skip contract: skip ==> the storage already holds exactly what a full
	 * walk would leave behind. Three legs carry it:
	 *  1. Object slots [0]/[1] are pure (per-machine-deterministic) functions
	 *     of the shadowed inputs. Slot [1]: units' GetTransformMatrix() ==
	 *     ComposeMatrix(pos) reads only {pos, -rightdir, updir, frontdir};
	 *     features' serves the stored transMatrix. The key bitwise-compares
	 *     those CONSUMED values at the edge, so every mutation path -- the
	 *     movetypes' in-place `frontdir += ...` compound writes through
	 *     SyncedFloat3 references, transport attach dir pokes, and paths
	 *     nobody has written yet -- lands in the compare; there is no choke
	 *     inventory to miss. Slot [0]'s input, preFrameTra, is shadowed AS A
	 *     VALUE (skip.preFrameTra) rather than derived from the key: it is a
	 *     frame-START sample of the same pose state (UpdatePrevFrameTransform,
	 *     SolidObject.cpp) while the key samples the frame END, and only at
	 *     extraction edges -- with several sim frames between edges (FF) a
	 *     pose can change and return BIT-EXACTLY inside the blind window
	 *     (common for rotation: dirs re-derive from the quantized heading, and
	 *     turn-in-place leaves pos untouched), leaving preFrameTra carrying
	 *     the excursion at an edge whose key matches (the oracle caught this
	 *     as transient objPrev staleness). Shadowing the consumed value makes
	 *     the slot-[0] equivalence exact by construction: skip requires the
	 *     bytes UpdateIfChanged(0, ...) would reconcile to be the bytes the
	 *     last walk already reconciled. This also closes the design doc's §6
	 *     "accepted residual" (bit-exact pose return while gated out), which
	 *     was the same class.
	 *  2. Piece slots are pure functions of piece-local state (pos/rot/scale/
	 *     pieceSpaceTra, scriptSetVisible, noInterpolation, blockScriptAnims
	 *     + the parent chain); every mutation choke for that state bumps the
	 *     WS-1 piece-tree version (LocalModelPiece::SetDirty entry,
	 *     SetScriptVisible, colvol acquisition, no-interpolation arming,
	 *     CLuaUnitScript::CreateScript), so an unchanged version means no
	 *     piece content changed since the last completed walk.
	 *  3. quiescentEdges counts OBSERVED piece-channel quiescence, not a
	 *     derived settle timeline: it advances only when a frame-distinct
	 *     completed walk was unchanged AND saw zero armed pieces (wrote no
	 *     piece slots), and skipping requires two such walks. Deriving the
	 *     settle point instead ("prev settles one edge after the mutation")
	 *     skipped too early in practice -- the wasUpdated[2] double-trigger
	 *     can drain without the prev slot settling (e.g. a pending-triggered
	 *     re-extraction in the SAME sim frame consumes both triggers before
	 *     the frame-start prevModelSpaceTra re-save; the oracle caught the
	 *     resulting persistent stale piecePrev). Observation closes every
	 *     such path: after two clean walks the piece channel is provably
	 *     drained, and with an unchanged WS-1 version no piece can re-arm --
	 *     arming requires a recompute-of-dirty (TickAllAnims BFS or the lazy
	 *     GetModelSpaceTransform) or SetScriptVisible, and every dirty/
	 *     visible transition bumps; SetDirtyRaw is only used by the BFS to
	 *     clear. A skipped walk would therefore write nothing to the piece
	 *     slots, and the object slots are idempotent under leg 1. Corrected
	 *     timeline, last mutation in frame M: edge M walks (mismatch, writes
	 *     moving prev + final curr), edge M+1 walks (unchanged, pieces still
	 *     armed via wasUpdated[1]: uploads the settled preFrameTra slot [0]
	 *     and settled prevModelSpaceTra), edges M+2/M+3 walk clean (armed
	 *     drained, nothing written; quiescentEdges 1, then 2), edge M+4
	 *     skips.
	 * The record is refreshed ONLY at the end of a completed walk (the LOS
	 * gate return above never touches it), so key/version always describe the
	 * sim state at the moment the storage was last written -- changes made
	 * while gated out of extraction force a walk on the next gate-passing
	 * edge, which is what makes LOS re-entry / spectatingFullView toggles /
	 * allyteam switches need no invalidation hooks. alwaysUpdateMat objects
	 * never skip (conservative: their Lua consumers force-walk every edge). */
	TransformSkipState& skip = transformSkipStates[o->id];
	const TransformSkipKey curKey = CurrentTransformSkipKey(o);
	const uint64_t curPieceTreeVersion = o->localModel.GetPieceTreeVersion();

	const bool unchanged =
		skip.everWalked &&
		(skip.pieceTreeVersion == curPieceTreeVersion) &&
		(std::memcmp(&skip.key, &curKey, sizeof(TransformSkipKey)) == 0) &&
		(std::memcmp(&skip.preFrameTra, &o->preFrameTra, sizeof(Transform)) == 0);

	if (unchanged && skip.quiescentEdges >= 2 && !o->alwaysUpdateMat) {
		traObjSkipped.fetch_add(1, std::memory_order_relaxed);

		if unlikely(transformSkipOracleThisEpoch)
			RunTransformSkipOracle(o);

		return;
	}

	traObjWalked.fetch_add(1, std::memory_order_relaxed);

	bool anyPieceArmed = false;

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

		anyPieceArmed = true;

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

	// WS-2: refresh the skip record only on a completed walk (see above).
	// Quiescence is observational (comment leg 3): the counter advances only
	// when this walk was unchanged AND wrote no piece slots, on a new frame
	// edge (same-frame re-walks -- pending re-extractions -- are byte-identical
	// repeats, not new edges); any piece write resets it.
	if (!unchanged || anyPieceArmed)
		skip.quiescentEdges = 0;
	else if (gs->frameNum != skip.lastWalkFrame)
		skip.quiescentEdges = std::min<uint8_t>(skip.quiescentEdges + 1, 2);

	skip.key = curKey;
	skip.preFrameTra = o->preFrameTra;
	skip.pieceTreeVersion = curPieceTreeVersion;
	skip.lastWalkFrame = gs->frameNum;
	skip.everWalked = true;
}

/* WS-2 §8 value-equivalence oracle: for an object the skip predicate just
 * skipped, recompute what a full walk WOULD WRITE and compare it against the
 * storage. The reference is the walk, not from-scratch freshness: object
 * slots go through UpdateIfChanged's eps-compare, so "what a walk would
 * leave" differs from the stored bytes only when the recomputed value fails
 * Transform::equals; piece slots are only written when the piece's
 * wasUpdated trigger is armed, so unarmed pieces are NOT compared -- a walk
 * would leave their bytes untouched, and storage that is stale relative to a
 * from-scratch recompute but that no walk would refresh (a drained-trigger
 * residue the flag-off baseline shares) is value-equivalent by the program's
 * byte-identity-to-baseline contract, not a skip defect. Armed pieces on a
 * skipped object compare bitwise (UpdateForced bytes); under an unchanged
 * WS-1 version an armed piece here means a bumpless arming path. Read-only:
 * no ResetWasUpdated, no storage writes -- the piece reads may recompute a
 * dirty piece, which is legal at the edge (sim quiescent) and side-effect-
 * equivalent to what the skipped walk would have done. A mismatch names a
 * mutation path that reached SSBO-feeding state without perturbing the skip
 * key -- the "missed a path" failure class turned into a log line. */
template<typename T>
inline void CModelDrawerDataBase<T>::RunTransformSkipOracle(const T* o)
{
	const auto& stma = std::as_const(*this).GetObjectTransformMemAlloc(o);

	if (!stma.Valid())
		return;

	traSkipOracleChecked.fetch_add(1, std::memory_order_relaxed);

	const char* diff = nullptr;
	int diffPiece = -1;

	const auto& tmPrev = o->preFrameTra;
	const auto  tmCurr = Transform::FromMatrix(o->GetTransformMatrix());

	if (!stma[0].equals(tmPrev)) {
		diff = "objPrev";
	} else if (!stma[1].equals(tmCurr)) {
		diff = "objCurr";
	} else {
		for (int i = 0; i < o->localModel.pieces.size(); ++i) {
			const LocalModelPiece& lmp = o->localModel.pieces[i];

			const auto& lmpTransform = lmp.GetModelSpaceTransform();

			// a walk writes this piece's slots only when armed (see above)
			if (!lmp.GetWasUpdated())
				continue;

			Transform expPrev;
			Transform expCurr;

			if (!lmp.GetScriptVisible()) {
				expPrev = Transform::Zero();
				expCurr = Transform::Zero();
			} else {
				expPrev = lmp.GetEffectivePrevModelSpaceTransform();
				expCurr = lmpTransform;
			}

			if (std::memcmp(&stma[2 * (1 + i) + 0], &expPrev, sizeof(Transform)) != 0) {
				diff = "piecePrev";
				diffPiece = i;
				break;
			}
			if (std::memcmp(&stma[2 * (1 + i) + 1], &expCurr, sizeof(Transform)) != 0) {
				diff = "pieceCurr";
				diffPiece = i;
				break;
			}
		}
	}

	if (diff == nullptr)
		return;

	traSkipOracleMismatched.fetch_add(1, std::memory_order_relaxed);
	LOG_L(L_ERROR, "[TransformSkipOracle] %s id=%d field=%s piece=%d frame=%d (skip predicate claimed quiescence over stale storage)",
			GetName().c_str(), o->id, diff, diffPiece, gs->frameNum);
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
