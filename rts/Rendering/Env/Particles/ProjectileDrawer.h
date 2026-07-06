/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <memory>

#include "Sim/Projectiles/Projectile.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GL/FBO.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Models/ModelRenderContainer.h"
#include "Rendering/DepthBufferCopy.h"
#include "System/EventClient.h"
#include "System/UnorderedSet.hpp"

class CSolidObject;
class CTextureAtlas;
struct AtlasedTexture;
class CGroundFlash;
struct FlyingPiece;
class LuaTable;

// packed projectile handle for the drawer containers (PR 14): the id plus
// the namespace bit, matching ModelRenderContainerTraits<CProjectile>
inline uint32_t ModelRenderContainerTraits<CProjectile>::ToHandle(const CProjectile* o)
{
	return (uint32_t(o->id) << 1) | uint32_t(o->synced);
}


class CProjectileDrawer: public CEventClient {
public:
	CProjectileDrawer(): CEventClient("[CProjectileDrawer]", 123456, false), perlinFB(true) {}

	static void InitStatic();
	static void KillStatic(bool reload);

	void Init();
	void Kill();

	void UpdateDrawFlags();

	void DrawOpaque(bool drawReflection, bool drawRefraction = false);
	void DrawAlpha(bool drawAboveWater, bool drawBelowWater, bool drawReflection, bool drawRefraction);

	void DrawProjectilesMiniMap();

	void DrawGroundFlashes();

	void DrawShadowOpaque();
	void DrawShadowTransparent();

	void LoadWeaponTextures();
	void UpdateTextures();


	bool WantsEvent(const std::string& eventName) {
		return
			(eventName == "RenderProjectileCreated") ||
			(eventName == "RenderProjectileDestroyed");
	}
	bool GetFullRead() const { return true; }
	int GetReadAllyTeam() const { return AllAccessTeam; }

	void RenderProjectileCreated(const CProjectile* projectile);
	void RenderProjectileDestroyed(const CProjectile* projectile);

	// drawer-owned interpolated draw position, keyed by id per namespace
	// (sim/draw §A drawPos eviction; was a CProjectile field; PR 14 rekeyed
	// renderIndex -> [synced][id]). Zero for projectiles not (yet)
	// registered, as the old member default was.
	const float3& GetDrawPos(const CProjectile* p) const {
		static const float3 zero;
		const auto& v = drawPositions[p->synced];
		return (size_t(p->id) < v.size()) ? v[p->id] : zero;
	}
	// draw-time transform (was CProjectile::GetTransformMatrix, "UNSYNCED ONLY")
	CMatrix44f GetTransformMatrix(const CProjectile* p, bool offsetPos) const;

	// drawer-owned draw-visibility flags, keyed by id per namespace (sim/draw §A
	// drawFlag eviction; was a CProjectile field). SO_NODRAW_FLAG for projectiles not
	// (yet) registered, as the old member default was. (previousDrawFlag was dropped:
	// it was a per-frame dead store for projectiles — no reader ever consumed it, the
	// GetRenderObjectsDrawFlagChanged consumer only queries units/features.)
	// PR 27b: see the splitResolveCache member
	void BuildSplitResolveCache();
	bool SplitResolveCacheBuilt() const { return splitResolveCacheBuilt; }
	const CProjectile* ResolveSplitCachedProjectile(int id, bool synced) const {
		const auto& v = splitResolveCache[synced];
		return (size_t(id) < v.size()) ? v[id] : nullptr;
	}

	uint8_t GetDrawFlag(const CProjectile* p) const {
		const auto& v = drawFlags[p->synced];
		return (size_t(p->id) < v.size()) ? v[p->id] : DrawFlags::SO_NODRAW_FLAG;
	}
	bool HasDrawFlag(const CProjectile* p, DrawFlags f) const { return (GetDrawFlag(p) & f) == f; }

	// drawer-owned per-camera z-sort keys, keyed by id per namespace (sim/draw §A
	// sortDist eviction; was a CProjectile field). Written by UpdateDrawFlags for the
	// cameras a projectile is in view of (stale slots keep their last value, as the
	// old member did); zero for projectiles not (yet) registered, as the old member
	// default was. Includes the sim-authored p->sortDistOffset, like the old setter.
	float GetSortDist(const CProjectile* p, uint32_t camType) const {
		const auto& v = sortDists[p->synced];
		return (size_t(p->id) < v.size()) ? v[p->id][camType] : 0.0f;
	}

	unsigned int NumSmokeTextures() const { return (smokeTextures.size()); }

	void IncPerlinTexObjectCount() { perlinTexObjects++; }
	void DecPerlinTexObjectCount() { perlinTexObjects--; }

	bool EnableSorting(bool b) { return (drawSorted =           b); }
	bool ToggleSorting(      ) { return (drawSorted = !drawSorted); }

	static bool CheckSoftenExt();
	bool CanDrawSoften() {
		return
			CheckSoftenExt() &&
			fxShader && fxShader->IsValid() &&
			depthBufferCopy->IsValid(false);
	};

	int EnableSoften(int b) { return CanDrawSoften() ? (wantSoften = std::clamp(b, 0, WANT_SOFTEN_COUNT - 1)) : 0; }
	int ToggleSoften() { return EnableSoften((wantSoften + 1) % WANT_SOFTEN_COUNT); }

	int EnableDrawOrder(int b) { return wantDrawOrder = b; }
	int ToggleDrawOrder() { return EnableDrawOrder((wantDrawOrder + 1) % 2); }

	const AtlasedTexture* GetSmokeTexture(unsigned int i) const { return smokeTextures[i]; }

	CTextureAtlas* textureAtlas = nullptr;  ///< texture atlas for projectiles
	CTextureAtlas* groundFXAtlas = nullptr; ///< texture atlas for ground fx

	// texture-coordinates for projectiles
	AtlasedTexture* flaretex = nullptr;
	AtlasedTexture* dguntex = nullptr;            ///< dgun texture
	AtlasedTexture* flareprojectiletex = nullptr; ///< texture used by flares that trick missiles
	AtlasedTexture* sbtrailtex = nullptr;         ///< default first section of starburst missile trail texture
	AtlasedTexture* missiletrailtex = nullptr;    ///< default first section of missile trail texture
	AtlasedTexture* muzzleflametex = nullptr;     ///< default muzzle flame texture
	AtlasedTexture* repulsetex = nullptr;         ///< texture of impact on repulsor
	AtlasedTexture* sbflaretex = nullptr;         ///< default starburst  missile flare texture
	AtlasedTexture* missileflaretex = nullptr;    ///< default missile flare texture
	AtlasedTexture* beamlaserflaretex = nullptr;  ///< default beam laser flare texture
	AtlasedTexture* explotex = nullptr;
	AtlasedTexture* explofadetex = nullptr;
	AtlasedTexture* heatcloudtex = nullptr;
	AtlasedTexture* circularthingytex = nullptr;
	AtlasedTexture* bubbletex = nullptr;          ///< torpedo trail texture
	AtlasedTexture* geosquaretex = nullptr;       ///< unknown use
	AtlasedTexture* gfxtex = nullptr;             ///< nanospray texture
	AtlasedTexture* projectiletex = nullptr;      ///< appears to be unused
	AtlasedTexture* repulsegfxtex = nullptr;      ///< used by repulsor
	AtlasedTexture* sphereparttex = nullptr;      ///< sphere explosion texture
	AtlasedTexture* torpedotex = nullptr;         ///< appears in-game as a 1 texel texture
	AtlasedTexture* wrecktex = nullptr;           ///< smoking explosion part texture
	AtlasedTexture* plasmatex = nullptr;          ///< default plasma texture
	AtlasedTexture* laserendtex = nullptr;
	AtlasedTexture* laserfallofftex = nullptr;
	AtlasedTexture* randdotstex = nullptr;
	AtlasedTexture* smoketrailtex = nullptr;
	AtlasedTexture* waketex = nullptr;
	AtlasedTexture* perlintex = nullptr;
	AtlasedTexture* flametex = nullptr;

	AtlasedTexture* groundflashtex = nullptr;
	AtlasedTexture* groundringtex = nullptr;

	AtlasedTexture* seismictex = nullptr;
public:
	static bool CanDrawProjectile(const CProjectile* pro, int allyTeam);
	// non-static: reads the drawer-owned drawFlags storage (PR 4)
	bool ShouldDrawProjectile(const CProjectile* pro, uint8_t thisPassMask) const;

	static TypedRenderBuffer<VA_TYPE_C>& GetMiniMapLinesRB();
	static TypedRenderBuffer<VA_TYPE_C>& GetMiniMapPointsRB();
private:
	static void ParseAtlasTextures(const bool, const LuaTable&, spring::unordered_set<std::string>&, CTextureAtlas*);

	void DrawProjectiles(int modelType, bool drawReflection, bool drawRefraction);
	void DrawProjectilesShadow(int modelType);
	void DrawFlyingPieces(int modelType) const;

	static void DrawProjectileModel(const CProjectile* projectile);

	void UpdatePerlin();
	static void GenerateNoiseTex(unsigned int tex);

private:
	static constexpr int perlinBlendTexSize = 16;
	static constexpr int perlinTexSize = 128;

	// start edge fading of regular CEGs if height difference is less than [0]
	// fade out groundflashes to 0 as height difference reaches [1]
	static constexpr float softenThreshold[2] = { 8.0f, 350.0f };
	static constexpr float softenExponent[2]  = { 0.6f, 8.0f };

	GLuint perlinBlendTex[8];
	float perlinBlend[4];

	int perlinTexObjects = 0;
	bool drawPerlinTex = false;

	FBO perlinFB;

	std::vector<const AtlasedTexture*> smokeTextures;

	/// interpolated draw positions, keyed [synced][id] (see GetDrawPos)
	std::array<std::vector<float3>, 2> drawPositions;

	/// draw-visibility flags, keyed [synced][id] (see GetDrawFlag)
	std::array<std::vector<uint8_t>, 2> drawFlags;

	/// per-camera z-sort keys, keyed [synced][id] (see GetSortDist)
	std::array<std::vector<std::array<float, 3>>, 2> sortDists;

	/// registered projectiles: packed (id << 1 | synced) handles in
	/// registration order -- the drawer's persistent iteration set, mutated
	/// only by the render events (PR 14: no object pointers)
	std::vector<uint32_t> renderHandles;

	// PR 27b: boundary-built handle->object resolution cache, keyed
	// [synced][id] like renderIndices (see ResolveProjectileHandle: with the
	// split running, post-release passes may not resolve through the
	// sim-owned FreeListMapCompact containers). Built by
	// BuildSplitResolveCache from the SimDrawBarrier / valve service.
	std::array<std::vector<const CProjectile*>, 2> splitResolveCache;
	bool splitResolveCacheBuilt = false;

	/// position of a handle in renderHandles, keyed [synced][id]; -1u when
	/// not registered (replaces the old CProjectile::renderIndex backref)
	std::array<std::vector<uint32_t>, 2> renderIndices;

	/// per-draw-frame pointer resolution of renderHandles, parallel to it;
	/// rebuilt by UpdateDrawFlags right after the boundary drain and only
	/// valid for the draw passes of the same frame (never crosses a sim
	/// boundary: every id resolves to a live object, asserted at rebuild)
	std::vector<const CProjectile*> renderProjectiles;

	/// projectiles with a model, binned by model type and textures
	std::array<ModelRenderContainer<CProjectile>, MODELTYPE_CNT> modelRenderers;

	/// used to render particle effects in back-to-front order. {unsorted, sorted}
	std::array<std::vector<const CProjectile*>, 2> drawParticles;

	bool drawSorted = true;

	Shader::IProgramObject* fxShader = nullptr;
	Shader::IProgramObject* fxShadowShader = nullptr;

	constexpr static int WANT_SOFTEN_COUNT = 2;
	int wantSoften = 0;

	bool wantDrawOrder = true;

	std::unique_ptr<ScopedDepthBufferCopy> sdbc;

	// Instance members to ensure proper cleanup during Kill() before OpenGL context is destroyed
	TypedRenderBuffer<VA_TYPE_C> minimapLinesRB{ 1 << 12, 0 };
	TypedRenderBuffer<VA_TYPE_C> minimapPointsRB{ 1 << 14, 0 };
};

extern CProjectileDrawer* projectileDrawer;