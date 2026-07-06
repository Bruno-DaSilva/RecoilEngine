#include "UnitDrawerData.h"

#include "System/MemPoolTypes.h"
#include "System/ContainerUtil.h"
#include "System/EventHandler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Config/ConfigHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/UI/MiniMap.h"
#include "Rendering/Common/ModelDrawerHelpers.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Models/IModelParser.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/IconHandler.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/Env/IWater.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"

#include "System/Misc/TracyDefs.h"

static FixedDynMemPoolT<MAX_UNITS / 1000, MAX_UNITS / 32, GhostSolidObject> ghostMemPool;

// id resolution for the drawer-side containers (see ModelDrawerData.h)
template<>
const CUnit* DrawerGetObjectByID<CUnit>(int id)
{
	const CUnit* unit = unitHandler.GetUnit(id);
	assert(unit != nullptr);
	return unit;
}

///////////////////////////

CR_BIND_POOL(GhostSolidObject, ,ghostMemPool.allocMem, ghostMemPool.freeMem)
CR_REG_METADATA(GhostSolidObject, (
	CR_MEMBER(modelName),

	CR_MEMBER(pos),
	CR_MEMBER(midPos),
	CR_MEMBER(dir),
	CR_MEMBER(radius),
	CR_MEMBER(iconRadius),

	CR_MEMBER(refCount),

	CR_MEMBER(facing),
	CR_MEMBER(team),
	CR_IGNORED(currentIconIndex),

	CR_IGNORED(model),

	CR_POSTLOAD(PostLoad)
))

CR_BIND(CUnitDrawerData::TempDrawUnit, )
CR_REG_METADATA(CUnitDrawerData::TempDrawUnit, (
	CR_MEMBER(unitDefId),

	CR_MEMBER(team),
	CR_MEMBER(facing),
	CR_MEMBER(timeout),

	CR_MEMBER(pos),
	CR_MEMBER(rotation),

	CR_MEMBER(drawAlpha),
	CR_MEMBER(drawBorder),

	CR_IGNORED(unitDef)
))

CR_BIND(CUnitDrawerData::SavedData, )
CR_REG_METADATA(CUnitDrawerData::SavedData, (
	CR_MEMBER(tempOpaqueUnits),
	CR_MEMBER(tempAlphaUnits),
	CR_MEMBER(deadGhostBuildings),
	CR_MEMBER(liveGhostBuildings)
))

///////////////////////////


void GhostSolidObject::PostLoad()
{
	RECOIL_DETAILED_TRACY_ZONE;
	model = nullptr;
	GetModel();
}

const S3DModel* GhostSolidObject::GetModel() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!model)
		model = modelLoader.LoadModel(modelName);

	return model;
}

const UnitDef* CUnitDrawerData::TempDrawUnit::GetUnitDef() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!unitDef)
		unitDef = unitDefHandler->GetUnitDefByID(unitDefId);

	return unitDef;
}

///////////////////////////

CUnitDrawerData::CUnitDrawerData(bool& mtModelDrawer_)
	: CUnitDrawerDataBase("[CUnitDrawerData]", 271828, mtModelDrawer_)
{
	RECOIL_DETAILED_TRACY_ZONE;
	//LuaObjectDrawer::ReadLODScales(LUAOBJ_UNIT);

	eventHandler.AddClient(this); //cannot be done in CModelRenderDataConcept, because object is not fully constructed

	SetUnitIconDist(static_cast<float>(configHandler->GetInt("UnitIconDist")));

	iconScale      = configHandler->GetFloat("UnitIconScaleUI");
	iconFadeStart  = configHandler->GetFloat("UnitIconFadeStart");
	iconFadeVanish = configHandler->GetFloat("UnitIconFadeVanish");
	useScreenIcons = configHandler->GetBool("UnitIconsAsUI");
	iconHideWithUI = configHandler->GetBool("UnitIconsHideWithUI");
	ghostIconDimming = configHandler->GetFloat("UnitGhostIconsDimming");

	configHandler->NotifyOnChange(this, {"UnitGhostIconsDimming"});

	unitDefImages.clear();
	unitDefImages.resize(unitDefHandler->NumUnitDefs() + 1);

	savedData.deadGhostBuildings.resize(teamHandler.ActiveAllyTeams());
	savedData.liveGhostBuildings.resize(teamHandler.ActiveAllyTeams());
}

CUnitDrawerData::~CUnitDrawerData()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// rendering is torn down before the sim, so the ids still resolve
	for (const int unitID : unsortedObjects) {
		groundDecals->ForceRemoveSolidObject(DrawerGetObjectByID<CUnit>(unitID));
	}

	for (UnitDefImage& img : unitDefImages) {
		img.Free();
	}

	for (int allyTeam = 0; allyTeam < savedData.deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
			auto& lgb = savedData.liveGhostBuildings[allyTeam][modelType];
			auto& dgb = savedData.deadGhostBuildings[allyTeam][modelType];

			for (auto& gso : dgb) {
				auto* tmpGso = std::exchange(gso, nullptr);
				if (tmpGso->DecRef())
					continue;

				// <ghost> might be the gbOwner of a decal; groundDecals is deleted after us
				groundDecals->GhostDestroyed(tmpGso);
				ghostMemPool.free(tmpGso);
			}
			dgb.clear();
			lgb.clear();
		}
	}
	assert(ghostMemPool.allocs() == 0);
	ghostMemPool.clear();

	configHandler->RemoveObserver(this);
}

void CUnitDrawerData::ConfigNotify(const std::string& key, const std::string& value)
{
	ghostIconDimming = configHandler->GetFloat("UnitGhostIconsDimming");
}

void CUnitDrawerData::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;

	// defined extraction point: snapshot piece/object transforms once per new sim frame
	ExtractTransforms();

	iconSizeBase = std::max(1.0f, std::max(globalRendering->viewSizeX, globalRendering->viewSizeY) * iconSizeMult * iconScale);

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
		UpdateTempDrawUnits(savedData.tempOpaqueUnits[modelType]);
		UpdateTempDrawUnits(savedData.tempAlphaUnits[modelType]);
	}

	const float3 camPos = (camHandler->GetCurrentController()).GetPos();
	const float3 camDir = (camHandler->GetCurrentController()).GetDir();
	float dist = CGround::LineGroundCol(camPos, camDir * 150000.0f, false);
	if (dist < 0)
		dist = std::max(0.0f, CGround::LinePlaneCol(camPos, camDir, 150000.0f, readMap->GetCurrAvgHeight()));

	iconZoomDist = dist;

	const auto updateBody = [this](const CUnit* u) {
		UpdateDrawPos(u);

		if (useScreenIcons)
			UpdateUnitIconStateScreen(u);
		else
			UpdateUnitIconState(u);

		UpdateCommon(u);
	};

	if (mtModelDrawer) {
		for_mt_chunk(0, unsortedObjects.size(), [this, &updateBody](const int k) {
			updateBody(DrawerGetObjectByID<CUnit>(unsortedObjects[k]));
		}, CModelDrawerDataConcept::MT_CHUNK_OR_MIN_CHUNK_SIZE_UPDT);
	}
	else {
		for (const int unitID : unsortedObjects)
			updateBody(DrawerGetObjectByID<CUnit>(unitID));
	}

	if ((useDistToGroundForIcons = (camHandler->GetCurrentController()).GetUseDistToGroundForIcons())) {
		const float3& camPos = camera->GetPos();
		// use the height at the current camera position
		//const float groundHeight = CGround::GetHeightAboveWater(camPos.x, camPos.z, false);
		// use the middle between the highest and lowest position on the map as average
		const float groundHeight = readMap->GetCurrAvgHeight();
		const float overGround = camPos.y - groundHeight;

		sqCamDistToGroundForIcons = overGround * overGround;
	}
}

void CUnitDrawerData::UpdateGhostedBuildings()
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (int allyTeam = 0; allyTeam < savedData.deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_CNT; modelType++) {
			auto& dgb = savedData.deadGhostBuildings[allyTeam][modelType];

			for (int i = 0; i < dgb.size(); /*no-op*/) {
				GhostSolidObject* gso = dgb[i];

				if (!losHandler->InLos(gso->pos, allyTeam)) {
					++i;
					continue;
				}

				// obtained LOS on the ghost of a dead building
				RemoveDeadGhost(gso, dgb, i); // swaps element with last so counter shouldn't be increased.
			}
		}
	}
}

void CUnitDrawerData::UpdateUnitIconsByUnitDef(const UnitDef* ud)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (int teamNum = 0; teamNum < teamHandler.ActiveTeams(); teamNum++) {
		for (auto* unit : unitHandler.GetUnitsByTeamAndDef(teamNum, ud->id)) {
			UpdateCurrentUnitIcon(unit);
		}
	}
}

void CUnitDrawerData::UpdateCurrentUnitIcon(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	constexpr unsigned short inLosOrRad = (LOS_INLOS | LOS_INRADAR);
	constexpr unsigned short prevMask = (LOS_PREVLOS | LOS_CONTRADAR);

	// use the unit's custom icon if we can currently see it,
	// or have seen it before and did not lose contact since
	bool unitVisible =
		(losStatus & inLosOrRad) != 0 &&
		(losStatus & prevMask) == prevMask ||
		gameSetup->ghostedBuildings && unit->leavesGhost && (losStatus & LOS_PREVLOS) != 0;

	const bool typedIcon = (unitVisible || gu->spectatingFullView);

	auto& iconState = IconStateRef(unit);

	if (typedIcon) {
		iconState.currentIconIndex =
			(iconState.customIconIndex != icon::INVALID_ICON_INDEX) ? iconState.customIconIndex : icon::iconHandler.GetIconIdxOrDefault(iconState.definedIconName);
	}
	else if ((losStatus & LOS_INRADAR) != 0) {
		iconState.currentIconIndex = icon::iconHandler.GetDefaultIconIdx();
	}
	else {
		iconState.currentIconIndex = icon::INVALID_ICON_INDEX;
	}
}

void CUnitDrawerData::UpdateUnitIconState(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	SetUnitIsIcon(unit, (losStatus & LOS_INRADAR) != 0);

	//further refinement if visible
	if ((losStatus & LOS_INLOS) != 0 || gu->spectatingFullView) {
		bool asIcon = true;

		asIcon &= (!unit->noDraw);
		asIcon &= (!unit->IsInVoid());

		asIcon &= DrawAsIconByDistance(unit, (unit->pos - camera->GetPos()).SqLength());
		// drawing icons is cheap but not free, avoid a perf-hit when many are offscreen
		asIcon &= (camera->InView(GetDrawMidPos(unit), unit->GetDrawRadius()));
		SetUnitIsIcon(unit, asIcon);
	}
}

void CUnitDrawerData::UpdateUnitIconStateScreen(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (game->hideInterface && iconHideWithUI) // icons are hidden with UI
	{
		SetUnitIsIcon(unit, false); // draw unit model always
		return;
	}

	if (GetUnitIconIndex(unit) == icon::INVALID_ICON_INDEX || unit->health <= 0 || unit->beingBuilt || unit->noDraw || unit->IsInVoid())
	{
		SetUnitIsIcon(unit, false);
		return;
	}

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	const auto& iconData = icon::iconHandler.GetIconData(GetUnitIconIndex(unit));

	float iconSizeMult = iconData.GetSize();
	if (iconData.GetRadiusAdjust())
		iconSizeMult *= (unit->radius / iconData.GetRadiusScale());
	iconSizeMult = iconSizeMult * 0.75f + 0.25f;

	float limit = iconSizeBase / 2 * iconSizeMult;

	// calculate unit's radius in screen space and compare with the size of the icon
	float3 pos = unit->pos;
	float3 radiusPos = pos + camera->right * unit->radius;

	pos = camera->CalcViewPortCoordinates(pos);
	radiusPos = camera->CalcViewPortCoordinates(radiusPos);

	SetUnitIconRadius(unit, unit->radius * ((limit * 0.9) / std::abs(pos.x - radiusPos.x))); // used for clicking on iconified units (world space!!!)

	if (!(losStatus & LOS_INLOS) && !gu->spectatingFullView) // no LOS on unit
	{
		SetUnitIsIcon(unit, losStatus & LOS_INRADAR); // draw icon if unit is on radar
		return;
	}

	// don't render unit's model if it is smaller than icon by 10% in screen space
	// render it anyway in case icon isn't completely opaque (below FadeStart distance)
	SetUnitIsIcon(unit, iconZoomDist / iconSizeMult > iconFadeStart && std::abs(pos.x - radiusPos.x) < limit * 0.9);
}

void CUnitDrawerData::UpdateDrawPos(const CUnit* u)
{
	RECOIL_DETAILED_TRACY_ZONE;

	auto& dp = drawPositions[u->id];

	if (const CUnit* t = u->GetTransporter(); t != nullptr) {
		dp.pos = u->GetDrawPosOther(t->preFrameTra.t, t->pos, globalRendering->timeOffset);
	}
	else {
		dp.pos = u->GetDrawPos(globalRendering->timeOffset);
	}

	dp.midPos = GetMdlDrawMidPos(u);
}

float3 CUnitDrawerData::GetObjDrawErrorPos(const CUnit* unit, int allyteam) const
{
	return (GetObjDrawMidPos(unit) + unit->GetErrorVector(allyteam));
}

CMatrix44f CUnitDrawerData::GetUnsyncedTransformMatrix(const CUnit* unit, bool fullread) const
{
	float3 interPos = GetDrawPos(unit);

	if (!fullread && !gu->spectatingFullView)
		interPos += unit->GetErrorVector(gu->myAllyTeam);

	return (unit->ComposeMatrix(interPos));
}

void CUnitDrawerData::UpdateObjectDrawFlags(const CSolidObject* o)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const CUnit* u = static_cast<const CUnit*>(o);

	{
		//icons flag is set before UpdateObjectDrawFlags() is called
		const bool isIcon = HasDrawFlag(u, DrawFlags::SO_DRICON_FLAG);
		ResetDrawFlag(u);
		SetUnitIsIcon(u, isIcon);
	}

	for (uint32_t camType = CCamera::CAMTYPE_PLAYER; camType < CCamera::CAMTYPE_ENVMAP; ++camType) {
		if (camType == CCamera::CAMTYPE_UWREFL && !IWater::GetWater()->CanDrawReflectionPass())
			continue;

		if (camType == CCamera::CAMTYPE_SHADOW && ((shadowHandler.shadowGenBits & CShadowHandler::SHADOWGEN_BIT_MODEL) == 0))
			continue;

		const CCamera* cam = CCameraHandler::GetCamera(camType);

		if (u->noDraw)
			continue;

		// unit will be drawn as icon instead
		if (GetUnitIsIcon(u))
			continue;

		if (u->IsInVoid())
			continue;

		if (!(u->losStatus[gu->myAllyTeam] & LOS_INLOS) && !gu->spectatingFullView)
			continue;

		if (!cam->InView(GetDrawMidPos(u), u->GetDrawRadius()))
			continue;

		switch (camType)
		{
			case CCamera::CAMTYPE_PLAYER: {
				const float sqrCamDist = (GetDrawPos(u) - cam->GetPos()).SqLength();

				if (!IsAlpha(u)) {
					SetDrawFlag(u, DrawFlags::SO_OPAQUE_FLAG);
				}
				else {
					SetDrawFlag(u, DrawFlags::SO_ALPHAF_FLAG);
				}

				if (u->IsInWater())
					AddDrawFlag(u, DrawFlags::SO_REFRAC_FLAG);
			} break;

			case CCamera::CAMTYPE_UWREFL: {
				if (CModelDrawerHelper::ObjectVisibleReflection(GetDrawMidPos(u), cam->GetPos(), u->GetDrawRadius()))
					AddDrawFlag(u, DrawFlags::SO_REFLEC_FLAG);
			} break;

			case CCamera::CAMTYPE_SHADOW: {
				if unlikely(IsAlpha(u))
					AddDrawFlag(u, DrawFlags::SO_SHTRAN_FLAG);
				else
					AddDrawFlag(u, DrawFlags::SO_SHOPAQ_FLAG);
			} break;

			default: { assert(false); } break;

		}

	}
}

bool CUnitDrawerData::DrawAsIconByDistance(const CUnit* unit, const float sqUnitCamDist) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto& sqIconDistMult = icon::iconHandler.GetIconData(GetUnitIconIndex(unit)).GetDistanceSq();
	const float realIconLength = iconLength * sqIconDistMult;

	if (useDistToGroundForIcons)
		return (sqCamDistToGroundForIcons > realIconLength);
	else
		return (sqUnitCamDist > realIconLength);
}

static inline bool LoadBuildPic(const std::string& filename, CBitmap& bitmap)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (CFileHandler::FileExists(filename, SPRING_VFS_RAW_FIRST)) {
		bitmap.Load(filename);
		return true;
	}

	return false;
}

void CUnitDrawerData::SetUnitDefImage(const UnitDef* unitDef, const std::string& texName)
{
	RECOIL_DETAILED_TRACY_ZONE;
	UnitDefImage*& unitImage = unitDef->buildPic;

	if (unitImage == nullptr) {
		unitImage = &unitDefImages[unitDef->id];
	}
	else {
		unitImage->Free();
	}

	CBitmap bitmap;

	if (!texName.empty()) {
		bitmap.Load("unitpics/" + texName);
	}
	else {
		if (!LoadBuildPic("unitpics/" + unitDef->name + ".dds", bitmap) &&
			!LoadBuildPic("unitpics/" + unitDef->name + ".png", bitmap) &&
			!LoadBuildPic("unitpics/" + unitDef->name + ".pcx", bitmap) &&
			!LoadBuildPic("unitpics/" + unitDef->name + ".bmp", bitmap)) {
			bitmap.AllocDummy(SColor(255, 0, 0, 255));
		}
	}

	unitImage->textureID = bitmap.CreateTexture(GL::TextureCreationParams{
		.aniso = 0.0f,
		.lodBias = 0.0f,
		.texID = 0,
		.reqNumLevels = 1,
		.linearMipMapFilter = false,
		.linearTextureFilter = true
	});

	unitImage->imageSizeX = bitmap.xsize;
	unitImage->imageSizeY = bitmap.ysize;
}

void CUnitDrawerData::SetUnitDefImage(const UnitDef* unitDef, unsigned int texID, int xsize, int ysize)
{
	RECOIL_DETAILED_TRACY_ZONE;
	UnitDefImage*& unitImage = unitDef->buildPic;

	if (unitImage == nullptr) {
		unitImage = &unitDefImages[unitDef->id];
	}
	else {
		unitImage->Free();
	}

	unitImage->textureID = texID;
	unitImage->imageSizeX = xsize;
	unitImage->imageSizeY = ysize;
}

uint32_t CUnitDrawerData::GetUnitDefImage(const UnitDef* unitDef)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (unitDef->buildPic == nullptr)
		SetUnitDefImage(unitDef, unitDef->buildPicName);

	return (unitDef->buildPic->textureID);
}

void CUnitDrawerData::AddTempDrawUnit(const TempDrawUnit& tdu)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const UnitDef* unitDef = tdu.GetUnitDef();
	const S3DModel* model = unitDef->LoadModel();

	if (tdu.drawAlpha) {
		savedData.tempAlphaUnits[model->type].push_back(tdu);
	}
	else {
		savedData.tempOpaqueUnits[model->type].push_back(tdu);
	}
}

void CUnitDrawerData::UpdateTempDrawUnits(std::vector<TempDrawUnit>& tempDrawUnits)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (unsigned int n = 0; n < tempDrawUnits.size(); /*no-op*/) {
		if (tempDrawUnits[n].timeout <= gs->frameNum) {
			// do not use spring::VectorErase; we already know the index
			tempDrawUnits[n] = tempDrawUnits.back();
			tempDrawUnits.pop_back();
			continue;
		}

		++n;
	}
}

const CUnitDrawerData::UnitIconState& CUnitDrawerData::GetIconState(const CUnit* u) const
{
	static const UnitIconState def = {};
	return (u->id < iconStates.size()) ? iconStates[u->id] : def;
}

CUnitDrawerData::UnitIconState& CUnitDrawerData::IconStateRef(const CUnit* u)
{
	if (u->id >= iconStates.size())
		iconStates.resize(u->id + 1);

	return iconStates[u->id];
}

void CUnitDrawerData::RenderUnitPreCreated(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	UpdateObject(unit, true);

	// fresh slot for a fresh unit (the id may be recycled); definedIconName
	// freezes the unitDef's icon at registration time, replacing the old
	// creation-time CUnit::UpdateRenderParams copy
	auto& iconState = IconStateRef(unit);
	iconState = {};
	iconState.definedIconName = unit->unitDef->iconName;
}

void CUnitDrawerData::RenderUnitCreated(const CUnit* unit, int cloaked)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(std::find(unsortedObjects.begin(), unsortedObjects.end(), unit->id) != unsortedObjects.end());
	UpdateCurrentUnitIcon(unit);
}

S3DModel* CUnitDrawerData::GetUnitModel(const CUnit* unit) const
{
	const UnitDef* unitDef = unit->unitDef;
	const UnitDef* decoyDef = unitDef->decoyDef;

	// FIXME -- adjust decals for decoys? gets weird?
	S3DModel* gsoModel = (decoyDef == nullptr) ? unit->model : decoyDef->LoadModel();
	return gsoModel;
}

// which allyteams get a dead ghost when this unit dies / stops leaving
// ghosts; computed at event time because sim owns (and clears) the
// LOS_PREVLOS bits this reads
GhostAllyMask CUnitDrawerData::CalcDeadGhostAllyMask(const CUnit* unit) const
{
	GhostAllyMask mask = {};

	if (!gameSetup->ghostedBuildings)
		return mask;

	for (int allyTeam = 0; allyTeam < savedData.deadGhostBuildings.size(); ++allyTeam) {
		const auto ls = unit->losStatus[allyTeam];

		if (!(ls & (LOS_INLOS | LOS_CONTRADAR | LOS_INRADAR)) && (ls & LOS_PREVLOS))
			mask[allyTeam / 64] |= (uint64_t(1) << (allyTeam % 64));
	}

	return mask;
}

bool CUnitDrawerData::UpdateUnitGhosts(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask)
{
	if (!gameSetup->ghostedBuildings)
		return false;

	bool addedOwnAllyTeam = false;

	// TODO - make ghosted buildings per allyTeam - so they are correctly dealt with
	// when spectating
	GhostSolidObject* gso = nullptr;
	S3DModel* gsoModel = GetUnitModel(unit);

	for (int allyTeam = 0; allyTeam < savedData.deadGhostBuildings.size(); ++allyTeam) {
		const bool canSeeGhost = (deadGhostAllyMask[allyTeam / 64] & (uint64_t(1) << (allyTeam % 64))) != 0;

		if (canSeeGhost) {
			if (gso == nullptr) {
				gso = ghostMemPool.alloc<GhostSolidObject>();

				gso->pos = unit->pos;
				gso->midPos = unit->midPos;
				gso->modelName = gsoModel->name;
				gso->refCount = 0;
				gso->facing = unit->buildFacing;
				gso->dir = unit->frontdir;
				gso->team = unit->team;
				gso->radius = unit->radius;
				gso->GetModel();

				// gso is a shared object, we can't rely on the unit's currentIconIndex being representative in case the team changes
				gso->currentIconIndex = icon::iconHandler.GetIconIdxOrDefault(GetIconState(unit).definedIconName);

				gso->iconRadius = GetUnitIconRadius(unit);

				groundDecals->GhostCreated(unit, gso);

			}

			// <gso> can be inserted for multiple allyteams
			// (the ref-counter saves us come deletion time)
			savedData.deadGhostBuildings[allyTeam][gsoModel->type].push_back(gso);
			gso->IncRef();

			if (allyTeam == gu->myAllyTeam)
				addedOwnAllyTeam = true;

		}

		spring::VectorErase(savedData.liveGhostBuildings[allyTeam][MDL_TYPE(unit)], unit->id);
	}
	return addedOwnAllyTeam;
}

void CUnitDrawerData::RenderUnitDestroyed(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// dispatched at the boundary drain against the unit's deferred shell
	// (PR 13); losStatus froze at death -- sim detached the unit from all
	// LOS updating before parking it -- so the mask still reads the values
	// the old synchronous dispatch saw
	UpdateUnitGhosts(unit, unit->leavesGhost ? CalcDeadGhostAllyMask(unit) : GhostAllyMask{});

	// container invariant (PR 14): a dead unit's id must leave the live-ghost
	// lists even when UpdateUnitGhosts early-returned (ghostedBuildings off,
	// but Lua can set leavesGhost regardless -- master left a stale pointer
	// here that only mempool zeroing kept benign)
	if (!gameSetup->ghostedBuildings && unit->model != nullptr) {
		for (int allyTeam = 0; allyTeam < savedData.liveGhostBuildings.size(); ++allyTeam) {
			spring::VectorErase(savedData.liveGhostBuildings[allyTeam][MDL_TYPE(unit)], unit->id);
		}
	}

	// must happen after UpdateUnitGhosts()
	IconStateRef(unit).currentIconIndex = icon::INVALID_ICON_INDEX;

	DelObject(unit, true);	

	LuaObjectDrawer::SetObjectLOD(unit, LUAOBJ_UNIT, 0);
}

void CUnitDrawerData::UnitEnteredRadar(const CUnit* unit, int allyTeam)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (allyTeam != gu->myAllyTeam)
		return;

	renderEventQueue.UnitEnteredRadar(unit, allyTeam);
}

void CUnitDrawerData::UnitLeftRadar(const CUnit* unit, int allyTeam)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (allyTeam != gu->myAllyTeam)
		return;

	renderEventQueue.UnitLeftRadar(unit, allyTeam);
}

void CUnitDrawerData::ApplyUnitRadarChanged(const CUnit* unit, int allyTeam)
{
	// re-check: gu->myAllyTeam may have changed since the record was queued;
	// PlayerChanged refreshes every icon on such switches
	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateCurrentUnitIcon(unit);
}

void CUnitDrawerData::UnitEnteredLos(const CUnit* unit, int allyTeam)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// master's no-op set, evaluated at event time like master does
	if (allyTeam != gu->myAllyTeam && !unit->leavesGhost)
		return;

	renderEventQueue.UnitEnteredLos(unit, allyTeam, unit->leavesGhost);
}

void CUnitDrawerData::ApplyUnitEnteredLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent)
{
	// use the event-time leavesGhost value: interleaved UnitLeavesGhostChanged
	// records replay any later flips in order, so the live-ghost container ops
	// mirror master's op-for-op
	if (leavesGhostAtEvent)
		spring::VectorErase(savedData.liveGhostBuildings[allyTeam][MDL_TYPE(unit)], unit->id);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateCurrentUnitIcon(unit);
}

void CUnitDrawerData::UnitLeftLos(const CUnit* unit, int allyTeam)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (allyTeam != gu->myAllyTeam && !unit->leavesGhost)
		return;

	renderEventQueue.UnitLeftLos(unit, allyTeam, unit->leavesGhost);
}

void CUnitDrawerData::ApplyUnitLeftLos(const CUnit* unit, int allyTeam, bool leavesGhostAtEvent)
{
	if (leavesGhostAtEvent)
		spring::VectorInsertUnique(savedData.liveGhostBuildings[allyTeam][MDL_TYPE(unit)], unit->id, true);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateCurrentUnitIcon(unit);
}

void CUnitDrawerData::UnitLeavesGhostChanged(const CUnit* unit, const bool leaveDeadGhost)
{
	// LOS_PREVLOS accounting for the leavesGhost=true transition is synced
	// state and handled by CUnit::SetLeavesGhost before this notification
	if (unit->leavesGhost)
		return;

	// capture the dead-ghost decision now: CUnit::SetLeavesGhost clears the
	// LOS_PREVLOS bits it derives from right after this notification returns
	renderEventQueue.UnitLeavesGhostChanged(unit, leaveDeadGhost ? CalcDeadGhostAllyMask(unit) : GhostAllyMask{});
}

void CUnitDrawerData::ApplyUnitLeavesGhostChanged(const CUnit* unit, const GhostAllyMask& deadGhostAllyMask)
{
	if (UpdateUnitGhosts(unit, deadGhostAllyMask)) {
		// left decoy dead ghost for own team
		UpdateCurrentUnitIcon(unit);
	}
}

void CUnitDrawerData::PlayerChanged(int playerID)
{
	for (const int unitID : unsortedObjects) {
		UpdateCurrentUnitIcon(DrawerGetObjectByID<CUnit>(unitID));
	}
}

void CUnitDrawerData::RemoveDeadGhost(GhostSolidObject* gso, std::vector<GhostSolidObject*>& dgb, int index)
{
	if (!gso->DecRef()) {
		groundDecals->GhostDestroyed(gso);
		ghostMemPool.free(gso);
	}

	dgb[index] = dgb.back();
	dgb.pop_back();
}