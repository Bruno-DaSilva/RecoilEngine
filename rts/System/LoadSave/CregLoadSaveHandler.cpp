/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <sstream>
#include <zlib.h>

#include "ExternalAI/SkirmishAIHandler.h"
#include "ExternalAI/EngineOutHandler.h"
#include "CregLoadSaveHandler.h"
#include "Map/MapDamage.h"
#include "Map/ReadMap.h"
#include "Game/Game.h"
#include "Game/GameHelper.h"
#include "Game/GameSetup.h"
#include "Game/GameVersion.h"
#include "Game/GlobalUnsynced.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/WaitCommandsAI.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/UI/Groups/GroupHandler.h"
#include "Lua/LuaGaia.h"
#include "Lua/LuaRules.h"
#include "Net/GameServer.h"
#include "System/LoadSave/DemoReader.h"
#include "Rendering/Textures/ColorMap.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Env/Decals/GroundDecalHandler.h"
#include "Sim/Ecs/Helper.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Misc/BuildingMaskMap.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/InterceptHandler.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/CategoryHandler.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Path/QTPFS/PathManager.h"
#include "Sim/Misc/Wind.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/CommandAI/CommandDescription.h"
#include "Sim/Units/Scripts/CobEngine.h"
#include "Sim/Units/Scripts/UnitScriptEngine.h"
#include "Sim/Units/Scripts/NullUnitScript.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "System/SafeUtil.h"
#include "System/Platform/errorhandler.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/FileSystem/GZFileHandler.h"
#include "System/Threading/ThreadPool.h"
#include "System/creg/SerializeLuaState.h"
#include "System/creg/Serializer.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/Sync/SyncChecker.h"
#include "System/TdfParser.h"

#include <future>

#define MAX_STRING_SIZE (1 << 19) // 512kB excluding null-term


CCregLoadSaveHandler::CCregLoadSaveHandler()
{}

CCregLoadSaveHandler::~CCregLoadSaveHandler() = default;

namespace {
	bool pendingSyncChecksumRestore = false;
	unsigned int pendingSyncChecksum = 0;

	// The checkpoint gzip write runs asynchronously (SaveGame). We keep its
	// future here so a subsequent load (e.g. rewinding to a just-created
	// checkpoint) can block until the file is fully flushed; otherwise the
	// reader gets a truncated stream and creg deserialization fails.
	std::future<void> pendingCheckpointWriteFuture;

	void WaitForPendingCheckpointWrite()
	{
		if (pendingCheckpointWriteFuture.valid())
			pendingCheckpointWriteFuture.wait();
	}
}

bool cregLoadSave::ApplyPendingSyncChecksumRestore(unsigned int& checksum)
{
#ifdef SYNCCHECK
	if (!pendingSyncChecksumRestore)
		return false;

	checksum = pendingSyncChecksum;
	CSyncChecker::SetChecksum(checksum);
	pendingSyncChecksumRestore = false;
	return true;
#else
	checksum = 0;
	return false;
#endif
}

#ifdef USING_CREG
#ifdef SYNCCHECK
class CSyncCheckerStateCollector
{
	CR_DECLARE_STRUCT(CSyncCheckerStateCollector)

public:
	CSyncCheckerStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	unsigned int checksum = 0xfade1eaf;
};

CR_BIND(CSyncCheckerStateCollector, )
CR_REG_METADATA(CSyncCheckerStateCollector, (
	CR_MEMBER(checksum),
	CR_POSTLOAD(PostLoad)
))

void CSyncCheckerStateCollector::CaptureForSave()
{
	checksum = CSyncChecker::GetChecksum();
}

void CSyncCheckerStateCollector::PostLoad()
{
	// Final load processing mutates additional synced runtime state. Apply the
	// captured checksum only after all post-load finalization has completed.
	pendingSyncChecksum = checksum;
	pendingSyncChecksumRestore = true;
}
#endif

class CSelectedUnitsStateCollector
{
	CR_DECLARE_STRUCT(CSelectedUnitsStateCollector)

public:
	CSelectedUnitsStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	std::vector<std::vector<int>> netSelected;
};

CR_BIND(CSelectedUnitsStateCollector, )
CR_REG_METADATA(CSelectedUnitsStateCollector, (
	CR_MEMBER(netSelected),
	CR_POSTLOAD(PostLoad)
))

void CSelectedUnitsStateCollector::CaptureForSave()
{
	netSelected = selectedUnitsHandler.netSelected;
}

void CSelectedUnitsStateCollector::PostLoad()
{
	selectedUnitsHandler.netSelected = netSelected;

	if (selectedUnitsHandler.netSelected.size() < playerHandler.ActivePlayers())
		selectedUnitsHandler.netSelected.resize(playerHandler.ActivePlayers());
}

class CWaitingDamageStateCollector
{
	CR_DECLARE_STRUCT(CWaitingDamageStateCollector)

public:
	CWaitingDamageStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	std::vector<unsigned int> slotOffsets;
	std::vector<int> attackerIDs;
	std::vector<int> targetIDs;
	std::vector<int> weaponIDs;
	std::vector<int> projectileIDs;
	std::vector<DamageArray> damages;
	std::vector<float3> impulses;
};

CR_BIND(CWaitingDamageStateCollector, )
CR_REG_METADATA(CWaitingDamageStateCollector, (
	CR_MEMBER(slotOffsets),
	CR_MEMBER(attackerIDs),
	CR_MEMBER(targetIDs),
	CR_MEMBER(weaponIDs),
	CR_MEMBER(projectileIDs),
	CR_MEMBER(damages),
	CR_MEMBER(impulses),
	CR_POSTLOAD(PostLoad)
))

void CWaitingDamageStateCollector::CaptureForSave()
{
	helper->CaptureWaitingDamageState(
		slotOffsets,
		attackerIDs,
		targetIDs,
		weaponIDs,
		projectileIDs,
		damages,
		impulses
	);
}

void CWaitingDamageStateCollector::PostLoad()
{
	helper->RestoreWaitingDamageState(
		slotOffsets,
		attackerIDs,
		targetIDs,
		weaponIDs,
		projectileIDs,
		damages,
		impulses
	);
}

class CGlobalSyncedRNGStateCollector
{
	CR_DECLARE_STRUCT(CGlobalSyncedRNGStateCollector)

public:
	CGlobalSyncedRNGStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	CGlobalSyncedRNG::rng_val_type initSeed = 0;
	CGlobalSyncedRNG::rng_val_type lastSeed = 0;
	CGlobalSyncedRNG::rng_val_type genState = 0;
};

CR_BIND(CGlobalSyncedRNGStateCollector, )
CR_REG_METADATA(CGlobalSyncedRNGStateCollector, (
	CR_MEMBER(initSeed),
	CR_MEMBER(lastSeed),
	CR_MEMBER(genState),
	CR_POSTLOAD(PostLoad)
))

void CGlobalSyncedRNGStateCollector::CaptureForSave()
{
	initSeed = gsRNG.GetInitSeed();
	lastSeed = gsRNG.GetLastSeed();
	genState = gsRNG.GetGenState();
}

void CGlobalSyncedRNGStateCollector::PostLoad()
{
	gsRNG.SetState(initSeed, lastSeed, genState);
}

class CReplayCheckpointNetStateCollector
{
	CR_DECLARE_STRUCT(CReplayCheckpointNetStateCollector)

public:
	CReplayCheckpointNetStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	bool valid = false;
	bool demoStateValid = false;
	int demoFilePos = 0;
	int demoBytesRemaining = 0;
	float demoTimeOffset = 0.0f;
	float demoNextReadTime = 0.0f;
	float demoChunkModGameTime = 0.0f;
	unsigned int demoChunkLength = 0;
	unsigned int localConnectionInstances = 0;
	std::vector<std::vector<uint8_t>> localConnectionQueue0;
	std::vector<std::vector<uint8_t>> localConnectionQueue1;
	int replayServerFrameNum = -1;
	float replayGameTime = 0.0f;
	float replayModGameTime = 0.0f;
	float replayStartTime = 0.0f;
};

CR_BIND(CReplayCheckpointNetStateCollector, )
CR_REG_METADATA(CReplayCheckpointNetStateCollector, (
	CR_MEMBER(valid),
	CR_MEMBER(demoStateValid),
	CR_MEMBER(demoFilePos),
	CR_MEMBER(demoBytesRemaining),
	CR_MEMBER(demoTimeOffset),
	CR_MEMBER(demoNextReadTime),
	CR_MEMBER(demoChunkModGameTime),
	CR_MEMBER(demoChunkLength),
	CR_MEMBER(localConnectionInstances),
	CR_MEMBER(localConnectionQueue0),
	CR_MEMBER(localConnectionQueue1),
	CR_MEMBER(replayServerFrameNum),
	CR_MEMBER(replayGameTime),
	CR_MEMBER(replayModGameTime),
	CR_MEMBER(replayStartTime),
	CR_POSTLOAD(PostLoad)
))

void CReplayCheckpointNetStateCollector::CaptureForSave()
{
	valid = false;
	DemoReaderStreamState capturedDemoState;
	demoStateValid = false;
	demoFilePos = 0;
	demoBytesRemaining = 0;
	demoTimeOffset = 0.0f;
	demoNextReadTime = 0.0f;
	demoChunkModGameTime = 0.0f;
	demoChunkLength = 0;
	localConnectionInstances = 0;
	localConnectionQueue0.clear();
	localConnectionQueue1.clear();
	replayServerFrameNum = -1;
	replayGameTime = 0.0f;
	replayModGameTime = 0.0f;
	replayStartTime = 0.0f;

	if (!globalSaveFileData.replayCheckpoint)
		return;
	if (gameServer == nullptr)
		return;

	valid = gameServer->CaptureReplayCheckpointNetState(
		globalSaveFileData.replayCheckpointFrame,
		capturedDemoState,
		localConnectionInstances,
		localConnectionQueue0,
		localConnectionQueue1,
		replayServerFrameNum,
		replayGameTime,
		replayModGameTime,
		replayStartTime
	);

	demoStateValid = capturedDemoState.valid;
	demoFilePos = capturedDemoState.filePos;
	demoBytesRemaining = capturedDemoState.bytesRemaining;
	demoTimeOffset = capturedDemoState.demoTimeOffset;
	demoNextReadTime = capturedDemoState.nextDemoReadTime;
	demoChunkModGameTime = capturedDemoState.chunkModGameTime;
	demoChunkLength = capturedDemoState.chunkLength;

	LOG("[ReplayRewind] captured replay net state: valid=%d serverFrame=%d demoFilePos=%d demoNextTime=%.3f q0=%u q1=%u",
		int(valid),
		replayServerFrameNum,
		demoFilePos,
		demoNextReadTime,
		static_cast<unsigned int>(localConnectionQueue0.size()),
		static_cast<unsigned int>(localConnectionQueue1.size())
	);
}

void CReplayCheckpointNetStateCollector::PostLoad()
{
	if (!valid)
		return;
	if (gameServer == nullptr)
		return;

	DemoReaderStreamState restoredDemoState;
	restoredDemoState.valid = demoStateValid;
	restoredDemoState.filePos = demoFilePos;
	restoredDemoState.bytesRemaining = demoBytesRemaining;
	restoredDemoState.demoTimeOffset = demoTimeOffset;
	restoredDemoState.nextDemoReadTime = demoNextReadTime;
	restoredDemoState.chunkModGameTime = demoChunkModGameTime;
	restoredDemoState.chunkLength = demoChunkLength;

	const bool restored = gameServer->RestoreReplayCheckpointNetState(
		restoredDemoState,
		localConnectionInstances,
		localConnectionQueue0,
		localConnectionQueue1,
		replayServerFrameNum,
		replayGameTime,
		replayModGameTime,
		replayStartTime
	);

	LOG("[ReplayRewind] restored replay net state: restored=%d serverFrame=%d demoFilePos=%d demoNextTime=%.3f q0=%u q1=%u",
		int(restored),
		replayServerFrameNum,
		demoFilePos,
		demoNextReadTime,
		static_cast<unsigned int>(localConnectionQueue0.size()),
		static_cast<unsigned int>(localConnectionQueue1.size())
	);
}

using CQTPFSLoadSavePathNode = QTPFS::LoadSavePathNode;
CR_BIND(CQTPFSLoadSavePathNode, )
CR_REG_METADATA(CQTPFSLoadSavePathNode, (
	CR_MEMBER(nodeId),
	CR_MEMBER(nodeNumber),
	CR_MEMBER(netPointX),
	CR_MEMBER(netPointY),
	CR_MEMBER(pathPointIndex),
	CR_MEMBER(xmin),
	CR_MEMBER(zmin),
	CR_MEMBER(xmax),
	CR_MEMBER(zmax),
	CR_MEMBER(badNode)
))

using CQTPFSLoadSaveNetPoint = QTPFS::LoadSaveNetPoint;
CR_BIND(CQTPFSLoadSaveNetPoint, )
CR_REG_METADATA(CQTPFSLoadSaveNetPoint, (
	CR_MEMBER(x),
	CR_MEMBER(y)
))

using CQTPFSLoadSaveNodeNeighbour = QTPFS::LoadSaveNodeNeighbour;
CR_BIND(CQTPFSLoadSaveNodeNeighbour, )
CR_REG_METADATA(CQTPFSLoadSaveNodeNeighbour, (
	CR_MEMBER(nodeId),
	CR_MEMBER(netPoints)
))

using CQTPFSLoadSaveQTNodeState = QTPFS::LoadSaveQTNodeState;
CR_BIND(CQTPFSLoadSaveQTNodeState, )
CR_REG_METADATA(CQTPFSLoadSaveQTNodeState, (
	CR_MEMBER(active),
	CR_MEMBER(poolIndex),
	CR_MEMBER(nodeNumber),
	CR_MEMBER(rawIndex),
	CR_MEMBER(childBaseIndex),
	CR_MEMBER(xmin),
	CR_MEMBER(xmax),
	CR_MEMBER(zmin),
	CR_MEMBER(zmax),
	CR_MEMBER(moveCostAvg),
	CR_MEMBER(neighbours)
))

using CQTPFSLoadSaveNodeLayerState = QTPFS::LoadSaveNodeLayerState;
CR_BIND(CQTPFSLoadSaveNodeLayerState, )
CR_REG_METADATA(CQTPFSLoadSaveNodeLayerState, (
	CR_MEMBER(layerNumber),
	CR_MEMBER(numLeafNodes),
	CR_MEMBER(updateCounter),
	CR_MEMBER(numOpenNodes),
	CR_MEMBER(numClosedNodes),
	CR_MEMBER(maxNodesAlloced),
	CR_MEMBER(numRootNodes),
	CR_MEMBER(xRootNodes),
	CR_MEMBER(zRootNodes),
	CR_MEMBER(rootNodeSize),
	CR_MEMBER(rootMask),
	CR_MEMBER(xsize),
	CR_MEMBER(zsize),
	CR_MEMBER(maxRelSpeedMod),
	CR_MEMBER(avgRelSpeedMod),
	CR_MEMBER(useShortestPath),
	CR_MEMBER(recycledNodeIndices),
	CR_MEMBER(poolNodeStates)
))

using CQTPFSLoadSaveMapChangeTrack = QTPFS::LoadSaveMapChangeTrack;
CR_BIND(CQTPFSLoadSaveMapChangeTrack, )
CR_REG_METADATA(CQTPFSLoadSaveMapChangeTrack, (
	CR_MEMBER(damageMap),
	CR_MEMBER(damageQueue)
))

using CQTPFSLoadSavePathRecord = QTPFS::LoadSavePathRecord;
CR_BIND(CQTPFSLoadSavePathRecord, )
CR_REG_METADATA(CQTPFSLoadSavePathRecord, (
	CR_MEMBER(entityId),
	CR_MEMBER(kind),
	CR_MEMBER(ownerId),
	CR_MEMBER(ownerTeam),
	CR_MEMBER(ownerCreationFrame),
	CR_MEMBER(ownerMoveDefPathType),
	CR_MEMBER(pathType),
	CR_MEMBER(nextPointIndex),
	CR_MEMBER(repathAtPointIndex),
	CR_MEMBER(numPathUpdates),
	CR_MEMBER(firstNodeIdOfCleanPath),
	CR_MEMBER(hashLow),
	CR_MEMBER(hashHigh),
	CR_MEMBER(virtualHashLow),
	CR_MEMBER(virtualHashHigh),
	CR_MEMBER(radius),
	CR_MEMBER(synced),
	CR_MEMBER(haveFullPath),
	CR_MEMBER(havePartialPath),
	CR_MEMBER(boundingBoxOverride),
	CR_MEMBER(isRawPath),
	CR_MEMBER(hasRequeueComponent),
	CR_MEMBER(requeueSearch),
	CR_MEMBER(hasDirtyComponent),
	CR_MEMBER(hasToBeUpdatedComponent),
	CR_MEMBER(wasTempPath),
	CR_MEMBER(hadSearchRef),
	CR_MEMBER(hadUpdatedCounterIncrease),
	CR_MEMBER(pendingSearchRawPathCheck),
	CR_MEMBER(pendingSearchAllowPartialSearch),
	CR_MEMBER(pendingSearchTryPathRepair),
	CR_MEMBER(hasSharedPathChain),
	CR_MEMBER(isSharedPathHead),
	CR_MEMBER(sharedPathPrevId),
	CR_MEMBER(sharedPathNextId),
	CR_MEMBER(hasPartialSharedPathChain),
	CR_MEMBER(isPartialSharedPathHead),
	CR_MEMBER(partialSharedPathPrevId),
	CR_MEMBER(partialSharedPathNextId),
	CR_MEMBER(points),
	CR_MEMBER(nodes),
	CR_MEMBER(boundingBoxMins),
	CR_MEMBER(boundingBoxMaxs),
	CR_MEMBER(goalPosition)
))

using CQTPFSLoadSaveState = QTPFS::LoadSaveState;
CR_BIND(CQTPFSLoadSaveState, )
CR_REG_METADATA(CQTPFSLoadSaveState, (
	CR_MEMBER(paths),
	CR_MEMBER(nodeLayerStates),
	CR_MEMBER(mapChangeTrackers),
	CR_MEMBER(registryEntitySnapshot),
	CR_MEMBER(damageTrackWidth),
	CR_MEMBER(damageTrackHeight),
	CR_MEMBER(damageTrackCellSize),
	CR_MEMBER(updateDirtyPathRate),
	CR_MEMBER(updateDirtyPathRemainder),
	CR_MEMBER(refreshDirtyPathRateFrame),
	CR_MEMBER(pfsChecksum),
	CR_MEMBER(nodeLayerChecksum),
	CR_MEMBER(skippedPendingPaths),
	CR_MEMBER(skippedUnsyncedPaths)
))

class CQTPFSStateCollector
{
	CR_DECLARE_STRUCT(CQTPFSStateCollector)

public:
	CQTPFSStateCollector() = default;

	void CaptureForSave();
	void PostLoad();

	QTPFS::LoadSaveState state;
};

CR_BIND(CQTPFSStateCollector, )
CR_REG_METADATA(CQTPFSStateCollector, (
	CR_MEMBER(state),
	CR_POSTLOAD(PostLoad)
))

void CQTPFSStateCollector::CaptureForSave()
{
	auto* qtpfsPathManager = dynamic_cast<QTPFS::PathManager*>(pathManager);
	if (qtpfsPathManager == nullptr) {
		return;
	}

	qtpfsPathManager->CaptureLoadSaveState(state);
}

void CQTPFSStateCollector::PostLoad()
{
	auto* qtpfsPathManager = dynamic_cast<QTPFS::PathManager*>(pathManager);
	if (qtpfsPathManager == nullptr) {
		return;
	}

	qtpfsPathManager->QueueLoadSaveRestore(state);
}

class CLoadSavePostLoadFinalizer
{
	CR_DECLARE_STRUCT(CLoadSavePostLoadFinalizer)

public:
	CLoadSavePostLoadFinalizer() = default;

	void PostLoad();
};

CR_BIND(CLoadSavePostLoadFinalizer, )
CR_REG_METADATA(CLoadSavePostLoadFinalizer, (
	CR_POSTLOAD(PostLoad)
))

void CLoadSavePostLoadFinalizer::PostLoad()
{
	// Kept as a serialized marker for save compatibility. The actual rewind
	// finalization is run by CCregLoadSaveHandler::LoadGame after all load/save
	// ECS event pointers have been resolved.
}

class CGameStateCollector
{
	CR_DECLARE_STRUCT(CGameStateCollector)

public:
	CGameStateCollector() = default;

	void Serialize(creg::ISerializer* s);

private:
#ifdef SYNCCHECK
	CSyncCheckerStateCollector syncCheckerState;
#endif
	CGlobalSyncedRNGStateCollector globalSyncedRNGState;
	CReplayCheckpointNetStateCollector replayCheckpointNetState;
	CQTPFSStateCollector qtpfsState;
	CSelectedUnitsStateCollector selectedUnitsState;
	CWaitingDamageStateCollector waitingDamageState;
	CLoadSavePostLoadFinalizer postLoadFinalizer;
};

CR_BIND(CGameStateCollector, )
CR_REG_METADATA(CGameStateCollector, (
	CR_SERIALIZER(Serialize)
))


void CGameStateCollector::Serialize(creg::ISerializer* s)
{
	if (s->IsWriting()) {
#ifdef SYNCCHECK
		syncCheckerState.CaptureForSave();
#endif
		globalSyncedRNGState.CaptureForSave();
		replayCheckpointNetState.CaptureForSave();
		qtpfsState.CaptureForSave();
		selectedUnitsState.CaptureForSave();
		waitingDamageState.CaptureForSave();
	}

	s->SerializeObjectInstance(gs, gs->GetClass());
	s->SerializeObjectInstance(gu, gu->GetClass());
	s->SerializeObjectInstance(gameSetup, gameSetup->GetClass());
	s->SerializeObjectInstance(&playerHandler, playerHandler.GetClass());
	s->SerializeObjectInstance(game, game->GetClass());
#ifdef SYNCCHECK
	s->SerializeObjectInstance(&syncCheckerState, syncCheckerState.GetClass());
#endif
	s->SerializeObjectInstance(&globalSyncedRNGState, globalSyncedRNGState.GetClass());
	s->SerializeObjectInstance(&replayCheckpointNetState, replayCheckpointNetState.GetClass());
	s->SerializeObjectInstance(&qtpfsState, qtpfsState.GetClass());
	s->SerializeObjectInstance(&selectedUnitsState, selectedUnitsState.GetClass());
	s->SerializeObjectInstance(&waitingDamageState, waitingDamageState.GetClass());
	mapDamage->Serialize(s);
	s->SerializeObjectInstance(readMap, readMap->GetClass());
	s->SerializeObjectInstance(&smoothGround, smoothGround.GetClass());
	s->SerializeObjectInstance(&quadField, quadField.GetClass());
	s->SerializeObjectInstance(&unitHandler, unitHandler.GetClass());
	s->SerializeObjectInstance(&globalUnitParams, globalUnitParams.GetClass());
	s->SerializeObjectInstance(cobEngine, cobEngine->GetClass());
	s->SerializeObjectInstance(unitScriptEngine, unitScriptEngine->GetClass());
	s->SerializeObjectInstance(&CNullUnitScript::value, CNullUnitScript::value.GetClass());
	s->SerializeObjectInstance(&featureHandler, featureHandler.GetClass());
	s->SerializeObjectInstance(losHandler, losHandler->GetClass());
	s->SerializeObjectInstance(&interceptHandler, interceptHandler.GetClass());
	s->SerializeObjectInstance(CCategoryHandler::Instance(), CCategoryHandler::Instance()->GetClass());
	s->SerializeObjectInstance(&groundBlockingObjectMap, groundBlockingObjectMap.GetClass());
	s->SerializeObjectInstance(&yardmapStatusEffectsMap, yardmapStatusEffectsMap.GetClass());
	s->SerializeObjectInstance(&buildingMaskMap, buildingMaskMap.GetClass());
	s->SerializeObjectInstance(&projectileHandler, projectileHandler.GetClass());
	CPlasmaRepulser::SerializeShieldSegmentCollectionPool(s);
	CColorMap::SerializeColorMaps(s);
	s->SerializeObjectInstance(&waitCommandsAI, waitCommandsAI.GetClass());
	s->SerializeObjectInstance(&envResHandler, envResHandler.GetClass());
	s->SerializeObjectInstance(&moveDefHandler, moveDefHandler.GetClass());
	s->SerializeObjectInstance(&teamHandler, teamHandler.GetClass());
	for (int a = 0; a < teamHandler.ActiveTeams(); a++) {
		s->SerializeObjectInstance(&uiGroupHandlers[a], uiGroupHandlers[a].GetClass());
	}
	s->SerializeObjectInstance(&commandDescriptionCache, commandDescriptionCache.GetClass());
	CSkirmishAIHandler::SerializeSkirmishAIHandler(s);
	s->SerializeObjectInstance(eoh, eoh->GetClass());
	std::unique_ptr<creg::IType> mapType = creg::DeduceType<decltype(CSplitLuaHandle::gameParams)>::Get();
	mapType->Serialize(s, &CSplitLuaHandle::gameParams);

	s->SerializeObjectInstance(CUnitDrawer::modelDrawerData->GetSavedData(), CUnitDrawer::modelDrawerData->GetSavedData()->GetClass());
	s->SerializeObjectInstance(groundDecals, groundDecals->GetClass());
	s->SerializeObjectInstance(&postLoadFinalizer, postLoadFinalizer.GetClass());
}


class CLuaStateCollector
{
	CR_DECLARE_STRUCT(CLuaStateCollector)

public:
	CLuaStateCollector() = default;
	void Read(const CSplitLuaHandle* handle);
	void Write(CSplitLuaHandle* handle);

	bool valid;
	lua_State* L = nullptr;
	lua_State* L_GC = nullptr;
	std::vector<bool> watchUnitDefs;        // callin masks for Unit*Collision, UnitMoveFailed
	std::vector<bool> watchFeatureDefs;     // callin masks for UnitFeatureCollision
	std::vector<bool> watchProjectileDefs;  // callin masks for Projectile*
	std::vector<bool> watchExplosionDefs;   // callin masks for Explosion
	std::vector<bool> watchAllowTargetDefs; // callin masks for AllowWeapon*Target*
	decltype(CLuaHandle::delayedCallsByFrame) delayedCallsByFrame;

	void Serialize(creg::ISerializer* s);
};

CR_BIND(CLuaStateCollector, )
CR_REG_METADATA(CLuaStateCollector, (
	CR_MEMBER(valid),
	CR_IGNORED(L),
	CR_IGNORED(L_GC),
	CR_MEMBER(watchUnitDefs),
	CR_MEMBER(watchFeatureDefs),
	CR_MEMBER(watchProjectileDefs),
	CR_MEMBER(watchExplosionDefs),
	CR_MEMBER(watchAllowTargetDefs),
	CR_MEMBER(delayedCallsByFrame),
	CR_SERIALIZER(Serialize)
))

void CLuaStateCollector::Read(const CSplitLuaHandle* handle) {
	valid = (handle != nullptr) && handle->syncedLuaHandle.IsValid();
	if (!valid)
		return;

	L = handle->syncedLuaHandle.GetLuaState();
	L_GC = handle->syncedLuaHandle.GetLuaGCState();
	watchUnitDefs = handle->syncedLuaHandle.watchUnitDefs;
	watchFeatureDefs = handle->syncedLuaHandle.watchFeatureDefs;
	watchProjectileDefs = handle->syncedLuaHandle.watchProjectileDefs;
	watchExplosionDefs = handle->syncedLuaHandle.watchExplosionDefs;
	watchAllowTargetDefs = handle->syncedLuaHandle.watchAllowTargetDefs;

	/* This container only holds indexes to the Lua registry, which is
	 * saved alongside the rest of the Lua state since it's fundamentally
	 * just a regular Lua table. So just a shallow copy is sufficient. */
	delayedCallsByFrame = handle->syncedLuaHandle.delayedCallsByFrame;

	lua_gc(L_GC, LUA_GCCOLLECT, 0);
}

void CLuaStateCollector::Write(CSplitLuaHandle* handle) {
	if ((handle == nullptr) || !handle->syncedLuaHandle.IsValid() || !valid)
		return;

	handle->SwapSyncedHandle(L, L_GC);
	handle->syncedLuaHandle.watchUnitDefs = watchUnitDefs;
	handle->syncedLuaHandle.watchFeatureDefs = watchFeatureDefs;
	handle->syncedLuaHandle.watchProjectileDefs = watchProjectileDefs;
	handle->syncedLuaHandle.watchExplosionDefs = watchExplosionDefs;
	handle->syncedLuaHandle.watchAllowTargetDefs = watchAllowTargetDefs;
	handle->syncedLuaHandle.delayedCallsByFrame = delayedCallsByFrame;
}

void CLuaStateCollector::Serialize(creg::ISerializer* s) {
	if (!valid)
		return;

	creg::SerializeLuaState(s, &L);
	creg::SerializeLuaThread(s, &L_GC);
}

static void WriteString(std::ostream& s, const std::string& str)
{
	if (str.length() > MAX_STRING_SIZE)
		throw content_error("[creg::WriteString] string \"" + str + "\" too long");

	s.write(str.c_str(), str.length() + 1);
}

static void PrintSize(const char* txt, int size)
{
	if (size > (1024 * 1024 * 1024)) {
		LOG("%s %.1f GB", txt, size / (1024.0f * 1024 * 1024));
	} else if (size >  (1024 * 1024)) {
		LOG("%s %.1f MB", txt, size / (1024.0f * 1024));
	} else if (size > 1024) {
		LOG("%s %.1f KB", txt, size / (1024.0f));
	} else {
		LOG("%s %u B",    txt, size);
	}
}
#endif //USING_CREG

static void ReadString(std::istream& s, std::string& str)
{
	char cstr[MAX_STRING_SIZE + 1];
	s.getline(cstr, sizeof(cstr) - 1, 0);
	str.clear();
	str.append(cstr);
}

static std::string BuildSaveScriptText(const SaveFileData& saveFileData)
{
	TdfParser script(gameSetup->setupText.c_str(), gameSetup->setupText.size());
	TdfParser::TdfSection* gameSection = script.GetRootSection()->sections["game"];

	if (gameSection == nullptr)
		return gameSetup->setupText;

	gameSection->remove("ReplayCheckpoint", false);
	gameSection->remove("ReplayCheckpointSchema", false);
	gameSection->remove("ReplayDemoStartFrame", false);
	gameSection->remove("ReplayDemoStartTime", false);

	if (saveFileData.replayCheckpoint) {
		gameSection->AddPair("ReplayCheckpoint", 1);
		gameSection->AddPair("ReplayCheckpointSchema", int(saveFileData.replayCheckpointSchema));
		gameSection->AddPair("ReplayDemoStartFrame", saveFileData.replayCheckpointFrame);
		gameSection->AddPair("ReplayDemoStartTime", saveFileData.replayCheckpointTime);
	}

	std::ostringstream rebuiltScript;
	script.print(rebuiltScript);
	return rebuiltScript.str();
}


static void SaveLuaState(CSplitLuaHandle* handle, creg::COutputStreamSerializer& os, std::stringstream& oss)
{
	CLuaStateCollector lsc;
	lsc.Read(handle);
	os.SavePackage(&oss, &lsc, lsc.GetClass());
}


static void LoadLuaState(CSplitLuaHandle* handle, creg::CInputStreamSerializer& is, std::stringstream& iss)
{
	void* plsc;
	creg::Class* plsccls = nullptr;

	if ((handle != nullptr) && handle->syncedLuaHandle.IsValid())
		creg::CopyLuaContext(handle->syncedLuaHandle.GetLuaState());

	is.LoadPackage(&iss, plsc, plsccls);
	assert(plsc && plsccls == CLuaStateCollector::StaticClass());
	CLuaStateCollector* lsc = static_cast<CLuaStateCollector*>(plsc);

	lsc->Write(handle);

	spring::SafeDelete(lsc);
}


void CCregLoadSaveHandler::SaveGame(const std::string& path)
{
#ifdef USING_CREG
	LOG("[LSH::%s] saving game to \"%s\"", __func__, path.c_str());

	if (globalSaveFileData.replayCheckpoint && gs != nullptr) {
		const int requestedFrame = globalSaveFileData.replayCheckpointFrame;
		const int actualFrame = gs->frameNum;

		globalSaveFileData.replayCheckpointFrame = actualFrame;
		globalSaveFileData.replayCheckpointTime = actualFrame * INV_GAME_SPEED;

		if (requestedFrame != actualFrame) {
			LOG("[ReplayRewind] aligned checkpoint metadata to saved simulation frame: requestedFrame=%d savedFrame=%d",
				requestedFrame,
				actualFrame
			);
		}
	}

	// Capture the genuine feature update queue so a checkpoint load can restore it
	// exactly (the load otherwise re-queues every active feature). See
	// CFeatureHandler::RebuildUpdateQueueAfterLoad.
	featureHandler.SnapshotUpdateQueueForSave();

	// NB: Selection leaves CObject reference as Unit's listener,
	//     But isn't serialized - leak on load.
	selectedUnitsHandler.ClearSelected();

	try {
		std::stringstream oss;

		// write our own header. SavePackage() will add its own
		WriteString(oss, SpringVersion::GetSync());
		WriteString(oss, BuildSaveScriptText(globalSaveFileData));
		WriteString(oss, modName);
		WriteString(oss, mapName);


		{
			Sim::SaveComponents(oss);

			creg::COutputStreamSerializer os;

			// save lua state first as lua unit scripts depend on it
			const int luaStart = oss.tellp();
			SaveLuaState(luaGaia, os, oss);
			SaveLuaState(luaRules, os, oss);
			PrintSize("Lua", ((int)oss.tellp()) - luaStart);

			// save creg state
			const int gameStart = oss.tellp();
			CGameStateCollector gsc;
			os.SavePackage(&oss, &gsc, gsc.GetClass());
			PrintSize("Game", ((int)oss.tellp()) - gameStart);


			// save AI state
			const int aiStart = oss.tellp();

			for (const auto& ai: skirmishAIHandler.GetAllSkirmishAIs()) {
				std::stringstream aiData;
				eoh->Save(&aiData, ai.first);

				std::uint64_t aiSize = aiData.tellp();
				creg::WriteUInt(&oss, aiSize);
				if (aiSize > 0)
					oss << aiData.rdbuf();
			}
			PrintSize("AIs", ((int)oss.tellp()) - aiStart);
		}

		{
			gzFile file = gzopen(dataDirsAccess.LocateFile(path, FileQueryFlags::WRITE).c_str(), "wb5");

			if (file == nullptr) {
				LOG_L(L_ERROR, "[LSH::%s] could not open save-file", __func__);
				return;
			}

			std::string data = oss.str();
			std::function<void(gzFile, std::string&&)> func = [](gzFile file, std::string&& data) {
				gzwrite(file, data.c_str(), data.size());
				gzflush(file, Z_FINISH);
				gzclose(file);
			};

			// Serialize against any previous still-running checkpoint write, then
			// launch this one asynchronously and retain its future so a later load
			// (rewind to this checkpoint) can wait for the write to finish. The
			// future is stored (not dropped) so its destructor does not block here.
			// gzFile is just a plain typedef (struct gzFile_s {}* gzFile), can be copied.
			WaitForPendingCheckpointWrite();
			pendingCheckpointWriteFuture = std::async(std::launch::async, std::move(func), file, std::move(data));
		}

		//FIXME add lua state
	} catch (const content_error& ex) {
		LOG_L(L_ERROR, "[LSH::%s] content error \"%s\"", __func__, ex.what());
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[LSH::%s] exception \"%s\"", __func__, ex.what());
	} catch (const char*& exStr) {
		LOG_L(L_ERROR, "[LSH::%s] cstr error \"%s\"", __func__, exStr);
	} catch (const std::string& str) {
		LOG_L(L_ERROR, "[LSH::%s] str error \"%s\"", __func__, str.c_str());
	} catch (...) {
		LOG_L(L_ERROR, "[LSH::%s] unknown error", __func__);
	}
#else //USING_CREG
	LOG_L(L_ERROR, "[LSH::%s] creg is disabled", __func__);
#endif //USING_CREG
}

/// loads the data (map&mod-name,setup-script) needed by PreGame
bool CCregLoadSaveHandler::LoadGameStartInfo(const std::string& path)
{
	// A checkpoint save flushes its gzip stream on a background thread; make sure
	// any in-flight write has completed before we read the file back (e.g. when
	// rewinding to a just-created checkpoint), otherwise CGZFileHandler reads a
	// truncated stream and creg deserialization throws "unknown class".
	WaitForPendingCheckpointWrite();

	CGZFileHandler saveFile(dataDirsAccess.LocateFile(FindSaveFile(path)), SPRING_VFS_RAW_FIRST);

	std::stringbuf* sbuf = iss.rdbuf();
	std::string saveVersion;
	std::string syncVersion = SpringVersion::GetSync();

	char buf[4096];
	int len;
	while ((len = saveFile.Read(buf, sizeof(buf))) > 0)
		sbuf->sputn(buf, len);

	ReadString(iss, saveVersion);

	// check saved engine version against current build
	// in general these will *not* be binary-compatible
	// (so prefer to terminate loading from PreGame)
	if (saveVersion != syncVersion)
		LOG_L(L_WARNING, "[LSH::%s][release=%d] file \"%s\" saved by engine version \"%s\" incompatible with \"%s\"", __func__, SpringVersion::IsRelease(), path.c_str(), saveVersion.c_str(), syncVersion.c_str());

	// read our own header
	ReadString(iss, scriptText);
	ReadString(iss, modName);
	ReadString(iss, mapName);

	CGameSetup::LoadSavedScript(path, scriptText);
	return (saveVersion == syncVersion);
}

/// this should be called on frame 0 when the game has started
void CCregLoadSaveHandler::LoadGame()
{
#ifdef USING_CREG
	ENTER_SYNCED_CODE();
	{
		Sim::LoadComponents(iss);

		creg::CInputStreamSerializer inputStream;

		// load lua state first, as lua unit scripts depend on it
		LoadLuaState(luaGaia, inputStream, iss);
		LoadLuaState(luaRules, inputStream, iss);

		// load creg state
		void* pGSC = nullptr;
		creg::Class* gsccls = nullptr;

		inputStream.LoadPackage(&iss, pGSC, gsccls);
		assert(pGSC && gsccls == CGameStateCollector::StaticClass());

		// the only job of gsc is to collect gamestate data
		CGameStateCollector* gsc = static_cast<CGameStateCollector*>(pGSC);
		spring::SafeDelete(gsc);

		MoveTypes::ResolveLoadSaveEventPointers();
		MoveTypes::ResetLoadSaveTransientEvents();

		if (game != nullptr)
			game->FinalizeLoadSavePostLoad();
	}

	LEAVE_SYNCED_CODE();
#else //USING_CREG
	LOG_L(L_ERROR, "Load failed: creg is disabled");
#endif //USING_CREG
}

/// this should be called on frame 0 when the game has started
void CCregLoadSaveHandler::LoadAIData()
{
#ifdef USING_CREG
	ENTER_SYNCED_CODE();

	// load ai state
	for (const auto& ai: skirmishAIHandler.GetAllSkirmishAIs()) {
		std::uint64_t aiSize;
		creg::ReadUInt(&iss, &aiSize);

		std::vector<char> buffer(aiSize);
		std::stringstream aiData;
		iss.read(buffer.data(), buffer.size());
		aiData.write(buffer.data(), buffer.size());

		eoh->Load(&aiData, ai.first);
	}

	// cleanup
	iss.str("");

	gs->paused = false;
	if (gameServer != nullptr) {
		gameServer->isPaused = false;
		gameServer->syncErrorFrame = 0;
	}

	LEAVE_SYNCED_CODE();
#else //USING_CREG
	LOG_L(L_ERROR, "Load failed: creg is disabled");
#endif //USING_CREG
}
