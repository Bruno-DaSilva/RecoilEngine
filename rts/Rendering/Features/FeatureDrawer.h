/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#pragma once

#include <vector>
#include <array>

#include "Game/Camera.h"
#include "Rendering/Models/ModelRenderContainer.h"
#include "Rendering/Common/ModelDrawer.h"
#include "Rendering/Features/FeatureDrawerData.h"

class CFeature;

class CFeatureDrawer: public CModelDrawerBase<CFeatureDrawerData, CFeatureDrawer>
{
public:
	static void InitStatic();
	//static void KillStatic(bool reload); will use base
	//static void UpdateStatic();
public:
	// DrawFeature*
	virtual void DrawFeatureNoTrans(const CFeature* feature, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall) const = 0;
	virtual void DrawFeatureTrans(const CFeature* feature, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall) const = 0;

	/// LuaOpenGL::Feature{Raw}: draw a single feature with full state setup
	virtual void DrawIndividual(const CFeature* feature, bool noLuaCall) const = 0;
	virtual void DrawIndividualNoTrans(const CFeature* feature, bool noLuaCall) const = 0;
protected:
	virtual void DrawOpaqueFeature(const CFeature* f, uint8_t thisPassMask) const = 0;
	virtual void DrawAlphaFeature(const CFeature* f, uint8_t thisPassMask) const = 0;
public:
	// modelDrawerData proxies
	void ConfigNotify(const std::string& key, const std::string& value) { modelDrawerData->ConfigNotify(key, value); }
	static const std::vector<int>& GetUnsortedFeatures() { return modelDrawerData->GetUnsortedObjects(); } // feature ids (PR 14)

	// drawer-owned draw-time positions/transforms (sim/draw §A drawPos eviction)
	static const float3& GetDrawPos(const CFeature* feature) { return modelDrawerData->GetDrawPos(feature); }
	static const float3& GetDrawMidPos(const CFeature* feature) { return modelDrawerData->GetDrawMidPos(feature); }
	// id-keyed forms for the sim|draw PR 40 frustum/screen-rect twins
	static const float3& GetDrawPos(int featureID) { return modelDrawerData->GetDrawPos(featureID); }
	static const float3& GetDrawMidPos(int featureID) { return modelDrawerData->GetDrawMidPos(featureID); }
	static float GetDrawRadius(int featureID) { return modelDrawerData->GetDrawRadius(featureID); }
	static float3 GetObjDrawMidPos(const CFeature* feature) { return modelDrawerData->GetObjDrawMidPos(feature); }
	static const CMatrix44f& GetUnsyncedTransformMatrix(const CFeature* feature) { return modelDrawerData->GetUnsyncedTransformMatrix(feature); }
	static const CMatrix44f& GetUnsyncedTransformMatrix(int featureID) { return modelDrawerData->GetUnsyncedTransformMatrix(featureID); }

	// drawer-owned distance-fade alpha (sim/draw §A drawAlpha eviction, PR 6)
	static float GetDrawAlpha(const CFeature* feature) { return modelDrawerData->GetDrawAlpha(feature); }

	// drawer-owned draw-visibility flags (sim/draw §A drawFlag eviction, PR 4)
	static uint8_t GetDrawFlag(const CFeature* feature) { return modelDrawerData->GetDrawFlag(feature); }
	static uint8_t GetDrawFlag(int featureID) { return modelDrawerData->GetDrawFlag(featureID); } // sim|draw PR 40 (GetVisibleFeatures noIcons)
	static uint8_t GetPreviousDrawFlag(const CFeature* feature) { return modelDrawerData->GetPreviousDrawFlag(feature); }
	static bool HasDrawFlag(const CFeature* feature, DrawFlags f) { return modelDrawerData->HasDrawFlag(feature, f); }

	static void ClearPreviousDrawFlags() { modelDrawerData->ClearPreviousDrawFlags(); }

	// SCOPE-1: drawer-owned per-feature render record (immutable header + sim-owned
	// mutable fields the draw-window passes read); never dereference live CFeature
	static const CFeatureDrawerData::FeatureRenderRecord& GetRenderRecord(const CFeature* feature) { return modelDrawerData->GetRenderRecord(feature); }
	static const CFeatureDrawerData::FeatureRenderRecord& GetRenderRecord(int featureID) { return modelDrawerData->GetRenderRecord(featureID); }
	// PR 43 (3b): barrier step 8 / valve hook (see CFeatureDrawerData)
	static void ClearDeadRetainedRecords() { if (modelDrawerData != nullptr) modelDrawerData->ClearDeadRetainedRecords(); }
	// PR 44a: producer-side transform extraction (sim thread, frame edge,
	// forced-serial -- see CModelDrawerDataBase::ExtractTransforms)
	static void ExtractTransformsAtSimEdge() { if (modelDrawerData != nullptr) modelDrawerData->ExtractTransformsAtSimEdge(); }

	// drawer-owned Lua material state + per-piece LOD display lists evicted from
	// LocalModel/LocalModelPiece (sim/draw PR 10)
	static LuaObjectMaterialData& GetLuaMaterialData(int featureID) { return modelDrawerData->GetLuaMaterialDataRef(featureID); }
	static std::vector<std::vector<uint32_t>>& GetLodDispLists(int featureID) { return modelDrawerData->GetLodDispListsRef(featureID); }
public:
	virtual void DrawFeatureModel(const CFeature* feature, bool noLuaCall) const = 0;
protected:
	static bool ShouldDrawOpaqueFeature(const CFeature* f, uint8_t thisPassMask);
	static bool ShouldDrawAlphaFeature(const CFeature* f, uint8_t thisPassMask);
	static bool ShouldDrawFeatureShadow(const CFeature* f);

	void PushIndividualState(const CFeature* feature, bool deferredPass) const;
	void PopIndividualState(const CFeature* feature, bool deferredPass) const;
};

class CFeatureDrawerBase : public CFeatureDrawer
{
public:
	void DrawOpaquePass(bool deferredPass, bool drawReflection, bool drawRefraction) const override {
		DrawOpaquePassImpl<LuaObjType::LUAOBJ_FEATURE>(deferredPass, drawReflection, drawRefraction);
	}
	void DrawAlphaPass(bool drawReflection, bool drawRefraction = false) const override {
		DrawAlphaPassImpl<LuaObjType::LUAOBJ_FEATURE>(drawReflection, drawRefraction);
	};
protected:
	void DrawOpaqueObjectsLua(bool deferredPass, bool drawReflection, bool drawRefraction) const override {
		eventHandler.DrawOpaqueFeaturesLua(deferredPass, drawReflection, drawRefraction);
	}
	void DrawAlphaObjectsLua(bool drawReflection, bool drawRefraction) const override {
		eventHandler.DrawAlphaFeaturesLua(drawReflection, drawRefraction);
	}
	void DrawShadowObjectsLua() const override {
		eventHandler.DrawShadowFeaturesLua();
	}

	void DrawOpaqueObjectsAux(int modelType) const override {} //no aux objects here
	void DrawAlphaObjectsAux(int modelType) const override {} //no aux objects here
	void Update() const override;
};

class CFeatureDrawerLegacy : public CFeatureDrawerBase
{
public:
	void Draw(bool drawReflection, bool drawRefraction) const override {
		DrawImpl<true, LuaObjType::LUAOBJ_FEATURE>(drawReflection, drawRefraction);
	}
	void DrawShadowPass() const override {
		DrawShadowPassImpl<true, LuaObjType::LUAOBJ_FEATURE>();
	}

	void DrawFeatureNoTrans(const CFeature* feature, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall) const override;
	void DrawFeatureTrans(const CFeature* feature, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall) const override;

	/// LuaOpenGL::Feature{Raw}: draw a single feature with full state setup
	void DrawIndividual(const CFeature* feature, bool noLuaCall) const override;
	void DrawIndividualNoTrans(const CFeature* feature, bool noLuaCall) const override;
protected:
	void DrawObjectsShadow(int modelType) const override;
	void DrawOpaqueObjects(int modelType, bool drawReflection, bool drawRefraction) const override;
	void DrawAlphaObjects(int modelType, bool drawReflection, bool drawRefraction) const override;

	void DrawOpaqueFeature(const CFeature* f, uint8_t thisPassMask) const override;
	void DrawAlphaFeature(const CFeature* f, uint8_t thisPassMask) const override;
	void DrawFeatureShadow(const CFeature* f) const;

	void DrawFeatureModel(const CFeature* feature, bool noLuaCall) const override;
};

class CFeatureDrawerGLSL final : public CFeatureDrawerLegacy {};

//TODO remove CFeatureDrawerLegacy inheritance
class CFeatureDrawerGL4 final: public CFeatureDrawerLegacy//CFeatureDrawerBase
{
public:
	void Draw(bool drawReflection, bool drawRefraction) const override {
		DrawImpl<false, LuaObjType::LUAOBJ_FEATURE>(drawReflection, drawRefraction);
	}
	void DrawShadowPass() const override {
		DrawShadowPassImpl<false, LuaObjType::LUAOBJ_FEATURE>();
	}
protected:
	void DrawObjectsShadow(int modelType) const override;

	void DrawOpaqueObjects(int modelType, bool drawReflection, bool drawRefraction) const override;
	void DrawAlphaObjects(int modelType, bool drawReflection, bool drawRefraction) const override;
};

#define featureDrawer (CFeatureDrawer::modelDrawer)