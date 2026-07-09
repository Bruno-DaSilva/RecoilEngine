/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/myGL.h"

#include <Rml/Backends/RmlUi_Backend.h>
#include <RmlUi/Core.h>

#include <array>    // §4.5 pause-surface telemetry counters
#include <atomic>
#include <cstdint>
#include "Game.h"
#include "BoundaryStats.h"
#include "Camera.h"
#include "CameraHandler.h"
#include "ChatMessage.h"
#include "CommandMessage.h"
#include "ConsoleHistory.h"
#include "GameHelper.h"
#include "GameSetup.h"
#include "GlobalUnsynced.h"
#include "LoadScreen.h"
#include "SelectedUnitsHandler.h"
#include "WaitCommandsAI.h"
#include "WordCompletion.h"
#include "IVideoCapturing.h"
#include "InMapDraw.h"
#include "InMapDrawModel.h"
#include "SyncedActionExecutor.h"
#include "SyncedGameCommands.h"
#include "UnsyncedActionExecutor.h"
#include "UnsyncedGameCommands.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/UI/PlayerRoster.h"
#include "Game/UI/PlayerRosterDrawer.h"
#include "Game/UI/UnitTracker.h"
#include "ExternalAI/AILibraryManager.h"
#include "ExternalAI/EngineOutHandler.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Rendering/WorldDrawer.h"
#include "Rendering/Common/RenderEventQueue.h"
#include "Rendering/Common/DrawMapMirrors.h"
#include "Rendering/Common/SimSnapshot.h"
#include "Rendering/Common/SnapshotPickGrid.h"
#include "Rendering/Common/SnapshotHash.h"
#include "Rendering/Common/SnapshotDiffGate.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/Env/WaterRendering.h"
#include "Rendering/Env/MapRendering.h"
#include "Rendering/Fonts/CFontTexture.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/CommandDrawer.h"
#include "Rendering/LineDrawer.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/DebugDrawerAI.h"
#include "Rendering/HUDDrawer.h"
#include "Rendering/IconHandler.h"
#include "Rendering/ModelsDataUploader.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/TeamHighlight.h"
#include "Map/BaseGroundDrawer.h"
#include "Rendering/Common/ModelDrawer.h"
#include "Rendering/Models/IModelParser.h"
#include "Rendering/GL/LightHandler.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/UniformConstants.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/Textures/NamedTextures.h"
#include "Lua/LuaGaia.h"
#include "Lua/LuaHandle.h"
#include "Lua/LuaInputReceiver.h"
#include "Lua/LuaMenu.h"
#include "Lua/LuaRules.h"
#include "Lua/LuaOpenGL.h"
#include "Lua/LuaSnapshotServe.h"
#include "Lua/LuaSplitContract.h"
#include "System/SimDrawSplit.h"
#include "System/UnsyncedBoundaryQueue.h"
#include "Lua/LuaParser.h"
#include "Lua/LuaSyncedRead.h"
#include "Lua/LuaUI.h"
#include "Map/MapDamage.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Net/GameServer.h"
#include "Net/Protocol/NetProtocol.h"
#include "Sim/Ecs/Registry.h"
#include "Sim/Ecs/Helper.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/CategoryHandler.h"
#include "Sim/Objects/DeferredObjectDeleter.h"
#include "Sim/Misc/DamageArrayHandler.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/BuildingMaskMap.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/InterceptHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/SideParser.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/Wind.h"
#include "Sim/Misc/ResourceHandler.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveTypeFactory.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/Scripts/UnitScriptFactory.h"
#include "Sim/Units/Scripts/UnitScriptEngine.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Sim/Weapons/WeaponLoader.h"
#include "UI/CommandColors.h"
#include "UI/EndGameBox.h"
#include "UI/GameSetupDrawer.h"
#include "UI/GuiHandler.h"
#include "UI/InfoConsole.h"
#include "UI/KeyBindings.h"
#include "UI/MiniMap.h"
#include "UI/MouseHandler.h"
#include "UI/ResourceBar.h"
#include "UI/SelectionKeyHandler.h"
#include "UI/TooltipConsole.h"
#include "UI/ProfileDrawer.h"
#include "UI/Groups/GroupHandler.h"
#include "System/Config/ConfigHandler.h"
#include "System/creg/SerializeLuaState.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Sync/FPUCheck.h"
#include "System/SafeUtil.h"
#include "System/SpringExitCode.h"
#include "System/SpringMath.h"
#include "System/FileSystem/FileSystem.h"
#include "System/LoadSave/LoadSaveHandler.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/Log/ILog.h"
#include "System/Platform/Misc.h"
#include "lib/streflop/streflop_cond.h"
#include "System/Platform/Watchdog.h"
#include "System/Threading/SpringThreading.h"
#include "System/Platform/errorhandler.h"
#include "System/Sound/ISound.h"
#include "System/Sound/ISoundChannels.h"
#include "System/Sync/DumpState.h"
#include "System/TimeProfiler.h"
#include "System/LoadLock.h"

#include "System/Misc/TracyDefs.h"

#include "fmt/ranges.h"


#undef CreateDirectory

CONFIG(bool, GameEndOnConnectionLoss).defaultValue(true);
// CONFIG(bool, LuaCollectGarbageOnSimFrame).defaultValue(true);

CONFIG(bool, ShowFPS).defaultValue(false).description("Displays current framerate.");
CONFIG(bool, ShowClock).defaultValue(true).headlessValue(false).description("Displays a clock on the top-right corner of the screen showing the elapsed time of the current game.");
CONFIG(bool, ShowSpeed).defaultValue(false).description("Displays current game speed.");
CONFIG(int, ShowPlayerInfo).defaultValue(1).headlessValue(0);
CONFIG(float, GuiOpacity).defaultValue(0.8f).minimumValue(0.0f).maximumValue(1.0f).description("Sets the opacity of the built-in Spring UI. Generally has no effect on LuaUI widgets. Can be set in-game using shift+, to decrease and shift+. to increase.");
CONFIG(std::string, InputTextGeo).defaultValue("");

CONFIG(int, SmoothTimeOffset).defaultValue(0).headlessValue(0).description("Enables frametimeoffset smoothing, 0 = off (old version), -1 = forced 0.5,  1-20 smooth, recommended = 2-3");
CONFIG(int, SplitWindowShrink).defaultValue(1).description("PR 42 (SimDrawSplit only): 1 = defer the mirror-fed info textures + widget Update callins past the sim-pause release so they run with the sim live (shrinks the parked window); 0 = keep the whole UI phase parked (pre-42 behavior). For A/B measurement.");

CGame* game = nullptr;


CR_BIND(CGame, (std::string(""), std::string(""), nullptr))

CR_REG_METADATA(CGame, (
	CR_MEMBER(lastSimFrame),
	CR_IGNORED(lastNumQueuedSimFrames),
	CR_IGNORED(numDrawFrames),

	CR_IGNORED(frameStartTime),
	CR_IGNORED(lastSimFrameTime),
	CR_IGNORED(lastDrawFrameTime),
	CR_IGNORED(lastFrameTime),
	CR_IGNORED(lastReadNetTime),
	CR_IGNORED(lastNetPacketProcessTime),
	CR_IGNORED(lastReceivedNetPacketTime),
	CR_IGNORED(lastSimFrameNetPacketTime),
	CR_IGNORED(lastUnsyncedUpdateTime),
	CR_IGNORED(skipLastDrawTime),

	CR_IGNORED(updateDeltaSeconds),
	CR_MEMBER(totalGameTime),

	CR_IGNORED(chatSound),
	CR_MEMBER(hideInterface),

	// FIXME: atomic type deduction
	CR_IGNORED(loadDone),
	CR_IGNORED(gameOver),

	CR_IGNORED(gameDrawMode),
	CR_MEMBER(showFPS),
	CR_MEMBER(showClock),
	CR_MEMBER(showSpeed),

	CR_IGNORED(playerTraffic),
	CR_MEMBER(noSpectatorChat),
	CR_MEMBER(gameID),

	CR_IGNORED(skipping),
	CR_MEMBER(playing),
	CR_IGNORED(paused),

	CR_IGNORED(msgProcTimeLeft),
	CR_IGNORED(consumeSpeedMult),

/*
	CR_IGNORED(skipStartFrame),
	CR_IGNORED(skipEndFrame),
	CR_IGNORED(skipTotalFrames),
	CR_IGNORED(skipSeconds),
	CR_IGNORED(skipSoundmute),
	CR_IGNORED(skipOldSpeed),
	CR_IGNORED(skipOldUserSpeed),
*/

	CR_MEMBER(speedControl),
	CR_MEMBER(luaGCControl),

	CR_IGNORED(jobDispatcher),
	CR_IGNORED(worldDrawer),
	CR_IGNORED(saveFileHandler),
	CR_IGNORED(gameInputReceiver),

	// Post Load
	CR_POSTLOAD(PostLoad)
))



CGame::CGame(const std::string& mapFileName, const std::string& modFileName, ILoadSaveHandler* saveFile)
	: frameStartTime(spring_gettime())
	, lastSimFrameTime(spring_gettime())
	, lastDrawFrameTime(spring_gettime())
	, lastFrameTime(spring_gettime())
	, lastReadNetTime(spring_gettime())
	, lastNetPacketProcessTime(spring_gettime())
	, lastReceivedNetPacketTime(spring_gettime())
	, lastSimFrameNetPacketTime(spring_gettime())
	, lastUnsyncedUpdateTime(spring_gettime())
	, skipLastDrawTime(spring_gettime())

	, saveFileHandler(saveFile)
{
	game = this;

	memset(gameID, 0, sizeof(gameID));

	// set "Headless" in config overlay (not persisted)
	configHandler->Set("Headless", (SpringVersion::IsHeadless()) ? 1 : 0, true);

	// cache the sim|draw split flag for this game session (PR 27b)
	SimDrawSplit::UpdateConfig();

	showFPS   = configHandler->GetBool("ShowFPS");
	showClock = configHandler->GetBool("ShowClock");
	showSpeed = configHandler->GetBool("ShowSpeed");

	speedControl = configHandler->GetInt("SpeedControl");

	playerRoster.SetSortTypeByCode((PlayerRoster::SortType)configHandler->GetInt("ShowPlayerInfo"));

	CInputReceiver::guiAlpha = configHandler->GetFloat("GuiOpacity");

	ParseInputTextGeometry("default");
	ParseInputTextGeometry(configHandler->GetString("InputTextGeo"));

	// clear left-over receivers in case we reloaded
	gameCommandConsole.ResetState();

	envResHandler.ResetState();

	modInfo.Init(modFileName);

	// needed for LuaIntro (pushes LuaConstGame)
	assert(mapInfo == nullptr);
	mapInfo = new CMapInfo(mapFileName, gameSetup->mapName);

	if (!sideParser.Load())
		throw content_error(sideParser.GetErrorLog());


	// after this, other components are able to register chat action-executors
	SyncedGameCommands::CreateInstance();
	UnsyncedGameCommands::CreateInstance();

	// note: makes no sense to create this unless we have AI's
	// (events will just go into the void otherwise) but it is
	// unconditionally deref'ed in too many places
	CEngineOutHandler::Create();

	CResourceHandler::CreateInstance();
	CCategoryHandler::CreateInstance();
}

CGame::~CGame()
{
	// PR 27b: the sim thread must be gone before any teardown below touches
	// state it consumes (handlers, net, Lua); no-op when the split is off
	JoinSimThread();

	ENTER_SYNCED_CODE();
	LOG("[Game::%s][1]", __func__);

	// write out a partial /boundarydump if the game ends before its end frame
	BoundaryStats::FlushPartial();
	// flush the /snaphashdump file; runs on every rewind reload too, and the
	// dump deliberately survives the reload so forward + resim passes accumulate
	SnapshotHash::FlushPartial();

	// report the snapshot differential gate totals if it was left armed
	snapshotDiffGate.FlushPartial();

	// §4.5: dump the mid-gameplay sim-park survey (which pause sites engaged)
	DumpSimPauseSurvey();

	RmlGui::Shutdown();
	helper->Kill();
	KillLua(true);
	KillMisc();
	KillRendering();
	KillInterface();
	KillSimulation();

	LOG("[Game::%s][2]", __func__);
	spring::SafeDelete(saveFileHandler); // ILoadSaveHandler, depends on vfsHandler via ~IArchive

	LOG("[Game::%s][3]", __func__);
	CCategoryHandler::RemoveInstance();
	CResourceHandler::FreeInstance();

	LEAVE_SYNCED_CODE();
}


void CGame::AddTimedJobs()
{
	RECOIL_DETAILED_TRACY_ZONE;
	{
		JobDispatcher::Job j;

		j.f = [this]() -> bool {
			const float simFrameDeltaTime = (spring_gettime() - lastSimFrameNetPacketTime).toMilliSecsf();
			const float gcForcedDeltaTime = (5.0f * 1000.0f) / (GAME_SPEED * gs->speedFactor);

			// SimFrame handles gc when not paused, this all other cases
			// do not check the global synced state, never true in demos
			// PR 27b: under the split this job runs on the main/draw thread,
			// so it may not lua_gc the sim-thread-owned synced states -- and
			// it is the ONLY collector the unsynced states have: SimFrame's
			// call is filtered to the synced mirror on the sim thread, so
			// the paused-only gate master used would let LuaUI grow without
			// bound during active play (windowed-dogfood LUA_ERRMEM at ~3min)
			if (SimDrawSplit::Enabled()) {
				eventHandler.CollectGarbage(false, CEventHandler::GC_UNSYNCED_ONLY);
			} else if (luaGCControl == 1 || simFrameDeltaTime > gcForcedDeltaTime) {
				eventHandler.CollectGarbage(false, CEventHandler::GC_ALL);
			}

			CInputReceiver::CollectGarbage();
			return true;
		};

		j.freq = GAME_SPEED;
		j.time = (1000.0f / j.freq) * (1 - j.startDirect);
		j.name = "EventHandler::CollectGarbage";

		jobDispatcher.AddTimedJob(j);
	}

	{
		JobDispatcher::Job j;

		j.f = []() -> bool {
			CTimeProfiler::GetInstance().Update();
			return true;
		};

		j.freq = 1.0f;
		j.time = (1000.0f / j.freq) * (1 - j.startDirect);
		j.name = "Profiler::Update";

		jobDispatcher.AddTimedJob(j);
	}
}

void CGame::Load(const std::string& mapFileName)
{
	// NOTE:
	//   this is needed for LuaHandle::CallOut*UpdateCallIn
	//   the main-thread is NOT the same as the load-thread
	//   when LoadingMT=1 (!!!)
	Threading::SetGameLoadThread();
	Watchdog::RegisterThread(WDT_LOAD);

	ZoneScoped;

	std::vector<std::string> contentErrors;

	auto& globalQuit = gu->globalQuit;
	bool  forcedQuit = false;

	LuaParser baseDefsParser("gamedata/defs.lua", SPRING_VFS_MOD_BASE, SPRING_VFS_ZIP, {true}, {false});
	LuaParser nullDefsParser("return {UnitDefs = {}, FeatureDefs = {}, WeaponDefs = {}, ArmorDefs = {}, MoveDefs = {}}", SPRING_VFS_ZIP, 0, {true}, {true});

	LuaParser* defsParser = &baseDefsParser;

	try {
		LOG("[Game::%s][1] globalQuit=%d threaded=%d", __func__, globalQuit.load(), !Threading::IsMainThread());

		LoadMap(mapFileName);
		Watchdog::ClearTimer(WDT_LOAD);
		LoadDefs(defsParser);
		Watchdog::ClearTimer(WDT_LOAD);
	} catch (const content_error& e) {
		contentErrors.emplace_back(e.what());
		LOG_L(L_ERROR, "[Game::%s][1] forced quit with exception \"%s\"", __func__, e.what());

		defsParser = &nullDefsParser;
		defsParser->Execute();

		// we can not (yet) do a clean early exit here because the dtor assumes
		// all loading stages proceeded normally; just force automatic shutdown
		forcedQuit = true;
	}

	try {
		LOG("[Game::%s][2] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

		PreLoadSimulation(defsParser);
		Watchdog::ClearTimer(WDT_LOAD);
		PreLoadRendering();
		Watchdog::ClearTimer(WDT_LOAD);
	} catch (const content_error& e) {
		contentErrors.emplace_back(e.what());
		LOG_L(L_ERROR, "[Game::%s][2] forced quit with exception \"%s\"", __func__, e.what());
		forcedQuit = true;
	}

	try {
		LOG("[Game::%s][3] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

		PostLoadSimulation(defsParser);
		Watchdog::ClearTimer(WDT_LOAD);
		PostLoadRendering();
		Watchdog::ClearTimer(WDT_LOAD);
	} catch (const content_error& e) {
		contentErrors.emplace_back(e.what());
		LOG_L(L_ERROR, "[Game::%s][3] forced quit with exception \"%s\"", __func__, e.what());
		forcedQuit = true;
	}
	if (!forcedQuit) {
		try {
			LOG("[Game::%s][4] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

			LoadInterface();
			Watchdog::ClearTimer(WDT_LOAD);
		} catch (const content_error& e) {
			contentErrors.emplace_back(e.what());
			LOG_L(L_ERROR, "[Game::%s][4] forced quit with exception \"%s\"", __func__, e.what());
			forcedQuit = true;
		}
	}

	if (!forcedQuit) {
		try {
			LOG("[Game::%s][5] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

			LoadFinalize();
			Watchdog::ClearTimer(WDT_LOAD);
		} catch (const content_error& e) {
			contentErrors.emplace_back(e.what());
			LOG_L(L_ERROR, "[Game::%s][5] forced quit with exception \"%s\"", __func__, e.what());
			forcedQuit = true;
		}
	}

	if (!forcedQuit) {
		try {
			LOG("[Game::%s][6] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

			LoadLua(saveFileHandler != nullptr, false);
			Watchdog::ClearTimer(WDT_LOAD);
		} catch (const content_error& e) {
			contentErrors.emplace_back(e.what());
			LOG_L(L_ERROR, "[Game::%s][6] forced quit with exception \"%s\"", __func__, e.what());
			forcedQuit = true;
		}
	}

	try {
		LOG("[Game::%s][7] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

		if (!globalQuit && saveFileHandler != nullptr) {
			loadscreen->SetLoadMessage("Loading Saved Game");
			{
				auto lock = CLoadLock::GetUniqueLock();
				saveFileHandler->LoadGame();
				Watchdog::ClearTimer(WDT_LOAD);
			}
			LoadLua(false, true);
			Watchdog::ClearTimer(WDT_LOAD);
		} else {
			ENTER_SYNCED_CODE();
			{
				auto lock = CLoadLock::GetUniqueLock();
				eventHandler.GamePreload();
				Watchdog::ClearTimer(WDT_LOAD);
				eventHandler.CollectGarbage(true);
				Watchdog::ClearTimer(WDT_LOAD);
			}
			LEAVE_SYNCED_CODE();
		}
		// Update height bounds and pathing after pregame or a saved game load.
		{
			ENTER_SYNCED_CODE();
			//needed in case pre-game terraform changed the map
			readMap->UpdateHeightBounds();
			Watchdog::ClearTimer(WDT_LOAD);
			pathManager->PostFinalizeRefresh();
			Watchdog::ClearTimer(WDT_LOAD);
			LEAVE_SYNCED_CODE();
		}

		{
			char msgBuf[512];

			SNPRINTF(msgBuf, sizeof(msgBuf), "[Game::%s][lua{Rules,Gaia}={%p,%p}][locale=\"%s\"]", __func__, luaRules, luaGaia, setlocale(LC_ALL, nullptr));
			CLIENT_NETLOG(gu->myPlayerNum, LOG_LEVEL_INFO, msgBuf);
		}
	} catch (const content_error& e) {
		contentErrors.emplace_back(e.what());
		LOG_L(L_ERROR, "[Game::%s][7] forced quit with exception \"%s\"", __func__, e.what());
		forcedQuit = true;
	}

	if (!forcedQuit) {
		try {
			LOG("[Game::%s][8] globalQuit=%d forcedQuit=%d", __func__, globalQuit.load(), forcedQuit);

			LoadSkirmishAIs();
			Watchdog::ClearTimer(WDT_LOAD);
		} catch (const content_error& e) {
			contentErrors.emplace_back(e.what());
			LOG_L(L_ERROR, "[Game::%s][8] forced quit with exception \"%s\"", __func__, e.what());
			forcedQuit = true;
		}
	}

	Watchdog::DeregisterThread(WDT_LOAD);
	AddTimedJobs();

	if (forcedQuit)
		spring::exitCode = spring::EXIT_CODE_NOLOAD;

	if (!contentErrors.empty())
		ErrorMessageBox(fmt::format("Errors:\n{}", fmt::join(contentErrors, "\n")).c_str(), "Recoil: caught content_error(s)", MBF_OK | MBF_CRASH);

	loadDone = true;
	globalQuit = globalQuit | forcedQuit;
}


void CGame::LoadMap(const std::string& mapFileName)
{
	ENTER_SYNCED_CODE();

	{
		SCOPED_ONCE_TIMER("Game::LoadMap");
		loadscreen->SetLoadMessage("Parsing Map Information");

		waterRendering->Init();
		mapRendering->Init();

		// simulation components
		helper->Init();
		readMap = CReadMap::LoadMap(mapFileName);

		/* Uses half-size grid because it *incorrectly* assumes
		 * building positions are always snapped to the build grid. */
		static_assert(BUILD_GRID_RESOLUTION == 2);
		buildingMaskMap.Init(mapDims.hmapx * mapDims.hmapy);

		groundBlockingObjectMap.Init(mapDims.mapSquares);
		yardmapStatusEffectsMap.InitNewYardmapStatusEffectsMap();
	}

	LEAVE_SYNCED_CODE();
}


void CGame::LoadDefs(LuaParser* defsParser)
{
	ENTER_SYNCED_CODE();

	{
		SCOPED_ONCE_TIMER("Game::LoadDefs (GameData)");
		loadscreen->SetLoadMessage("Loading GameData Definitions");

		defsParser->SetupLua(true, true);
		// customize the defs environment; LuaParser has no access to LuaSyncedRead
		#define LSR_ADDFUNC(f) defsParser->AddFunc(#f, LuaSyncedRead::f)
		defsParser->GetTable("Spring");

		LSR_ADDFUNC(GetModOptions);
		LSR_ADDFUNC(GetModOption);
		LSR_ADDFUNC(GetMapOptions);
		LSR_ADDFUNC(GetMapOption);
		LSR_ADDFUNC(GetTeamLuaAI);
		LSR_ADDFUNC(GetTeamList);
		LSR_ADDFUNC(GetGaiaTeamID);
		LSR_ADDFUNC(GetPlayerList);
		LSR_ADDFUNC(GetAllyTeamList);
		LSR_ADDFUNC(GetTeamInfo);
		LSR_ADDFUNC(GetAllyTeamInfo);
		LSR_ADDFUNC(GetAIInfo);
		LSR_ADDFUNC(GetTeamAllyTeamID);
		LSR_ADDFUNC(AreTeamsAllied);
		LSR_ADDFUNC(ArePlayersAllied);
		LSR_ADDFUNC(GetSideData);

		defsParser->EndTable();
		#undef LSR_ADDFUNC

		// run the parser
		if (!defsParser->Execute())
			throw content_error("Defs-Parser: " + defsParser->GetErrorLog());

		const LuaTable& root = defsParser->GetRoot();

		if (!root.IsValid())
			throw content_error("Error loading gamedata definitions");

		// bail now if any of these tables are invalid
		// makes searching for errors that much easier
		if (!root.SubTable("UnitDefs").IsValid())
			throw content_error("Error loading UnitDefs");

		if (!root.SubTable("FeatureDefs").IsValid())
			throw content_error("Error loading FeatureDefs");

		if (!root.SubTable("WeaponDefs").IsValid())
			throw content_error("Error loading WeaponDefs");

		if (!root.SubTable("ArmorDefs").IsValid())
			throw content_error("Error loading ArmorDefs");

		if (!root.SubTable("MoveDefs").IsValid())
			throw content_error("Error loading MoveDefs");

	}

	{
		loadscreen->SetLoadMessage("Loading Radar Icons");
		auto lock = CLoadLock::GetUniqueLock();
		icon::iconHandler.Init();
	}
	{
		SCOPED_ONCE_TIMER("Game::LoadDefs (Sound)");
		loadscreen->SetLoadMessage("Loading Sound Definitions");

		LuaParser soundDefsParser("gamedata/sounds.lua", SPRING_VFS_MOD_BASE, SPRING_VFS_MOD_BASE);
		soundDefsParser.GetTable("Spring");
		soundDefsParser.AddFunc("GetModOptions", LuaSyncedRead::GetModOptions);
		soundDefsParser.AddFunc("GetMapOptions", LuaSyncedRead::GetMapOptions);
		soundDefsParser.EndTable();

		sound->LoadSoundDefs(&soundDefsParser);
		chatSound = sound->GetDefSoundId("IncomingChat");
	}

	LEAVE_SYNCED_CODE();
}


void CGame::PreLoadSimulation(LuaParser* defsParser)
{
	ZoneScoped;
	ENTER_SYNCED_CODE();

	loadscreen->SetLoadMessage("Creating Smooth Height Mesh");
	smoothGround.Init(int2(mapDims.mapx, mapDims.mapy), modInfo.smoothMeshResDivider, modInfo.smoothMeshSmoothRadius);

	loadscreen->SetLoadMessage("Creating QuadField & CEGs");
	moveDefHandler.Init(defsParser);
	quadField.Init(int2(mapDims.mapx, mapDims.mapy), modInfo.quadFieldQuadSizeInElmos);
	damageArrayHandler.Init(defsParser);
	explGenHandler.Init();
}

void CGame::PostLoadSimulation(LuaParser* defsParser)
{
	ZoneScoped;
	CommonDefHandler::InitStatic();

	{
		SCOPED_ONCE_TIMER("Game::PostLoadSim (WeaponDefs)");
		loadscreen->SetLoadMessage("Loading Weapon Definitions");
		weaponDefHandler->Init(defsParser);
	}
	{
		SCOPED_ONCE_TIMER("Game::PostLoadSim (UnitDefs)");
		loadscreen->SetLoadMessage("Loading Unit Definitions");
		unitDefHandler->Init(defsParser);
	}
	{
		SCOPED_ONCE_TIMER("Game::PostLoadSim (FeatureDefs)");
		loadscreen->SetLoadMessage("Loading Feature Definitions");
		featureDefHandler->Init(defsParser);
	}

	CUnit::InitStatic();
	CCommandAI::InitCommandDescriptionCache();
	CUnitScriptFactory::InitStatic();
	CUnitScriptEngine::InitStatic();
	MoveTypeFactory::InitStatic();
	CWeaponLoader::InitStatic(unitDefHandler);

	unitHandler.Init();
	featureHandler.Init();
	projectileHandler.Init();
	CLosHandler::InitStatic();

	readMap->InitHeightMapDigestVectors(losHandler->los.size);

	// pre-load the PFS, gets finalized after Lua
	//
	// features loaded from the map (and any terrain changes
	// made by Lua while loading) would otherwise generate a
	// queue of pending PFS updates, which should be consumed
	// to avoid blocking regular updates from being processed
	// however, doing so was impossible without stalling the
	// loading thread for *minutes* in the worst-case scenario
	//
	// the only disadvantage is that LuaPathFinder can not be
	// used during Lua initialization anymore (not a concern)
	//
	// NOTE:
	//   the cache written to disk will reflect changes made by
	//   Lua which can vary each run with {mod,map}options, etc
	//   --> need a way to let Lua flush it or re-calculate map
	//   checksum (over heightmap + blockmap, not raw archive)
	mapDamage = IMapDamage::InitMapDamage();
	pathManager = IPathManager::GetInstance(modInfo.pathFinderSystem);
	moveDefHandler.PostSimInit();

	// load map-specific features
	loadscreen->SetLoadMessage("Initializing Map Features");
	featureDefHandler->LoadFeatureDefsFromMap();
	if (saveFileHandler == nullptr)
		featureHandler.LoadFeaturesFromMap();

	// must be called after features are all loaded
	unitDefHandler->SanitizeUnitDefs();

	envResHandler.LoadTidal(mapInfo->map.tidalStrength);
	envResHandler.LoadWind(mapInfo->atmosphere.minWind, mapInfo->atmosphere.maxWind);


	inMapDrawerModel = new CInMapDrawModel();
	inMapDrawer = new CInMapDraw();

	LEAVE_SYNCED_CODE();
}


void CGame::PreLoadRendering()
{
	ZoneScoped;
	auto lock = CLoadLock::GetUniqueLock();

	geometricObjects = new CGeometricObjects();

	// load components that need to exist before PostLoadSimulation
	modelUniformsStorage.Init();
	//transformsMemStorage.Init(); // Add?

	transformsUploader.Init();
	modelUniformsUploader.Init();
	worldDrawer.InitPre();
}

void CGame::PostLoadRendering() {
	ZoneScoped;
	worldDrawer.InitPost();
}


void CGame::LoadInterface()
{
	ZoneScoped;
	auto lock = CLoadLock::GetUniqueLock();

	camHandler->Init();
	mouse->ReloadCursors();

	selectedUnitsHandler.Init(playerHandler.ActivePlayers());

	// NB: these are also added to word-completion
	syncedGameCommands->AddDefaultActionExecutors();
	unsyncedGameCommands->AddDefaultActionExecutors();

	// interface components
	cmdColors.LoadConfigFromFile("cmdcolors.txt");

	keyBindings.Init();
	keyBindings.LoadDefaults();
	keyBindings.Load();

	{
		SCOPED_ONCE_TIMER("Game::LoadInterface (Console)");

		gameConsoleHistory.Init();
		gameTextInput.ClearInput();

		wordCompletion.Init();

		for (int pp = 0; pp < playerHandler.ActivePlayers(); pp++) {
			wordCompletion.AddWordRaw(playerHandler.Player(pp)->name, false, false, false);
		}

		// add the Skirmish AIs instance names to word completion (eg for chatting)
		for (const auto& ai: skirmishAIHandler.GetAllSkirmishAIs()) {
			wordCompletion.AddWordRaw(ai.second->name + " ", false, false, false);
		}
		// add the available Skirmish AI libraries to word completion, for /aicontrol
		for (const auto& aiLib: aiLibManager->GetSkirmishAIKeys()) {
			wordCompletion.AddWordRaw(aiLib.GetShortName() + " " + aiLib.GetVersion() + " ", false, false, false);
		}

		// add the available Lua AI implementations to word completion, for /aicontrol
		for (const std::string& sn: skirmishAIHandler.GetLuaAIImplShortNames()) {
			wordCompletion.AddWordRaw(sn + " ", false, false, false);
		}

		// register {Unit,Feature}Def names
		for (const auto& pair: unitDefHandler->GetUnitDefIDs()) {
			wordCompletion.AddWordRaw(pair.first + " ", false, true, false);
		}
		for (const auto& pair: featureDefHandler->GetFeatureDefIDs()) {
			wordCompletion.AddWordRaw(pair.first + " ", false, true, false);
		}

		// register /command's
		for (const auto& pair: syncedGameCommands->GetActionExecutors()) {
			wordCompletion.AddWordRaw("/" + pair.first + " ", true, false, false);
		}
		for (const auto& pair: unsyncedGameCommands->GetActionExecutors()) {
			wordCompletion.AddWordRaw("/" + pair.first + " ", true, false, false);
		}
		// legacy commands without executors
		for (const auto& pair: gameCommandConsole.GetCommandMap()) {
			wordCompletion.AddWordRaw("/" + pair.first + " ", true, false, false);
		}

		wordCompletion.Sort();
		wordCompletion.Filter();
	}

	tooltip = new CTooltipConsole();
	guihandler = new CGuiHandler();
	minimap = new CMiniMap();
	resourceBar = new CResourceBar();
	selectionKeys.Init();

	uiGroupHandlers.clear();
	uiGroupHandlers.reserve(teamHandler.ActiveTeams());

	for (int t = 0; t < teamHandler.ActiveTeams(); ++t) {
		uiGroupHandlers.emplace_back(t);
	}

	if (saveFileHandler == nullptr) {
		// note: disable is needed in case user reloads before StartPlaying
		GameSetupDrawer::Disable();
		GameSetupDrawer::Enable();
	}

	RmlGui::Initialize();
}

void CGame::LoadLua(bool dryRun, bool onlyUnsynced)
{
	ZoneScoped;
	assert(!(dryRun && onlyUnsynced));
	// Lua components
	ENTER_SYNCED_CODE();
	CLuaHandle::SetDevMode(gameSetup->luaDevMode);
	LOG("[Game::%s] Lua developer mode %sabled", __func__, (CLuaHandle::GetDevMode()? "en": "dis"));

	const std::string prefix = (dryRun ? "Synced " : (onlyUnsynced ? "Unsynced " : ""));
	const std::string names[] = {"LuaRules", "LuaGaia"};

	CSplitLuaHandle* handles[] = {luaRules, luaGaia};
	decltype(&CLuaRules::LoadFreeHandler) loaders[] = {CLuaRules::LoadFreeHandler, CLuaGaia::LoadFreeHandler};

	for (int i = 0; i < 2; i++) {
		loadscreen->SetLoadMessage("Loading " + prefix + names[i]);

		if (onlyUnsynced && handles[i] != nullptr) {
			handles[i]->InitUnsynced();
		} else {
			loaders[i](dryRun);
		}
	}

	LEAVE_SYNCED_CODE();

	if (!dryRun) {
		loadscreen->SetLoadMessage("Loading LuaUI");
		auto lock = CLoadLock::GetUniqueLock();
		CLuaUI::LoadFreeHandler();
	}
}

void CGame::LoadSkirmishAIs()
{
	if (gameSetup->hostDemo)
		return;
	// happens if LoadInterface was skipped or interrupted on forcedQuit
	// the AI callback code expects this to be non-empty on construction
	if (uiGroupHandlers.empty())
		return;

	// create Skirmish AI's if required
	const std::vector<uint8_t>& localAIs = skirmishAIHandler.GetSkirmishAIsByPlayer(gu->myPlayerNum);
	if (localAIs.empty() && !IsSavedGame())
		return;

	SCOPED_ONCE_TIMER("Game::LoadSkirmishAIs");
	loadscreen->SetLoadMessage("Loading Skirmish AIs");

	for (uint8_t localAI: localAIs)
		skirmishAIHandler.CreateLocalSkirmishAI(localAI, IsSavedGame());

	if (IsSavedGame()) {
		saveFileHandler->LoadAIData();

		for (uint8_t localAI: localAIs)
			skirmishAIHandler.PostLoadSkirmishAI(localAI);
	}
}

void CGame::LoadFinalize()
{
	ZoneScoped;
	{
		loadscreen->SetLoadMessage("[" + std::string(__func__) + "] finalizing PFS");

		ENTER_SYNCED_CODE();
		const std::uint64_t dt = pathManager->Finalize();
		const std::uint32_t cs = pathManager->GetPathCheckSum();
		LEAVE_SYNCED_CODE();

		loadscreen->SetLoadMessage(
			"[" + std::string(__func__) + "] finalized PFS " +
			"(" + IntToString(dt, "%ld") + "ms, checksum " + IntToString(cs, "%08x") + ")"
		);
	}

	lastReadNetTime = spring_gettime();
	lastSimFrameTime = lastReadNetTime;
	lastDrawFrameTime = lastReadNetTime;
	updateDeltaSeconds = 0.0f;
}


void CGame::PostLoad()
{
	RECOIL_DETAILED_TRACY_ZONE;
	GameSetupDrawer::Disable();

	Sim::systemUtils.NotifyPostLoad();

	if (gameServer != nullptr) {
		gameServer->PostLoad(gs->frameNum);
	}
}


void CGame::KillLua(bool dtor)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// belongs here; destructs LuaIntro (which might access sound, etc)
	// if LoadingMT=1, a reload-request might be seen by SpringApp::Run
	// while the loading thread is still alive so this must go first
	assert((!dtor) || (loadscreen == nullptr));

	LOG("[Game::%s][0] dtor=%d loadscreen=%p", __func__, dtor, loadscreen);
	CLoadScreen::DeleteInstance();

	// kill LuaUI here, various handler pointers are invalid in ~GuiHandler
	LOG("[Game::%s][1] dtor=%d luaUI=%p", __func__, dtor, luaUI);
	CLuaUI::FreeHandler();

	ENTER_SYNCED_CODE();
	LOG("[Game::%s][2] dtor=%d luaGaia=%p", __func__, dtor, luaGaia);
	CLuaGaia::FreeHandler();

	LOG("[Game::%s][3] dtor=%d luaRules=%p", __func__, dtor, luaRules);
	CLuaRules::FreeHandler();

	CSplitLuaHandle::ClearGameParams();
	LEAVE_SYNCED_CODE();


	LOG("[Game::%s][4] dtor=%d", __func__, dtor);
	LuaOpenGL::Free();

	LOG("[Game::%s][5] dtor=%d", __func__, dtor);
	creg::UnregisterAllCFunctions();
}

void CGame::KillMisc()
{
	RECOIL_DETAILED_TRACY_ZONE;
	LOG("[Game::%s][1]", __func__);
	CEndGameBox::Destroy();
	IVideoCapturing::FreeInstance();

	LOG("[Game::%s][2]", __func__);
	// delete this first since AI's might call back into sim-components in their dtors
	// this means the simulation *should not* assume the EOH still exists on game exit
	CEngineOutHandler::Destroy();

	LOG("[Game::%s][3]", __func__);
	// TODO move these to the end of this dtor, once all action-executors are registered by their respective engine sub-parts
	UnsyncedGameCommands::DestroyInstance(gu->globalReload);
	SyncedGameCommands::DestroyInstance(gu->globalReload);
}

void CGame::KillRendering()
{
	RECOIL_DETAILED_TRACY_ZONE;
	LOG("[Game::%s][1]", __func__);
	// pending records reference sim objects that die without further drains
	// (CUnitHandler::Kill frees units without Render*Destroyed notifications)
	renderEventQueue.Clear();
	// same for boundary-deferred unsynced dispatches (their targets die here)
	UnsyncedBoundaryQueue::Clear();
	SimDrawSplit::Clear();
	simSnapshot.Clear();
	drawMapMirrors.Clear(); // PR 28: forget the map-layer mirrors for the next game
	snapshotPickGrid.Clear();
	// per-generation serving caches (the generation counter resets with the snapshot)
	LuaSnapshotServe::ClearCaches();
	// dumps the draw-contract trip inventory into the infolog before reset
	// (the PR-27a gate artifact; no-op when the contract never tripped)
	LuaSplitContract::Clear();
	icon::iconHandler.Kill();
	spring::SafeDelete(geometricObjects);
	worldDrawer.Kill();

	modelUniformsStorage.Kill();
	//transformsMemStorage.Kill(); //Add?

	transformsUploader.Kill();
	modelUniformsUploader.Kill();
}

void CGame::KillInterface()
{
	RECOIL_DETAILED_TRACY_ZONE;
	LOG("[Game::%s][1]", __func__);
	ProfileDrawer::SetEnabled(false);
	camHandler->Kill();
	spring::SafeDelete(guihandler);
	spring::SafeDelete(minimap);
	spring::SafeDelete(resourceBar);
	spring::SafeDelete(tooltip); // CTooltipConsole*

	LOG("[Game::%s][2]", __func__);
	keyBindings.Kill();
	selectionKeys.Kill(); // CSelectionKeyHandler*
	spring::SafeDelete(inMapDrawerModel);
	spring::SafeDelete(inMapDrawer);
}

void CGame::KillSimulation()
{
	RECOIL_DETAILED_TRACY_ZONE;
	LOG("[Game::%s][1]", __func__);

	// Kill all teams that are still alive, in
	// case the game did not do so through Lua.
	//
	// must happen after Lua (cause CGame is already
	// null'ed and Died() causes a Lua event, which
	// could issue Lua code that tries to access it)
	for (int t = 0; t < teamHandler.ActiveTeams(); ++t) {
		teamHandler.Team(t)->Died(false);
	}

	LOG("[Game::%s][2]", __func__);
	unitHandler.DeleteScripts();

	// KillRendering dropped any queued destroy records; destruct and free
	// the deferred shells before the handlers clear the pools under them
	deferredObjectDeleter.Clear();

	featureHandler.Kill(); // depends on unitHandler (via ~CFeature)
	unitHandler.Kill();
	projectileHandler.Kill();

	LOG("[Game::%s][3]", __func__);
	IPathManager::FreeInstance(pathManager);
	IMapDamage::FreeMapDamage(mapDamage);

	spring::SafeDelete(readMap);
	smoothGround.Kill();

	groundBlockingObjectMap.Kill();
	buildingMaskMap.Kill();

	CLosHandler::KillStatic(gu->globalReload);
	quadField.Kill();
	moveDefHandler.Kill();
	unitDefHandler->Kill();
	featureDefHandler->Kill();
	weaponDefHandler->Kill();
	damageArrayHandler.Kill();
	explGenHandler.Kill();
	spring::SafeDelete((mapInfo = const_cast<CMapInfo*>(mapInfo)));

	LOG("[Game::%s][4]", __func__);
	CCommandAI::KillCommandDescriptionCache();
	CUnitScriptEngine::KillStatic();
	CWeaponLoader::KillStatic();
	CommonDefHandler::KillStatic();

	Sim::ClearRegistry();
}





void CGame::ResizeEvent()
{
	LOG("[Game::%s][1]", __func__);

	{
		SCOPED_ONCE_TIMER("Game::ViewResize")

		if (minimap != nullptr)
			minimap->UpdateGeometry();

		//recreate water on resize (lazy but works)
		const auto wt = IWater::GetWater()->GetID();
		IWater::KillWater();
		IWater::SetWater(wt);
	}

	LOG("[Game::%s][2]", __func__);

	{
		SCOPED_ONCE_TIMER("EventHandler::ViewResize");

		gameTextInput.ViewResize();
		eventHandler.ViewResize();
	}
}

int CGame::KeyPressed(int keyCode, int scanCode, bool isRepeat)
{
	gameInputReceiver.KeyPressed(keyCode, scanCode, isRepeat);
	return 0;
}

int CGame::KeyReleased(int keyCode, int scanCode)
{
	gameInputReceiver.KeyReleased(keyCode, scanCode);
	return 0;
}

CInputReceiver* CGame::GetInputReceiver()
{
	return &gameInputReceiver;
}

int CGame::KeyMapChanged()
{
	RECOIL_DETAILED_TRACY_ZONE;
	eventHandler.KeyMapChanged();

	return 0;
}

int CGame::TextInput(const std::string& utf8Text)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (RmlGui::ProcessTextInput(utf8Text))
		return 0;

	if (eventHandler.TextInput(utf8Text))
		return 0;

	return (gameTextInput.SetInputText(utf8Text));
}

int CGame::TextEditing(const std::string& utf8Text, unsigned int start, unsigned int length)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (eventHandler.TextEditing(utf8Text, start, length))
		return 0;

	return (gameTextInput.SetEditText(utf8Text));
}


bool CGame::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
	good_fpu_control_registers("CGame::Update");

	// sim->render events fired below (ClientReadNet -> SimFrame) queue up as
	// records; CGame::Draw drains them at the draw boundary. Under the split
	// the sim thread opens the phase itself, each loop iteration.
	if (!SimDrawSplit::Enabled())
		renderEventQueue.BeginSimPhase();

	jobDispatcher.Update();
	clientNet->Update();

	// When video recording do step by step simulation, so each simframe gets a corresponding videoframe
	// FIXME: SERVER ALREADY DOES THIS BY ITSELF
	if (playing && gameServer != nullptr && videoCapturing->AllowRecord())
		gameServer->CreateNewFrame(false, true);

	if (SimDrawSplit::Enabled()) {
		// PR 27b: the sim thread owns ClientReadNet -> SimFrames (spawned
		// here so the whole CGame is constructed first). The main thread
		// keeps connection upkeep -- every clientNet entry point used below
		// locks -- and the CPU-usage report, which reads main-side data.
		SpawnSimThread();
		SendClientProcUsage();

		if (!gameOver) {
			if (clientNet->NeedsReconnect())
				clientNet->AttemptReconnect(SpringVersion::GetSync(), Platform::GetPlatformStr());

			if (clientNet->CheckTimeout(0, gs->PreSimFrame()))
				GameEnd({}, true);
		}
	} else {
		ENTER_SYNCED_CODE();
		SendClientProcUsage();
		{
			// the sim phase (PR 27b): sim-fired unsynced work inside this
			// bracket boundary-defers when the split flag is on; under the
			// split the sim thread's loop opens the same bracket instead
			SimDrawSplit::ScopedSimPhase simPhase;
			ClientReadNet(); // issues new SimFrame()s
		}

		if (!gameOver) {
			if (clientNet->NeedsReconnect())
				clientNet->AttemptReconnect(SpringVersion::GetSync(), Platform::GetPlatformStr());

			if (clientNet->CheckTimeout(0, gs->PreSimFrame()))
				GameEnd({}, true);
		}

		LEAVE_SYNCED_CODE();
	}

	{
		SLuaAllocError error = {};

		if (spring_lua_alloc_get_error(&error)) {
			// convert the "abc\ndef\n..." buffer into 0-terminated "abc", "def", ... chunks
			for (char *ptr = &error.msgBuf[0], *tmp = nullptr; (tmp = strstr(ptr, "\n")) != nullptr; ptr = tmp + 1) {
				*tmp = 0;

				LOG_L(L_FATAL, "%s", error.msgBuf);
				CLIENT_NETLOG(gu->myPlayerNum, LOG_LEVEL_FATAL, error.msgBuf);

				// force a restart if synced Lua died, simply reloading might not work
				gu->globalQuit = gu->globalQuit || (strstr(error.msgBuf, "[OOM] synced=1") != nullptr);
			}
		}
	}

	return true;
}


bool CGame::UpdateUnsynced(const spring_time currentTime)
{
	SCOPED_TIMER("Update");

	// timings and frame interpolation
	const spring_time deltaDrawFrameTime = currentTime - globalRendering->lastFrameStart;

	const float modGameDeltaTimeSecs = mix(deltaDrawFrameTime.toMilliSecsf() * 0.001f, 0.01f, skipping);
	const float unsyncedUpdateDeltaTime = (currentTime - lastUnsyncedUpdateTime).toSecsf();

	{
		// update game timings
		globalRendering->lastFrameStart = currentTime;
		globalRendering->lastFrameTime = deltaDrawFrameTime.toMilliSecsf();

		gu->avgFrameTime = mix(gu->avgFrameTime, deltaDrawFrameTime.toMilliSecsf(), 0.05f);
		gu->gameTime += modGameDeltaTimeSecs;
		gu->modGameTime += (modGameDeltaTimeSecs * gs->speedFactor * (1 - gs->paused));

		totalGameTime += (modGameDeltaTimeSecs * (playing && !gameOver));
		updateDeltaSeconds = modGameDeltaTimeSecs;
	}

	{
		// update sim-FPS counter once per second
		static int lsf = gs->frameNum;
		static spring_time lsft = currentTime;

		// toSecsf throws away too much precision
		const float diffMilliSecs = (currentTime - lsft).toMilliSecsf();

		if (diffMilliSecs >= 1000.0f) {
			gu->simFPS = (gs->frameNum - lsf) / (diffMilliSecs * 0.001f);
			lsft = currentTime;
			lsf = gs->frameNum;
		}
	}

	if (skipping) {
		// when fast-forwarding, maintain a draw-rate of 2Hz
		if (spring_tomsecs(currentTime - skipLastDrawTime) < 500.0f)
			return true;

		skipLastDrawTime = currentTime;

		DrawSkip();
		return true;
	}

	const bool newSimFrame = (lastSimFrame != gs->frameNum);
	numDrawFrames++;
	globalRendering->drawFrame = std::max(1U, globalRendering->drawFrame + 1);
	globalRendering->lastFrameStart = currentTime;
	// Update the interpolation coefficient (globalRendering->timeOffset)
	if (!gs->paused && !IsSimLagging() && !gs->PreSimFrame() && !videoCapturing->AllowRecord()) {
		globalRendering->weightedSpeedFactor = 0.001f * gu->simFPS;
		globalRendering->lastTimeOffset = globalRendering->timeOffset;
		globalRendering->timeOffset = (currentTime - lastFrameTime).toMilliSecsf() * globalRendering->weightedSpeedFactor;

		int SmoothTimeOffset = configHandler->GetInt("SmoothTimeOffset");
		float strictness = 0.9f; // This defines how strict we are going to be when trying to keep frame timings
		if (SmoothTimeOffset > 0) {
			strictness = 1.0f - (SmoothTimeOffset) * 0.025f;
		}

		// The main issue that SmoothTimeOffset tries to fix:
		// Is that lastFrameTime is reset when a sim frame is issued.
		// This makes the calculation of the timeOffset of the next draw frame after simframe incorrect (too small),
		// if the previous draw frame had a large timeOffset

		float drawsimratio = gu->simFPS * gu->avgFrameTime * 0.001f; // This should be like 0.5 for 60hz draw 30hz sim
		float LTO = globalRendering->lastTimeOffset;
		float CTO = globalRendering->timeOffset;

		// This mode forces a strict time step of 0.5 simframes per draw frames. Only useful for testing @ 60hz
		if (SmoothTimeOffset == -1) {
			if (newSimFrame) {
				if (LTO > (1.0f - drawsimratio * strictness))
					globalRendering->timeOffset = drawsimratio;
				else
					globalRendering->timeOffset = 0.0f;
			} else {
				if (LTO > drawsimratio * strictness)
					globalRendering->timeOffset = std::fmin(LTO + drawsimratio * strictness, 1.0f);
				else
					globalRendering->timeOffset = std::fmin(drawsimratio * strictness, 1.0f);
			}
		}

		// This mode tries to correct for the wrongly calculated timeOffset adaptively,
		// while trying to maintain a smooth interpolation rate
		// As frame rates dip below 45fps, this method is only marginally better than old method
		// But that is heavily dependent on whether the load is sim or draw based.
		// TODO: the camera smoothing still seems to take sim load into account heavily. So large sim loads jitter the camera quite a bit when moving
		if (SmoothTimeOffset > 0){

			// if we have a new sim frame, then check when the time and CTO of the previous draw frame was.
			drawsimratio = std::fmin(drawsimratio, 1.0);  // Clamp it otherwise we will accumulate delay when < 30 FPS
			float oldCTO = globalRendering->timeOffset;
			float newCTO = globalRendering->timeOffset;

			if (newSimFrame) {
				// newsimframe is a special case, as our new time offset is kind of wrong.
				// What we want to know is when the last draw happened, and at what offset.
				// There are two special cases here, if the last draw happened "on time", then we want to 'pull in' CTO to 0,
				// irrespective of the time spent in sim.
				// If the last draw frame didnt happen on time, and had a large CTO, then we need to 'carry over' some time offset

				if ((LTO + drawsimratio - 1.0 > (CTO)* strictness)) {
					newCTO = std::fmin((LTO + drawsimratio - 1.0f) * strictness, 1.3f);
					//LOG_L(L_DEBUG, "UpdateUnsynced newframe skipping, last = %.3f, currtimeoffset = %.3f, averageoffset = %.3f, now cheating it to %.3f", globalRendering->lastTimeOffset, globalRendering->timeOffset, drawsimratio, newCTO);
					globalRendering->timeOffset = newCTO;
				}
			}
			else {
				// On draw frames that dont have a preceding sim frame, we want to 'smooth' the CTO out a bit.
				// Otherwise, the sim frame is also calculated into the offset, making things jittery
				if ((CTO - LTO < (drawsimratio) * strictness)) {
					newCTO = std::fmin(LTO + drawsimratio * strictness, 1.3f);
					//LOG_L(L_DEBUG, "UpdateUnsynced Too short draw offset, last = %.3f, currtimeoffset = %.3f, averageoffset = %.3f, now cheating it to %.3f", globalRendering->lastTimeOffset, globalRendering->timeOffset, drawsimratio, newCTO);
					globalRendering->timeOffset = newCTO;
				}

			}
			//LOG_L(L_DEBUG, "oldCTO = %.3f newCTO = %.3f, drawsimratio = %.3f,  newframe = %d", oldCTO, newCTO, drawsimratio, newSimFrame);
		}



	} else {
		globalRendering->timeOffset = videoCapturing->GetTimeOffset();

		lastSimFrameTime = currentTime;
		lastFrameTime = currentTime;
	}

	if ((currentTime - frameStartTime).toMilliSecsf() >= 1000.0f) {
		globalRendering->FPS = (numDrawFrames * 1000.0f) / std::max(0.01f, (currentTime - frameStartTime).toMilliSecsf());

		// update draw-FPS counter once every second
		frameStartTime = currentTime;
		numDrawFrames = 0;

	}

	const bool forceUpdate = (unsyncedUpdateDeltaTime >= INV_GAME_SPEED);

	lastSimFrame = gs->frameNum;

	// PR 42 window shrink: when the split is actually running, the sim is parked
	// from the barrier through the drawer extraction below. Everything served /
	// mirror-fed / draw-owned can run AFTER the sim resumes -- so the two heavy
	// consumers that are safe sim-live (the mirror-fed info textures [PR 42's
	// InfoTexture->DrawMapMirrors conversion] and the widget Update callins
	// [snapshot-served]) are deferred to just after ReleaseSimPause below,
	// shrinking the parked window by their cost. Flag-off (or split not running)
	// keeps the exact current in-place order -- byte-identical. Enumerated
	// deviation (flag-on only): a widget that moves the camera / sets unit
	// tracking in its Update is reflected one draw frame late, because camera +
	// culling extraction now finalize before the widget callins run.
	// config toggle (default on) so the shrink can be A/B'd on one binary for
	// measurement / bisection: SplitWindowShrink = 0 keeps the whole UI phase
	// parked (pre-42 behavior), 1 defers the sim-live consumers past release.
	const bool shrinkWindow = SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning()
		&& (configHandler->GetInt("SplitWindowShrink") != 0);

	// the PR-11b boundary movers that used to run here moved into
	// SimDrawBarrier() (PR 26) -- same per-sim-frame-batch gating, same order
	// relative to the render-event drain, now inside the one barrier function

	// §8.1: apply the FPS direct-control camera-rotY nudge the sim thread deferred
	// this batch (draw owns the camera); no-op flag-off / when nothing pending
	camHandler->ApplyPendingFPSDirectControlRotY();

	// set camera
	camHandler->UpdateController(playerHandler.Player(gu->myPlayerNum), gu->fpsMode);

	lineDrawer.UpdateLineStipple();

	icon::iconHandler.Update();
	CNamedTextures::Update();

	// always update InfoTexture and SoundListener at <= 30Hz (even when paused)
	const bool doInfoTexSoundUpdate = (newSimFrame || forceUpdate);
	if (doInfoTexSoundUpdate) {
		lastUnsyncedUpdateTime = currentTime;

		// PR 42: infoTextureHandler->Update reads the boundary-drained mirrors
		// (DrawMapMirrors), so under the running split it defers to after
		// ReleaseSimPause (sim-live); in-place otherwise. sound->UpdateListener
		// just copies the camera vectors + flags the sound thread -- safe either
		// side, deferred with infotex to keep the pair together.
		if (!shrinkWindow) {
			// TODO: should be moved to WorldDrawer::Update
			infoTextureHandler->Update();
			// TODO call only when camera changed
			sound->UpdateListener(camera->GetPos(), camera->GetDir(), camera->GetUp());
		}
	}
	SetDrawMode(gameNormalDraw); //TODO move to ::Draw()?

	if (luaUI != nullptr) {
		luaUI->CheckStack();
		luaUI->CheckAction();
	}
	// PR 27b: the split-handle CheckStack pokes the synced lua_State too --
	// skip the diagnostic while the sim thread owns those states
	if (!(SimDrawSplit::Enabled() && SimDrawSplit::SimThreadRunning())) {
		if (luaGaia != nullptr)
			luaGaia->CheckStack();
		if (luaRules != nullptr)
			luaRules->CheckStack();
	}

	if (gameTextInput.SendPromptInput()) {
		gameConsoleHistory.AddLine(gameTextInput.userInput);
		SendNetChat(gameTextInput.userInput);
		gameTextInput.ClearInput();
	}
	if (inMapDrawer->IsWantLabel() && gameTextInput.SendLabelInput())
		gameTextInput.ClearInput();

	infoConsole->PushNewLinesToEventHandler();
	infoConsole->Update();

	//infoConsole->Update() can in theory cause the need to update fonts, so update here
	CFontTexture::Update();

	mouse->Update();
	mouse->UpdateCursors();
	// PR 42 window shrink + sim|draw PR 44 (Gap B): guihandler->Update's cursor
	// path now reads the snapshot-served command surface + the barrier-published
	// default-command reply instead of walking live commandAI, so under the
	// running split it defers to after ReleaseSimPause (sim-live) with the other
	// deferred UI consumers -- removing the PR-42b "keep guihandler parked"
	// carve-out. In-place (byte-identical) flag-off / when not shrinking.
	if (!shrinkWindow)
		guihandler->Update();
	commandDrawer->Update();

	// UI unit-group housekeeping: draw-owned containers, no frame-keyed logic,
	// safe at draw rate (moved from CGame::SimFrame, PR 11b)
	for (auto& grouphandler: uiGroupHandlers)
		grouphandler.Update();

	// PR 42: the widget Update callins read snapshot-served / draw-owned state,
	// so under the running split they defer to after ReleaseSimPause (sim-live);
	// in-place otherwise. (Deferred with infotex below.)
	if (!shrinkWindow) {
		SCOPED_TIMER("Update::EventHandler");
		eventHandler.Update();
	}

	if (unitTracker.Enabled())
		unitTracker.SetCam();

	camera->Update();
	shadowHandler.Update();
	{
		worldDrawer.Update(newSimFrame);
		// PR 27b: the drawer extraction (the pause window's second half, the
		// PR-26 deferral) is complete -- everything below reads extracted or
		// draw-owned storage only, so the sim may resume consuming
		ReleaseSimPause();

		// PR 42 window shrink: run the deferred sim-live consumers now that the
		// sim has resumed (see the shrinkWindow note above). infotex reads the
		// boundary-drained DrawMapMirrors; the widget Update callins read
		// snapshot-served / draw-owned state. Same relative order as the in-place
		// flag-off path (infotex+sound, then widget Update).
		if (shrinkWindow) {
			if (doInfoTexSoundUpdate) {
				infoTextureHandler->Update();
				sound->UpdateListener(camera->GetPos(), camera->GetDir(), camera->GetUp());
			}
			// sim|draw PR 44 (Gap B): deferred from the parked window above. Reads
			// the served command surface + the barrier default-command reply; kept
			// before eventHandler.Update to preserve the in-place relative order.
			guihandler->Update();
			{
				SCOPED_TIMER("Update::EventHandler");
				eventHandler.Update();
			}
		}

		transformsUploader.Update();
		modelUniformsUploader.Update();
	}

	mouse->UpdateCursorCameraDir(); // make sure mouse->dir is in sync with camera

	//Update per-drawFrame UBO
	UniformConstants::GetInstance().Update();

	eventHandler.DbgTimingInfo(TIMING_UNSYNCED, currentTime, spring_now());
	return false;
}


/**
 * @brief SimDrawBarrier -- the sim|draw extract barrier (PR 26)
 *
 * THE place where mutable sim state crosses to the draw side, once per draw
 * frame. Under the Phase-2 thread split this function body becomes the
 * sim-pause window verbatim: sim pauses at a frame edge (never mid-SimFrame),
 * this runs, sim resumes -- so everything that must happen "at the boundary"
 * has to live inside it, and nothing else may read mutable synced state
 * per-boundary from outside it.
 *
 * Contract:
 *  - SINGLE CALL SITE: the top of CGame::Draw, before any other draw-side
 *    code runs (input handlers calling in from elsewhere read the previous
 *    boundary's published data). Do not add call sites.
 *  - ORDER (fixed; each step's comment states its dependency):
 *      1. renderEventQueue.Drain()            -- drawer containers reach the
 *         completed sim frame's end state before anything draw-side reads them
 *         (PR 12; catch-up batches apply N frames of records here, in order).
 *      2. deferredObjectDeleter.AckDrainedDestroys() -- the drain dispatched
 *         every queued destroy record, so destruct the deferred shells and
 *         poison the slots (PR 13). ReleaseAcked() at the end of the same
 *         Draw returns the slots to the pools -- the ack/release pair brackets
 *         the draw frame; at split time the release folds into the next
 *         barrier (pools are sim-owned).
 *      3. simSnapshot.Update()                -- publish the observable-state
 *         snapshot (unit/projectile/feature rows due-checked per SimSnapshot.h;
 *         the PR-26 team/player boundary copy re-extracts unconditionally).
 *         After the drain, so snapshot and drawer containers agree on the same
 *         completed frame.
 *      4. snapshotDiffGate.CheckBoundary()    -- TEST-ONLY (PR 17) gate hook,
 *         right after publish; a single branch when unarmed.
 *      5. the PR-11b boundary movers          -- once per batch of completed
 *         sim frames, skip-gated exactly as before (see the block comment).
 *      6. readMap->UpdateDraw()               -- the heightmap dirty-rect
 *         drain (synced heightmap -> unsynced copy + UnsyncedHeightMapUpdate
 *         events), moved here from CWorldDrawer::Update: it is boundary work
 *         (sim writes the rect queue). Runs after the movers, preserving
 *         their old relative order.
 *      7. UnsyncedBoundaryQueue::Drain()      -- replay the sim phase's
 *         boundary-deferred unsynced dispatches (PR 27b: LuaUI events,
 *         Cob2Lua, SendToUnsynced, net-message draw pokes) in fire order.
 *         Empty unless the split flag is on.
 *  - MAY NOT run inside: rendering, GL work, or Lua callins -- with the
 *    sanctioned exceptions of the drain's Render* event dispatches, the
 *    UnsyncedHeightMapUpdate events of step 6, and the deferred unsynced
 *    dispatches of step 7, which exist precisely to fire at the boundary.
 *  - Deferred second half (documented decision, PR 26): the drawer extraction
 *    layer (CModelDrawerDataBase::Update/ExtractTransforms/
 *    UpdateObjectUniforms, projectileDrawer->UpdateDrawFlags -- the section-C
 *    UPD class) still runs inside worldDrawer.Update(), later in the frame,
 *    because it consumes the camera state updated between here and there;
 *    moving it into this function today would change culling inputs (a
 *    behavior change this PR forbids). At split time (27b) the pause window
 *    must span it: either the camera update moves ahead of the barrier and
 *    the extraction half moves in here, or the window extends -- decided
 *    there.
 */
void CGame::SimDrawBarrier()
{
	// boundary-cost telemetry (the PR-27b gate's fine-print number)
	SCOPED_TIMER("Misc::SimDrawBarrier");

	// (0) split only: open the boundary drain window -- the destroy records
	// about to dispatch populate the id->shell fallback that lets step 7's
	// deferred handlers resolve objects that died later in the same burst
	// (closed again at step 8, before the ack poisons the shells)
	if (SimDrawSplit::Enabled())
		SimDrawSplit::SetBoundaryShellWindow(true);

	// (0b) split only: run the GL upload half of any sim-thread model loads
	// BEFORE the drain -- the creation records about to dispatch may
	// register objects with these models (PR 27b commit c)
	modelLoader.ServiceQueuedUploads();

	// (1) apply the sim frames' queued render-event records (object creation,
	// destruction, LOS transitions) before any draw-side code reads the
	// drawer containers
	renderEventQueue.Drain();

	// (1b) split only: deliver the drained destroys to the draw-owned death
	// dependents (selection, wait-AI, tracked lights) -- they skip
	// registering real death-dependences on sim objects under the split
	// (SimDrawSplit.h), and this is their replacement notification
	DeliverBoundaryDeaths();

	// (1c) split only: snapshot the sim-owned effect containers the draw passes
	// iterate (the draw passes may not touch the sim-owned tables post-release;
	// see ModelDrawerData.h). After the drain so fresh registrations are
	// covered, before anything downstream resolves.
	if (SimDrawSplit::Enabled()) {
		// SCOPE-1 (plan PR 39) + PR 40: the unit/feature/projectile id->object
		// resolution caches are all DELETED — those drawers now resolve ids
		// through a producer-captured deferred-safe handle (no per-barrier
		// rebuild). The projectile drawer still needs a boundary copy of the
		// ground-flash / flying-piece containers it iterates live.
		projectileDrawer->SnapshotEffectContainers();
	}

	// (2) the drain dispatched every queued destroy record: the draw side has
	// acked those objects, so destruct their deferred shells and poison the
	// slots; ReleaseAcked() at the end of this Draw returns them to the pools.
	// Split only: the ack moves to the END of the barrier -- the deferred
	// unsynced dispatches of step 7 hold shell pointers that must stay
	// readable until they ran (and the release folds into the next ack).
	if (!SimDrawSplit::Enabled())
		deferredObjectDeleter.AckDrainedDestroys();

	// (2b) apply the draw-side Lua contract's boundary-deferred sim pokes
	// (LuaUnsyncedCtrl direct-sim-poke class under SplitDrawContract, PR 27a):
	// this is the pause window, so the writes land while sim state is mutable
	// and before the snapshot publish below makes them draw-visible. Empty
	// (and free) unless the contract flag queued something last draw frame.
	if (LuaSplitContract::DrainBoundaryApplies() > 0) {
		// the applied pokes mutated sim state outside any sim frame -- the
		// snapshot's frameNum-based due-check cannot see that (the same
		// class as net-driven team transfers, see CUnit::ChangedTeam)
		simSnapshot.MarkMutatedOutsideFrame();
	}

	// (2c) drain the map-layer mirrors (PR 28, DrawMapMirrors): copy the dirty
	// LOS/airLos/radar/sonar/jammer maps + the near-static terrain-type/
	// smooth-mesh/orig-heightmap layers into the draw-owned mirror, so the
	// positional-LOS + map-info serving twins never touch losHandler/mapInfo/
	// smoothGround/the synced orig-heightmap. BEFORE the snapshot publish so
	// the mirror, the object rows and the drawer containers all describe the
	// same completed sim frame (the same "no sim runs after here" boundary the
	// diff-gate memcmp pass relies on).
	drawMapMirrors.DrainAtBarrier();

	// (3) publish the observable-state snapshot for draw-side consumers
	// (contract in SimSnapshot.h; the team/player copy refreshes every call).
	// PR 43: Update() is the epoch PRODUCER half (extract into a free ring
	// slot + publish it as the newest-complete epoch).
	simSnapshot.Update();

	// (3a) PR 43: the epoch CONSUMER half -- acquire the newest complete
	// epoch (ref++), release the previously held one (ref--). Under the 43
	// lockstep this immediately follows every publish (a pointer rotation
	// with the double buffer's values). A release that drops a slot's
	// refcount to zero RETIRES that epoch: run the retirement hooks -- the
	// DeferredObjectDeleter release is rekeyed from "end of Draw" to "epoch
	// retirement" (§2.2), returning the pool slots of every shell acked under
	// the retired (or an earlier) epoch.
	{
		const uint64_t retiredEpoch = simSnapshot.AcquireNewestEpoch();

		if (SimDrawSplit::Enabled() && retiredEpoch != 0)
			deferredObjectDeleter.ReleaseRetired(retiredEpoch);
	}

	// (3b) refresh the draw-side command-queue copies (PR 27b serving batch 2,
	// LuaSnapshotServe). AFTER the publish, not with the resolve caches at
	// (1c): the refresh is generation-gated so queue copies and snapshot rows
	// always describe the same boundary -- and so the unit walk is skipped
	// entirely when the publish didn't swap. Also after (2b): drained boundary
	// pokes may have mutated queues, and their version bumps must be visible
	// to this refresh.
	LuaSnapshotServe::RefreshCommandQueues();

	// (3b') refresh the draw-side piece caches (PR 33 pieces/scripts serving).
	// AFTER the publish: generation-gated so the piece copies always describe
	// the same boundary as the published rows, and the walk is skipped when the
	// publish didn't swap. Captures final piece transforms via the live
	// accessors while the sim is parked (single-threaded pre-flip), so the
	// served values are bit-identical to the live callouts.
	LuaSnapshotServe::RefreshPieces();

	// (3b'') PR 43 §2.1: record the epoch's channel-version scalars into the
	// held ring slot (the old singleton cmdQueueCacheGeneration /
	// pieceCacheGeneration / mirror drained-version scalars become per-slot
	// state). Bookkeeping under the 43 lockstep -- the physical caches track
	// the newest epoch; per-slot COPIES arrive with the 44a producer flip.
	simSnapshot.SealEpochChannelVersions(
		LuaSnapshotServe::CmdQueueCacheEpoch(),
		LuaSnapshotServe::PieceCacheEpoch(),
		drawMapMirrors.DrainSerial());

	// (3c) evaluate the draw side's pending weapon trace-test queries (sim|draw
	// PR 35, Batch-3 sim-side query/reply channel). The sim is parked here, so
	// the exact live CWeapon predicates run against valid boundary-N sim state --
	// the same sanctioned live-read class as RefreshCommandQueues above; the
	// replies publish for the next draw frame. No-op (empty check) flag-off or
	// when the draw side enqueued nothing since the last barrier.
	LuaSnapshotServe::EvaluateTraceQueries();

	// (3d) evaluate the draw side's pending placement-test queries (sim|draw
	// PR 38e, same sim-side query/reply channel as (3c)). Sim parked, so the exact
	// live CGameHelper::TestUnitBuildSquare / MoveDef::TestMoveSquare /
	// ClosestBuildPos predicates run against valid boundary-N terrain/blocking
	// state; replies publish for the next draw frame. No-op (empty check) flag-off
	// or when the draw side enqueued nothing since the last barrier.
	LuaSnapshotServe::EvaluatePlacementQueries();

	// (3e) evaluate the context-cursor default-command query (sim|draw PR 44,
	// Gap B). Sim parked, so the deep-CAI GetDefaultCmd + GuiTraceRay run against
	// valid boundary-N sim state -- the same sanctioned live-read class as
	// RefreshCommandQueues / EvaluateTraceQueries; the reply publishes for the
	// next draw frame so guihandler->Update can read it sim-live. No-op unless
	// SetCursorIcon requested a re-eval since the last barrier.
	if (guihandler != nullptr)
		guihandler->EvaluateDefaultCmdQuery();

	// (4) TEST-ONLY (PR 17): when armed via /snapshotdiffgate, verify every
	// value the snapshot would serve bit-matches the live sim read at this
	// boundary. A single branch when unarmed.
	snapshotDiffGate.CheckBoundary();

	// (5) sim-frame boundary: unsynced work relocated out of CGame::SimFrame's
	// misplaced-work block (PR 11b). Runs once per batch of sim frames (>1
	// under fast-forward) at the sim-quiescent boundary, after ClientReadNet
	// drained its frame budget and after the render-event queue drain above.
	// None of these write synced state or consume gsRNG; their only sim-facing
	// effect is net messages, timing-equivalent to user input. The !skipping
	// gate preserves the former UpdateUnsynced early-return (movers never ran
	// during /skip fast-forward); lastSimFrame itself is only advanced by
	// UpdateUnsynced, exactly as before.
	if (!skipping && (lastSimFrame != gs->frameNum) && !gs->PreSimFrame()) {
		// last processed sim frame, for the movers' crossing checks
		const int prevSimFrame = lastSimFrame;

		// keep waitCommandsAI before sound->NewFrame: a wait release plays a
		// unit-reply sample that must land in the same emit budget as on master
		waitCommandsAI.Update(prevSimFrame);
		// sweep all expired buckets, not just frameNum's (see GeometricObjects.cpp)
		geometricObjects->Update();
		// reset the audio emit counter once per batch (never at raw draw rate)
		sound->NewFrame();

		playerHandler.Player(gu->myPlayerNum)->fpsController.SendStateUpdate();

		CTeamHighlight::Update(prevSimFrame);

		// dead-ghost pruning is draw-owned; run it after the batch's render-event
		// drain so ghosts created by the batch's destroy events exist first
		CUnitDrawer::UpdateGhostedBuildings();
	}

	// (6) heightmap dirty-rect drain: copy sim heightmap updates into the
	// unsynced heightmap + fire UnsyncedHeightMapUpdate (moved here from
	// CWorldDrawer::Update, which never ran while skipping -- keep that gate
	// so rects keep accumulating across /skip)
	if (!skipping) {
		readMap->UpdateDraw(firstUnsyncedHeightMapDrain);
		firstUnsyncedHeightMapDrain = false;
	}

	// (7) replay the sim phase's boundary-deferred unsynced dispatches (PR
	// 27b, UnsyncedBoundaryQueue.h): LuaUI/unsynced-handle events, Cob2Lua,
	// SendToUnsynced, ... -- in exact fire order, after the snapshot publish
	// so their callin bodies read this boundary's frame. Empty (and free)
	// unless the split flag deferred something since the last barrier. These
	// are sanctioned boundary callins, the same class as the Render* event
	// dispatches of step 1. A non-empty drain may have poked sim state
	// directly (live exception, post-snapshot) -- mark, or a no-new-frame
	// boundary serves stale rows.
	if (UnsyncedBoundaryQueue::Drain() > 0)
		simSnapshot.MarkMutatedOutsideFrame();

	// (8) split only: the relocated shell ack (see step 2). The drain window
	// closes first -- the ack poisons the shells the window's id->shell
	// fallback serves from (died-in-burst resolution for step 7's dispatches)
	if (SimDrawSplit::Enabled()) {
		SimDrawSplit::SetBoundaryShellWindow(false);
		// PR 43: the dispatch window has closed -- revert the published slot's
		// DEAD_THIS_BATCH marks (they only read as valid inside the window)
		// and clear the drawers' dead-retained render records (their shell
		// handles are about to be poisoned by the ack below; item 3b keeps
		// them resolvable exactly through the deferred-dispatch window)
		simSnapshot.ClearDeadThisBatch();
		CUnitDrawer::ClearDeadRetainedRecords();
		CFeatureDrawer::ClearDeadRetainedRecords();
		renderEventQueue.ClearBoundaryDeadShells();
		// PR 43: the split ack tags the shells with the epoch whose record
		// dispatch just completed; their pool RELEASE is keyed to that epoch's
		// retirement (barrier step 3a), replacing the end-of-Draw ReleaseAcked
		deferredObjectDeleter.AckDrainedDestroysEpoch(simSnapshot.EpochId());
	}
}

// PR 27b: see barrier step (1b). Also used by the pool-pressure valve
// service in AcquireSimPause, which flushes destroys mid-frame.
void CGame::DeliverBoundaryDeaths()
{
	if (!SimDrawSplit::Enabled())
		return;

	const auto& deadUnits = renderEventQueue.BoundaryDestroyedUnits();
	const auto& deadProjs = renderEventQueue.BoundaryDestroyedProjectiles();

	GL::LightHandler* groundLights = (readMap != nullptr && readMap->GetGroundDrawer() != nullptr) ? readMap->GetGroundDrawer()->GetLightHandler() : nullptr;
	GL::LightHandler* modelLights = CModelDrawerConcept::GetLightHandler();

	for (const CUnit* u: deadUnits) {
		CUnit* unit = const_cast<CUnit*>(u);

		// Gap A: the death-path group prune deferred out of CUnit::PreDestruct
		// (draw-owned UI control-groups; team is unchanged on death).
		unit->SetGroup(nullptr);

		selectedUnitsHandler.DependentDied(unit);
		waitCommandsAI.DeliverBoundaryDeath(unit);

		if (groundLights != nullptr)
			groundLights->DeliverBoundaryDeath(unit);
		if (modelLights != nullptr)
			modelLights->DeliverBoundaryDeath(unit);
	}

	for (const CProjectile* p: deadProjs) {
		if (groundLights != nullptr)
			groundLights->DeliverBoundaryDeath(p);
		if (modelLights != nullptr)
			modelLights->DeliverBoundaryDeath(p);
	}

	renderEventQueue.ClearBoundaryDestroys();
}

// ---------------------------------------------------------------------------
// PR 27b: the sim thread (active only with SimDrawSplit=1)
// ---------------------------------------------------------------------------

// joined by JoinSimThread; file-static so Game.h stays include-light
static spring::thread simNetThread;

// when the current pause window parked the sim thread (RequestPause return);
// ReleaseSimPause emits it as the /debug frame grapher's "Parked" slice
static spring_time simPauseBeginTime;

__FORCE_ALIGN_STACK__
void CGame::SimThreadProc()
{
	Threading::SetThreadName("sim");
	// registers thread controls so watchdog/crash dumps can suspend us
	Threading::SetSimThread();

	// not needed to maintain sync (precision flags are per-process) but fpu
	// exceptions are per-thread (the GameLoadThread pattern; sync risk #1)
	streflop::streflop_init<streflop::Simple>();

	Watchdog::RegisterThread(WDT_SIM);
	// simRunning was already set by SpawnSimThread BEFORE the thread
	// existed: the pause handshake must see the thread from the very first
	// Draw, or the barrier drains unparked against a consuming sim thread
	// (the TSan-found spawn race)
	assert(SimDrawSplit::SimThreadRunning());

	try {
		while (!SimDrawSplit::SimThreadExitRequested() && !gu->globalQuit) {
			Watchdog::ClearTimer(WDT_SIM);

			// the frame-edge park point (the barrier handshake); the second
			// one sits between packets inside ClientReadNet
			SimDrawSplit::YieldIfPauseRequested();

			if (SimDrawSplit::SimThreadExitRequested() || gu->globalQuit)
				break;

			good_fpu_control_registers("CGame::SimThreadProc");

			{
				// exactly the bracket the single-threaded path wraps around
				// ClientReadNet in CGame::Update
				renderEventQueue.BeginSimPhase();
				SimDrawSplit::ScopedSimPhase simPhase;

				ENTER_SYNCED_CODE();
				ClientReadNet(); // issues new SimFrame()s
				LEAVE_SYNCED_CODE();
			}

			// keep consuming without a nap while budget and packets remain
			// (ClientReadNet returns on its per-call wall-time cap during
			// catch-up; napping there throttles fast-forward to ~half speed)
			if (msgProcTimeLeft > 0.0f && clientNet->Peek(0) != nullptr)
				continue;

			// bounded nap; woken early by a pause request or exit
			SimDrawSplit::SimIdleWait();
		}
	} CATCH_SPRING_ERRORS

	SimDrawSplit::SetSimThreadRunning(false);
	Watchdog::DeregisterThread(WDT_SIM);
}

void CGame::SpawnSimThread()
{
	if (simNetThread.joinable())
		return;

	// the sim thread interleaves Peek/GetData with main-thread Sends; the
	// two unlocked queue ops take the connection lock from here on
	clientNet->SetThreadSafeQueueOps(true);

	SimDrawSplit::ResetSimThreadExit();
	// set BEFORE the thread exists (see the assert in SimThreadProc)
	SimDrawSplit::SetSimThreadRunning(true);
	simNetThread = spring::thread(std::bind(&CGame::SimThreadProc, this));

	LOG("[Game::%s] sim thread spawned (SimDrawSplit=1)", __func__);
}

void CGame::JoinSimThread()
{
	if (!simNetThread.joinable())
		return;

	SimDrawSplit::RequestSimThreadExit();
	simNetThread.join();
	Threading::ClearSimThread();

	LOG("[Game::%s] sim thread joined", __func__);
}

void CGame::AcquireSimPause()
{
	if (!SimDrawSplit::Enabled() || !SimDrawSplit::SimThreadRunning())
		return;

	{
		// how long the draw side waits for the sim to reach a frame edge
		// (the other half of the boundary cost; worst case one sim frame)
		SCOPED_TIMER("Misc::SimPauseWait");
		SimDrawSplit::RequestPause();
	}

	// the sim thread is parked (frame edge or valve) from here to ReleasePause
	simPauseBeginTime = spring_now();

	// pool-pressure valve service (the PR-13 valve became "sim waits for the
	// boundary" under the split): the sim parked MID-frame out of pool
	// headroom. Give its pages back: dispatch pending records in place
	// (Flush keeps the deferral window open), deliver boundary deaths and
	// run the deferred unsynced dispatches while the shells are readable,
	// then destruct + release. Loops in case pressure recurs before the
	// frame edge.
	while (SimDrawSplit::ParkedAtValve()) {
		// same live-read legality as the barrier (sim parked mid-frame)
		LuaSplitContract::ScopedLiveException valveLive;

		// same drain-window bracket as the barrier (steps 0 / 8)
		SimDrawSplit::SetBoundaryShellWindow(true);

		modelLoader.ServiceQueuedUploads();
		renderEventQueue.Flush();
		DeliverBoundaryDeaths();

		// keep the effect-container snapshot in step with the flushed
		// registrations. SCOPE-1 (plan PR 39) + PR 40: the unit/feature/
		// projectile id->object caches are DELETED — those drawers resolve ids
		// through a producer-captured deferred-safe handle (no per-barrier
		// rebuild); the projectile drawer still needs the ground-flash /
		// flying-piece container copies.
		projectileDrawer->SnapshotEffectContainers();

		// generation-gated no-op today (the valve does not republish the
		// snapshot, and queue copies must stay boundary-consistent with the
		// published rows); here so a future valve-side republish stays covered
		LuaSnapshotServe::RefreshCommandQueues();

		// see barrier step 7: a non-empty drain may have poked sim state
		if (UnsyncedBoundaryQueue::Drain() > 0)
			simSnapshot.MarkMutatedOutsideFrame();

		SimDrawSplit::SetBoundaryShellWindow(false);
		// PR 43 (3b): the valve's Flush dispatched destroy records too -- the
		// drawers retained those records; clear them before the ack poisons
		// their shell handles (the valve does not republish, so no
		// DEAD_THIS_BATCH marks exist here)
		CUnitDrawer::ClearDeadRetainedRecords();
		CFeatureDrawer::ClearDeadRetainedRecords();
		renderEventQueue.ClearBoundaryDeadShells();

		// PR 43: the valve must free pages IMMEDIATELY (the sim is parked
		// mid-frame out of pool headroom), so it keeps the ack+release-all
		// pair -- the documented lockstep-degenerate form of "sim waits for
		// epoch retirement" (§2.2: the epoch retires in place). Poisoned
		// shells are never legally read, so releasing shells whose epoch has
		// not formally retired only changes pool-return timing (address-blind).
		deferredObjectDeleter.AckDrainedDestroys();
		deferredObjectDeleter.ReleaseAcked();
		SimDrawSplit::ResumeFromValve();
	}

	simPauseHeld = true;
}

void CGame::ReleaseSimPause()
{
	if (!simPauseHeld)
		return;

	simPauseHeld = false;
	SimDrawSplit::ReleasePause(gs->frameNum);

	// gate telemetry for the /debug frame grapher (sim-row "Parked" slice)
	eventHandler.DbgTimingInfo(TIMING_SIM_PARKED, simPauseBeginTime, spring_now());
}

// §4.5 pause-surface telemetry: per-site count of parks that actually engaged
// (re-parked the running sim). Relaxed atomics -- read/written from the draw
// thread today, but kept atomic so the eventual 44b consumers stay clean.
static std::array<std::atomic<uint64_t>, size_t(CGame::SimPauseSite::COUNT)> simPauseSiteCounts = {};

void CGame::DumpSimPauseSurvey()
{
	static const char* siteNames[size_t(SimPauseSite::COUNT)] = {
		"LIFECYCLE", "GUI_TRY_TARGET", "GUI_TEST_BUILDSQUARE", "GUI_GET_COMMAND",
		"GUI_GET_BUILDPOS", "GUI_DRAW_MAPSTUFF", "GUI_GET_DEFAULT_CMD",
		"MOUSE_RELEASE", "MINIMAP_FRUSTUM", "LUA_SEND_COMMANDS", "LUA_GIVE_ORDER",
	};

	uint64_t total = 0;
	for (auto& c: simPauseSiteCounts)
		total += c.load(std::memory_order_relaxed);

	if (total == 0)
		return; // split off / never parked mid-gameplay

	LOG("[SimPauseSurvey] mid-gameplay ScopedExternalSimPause parks that engaged "
	    "(re-parked the running sim), by site:");
	for (size_t i = 0; i < size_t(SimPauseSite::COUNT); ++i) {
		const uint64_t n = simPauseSiteCounts[i].load(std::memory_order_relaxed);
		if (n > 0)
			LOG("[SimPauseSurvey]   %-22s %llu", siteNames[i], (unsigned long long)n);
	}
}

CGame::ScopedExternalSimPause::ScopedExternalSimPause(SimPauseSite site)
{
	if (game == nullptr || game->simPauseHeld)
		return;

	game->AcquireSimPause();
	acquired = game->simPauseHeld;

	// telemetry (§4.5): only count parks that actually engaged (a nested
	// bracket returns above with acquired=false and is not a real re-park)
	if (acquired)
		simPauseSiteCounts[size_t(site)].fetch_add(1, std::memory_order_relaxed);
}

CGame::ScopedExternalSimPause::~ScopedExternalSimPause()
{
	if (acquired && game != nullptr)
		game->ReleaseSimPause();
}


bool CGame::Draw() {
	const spring_time currentTimePreBarrier = spring_now();

	// PR 27b: park the sim thread at a frame edge (servicing pool-pressure
	// valve parks along the way); no-op when the split is off. The pause
	// spans the barrier + UpdateUnsynced through the drawer extraction.
	AcquireSimPause();

	{
		// the barrier's own dispatches (Render* events, deferred unsynced
		// callins) legally read live sim state -- the sim is parked; the
		// widget callins later in the frame stay contract-served
		LuaSplitContract::ScopedLiveException barrierLive;
		SimDrawBarrier();
	}

	// gate telemetry for the /debug frame grapher (draw-row "Gate" slice):
	// the pause-wait + valve service + barrier span no other category covers
	if (SimDrawSplit::Enabled())
		eventHandler.DbgTimingInfo(TIMING_BARRIER, currentTimePreBarrier, spring_now());

	// everything from here to the end of Draw is draw-thread context under
	// the split contract (PR 27a): unsynced Lua ran below this line executes
	// on the draw thread at 27b while sim advances. The barrier above is
	// deliberately OUTSIDE the window -- it is the sim-pause bracket, where
	// live reads (the Render* event dispatches) stay legal.
	LuaSplitContract::ScopedDrawWindow splitContractWindow;

	const spring_time currentTimePreUpdate = spring_gettime();

	if (UpdateUnsynced(currentTimePreUpdate)) {
		// early-out paths (skipping 2Hz redraw etc.) exit before the normal
		// release point inside UpdateUnsynced
		ReleaseSimPause();
		return false;
	}

	RmlGui::Update();
	const spring_time currentTimePreDraw = spring_gettime();

	SCOPED_SPECIAL_TIMER("Draw");
	SCOPED_GL_DEBUGGROUP("Draw");
	globalRendering->SetGLTimeStamp(CGlobalRendering::FRAME_REF_TIME_QUERY_IDX);

	SetDrawMode(gameNormalDraw);

	// Bind per-drawFrame UBO
	UniformConstants::GetInstance().Bind();

	{
		SCOPED_TIMER("Draw::DrawGenesis");
		eventHandler.DrawGenesis();
	}

	if (!globalRendering->active) {
		spring_sleep(spring_msecs(10));

		// return early if and only if less than 30K milliseconds have passed since last draw-frame
		// so we force render two frames per minute when minimized to clear batches and free memory
		// don't need to mess with globalRendering->active since only mouse-input code depends on it
		if ((currentTimePreDraw - lastDrawFrameTime).toSecsi() < 30)
			return false;
	}

	if (globalRendering->drawDebug) {
		const float deltaFrameTime = (currentTimePreUpdate - lastSimFrameTime).toMilliSecsf();
		const float deltaNetPacketProcTime  = (currentTimePreUpdate - lastNetPacketProcessTime ).toMilliSecsf();
		const float deltaReceivedPacketTime = (currentTimePreUpdate - lastReceivedNetPacketTime).toMilliSecsf();
		const float deltaSimFramePacketTime = (currentTimePreUpdate - lastSimFrameNetPacketTime).toMilliSecsf();

		const float currTimeOffset = globalRendering->timeOffset;
		static float lastTimeOffset = globalRendering->timeOffset;
		static int lastGameFrame = gs->frameNum;

		static const char* minFmtStr = "assert(CTO >= 0.0f) failed (SF=%u : DF=%u : CTO=%f : WSF=%f : DT=%fms : DLNPPT=%fms | DLRPT=%fms | DSFPT=%fms : NP=%u)";
		static const char* maxFmtStr = "assert(CTO <= 1.3f) failed (SF=%u : DF=%u : CTO=%f : WSF=%f : DT=%fms : DLNPPT=%fms | DLRPT=%fms | DSFPT=%fms : NP=%u)";

		// CTO = MILLISECSF(CT - LSFT) * WSF = MILLISECSF(CT - LSFT) * (SFPS * 0.001)
		// AT 30Hz LHS (MILLISECSF(CT - LSFT)) SHOULD BE ~33ms, RHS SHOULD BE ~0.03
		assert(currTimeOffset >= 0.0f);

		if (currTimeOffset < 0.0f) LOG_L(L_DEBUG, minFmtStr, gs->frameNum, globalRendering->drawFrame, currTimeOffset, globalRendering->weightedSpeedFactor, deltaFrameTime, deltaNetPacketProcTime, deltaReceivedPacketTime, deltaSimFramePacketTime, clientNet->GetNumWaitingServerPackets());
		if (currTimeOffset > 1.3f) LOG_L(L_DEBUG, maxFmtStr, gs->frameNum, globalRendering->drawFrame, currTimeOffset, globalRendering->weightedSpeedFactor, deltaFrameTime, deltaNetPacketProcTime, deltaReceivedPacketTime, deltaSimFramePacketTime, clientNet->GetNumWaitingServerPackets());

		// test for monotonicity, normally should only fail
		// when SimFrame() advances time or if simframe rate
		// changes
		if (lastGameFrame == gs->frameNum && currTimeOffset < lastTimeOffset)
			LOG_L(L_DEBUG, "assert(CTO >= LTO) failed (SF=%u : DF=%u : CTO=%f : LTO=%f : WSF=%f : DT=%fms)", gs->frameNum, globalRendering->drawFrame, currTimeOffset, lastTimeOffset, globalRendering->weightedSpeedFactor, deltaFrameTime);

		lastTimeOffset = currTimeOffset;
		lastGameFrame = gs->frameNum;
	}

	//FIXME move both to UpdateUnsynced?
	CTeamHighlight::Enable(spring_tomsecs(currentTimePreDraw));
	{
		minimap->Update();

		// note: neither this call nor DrawWorld can be made conditional on minimap->GetMaximized()
		// minimap never covers entire screen when maximized unless map aspect-ratio matches screen
		// (unlikely);
		worldDrawer.GenerateIBLTextures();

		// restore back to the default FBO / Viewport
		if (FBO::IsSupported())
			FBO::Unbind();
		camera->LoadViewport();

		worldDrawer.Draw();
		worldDrawer.ResetMVPMatrices();
	}

	{
		SCOPED_TIMER("Draw::Screen");
		SCOPED_GL_DEBUGGROUP("Draw::Screen");
		if (CUnitDrawer::UseScreenIcons())
			unitDrawer->DrawUnitIconsScreen();

		eventHandler.DrawScreenEffects();

		hudDrawer->Draw((gu->GetMyPlayer())->fpsController.GetControllee());
		debugDrawerAI->Draw();

		DrawInputReceivers();
		DrawInputText();
		DrawInterfaceWidgets();
		RmlGui::RenderFrame();
		mouse->DrawCursor();

		eventHandler.DrawScreenPost();
	}

	glEnable(GL_DEPTH_TEST);
	glLoadIdentity();

	if (videoCapturing->AllowRecord()) {
		videoCapturing->SetLastFrameTime(globalRendering->lastFrameTime = 1000.0f / GAME_SPEED);
		// does nothing unless StartCapturing has also been called via /createvideo (Windows-only)
		videoCapturing->RenderFrame();
	}

	SetDrawMode(gameNotDrawing);
	CTeamHighlight::Disable();

	const spring_time currentTimePostDraw = spring_gettime();
	const spring_time currentFrameDrawTime = currentTimePostDraw - currentTimePreDraw;
	gu->avgDrawFrameTime = mix(gu->avgDrawFrameTime, currentFrameDrawTime.toMilliSecsf(), 0.05f);

	eventHandler.DbgTimingInfo(TIMING_VIDEO, currentTimePreDraw, currentTimePostDraw);
	globalRendering->SetGLTimeStamp(CGlobalRendering::FRAME_END_TIME_QUERY_IDX);

	lastDrawFrameTime = currentTimePostDraw;

	// return the poisoned slots of this Draw's acked destroys to the pools.
	// PR 27b/43: under the split the pools are sim-owned and the sim thread is
	// running again here -- the release is keyed to EPOCH RETIREMENT instead
	// (barrier step 3a: DeferredObjectDeleter::ReleaseRetired when the ring
	// retires the epoch the shells were acked under)
	if (!SimDrawSplit::Enabled())
		deferredObjectDeleter.ReleaseAcked();

	// safety net for any Draw exit path that skipped the normal release
	// point (no-op when already released)
	ReleaseSimPause();

	return true;
}


void CGame::DrawInputReceivers()
{

	glEnable(GL_TEXTURE_2D);

	if (!hideInterface) {
		{
			SCOPED_TIMER("Draw::Screen::InputReceivers");
			CInputReceiver::DrawReceivers();
		}
		{
			// this has MANUAL ordering, draw it last (front-most)
			SCOPED_TIMER("Draw::Screen::DrawScreen");
			SCOPED_GL_DEBUGGROUP("Draw::Screen::DrawScreen");
			luaInputReceiver->Draw();
		}
	} else {
		SCOPED_TIMER("Draw::Screen::Minimap");
		SCOPED_GL_DEBUGGROUP("Draw::Screen::Minimap");

		if (globalRendering->dualScreenMode) {
			// minimap is on its own screen, so always draw it
			minimap->Draw();
		}
	}

	glEnable(GL_TEXTURE_2D);
}

void CGame::DrawInterfaceWidgets()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (hideInterface)
		return;

	smallFont->Begin();

	#define KEY_FONT_FLAGS (FONT_SCALE | FONT_CENTER | FONT_NORM)
	#define INF_FONT_FLAGS (FONT_RIGHT | FONT_SCALE | FONT_NORM | (FONT_OUTLINE * guihandler->GetOutlineFonts()))

	if (showClock) {
		static constexpr float4 white(0.9f, 0.9f, 0.9f, 1.0f);
		smallFont->SetColors(&white, NULL);

		const int seconds = (gs->frameNum / GAME_SPEED);
		if (seconds < 3600) {
			smallFont->glFormat(0.99f, 0.94f, 1.0f, INF_FONT_FLAGS, "%02i:%02i", seconds / 60, seconds % 60);
		} else {
			smallFont->glFormat(0.99f, 0.94f, 1.0f, INF_FONT_FLAGS, "%02i:%02i:%02i", seconds / 3600, (seconds / 60) % 60, seconds % 60);
		}
	}

	if (showFPS) {
		static constexpr float4 yellow(1.0f, 1.0f, 0.25f, 1.0f);
		smallFont->SetColors(&yellow,NULL);
		smallFont->glFormat(0.99f, 0.92f, 1.0f, INF_FONT_FLAGS, "%.0f", globalRendering->FPS);
	}

	if (showSpeed) {
		const float4 speedcol(1.0f, gs->speedFactor < gs->wantedSpeedFactor * 0.99f ? 0.25f : 1.0f, 0.25f, 1.0f);
		smallFont->SetColors(&speedcol, NULL);
		smallFont->glFormat(0.99f, 0.90f, 1.0f, INF_FONT_FLAGS, "%2.2f", gs->speedFactor);
	}

	CPlayerRosterDrawer::Draw();
	smallFont->End();
}


void CGame::ParseInputTextGeometry(const string& geo)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (geo == "default") { // safety
		ParseInputTextGeometry("0.26 0.73 0.02 0.028");
		return;
	}

	float2 pos;
	float2 size;

	if (sscanf(geo.c_str(), "%f %f %f %f", &pos.x, &pos.y, &size.x, &size.y) == 4) {
		gameTextInput.SetPos(pos.x, pos.y);
		gameTextInput.SetSize(size.x, size.y);
		gameTextInput.ViewResize();

		configHandler->SetString("InputTextGeo", geo);
	}
}


void CGame::DrawInputText()
{
	RECOIL_DETAILED_TRACY_ZONE;
	gameTextInput.Draw();
}


void CGame::StartPlaying()
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(!playing);
	playing = true;

	{
		lastReadNetTime = spring_gettime();

		gu->startTime = gu->gameTime;
		gu->myTeam = gu->GetMyPlayer()->team;
		gu->myAllyTeam = teamHandler.AllyTeam(gu->myTeam);
	}

	// PR 27b: both are draw/UI-owned (the setup-drawer singleton and the
	// unsynced LuaUI state); StartPlaying fires from net-message handling
	if (SimDrawSplit::DeferUnsyncedNow()) {
		UnsyncedBoundaryQueue::Defer([]() {
			GameSetupDrawer::Disable();
			CLuaUI::UpdateTeams();
		});
	} else {
		GameSetupDrawer::Disable();
		CLuaUI::UpdateTeams();
	}

	teamHandler.SetDefaultStartPositions(gameSetup);

	if (saveFileHandler == nullptr)
		eventHandler.GameStart();
}

static const char* const tracingSimFrameName = "SimFrame";

void CGame::SimFrame() {
	// tag every zone (incl. unsynced widget callins run from here) as sim-budget,
	// so the profiler can split sim vs draw/update time
	ScopedSimFramePhase simPhase;

	ENTER_SYNCED_CODE();
	ASSERT_SYNCED(gsRNG.GetGenState());

	DumpRNG(-1, -1);

	good_fpu_control_registers("CGame::SimFrame");

	FrameMarkStart(tracingSimFrameName);

	// note: starts at -1, first actual frame is 0
	gs->frameNum += 1;
#ifdef SYNC_HISTORY
	CSyncChecker::NewGameFrame();
#endif
	lastFrameTime = spring_gettime();
	// This is not very ideal, as the timeoffset of each new draw frame is also calculated from this
	// with a strange side effect: if the timeOffset was a high number, like 0.9, then this will force the next draw frame to have an offset of 0.0x
	// What this means, is that in the case where we have frames to spare, and and over rendering, then the following can happen at 60hz:
	// simframe
	// drawframe timeOffset ~ 0.0
	// drawframe timeoffset ~ 0.5
	// drawframe timeoffset ~ 1.0 (1 extra draw!)
	// simframe
	// drawframe timeoffset ~ 0.0 // THIS is the problematic case, as visually, this frame is 'near identical' to the previously drawn one!
	// simframe
	// drawframe timeoffset ~ 0.0
	// drawframe timeoffset ~ 0.5
	// simframe
	// etc...
	// See SmoothTimeOffset for a fix to this


#if 0
	if (globalRendering->timeOffset > 1.0)
		lastFrameTime += spring_time::fromNanoSecs(static_cast<int64_t>((globalRendering->timeOffset - 1.0f) / globalRendering->weightedSpeedFactor * std::int64_t(1e6)));

	if (globalRendering->timeOffset < 0.0)
		lastFrameTime += spring_time::fromNanoSecs(static_cast<int64_t>((globalRendering->timeOffset       ) / globalRendering->weightedSpeedFactor * std::int64_t(1e6)));
#endif

	// clear allocator statistics periodically
	// note: allocator itself should do this (so that
	// stats are reliable when paused) but see LuaUser
	spring_lua_alloc_update_stats((gs->frameNum % GAME_SPEED) == 0);

	if (!skipping) {
		// eoh->Update stays on the sim path: its single-player cheat callbacks
		// mutate synced state, and AIs assume once-per-sim-frame Update(frame)
		// cadence. The rest of this block was unsynced and moved to
		// CGame::UpdateUnsynced / Draw per PR 11b (11a classification table).
		eoh->Update();
	}

	// everything from here is simulation
	{
		SCOPED_SPECIAL_TIMER("Sim");

		// Lua unit scripts change piece positions and orientations in eventHandler.GameFrame(gs->frameNum);
		// so we need to save the previous unit state before it happened
		unitHandler.UpdatePreFrame();
		featureHandler.UpdatePreFrame();

		{
			SCOPED_TIMER("Sim::GameFrame");

			// keep garbage-collection rate tied to sim-speed
			// (fixed 30Hz gc is not enough while catching up)
			// PR 27b: under the split the sim phase GCs only the synced
			// lua_States it owns; the main-thread timed job covers the rest
			if (luaGCControl == 0)
				eventHandler.CollectGarbage(false, SimDrawSplit::Enabled() ? CEventHandler::GC_SYNCED_ONLY : CEventHandler::GC_ALL);

			eventHandler.GameFrame(gs->frameNum);
		}

		helper->Update();
		readMap->Update();
		smoothGround.UpdateSmoothMesh();
		mapDamage->Update();
		unitHandler.Update();
		pathManager->Update();
		projectileHandler.Update();
		featureHandler.Update();
		{
			/* The default GAME_SPEED is 30, which doesn't divide 1000 well,
			 * so scripts will perceive 990ms per second. But this is fine,
			 * since doing "29th February" style of extra counting would be
			 * disruptive to sleeps that assume a constant tick length while
			 * not being otherwise perceptible since most animations don't
			 * run that long. */
			static constexpr int tickMs = 1000 / GAME_SPEED;

			SCOPED_TIMER("Sim::Script");
			unitScriptEngine->Tick(tickMs);

			unitHandler.UpdatePostAnimation();
		}
		envResHandler.Update();
		losHandler->Update();
		// UpdateGhostedBuildings() (dead-ghost pruning) moved to the unsynced
		// boundary in CGame::UpdateUnsynced (PR 11b): it is draw-owned state and
		// must run after the batch's render-event drain, not on the sim path.
		interceptHandler.Update(false);

		teamHandler.GameFrame(gs->frameNum);
		playerHandler.GameFrame(gs->frameNum);
		eventHandler.GameFramePost(gs->frameNum);
	}

	lastSimFrameTime = spring_gettime();
	gu->avgSimFrameTime = mix(gu->avgSimFrameTime, (lastSimFrameTime - lastFrameTime).toMilliSecsf(), 0.05f);
	gu->avgSimFrameTime = std::max(gu->avgSimFrameTime, 0.01f);

	eventHandler.DbgTimingInfo(TIMING_SIM, lastFrameTime, lastSimFrameTime);

	FrameMarkEnd(tracingSimFrameName);

	// sample per-sim-frame profiler self/inclusive/count for an active /profiledump
	CTimeProfiler::GetInstance().DumpFrame(gs->frameNum);
	// sample per-sim-frame boundary-size stats for an active /boundarydump
	BoundaryStats::SampleFrame(gs->frameNum);
	// hash this completed sim frame's SimSnapshot for an active /snaphashdump
	// (per sim frame, not per draw frame, so fast-forward doesn't skip frames)
	simSnapshot.HashCompletedFrame(gs->frameNum);

	#ifdef HEADLESS
	{
		const float msecMaxSimFrameTime = 1000.0f / (GAME_SPEED * gs->wantedSpeedFactor);
		const float msecDifSimFrameTime = (lastSimFrameTime - lastFrameTime).toMilliSecsf();
		// multiply by 0.5 to give unsynced code some execution time (50% of our sleep-budget)
		const float msecSleepTime = (msecMaxSimFrameTime - msecDifSimFrameTime) * 0.5f;

		if (msecSleepTime > 0.0f) {
			spring_sleep(spring_msecs(msecSleepTime));
		}
	}
	#endif

	// useful for desync-debugging (enter instead of -1 start & end frame of the range you want to debug)
	DumpState(-1, -1, 1, std::nullopt);

	ASSERT_SYNCED(gsRNG.GetGenState());
	LEAVE_SYNCED_CODE();
}


void CGame::GameEnd(const std::vector<unsigned char>& winningAllyTeams, bool timeout)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (gameOver)
		return;

	if (timeout) {
		// client timed out, don't send anything (in theory the GAMEOVER
		// message should not be able to reach the server if connection
		// is lost, but in practice it can get through --> timeout check
		// needs work)
		if (!configHandler->GetBool("GameEndOnConnectionLoss")) {
			LOG_L(L_WARNING, "[%s] lost connection to server; continuing game", __func__);
			clientNet->InitLocalClient();
			return;
		}

		// Force the connection to close if it hasn't been already to avoid the chance of starting the game in an
		// desynced state.
		clientNet->Close();

		LOG_L(L_ERROR, "[%s] lost connection to server; terminating game", __func__);
	} else {
		// pass the winner info to the host in the case it's a dedicated server
		clientNet->Send(CBaseNetProtocol::Get().SendGameOver(gu->myPlayerNum, winningAllyTeams));
	}


	gameOver = true;
	eventHandler.GameOver(winningAllyTeams);

	// PR 27b: the end-game box is an agui/UI object; GameEnd can fire from
	// net-message handling (sim thread under the split) -- boundary-defer
	if (SimDrawSplit::DeferUnsyncedNow()) {
		UnsyncedBoundaryQueue::Defer([winningAllyTeams]() { CEndGameBox::Create(winningAllyTeams); });
	} else {
		CEndGameBox::Create(winningAllyTeams);
	}
#ifdef    HEADLESS
	CTimeProfiler::GetInstance().PrintProfilingInfo();
#endif // HEADLESS

	CDemoRecorder* record = clientNet->GetDemoRecorder();

	if (!record->IsValid())
		return;

	// Write CPlayer::Statistics and CTeam::Statistics to demo
	// TODO: move this to a method in CTeamHandler
	const int numPlayers = playerHandler.ActivePlayers();
	const int numTeams = teamHandler.ActiveTeams() - int(gs->useLuaGaia);

	record->SetTime(gs->frameNum / GAME_SPEED, (int)gu->gameTime);
	record->InitializeStats(numPlayers, numTeams);
	// pass the list of winners
	record->SetWinningAllyTeams(winningAllyTeams);

	// tell everybody about our APM, it's the most important statistic
	if (!timeout)
		clientNet->Send(CBaseNetProtocol::Get().SendPlayerStat(gu->myPlayerNum, playerHandler.Player(gu->myPlayerNum)->currentStats));

	for (int i = 0; i < numPlayers; ++i) {
		record->SetPlayerStats(i, playerHandler.Player(i)->currentStats);
	}
	for (int i = 0; i < numTeams; ++i) {
		const CTeam* team = teamHandler.Team(i);
		record->SetTeamStats(i, team->statHistory);
		if (!timeout)
			clientNet->Send(CBaseNetProtocol::Get().SendTeamStat(team->teamNum, team->GetCurrentStats()));
	}
}

void CGame::SendNetChat(std::string message, int destination)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (message.empty())
		return;

	if (destination == -1) {
		// overwrite
		destination = ChatMessage::TO_EVERYONE;

		if ((message.length() >= 2) && (message[1] == ':')) {
			switch (tolower(message[0])) {
				case 'a': {
					destination = ChatMessage::TO_ALLIES;
					message = message.substr(2);
				} break;
				case 's': {
					destination = ChatMessage::TO_SPECTATORS;
					message = message.substr(2);
				} break;
				default: {
				} break;
			}
		}
	}

	ChatMessage buf(gu->myPlayerNum, destination, message);
	clientNet->Send(buf.Pack());
}


// PR 27b: the chat-notification sample plays through the UI sound channel
// (main-thread/audio-owned); chat arrives via net-message handling, which
// runs on the sim thread under the split
static void PlayDeferrableChatSound(int soundID)
{
	if (SimDrawSplit::DeferUnsyncedNow()) {
		UnsyncedBoundaryQueue::Defer([soundID]() { Channels::UserInterface->PlaySample(soundID, 5); });
	} else {
		Channels::UserInterface->PlaySample(soundID, 5);
	}
}

void CGame::HandleChatMsg(const ChatMessage& msg)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if ((msg.fromPlayer < 0) ||
		((msg.fromPlayer >= playerHandler.ActivePlayers()) &&
			(static_cast<unsigned int>(msg.fromPlayer) != SERVER_PLAYER))) {
		return;
	}

	const std::string& s = msg.msg;

	if (!s.empty()) {
		CPlayer* player = (msg.fromPlayer >= 0 && static_cast<unsigned int>(msg.fromPlayer) == SERVER_PLAYER) ? nullptr : playerHandler.Player(msg.fromPlayer);
		const bool myMsg = (msg.fromPlayer == gu->myPlayerNum);

		string label;
		if (!player) {
			label = "> ";
		} else if (player->spectator) {
			if (player->isFromDemo)
				// make clear that the message is from the replay
				label = "[" + player->name + " (replay)" + "] ";
			else
				// its from a spectator not from replay
				label = "[" + player->name + "] ";
		} else {
			// players are always from a replay (if its a replay and not a game)
			label = "<" + player->name + "> ";
		}

		/*
		- If you're spectating you always see all chat messages.
		- If you're playing you see:
		- TO_ALLIES-messages sent by allied players,
		- TO_SPECTATORS-messages sent by yourself,
		- TO_EVERYONE-messages from players,
		- TO_EVERYONE-messages from spectators only if noSpectatorChat is off!
		- private messages if they are for you ;)
		*/

		if (msg.destination == ChatMessage::TO_ALLIES && player) {
			const int msgAllyTeam = teamHandler.AllyTeam(player->team);
			const bool allied = teamHandler.Ally(msgAllyTeam, gu->myAllyTeam);
			if (gu->spectating || (allied && !player->spectator)) {
				LOG("%sAllies: %s", label.c_str(), s.c_str());
				PlayDeferrableChatSound(chatSound);
			}
		}
		else if (msg.destination == ChatMessage::TO_SPECTATORS) {
			if (gu->spectating || myMsg) {
				LOG("%sSpectators: %s", label.c_str(), s.c_str());
				PlayDeferrableChatSound(chatSound);
			}
		}
		else if (msg.destination == ChatMessage::TO_EVERYONE) {
			const bool specsOnly = noSpectatorChat && (player && player->spectator);
			if (gu->spectating || !specsOnly) {
				if (specsOnly) {
					LOG("%sSpectators: %s", label.c_str(), s.c_str());
				} else {
					LOG("%s%s", label.c_str(), s.c_str());
				}
				PlayDeferrableChatSound(chatSound);
			}
		}
		else if ((msg.destination < playerHandler.ActivePlayers()) && player)
		{	// player -> spectators and spectator -> player PMs should be forbidden
			// player <-> player and spectator <-> spectator are allowed
			// all replay whispers can be read when watching it
			if (player->isFromDemo) {
				LOG("%s whispered %s: %s", label.c_str(), playerHandler.Player(msg.destination)->name.c_str(), s.c_str());
			} else if (msg.destination == gu->myPlayerNum && player->spectator == gu->spectating) {
				LOG("%sPrivate: %s", label.c_str(), s.c_str());
				PlayDeferrableChatSound(chatSound);
			}
			else if (player->playerNum == gu->myPlayerNum)
			{
				LOG("You whispered %s: %s", playerHandler.Player(msg.destination)->name.c_str(), s.c_str());
			}
		}
	}

	eoh->SendChatMessage(msg.msg.c_str(), msg.fromPlayer);
}



void CGame::StartSkip(int toFrame) {
	RECOIL_DETAILED_TRACY_ZONE;
	#if 0 // FIXME: desyncs
	if (skipping)
		LOG_L(L_ERROR, "skipping appears to be busted (%i)", skipping);

	skipStartFrame = gs->frameNum;
	skipEndFrame = toFrame;

	if (skipEndFrame <= skipStartFrame) {
		LOG_L(L_WARNING, "Already passed %i (%i)", skipEndFrame / GAME_SPEED, skipEndFrame);
		return;
	}

	skipTotalFrames = skipEndFrame - skipStartFrame;
	skipSeconds = skipTotalFrames * INV_GAME_SPEED;

	skipSoundmute = sound->IsMuted();
	if (!skipSoundmute)
		sound->Mute(); // no sounds

	//FIXME not smart to change SYNCED values in demo playbacks etc.
	skipOldSpeed     = gs->speedFactor;
	skipOldUserSpeed = gs->wantedSpeedFactor;
	const float speed = 1.0f;
	gs->speedFactor     = speed;
	gs->wantedSpeedFactor = speed;

	skipLastDrawTime = spring_gettime();

	skipping = true;
	#endif
}

void CGame::EndSkip() {
	RECOIL_DETAILED_TRACY_ZONE;
	#if 0 // FIXME
	skipping = false;

	gu->gameTime    += skipSeconds;
	gu->modGameTime += skipSeconds;

	gs->speedFactor     = skipOldSpeed;
	gs->wantedSpeedFactor = skipOldUserSpeed;

	if (!skipSoundmute)
		sound->Mute(); // sounds back on

	LOG("Skipped %.1f seconds", skipSeconds);
	#endif
}



void CGame::DrawSkip(bool blackscreen) {
	RECOIL_DETAILED_TRACY_ZONE;
	#if 0
	const int framesLeft = (skipEndFrame - gs->frameNum);
	if (blackscreen) {
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glColor3f(0.5f, 1.0f, 0.5f);
	font->glFormat(0.5f, 0.55f, 2.5f, FONT_CENTER | FONT_SCALE | FONT_NORM, "Skipping %.1f game seconds", skipSeconds);
	glColor3f(1.0f, 1.0f, 1.0f);
	font->glFormat(0.5f, 0.45f, 2.0f, FONT_CENTER | FONT_SCALE | FONT_NORM, "(%i frames left)", framesLeft);

	const float ff = (float)framesLeft / (float)skipTotalFrames;
	glDisable(GL_TEXTURE_2D);
	const float b = 0.004f; // border
	const float yn = 0.35f;
	const float yp = 0.38f;
	glColor3f(0.2f, 0.2f, 1.0f);
	glRectf(0.25f - b, yn - b, 0.75f + b, yp + b);
	glColor3f(0.25f + (0.75f * ff), 1.0f - (0.75f * ff), 0.0f);
	glRectf(0.5 - (0.25f * ff), yn, 0.5f + (0.25f * ff), yp);
	#endif
}



void CGame::ReloadCOB(const string& msg, int player)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!gs->cheatEnabled) {
		LOG_L(L_WARNING, "[Game::%s] can only be used if cheating is enabled", __func__);
		return;
	}

	if (msg.empty()) {
		LOG_L(L_WARNING, "[Game::%s] missing UnitDef name", __func__);
		return;
	}

	const UnitDef* udef = unitDefHandler->GetUnitDefByName(msg);

	if (udef == nullptr) {
		LOG_L(L_WARNING, "[Game::%s] unknown UnitDef name: \"%s\"", __func__, msg.c_str());
		return;
	}

	unitScriptEngine->ReloadScripts(udef);
}


bool CGame::IsSimLagging(float maxLatency) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const float deltaTime = spring_tomsecs(spring_gettime() - lastFrameTime);
	const float sfLatency = maxLatency / gs->speedFactor;

	return (!gs->paused && (deltaTime > sfLatency));
}


void CGame::Save(std::string&& fileName, std::string&& saveArgs)
{
	RECOIL_DETAILED_TRACY_ZONE;
	globalSaveFileData.name = std::move(fileName);
	globalSaveFileData.args = std::move(saveArgs);
}




bool CGame::ProcessCommandText(const std::string& command) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (command.size() <= 2)
		return false;

	if ((command[0] == '/') && (command[1] != '/')) {
		// strip the '/'
		ProcessAction(Action(command.substr(1)), false);
		return true;
	}

	return false;
}

bool CGame::ProcessAction(const Action& action, bool isRepeat)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (ActionPressed(action, isRepeat))
		return true;

	bool handled = false;
	// maybe a widget is interested?
	if (luaUI != nullptr && luaUI->GotChatMsg(action.rawline, false))
		handled = true;

	if (luaMenu != nullptr && luaMenu->GotChatMsg(action.rawline, false))
		handled = true;

	return handled;
}

void CGame::ActionReceived(const Action& action, int playerID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const ISyncedActionExecutor* executor = syncedGameCommands->GetActionExecutor(action.command);

	if (executor != nullptr) {
		// an executor for that action was found
		executor->ExecuteAction(SyncedAction(action, playerID));
		return;
	}

	if (!gs->PreSimFrame()) {
		eventHandler.SyncedActionFallback(action.rawline, playerID);
		//FIXME add unsynced one?
	}
}

bool CGame::ActionPressed(const Action& action, bool isRepeat)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (unsyncedGameCommands->ActionPressed(action, isRepeat))
		return true;

	if (CGameServer::IsServerCommand(action.command)) {
		CommandMessage pckt(action, gu->myPlayerNum);
		clientNet->Send(pckt.Pack());
		return true;
	}

	return (gameCommandConsole.ExecuteAction(action));
}

bool CGame::ActionReleased(const Action& action)
{
	return unsyncedGameCommands->ActionReleased(action);
}

const ActionList& CGame::GetLastActionList()
{
	return gameInputReceiver.lastActionList;
}

