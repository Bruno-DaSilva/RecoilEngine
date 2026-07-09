#pragma once

#include "System/float3.h"
#include "Rendering/Common/ModelDrawerData.h"
#include "Lua/LuaObjectMaterial.h"

class CFeature;
class CCamera;
struct S3DModel;
struct LocalModel;
struct FeatureDef;

class CFeatureDrawerData : public CFeatureDrawerDataBase {
public:
	// CEventClient interface
	bool WantsEvent(const std::string & eventName) {
		return
			eventName == "RenderFeaturePreCreated" ||
			eventName == "RenderFeatureCreated"    ||
			eventName == "RenderFeatureDestroyed";
	}
	void RenderFeaturePreCreated(const CFeature* feature) override;
	void RenderFeatureCreated(const CFeature* feature) override;
	void RenderFeatureDestroyed(const CFeature* feature) override;
public:
	CFeatureDrawerData(bool& mtModelDrawer_);
	virtual ~CFeatureDrawerData();
public:
	void ConfigNotify(const std::string& key, const std::string& value);
public:
	void Update() override;
	bool IsAlpha(const CFeature* co) const override;

	// draw-time transform (drawer-owned since the §A drawPos eviction; this
	// replaced the unsynced half of CFeature::transMatrix). Identity until the
	// feature first passes the draw-flag gate below, as the old member was.
	const CMatrix44f& GetUnsyncedTransformMatrix(const CFeature* f) const;
	// id-keyed variant for draw-side picking (TraceRay holds a snapshot id, not
	// a CFeature*); same identity default for unregistered/stale ids
	const CMatrix44f& GetUnsyncedTransformMatrix(int id) const;

	// distance-fade alpha, written by UpdateObjectDrawFlags each draw frame
	// (drawer-owned since the §A drawAlpha eviction; was a CFeature field).
	// 1.0f until first written, for features never registered here (non-model
	// features) and again after the owning feature's slot is released — each
	// matching the old member's construction-time init.
	float GetDrawAlpha(const CFeature* f) const;

	// SCOPE-1 (plan PR 39): drawer-owned per-feature render record — the
	// immutable header ({model, def}) plus the sim-owned mutable fields the
	// DRAW-window passes read off the live CFeature. Populated producer-side
	// (immutable at RenderFeaturePreCreated; mutable in UpdateRenderRecord from
	// the once-per-frame Update). Draw passes read this by id and never
	// dereference the live sim object. Keyed by feature id; stale after death
	// until id reuse (same semantics as unsyncedTransforms/drawAlphas).
	struct FeatureRenderRecord {
		// deferred-deletion-safe handle (see UnitRenderRecord::obj). For non-model
		// features (trees/geo-vents) that RenderFeaturePreCreated does not register
		// with the model drawer, this is still set (RegisterNonModelFeature) so the
		// Lua draw-payload resolvers (LuaSnapshotServe::ResolveDrawFeature) find them
		// — replacing the deleted RegisterExtraSplitResolveIDs / splitResolveCache.
		const CFeature*   obj   = nullptr;
		const S3DModel*   model = nullptr; // immutable: feature->model (null for non-model)
		const FeatureDef* def   = nullptr; // immutable: feature->def
		// immutable interior pointer for the legacy immediate-mode piece path
		// (see UnitRenderRecord::localModel); GL4 SSBO path never touches it
		const LocalModel* localModel = nullptr;
		int     team           = 0;
		uint8_t engineDrawMask = 0;
		bool    luaDraw        = false;
		// evicted from LocalModel/LocalModelPiece (sim/draw PR 10): draw/Lua-owned
		// per-object Lua material state + per-piece LOD display lists. Never
		// sim-touched; keyed by feature id, cleared with the record at destroy.
		LuaObjectMaterialData luaMaterialData;
		std::vector<std::vector<uint32_t>> lodDispLists; // [pieceIndex][lod]
	};

	const FeatureRenderRecord& GetRenderRecord(const CFeature* f) const;
	const FeatureRenderRecord& GetRenderRecord(int id) const {
		static const FeatureRenderRecord def = {};
		return (static_cast<size_t>(id) < renderRecords.size()) ? renderRecords[id] : def;
	}

	// MUTABLE draw/Lua-owned eviction accessors (sim/draw PR 10); resize the
	// record store on demand so unregistered ids get a valid default-constructed
	// slot instead of crashing
	LuaObjectMaterialData& GetLuaMaterialDataRef(int id) {
		if (static_cast<size_t>(id) >= renderRecords.size())
			renderRecords.resize(id + 1);
		return renderRecords[id].luaMaterialData;
	}
	std::vector<std::vector<uint32_t>>& GetLodDispListsRef(int id) {
		if (static_cast<size_t>(id) >= renderRecords.size())
			renderRecords.resize(id + 1);
		return renderRecords[id].lodDispLists;
	}

	// PR 43 (3b): clear the destroy-retained records at the end of the
	// boundary dispatch window (barrier step 8 / valve), before the ack
	// poisons their shell handles. See RenderFeatureDestroyed.
	void ClearDeadRetainedRecords();
protected:
	void UpdateObjectDrawFlags(const CSolidObject* o) override;
private:
	// SCOPE-1: register the deferred-safe handle for non-model features (trees/
	// geo-vents) so the Lua draw-payload resolvers find them. Called each Update
	// (producer-side). Replaces the deleted RegisterExtraSplitResolveIDs.
	void RegisterNonModelFeatureRecords();

	void UpdateDrawPos(const CFeature* f);
	void UpdateUnsyncedTransform(const CFeature* f);

	// render-record producer-side authoring (see FeatureRenderRecord above)
	FeatureRenderRecord& RenderRecordRef(const CFeature* f);
	void UpdateRenderRecord(const CFeature* feature);

	// PR 43 (3b): (id, shell) of records retained past their destroy-record
	// dispatch so died-in-batch ids resolve through DrawerGetObjectByID for
	// the deferred dispatches (replaces the IdToObject shell fallback)
	std::vector<std::pair<int, const CFeature*>> deadRetainedRecords;
public:
	float featureDrawDistance;
	float featureFadeDistance;
private:
	std::vector<CMatrix44f> unsyncedTransforms; // indexed by feature id
	std::vector<float> drawAlphas; // indexed by feature id (see GetDrawAlpha)
	std::vector<FeatureRenderRecord> renderRecords; // indexed by feature id
};