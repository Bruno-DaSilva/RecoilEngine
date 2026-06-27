/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/GL/myGL.h"

#include <Rml/Backends/RmlUi_Backend.h>
#include <RmlUi/Core.h>
#include "Game.h"
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
#include "Sim/Ecs/Registry.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "Sim/MoveTypes/MoveTypeFactory.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Path/QTPFS/PathManager.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/CommandAI/BuilderCaches.h"
#include "Sim/Units/Scripts/UnitScriptFactory.h"
#include "Sim/Units/Scripts/UnitScriptEngine.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Weapons/Weapon.h"
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
#include "System/creg/Serializer.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Sync/FPUCheck.h"
#include "System/SafeUtil.h"
#include "System/SpringExitCode.h"
#include "System/SpringMath.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/FileSystem/FileSystem.h"
#include "System/LoadSave/LoadSaveHandler.h"
#include "System/LoadSave/CregLoadSaveHandler.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/Log/ILog.h"
#include "System/Net/LocalConnection.h"
#include "System/Platform/Misc.h"
#include "System/Platform/Watchdog.h"
#include "System/Platform/errorhandler.h"
#include "System/Sound/ISound.h"
#include "System/Sound/ISoundChannels.h"
#include "System/Sync/DumpState.h"
#include "System/TimeProfiler.h"
#include "System/LoadLock.h"

#include "System/Misc/TracyDefs.h"

namespace netcode {
	void RestoreLocalSyncChecksum(int32_t frameNum, uint32_t checksum);
}

#include "fmt/format.h"
#include "fmt/ranges.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>


#undef CreateDirectory

namespace {
	struct ReplayResumeRequest {
		bool active = false;
		bool expectCheckpoint = false;
		int targetFrame = -1;
		float targetTime = -1.0f;
		std::string demoName;
	};

	static ReplayResumeRequest replayResumeRequest;

	static constexpr int replayAutoCheckpointInterval = 600;

	static std::string QuoteTdfValue(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size() + 2);

		for (const char c: value) {
			if (c == '\\' || c == '"')
				escaped.push_back('\\');

			escaped.push_back(c);
		}

		return "\"" + escaped + "\"";
	}

	static std::string FormatReplayTime(int totalSeconds)
	{
		totalSeconds = std::max(0, totalSeconds);

		const int hours = totalSeconds / 3600;
		const int minutes = (totalSeconds % 3600) / 60;
		const int seconds = totalSeconds % 60;

		if (hours > 0)
			return fmt::format("{}:{:02d}:{:02d}", hours, minutes, seconds);

		return fmt::format("{:02d}:{:02d}", minutes, seconds);
	}

	struct RewindAuditSettings {
		bool initialized = false;
		bool enabled = false;
		int periodFrames = 900;
		std::vector<int> frames;
		std::string label = "audit";
		std::string outputPath;
	};

	static uint32_t RewindAuditFloatBits(float value)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static void RewindAuditMix(uint64_t& digest, uint64_t value)
	{
		digest ^= value;
		digest *= 1099511628211ull;
	}

	static void RewindAuditMixFloat3(uint64_t& digest, const float3& value)
	{
		RewindAuditMix(digest, RewindAuditFloatBits(value.x));
		RewindAuditMix(digest, RewindAuditFloatBits(value.y));
		RewindAuditMix(digest, RewindAuditFloatBits(value.z));
	}

	static std::string SanitizeRewindAuditLabel(std::string label)
	{
		for (char& c: label) {
			const unsigned char uc = static_cast<unsigned char>(c);
			if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
				c = '_';
		}

		if (label.empty())
			label = "audit";
		if (label.size() > 80)
			label.resize(80);

		return label;
	}

	static int ParseRewindAuditTimeToken(const std::string& token)
	{
		if (token.empty())
			return -1;

		if (token.find(':') == std::string::npos) {
			char* end = nullptr;
			const long frame = std::strtol(token.c_str(), &end, 10);
			return (end != token.c_str() && *end == '\0' && frame >= 0) ? static_cast<int>(frame) : -1;
		}

		std::vector<int> parts;
		std::stringstream stream(token);
		std::string part;
		while (std::getline(stream, part, ':')) {
			char* end = nullptr;
			const long value = std::strtol(part.c_str(), &end, 10);
			if (end == part.c_str() || *end != '\0' || value < 0)
				return -1;
			parts.push_back(static_cast<int>(value));
		}

		if (parts.size() == 2)
			return ((parts[0] * 60) + parts[1]) * GAME_SPEED;
		if (parts.size() == 3)
			return ((parts[0] * 3600) + (parts[1] * 60) + parts[2]) * GAME_SPEED;

		return -1;
	}

	static std::vector<int> ParseRewindAuditFrames(const std::string& value)
	{
		std::vector<int> frames;
		std::string token;

		const auto flushToken = [&]() {
			const int frame = ParseRewindAuditTimeToken(token);
			if (frame >= 0)
				frames.push_back(frame);
			token.clear();
		};

		for (const char c: value) {
			if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
				if (!token.empty())
					flushToken();
				continue;
			}

			token.push_back(c);
		}

		if (!token.empty())
			flushToken();

		std::sort(frames.begin(), frames.end());
		frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
		return frames;
	}

	static RewindAuditSettings& GetRewindAuditSettings()
	{
		static RewindAuditSettings settings;

		if (settings.initialized)
			return settings;

		settings.initialized = true;
		settings.enabled = configHandler->GetBool("RewindAudit");
		settings.periodFrames = std::max(0, configHandler->GetInt("RewindAuditPeriod"));
		settings.frames = ParseRewindAuditFrames(configHandler->GetString("RewindAuditFrames"));
		settings.label = SanitizeRewindAuditLabel(configHandler->GetString("RewindAuditLabel"));

		if (!settings.enabled)
			return settings;

		settings.outputPath = dataDirsAccess.LocateFile(
			fmt::format("rewind_audit/{}/audit.log", settings.label),
			FileQueryFlags::WRITE | FileQueryFlags::CREATE_DIRS
		);

		std::ofstream file(settings.outputPath, std::ios::out | std::ios::trunc);
		if (file.is_open()) {
			file << "# BAR Rewind audit log\n";
			file << "# This file is diagnostic output only. It does not modify replay files or replay command data.\n";
			file << "label=" << settings.label << "\n";
			file << "periodFrames=" << settings.periodFrames << "\n";
			file << "frames=" << configHandler->GetString("RewindAuditFrames") << "\n";
			if (gameSetup != nullptr) {
				file << "map=" << gameSetup->mapName << "\n";
				file << "mod=" << gameSetup->modName << "\n";
				file << "demoFile=" << FileSystem::GetFilename(gameSetup->demoName) << "\n";
			}
		}

		LOG("[RewindAudit] enabled, writing to %s", settings.outputPath.c_str());
		return settings;
	}

	static bool ShouldWriteRewindAuditFrame(const int frameNum)
	{
		RewindAuditSettings& settings = GetRewindAuditSettings();
		if (!settings.enabled)
			return false;

		if (std::binary_search(settings.frames.begin(), settings.frames.end(), frameNum))
			return true;

		return (settings.periodFrames > 0 && frameNum >= 0 && (frameNum % settings.periodFrames) == 0);
	}

	static void AppendRewindAuditLine(const std::string& line)
	{
		const RewindAuditSettings& settings = GetRewindAuditSettings();
		if (!settings.enabled || settings.outputPath.empty())
			return;

		std::ofstream file(settings.outputPath, std::ios::out | std::ios::app);
		if (file.is_open())
			file << line << '\n';
	}

	static uint64_t BuildRewindAuditTeamDigest()
	{
		uint64_t digest = 1469598103934665603ull;

		for (int teamNum = 0; teamNum < teamHandler.ActiveTeams(); ++teamNum) {
			const CTeam* team = teamHandler.Team(teamNum);
			if (team == nullptr)
				continue;

			RewindAuditMix(digest, static_cast<uint64_t>(team->teamNum));
			RewindAuditMix(digest, static_cast<uint64_t>(team->leader));
			RewindAuditMix(digest, static_cast<uint64_t>(team->isDead));
			RewindAuditMix(digest, static_cast<uint64_t>(team->gaia));
			RewindAuditMix(digest, RewindAuditFloatBits(team->res.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->res.energy));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resStorage.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resStorage.energy));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resIncome.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resIncome.energy));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resExpense.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resExpense.energy));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resPull.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resPull.energy));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resShare.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(team->resShare.energy));
		}

		return digest;
	}

	static std::vector<const CUnit*> GetSortedRewindAuditUnits()
	{
		std::vector<const CUnit*> units;
		units.reserve(unitHandler.GetActiveUnits().size());

		for (const CUnit* unit: unitHandler.GetActiveUnits()) {
			if (unit != nullptr)
				units.push_back(unit);
		}

		std::sort(units.begin(), units.end(), [](const CUnit* a, const CUnit* b) {
			return (a->id < b->id);
		});

		return units;
	}

	static uint64_t BuildRewindAuditCommandDigest(const std::vector<const CUnit*>& units)
	{
		uint64_t digest = 1469598103934665603ull;

		for (const CUnit* unit: units) {
			const CCommandAI* commandAI = unit->commandAI;
			if (commandAI == nullptr)
				continue;

			RewindAuditMix(digest, static_cast<uint64_t>(unit->id));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->lastUserCommand));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->selfDCountdown));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->lastFinishCommand));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->targetDied));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->repeatOrders));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->lastSelectedCommandPage));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->inCommand));
			RewindAuditMix(digest, static_cast<uint64_t>(commandAI->commandQue.size()));

			for (const Command& command: commandAI->commandQue) {
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetID()));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetID(true)));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetTimeOut()));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetpageIndex()));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetNumParams()));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetTag()));
				RewindAuditMix(digest, static_cast<uint64_t>(command.GetOpts()));

				for (unsigned int paramIdx = 0; paramIdx < command.GetNumParams(); ++paramIdx)
					RewindAuditMix(digest, RewindAuditFloatBits(command.GetParam(paramIdx)));
			}
		}

		return digest;
	}

	struct RewindAuditUnitDigests {
		uint64_t identityDigest = 1469598103934665603ull;
		uint64_t kinematicsDigest = 1469598103934665603ull;
		uint64_t moveDigest = 1469598103934665603ull;
		uint64_t combinedDigest = 1469598103934665603ull;
	};

	struct RewindAuditUnitSnapshot {
		int id = -1;
		int team = -1;
		int unitDefId = -1;
		int commandCount = 0;
		int firstCommandId = 0;
		int progressState = 0;
		unsigned int pathID = 0;
		float3 pos;
		float3 speed;
		float3 goalPos;
		float3 oldPos;
		float3 oldSlowUpdatePos;
		float3 currWayPoint;
		float3 nextWayPoint;
		float3 frontDir;
		float3 rightDir;
		float3 upDir;
		float health = 0.0f;
		float currentSpeed = 0.0f;
		float wantedSpeed = 0.0f;
		float deltaSpeed = 0.0f;
		float currWayPointDist = 0.0f;
		float prevWayPointDist = 0.0f;
		short heading = 0;
		short buildFacing = 0;
		bool atGoal = false;
		bool reversing = false;
	};

	static bool RewindAuditSameFloat(float a, float b)
	{
		return (RewindAuditFloatBits(a) == RewindAuditFloatBits(b));
	}

	static bool RewindAuditSameFloat3(const float3& a, const float3& b)
	{
		return (
			RewindAuditSameFloat(a.x, b.x) &&
			RewindAuditSameFloat(a.y, b.y) &&
			RewindAuditSameFloat(a.z, b.z)
		);
	}

	static std::string RewindAuditFormatFloat3(const float3& value)
	{
		return fmt::format("{:.3f},{:.3f},{:.3f}", value.x, value.y, value.z);
	}

	static RewindAuditUnitSnapshot BuildRewindAuditUnitSnapshot(const CUnit* unit)
	{
		RewindAuditUnitSnapshot snapshot;
		if (unit == nullptr)
			return snapshot;

		snapshot.id = unit->id;
		snapshot.team = unit->team;
		snapshot.unitDefId = (unit->unitDef != nullptr) ? unit->unitDef->id : -1;
		snapshot.pos = unit->pos;
		snapshot.speed = float3(unit->speed.x, unit->speed.y, unit->speed.z);
		snapshot.health = unit->health;
		snapshot.heading = unit->heading;
		snapshot.buildFacing = unit->buildFacing;
		snapshot.frontDir = unit->frontdir;
		snapshot.rightDir = unit->rightdir;
		snapshot.upDir = unit->updir;

		if (unit->commandAI != nullptr) {
			snapshot.commandCount = static_cast<int>(unit->commandAI->commandQue.size());
			if (!unit->commandAI->commandQue.empty())
				snapshot.firstCommandId = unit->commandAI->commandQue.front().GetID();
		}

		if (unit->moveType != nullptr) {
			snapshot.goalPos = unit->moveType->goalPos;
			snapshot.oldPos = unit->moveType->oldPos;
			snapshot.oldSlowUpdatePos = unit->moveType->oldSlowUpdatePos;
			snapshot.progressState = unit->moveType->progressState;
		}

		if (const auto* groundMoveType = dynamic_cast<const CGroundMoveType*>(unit->moveType); groundMoveType != nullptr) {
			snapshot.pathID = groundMoveType->GetPathID();
			snapshot.currWayPoint = groundMoveType->GetCurrWayPoint();
			snapshot.nextWayPoint = groundMoveType->GetNextWayPoint();
			snapshot.currentSpeed = groundMoveType->GetCurrentSpeed();
			snapshot.wantedSpeed = groundMoveType->GetWantedSpeed();
			snapshot.deltaSpeed = groundMoveType->GetDeltaSpeed();
			snapshot.currWayPointDist = groundMoveType->GetCurrWayPointDist();
			snapshot.prevWayPointDist = groundMoveType->GetPrevWayPointDist();
			snapshot.atGoal = groundMoveType->IsAtGoal();
			snapshot.reversing = groundMoveType->IsReversing();
		}

		return snapshot;
	}

	static bool RewindAuditUnitSnapshotDiffers(const RewindAuditUnitSnapshot& a, const RewindAuditUnitSnapshot& b)
	{
		return (
			a.team != b.team ||
			a.unitDefId != b.unitDefId ||
			a.commandCount != b.commandCount ||
			a.firstCommandId != b.firstCommandId ||
			a.progressState != b.progressState ||
			a.pathID != b.pathID ||
			!RewindAuditSameFloat3(a.pos, b.pos) ||
			!RewindAuditSameFloat3(a.speed, b.speed) ||
			!RewindAuditSameFloat3(a.goalPos, b.goalPos) ||
			!RewindAuditSameFloat3(a.oldPos, b.oldPos) ||
			!RewindAuditSameFloat3(a.oldSlowUpdatePos, b.oldSlowUpdatePos) ||
			!RewindAuditSameFloat3(a.currWayPoint, b.currWayPoint) ||
			!RewindAuditSameFloat3(a.nextWayPoint, b.nextWayPoint) ||
			!RewindAuditSameFloat3(a.frontDir, b.frontDir) ||
			!RewindAuditSameFloat3(a.rightDir, b.rightDir) ||
			!RewindAuditSameFloat3(a.upDir, b.upDir) ||
			!RewindAuditSameFloat(a.health, b.health) ||
			!RewindAuditSameFloat(a.currentSpeed, b.currentSpeed) ||
			!RewindAuditSameFloat(a.wantedSpeed, b.wantedSpeed) ||
			!RewindAuditSameFloat(a.deltaSpeed, b.deltaSpeed) ||
			!RewindAuditSameFloat(a.currWayPointDist, b.currWayPointDist) ||
			!RewindAuditSameFloat(a.prevWayPointDist, b.prevWayPointDist) ||
			a.heading != b.heading ||
			a.buildFacing != b.buildFacing ||
			a.atGoal != b.atGoal ||
			a.reversing != b.reversing
		);
	}

	static std::string RewindAuditUnitDifferenceNames(const RewindAuditUnitSnapshot& ref, const RewindAuditUnitSnapshot& cur)
	{
		std::vector<std::string> names;
		if (ref.team != cur.team) names.emplace_back("team");
		if (ref.unitDefId != cur.unitDefId) names.emplace_back("unitDef");
		if (ref.commandCount != cur.commandCount) names.emplace_back("cmdCount");
		if (ref.firstCommandId != cur.firstCommandId) names.emplace_back("firstCmd");
		if (ref.progressState != cur.progressState) names.emplace_back("progress");
		if (ref.pathID != cur.pathID) names.emplace_back("pathID");
		if (!RewindAuditSameFloat3(ref.pos, cur.pos)) names.emplace_back("pos");
		if (!RewindAuditSameFloat3(ref.speed, cur.speed)) names.emplace_back("speed");
		if (!RewindAuditSameFloat3(ref.goalPos, cur.goalPos)) names.emplace_back("goal");
		if (!RewindAuditSameFloat3(ref.oldPos, cur.oldPos)) names.emplace_back("oldPos");
		if (!RewindAuditSameFloat3(ref.oldSlowUpdatePos, cur.oldSlowUpdatePos)) names.emplace_back("oldSlowPos");
		if (!RewindAuditSameFloat3(ref.currWayPoint, cur.currWayPoint)) names.emplace_back("currWP");
		if (!RewindAuditSameFloat3(ref.nextWayPoint, cur.nextWayPoint)) names.emplace_back("nextWP");
		if (!RewindAuditSameFloat3(ref.frontDir, cur.frontDir)) names.emplace_back("frontDir");
		if (!RewindAuditSameFloat3(ref.rightDir, cur.rightDir)) names.emplace_back("rightDir");
		if (!RewindAuditSameFloat3(ref.upDir, cur.upDir)) names.emplace_back("upDir");
		if (!RewindAuditSameFloat(ref.health, cur.health)) names.emplace_back("health");
		if (!RewindAuditSameFloat(ref.currentSpeed, cur.currentSpeed)) names.emplace_back("curSpeed");
		if (!RewindAuditSameFloat(ref.wantedSpeed, cur.wantedSpeed)) names.emplace_back("wantSpeed");
		if (!RewindAuditSameFloat(ref.deltaSpeed, cur.deltaSpeed)) names.emplace_back("deltaSpeed");
		if (!RewindAuditSameFloat(ref.currWayPointDist, cur.currWayPointDist)) names.emplace_back("currWPD");
		if (!RewindAuditSameFloat(ref.prevWayPointDist, cur.prevWayPointDist)) names.emplace_back("prevWPD");
		if (ref.heading != cur.heading) names.emplace_back("heading");
		if (ref.buildFacing != cur.buildFacing) names.emplace_back("buildFacing");
		if (ref.atGoal != cur.atGoal) names.emplace_back("atGoal");
		if (ref.reversing != cur.reversing) names.emplace_back("reversing");
		return fmt::format("{}", fmt::join(names, ","));
	}

	static std::string RewindAuditFormatUnitSnapshot(const RewindAuditUnitSnapshot& snapshot)
	{
		return fmt::format(
			"id={} team={} def={} pos=[{}] speed=[{}] health={:.3f} heading={} front=[{}] right=[{}] up=[{}] goal=[{}] oldPos=[{}] oldSlow=[{}] progress={} path={} currWP=[{}] nextWP=[{}] curSpeed={:.3f} wanted={:.3f} delta={:.3f} currWPD={:.3f} prevWPD={:.3f} atGoal={} reversing={} cmds={} firstCmd={}",
			snapshot.id,
			snapshot.team,
			snapshot.unitDefId,
			RewindAuditFormatFloat3(snapshot.pos),
			RewindAuditFormatFloat3(snapshot.speed),
			snapshot.health,
			snapshot.heading,
			RewindAuditFormatFloat3(snapshot.frontDir),
			RewindAuditFormatFloat3(snapshot.rightDir),
			RewindAuditFormatFloat3(snapshot.upDir),
			RewindAuditFormatFloat3(snapshot.goalPos),
			RewindAuditFormatFloat3(snapshot.oldPos),
			RewindAuditFormatFloat3(snapshot.oldSlowUpdatePos),
			snapshot.progressState,
			snapshot.pathID,
			RewindAuditFormatFloat3(snapshot.currWayPoint),
			RewindAuditFormatFloat3(snapshot.nextWayPoint),
			snapshot.currentSpeed,
			snapshot.wantedSpeed,
			snapshot.deltaSpeed,
			snapshot.currWayPointDist,
			snapshot.prevWayPointDist,
			int(snapshot.atGoal),
			int(snapshot.reversing),
			snapshot.commandCount,
			snapshot.firstCommandId
		);
	}

	static void CompareOrStoreRewindAuditUnitSnapshots(const int frameNum, const std::vector<const CUnit*>& units)
	{
		static std::unordered_map<int, std::vector<RewindAuditUnitSnapshot>> referenceSnapshots;

		std::vector<RewindAuditUnitSnapshot> currentSnapshots;
		currentSnapshots.reserve(units.size());
		for (const CUnit* unit: units)
			currentSnapshots.emplace_back(BuildRewindAuditUnitSnapshot(unit));

		const auto refIt = referenceSnapshots.find(frameNum);
		if (refIt == referenceSnapshots.end()) {
			referenceSnapshots.emplace(frameNum, std::move(currentSnapshots));
			return;
		}

		const auto& refSnapshots = refIt->second;
		std::size_t refIdx = 0;
		std::size_t curIdx = 0;
		unsigned int loggedDiffs = 0;
		unsigned int totalDiffs = 0;
		constexpr unsigned int maxLoggedDiffs = 12;

		while (refIdx < refSnapshots.size() || curIdx < currentSnapshots.size()) {
			const RewindAuditUnitSnapshot* ref = (refIdx < refSnapshots.size()) ? &refSnapshots[refIdx] : nullptr;
			const RewindAuditUnitSnapshot* cur = (curIdx < currentSnapshots.size()) ? &currentSnapshots[curIdx] : nullptr;

			if (ref != nullptr && cur != nullptr && ref->id == cur->id) {
				if (RewindAuditUnitSnapshotDiffers(*ref, *cur)) {
					totalDiffs += 1;
					if (loggedDiffs < maxLoggedDiffs) {
						AppendRewindAuditLine(fmt::format(
							"detail frame={} unit={} diff={} reference=\"{}\" current=\"{}\"",
							frameNum,
							ref->id,
							RewindAuditUnitDifferenceNames(*ref, *cur),
							RewindAuditFormatUnitSnapshot(*ref),
							RewindAuditFormatUnitSnapshot(*cur)
						));
						loggedDiffs += 1;
					}
				}
				refIdx += 1;
				curIdx += 1;
				continue;
			}

			if (cur == nullptr || (ref != nullptr && ref->id < cur->id)) {
				totalDiffs += 1;
				if (loggedDiffs < maxLoggedDiffs) {
					AppendRewindAuditLine(fmt::format(
						"detail frame={} missing-current unit={} reference=\"{}\"",
						frameNum,
						ref->id,
						RewindAuditFormatUnitSnapshot(*ref)
					));
					loggedDiffs += 1;
				}
				refIdx += 1;
				continue;
			}

			totalDiffs += 1;
			if (loggedDiffs < maxLoggedDiffs) {
				AppendRewindAuditLine(fmt::format(
					"detail frame={} missing-reference unit={} current=\"{}\"",
					frameNum,
					cur->id,
					RewindAuditFormatUnitSnapshot(*cur)
				));
				loggedDiffs += 1;
			}
			curIdx += 1;
		}

		if (totalDiffs > 0) {
			AppendRewindAuditLine(fmt::format(
				"detail-summary frame={} differingUnits={} logged={}",
				frameNum,
				totalDiffs,
				loggedDiffs
			));
		}
	}

	static RewindAuditUnitDigests BuildRewindAuditUnitDigests(const std::vector<const CUnit*>& units)
	{
		RewindAuditUnitDigests digests;

		for (const CUnit* unit: units) {
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->id));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->team));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>((unit->unitDef != nullptr) ? unit->unitDef->id : -1));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->isDead));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->IsStunned()));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->beingBuilt));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->fireState));
			RewindAuditMix(digests.identityDigest, static_cast<uint64_t>(unit->moveState));

			RewindAuditMix(digests.kinematicsDigest, static_cast<uint64_t>(unit->id));
			RewindAuditMixFloat3(digests.kinematicsDigest, unit->pos);
			RewindAuditMix(digests.kinematicsDigest, RewindAuditFloatBits(unit->speed.x));
			RewindAuditMix(digests.kinematicsDigest, RewindAuditFloatBits(unit->speed.y));
			RewindAuditMix(digests.kinematicsDigest, RewindAuditFloatBits(unit->speed.z));
			RewindAuditMix(digests.kinematicsDigest, RewindAuditFloatBits(unit->speed.w));
			RewindAuditMix(digests.kinematicsDigest, RewindAuditFloatBits(unit->health));
			RewindAuditMix(digests.kinematicsDigest, static_cast<uint64_t>(unit->heading));
			RewindAuditMix(digests.kinematicsDigest, static_cast<uint64_t>(unit->buildFacing));
			RewindAuditMixFloat3(digests.kinematicsDigest, unit->frontdir);
			RewindAuditMixFloat3(digests.kinematicsDigest, unit->rightdir);
			RewindAuditMixFloat3(digests.kinematicsDigest, unit->updir);

			if (unit->moveType != nullptr) {
				RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(unit->id));
				RewindAuditMixFloat3(digests.moveDigest, unit->moveType->goalPos);
				RewindAuditMixFloat3(digests.moveDigest, unit->moveType->oldPos);
				RewindAuditMixFloat3(digests.moveDigest, unit->moveType->oldSlowUpdatePos);
				RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(unit->moveType->progressState));

				if (const auto* groundMoveType = dynamic_cast<const CGroundMoveType*>(unit->moveType); groundMoveType != nullptr) {
					RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(groundMoveType->GetPathID()));
					RewindAuditMixFloat3(digests.moveDigest, groundMoveType->GetCurrWayPoint());
					RewindAuditMixFloat3(digests.moveDigest, groundMoveType->GetNextWayPoint());
					RewindAuditMix(digests.moveDigest, RewindAuditFloatBits(groundMoveType->GetCurrentSpeed()));
					RewindAuditMix(digests.moveDigest, RewindAuditFloatBits(groundMoveType->GetWantedSpeed()));
					RewindAuditMix(digests.moveDigest, RewindAuditFloatBits(groundMoveType->GetDeltaSpeed()));
					RewindAuditMix(digests.moveDigest, RewindAuditFloatBits(groundMoveType->GetCurrWayPointDist()));
					RewindAuditMix(digests.moveDigest, RewindAuditFloatBits(groundMoveType->GetPrevWayPointDist()));
					RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(groundMoveType->IsReversing()));
					RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(groundMoveType->IsAtGoal()));
				}
			} else {
				RewindAuditMix(digests.moveDigest, static_cast<uint64_t>(unit->id));
				RewindAuditMix(digests.moveDigest, 0ull);
			}
		}

		digests.combinedDigest = digests.identityDigest;
		RewindAuditMix(digests.combinedDigest, digests.kinematicsDigest);
		RewindAuditMix(digests.combinedDigest, digests.moveDigest);

		return digests;
	}

	static uint64_t BuildRewindAuditFeatureDigest(unsigned int& activeFeatures)
	{
		std::vector<int> featureIDs(featureHandler.GetActiveFeatureIDs().begin(), featureHandler.GetActiveFeatureIDs().end());
		std::sort(featureIDs.begin(), featureIDs.end());
		activeFeatures = static_cast<unsigned int>(featureIDs.size());

		uint64_t digest = 1469598103934665603ull;
		for (const int featureID: featureIDs) {
			const CFeature* feature = featureHandler.GetFeature(featureID);
			if (feature == nullptr)
				continue;

			RewindAuditMix(digest, static_cast<uint64_t>(feature->id));
			RewindAuditMix(digest, static_cast<uint64_t>(feature->team));
			RewindAuditMix(digest, static_cast<uint64_t>((feature->def != nullptr) ? feature->def->id : -1));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->pos.x));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->pos.y));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->pos.z));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->health));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->reclaimLeft));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->resurrectProgress));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->resources.metal));
			RewindAuditMix(digest, RewindAuditFloatBits(feature->resources.energy));
		}

		return digest;
	}

	struct RewindAuditWeaponSnapshot {
		int unitId = -1;
		int weaponIndex = -1;
		int targetType = Target_None;
		int targetUnitId = -1;
		int targetProjectileId = -1;
		bool targetIsUser = false;
		bool targetIsAuto = false;
		bool targetIsManual = false;
		int aimFromPiece = -1;
		int muzzlePiece = -1;
		int reloadStatus = 0;
		int nextSalvo = 0;
		int salvoLeft = 0;
		int lastAimedFrame = 0;
		float3 relAimFromPos;
		float3 aimFromPos;
		float3 relWeaponMuzzlePos;
		float3 weaponMuzzlePos;
		float3 weaponDir;
		float3 wantedDir;
		float3 lastRequestedDir;
		float3 currentTargetPos;
		float3 salvoError;
		float3 errorVector;
		float3 errorVectorAdd;
		float3 targetGroundPos;
	};

	static RewindAuditWeaponSnapshot BuildRewindAuditWeaponSnapshot(
		const CUnit* unit,
		const CWeapon* weapon,
		const int weaponIndex
	) {
		RewindAuditWeaponSnapshot snapshot;
		const SWeaponTarget& target = weapon->GetCurrentTarget();
		snapshot.unitId = unit->id;
		snapshot.weaponIndex = weaponIndex;
		snapshot.targetType = target.type;
		snapshot.targetUnitId = (target.unit != nullptr) ? target.unit->id : -1;
		snapshot.targetProjectileId = (target.intercept != nullptr) ? target.intercept->id : -1;
		snapshot.targetIsUser = target.isUserTarget;
		snapshot.targetIsAuto = target.isAutoTarget;
		snapshot.targetIsManual = target.isManualFire;
		snapshot.aimFromPiece = weapon->aimFromPiece;
		snapshot.muzzlePiece = weapon->muzzlePiece;
		snapshot.reloadStatus = weapon->reloadStatus;
		snapshot.nextSalvo = weapon->nextSalvo;
		snapshot.salvoLeft = weapon->salvoLeft;
		snapshot.lastAimedFrame = weapon->lastAimedFrame;
		snapshot.relAimFromPos = weapon->relAimFromPos;
		snapshot.aimFromPos = weapon->aimFromPos;
		snapshot.relWeaponMuzzlePos = weapon->relWeaponMuzzlePos;
		snapshot.weaponMuzzlePos = weapon->weaponMuzzlePos;
		snapshot.weaponDir = weapon->weaponDir;
		snapshot.wantedDir = weapon->wantedDir;
		snapshot.lastRequestedDir = weapon->lastRequestedDir;
		snapshot.currentTargetPos = weapon->GetCurrentTargetPos();
		snapshot.salvoError = weapon->salvoError;
		snapshot.errorVector = weapon->errorVector;
		snapshot.errorVectorAdd = weapon->errorVectorAdd;
		snapshot.targetGroundPos = target.groundPos;
		return snapshot;
	}

	static bool RewindAuditWeaponSnapshotDiffers(
		const RewindAuditWeaponSnapshot& reference,
		const RewindAuditWeaponSnapshot& current
	) {
		return (
			reference.targetType != current.targetType ||
			reference.targetUnitId != current.targetUnitId ||
			reference.targetProjectileId != current.targetProjectileId ||
			reference.targetIsUser != current.targetIsUser ||
			reference.targetIsAuto != current.targetIsAuto ||
			reference.targetIsManual != current.targetIsManual ||
			reference.aimFromPiece != current.aimFromPiece ||
			reference.muzzlePiece != current.muzzlePiece ||
			reference.reloadStatus != current.reloadStatus ||
			reference.nextSalvo != current.nextSalvo ||
			reference.salvoLeft != current.salvoLeft ||
			reference.lastAimedFrame != current.lastAimedFrame ||
			!RewindAuditSameFloat3(reference.relAimFromPos, current.relAimFromPos) ||
			!RewindAuditSameFloat3(reference.aimFromPos, current.aimFromPos) ||
			!RewindAuditSameFloat3(reference.relWeaponMuzzlePos, current.relWeaponMuzzlePos) ||
			!RewindAuditSameFloat3(reference.weaponMuzzlePos, current.weaponMuzzlePos) ||
			!RewindAuditSameFloat3(reference.weaponDir, current.weaponDir) ||
			!RewindAuditSameFloat3(reference.wantedDir, current.wantedDir) ||
			!RewindAuditSameFloat3(reference.lastRequestedDir, current.lastRequestedDir) ||
			!RewindAuditSameFloat3(reference.currentTargetPos, current.currentTargetPos) ||
			!RewindAuditSameFloat3(reference.salvoError, current.salvoError) ||
			!RewindAuditSameFloat3(reference.errorVector, current.errorVector) ||
			!RewindAuditSameFloat3(reference.errorVectorAdd, current.errorVectorAdd) ||
			!RewindAuditSameFloat3(reference.targetGroundPos, current.targetGroundPos)
		);
	}

	static std::string RewindAuditFormatFloat3Precise(const float3& value)
	{
		return fmt::format("{:.9f},{:.9f},{:.9f}", value.x, value.y, value.z);
	}

	static std::string RewindAuditFormatWeaponSnapshot(const RewindAuditWeaponSnapshot& snapshot)
	{
		return fmt::format(
			"unit={} weapon={} targetType={} targetUnit={} targetProjectile={} targetUser={} targetAuto={} targetManual={} targetGround=[{}] aimPiece={} muzzlePiece={} reload={} nextSalvo={} salvoLeft={} lastAim={} relAim=[{}] aim=[{}] relMuzzle=[{}] muzzle=[{}] dir=[{}] wanted=[{}] requested=[{}] target=[{}] salvoError=[{}] error=[{}] errorAdd=[{}]",
			snapshot.unitId,
			snapshot.weaponIndex,
			snapshot.targetType,
			snapshot.targetUnitId,
			snapshot.targetProjectileId,
			int(snapshot.targetIsUser),
			int(snapshot.targetIsAuto),
			int(snapshot.targetIsManual),
			RewindAuditFormatFloat3Precise(snapshot.targetGroundPos),
			snapshot.aimFromPiece,
			snapshot.muzzlePiece,
			snapshot.reloadStatus,
			snapshot.nextSalvo,
			snapshot.salvoLeft,
			snapshot.lastAimedFrame,
			RewindAuditFormatFloat3Precise(snapshot.relAimFromPos),
			RewindAuditFormatFloat3Precise(snapshot.aimFromPos),
			RewindAuditFormatFloat3Precise(snapshot.relWeaponMuzzlePos),
			RewindAuditFormatFloat3Precise(snapshot.weaponMuzzlePos),
			RewindAuditFormatFloat3Precise(snapshot.weaponDir),
			RewindAuditFormatFloat3Precise(snapshot.wantedDir),
			RewindAuditFormatFloat3Precise(snapshot.lastRequestedDir),
			RewindAuditFormatFloat3Precise(snapshot.currentTargetPos),
			RewindAuditFormatFloat3Precise(snapshot.salvoError),
			RewindAuditFormatFloat3Precise(snapshot.errorVector),
			RewindAuditFormatFloat3Precise(snapshot.errorVectorAdd)
		);
	}

	static void CompareOrStoreRewindAuditWeaponSnapshots(
		const int frameNum,
		const std::vector<const CUnit*>& units
	) {
		static std::unordered_map<int, std::vector<RewindAuditWeaponSnapshot>> referenceSnapshots;
		std::vector<RewindAuditWeaponSnapshot> currentSnapshots;

		for (const CUnit* unit: units) {
			for (std::size_t weaponIndex = 0; weaponIndex < unit->weapons.size(); ++weaponIndex) {
				const CWeapon* weapon = unit->weapons[weaponIndex];
				if (weapon != nullptr)
					currentSnapshots.emplace_back(BuildRewindAuditWeaponSnapshot(unit, weapon, weaponIndex));
			}
		}

		const auto refIt = referenceSnapshots.find(frameNum);
		if (refIt == referenceSnapshots.end()) {
			referenceSnapshots.emplace(frameNum, std::move(currentSnapshots));
			return;
		}

		const auto& reference = refIt->second;
		const std::size_t maxSize = std::max(reference.size(), currentSnapshots.size());
		unsigned int totalDiffs = 0;
		unsigned int loggedDiffs = 0;
		constexpr unsigned int maxLoggedDiffs = 12;

		for (std::size_t i = 0; i < maxSize; ++i) {
			const bool haveReference = (i < reference.size());
			const bool haveCurrent = (i < currentSnapshots.size());
			const bool differs = (
				haveReference != haveCurrent ||
				(
					haveReference &&
					haveCurrent &&
					(
						reference[i].unitId != currentSnapshots[i].unitId ||
						reference[i].weaponIndex != currentSnapshots[i].weaponIndex ||
						RewindAuditWeaponSnapshotDiffers(reference[i], currentSnapshots[i])
					)
				)
			);

			if (!differs)
				continue;

			totalDiffs += 1;
			if (loggedDiffs >= maxLoggedDiffs)
				continue;

			const auto ref = haveReference ? reference[i] : RewindAuditWeaponSnapshot{};
			const auto cur = haveCurrent ? currentSnapshots[i] : RewindAuditWeaponSnapshot{};
			AppendRewindAuditLine(fmt::format(
				"weapon-detail frame={} index={} reference=[present={} {}] current=[present={} {}]",
				frameNum,
				i,
				int(haveReference),
				RewindAuditFormatWeaponSnapshot(ref),
				int(haveCurrent),
				RewindAuditFormatWeaponSnapshot(cur)
			));
			loggedDiffs += 1;
		}

		if (totalDiffs > 0) {
			AppendRewindAuditLine(fmt::format(
				"weapon-detail-summary frame={} differingWeapons={} logged={} referenceSize={} currentSize={}",
				frameNum,
				totalDiffs,
				loggedDiffs,
				reference.size(),
				currentSnapshots.size()
			));
		}
	}

	static uint64_t BuildRewindAuditProjectileDigest(unsigned int& activeProjectiles)
	{
		std::vector<const CProjectile*> projectiles;
		projectiles.reserve(projectileHandler.GetActiveProjectiles(true).size());

		for (const CProjectile* projectile: projectileHandler.GetActiveProjectiles(true)) {
			if (projectile != nullptr)
				projectiles.push_back(projectile);
		}

		std::sort(projectiles.begin(), projectiles.end(), [](const CProjectile* a, const CProjectile* b) {
			return (a->id < b->id);
		});

		activeProjectiles = static_cast<unsigned int>(projectiles.size());

		uint64_t digest = 1469598103934665603ull;
		for (const CProjectile* projectile: projectiles) {
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->id));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->GetProjectileType()));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->GetOwnerID()));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->GetTeamID()));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->createMe));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->deleteMe));
			RewindAuditMix(digest, static_cast<uint64_t>(projectile->checkCol));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->pos.x));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->pos.y));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->pos.z));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->speed.x));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->speed.y));
			RewindAuditMix(digest, RewindAuditFloatBits(projectile->speed.z));
		}

		return digest;
	}

	struct RewindAuditProjectileSnapshot {
		int id = -1;
		uint32_t type = 0;
		uint32_t owner = 0;
		uint32_t team = 0;
		bool createMe = false;
		bool deleteMe = false;
		bool checkCol = false;
		float3 pos;
		float3 speed;
	};

	static RewindAuditProjectileSnapshot BuildRewindAuditProjectileSnapshot(const CProjectile* projectile)
	{
		RewindAuditProjectileSnapshot snapshot;
		snapshot.id = projectile->id;
		snapshot.type = projectile->GetProjectileType();
		snapshot.owner = projectile->GetOwnerID();
		snapshot.team = projectile->GetTeamID();
		snapshot.createMe = projectile->createMe;
		snapshot.deleteMe = projectile->deleteMe;
		snapshot.checkCol = projectile->checkCol;
		snapshot.pos = projectile->pos;
		snapshot.speed = projectile->speed;
		return snapshot;
	}

	static bool RewindAuditProjectileSnapshotDiffers(
		const RewindAuditProjectileSnapshot& reference,
		const RewindAuditProjectileSnapshot& current
	) {
		return (
			reference.type != current.type ||
			reference.owner != current.owner ||
			reference.team != current.team ||
			reference.createMe != current.createMe ||
			reference.deleteMe != current.deleteMe ||
			reference.checkCol != current.checkCol ||
			!RewindAuditSameFloat3(reference.pos, current.pos) ||
			!RewindAuditSameFloat3(reference.speed, current.speed)
		);
	}

	static std::string RewindAuditFormatProjectileSnapshot(const RewindAuditProjectileSnapshot& snapshot)
	{
		return fmt::format(
			"id={} type={} owner={} team={} create={} delete={} checkCol={} pos={} speed={}",
			snapshot.id,
			snapshot.type,
			snapshot.owner,
			snapshot.team,
			int(snapshot.createMe),
			int(snapshot.deleteMe),
			int(snapshot.checkCol),
			RewindAuditFormatFloat3(snapshot.pos),
			RewindAuditFormatFloat3(snapshot.speed)
		);
	}

	static void CompareOrStoreRewindAuditProjectileSnapshots(const int frameNum)
	{
		static std::unordered_map<int, std::vector<RewindAuditProjectileSnapshot>> referenceSnapshots;
		std::vector<RewindAuditProjectileSnapshot> currentSnapshots;

		for (const CProjectile* projectile: projectileHandler.GetActiveProjectiles(true)) {
			if (projectile != nullptr)
				currentSnapshots.emplace_back(BuildRewindAuditProjectileSnapshot(projectile));
		}

		std::sort(currentSnapshots.begin(), currentSnapshots.end(), [](const auto& a, const auto& b) {
			return (a.id < b.id);
		});

		const auto refIt = referenceSnapshots.find(frameNum);
		if (refIt == referenceSnapshots.end()) {
			referenceSnapshots.emplace(frameNum, std::move(currentSnapshots));
			return;
		}

		const auto& reference = refIt->second;
		std::size_t refIdx = 0;
		std::size_t curIdx = 0;
		unsigned int loggedDiffs = 0;
		unsigned int totalDiffs = 0;
		constexpr unsigned int maxLoggedDiffs = 12;

		while (refIdx < reference.size() || curIdx < currentSnapshots.size()) {
			const auto* ref = (refIdx < reference.size()) ? &reference[refIdx] : nullptr;
			const auto* cur = (curIdx < currentSnapshots.size()) ? &currentSnapshots[curIdx] : nullptr;

			if (ref != nullptr && cur != nullptr && ref->id == cur->id) {
				if (RewindAuditProjectileSnapshotDiffers(*ref, *cur)) {
					totalDiffs += 1;
					if (loggedDiffs < maxLoggedDiffs) {
						AppendRewindAuditLine(fmt::format(
							"projectile-detail frame={} projectile={} reference=\"{}\" current=\"{}\"",
							frameNum,
							ref->id,
							RewindAuditFormatProjectileSnapshot(*ref),
							RewindAuditFormatProjectileSnapshot(*cur)
						));
						loggedDiffs += 1;
					}
				}
				refIdx += 1;
				curIdx += 1;
				continue;
			}

			if (cur == nullptr || (ref != nullptr && ref->id < cur->id)) {
				totalDiffs += 1;
				if (loggedDiffs < maxLoggedDiffs) {
					AppendRewindAuditLine(fmt::format(
						"projectile-detail frame={} missing-current projectile={} reference=\"{}\"",
						frameNum,
						ref->id,
						RewindAuditFormatProjectileSnapshot(*ref)
					));
					loggedDiffs += 1;
				}
				refIdx += 1;
				continue;
			}

			totalDiffs += 1;
			if (loggedDiffs < maxLoggedDiffs) {
				AppendRewindAuditLine(fmt::format(
					"projectile-detail frame={} missing-reference projectile={} current=\"{}\"",
					frameNum,
					cur->id,
					RewindAuditFormatProjectileSnapshot(*cur)
				));
				loggedDiffs += 1;
			}
			curIdx += 1;
		}

		if (totalDiffs > 0) {
			AppendRewindAuditLine(fmt::format(
				"projectile-detail-summary frame={} differingProjectiles={} logged={}",
				frameNum,
				totalDiffs,
				loggedDiffs
			));
		}
	}

	template <typename Component>
	static uint64_t BuildRewindAuditEcsOrderDigest()
	{
		uint64_t digest = 1469598103934665603ull;
		auto view = Sim::registry.view<Component>();

		RewindAuditMix(digest, static_cast<uint64_t>(view.size()));
		for (std::size_t i = 0; i < view.size(); ++i) {
			const entt::entity entity = view.template storage<Component>()[i];
			const Component& component = view.template get<Component>(entity);

			RewindAuditMix(digest, static_cast<uint64_t>(entt::to_integral(entity)));

			if constexpr (requires { component.value; }) {
				if constexpr (std::is_integral_v<std::decay_t<decltype(component.value)>>) {
					RewindAuditMix(digest, static_cast<uint64_t>(component.value));
				} else {
					RewindAuditMix(digest, static_cast<uint64_t>(component.value.size()));
				}
			} else if constexpr (std::is_same_v<Component, MoveTypes::UnitTrapCheck>) {
				RewindAuditMix(digest, static_cast<uint64_t>(component.type));
				RewindAuditMix(digest, static_cast<uint64_t>(component.id));
			} else if constexpr (std::is_same_v<Component, MoveTypes::UnitMovedEvent>) {
				RewindAuditMix(digest, static_cast<uint64_t>(component.unitId));
				RewindAuditMix(digest, static_cast<uint64_t>(component.moved));
			} else if constexpr (std::is_same_v<Component, MoveTypes::ChangeHeadingEvent>) {
				RewindAuditMix(digest, static_cast<uint64_t>(component.unitId));
				RewindAuditMix(digest, static_cast<uint64_t>(component.deltaHeading));
				RewindAuditMix(digest, static_cast<uint64_t>(component.changed));
			} else if constexpr (std::is_same_v<Component, MoveTypes::ChangeMainHeadingEvent>) {
				RewindAuditMix(digest, static_cast<uint64_t>(component.unitId));
				RewindAuditMix(digest, static_cast<uint64_t>(component.changed));
			}
		}

		return digest;
	}

	template <typename Component>
	static uint64_t BuildRewindAuditEcsEntityOrderDigest()
	{
		uint64_t digest = 1469598103934665603ull;
		auto view = Sim::registry.view<Component>();

		RewindAuditMix(digest, static_cast<uint64_t>(view.size()));
		for (std::size_t i = 0; i < view.size(); ++i) {
			RewindAuditMix(
				digest,
				static_cast<uint64_t>(entt::to_integral(view.template storage<Component>()[i]))
			);
		}

		return digest;
	}

	static uint64_t BuildRewindAuditChangeHeadingValueDigest()
	{
		uint64_t digest = 1469598103934665603ull;
		auto view = Sim::registry.view<MoveTypes::ChangeHeadingEvent>();

		RewindAuditMix(digest, static_cast<uint64_t>(view.size()));
		for (std::size_t i = 0; i < view.size(); ++i) {
			const entt::entity entity = view.storage<MoveTypes::ChangeHeadingEvent>()[i];
			const auto& component = view.get<MoveTypes::ChangeHeadingEvent>(entity);

			RewindAuditMix(digest, static_cast<uint64_t>(component.unitId));
			RewindAuditMix(digest, static_cast<uint64_t>(component.deltaHeading));
			RewindAuditMix(digest, static_cast<uint64_t>(component.changed));
		}

		return digest;
	}

	struct RewindAuditChangeHeadingSnapshot {
		uint32_t entity = 0;
		int unitId = -1;
		short deltaHeading = 0;
		bool changed = false;
	};

	static void CompareOrStoreRewindAuditChangeHeadingSnapshots(const int frameNum)
	{
		static std::unordered_map<int, std::vector<RewindAuditChangeHeadingSnapshot>> referenceSnapshots;

		auto view = Sim::registry.view<MoveTypes::ChangeHeadingEvent>();
		std::vector<RewindAuditChangeHeadingSnapshot> currentSnapshots;
		currentSnapshots.reserve(view.size());

		for (std::size_t i = 0; i < view.size(); ++i) {
			const entt::entity entity = view.storage<MoveTypes::ChangeHeadingEvent>()[i];
			const auto& component = view.get<MoveTypes::ChangeHeadingEvent>(entity);
			currentSnapshots.push_back({
				static_cast<uint32_t>(entt::to_integral(entity)),
				component.unitId,
				component.deltaHeading,
				component.changed
			});
		}

		const auto refIt = referenceSnapshots.find(frameNum);
		if (refIt == referenceSnapshots.end()) {
			referenceSnapshots.emplace(frameNum, std::move(currentSnapshots));
			return;
		}

		const auto& reference = refIt->second;
		const std::size_t maxSize = std::max(reference.size(), currentSnapshots.size());
		unsigned int totalDiffs = 0;
		unsigned int loggedDiffs = 0;
		constexpr unsigned int maxLoggedDiffs = 12;

		for (std::size_t i = 0; i < maxSize; ++i) {
			const bool haveReference = (i < reference.size());
			const bool haveCurrent = (i < currentSnapshots.size());
			const bool differs = (
				haveReference != haveCurrent ||
				(
					haveReference &&
					haveCurrent &&
					(
						reference[i].entity != currentSnapshots[i].entity ||
						reference[i].unitId != currentSnapshots[i].unitId ||
						reference[i].deltaHeading != currentSnapshots[i].deltaHeading ||
						reference[i].changed != currentSnapshots[i].changed
					)
				)
			);

			if (!differs)
				continue;

			totalDiffs += 1;
			if (loggedDiffs >= maxLoggedDiffs)
				continue;

			const auto ref = haveReference ? reference[i] : RewindAuditChangeHeadingSnapshot{};
			const auto cur = haveCurrent ? currentSnapshots[i] : RewindAuditChangeHeadingSnapshot{};
			AppendRewindAuditLine(fmt::format(
				"heading-detail frame={} index={} reference=[present={} entity={} unit={} delta={} changed={}] current=[present={} entity={} unit={} delta={} changed={}]",
				frameNum,
				i,
				int(haveReference),
				ref.entity,
				ref.unitId,
				ref.deltaHeading,
				int(ref.changed),
				int(haveCurrent),
				cur.entity,
				cur.unitId,
				cur.deltaHeading,
				int(cur.changed)
			));
			loggedDiffs += 1;
		}

		if (totalDiffs > 0) {
			AppendRewindAuditLine(fmt::format(
				"heading-detail-summary frame={} differingEntries={} logged={} referenceSize={} currentSize={}",
				frameNum,
				totalDiffs,
				loggedDiffs,
				reference.size(),
				currentSnapshots.size()
			));
		}
	}

	static void WriteRewindAuditSnapshot(const int frameNum)
	{
		if (!ShouldWriteRewindAuditFrame(frameNum))
			return;

		const int2 queuedPathUpdates = pathManager->GetNumQueuedUpdates();
		const std::vector<const CUnit*> units = GetSortedRewindAuditUnits();
		unsigned int activeFeatures = 0;
		unsigned int activeProjectiles = 0;

		const uint64_t teamDigest = BuildRewindAuditTeamDigest();
		const uint64_t commandDigest = BuildRewindAuditCommandDigest(units);
		const RewindAuditUnitDigests unitDigests = BuildRewindAuditUnitDigests(units);
		const uint64_t featureDigest = BuildRewindAuditFeatureDigest(activeFeatures);
		const uint64_t projectileDigest = BuildRewindAuditProjectileDigest(activeProjectiles);
		const uint64_t generalMoveOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::GeneralMoveType>();
		const uint64_t groundMoveOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::GroundMoveType>();
		const uint64_t unitTrapOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::UnitTrapCheck>();
		const uint64_t unitMovedOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::UnitMovedEvent>();
		const uint64_t changeHeadingOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::ChangeHeadingEvent>();
		const uint64_t changeMainHeadingOrderDigest = BuildRewindAuditEcsOrderDigest<MoveTypes::ChangeMainHeadingEvent>();
		const uint64_t changeHeadingEntityOrderDigest = BuildRewindAuditEcsEntityOrderDigest<MoveTypes::ChangeHeadingEvent>();
		const uint64_t changeHeadingValueDigest = BuildRewindAuditChangeHeadingValueDigest();

		AppendRewindAuditLine(fmt::format(
			"frame={} time={} pathChecksum={} queuedPathUpdates={},{} rngInit={} rngLast={} rngState={} teamDigest={} commandDigest={} unitDigest={} unitIdentityDigest={} unitKinematicsDigest={} unitMoveDigest={} unitIDPoolDigest={} featureDigest={} projectileDigest={} generalMoveOrderDigest={} groundMoveOrderDigest={} unitTrapOrderDigest={} unitMovedOrderDigest={} changeHeadingOrderDigest={} changeMainHeadingOrderDigest={} changeHeadingEntityOrderDigest={} changeHeadingValueDigest={} activeTeams={} activeUnits={} activeFeatures={} activeProjectiles={}",
			frameNum,
			FormatReplayTime(frameNum / GAME_SPEED),
			pathManager->GetPathCheckSum(),
			queuedPathUpdates.x,
			queuedPathUpdates.y,
			static_cast<unsigned long long>(gsRNG.GetInitSeed()),
			static_cast<unsigned long long>(gsRNG.GetLastSeed()),
			static_cast<unsigned long long>(gsRNG.GetGenState()),
			static_cast<unsigned long long>(teamDigest),
			static_cast<unsigned long long>(commandDigest),
			static_cast<unsigned long long>(unitDigests.combinedDigest),
			static_cast<unsigned long long>(unitDigests.identityDigest),
			static_cast<unsigned long long>(unitDigests.kinematicsDigest),
			static_cast<unsigned long long>(unitDigests.moveDigest),
			static_cast<unsigned long long>(unitHandler.GetIDPoolDebugDigest()),
			static_cast<unsigned long long>(featureDigest),
			static_cast<unsigned long long>(projectileDigest),
			static_cast<unsigned long long>(generalMoveOrderDigest),
			static_cast<unsigned long long>(groundMoveOrderDigest),
			static_cast<unsigned long long>(unitTrapOrderDigest),
			static_cast<unsigned long long>(unitMovedOrderDigest),
			static_cast<unsigned long long>(changeHeadingOrderDigest),
			static_cast<unsigned long long>(changeMainHeadingOrderDigest),
			static_cast<unsigned long long>(changeHeadingEntityOrderDigest),
			static_cast<unsigned long long>(changeHeadingValueDigest),
			teamHandler.ActiveTeams(),
			units.size(),
			activeFeatures,
			activeProjectiles
		));

		CompareOrStoreRewindAuditUnitSnapshots(frameNum, units);
		CompareOrStoreRewindAuditWeaponSnapshots(frameNum, units);
		CompareOrStoreRewindAuditChangeHeadingSnapshots(frameNum);
		CompareOrStoreRewindAuditProjectileSnapshots(frameNum);
	}

	struct RewindSimPhaseDigest {
		uint64_t rng = 0;
		uint64_t teams = 0;
		uint64_t commands = 0;
		uint64_t units = 0;
		uint64_t features = 0;
		uint64_t projectiles = 0;
		uint64_t paths = 0;
	};

	static RewindSimPhaseDigest BuildRewindSimPhaseDigest()
	{
		const std::vector<const CUnit*> units = GetSortedRewindAuditUnits();
		const RewindAuditUnitDigests unitDigests = BuildRewindAuditUnitDigests(units);
		unsigned int activeFeatures = 0;
		unsigned int activeProjectiles = 0;

		RewindSimPhaseDigest digest;
		digest.rng = static_cast<uint64_t>(gsRNG.GetGenState());
		digest.teams = BuildRewindAuditTeamDigest();
		digest.commands = BuildRewindAuditCommandDigest(units);
		digest.units = unitDigests.combinedDigest;
		digest.features = BuildRewindAuditFeatureDigest(activeFeatures);
		digest.projectiles = BuildRewindAuditProjectileDigest(activeProjectiles);
		digest.paths = static_cast<uint64_t>(pathManager->GetPathCheckSum());
		RewindAuditMix(digest.paths, static_cast<uint64_t>(pathManager->GetNumQueuedUpdates().x));
		RewindAuditMix(digest.paths, static_cast<uint64_t>(pathManager->GetNumQueuedUpdates().y));
		return digest;
	}

	static void AuditRewindSimPhase(const unsigned int phase, const char* phaseName)
	{
		if (!GetRewindAuditSettings().enabled)
			return;

		static std::unordered_map<uint64_t, RewindSimPhaseDigest> references;
		static int firstDivergentFrame = -1;

		const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(gs->frameNum)) << 8) | phase;
		const RewindSimPhaseDigest current = BuildRewindSimPhaseDigest();
		const auto [it, inserted] = references.emplace(key, current);
		if (inserted)
			return;

		const RewindSimPhaseDigest& reference = it->second;
		const bool differs = (
			reference.rng != current.rng ||
			reference.teams != current.teams ||
			reference.commands != current.commands ||
			reference.units != current.units ||
			reference.features != current.features ||
			reference.projectiles != current.projectiles ||
			reference.paths != current.paths
		);
		if (!differs)
			return;

		if (firstDivergentFrame < 0)
			firstDivergentFrame = gs->frameNum;
		if (gs->frameNum > firstDivergentFrame)
			return;

		LOG(
			"[RewindSimPhaseAudit] frame=%d phase=%s rng=%llu/%llu teams=%llu/%llu commands=%llu/%llu units=%llu/%llu features=%llu/%llu projectiles=%llu/%llu paths=%llu/%llu",
			gs->frameNum,
			phaseName,
			static_cast<unsigned long long>(reference.rng),
			static_cast<unsigned long long>(current.rng),
			static_cast<unsigned long long>(reference.teams),
			static_cast<unsigned long long>(current.teams),
			static_cast<unsigned long long>(reference.commands),
			static_cast<unsigned long long>(current.commands),
			static_cast<unsigned long long>(reference.units),
			static_cast<unsigned long long>(current.units),
			static_cast<unsigned long long>(reference.features),
			static_cast<unsigned long long>(current.features),
			static_cast<unsigned long long>(reference.projectiles),
			static_cast<unsigned long long>(current.projectiles),
			static_cast<unsigned long long>(reference.paths),
			static_cast<unsigned long long>(current.paths)
		);
	}
}

CONFIG(bool, GameEndOnConnectionLoss).defaultValue(true);
// CONFIG(bool, LuaCollectGarbageOnSimFrame).defaultValue(true);

CONFIG(bool, ShowFPS).defaultValue(false).description("Displays current framerate.");
CONFIG(bool, ShowClock).defaultValue(true).headlessValue(false).description("Displays a clock on the top-right corner of the screen showing the elapsed time of the current game.");
CONFIG(bool, ShowSpeed).defaultValue(false).description("Displays current game speed.");
CONFIG(int, ShowPlayerInfo).defaultValue(1).headlessValue(0);
CONFIG(float, GuiOpacity).defaultValue(0.8f).minimumValue(0.0f).maximumValue(1.0f).description("Sets the opacity of the built-in Spring UI. Generally has no effect on LuaUI widgets. Can be set in-game using shift+, to decrease and shift+. to increase.");
CONFIG(std::string, InputTextGeo).defaultValue("");

CONFIG(int, SmoothTimeOffset).defaultValue(0).headlessValue(0).description("Enables frametimeoffset smoothing, 0 = off (old version), -1 = forced 0.5,  1-20 smooth, recommended = 2-3");
CONFIG(bool, RewindAudit).defaultValue(false).description("Enables read-only BAR Rewind audit digests for replay comparison. Writes local files only and never modifies replay files.");
CONFIG(std::string, RewindAuditLabel).defaultValue("audit").description("Output label for BAR Rewind audit logs.");
CONFIG(std::string, RewindAuditFrames).defaultValue("").description("Optional BAR Rewind audit frame list. Accepts frames and mm:ss/hh:mm:ss tokens separated by commas, spaces, or semicolons.");
CONFIG(int, RewindAuditPeriod).defaultValue(900).minimumValue(0).description("BAR Rewind audit period in frames when RewindAudit is enabled. Zero disables periodic snapshots.");
CONFIG(bool, ReplayAutoCheckpoints).defaultValue(true).description("Enables automatic replay rewind checkpoints during replay playback.");

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
	ENTER_SYNCED_CODE();
	LOG("[Game::%s][1]", __func__);

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
			if (luaGCControl == 1 || simFrameDeltaTime > gcForcedDeltaTime)
				eventHandler.CollectGarbage(false);

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
			ActivatePendingReplayResumeRequest();
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
			bool skipPathPostFinalizeRefresh = false;
			if (saveFileHandler != nullptr) {
				if (auto* qtpfsPathManager = dynamic_cast<QTPFS::PathManager*>(pathManager); qtpfsPathManager != nullptr) {
					skipPathPostFinalizeRefresh = qtpfsPathManager->ConsumeLoadSavePostLoadRefreshSkip();
				}
			}

			if (skipPathPostFinalizeRefresh) {
				LOG("[Game::%s] skipping post-load QTPFS refresh because a full rewind path snapshot was restored", __func__);
			} else {
				pathManager->PostFinalizeRefresh();
			}
			Watchdog::ClearTimer(WDT_LOAD);
			LEAVE_SYNCED_CODE();
		}

		if (saveFileHandler != nullptr)
			ActivatePendingReplayResumeRequest();

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
	// The LoadSave handler runs finalization after all embedded Rewind collectors
	// have loaded. Running it here is too early for queued QTPFS and net restores.
}


void CGame::FinalizeLoadSavePostLoad()
{
	RECOIL_DETAILED_TRACY_ZONE;
	GameSetupDrawer::Disable();

	// Reapply only the local unsynced player view. Do not recompute every
	// player's controlledTeams here; the replay checkpoint restored the exact
	// per-player control masks from the save and command playback depends on
	// those masks staying identical to the captured frame.
	if (playerHandler.IsValidPlayer(gu->myPlayerNum)) {
		gu->SetMyPlayer(gu->myPlayerNum);
	}

	// LOS, radar, sonar, and jammer maps are derived runtime state and are not
	// serialized by CREG. Rebuild them before the first resumed simulation frame
	// so Unit::UpdateLosStatus sees the same sensor coverage as an uninterrupted
	// replay instead of one frame of empty maps.
	if (losHandler != nullptr) {
		losHandler->Update();
		// the live-unit rebuild above cannot recreate sight coverage that was
		// lingering from dead units (delayed-delete decay) or left stale by a
		// pending terraform relos; reapply that serialized coverage so a rewound
		// replay sees identical LOS/radar to an uninterrupted one.
		losHandler->RestoreLingerAfterRebuild();
		LOG("[Game::%s] rebuilt LOS and sensor maps from restored units", __func__);
	}

	if (auto* qtpfsPathManager = dynamic_cast<QTPFS::PathManager*>(pathManager); qtpfsPathManager != nullptr) {
		// Older rewind saves only contain path-level restore data and still need a full graph refresh.
		// Newer saves can queue a complete QTPFS node-layer snapshot and should apply it directly.
		if (!qtpfsPathManager->HasQueuedFullLoadSaveState()) {
			LOG("[Game::%s] QTPFS full snapshot not present, rebuilding path graph from restored map state", __func__);
			qtpfsPathManager->TerrainChange(0, 0, mapDims.mapx, mapDims.mapy, 0);
			qtpfsPathManager->PostFinalizeRefresh();
		} else {
			LOG("[Game::%s] QTPFS full snapshot detected, skipping full-map path graph rebuild", __func__);
		}
		qtpfsPathManager->ApplyLoadSaveRestore();

		unsigned int rebuiltGroundMovePaths = 0;
		unsigned int missingGroundMovePaths = 0;
		unsigned int mismatchedGroundMovePaths = 0;
		const bool rebuildAllGroundMovePaths = qtpfsPathManager->HadLoadSaveRestoreNodeLayerChecksumMismatch();

		for (CUnit* unit: unitHandler.GetActiveUnits()) {
			if ((unit == nullptr) || (unit->moveType == nullptr))
				continue;

			auto* groundMoveType = dynamic_cast<CGroundMoveType*>(unit->moveType);
			if (groundMoveType == nullptr)
				continue;
			const unsigned int groundPathId = groundMoveType->GetPathID();
			const bool hasAnyLiveGroundPath = (groundPathId != 0) && qtpfsPathManager->HasLivePath(groundPathId, true);
			const bool hasCompatibleLiveGroundPath = (groundPathId != 0) && qtpfsPathManager->HasCompatibleLivePath(groundPathId, unit, true);
			const bool forcePathRebuild = rebuildAllGroundMovePaths || ((groundPathId != 0) && !hasCompatibleLiveGroundPath);

			if (!forcePathRebuild && !groundMoveType->NeedsLoadSavePathReinit())
				continue;
			if (!forcePathRebuild && hasCompatibleLiveGroundPath)
				continue;

			missingGroundMovePaths += (!hasAnyLiveGroundPath);
			mismatchedGroundMovePaths += (hasAnyLiveGroundPath && !hasCompatibleLiveGroundPath);
			groundMoveType->RebuildPathAfterLoadSaveRestore(forcePathRebuild);
			rebuiltGroundMovePaths += 1;
		}

		if (rebuiltGroundMovePaths > 0 || qtpfsPathManager->NeedsLoadSaveGroundPathRebuild() || rebuildAllGroundMovePaths) {
			LOG("[Game::%s] rebuilt %u active ground-unit paths after QTPFS rewind restore (missingPathIds=%u mismatchedPathOwners=%u rebuildAll=%d)",
				__func__,
				rebuiltGroundMovePaths,
				missingGroundMovePaths,
				mismatchedGroundMovePaths,
				int(rebuildAllGroundMovePaths)
			);
		}
	}

	if (gameSetup != nullptr && gameSetup->replayCheckpoint) {
		featureHandler.RebuildUpdateQueueAfterLoad();
		unitScriptEngine->RebuildAnimatingAfterLoad();
		// reclaim/resurrect arbitration caches are not creg-serialized; rebuild
		// them from the restored command queues so cons do not re-target features
		// already being reclaimed/resurrected (which injected phantom CMD_RECLAIM
		// orders and desynced the resumed demo ~one read-ahead window after the
		// checkpoint). See CBuilderCaches::RepopulateAfterLoad.
		CBuilderCaches::RepopulateAfterLoad();
	}
	Sim::systemUtils.NotifyPostLoad();

	if (gameServer != nullptr) {
		gameServer->PostLoad(gs->frameNum);
	}

	netcode::CLocalConnection::ApplyPendingRestore();

	unsigned int restoredSyncChecksum = 0;
	if (cregLoadSave::ApplyPendingSyncChecksumRestore(restoredSyncChecksum)) {
		netcode::RestoreLocalSyncChecksum(gs->frameNum, restoredSyncChecksum);
		LOG(
			"[Game::%s] restored sync checksum %08x for checkpoint frame %d",
			__func__,
			restoredSyncChecksum,
			gs->frameNum
		);
	}

	if (saveFileHandler != nullptr && gameSetup != nullptr && gameSetup->replayCheckpoint && pendingReplayResumeSkip) {
		pendingReplayResumeSkip = false;

		if (gameServer != nullptr && pendingReplayResumeTargetFrame > gs->frameNum) {
			LOG(
				"[ReplayRewind] Resuming replay from checkpoint and skipping to %s after load finalization",
				FormatReplayTime(static_cast<int>(pendingReplayResumeTargetTime + 0.5f)).c_str()
			);
			gameServer->SkipToReplayFrame(pendingReplayResumeTargetFrame);
		} else if (pendingReplayResumeTargetTime >= 0.0f) {
			LOG(
				"[ReplayRewind] Replay restored to %s",
				FormatReplayTime(static_cast<int>(pendingReplayResumeTargetTime + 0.5f)).c_str()
			);
		}

		pendingReplayResumeTargetFrame = -1;
		pendingReplayResumeTargetTime = -1.0f;
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

	jobDispatcher.Update();
	clientNet->Update();

	// When video recording do step by step simulation, so each simframe gets a corresponding videoframe
	// FIXME: SERVER ALREADY DOES THIS BY ITSELF
	if (playing && gameServer != nullptr && videoCapturing->AllowRecord())
		gameServer->CreateNewFrame(false, true);

	ENTER_SYNCED_CODE();
	SendClientProcUsage();
	ClientReadNet(); // issues new SimFrame()s

	if (!gameOver) {
		if (clientNet->NeedsReconnect())
			clientNet->AttemptReconnect(SpringVersion::GetSync(), Platform::GetPlatformStr());

		if (clientNet->CheckTimeout(0, gs->PreSimFrame()))
			GameEnd({}, true);
	}

	LEAVE_SYNCED_CODE();

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

	if (pendingReplayResumeSkip) {
		pendingReplayResumeSkip = false;

		if (gameServer != nullptr && pendingReplayResumeTargetFrame > gs->frameNum) {
			LOG(
				"[ReplayRewind] Resuming replay from checkpoint and skipping to %s",
				FormatReplayTime(static_cast<int>(pendingReplayResumeTargetTime + 0.5f)).c_str()
			);
			CommandMessage msg(Action("skip " + std::to_string(std::max(0, pendingReplayResumeTargetFrame / GAME_SPEED))), gu->myPlayerNum);
			clientNet->Send(msg.Pack());
		} else if (pendingReplayResumeTargetTime >= 0.0f) {
			LOG(
				"[ReplayRewind] Replay restored to %s",
				FormatReplayTime(static_cast<int>(pendingReplayResumeTargetTime + 0.5f)).c_str()
			);
		}

		pendingReplayResumeTargetFrame = -1;
		pendingReplayResumeTargetTime = -1.0f;
	}

	// set camera
	camHandler->UpdateController(playerHandler.Player(gu->myPlayerNum), gu->fpsMode);

	lineDrawer.UpdateLineStipple();

	icon::iconHandler.Update();
	CNamedTextures::Update();

	// always update InfoTexture and SoundListener at <= 30Hz (even when paused)
	if (newSimFrame || forceUpdate) {
		lastUnsyncedUpdateTime = currentTime;

		// TODO: should be moved to WorldDrawer::Update
		infoTextureHandler->Update();
		// TODO call only when camera changed
		sound->UpdateListener(camera->GetPos(), camera->GetDir(), camera->GetUp());
	}
	SetDrawMode(gameNormalDraw); //TODO move to ::Draw()?

	if (luaUI != nullptr) {
		luaUI->CheckStack();
		luaUI->CheckAction();
	}
	if (luaGaia != nullptr)
		luaGaia->CheckStack();
	if (luaRules != nullptr)
		luaRules->CheckStack();

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
	guihandler->Update();
	commandDrawer->Update();

	{
		SCOPED_TIMER("Update::EventHandler");
		eventHandler.Update();
	}

	if (unitTracker.Enabled())
		unitTracker.SetCam();

	camera->Update();
	shadowHandler.Update();
	{
		worldDrawer.Update(newSimFrame);
		transformsUploader.Update();
		modelUniformsUploader.Update();
	}

	mouse->UpdateCursorCameraDir(); // make sure mouse->dir is in sync with camera

	//Update per-drawFrame UBO
	UniformConstants::GetInstance().Update();

	eventHandler.DbgTimingInfo(TIMING_UNSYNCED, currentTime, spring_now());
	return false;
}


bool CGame::Draw() {
	const spring_time currentTimePreUpdate = spring_gettime();

	if (UpdateUnsynced(currentTimePreUpdate))
		return false;

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

	GameSetupDrawer::Disable();
	CLuaUI::UpdateTeams();

	teamHandler.SetDefaultStartPositions(gameSetup);

	if (saveFileHandler == nullptr) {
		eventHandler.GameStart();
		ActivatePendingReplayResumeRequest();
	}
}

static const char* const tracingSimFrameName = "SimFrame";

static void LogReplayCompatSyncSnapshot(const int frameNum)
{
	WriteRewindAuditSnapshot(frameNum);
}

void CGame::SimFrame() {
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
	LogReplayCompatSyncSnapshot(gs->frameNum);
	AuditRewindSimPhase(0, "start");

	if (!skipping) {
		// everything here is unsynced and should ideally moved to Game::Update()
		waitCommandsAI.Update();
		geometricObjects->Update();
		sound->NewFrame();
		eoh->Update();

		for (auto& grouphandler: uiGroupHandlers)
			grouphandler.Update();

		CPlayer* p = playerHandler.Player(gu->myPlayerNum);
		FPSUnitController& c = p->fpsController;

		c.SendStateUpdate(/*camera->GetMovState(), mouse->buttons*/);

		CTeamHighlight::Update(gs->frameNum);
	}

	// everything from here is simulation
	{
		SCOPED_SPECIAL_TIMER("Sim");

		// Lua unit scripts change piece positions and orientations in eventHandler.GameFrame(gs->frameNum);
		// so we need to save the previous unit state before it happened
		unitHandler.UpdatePreFrame();
		featureHandler.UpdatePreFrame();
		AuditRewindSimPhase(1, "after-pre-frame");

		{
			SCOPED_TIMER("Sim::GameFrame");

			// keep garbage-collection rate tied to sim-speed
			// (fixed 30Hz gc is not enough while catching up)
			if (luaGCControl == 0)
				eventHandler.CollectGarbage(false);

			eventHandler.GameFrame(gs->frameNum);
		}
		AuditRewindSimPhase(2, "after-game-frame");

		helper->Update();
		AuditRewindSimPhase(3, "after-helper");
		readMap->Update();
		smoothGround.UpdateSmoothMesh();
		mapDamage->Update();
		AuditRewindSimPhase(4, "after-map");
		unitHandler.Update();
		AuditRewindSimPhase(5, "after-units");
		pathManager->Update();
		AuditRewindSimPhase(6, "after-paths");
		projectileHandler.Update();
		AuditRewindSimPhase(7, "after-projectiles");
		featureHandler.Update();
		AuditRewindSimPhase(8, "after-features");
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
		AuditRewindSimPhase(9, "after-scripts");
		envResHandler.Update();
		losHandler->Update();
		// dead ghosts have to be updated in sim, after los,
		// to make sure they represent the current knowledge correctly.
		// should probably be split from drawer
		CUnitDrawer::UpdateGhostedBuildings();
		interceptHandler.Update(false);
		AuditRewindSimPhase(10, "after-environment");

		teamHandler.GameFrame(gs->frameNum);
		playerHandler.GameFrame(gs->frameNum);
		eventHandler.GameFramePost(gs->frameNum);
		AuditRewindSimPhase(11, "after-frame-post");
	}

	lastSimFrameTime = spring_gettime();
	gu->avgSimFrameTime = mix(gu->avgSimFrameTime, (lastSimFrameTime - lastFrameTime).toMilliSecsf(), 0.05f);
	gu->avgSimFrameTime = std::max(gu->avgSimFrameTime, 0.01f);

	eventHandler.DbgTimingInfo(TIMING_SIM, lastFrameTime, lastSimFrameTime);

	FrameMarkEnd(tracingSimFrameName);

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
	MaybeCreateAutoReplayCheckpoint(true);
	FlushPendingReplayCheckpointSave();
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

	CEndGameBox::Create(winningAllyTeams);
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
				Channels::UserInterface->PlaySample(chatSound, 5);
			}
		}
		else if (msg.destination == ChatMessage::TO_SPECTATORS) {
			if (gu->spectating || myMsg) {
				LOG("%sSpectators: %s", label.c_str(), s.c_str());
				Channels::UserInterface->PlaySample(chatSound, 5);
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
				Channels::UserInterface->PlaySample(chatSound, 5);
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
				Channels::UserInterface->PlaySample(chatSound, 5);
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
	globalSaveFileData.replayCheckpoint = false;
	globalSaveFileData.replayCheckpointFrame = -1;
	globalSaveFileData.replayCheckpointTime = -1.0f;
	globalSaveFileData.replayCheckpointSchema = 0;
}

void CGame::SaveReplayCheckpoint(std::string&& fileName, std::string&& saveArgs, int checkpointFrame, float checkpointTime)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (pendingReplayCheckpointSave) {
		LOG("[ReplayRewind] A checkpoint request is already waiting for a safe simulation frame");
		return;
	}

	pendingReplayCheckpointFileName = std::move(fileName);
	pendingReplayCheckpointArgs = std::move(saveArgs);
	pendingReplayCheckpointSave = true;
	pendingReplayCheckpointIsAuto = false;
	pendingReplayCheckpointRequestedFrame = checkpointFrame;
	pendingReplayCheckpointRequestedTime = checkpointTime;
	pendingReplayCheckpointLastWaitLogFrame = -1;
}

std::string CGame::GetLocalReplayPlayerName() const
{
	if (gu != nullptr && gu->myPlayerNum >= 0) {
		if (const CPlayer* player = playerHandler.Player(gu->myPlayerNum); player != nullptr) {
			if (!player->name.empty())
				return player->name;
		}
	}

	return "UnnamedPlayer (spec)";
}

std::string CGame::BuildReplayCheckpointLoadScript(const std::string& saveRelativePath) const
{
	return
		"[GAME]\n"
		"{\n"
		"  IsHost=1;\n"
		"  MyPlayerName=" + QuoteTdfValue(GetLocalReplayPlayerName()) + ";\n"
		"  SaveFile=" + QuoteTdfValue(saveRelativePath) + ";\n"
		"}\n\n";
}

std::string CGame::BuildReplayStartScript() const
{
	if (gameSetup == nullptr || gameSetup->demoName.empty())
		return "";

	return
		"[GAME]\n"
		"{\n"
		"  IsHost=1;\n"
		"  MyPlayerName=" + QuoteTdfValue(GetLocalReplayPlayerName()) + ";\n"
		"  Demofile=" + QuoteTdfValue(gameSetup->demoName) + ";\n"
		"}\n\n";
}

bool CGame::QueueReplayReloadScript(std::string&& reloadScript, int targetFrame, float targetTime, bool expectCheckpoint)
{
	if (gameSetup == nullptr || gu == nullptr)
		return false;
	if (reloadScript.empty())
		return false;
	if (!gameSetup->hostDemo)
		return false;

	replayResumeRequest.active = true;
	replayResumeRequest.expectCheckpoint = expectCheckpoint;
	replayResumeRequest.targetFrame = targetFrame;
	replayResumeRequest.targetTime = targetTime;
	replayResumeRequest.demoName = gameSetup->demoName;

	gameSetup->reloadScript = std::move(reloadScript);
	gu->globalReload = true;
	return true;
}

bool CGame::QueueReplayResumeFromCheckpoint(const std::string& saveFileName, int targetFrame, float targetTime)
{
	return QueueReplayReloadScript(BuildReplayCheckpointLoadScript(saveFileName), targetFrame, targetTime, true);
}

bool CGame::QueueReplayResumeFromStart(int targetFrame, float targetTime)
{
	return QueueReplayReloadScript(BuildReplayStartScript(), targetFrame, targetTime, false);
}

void CGame::ActivatePendingReplayResumeRequest()
{
	if (!replayResumeRequest.active)
		return;

	const bool demoMatches = (gameSetup != nullptr && gameSetup->demoName == replayResumeRequest.demoName);
	const bool checkpointStateMatches = (gameSetup != nullptr && gameSetup->replayCheckpoint == replayResumeRequest.expectCheckpoint);

	if (!demoMatches || !checkpointStateMatches) {
		LOG_L(
			L_WARNING,
			"[Game::%s] replay resume request did not match the reloaded session (demoMatches=%d checkpointMatches=%d)",
			__func__,
			demoMatches,
			checkpointStateMatches
		);
		replayResumeRequest = {};
		return;
	}

	pendingReplayResumeTargetFrame = replayResumeRequest.targetFrame;
	pendingReplayResumeTargetTime = replayResumeRequest.targetTime;
	pendingReplayResumeSkip = true;

	replayResumeRequest = {};
}

void CGame::MaybeCreateAutoReplayCheckpoint(bool newSimFrame)
{
	if (!newSimFrame)
		return;
	if (gameSetup == nullptr || !gameSetup->hostDemo)
		return;
	if (!configHandler->GetBool("ReplayAutoCheckpoints"))
		return;
	if (!playing || gameOver)
		return;

	const int currentSeconds = (gs->frameNum / GAME_SPEED);

	// A reloaded checkpoint starts with a fresh CGame instance. Mark the bucket
	// containing the restored frame as handled so it is not immediately saved
	// again on the first simulation frame after loading.
	if (gameSetup->replayCheckpoint && lastReplayAutoCheckpointBucket < 0) {
		lastReplayAutoCheckpointBucket =
			(currentSeconds / replayAutoCheckpointInterval) * replayAutoCheckpointInterval;
	}

	if (currentSeconds < replayAutoCheckpointInterval)
		return;

	const int bucket = (currentSeconds / replayAutoCheckpointInterval) * replayAutoCheckpointInterval;
	if (bucket <= 0 || bucket == lastReplayAutoCheckpointBucket)
		return;

	lastReplayAutoCheckpointBucket = bucket;

	if (pendingReplayCheckpointSave) {
		LOG(
			"[ReplayRewind] Skipped auto-checkpoint at %s because another checkpoint request is still pending",
			FormatReplayTime(bucket).c_str()
		);
		return;
	}

	if (gameServer == nullptr || !gameServer->IsReplayCheckpointCaptureSafe(gs->frameNum)) {
		LOG(
			"[ReplayRewind] Skipped auto-checkpoint at %s because replay data is ahead of the simulation",
			FormatReplayTime(bucket).c_str()
		);
		return;
	}

	SaveReplayCheckpoint(
		fmt::format("Saves/rrw_cp_{:06d}.ssf", bucket),
		"-y",
		gs->frameNum,
		static_cast<float>(currentSeconds)
	);
	pendingReplayCheckpointIsAuto = true;
	LOG("[ReplayRewind] Auto-checkpoint requested at %s", FormatReplayTime(bucket).c_str());
}

void CGame::FlushPendingReplayCheckpointSave()
{
	if (!pendingReplayCheckpointSave)
		return;
	if (pendingReplayCheckpointFileName.empty()) {
		pendingReplayCheckpointSave = false;
		pendingReplayCheckpointIsAuto = false;
		pendingReplayCheckpointRequestedFrame = -1;
		pendingReplayCheckpointRequestedTime = -1.0f;
		pendingReplayCheckpointLastWaitLogFrame = -1;
		return;
	}

	if (gameServer == nullptr || !gameServer->IsReplayCheckpointCaptureSafe(gs->frameNum)) {
		if (pendingReplayCheckpointLastWaitLogFrame < 0 || (gs->frameNum - pendingReplayCheckpointLastWaitLogFrame) >= GAME_SPEED) {
			LOG("[ReplayRewind] Checkpoint request is waiting for replay network data to align with frame %d", gs->frameNum);
			pendingReplayCheckpointLastWaitLogFrame = gs->frameNum;
		}
		return;
	}

	unsigned int pendingQTPFSPaths = 0;
	if (auto* qtpfsPathManager = dynamic_cast<QTPFS::PathManager*>(pathManager); qtpfsPathManager != nullptr)
		pendingQTPFSPaths = qtpfsPathManager->GetNumPendingLoadSavePaths();

	if (pendingQTPFSPaths > 0) {
		if (pendingReplayCheckpointLastWaitLogFrame < 0 || (gs->frameNum - pendingReplayCheckpointLastWaitLogFrame) >= GAME_SPEED) {
			LOG(
				"[ReplayRewind] Checkpoint request is waiting for %u pending QTPFS path request%s",
				pendingQTPFSPaths,
				(pendingQTPFSPaths == 1) ? "" : "s"
			);
			pendingReplayCheckpointLastWaitLogFrame = gs->frameNum;
		}
		return;
	}

	const int requestedFrame = pendingReplayCheckpointRequestedFrame;
	const float requestedTime = pendingReplayCheckpointRequestedTime;
	const bool autoCheckpoint = pendingReplayCheckpointIsAuto;

	SaveFileData fileData;
	fileData.name = std::move(pendingReplayCheckpointFileName);
	fileData.args = std::move(pendingReplayCheckpointArgs);
	fileData.replayCheckpoint = true;
	fileData.replayCheckpointFrame = gs->frameNum;
	fileData.replayCheckpointTime = gs->frameNum * INV_GAME_SPEED;
	fileData.replayCheckpointSchema = 1;

	pendingReplayCheckpointFileName.clear();
	pendingReplayCheckpointArgs.clear();
	pendingReplayCheckpointSave = false;
	pendingReplayCheckpointIsAuto = false;
	pendingReplayCheckpointRequestedFrame = -1;
	pendingReplayCheckpointRequestedTime = -1.0f;
	pendingReplayCheckpointLastWaitLogFrame = -1;

	if (requestedFrame >= 0 && requestedFrame != gs->frameNum) {
		LOG(
			"[ReplayRewind] Checkpoint requested at %s became safe at %s after waiting %d frame%s",
			FormatReplayTime(std::max(0, static_cast<int>(requestedTime))).c_str(),
			FormatReplayTime(std::max(0, gs->frameNum / GAME_SPEED)).c_str(),
			gs->frameNum - requestedFrame,
			((gs->frameNum - requestedFrame) == 1) ? "" : "s"
		);
	}
	if (autoCheckpoint)
		LOG("[ReplayRewind] Auto-checkpoint capture started at %s", FormatReplayTime(std::max(0, gs->frameNum / GAME_SPEED)).c_str());

	ILoadSaveHandler::CreateSave(std::move(fileData));
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
