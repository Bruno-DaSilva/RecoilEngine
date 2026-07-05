#pragma once

#include "System/float3.h"
#include "Rendering/Common/ModelDrawerData.h"

class CFeature;
class CCamera;

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
protected:
	void UpdateObjectDrawFlags(CSolidObject* o) override;
private:
	void UpdateDrawPos(CFeature* f);
	void UpdateUnsyncedTransform(const CFeature* f);
public:
	float featureDrawDistance;
	float featureFadeDistance;
private:
	std::vector<CMatrix44f> unsyncedTransforms; // indexed by feature id
};