/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _FEATURE_HANDLER_H
#define _FEATURE_HANDLER_H

#include <vector>

#include "System/float3.h"
#include "System/Misc/NonCopyable.h"
#include "System/creg/creg_cond.h"
#include "System/UnorderedSet.hpp"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/SimObjectIDPool.h"

class CSolidObject;
struct UnitDef;
class LuaTable;
struct FeatureDef;

struct FeatureLoadParams {
	const CSolidObject* parentObj;
	const UnitDef* unitDef;
	const FeatureDef* featureDef;

	// not used if parentObj != nullptr
	float3 pos;
	float3 speed;

	int featureID;
	int teamID;
	int allyTeamID;

	short int heading;
	short int facing;

	int wreckLevels;
	int smokeTime;
};


class CFeature;
class CFeatureHandler : public spring::noncopyable
{
	CR_DECLARE_STRUCT(CFeatureHandler)

public:
	CFeatureHandler(): idPool(MAX_FEATURES) {}

	void Init();
	void Kill();

	CFeature* LoadFeature(const FeatureLoadParams& params);
	CFeature* CreateWreckage(const FeatureLoadParams& params);
	CFeature* GetFeature(unsigned int id) { return ((id < features.size())? features[id]: nullptr); }

	void UpdatePreFrame();
	void Update();

	bool UpdateFeature(CFeature* feature);
	bool TryFreeFeatureID(int id);
	bool AddFeature(CFeature* feature);
	void DeleteFeature(CFeature* feature);

	void LoadFeaturesFromMap();

	void SetFeatureUpdateable(CFeature* feature);
	void TerrainChanged(int x1, int y1, int x2, int y2);

	// A checkpoint load re-queues extra features for update (terrain restore and
	// quadfield re-registration add them), whereas a continuous run only keeps the
	// features that actually need per-frame updates. Processing the spurious extras
	// on the first post-load frame performs synced feature physics they would not
	// otherwise run, which desyncs the running checksum even though the resulting
	// state is identical. PostLoad snapshots the saved queue (before those side
	// effects) and RebuildUpdateQueueAfterLoad restores it exactly.
	void SnapshotUpdateQueueForSave();
	void RebuildUpdateQueueAfterLoad();

	const spring::unordered_set<int>& GetActiveFeatureIDs() const { return activeFeatureIDs; }

private:
	bool CanAddFeature(int id) const {
		// do we want to be assigned a random ID and are any left in pool?
		if (id < 0)
			return true;
		// is this ID not already in use *and* has it been recycled by pool?
		if (id < features.size())
			return (features[id] == nullptr && idPool.HasID(id));
		// AddFeature will not make new room for us
		return false;
	}

	void InsertActiveFeature(CFeature* feature);

private:
	SimObjectIDPool idPool;

	spring::unordered_set<int> activeFeatureIDs;
	std::vector<int> deletedFeatureIDs;
	std::vector<CFeature*> features;
	std::vector<CFeature*> updateFeatures;

	// The genuine update queue captured at save time (SnapshotUpdateQueueForSave).
	// Serialized alongside updateFeatures but, unlike it, never touched by the
	// checkpoint-load re-queue-every-feature pollution, so on load it still holds
	// exactly the saved set/order. RebuildUpdateQueueAfterLoad restores from it.
	std::vector<CFeature*> savedUpdateQueue;
};

extern CFeatureHandler featureHandler;


#endif // _FEATURE_HANDLER_H
