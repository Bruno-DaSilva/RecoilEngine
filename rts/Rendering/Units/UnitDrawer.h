/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#pragma once

#include <vector>
#include <array>

#include "Rendering/Common/ModelDrawer.h"
#include "Rendering/Common/ModelDrawerState.hpp"
#include "Rendering/Units/UnitDrawerData.h"
#include "Rendering/GL/LightHandler.h"
#include "Game/UI/CursorIcons.h"
#include "System/type2.h"
#include "Sim/Units/CommandAI/Command.h"

class CSolidObject;
class CUnit;
struct S3DModel;
struct SolidObjectDef;

namespace Shader { struct IProgramObject; }

class CUnitDrawer : public CModelDrawerBase<CUnitDrawerData, CUnitDrawer>
{
public:
	static void InitStatic();
	static void KillStatic(bool reload); //use base
	//static void UpdateStatic(); //use base
public:
	// Interface with CUnitDrawerData
	static void UpdateGhostedBuildings() { modelDrawerData->UpdateGhostedBuildings(); }

	static uint32_t GetUnitDefImage(const UnitDef* ud) { return modelDrawerData->GetUnitDefImage(ud); }
	static void SetUnitDefImage(const UnitDef* unitDef, const std::string& texName) { return modelDrawerData->SetUnitDefImage(unitDef, texName); }
	static void SetUnitDefImage(const UnitDef* unitDef, uint32_t texID, int xsize, int ysize) { return modelDrawerData->SetUnitDefImage(unitDef, texID, xsize, ysize); }

	static bool& UseScreenIcons() { return modelDrawerData->useScreenIcons; }

	static float GetUnitIconFadeStart() { return modelDrawerData->GetUnitIconFadeStart(); }
	static void SetUnitIconFadeStart(float scale) { modelDrawerData->SetUnitIconFadeStart(scale); }

	static float GetUnitIconScaleUI() { return modelDrawerData->GetUnitIconScaleUI(); }
	static void SetUnitIconScaleUI(float scale) { modelDrawerData->SetUnitIconScaleUI(scale); }

	static float GetUnitIconDist(float dist) { return modelDrawerData->unitIconDist; }
	static void SetUnitIconDist(float dist) { modelDrawerData->SetUnitIconDist(dist); }

	static float GetUnitIconFadeVanish() { return modelDrawerData->iconFadeVanish; }
	static void SetUnitIconFadeVanish(float dist) { modelDrawerData->SetUnitIconFadeVanish(dist); }

	static bool& IconHideWithUI() { return modelDrawerData->iconHideWithUI; }

	static void AddTempDrawUnit(const CUnitDrawerData::TempDrawUnit& tempDrawUnit) { modelDrawerData->AddTempDrawUnit(tempDrawUnit); }

	static const std::vector<const CUnit*>& GetUnsortedUnits() { return modelDrawerData->GetUnsortedObjects(); }

	// drawer-owned draw-time positions/transforms (sim/draw §A drawPos eviction)
	static const float3& GetDrawPos(const CUnit* unit) { return modelDrawerData->GetDrawPos(unit); }
	static const float3& GetDrawMidPos(const CUnit* unit) { return modelDrawerData->GetDrawMidPos(unit); }
	static float3 GetObjectSpaceDrawPos(const CUnit* unit, const float3& p) { return modelDrawerData->GetObjectSpaceDrawPos(unit, p); }
	static float3 GetObjDrawMidPos(const CUnit* unit) { return modelDrawerData->GetObjDrawMidPos(unit); }
	static float3 GetObjDrawErrorPos(const CUnit* unit, int allyteam) { return modelDrawerData->GetObjDrawErrorPos(unit, allyteam); }
	static CMatrix44f GetUnsyncedTransformMatrix(const CUnit* unit, bool fullread = false) { return modelDrawerData->GetUnsyncedTransformMatrix(unit, fullread); }

	// drawer-owned draw-visibility flags + icon state (sim/draw §A drawFlag eviction, PR 4)
	static uint8_t GetDrawFlag(const CUnit* unit) { return modelDrawerData->GetDrawFlag(unit); }
	static uint8_t GetPreviousDrawFlag(const CUnit* unit) { return modelDrawerData->GetPreviousDrawFlag(unit); }
	static bool HasDrawFlag(const CUnit* unit, DrawFlags f) { return modelDrawerData->HasDrawFlag(unit, f); }
	static bool GetIsIcon(const CUnit* unit) { return modelDrawerData->GetUnitIsIcon(unit); }

	static void ClearPreviousDrawFlags() { modelDrawerData->ClearPreviousDrawFlags(); }
	static void UnitLeavesGhostChanged(const CUnit* unit, const bool leaveDeadGhost) { modelDrawerData->UnitLeavesGhostChanged(unit, leaveDeadGhost); }

	// renderEventQueue dispatch targets for queued LOS-transition records
	static void ApplyUnitRadarChanged(const CUnit* unit, int allyTeam) { modelDrawerData->ApplyUnitRadarChanged(unit, allyTeam); }
	static void ApplyUnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent) { modelDrawerData->ApplyUnitEnteredLos(unit, allyTeam, leavesGhostAtEvent); }
	static void ApplyUnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent) { modelDrawerData->ApplyUnitLeftLos(unit, allyTeam, leavesGhostAtEvent); }
	static void ApplyUnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask) { modelDrawerData->ApplyUnitLeavesGhostChanged(unit, deadGhostAllyMask); }

	static void UpdateCurrentUnitIcon(const CUnit* unit) { modelDrawerData->UpdateCurrentUnitIcon(unit); }

	// drawer-owned per-unit icon state (sim/draw §A icon-state eviction, PR 5);
	// GetUnitIconRadius also serves draw-side picking (TraceRay)
	static size_t GetUnitIconIndex(const CUnit* unit) { return modelDrawerData->GetUnitIconIndex(unit); }
	static float GetUnitIconRadius(const CUnit* unit) { return modelDrawerData->GetUnitIconRadius(unit); }
	static bool GetUnitDrawIcon(const CUnit* unit) { return modelDrawerData->GetUnitDrawIcon(unit); }
	static void SetUnitDrawIcon(const CUnit* unit, bool b) { modelDrawerData->SetUnitDrawIcon(unit, b); }
	static void SetUnitCustomIcon(const CUnit* unit, size_t iconIdx) { modelDrawerData->SetUnitCustomIcon(unit, iconIdx); }
public:
	// DrawUnit*
	virtual void DrawUnitNoTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const = 0;
	virtual void DrawUnitTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const = 0;
	virtual void DrawIndividual(const CUnit* unit, bool noLuaCall) const = 0;
	virtual void DrawIndividualNoTrans(const CUnit* unit, bool noLuaCall) const = 0;

	// DrawIndividualDef*
	virtual void DrawIndividualDefOpaque(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const = 0;
	virtual void DrawIndividualDefAlpha(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const = 0;

	// Icons Minimap
	virtual void DrawUnitMiniMapIcons() const = 0;
	        void UpdateUnitIconsByUnitDef(const UnitDef* ud) { modelDrawerData->UpdateUnitIconsByUnitDef(ud); }

	// Icons Map
	virtual void DrawUnitIcons() const = 0;
	virtual void DrawUnitIconsScreen() const = 0;

	// Build Squares
	        bool ShowUnitBuildSquare(const BuildInfo& buildInfo) const { return ShowUnitBuildSquare(buildInfo, std::vector<Command>()); }
	virtual bool ShowUnitBuildSquare(const BuildInfo& buildInfo, const std::vector<Command>& commands) const = 0;

	virtual void DrawBuildIcons(const std::vector<CCursorIcons::BuildIcon>& buildIcons) const = 0;
protected:
	static bool ShouldDrawOpaqueUnit(const CUnit* u, uint8_t thisPassMask);
	static bool ShouldDrawAlphaUnit(const CUnit* u, uint8_t thisPassMask);
	static bool ShouldDrawUnitShadow(const CUnit* u);

	virtual void DrawGhostedBuildings(int modelType) const = 0;
protected:
	inline static Shader::IProgramObject* icons2DShader = nullptr;
	inline static Shader::IProgramObject* icons3DShader = nullptr;
private:
	inline static std::array<CUnitDrawer*, ModelDrawerTypes::MODEL_DRAWER_CNT> unitDrawers = {};
public:
	enum BuildStages {
		BUILDSTAGE_WIRE = 0,
		BUILDSTAGE_FLAT = 1,
		BUILDSTAGE_FILL = 2,
		BUILDSTAGE_NONE = 3,
		BUILDSTAGE_CNT = 4,
	};
};

class CUnitDrawerBase : public CUnitDrawer {
public:
	void DrawOpaquePass(bool deferredPass, bool drawReflection, bool drawRefraction) const override {
		DrawOpaquePassImpl<LuaObjType::LUAOBJ_UNIT>(deferredPass, drawReflection, drawRefraction);
	}
	void DrawAlphaPass(bool drawReflection, bool drawRefraction = false) const override {
		DrawAlphaPassImpl<LuaObjType::LUAOBJ_UNIT>(drawReflection, drawRefraction);
	}
protected:
	void DrawOpaqueObjectsLua(bool deferredPass, bool drawReflection, bool drawRefraction) const override {
		eventHandler.DrawOpaqueUnitsLua(deferredPass, drawReflection, drawRefraction);
	}
	void DrawAlphaObjectsLua(bool drawReflection, bool drawRefraction) const override {
		eventHandler.DrawAlphaUnitsLua(drawReflection, drawRefraction);
	}
	void DrawShadowObjectsLua() const override {
		eventHandler.DrawShadowUnitsLua();
	}
	void Update() const override;
};

class CUnitDrawerGLSL : public CUnitDrawerBase {
public:
	CUnitDrawerGLSL();
	~CUnitDrawerGLSL() override;
public:
	void Draw(bool drawReflection, bool drawRefraction = false) const override {
		DrawImpl<true, LuaObjType::LUAOBJ_UNIT>(drawReflection, drawRefraction);
	}
	void DrawShadowPass() const override {
		DrawShadowPassImpl<true, LuaObjType::LUAOBJ_UNIT>();
	}

	void DrawUnitModel(const CUnit* unit, bool noLuaCall) const;

	void DrawUnitNoTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const override;
	void DrawUnitTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const override;
	void DrawIndividual(const CUnit* unit, bool noLuaCall) const override;
	void DrawIndividualNoTrans(const CUnit* unit, bool noLuaCall) const override;

	void DrawIndividualDefOpaque(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const override;
	void DrawIndividualDefAlpha(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const override;

	bool ShowUnitBuildSquare(const BuildInfo& buildInfo, const std::vector<Command>& commands) const override;
	void DrawBuildIcons(const std::vector<CCursorIcons::BuildIcon>& buildIcons) const override;

	void DrawUnitMiniMapIcons() const override;
	void DrawUnitIcons() const override;
	void DrawUnitIconsScreen() const override;
protected:
	void DrawObjectsShadow(int modelType) const override;
	void DrawOpaqueObjects(int modelType, bool drawReflection, bool drawRefraction) const;
	void DrawOpaqueObjectsAux(int modelType) const override; //AI units

	void DrawAlphaObjects(int modelType, bool drawReflection, bool drawRefraction) const override;
	void DrawAlphaObjectsAux(int modelType) const override;

	void DrawGhostedBuildings(int modelType) const override;

	void DrawOpaqueUnit(const CUnit* unit, uint8_t thisPassMask) const;
	void DrawUnitShadow(const CUnit* unit) const;
	void DrawAlphaUnit(const CUnit* unit, int modelType, uint8_t thisPassMask, bool drawGhostBuildingsPass) const;

	void DrawOpaqueAIUnit(const CUnitDrawerData::TempDrawUnit& unit) const;
	void DrawAlphaAIUnit(const CUnitDrawerData::TempDrawUnit& unit) const;
	void DrawAlphaAIUnitBorder(const CUnitDrawerData::TempDrawUnit& unit) const;

	void DrawUnitModelBeingBuiltShadow(const CUnit* unit, bool noLuaCall) const;
	void DrawModelWireBuildStageShadow(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;
	void DrawModelFlatBuildStageShadow(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;
	void DrawModelFillBuildStageShadow(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;

	void DrawUnitModelBeingBuiltOpaque(const CUnit* unit, bool noLuaCall) const;
	void DrawModelWireBuildStageOpaque(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;
	void DrawModelFlatBuildStageOpaque(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;
	void DrawModelFillBuildStageOpaque(const CUnit* unit, const double* upperPlane, const double* lowerPlane, bool noLuaCall) const;

	void PushIndividualOpaqueState(const CUnit* unit, bool deferredPass) const;
	void PushIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass)  const;
	void PushIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass) const;

	void PopIndividualOpaqueState(const CUnit* unit, bool deferredPass) const;
	void PopIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass) const;
	void PopIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass) const;

	void DrawUnitMiniMapIcon(TypedRenderBuffer<VA_TYPE_2DTC3>& rb, size_t iconIdx, const float iconScale, const float3& pos, const SColor& color) const;
	float DrawUnitIcon(TypedRenderBuffer<VA_TYPE_TC3>& rb, size_t iconIdx, const float unitRadius, float3 pos, const SColor& color) const;
	void DrawUnitIconScreen(TypedRenderBuffer<VA_TYPE_2DTC3>& rb, size_t iconIdx, const float3& pos, SColor& color, float unitRadius, bool isIcon) const;
};

//TODO remove CUnitDrawerLegacy inheritance
class CUnitDrawerGL4 final : public CUnitDrawerGLSL {
public:
	void Draw(bool drawReflection, bool drawRefraction = false) const override {
		DrawImpl<false, LuaObjType::LUAOBJ_UNIT>(drawReflection, drawRefraction);
	}
	void DrawShadowPass() const override {
		DrawShadowPassImpl<false, LuaObjType::LUAOBJ_UNIT>();
	}

	// DrawUnit*
	/* TODO figure out
	void DrawUnitModel(const CUnit* unit, bool noLuaCall) const = 0;
	void DrawUnitModelBeingBuiltShadow(const CUnit* unit, bool noLuaCall) const = 0;
	void DrawUnitModelBeingBuiltOpaque(const CUnit* unit, bool noLuaCall) const = 0;
	void DrawUnitNoTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const = 0;
	void DrawUnitTrans(const CUnit* unit, uint32_t preList, uint32_t postList, bool lodCall, bool noLuaCall) const = 0;
	void DrawIndividual(const CUnit* unit, bool noLuaCall) const = 0;
	void DrawIndividualNoTrans(const CUnit* unit, bool noLuaCall) const = 0;
	*/

	// DrawIndividualDef*
	/*
	void DrawIndividualDefOpaque(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const = 0;
	void DrawIndividualDefAlpha(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen = false) const = 0;
	*/

	void DrawBuildIcons(const std::vector<CCursorIcons::BuildIcon>& buildIcons) const override;
protected:
	void DrawObjectsShadow(int modelType) const override;
	void DrawOpaqueObjects(int modelType, bool drawReflection, bool drawRefraction) const override;
	void DrawAlphaObjects(int modelType, bool drawReflection, bool drawRefraction) const override;

	void DrawAlphaObjectsAux(int modelType) const override;
	void DrawAlphaAIUnit(const CUnitDrawerData::TempDrawUnit& unit) const;
	void DrawAlphaAIUnitBorder(const CUnitDrawerData::TempDrawUnit& unit) const {} //not implemented

	void DrawOpaqueObjectsAux(int modelType) const override;
	void DrawOpaqueAIUnit(const CUnitDrawerData::TempDrawUnit& unit) const;

	void DrawGhostedBuildings(int modelType) const override {} //implemented in-line

	void DrawUnitModelBeingBuiltShadow(const CUnit* unit, bool noLuaCall) const;
	void DrawUnitModelBeingBuiltOpaque(const CUnit* unit, bool noLuaCall) const;
};

#define unitDrawer (CUnitDrawer::modelDrawer)
