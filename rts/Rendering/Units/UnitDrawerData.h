/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#pragma once

#include <ranges>

#include "System/float3.h"
#include "Rendering/Common/ModelDrawerData.h"
#include "Rendering/Common/RenderEventQueue.h"
#include "Rendering/UnitDefImage.h"
#include "Game/GlobalUnsynced.h"

struct S3DModel;
class CUnitDrawer;
struct UnitDef;

namespace GL {
	struct GeometryBuffer;
}

class GhostSolidObject {
	CR_DECLARE_STRUCT(GhostSolidObject)
public:
	void IncRef() { (refCount++); }
	bool DecRef() { return ((refCount--) > 1); }
	const S3DModel* GetModel() const;
	void PostLoad();
public:
	std::string modelName;

	float3 pos;
	float3 midPos;
	float3 dir;
	float radius;
	float iconRadius;

	int refCount;
	int facing; //FIXME replaced with dir-vector just legacy decal drawer uses this
	uint8_t team;
	size_t currentIconIndex;
private:
	mutable const S3DModel* model;
};

class CUnitDrawerData : public CUnitDrawerDataBase {
public:
	// CEventClient interface
	bool WantsEvent(const std::string& eventName) override {
		return
			eventName == "RenderUnitPreCreated" ||
			eventName == "RenderUnitCreated" || eventName == "RenderUnitDestroyed" ||
			eventName == "UnitEnteredRadar"  || eventName == "UnitEnteredLos"      ||
			eventName == "UnitLeftRadar"     || eventName == "UnitLeftLos"         ||
			eventName == "PlayerChanged";
	}

	void RenderUnitPreCreated(const CUnit* unit) override;
	void RenderUnitCreated(const CUnit* unit, int cloaked) override;
	void RenderUnitDestroyed(const CUnit* unit) override;

	// LOS-transition event handlers append records to renderEventQueue; the
	// Apply* counterparts below run at the draw-boundary drain (or in place
	// outside the sim phase) and hold the original mutation logic
	void UnitEnteredRadar(const CUnit* unit, int allyTeam) override;
	void UnitLeftRadar(const CUnit* unit, int allyTeam) override;

	void UnitEnteredLos(const CUnit* unit, int allyTeam) override;
	void UnitLeftLos(const CUnit* unit, int allyTeam) override;

	void ApplyUnitRadarChanged(const CUnit* unit, int allyTeam);
	void ApplyUnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent);
	void ApplyUnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent);
	void ApplyUnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask);

	void PlayerChanged(int playerID) override;

	GhostAllyMask CalcDeadGhostAllyMask(const CUnit* unit) const;
	bool UpdateUnitGhosts(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask);
	void UnitLeavesGhostChanged(const CUnit* unit, const bool leaveDeadGhost);
public:
	class TempDrawUnit {
		CR_DECLARE_STRUCT(TempDrawUnit)
	public:
		const UnitDef* GetUnitDef() const;
		int unitDefId;

		int team;
		int facing;
		int timeout;

		float3 pos;
		float rotation;

		bool drawAlpha;
		bool drawBorder;
	private:
		mutable const UnitDef* unitDef;
	};
	struct SavedData {
		CR_DECLARE_STRUCT(SavedData)

		/// AI unit ghosts
		std::array< std::vector<TempDrawUnit>, MODELTYPE_CNT> tempOpaqueUnits;
		std::array< std::vector<TempDrawUnit>, MODELTYPE_CNT> tempAlphaUnits;

		/// buildings that were in LOS_PREVLOS when they died and not in LOS since
		std::vector<std::array<std::vector<GhostSolidObject*>, MODELTYPE_CNT>> deadGhostBuildings;

		/// buildings that left LOS but are still alive
		std::vector<std::array<std::vector<CUnit*>, MODELTYPE_CNT>> liveGhostBuildings;
	};
public:
	CUnitDrawerData(bool& mtModelDrawer_);
	virtual ~CUnitDrawerData();
public:
	void SetUnitIconDist(float dist) {
		unitIconDist = dist;
		iconLength = unitIconDist * unitIconDist * 750.0f;
	}

	// IconsAsUI
	float GetUnitIconScaleUI() const { return iconScale; }
	float GetUnitIconFadeStart() const { return iconFadeStart; }
	float GetUnitIconFadeVanish() const { return iconFadeVanish; }
	void SetUnitIconScaleUI(float scale) { iconScale = std::clamp(scale, 0.1f, 10.0f); }
	void SetUnitIconFadeStart(float scale) { iconFadeStart = std::clamp(scale, 1.0f, 10000.0f); }
	void SetUnitIconFadeVanish(float scale) { iconFadeVanish = std::clamp(scale, 1.0f, 10000.0f); }

	// *UnitDefImage
	void SetUnitDefImage(const UnitDef* unitDef, const std::string& texName);
	void SetUnitDefImage(const UnitDef* unitDef, unsigned int texID, int xsize, int ysize);
	uint32_t GetUnitDefImage(const UnitDef* unitDef);
public:
	void Update() override;
	bool IsAlpha(const CUnit* co) const override { return co->IsCloaked(); }
public:
	void AddTempDrawUnit(const TempDrawUnit& tempDrawUnit);

	void UpdateGhostedBuildings();
	void UpdateUnitIconsByUnitDef(const UnitDef* ud);
public:
	void UpdateCurrentUnitIcon(const CUnit* unit);

	const auto& GetUnitDefImages() const { return unitDefImages; }
	      auto& GetUnitDefImages() { return unitDefImages; }

	const auto& GetTempOpaqueDrawUnits(int modelType) const { return savedData.tempOpaqueUnits[modelType]; }
	const auto& GetTempAlphaDrawUnits(int modelType) const { return  savedData.tempAlphaUnits[modelType]; }

	auto GetDeadGhostBuildings(int allyTeam) const {
		assert((unsigned)gu->myAllyTeam < savedData.deadGhostBuildings.size());
		return std::views::join(savedData.deadGhostBuildings[allyTeam]);
	}

	const auto& GetDeadGhostBuildings(int allyTeam, int modelType) const {
		assert((unsigned)gu->myAllyTeam < savedData.deadGhostBuildings.size());
		return savedData.deadGhostBuildings[allyTeam][modelType];
	}
	const auto& GetLiveGhostBuildings(int allyTeam, int modelType) const {
		assert((unsigned)gu->myAllyTeam < savedData.liveGhostBuildings.size());
		return savedData.liveGhostBuildings[allyTeam][modelType];
	}

	auto*       GetSavedData()       { return &savedData; }
	const auto* GetSavedData() const { return &savedData; }
protected:
	void UpdateObjectDrawFlags(CSolidObject* o) override;
public:
	// icon state lives in the drawFlag storage (SO_DRICON_FLAG); moved off CUnit
	// with the flags (sim/draw §A, PR 4). Read via CUnitDrawer::GetIsIcon.
	bool GetUnitIsIcon(const CUnit* u) const { return HasDrawFlag(u, DrawFlags::SO_DRICON_FLAG); }
	void SetUnitIsIcon(const CUnit* u, bool b) {
		if (b)
			AddDrawFlag(u, DrawFlags::SO_DRICON_FLAG);
		else
			DelDrawFlag(u, DrawFlags::SO_DRICON_FLAG);
	}

	// render-owned per-unit icon state (sim/draw §A, PR 5: these were CUnit
	// members authored by draw code and unsynced Lua — evicted to drawer
	// storage). Keyed by unit id; slots are default-initialized at the
	// RenderUnitPreCreated drain (definedIconName captured from the unitDef
	// there, matching the old CUnit::UpdateRenderParams creation-time freeze),
	// stale after death until id reuse, and unregistered ids read the defaults
	// below — each matching the old member semantics. iconRadius stays
	// picking-visible through GetUnitIconRadius (TraceRay reads it via the
	// CUnitDrawer static).
	struct UnitIconState {
		std::string definedIconName;             // unitDef->iconName frozen at registration
		size_t currentIconIndex = size_t(-1);    // icon::INVALID_ICON_INDEX
		size_t customIconIndex = size_t(-1);     // icon::INVALID_ICON_INDEX
		float iconRadius = 0.0f;                 // world-space click radius while iconified
		bool drawIcon = true;                    // Lua Spring.SetUnitIconDraw
	};

	size_t GetUnitIconIndex(const CUnit* u) const { return GetIconState(u).currentIconIndex; }
	float GetUnitIconRadius(const CUnit* u) const { return GetIconState(u).iconRadius; }
	bool GetUnitDrawIcon(const CUnit* u) const { return GetIconState(u).drawIcon; }

	// writes resize on demand: unsynced Lua can address a unit in the window
	// between its sim-side creation and the boundary drain that registers it
	// (such writes are then reset by the slot init at the drain; see the
	// enumerated deviation in the PR-5 commit message)
	void SetUnitCustomIcon(const CUnit* u, size_t iconIdx) { IconStateRef(u).customIconIndex = iconIdx; }
	void SetUnitDrawIcon(const CUnit* u, bool b) { IconStateRef(u).drawIcon = b; }
	void SetUnitIconRadius(const CUnit* u, float r) { IconStateRef(u).iconRadius = r; }
private:
	void UpdateTempDrawUnits(std::vector<TempDrawUnit>& tempDrawUnits);

	void UpdateUnitIconState(CUnit* unit);
	void UpdateUnitIconStateScreen(CUnit* unit);
	void UpdateDrawPos(CUnit* unit);
public:
	// draw-time positions/transforms with radar error applied (drawer-owned since
	// the §A drawPos eviction; these replaced CUnit::GetObjDrawErrorPos and the
	// unsynced branch of CUnit::GetTransformMatrix)
	float3 GetObjDrawErrorPos(const CUnit* unit, int allyteam) const;
	CMatrix44f GetUnsyncedTransformMatrix(const CUnit* unit, bool fullread = false) const;
private:

	/// Returns true if the given unit should be drawn as icon in the current frame.
	bool DrawAsIconByDistance(const CUnit* unit, const float sqUnitCamDist) const;
	//bool DrawAsIconScreen(CUnit* unit) const;
public:
	// lengths & distances
	float unitIconDist;
	float iconLength;

	//icons
	bool iconHideWithUI = true;
	float ghostIconDimming = 0.5f;

	// IconsAsUI
	bool useScreenIcons = false;
	float iconZoomDist;
	float iconSizeBase = 32.0f;
	float iconScale = 1.0f;
	float iconFadeStart = 3000.0f;
	float iconFadeVanish = 1000.0f;

	void ConfigNotify(const std::string& key, const std::string& value);
private:
	const UnitIconState& GetIconState(const CUnit* u) const;
	UnitIconState& IconStateRef(const CUnit* u);

	SavedData savedData;

	std::vector<UnitIconState> iconStates; // indexed by unit id

	std::vector<UnitDefImage> unitDefImages;

	S3DModel* GetUnitModel(const CUnit* unit) const;
	void RemoveDeadGhost(GhostSolidObject* gso, std::vector<GhostSolidObject*>& dgb, int index);

	// icons
	bool useDistToGroundForIcons;
	float sqCamDistToGroundForIcons;

	// IconsAsUI
	static constexpr float iconSizeMult = 0.005f; // 1/200
};
