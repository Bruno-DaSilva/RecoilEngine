/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimSnapshot.h"

#include <cstring>

#include "SnapshotHash.h"
#include "RenderEventQueue.h" // PR 43: boundary dead-id -> shell maps (DEAD_THIS_BATCH extraction)
#include "ExternalAI/SkirmishAIData.h"     // PR 36: GetTeamLuaAI / GetAIInfo per-team AI block
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Map/MapParser.h"                 // PR 36: GetMapStartPositions capture
#include "Sim/Misc/AllyTeam.h"             // PR 36: GetAllyTeamStartBox / GetAllyTeamInfo
#include "Sim/Misc/GlobalConstants.h"      // PR 36: SQUARE_SIZE / MAX_TEAMS
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/Wind.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
// PR 32 (deep per-unit state): moveType subtypes + CAI/builder/factory derefs
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "Sim/MoveTypes/HoverAirMoveType.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/MoveTypes/StaticMoveType.h"
#include "Sim/MoveTypes/ScriptMoveType.h"
#include "Sim/Path/IPathManager.h" // PR 38g GetUnitEstimatedPath (GetPathWayPoints)
#include "Sim/Misc/GlobalConstants.h" // GAME_SPEED
#include "Sim/Misc/NanoPieceCache.h"
#include "Sim/Units/CommandAI/CommandAI.h"   // repeatOrders
#include "Sim/Units/CommandAI/MobileCAI.h"   // repairBelowHealth
#include "Sim/Units/UnitToolTipMap.hpp"      // GetUnitTooltip custom string
#include "Sim/Units/UnitTypes/Builder.h"     // build-state family
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name
#include "Rendering/Models/3DModelPiece.hpp" // PR 33 S3DModelPiece::name (ppro->omp)
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
// PR 31 (weapon/shield scalar family): live weapon/shield/damages reads
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/BombDropper.h"
#include "Sim/Weapons/WeaponTarget.h"
#include "Sim/Misc/DamageArray.h"
#include "Lua/LuaHandleSynced.h"
#include "Lua/LuaSnapshotServe.h"     // PR 43: /epochstats channel bytes           // PR 38: CSplitLuaHandle::GetGameParams (game rules params)
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/TimeProfiler.h"

SimSnapshot simSnapshot;

// PR 38f/38g: event-time LOS-exit visibility override (see SimSnapshot.h). A
// single (unit, allyTeam) pair plus master's captured at-dispatch losStatus
// byte, installed by ScopedVisibility while a deferred UnitLeftLos handler runs
// at the barrier. Main-thread dispatch-window state; -1 (inert) flag-off and
// outside the window, so the Pov/ErrorVector consults below are no-ops there.
namespace {
	int losEvtUnitID = -1;
	int losEvtAllyTeam = -1;
	uint8_t losEvtLosStatus = 0;
}

bool SimSnapshotLosEvent::Active(int unitID, int allyTeam)
{
	return (unitID >= 0 && unitID == losEvtUnitID && allyTeam == losEvtAllyTeam);
}

uint8_t SimSnapshotLosEvent::LosStatus()
{
	return losEvtLosStatus;
}

// PR 38j: fast-reject accessor + allyTeam-agnostic unit match for the top-of-
// callout override consults (see SimSnapshot.h). Inert (losEvtUnitID == -1)
// flag-off and during the diff-gate dual-run, so the position/direction
// callouts fall through to the normal Route() there -> byte-identical.
bool SimSnapshotLosEvent::Installed()
{
	return (losEvtUnitID >= 0);
}

bool SimSnapshotLosEvent::ActiveForUnit(int unitID)
{
	return (unitID >= 0 && unitID == losEvtUnitID);
}

SimSnapshotLosEvent::ScopedVisibility::ScopedVisibility(int unitID, int allyTeam, uint8_t losStatus)
	: prevUnitID(losEvtUnitID)
	, prevAllyTeam(losEvtAllyTeam)
	, prevLosStatus(losEvtLosStatus)
{
	losEvtUnitID = unitID;
	losEvtAllyTeam = allyTeam;
	losEvtLosStatus = losStatus;
}

SimSnapshotLosEvent::ScopedVisibility::~ScopedVisibility()
{
	losEvtUnitID = prevUnitID;
	losEvtAllyTeam = prevAllyTeam;
	losEvtLosStatus = prevLosStatus;
}

// assign-if-different helpers for the team/player boundary copy: the copy is
// re-extracted EVERY boundary (values must be fresh), but the alloc-carrying
// fields (strings, customOpts maps) are almost always unchanged -- comparing
// first turns the steady-state cost into reads only, no allocations
static inline void CopyString(std::string& dst, const char* src)
{
	if (dst != src)
		dst = src;
}

static inline void CopyString(std::string& dst, const std::string& src)
{
	if (dst != src)
		dst = src;
}

static inline void CopyOpts(spring::unordered_map<std::string, std::string>& dst,
                            const spring::unordered_map<std::string, std::string>& src)
{
	const auto equal = [&]() {
		if (dst.size() != src.size())
			return false;
		for (const auto& [key, value] : src) {
			const auto it = dst.find(key);
			if (it == dst.end() || it->second != value)
				return false;
		}
		return true;
	};

	if (!equal())
		dst = src;
}

// GetSolidObjectBlocking's seven pushed booleans as one byte, bit i = push
// slot i (see the UnitRows::blockingBits layout comment)
static inline uint8_t PackBlockingBits(const CSolidObject* o)
{
	return static_cast<uint8_t>(
		(o->HasPhysicalStateBit(CSolidObject::PSTATE_BIT_BLOCKING)       << 0) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_SOLIDOBJECTS) << 1) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_PROJECTILES ) << 2) |
		(o->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS ) << 3) |
		(o->crushable          << 4) |
		(o->blockEnemyPushing  << 5) |
		(o->blockHeightChanges << 6));
}

// PR 31: flatten a live DynDamageArray into the POD DamagesSnap (see the
// DamagesSnap rationale in SimSnapshot.h). The float vector is assigned in
// place so it reuses capacity across boundaries (numArmorTypes is game-fixed).
// A null source reproduces the live "damages == nullptr" nil shape (valid=0).
static inline void CopyDamages(SimSnapshot::UnitRows::DamagesSnap& dst, const DynDamageArray* src)
{
	if (src == nullptr) {
		dst.valid = 0;
		dst.damages.clear();
		return;
	}

	dst.valid = 1;
	dst.paralyzeDamageTime = src->paralyzeDamageTime;
	dst.impulseFactor = src->impulseFactor;
	dst.impulseBoost = src->impulseBoost;
	dst.craterMult = src->craterMult;
	dst.craterBoost = src->craterBoost;
	dst.dynDamageExp = src->dynDamageExp;
	dst.dynDamageMin = src->dynDamageMin;
	dst.dynDamageRange = src->dynDamageRange;
	dst.dynDamageInverted = src->dynDamageInverted;
	dst.craterAreaOfEffect = src->craterAreaOfEffect;
	dst.damageAreaOfEffect = src->damageAreaOfEffect;
	dst.edgeEffectiveness = src->edgeEffectiveness;
	dst.explosionSpeed = src->explosionSpeed;

	const int n = src->GetNumTypes();
	dst.damages.resize(n);
	for (int i = 0; i < n; ++i)
		dst.damages[i] = src->Get(i);
}

bool SimSnapshot::UnitRows::PovUnitVisible(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	// PR 38g: during a deferred UnitLeftLos, present master's captured
	// at-dispatch (post-INLOS-clear) losStatus in place of the row's end-of-
	// frame byte, so the gate opens exactly when master's synchronous handler's
	// did (residual radar) and nils exactly when master's would.
	const uint8_t losStatus = SimSnapshotLosEvent::Active(unitID, readAllyTeam)
		? SimSnapshotLosEvent::LosStatus()
		: losStatusAll[readAllyTeam * MaxUnits() + unitID];

	return ((losStatus & (LOS_INLOS | LOS_INRADAR)) != 0);
}

bool SimSnapshot::UnitRows::PovUnitInLos(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	// PR 38g: residual at-dispatch losStatus during a deferred UnitLeftLos
	// (LOS_INLOS is cleared before master fires the event, so this reads false --
	// matching master's synchronous handler).
	const uint8_t losStatus = SimSnapshotLosEvent::Active(unitID, readAllyTeam)
		? SimSnapshotLosEvent::LosStatus()
		: losStatusAll[readAllyTeam * MaxUnits() + unitID];

	return ((losStatus & LOS_INLOS) != 0);
}

bool SimSnapshot::UnitRows::PovUnitTyped(int unitID, int readAllyTeam, bool fullRead) const
{
	if (PovAlliedUnit(unitID, readAllyTeam, fullRead))
		return true;
	if (readAllyTeam < 0 || readAllyTeam >= numAllyTeams)
		return false;

	// LuaUtils::IsUnitTyped mirror: currently in LOS, or not lost from radar
	// since last being visible.
	// PR 38g: residual at-dispatch losStatus during a deferred UnitLeftLos, in
	// place of the row's end-of-frame byte, matching master's synchronous handler.
	const uint8_t losStatus = SimSnapshotLosEvent::Active(unitID, readAllyTeam)
		? SimSnapshotLosEvent::LosStatus()
		: losStatusAll[readAllyTeam * MaxUnits() + unitID];
	constexpr uint8_t prevMask = (LOS_PREVLOS | LOS_CONTRADAR);

	return ((losStatus & LOS_INLOS) != 0 || (losStatus & prevMask) == prevMask);
}

float3 SimSnapshot::UnitRows::ErrorVector(int unitID, int argAllyTeam) const
{
	// bit-for-bit mirror of CUnit::GetErrorVector (Unit.cpp) from extracted
	// inputs; keep the float expression order identical to the live code
	if (argAllyTeam < 0 || argAllyTeam >= numAllyTeams)
		return (posErrorVector[unitID] * baseRadarErrorSize * 2.0f);

	const int atErrorMask = (posErrorBits[argAllyTeam * MaxUnits() + unitID] != 0);
	// PR 38g: during a deferred UnitLeftLos, feed master's captured at-dispatch
	// losStatus (LOS_INLOS cleared, radar/ghost residual) into the UNCHANGED
	// error math, so the mirror reproduces master's fuzzy radar-error position
	// EXACTLY (case 8 -> AllyTeamRadarErrorSize, case 0 -> BaseRadarErrorSize*2,
	// seenGhost -> zero) instead of PR 38f's forced zero error.
	const int atSightMask = SimSnapshotLosEvent::Active(unitID, argAllyTeam)
		? SimSnapshotLosEvent::LosStatus()
		: losStatusAll[argAllyTeam * MaxUnits() + unitID];

	const int isVisible = 2 * ((atSightMask & LOS_INLOS  ) != 0 || Allied(argAllyTeam, allyTeam[unitID])); // in LOS or allied, no error
	const int seenGhost = 4 * ((atSightMask & LOS_PREVLOS) != 0 && leavesGhost[unitID] != 0);              // seen ghosted immobiles, no error
	const int isOnRadar = 8 * ((atSightMask & LOS_INRADAR) != 0                                         ); // current radar contact

	float errorMult = 0.0f;

	switch (isVisible | seenGhost | isOnRadar) {
		case  0: { errorMult = baseRadarErrorSize * 2.0f        ; } break; //  !isVisible && !seenGhost  && !isOnRadar
		case  8: { errorMult = radarErrorSizes[argAllyTeam]     ; } break; //  !isVisible && !seenGhost  &&  isOnRadar
		default: {                                                } break; // ( isVisible ||  seenGhost) && !isOnRadar
	}

	return (posErrorVector[unitID] * errorMult * (atErrorMask != 0));
}

bool SimSnapshot::FeatureRows::IsInLosForAllyTeam(int id, int argAllyTeam) const
{
	// bit-for-bit mirror of CFeature::IsInLosForAllyTeam from extracted inputs
	if (alwaysVisible[id] != 0 || argAllyTeam == -1)
		return true;

	const bool isGaia = (allyTeam[id] == gaiaAllyTeam);

	switch (featureVisibility) {
		case CModInfo::FEATURELOS_NONE:
		default:
			return InLos(id, argAllyTeam);
		case CModInfo::FEATURELOS_GAIAONLY:
			return (isGaia || InLos(id, argAllyTeam));
		case CModInfo::FEATURELOS_GAIAALLIED:
			return (isGaia || allyTeam[id] == argAllyTeam || InLos(id, argAllyTeam));
		case CModInfo::FEATURELOS_ALL:
			return true;
	}
}

bool SimSnapshot::FramesDue() const
{
	const int newest = NewestIdx();

	return
		(buffers[newest].simFrame != gs->frameNum) ||
		(buffers[newest].aliveCount != static_cast<int32_t>(unitHandler.GetActiveUnits().size()));
}

bool SimSnapshot::ProduceDue() const
{
	return mutatedOutsideFrame || FramesDue();
}

void SimSnapshot::Update()
{
	SCOPED_TIMER("Update::SimSnapshot");

	// PR 36: GetMapStartPositions -- immutable map data, parsed once. Cached
	// here (draw/main thread) rather than inside ExtractTeams, which also runs
	// on the sim thread via HashCompletedFrame; LoadStartPositionsFromMap uses
	// the MapParser and must stay off the sim thread. (PR 44a: the flip
	// producer never runs Update(); SpawnSimThread pre-fills the cache.)
	CacheMapStartPositions();

	if (!ProduceDue()) {
		// LOCKSTEP EXCEPTION (PR 43, enumerated in the header block): no new
		// epoch, but the net-mutable channels still refresh every boundary --
		// net messages mutate team/player/global tables BETWEEN sim frames
		// (share/resign transfers, NETMSG_PLAYERINFO ping/cpu at net rate),
		// invisibly to the frameNum/aliveCount due-check. Pre-ring these
		// re-extracted+swapped unconditionally; the value-identical ring form
		// is an in-place refresh of the held epoch's channels (single-threaded
		// under the park). PR 44a: DISSOLVED under the flip (this function is
		// the lockstep producer only) -- the flip producer fully re-extracts
		// on a between-frames mutation mark instead (the ClientReadNet
		// handlers of the mutating net messages now mark, and a low-rate
		// fallback republish covers residual classes).
		ExtractTeams(teamBuffers[HeldIdx()]);
		ExtractPlayers(playerBuffers[HeldIdx()]);
		ExtractGlobals(globBuffers[HeldIdx()]);
		return;
	}

	const int target = ProduceSlotInternal();

	// lockstep publish: the consumer acquires it via AcquireNewestEpoch()
	// (the barrier calls it right after this returns, same thread)
	epochCounter.store(slotMeta[target].epochId, std::memory_order_relaxed);
	newestSlot.store(target, std::memory_order_release);
}

int SimSnapshot::ProduceSlotInternal()
{
	mutatedOutsideFrame = false;
	splitWasEnabled |= SimDrawSplit::Enabled(); // teardown-telemetry latch

	const spring_time t0 = spring_gettime();

	// producer half (PR 43): extract ALL channels into a free ring slot; the
	// caller publishes it as the newest-complete epoch.
	const int target = PickFreeSlot();

	// PR 43 §7.7 dead-row sources. Under lockstep the barrier's drain already
	// dispatched the batch's destroy records, populating the dispatch-time
	// dead maps; under the PR-44a flip the producer runs BEFORE the dispatch,
	// so the parked pending-destroy ledger holds the batch's deaths instead.
	// The union covers both modes (whichever source is inactive is empty; the
	// INACTIVE guard in ExtractDeadRowsFromShells also dedupes defensively).
	static std::vector<std::pair<int, const CUnit*>> deadUnits;
	static std::vector<std::pair<int, const CFeature*>> deadFeatures;
	static std::vector<std::pair<int, const CProjectile*>> deadProjectiles;
	deadUnits.clear();
	deadFeatures.clear();
	deadProjectiles.clear();

	// PR 43 §7.7: the grow-only feature/projectile rows must also cover the
	// batch's died-in-batch ids (their DEAD_THIS_BATCH rows are extracted
	// below from the DeferredObjectDeleter shells); a died-in-batch id can
	// exceed every live id when the youngest object died
	size_t minFeatSlots = 0;
	size_t minProjSlots = 0;

	if (SimDrawSplit::Enabled()) {
		for (const auto& [id, shell] : renderEventQueue.BoundaryDeadUnits())
			deadUnits.emplace_back(id, shell);
		for (const auto& [id, shell] : renderEventQueue.BoundaryDeadFeatures())
			deadFeatures.emplace_back(id, shell);
		for (const auto& [id, shell] : renderEventQueue.BoundaryDeadProjectiles())
			deadProjectiles.emplace_back(id, shell);

		// PR 44a: the flip-mode source (empty under lockstep -- the drain pops
		// the ledger; the fd41dbdd92 synced-namespace filter applies inside)
		renderEventQueue.CollectPendingDeadShells(deadUnits, deadFeatures, deadProjectiles);

		for (const auto& [id, shell] : deadFeatures)
			minFeatSlots = std::max(minFeatSlots, static_cast<size_t>(id + 1));
		for (const auto& [id, shell] : deadProjectiles)
			minProjSlots = std::max(minProjSlots, static_cast<size_t>(id + 1));
	}

	Extract(buffers[target]);
	ExtractProjectiles(projBuffers[target], minProjSlots);
	ExtractFeatures(featBuffers[target], minFeatSlots);
	ExtractTeams(teamBuffers[target]);
	ExtractPlayers(playerBuffers[target]);
	ExtractGlobals(globBuffers[target]);

	// PR 43 §7.7 (producer side): extract the batch's dying ids from their
	// deferred-deletion shells into the publishing slot, marked
	// DEAD_THIS_BATCH -- genuine at-death rows by construction (deletes the
	// 38b genuineness-guard class). The barrier dispatches still read live
	// (barrierLive) under 43/44a, so only the id-coverage gate consumes these
	// until 44b.
	if (SimDrawSplit::Enabled())
		ExtractDeadRowsFromShells(buffers[target], featBuffers[target], projBuffers[target], deadUnits, deadFeatures, deadProjectiles);

	EpochSlotMeta& meta = slotMeta[target];
	meta.epochId = epochCounter.load(std::memory_order_relaxed) + 1;
	// frame span: everything after the previous epoch's last frame; a forced
	// same-frame republish (net mutation between frames) yields first > last
	meta.firstSimFrame = slotMeta[NewestIdx()].lastSimFrame + 1;
	meta.lastSimFrame = buffers[target].simFrame;

	const float dt = (spring_gettime() - t0).toMilliSecsf();
	sumExtractMs += dt;
	maxExtractMs = std::max(maxExtractMs, dt);
	numExtractions += 1;
	peakAliveCount = std::max(peakAliveCount, buffers[target].aliveCount);

	return target;
}

int SimSnapshot::BeginEpochProduction()
{
	SCOPED_TIMER("Update::SimSnapshot");

	// PR 44a flip producer: MapParser cache must have been filled on the main
	// thread (SpawnSimThread); everything else in the produce body is
	// sim-thread-legal (the HashCompletedFrame precedent)
	assert(mapStartPosCached);

	return ProduceSlotInternal();
}

void SimSnapshot::PublishEpoch(int slot, uint64_t cmdQueueCacheEpoch, uint64_t pieceCacheEpoch, uint32_t mirrorDrainSerial)
{
	EpochSlotMeta& meta = slotMeta[slot];

	// seal the channel-version scalars pre-publish (the flip analogue of the
	// lockstep barrier's SealEpochChannelVersions call)
	meta.cmdQueueCacheEpoch = cmdQueueCacheEpoch;
	meta.pieceCacheEpoch = pieceCacheEpoch;
	meta.mirrorDrainSerial = mirrorDrainSerial;

	// publish: counter first (relaxed -- the release below orders both), then
	// the newest-slot store the consumer acquires against
	epochCounter.store(meta.epochId, std::memory_order_relaxed);
	newestSlot.store(slot, std::memory_order_release);
}

int SimSnapshot::PickFreeSlot() const
{
	const int newest = NewestIdx();
	const int held = HeldIdx();

	for (int s = 0; s < EPOCH_RING_SLOTS; ++s) {
		const int cand = (newest + 1 + s) % EPOCH_RING_SLOTS;

		if (cand == newest || cand == held)
			continue;
		if (slotMeta[cand].refCount.load(std::memory_order_relaxed) != 0)
			continue;

		return cand;
	}

	// cannot happen under the 43 lockstep (<= 1 held + 1 newest of 3), nor
	// under the 44a pacing (publish waits for consumption, so at most one
	// unconsumed newest + one held are pinned); if a future consumer leaks a
	// reference, republishing in place over the newest slot is the safe
	// degradation (the producer owns the newest slot until the acquire)
	assert(false);
	return newest;
}

uint64_t SimSnapshot::AcquireNewestEpoch()
{
	// PR 44a: acquire-load pairs with the producer's release publish -- every
	// slot/channel write that preceded the publish is visible from here on
	const int newest = newestSlot.load(std::memory_order_acquire);
	const int held = HeldIdx();

	if (holdingRef && newest == held)
		return 0;

	slotMeta[newest].refCount.fetch_add(1, std::memory_order_acq_rel);

	const int prev = held;
	const bool hadRef = holdingRef;

	heldSlot.store(newest, std::memory_order_relaxed);
	holdingRef = true;

	// PR 44b: the §3.2 pacing signal ("newest epoch CONSUMED") moved from
	// here to MarkNewestEpochConsumed(), stamped when the consumer finishes
	// ALL drawer-side consumption for this epoch (batch dispatch + drawer
	// Update + the SSBO upload) -- the producer's next extraction walks
	// drawer containers (unsortedObjects, the transform alloc map, the
	// transforms storage), so the stamp is what keeps producer extraction
	// and consumer-side drawer mutation mutually exclusive once the park is
	// gone (it reproduces the park's exclusion without blocking the sim:
	// production is skipped, simulation continues).

	if (!hadRef || prev == newest)
		return 0;

	if (slotMeta[prev].refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		// the released slot retired: report its epoch so the caller can run
		// the retirement hooks (DeferredObjectDeleter::ReleaseRetired)
		return slotMeta[prev].epochId;
	}

	return 0;
}


void SimSnapshot::CheckEpochIdCoverage()
{
	// THE ARMED ID-COVERAGE GATE (PR 43 §2.6 / §3.6a): every id referenced by
	// the batch's records must be servable from the published epoch -- ACTIVE
	// (alive at the epoch's edge; a just-created object IS extracted because
	// extraction runs after the batch's creations) or DEAD_THIS_BATCH (the
	// shell-sourced at-death row for created-and-died / died-in-batch ids).
	// Runs while the dispatch window is still open, so a raw validity read is
	// used (Valid() would also pass DEAD_THIS_BATCH here, but be explicit).
	const auto& refs = renderEventQueue.BatchCoverageRefs();

	if (refs.empty())
		return;

	constexpr uint64_t MAX_LOGGED = 50;

	const int held = HeldIdx();
	const UnitRows& u = buffers[held];
	const FeatureRows& f = featBuffers[held];
	const ProjectileRows& p = projBuffers[held];

	for (const RenderEventQueue::CoverageRef& ref : refs) {
		idCoverageChecked += 1;

		uint8_t v = SimSnapshotValid::INACTIVE;
		const char* kindName = "?";

		switch (ref.kind) {
			case 0: {
				kindName = "unit";
				if (static_cast<size_t>(ref.id) < u.valid.size())
					v = u.valid[ref.id];
			} break;
			case 1: {
				kindName = "feature";
				if (static_cast<size_t>(ref.id) < f.valid.size())
					v = f.valid[ref.id];
			} break;
			case 2: {
				kindName = "projectile";
				if (static_cast<size_t>(ref.id) < p.valid.size())
					v = p.valid[ref.id];
			} break;
		}

		if (v == SimSnapshotValid::ACTIVE || v == SimSnapshotValid::DEAD_THIS_BATCH)
			continue;

		idCoverageViolations += 1;

		if (idCoverageViolations <= MAX_LOGGED) {
			LOG_L(L_ERROR, "[EpochIdCoverage] epoch=%llu frame=%d %s id=%d unservable (validity=%u) -- a record referenced an id the epoch does not serve",
				(unsigned long long)slotMeta[held].epochId, u.simFrame, kindName, ref.id, unsigned(v));
		}
	}
}

void SimSnapshot::SealEpochChannelVersions(uint64_t cmdQueueCacheEpoch, uint64_t pieceCacheEpoch, uint32_t mirrorDrainSerial)
{
	EpochSlotMeta& meta = slotMeta[HeldIdx()];
	meta.cmdQueueCacheEpoch = cmdQueueCacheEpoch;
	meta.pieceCacheEpoch = pieceCacheEpoch;
	meta.mirrorDrainSerial = mirrorDrainSerial;
}

namespace {
	// approximate resident bytes of one LuaRulesParams::Params map copy
	// (string keys + variant values; container overhead ignored)
	size_t RulesParamsBytes(const LuaRulesParams::Params& params)
	{
		size_t bytes = 0;
		for (const auto& [key, param] : params) {
			bytes += key.size() + sizeof(param);
			if (const std::string* s = std::get_if<std::string>(&param.value))
				bytes += s->size();
		}
		return bytes;
	}
}

void SimSnapshot::LogEpochStats() const
{
	// ring occupancy + spans (the /epochstats telemetry, PR 43 §2.8)
	for (int s = 0; s < EPOCH_RING_SLOTS; ++s) {
		const EpochSlotMeta& m = slotMeta[s];
		LOG("[EpochStats] slot=%d epoch=%llu span=[%d,%d] ref=%d%s%s cmdCacheEpoch=%llu pieceCacheEpoch=%llu mirrorSerial=%u",
			s, (unsigned long long)m.epochId, m.firstSimFrame, m.lastSimFrame,
			m.refCount.load(std::memory_order_relaxed),
			(s == HeldIdx()) ? " HELD" : "", (s == NewestIdx()) ? " NEWEST" : "",
			(unsigned long long)m.cmdQueueCacheEpoch, (unsigned long long)m.pieceCacheEpoch,
			m.mirrorDrainSerial);
	}

	// per-channel payload bytes of the held epoch (§7.1 standing-TODO numbers;
	// flat-array sizes are exact, map/string channels are entry-walk estimates)
	const int held = HeldIdx();
	const UnitRows& u = buffers[held];
	const ProjectileRows& p = projBuffers[held];
	const FeatureRows& f = featBuffers[held];
	const TeamRows& t = teamBuffers[held];

	const size_t maxUnits = u.MaxUnits();
	const size_t unitFlatBytes =
		maxUnits * (sizeof(uint8_t) * 14 + sizeof(int16_t) * 2 + sizeof(int32_t) * 17 +
		            sizeof(float) * 20 + sizeof(float3) * 10 + sizeof(float4) +
		            sizeof(SResourcePack) * 6 + sizeof(CollisionVolume) + sizeof(MoveTypeBlock)) +
		u.losStatusAll.size() + u.posErrorBits.size() + u.inRadarAll.size() +
		u.unitInLosAll.size() + u.unitInAirLosAll.size() + u.unitInJammerAll.size();

	const size_t weaponFlatBytes = u.wAngleGood.size() * (sizeof(uint8_t) * 6 + sizeof(int32_t) * 13 + sizeof(float) * 10 + sizeof(float3) * 5 + sizeof(UnitRows::DamagesSnap));

	size_t unitRulesBytes = 0;
	size_t validUnits = 0;
	for (size_t id = 0; id < maxUnits; ++id) {
		if (u.valid[id] == 0)
			continue;
		validUnits += 1;
		unitRulesBytes += RulesParamsBytes(u.unitRulesParams[id]);
	}

	size_t featRulesBytes = 0;
	size_t validFeats = 0;
	for (size_t id = 0; id < f.MaxSlots(); ++id) {
		if (f.valid[id] == 0)
			continue;
		validFeats += 1;
		featRulesBytes += RulesParamsBytes(f.featureRulesParams[id]);
	}

	const size_t projFlatBytes = p.MaxSlots() *
		(sizeof(uint8_t) * 5 + sizeof(int32_t) * 8 + sizeof(float) * 4 +
		 sizeof(float3) * 4 + sizeof(float4) + sizeof(UnitRows::DamagesSnap)) +
		p.inLosAll.size() + p.visInLosAll.size();

	const size_t featFlatBytes = f.MaxSlots() *
		(sizeof(uint8_t) * 5 + sizeof(int16_t) * 2 + sizeof(int32_t) * 5 +
		 sizeof(float) * 8 + sizeof(float3) * 7 + sizeof(float4) +
		 sizeof(SResourcePack) * 2 + sizeof(CollisionVolume)) + f.inLosAll.size();

	size_t teamRulesBytes = 0;
	size_t teamStatsBytes = 0;
	for (int i = 0; i < t.activeTeams; ++i) {
		teamRulesBytes += RulesParamsBytes(t.teamRulesParams[i]);
		teamStatsBytes += t.statHistory[i].size() * sizeof(TeamStatistics);
	}

	LOG("[EpochStats] epoch=%llu simFrame=%d units=%d/%d feats=%d/%d projSlots=%d",
		(unsigned long long)slotMeta[held].epochId, u.simFrame,
		int(validUnits), int(maxUnits), int(validFeats), int(f.MaxSlots()), int(p.MaxSlots()));
	LOG("[EpochStats] idCoverage: checked=%llu violations=%llu",
		(unsigned long long)idCoverageChecked, (unsigned long long)idCoverageViolations);
	// the two singleton (newest-epoch-tracking) channels outside SimSnapshot
	size_t cmdQueueBytes = 0;
	size_t pieceBytes = 0;
	LuaSnapshotServe::EpochChannelBytes(cmdQueueBytes, pieceBytes);

	LOG("[EpochStats] channel bytes (held epoch, x%d slots resident): "
	    "unitFlat=%.1fKB weaponFlat=%.1fKB unitRules=%.1fKB "
	    "projFlat=%.1fKB featFlat=%.1fKB featRules=%.1fKB "
	    "teamRules=%.1fKB teamStatsHist=%.1fKB "
	    "cmdQueueCache=%.1fKB pieceCache=%.1fKB (cmd/piece: held slot's per-slot copy, PR 44a)",
		EPOCH_RING_SLOTS,
		unitFlatBytes / 1024.0f, weaponFlatBytes / 1024.0f, unitRulesBytes / 1024.0f,
		projFlatBytes / 1024.0f, featFlatBytes / 1024.0f, featRulesBytes / 1024.0f,
		teamRulesBytes / 1024.0f, teamStatsBytes / 1024.0f,
		cmdQueueBytes / 1024.0f, pieceBytes / 1024.0f);
}

void SimSnapshot::HashCompletedFrame(int frameNum)
{
	if (!SnapshotHash::Armed())
		return;

	// Extract a private copy of the same rows the published buffer holds, but
	// from the just-completed sim frame's live state (Extract stamps
	// gs->frameNum, which equals frameNum here). The epoch ring slots and the
	// epoch counter are untouched, so nothing draw-side observes this. Player
	// rows are not
	// hashed at all (net-layer state, see the PlayerRows comment), so no
	// player scratch exists.
	Extract(hashScratch);
	ExtractProjectiles(hashProjScratch);
	ExtractFeatures(hashFeatScratch);
	ExtractTeams(hashTeamScratch);
	SnapshotHash::HashFrame(frameNum, hashScratch, hashProjScratch, hashFeatScratch, hashTeamScratch);
}

void SimSnapshot::Clear()
{
	// PR 43 §2.8: end-of-game epoch telemetry (ring occupancy, channel bytes,
	// id-coverage counters) so every gate run leaves measured §7.1 numbers.
	// Keyed on splitWasEnabled, NOT Enabled(): CGame teardown runs
	// SimDrawSplit::Clear() (which drops the flag) before this.
	if (numExtractions > 0 && splitWasEnabled)
		LogEpochStats();
	splitWasEnabled = false;

	if (numExtractions > 0) {
		const auto& r = buffers[HeldIdx()];
		const size_t bufBytes =
			r.radarErrorSizes.size() * sizeof(float) + r.allied.size() +
			r.valid.size() + r.team.size() + r.allyTeam.size() +
			r.beingBuilt.size() + r.stunned.size() + r.leavesGhost.size() +
			r.losStatusAll.size() + r.posErrorBits.size() + r.inRadarAll.size() +
			(r.pos.size() + r.midPos.size() + r.aimPos.size() + r.relMidPos.size() +
			 r.frontdir.size() + r.updir.size() + r.rightdir.size() +
			 r.posErrorVector.size()) * sizeof(float3) +
			r.speed.size() * sizeof(float4) +
			(r.health.size() + r.maxHealth.size() + r.paralyzeDamage.size() +
			 r.captureProgress.size() + r.buildProgress.size() + r.radius.size()) * sizeof(float) +
			r.defID.size() * sizeof(int32_t) +
			r.noSelect.size() + r.inVoid.size() + r.selVol.size() * sizeof(CollisionVolume) +
			// PR 27a rows
			r.isDead.size() + r.neutral.size() + r.activated.size() +
			r.isCloaked.size() + r.armoredState.size() + r.blockingBits.size() +
			(r.heading.size() + r.buildFacing.size()) * sizeof(int16_t) +
			(r.armoredMultiple.size() + r.height.size() + r.mass.size() +
			 r.maxRange.size() + r.seismicSignature.size() + r.experience.size() +
			 r.limExperience.size() + r.buildTime.size()) * sizeof(float) +
			(r.selfDCountdown.size() + r.losRadius.size() + r.airLosRadius.size() +
			 r.radarRadius.size() + r.sonarRadius.size() + r.seismicRadius.size() +
			 r.jammerRadius.size() + r.sonarJamRadius.size() + r.moveDefID.size()) * sizeof(int32_t) +
			(r.resourcesMake.size() + r.resourcesUse.size() + r.harvested.size() +
			 r.harvestStorage.size() + r.cost.size()) * sizeof(SResourcePack) +
			// PR 32 (deep per-unit state): flat rows + the moveType full-table
			// block + the three LOS-variant strides (variable-size blocks and
			// strings are omitted -- they are near-empty for most units)
			r.storage.size() * sizeof(SResourcePack) +
			r.moveTypeBlock.size() * sizeof(SimSnapshot::MoveTypeBlock) +
			(r.fireState.size() + r.moveState.size() + r.nextPosErrorUpdate.size() +
			 r.lastAttackerID.size() + r.transporterID.size() + r.curBuildID.size()) * sizeof(int32_t) +
			(r.repairBelowHealth.size() + r.metalExtract.size() + r.buildeeRadius.size() +
			 r.buildDistance.size() + r.buildPower.size() + r.mtMaxSpeed.size() +
			 r.mtMaxWantedSpeed.size()) * sizeof(float) +
			(r.posErrorDelta.size() + r.mtGoalPos.size()) * sizeof(float3) +
			r.unitInLosAll.size() + r.unitInAirLosAll.size() + r.unitInJammerAll.size();
		LOG("[SimSnapshot] extractions=%u avgMs=%.4f maxMs=%.4f peakUnits=%d memKB=%.1f projSlots=%d featSlots=%d",
			numExtractions, sumExtractMs / numExtractions, maxExtractMs, peakAliveCount,
			(float(EPOCH_RING_SLOTS) * bufBytes) / 1024.0f,
			int(projBuffers[HeldIdx()].MaxSlots()), int(featBuffers[HeldIdx()].MaxSlots()));
	}

	for (UnitRows& rows : buffers) {
		rows.simFrame = -1;
		rows.aliveCount = 0;
	}
	for (ProjectileRows& rows : projBuffers)
		std::fill(rows.valid.begin(), rows.valid.end(), 0);
	for (FeatureRows& rows : featBuffers)
		std::fill(rows.valid.begin(), rows.valid.end(), 0);
	for (TeamRows& rows : teamBuffers)
		rows.activeTeams = 0;
	for (PlayerRows& rows : playerBuffers)
		rows.activePlayers = 0;

	// PR 36: force the immutable map-start cache to re-parse for the next game
	mapStartPosCached = false;
	mapStartPos.clear();
	mapStartPosValid.clear();

	// PR 43: reset the epoch ring with everything else (the ClearCaches-class
	// teardown the epoch contract names; epochId stays monotonic per game)
	for (EpochSlotMeta& meta : slotMeta) {
		meta.epochId = 0;
		meta.firstSimFrame = -1;
		meta.lastSimFrame = -1;
		meta.refCount.store(0, std::memory_order_relaxed);
		meta.cmdQueueCacheEpoch = 0;
		meta.pieceCacheEpoch = 0;
		meta.mirrorDrainSerial = 0;
	}
	heldSlot.store(0, std::memory_order_relaxed);
	newestSlot.store(0, std::memory_order_relaxed);
	holdingRef = false;
	epochCounter.store(0, std::memory_order_relaxed);
	consumedEpochId.store(0, std::memory_order_relaxed);
	idCoverageChecked = 0;
	idCoverageViolations = 0;

	mutatedOutsideFrame = false;
	sumExtractMs = 0.0f;
	maxExtractMs = 0.0f;
	numExtractions = 0;
	peakAliveCount = 0;
}

void SimSnapshot::Resize(UnitRows& rows, size_t maxUnits, int numAllyTeams)
{
	rows.numAllyTeams = numAllyTeams;
	rows.radarErrorSizes.resize(numAllyTeams);
	rows.allied.resize(size_t(numAllyTeams) * numAllyTeams);

	rows.valid.resize(maxUnits, 0);
	rows.pos.resize(maxUnits);
	rows.midPos.resize(maxUnits);
	rows.aimPos.resize(maxUnits);
	rows.speed.resize(maxUnits);
	rows.health.resize(maxUnits);
	rows.maxHealth.resize(maxUnits);
	rows.paralyzeDamage.resize(maxUnits);
	rows.captureProgress.resize(maxUnits);
	rows.team.resize(maxUnits);
	rows.allyTeam.resize(maxUnits);
	rows.defID.resize(maxUnits);
	rows.buildProgress.resize(maxUnits);
	rows.beingBuilt.resize(maxUnits);
	rows.stunned.resize(maxUnits);
	rows.radius.resize(maxUnits);
	rows.selVol.resize(maxUnits);
	rows.noSelect.resize(maxUnits);
	rows.inVoid.resize(maxUnits);
	rows.isDead.resize(maxUnits);
	rows.neutral.resize(maxUnits);
	rows.activated.resize(maxUnits);
	rows.isCloaked.resize(maxUnits);
	rows.armoredState.resize(maxUnits);
	rows.armoredMultiple.resize(maxUnits);
	rows.heading.resize(maxUnits);
	rows.buildFacing.resize(maxUnits);
	rows.height.resize(maxUnits);
	rows.mass.resize(maxUnits);
	rows.maxRange.resize(maxUnits);
	rows.seismicSignature.resize(maxUnits);
	rows.experience.resize(maxUnits);
	rows.limExperience.resize(maxUnits);
	rows.selfDCountdown.resize(maxUnits);
	rows.losRadius.resize(maxUnits);
	rows.airLosRadius.resize(maxUnits);
	rows.radarRadius.resize(maxUnits);
	rows.sonarRadius.resize(maxUnits);
	rows.seismicRadius.resize(maxUnits);
	rows.jammerRadius.resize(maxUnits);
	rows.sonarJamRadius.resize(maxUnits);
	rows.moveDefID.resize(maxUnits);
	rows.resourcesMake.resize(maxUnits);
	rows.resourcesUse.resize(maxUnits);
	rows.harvested.resize(maxUnits);
	rows.harvestStorage.resize(maxUnits);
	rows.cost.resize(maxUnits);
	rows.buildTime.resize(maxUnits);
	rows.blockingBits.resize(maxUnits);
	rows.relMidPos.resize(maxUnits);
	rows.frontdir.resize(maxUnits);
	rows.updir.resize(maxUnits);
	rows.rightdir.resize(maxUnits);
	rows.posErrorVector.resize(maxUnits);
	rows.leavesGhost.resize(maxUnits);
	rows.losStatusAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.posErrorBits.resize(size_t(numAllyTeams) * maxUnits);
	rows.inRadarAll.resize(size_t(numAllyTeams) * maxUnits);

	// PR 31 (weapon/shield family): per-unit rows (flat per-weapon arrays are
	// sized in Extract, where the total live weapon count is known)
	rows.weaponOffset.resize(maxUnits);
	rows.weaponCount.resize(maxUnits);
	rows.reloadSpeed.resize(maxUnits);
	rows.fpsNoFire.resize(maxUnits);
	rows.flankingMode.resize(maxUnits);
	rows.flankingDir.resize(maxUnits);
	rows.flankingMoveFactor.resize(maxUnits);
	rows.flankingAvgDamage.resize(maxUnits);
	rows.flankingDifDamage.resize(maxUnits);
	rows.flankingMobility.resize(maxUnits);
	rows.hasStockpile.resize(maxUnits);
	rows.stockpileNumStockpiled.resize(maxUnits);
	rows.stockpileNumQueued.resize(maxUnits);
	rows.stockpileBuildPercent.resize(maxUnits);
	rows.hasShieldWeapon.resize(maxUnits);
	rows.shieldWeaponEnabled.resize(maxUnits);
	rows.shieldWeaponPower.resize(maxUnits);
	rows.deathExpDamages.resize(maxUnits);
	rows.selfdExpDamages.resize(maxUnits);
	// ---- PR 32 (deep per-unit state) ----
	rows.fireState.resize(maxUnits);
	rows.moveState.resize(maxUnits);
	rows.repairBelowHealth.resize(maxUnits);
	rows.repeatOrders.resize(maxUnits);
	rows.wantCloak.resize(maxUnits);
	rows.useHighTrajectory.resize(maxUnits);
	rows.storage.resize(maxUnits);
	rows.metalExtract.resize(maxUnits);
	rows.buildeeRadius.resize(maxUnits);
	rows.posErrorDelta.resize(maxUnits);
	rows.nextPosErrorUpdate.resize(maxUnits);
	rows.lastAttackerID.resize(maxUnits);
	rows.transporterID.resize(maxUnits);
	rows.builderKind.resize(maxUnits);
	rows.curBuildID.resize(maxUnits);
	rows.buildDistance.resize(maxUnits);
	rows.range3D.resize(maxUnits);
	rows.inBuildStance.resize(maxUnits);
	rows.buildPower.resize(maxUnits);
	rows.customTooltip.resize(maxUnits);
	rows.moveTypeKind.resize(maxUnits);
	rows.mtMaxSpeed.resize(maxUnits);
	rows.mtMaxWantedSpeed.resize(maxUnits);
	rows.mtGoalPos.resize(maxUnits);
	rows.mtProgressState.resize(maxUnits);
	rows.mtAutoLand.resize(maxUnits);
	rows.mtLoopbackAttack.resize(maxUnits);
	rows.moveTypeBlock.resize(maxUnits);
	rows.nanoPieces.resize(maxUnits);
	rows.transportees.resize(maxUnits);
	rows.unitInLosAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.unitInAirLosAll.resize(size_t(numAllyTeams) * maxUnits);
	rows.unitInJammerAll.resize(size_t(numAllyTeams) * maxUnits);
	// ---- PR 38c: per-unit rules-params mirror (same maxUnits sizing) ----
	rows.unitRulesParams.resize(maxUnits);
	// ---- PR 38g: GetUnitEstimatedPath est-path block (same maxUnits sizing) ----
	rows.estPathHasPath.resize(maxUnits);
	rows.estPathPoints.resize(maxUnits);
	rows.estPathStarts.resize(maxUnits);
}

// ---- PR 32 (deep per-unit state) extraction helpers ----
// mirror the live dynamic_cast chains; values only, exact accessors so the
// served table/scalars are bit-identical to the live callouts.
static void ExtractUnitBuildState(SimSnapshot::UnitRows& rows, int id, const CUnit* u)
{
	rows.builderKind[id] = 0;
	rows.curBuildID[id] = -1;
	rows.buildDistance[id] = 0.0f;
	rows.range3D[id] = 0;
	rows.inBuildStance[id] = u->inBuildStance;
	rows.buildPower[id] = 0.0f;
	rows.nanoPieces[id].clear();

	if (const CBuilder* builder = dynamic_cast<const CBuilder*>(u); builder != nullptr) {
		rows.builderKind[id] = 1;
		rows.curBuildID[id] = (builder->curBuild != nullptr) ? builder->curBuild->id : -1;
		rows.buildDistance[id] = builder->buildDistance;
		rows.range3D[id] = builder->range3D;
		const NanoPieceCache& npc = builder->GetNanoPieceCache();
		rows.buildPower[id] = npc.GetBuildPower();
		rows.nanoPieces[id] = npc.GetNanoPieces();
		return;
	}
	if (const CFactory* factory = dynamic_cast<const CFactory*>(u); factory != nullptr) {
		rows.builderKind[id] = 2;
		rows.curBuildID[id] = (factory->curBuild != nullptr) ? factory->curBuild->id : -1;
		const NanoPieceCache& npc = factory->GetNanoPieceCache();
		rows.buildPower[id] = npc.GetBuildPower();
		rows.nanoPieces[id] = npc.GetNanoPieces();
		return;
	}
}

static void ExtractUnitMoveType(SimSnapshot::UnitRows& rows, int id, const CUnit* u)
{
	const AMoveType* mt = u->moveType; // never null

	rows.mtMaxSpeed[id] = mt->GetMaxSpeed() * GAME_SPEED;
	rows.mtMaxWantedSpeed[id] = mt->GetMaxWantedSpeed() * GAME_SPEED;
	rows.mtGoalPos[id] = mt->goalPos;
	rows.mtProgressState[id] = static_cast<uint8_t>(mt->progressState); // Done=0/Active=1/Failed=2
	rows.mtAutoLand[id] = 0;
	rows.mtLoopbackAttack[id] = 0;

	SimSnapshot::MoveTypeBlock& b = rows.moveTypeBlock[id];
	b = SimSnapshot::MoveTypeBlock{}; // ids are reused; default-zero for non-dynamic subtypes

	// PR 38g GetUnitEstimatedPath: default empty (non-ground / no path); populated
	// in the ground branch below. ids are reused, so clear every boundary.
	rows.estPathHasPath[id] = 0;
	rows.estPathPoints[id].clear();
	rows.estPathStarts[id].clear();

	if (const CGroundMoveType* g = dynamic_cast<const CGroundMoveType*>(mt); g != nullptr) {
		rows.moveTypeKind[id] = 1;
		b.turnRate = g->GetTurnRate();
		b.accRate = g->GetAccRate();
		b.decRate = g->GetDecRate();
		b.maxReverseSpeed = g->GetMaxReverseSpeed() * GAME_SPEED;
		b.wantedSpeed = g->GetWantedSpeed() * GAME_SPEED;
		b.currentSpeed = g->GetCurrentSpeed() * GAME_SPEED;
		b.goalRadius = g->GetGoalRadius();
		b.currWayPoint = g->GetCurrWayPoint();
		b.nextWayPoint = g->GetNextWayPoint();

		// PR 38g: capture the estimated path waypoints exactly as
		// LuaPathFinder::PushPathNodes reads them. GetPathWayPoints is a pure const
		// read (does NOT advance the path). pathID==0 => hasPath stays 0 and the
		// twin returns no tables, matching PushPathNodes' pathID==0 early return.
		if (const unsigned int pathID = g->GetPathID(); pathID != 0) {
			rows.estPathHasPath[id] = 1;
			// GetPathWayPoints takes vector<int>&; estPathStarts is vector<int32_t>
			// (int32_t == int on every supported target), copied verbatim below.
			std::vector<int> starts;
			pathManager->GetPathWayPoints(pathID, rows.estPathPoints[id], starts);
			rows.estPathStarts[id].assign(starts.begin(), starts.end());
		}
		return;
	}
	if (const CHoverAirMoveType* h = dynamic_cast<const CHoverAirMoveType*>(mt); h != nullptr) {
		rows.moveTypeKind[id] = 2;
		rows.mtAutoLand[id] = h->autoLand;
		b.wantedHeight = h->wantedHeight;
		b.collide = h->collide;
		b.useSmoothMesh = h->useSmoothMesh;
		b.aircraftState = h->aircraftState;
		b.flyState = h->flyState;
		b.goalDistance = h->goalDistance;
		b.bankingAllowed = h->bankingAllowed;
		b.currentBank = h->currentBank;
		b.currentPitch = h->currentPitch;
		b.turnRate = h->turnRate;
		b.accRate = h->accRate;
		b.decRate = h->decRate;
		b.altitudeRate = h->altitudeRate;
		b.dontLand = h->GetAllowLanding(); // pushed under key "dontLand" (== GetAllowLanding())
		b.maxDrift = h->maxDrift;
		return;
	}
	if (const CStrafeAirMoveType* s = dynamic_cast<const CStrafeAirMoveType*>(mt); s != nullptr) {
		rows.moveTypeKind[id] = 3;
		rows.mtAutoLand[id] = s->autoLand;
		rows.mtLoopbackAttack[id] = s->loopbackAttack;
		b.aircraftState = s->aircraftState;
		b.wantedHeight = s->wantedHeight;
		b.collide = s->collide;
		b.useSmoothMesh = s->useSmoothMesh;
		b.myGravity = s->myGravity;
		b.maxBank = s->maxBank;
		b.turnRadius = s->turnRadius;
		b.accRate = s->accRate;
		b.maxAileron = s->maxAileron;
		b.maxElevator = s->maxElevator;
		b.maxRudder = s->maxRudder;
		return;
	}
	if (dynamic_cast<const CStaticMoveType*>(mt) != nullptr) { rows.moveTypeKind[id] = 4; return; }
	if (dynamic_cast<const CScriptMoveType*>(mt) != nullptr) { rows.moveTypeKind[id] = 5; return; }
	rows.moveTypeKind[id] = 0;
}

void SimSnapshot::Extract(UnitRows& rows)
{
	const size_t maxUnits = unitHandler.MaxUnits();
	const int numAllyTeams = teamHandler.ActiveAllyTeams();

	if (rows.valid.size() != maxUnits || rows.numAllyTeams != numAllyTeams)
		Resize(rows, maxUnits, numAllyTeams);

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	// global block
	rows.baseRadarErrorSize = losHandler->GetBaseRadarErrorSize();
	for (int at = 0; at < numAllyTeams; ++at)
		rows.radarErrorSizes[at] = losHandler->GetAllyTeamRadarErrorSize(at);
	for (int a = 0; a < numAllyTeams; ++a)
		for (int b = 0; b < numAllyTeams; ++b)
			rows.allied[a * numAllyTeams + b] = teamHandler.Ally(a, b);

	const auto& activeUnits = unitHandler.GetActiveUnits();

	for (const CUnit* u : activeUnits) {
		const int id = u->id;

		rows.valid[id] = SimSnapshotValid::ACTIVE;
		rows.pos[id] = u->pos;
		rows.midPos[id] = u->midPos;
		rows.aimPos[id] = u->aimPos;
		rows.speed[id] = u->speed;
		rows.health[id] = u->health;
		rows.maxHealth[id] = u->maxHealth;
		rows.paralyzeDamage[id] = u->paralyzeDamage;
		rows.captureProgress[id] = u->captureProgress;
		rows.team[id] = static_cast<uint8_t>(u->team);
		rows.allyTeam[id] = static_cast<uint8_t>(u->allyteam);
		rows.defID[id] = u->unitDef->id;
		rows.buildProgress[id] = u->buildProgress;
		rows.beingBuilt[id] = u->beingBuilt;
		rows.stunned[id] = u->IsStunned();
		rows.radius[id] = u->radius;
		rows.selVol[id] = u->selectionVolume;
		rows.noSelect[id] = u->noSelect;
		rows.inVoid[id] = u->IsInVoid();
		rows.isDead[id] = u->isDead;
		rows.neutral[id] = u->neutral;
		rows.activated[id] = u->activated;
		rows.isCloaked[id] = u->isCloaked;
		rows.armoredState[id] = u->armoredState;
		rows.armoredMultiple[id] = u->armoredMultiple;
		rows.heading[id] = u->heading;
		rows.buildFacing[id] = u->buildFacing;
		rows.height[id] = u->height;
		rows.mass[id] = u->mass;
		rows.maxRange[id] = u->maxRange;
		rows.seismicSignature[id] = u->seismicSignature;
		rows.experience[id] = u->experience;
		rows.limExperience[id] = u->limExperience;
		rows.selfDCountdown[id] = u->selfDCountdown;
		rows.losRadius[id] = u->losRadius;
		rows.airLosRadius[id] = u->airLosRadius;
		rows.radarRadius[id] = u->radarRadius;
		rows.sonarRadius[id] = u->sonarRadius;
		rows.seismicRadius[id] = u->seismicRadius;
		rows.jammerRadius[id] = u->jammerRadius;
		rows.sonarJamRadius[id] = u->sonarJamRadius;
		rows.moveDefID[id] = (u->moveDef != nullptr) ? static_cast<int32_t>(u->moveDef->pathType) : -1;
		rows.resourcesMake[id] = u->resourcesMake;
		rows.resourcesUse[id] = u->resourcesUse;
		rows.harvested[id] = u->harvested;
		rows.harvestStorage[id] = u->harvestStorage;
		rows.cost[id] = u->cost;
		rows.buildTime[id] = u->buildTime;
		rows.blockingBits[id] = PackBlockingBits(u);
		rows.relMidPos[id] = u->relMidPos;
		rows.frontdir[id] = u->frontdir;
		rows.updir[id] = u->updir;
		rows.rightdir[id] = u->rightdir;
		rows.posErrorVector[id] = u->posErrorVector;
		rows.leavesGhost[id] = u->leavesGhost;

		// ---- PR 32 (deep per-unit state) ----
		rows.fireState[id] = u->fireState;
		rows.moveState[id] = u->moveState;
		{
			const CMobileCAI* mcai = dynamic_cast<const CMobileCAI*>(u->commandAI);
			rows.repairBelowHealth[id] = (mcai != nullptr) ? mcai->repairBelowHealth : -1.0f;
		}
		rows.repeatOrders[id] = u->commandAI->repeatOrders;
		rows.wantCloak[id] = u->wantCloak;
		rows.useHighTrajectory[id] = u->useHighTrajectory;
		rows.storage[id] = u->storage;
		rows.metalExtract[id] = u->metalExtract;
		rows.buildeeRadius[id] = u->buildeeRadius;
		rows.posErrorDelta[id] = u->posErrorDelta;
		rows.nextPosErrorUpdate[id] = u->nextPosErrorUpdate;
		rows.lastAttackerID[id] = (u->lastAttacker != nullptr) ? u->lastAttacker->id : -1;
		rows.transporterID[id] = (u->GetTransporter() != nullptr) ? u->GetTransporter()->id : -1;
		rows.customTooltip[id] = unitToolTipMap.GetConst(id);
		rows.transportees[id].clear();
		rows.transportees[id].reserve(u->transportedUnits.size());
		for (const CUnit::TransportedUnit& tu : u->transportedUnits)
			rows.transportees[id].push_back(tu.unit->id);
		ExtractUnitBuildState(rows, id, u);
		ExtractUnitMoveType(rows, id, u);

		// ---- PR 38c: per-unit rules-params (values only; the Param variant is a
		// bool/float/std::string, no pointer/sim-owned container) ----
		rows.unitRulesParams[id] = u->modParams;

		for (int at = 0; at < numAllyTeams; ++at) {
			rows.losStatusAll[at * maxUnits + id] = u->losStatus[at];
			rows.posErrorBits[at * maxUnits + id] = u->GetPosErrorBit(at);
			rows.inRadarAll[at * maxUnits + id] = losHandler->InRadar(u, at);
			// PR 32 LOS unit variants: store the computed answer (the gates fold
			// cloak/stealth/water/globalLOS logic, like inRadarAll)
			rows.unitInLosAll[at * maxUnits + id] = losHandler->InLos(u, at);
			rows.unitInAirLosAll[at * maxUnits + id] = losHandler->InAirLos(u, at);
			rows.unitInJammerAll[at * maxUnits + id] = losHandler->InJammer(u, at);
		}
	}

	// ================= PR 31: weapon/shield scalar family =================
	// Pass 1: per-unit weapon rows + weaponOffset/weaponCount, accumulating the
	// total live weapon count so the flat per-weapon arrays are sized once.
	int32_t totalWeapons = 0;
	for (const CUnit* u : activeUnits) {
		const int id = u->id;
		const int nw = static_cast<int>(u->weapons.size());

		rows.weaponOffset[id] = totalWeapons;
		rows.weaponCount[id] = nw;
		totalWeapons += nw;

		rows.reloadSpeed[id] = u->reloadSpeed;

		// CanFire's FPS-fire gate captured as one bool (CWeapon::CanFire)
		const CPlayer* fpsPlayer = u->fpsControlPlayer;
		rows.fpsNoFire[id] = (fpsPlayer != nullptr && !fpsPlayer->fpsController.mouse1 && !fpsPlayer->fpsController.mouse2);

		// GetUnitFlanking
		rows.flankingMode[id] = u->flankingBonusMode;
		rows.flankingDir[id] = u->flankingBonusDir;
		rows.flankingMoveFactor[id] = u->flankingBonusMobilityAdd;
		rows.flankingAvgDamage[id] = u->flankingBonusAvgDamage;
		rows.flankingDifDamage[id] = u->flankingBonusDifDamage;
		rows.flankingMobility[id] = u->flankingBonusMobility;

		// GetUnitStockpile (unit->stockpileWeapon; nil shape when null)
		const CWeapon* stockpile = u->stockpileWeapon;
		rows.hasStockpile[id] = (stockpile != nullptr);
		rows.stockpileNumStockpiled[id] = (stockpile != nullptr) ? stockpile->numStockpiled : 0;
		rows.stockpileNumQueued[id] = (stockpile != nullptr) ? stockpile->numStockpileQued : 0;
		rows.stockpileBuildPercent[id] = (stockpile != nullptr) ? stockpile->buildPercent : 0.0f;

		// GetUnitShieldState default case (static_cast in the live path, so a
		// non-null shieldWeapon is a CPlasmaRepulser by construction)
		const CPlasmaRepulser* shield = static_cast<const CPlasmaRepulser*>(u->shieldWeapon);
		rows.hasShieldWeapon[id] = (shield != nullptr);
		rows.shieldWeaponEnabled[id] = (shield != nullptr) ? uint8_t(shield->IsEnabled()) : uint8_t(0);
		rows.shieldWeaponPower[id] = (shield != nullptr) ? shield->GetCurPower() : 0.0f;

		// GetUnitWeaponDamages explosion arrays (unit-level; flattened POD)
		CopyDamages(rows.deathExpDamages[id], u->deathExpDamages);
		CopyDamages(rows.selfdExpDamages[id], u->selfdExpDamages);
	}

	// size the flat per-weapon arrays to the live weapon count (the DamagesSnap
	// float vectors keep their capacity across boundaries)
	{
		const size_t nw = static_cast<size_t>(totalWeapons);
		rows.wAngleGood.resize(nw);
		rows.wReloadStatus.resize(nw);
		rows.wSalvoLeft.resize(nw);
		rows.wNumStockpiled.resize(nw);
		rows.wNextSalvo.resize(nw);
		rows.wReloadTime.resize(nw);
		rows.wReaimTime.resize(nw);
		rows.wAccuracyExp.resize(nw);
		rows.wSprayAngleExp.resize(nw);
		rows.wSalvoError.resize(nw);
		rows.wMoveErrorExp.resize(nw);
		rows.wRange.resize(nw);
		rows.wProjectileSpeed.resize(nw);
		rows.wAutoTargetRangeBoost.resize(nw);
		rows.wSalvoSize.resize(nw);
		rows.wSalvoDelay.resize(nw);
		rows.wSalvoWindup.resize(nw);
		rows.wProjectilesPerShot.resize(nw);
		rows.wAvoidFlags.resize(nw);
		rows.wCollisionFlags.resize(nw);
		rows.wTtl.resize(nw);
		rows.wMuzzlePos.resize(nw);
		rows.wWantedDir.resize(nw);
		rows.wWeaponDir.resize(nw);
		rows.wProjectileType.resize(nw);
		rows.wDefStockpile.resize(nw);
		rows.wDefFireSubmersed.resize(nw);
		rows.wDefMaxFireAngle.resize(nw);
		rows.wIsBombDropper.resize(nw);
		rows.wAimFromPosY.resize(nw);
		rows.wLastRequestedDir.resize(nw);
		rows.wTargetType.resize(nw);
		rows.wTargetIsUser.resize(nw);
		rows.wTargetUnitID.resize(nw);
		rows.wTargetGroundPos.resize(nw);
		rows.wTargetInterceptID.resize(nw);
		rows.wIsShield.resize(nw);
		rows.wShieldEnabled.resize(nw);
		rows.wShieldPower.resize(nw);
		rows.wDamages.resize(nw);
	}

	// Pass 2: flat per-weapon state (computed values -- AccuracyExperience/
	// SprayAngleExperience/SalvoErrorExperience/MoveErrorExperience are stored
	// resolved so the twins push scalars, never reproduce the experience math)
	for (const CUnit* u : activeUnits) {
		const int base = rows.weaponOffset[u->id];
		const auto& weapons = u->weapons;

		for (size_t w = 0; w < weapons.size(); ++w) {
			const CWeapon* weapon = weapons[w];
			const WeaponDef* wdef = weapon->weaponDef;
			const int wi = base + static_cast<int>(w);

			// GetUnitWeaponState
			rows.wAngleGood[wi] = weapon->angleGood;
			rows.wReloadStatus[wi] = weapon->reloadStatus;
			rows.wSalvoLeft[wi] = weapon->salvoLeft;
			rows.wNumStockpiled[wi] = weapon->numStockpiled;
			rows.wNextSalvo[wi] = weapon->nextSalvo;
			rows.wReloadTime[wi] = weapon->reloadTime;
			rows.wReaimTime[wi] = weapon->reaimTime;
			rows.wAccuracyExp[wi] = weapon->AccuracyExperience();
			rows.wSprayAngleExp[wi] = weapon->SprayAngleExperience();
			rows.wSalvoError[wi] = weapon->SalvoErrorExperience();
			rows.wMoveErrorExp[wi] = weapon->MoveErrorExperience();
			rows.wRange[wi] = weapon->range;
			rows.wProjectileSpeed[wi] = weapon->projectileSpeed;
			rows.wAutoTargetRangeBoost[wi] = weapon->autoTargetRangeBoost;
			rows.wSalvoSize[wi] = weapon->salvoSize;
			rows.wSalvoDelay[wi] = weapon->salvoDelay;
			rows.wSalvoWindup[wi] = weapon->salvoWindup;
			rows.wProjectilesPerShot[wi] = weapon->projectilesPerShot;
			rows.wAvoidFlags[wi] = weapon->avoidFlags;
			rows.wCollisionFlags[wi] = weapon->collisionFlags;
			rows.wTtl[wi] = weapon->ttl;

			// GetUnitWeaponVectors (dir switch resolved by the twin from projectileType)
			rows.wMuzzlePos[wi] = weapon->weaponMuzzlePos;
			rows.wWantedDir[wi] = weapon->wantedDir;
			rows.wWeaponDir[wi] = weapon->weaponDir;
			rows.wProjectileType[wi] = static_cast<int32_t>(wdef->projectileType);

			// GetUnitWeaponCanFire inputs (def scalars + runtime state)
			rows.wDefStockpile[wi] = wdef->stockpile;
			rows.wDefFireSubmersed[wi] = wdef->fireSubmersed;
			rows.wDefMaxFireAngle[wi] = wdef->maxFireAngle;
			rows.wIsBombDropper[wi] = (dynamic_cast<const CBombDropper*>(weapon) != nullptr);
			rows.wAimFromPosY[wi] = weapon->aimFromPos.y;
			rows.wLastRequestedDir[wi] = weapon->lastRequestedDir;

			// GetUnitWeaponTarget (SWeaponTarget)
			const SWeaponTarget& tgt = weapon->GetCurrentTarget();
			rows.wTargetType[wi] = static_cast<uint8_t>(tgt.type);
			rows.wTargetIsUser[wi] = tgt.isUserTarget;
			rows.wTargetUnitID[wi] = (tgt.type == Target_Unit && tgt.unit != nullptr) ? tgt.unit->id : 0;
			rows.wTargetGroundPos[wi] = (tgt.type == Target_Pos) ? tgt.groundPos : ZeroVector;
			rows.wTargetInterceptID[wi] = (tgt.type == Target_Intercept && tgt.intercept != nullptr) ? tgt.intercept->id : 0;

			// GetUnitShieldState explicit-weapon case (dynamic_cast in the live path)
			const CPlasmaRepulser* repulser = dynamic_cast<const CPlasmaRepulser*>(weapon);
			rows.wIsShield[wi] = (repulser != nullptr);
			rows.wShieldEnabled[wi] = (repulser != nullptr) ? uint8_t(repulser->IsEnabled()) : uint8_t(0);
			rows.wShieldPower[wi] = (repulser != nullptr) ? repulser->GetCurPower() : 0.0f;

			// GetUnitWeaponDamages per-weapon (flattened POD)
			CopyDamages(rows.wDamages[wi], weapon->damages);
		}
	}

	rows.simFrame = gs->frameNum;
	rows.aliveCount = static_cast<int32_t>(activeUnits.size());
}

void SimSnapshot::ExtractProjectiles(ProjectileRows& rows, size_t minSlots)
{
	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	const auto& pc = projectileHandler.GetActiveProjectiles(true);

	// synced projectile ids are free-list ints; rows grow-only to the max id
	// seen so slot indices stay stable across extractions
	int maxID = -1;
	for (size_t i = 0; i < pc.size(); ++i)
		maxID = std::max(maxID, pc[i]->id);

	// PR 43: also cover this batch's died-in-batch ids (DEAD_THIS_BATCH rows)
	const size_t wantSlots = std::max(static_cast<size_t>(maxID + 1), minSlots);

	if (rows.valid.size() < wantSlots || rows.numAllyTeams != numAllyTeams) {
		const size_t n = std::max(wantSlots, rows.valid.size());
		rows.numAllyTeams = numAllyTeams;
		rows.valid.resize(n, 0);
		rows.pos.resize(n);
		rows.speed.resize(n);
		rows.allyTeam.resize(n);
		rows.ownerID.resize(n);
		rows.isWeapon.resize(n);
		rows.weaponDefID.resize(n);
		rows.targetType.resize(n);
		rows.targetID.resize(n);
		rows.targetPos.resize(n);
		rows.isPiece.resize(n);
		rows.dir.resize(n);
		rows.mygravity.resize(n);
		rows.teamID.resize(n);
		rows.ttl.resize(n);
		rows.intercepted.resize(n);
		// PR 33 piece-projectile params
		rows.pieceExplFlags.resize(n);
		rows.pieceSpinAngle.resize(n);
		rows.pieceSpinSpeed.resize(n);
		rows.pieceSpinVec.resize(n);
		rows.pieceName.resize(n);
		rows.radius.resize(n); // PR 34 (spatial/list remainder)
		rows.damages.resize(n); // PR 38g (GetProjectileDamages)
		rows.drawRadius.resize(n); // PR 41 (GetVisibleProjectiles)
		rows.hitscan.resize(n);    // PR 41 (GetVisibleProjectiles)
		rows.inLosAll.resize(size_t(numAllyTeams) * n);
		rows.visInLosAll.resize(size_t(numAllyTeams) * n); // PR 41
	}

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	const size_t slots = rows.MaxSlots();

	for (size_t i = 0; i < pc.size(); ++i) {
		const CProjectile* p = pc[i];
		const int id = p->id;

		rows.valid[id] = SimSnapshotValid::ACTIVE;
		rows.pos[id] = p->pos;
		rows.speed[id] = p->speed;
		rows.allyTeam[id] = p->GetAllyteamID();
		rows.ownerID[id] = p->GetOwnerID();
		rows.isWeapon[id] = p->weapon;
		rows.isPiece[id] = p->piece;
		rows.dir[id] = p->dir;
		rows.mygravity[id] = p->mygravity;
		rows.radius[id] = p->radius; // PR 34 (spatial/list remainder)
		// PR 41 (GetVisibleProjectiles): draw-cull radius + hitscan membership flag.
		// drawRadius is draw-authored but sim-rate for synced projectiles (only the
		// unsynced CBitmapMuzzleFlame::Draw writes it at draw rate, and the callout
		// filters unsynced out), so this sim-boundary value equals the call-time live
		// p->GetDrawRadius() bit-for-bit.
		rows.drawRadius[id] = p->GetDrawRadius();
		rows.hitscan[id] = p->hitscan;
		rows.teamID[id] = static_cast<int32_t>(p->GetTeamID());
		rows.weaponDefID[id] = -1;
		rows.targetType[id] = 0;
		rows.targetID[id] = 0;
		rows.targetPos[id] = ZeroVector;
		rows.ttl[id] = 0;
		rows.intercepted[id] = 0;
		// PR 33 piece-projectile params (default; filled for piece projectiles)
		rows.pieceExplFlags[id] = 0;
		rows.pieceSpinAngle[id] = 0.0f;
		rows.pieceSpinSpeed[id] = 0.0f;
		rows.pieceSpinVec[id] = ZeroVector;
		rows.pieceName[id].clear();
		rows.damages[id].valid = 0; // PR 38g: default nil-shape; filled for weapon projectiles

		if (p->piece) {
			// GetPieceProjectileParams/Name serving (all synced state; the
			// piece-projectile ctor passes isSynced=true so these ids resolve
			// via GetProjectileBySyncedID, exactly as the live callouts require)
			const CPieceProjectile* ppro = static_cast<const CPieceProjectile*>(p);
			rows.pieceExplFlags[id] = ppro->explFlags;
			rows.pieceSpinAngle[id] = ppro->spinAngle;
			rows.pieceSpinSpeed[id] = ppro->spinSpeed;
			rows.pieceSpinVec[id] = ppro->spinVec;
			if (ppro->omp != nullptr)
				rows.pieceName[id] = ppro->omp->name;
		}

		if (p->weapon) {
			const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
			const WeaponDef* wdef = wpro->GetWeaponDef();
			const CWorldObject* wtgt = wpro->GetTargetObject();

			rows.weaponDefID[id] = (wdef != nullptr) ? wdef->id : -1;
			rows.ttl[id] = wpro->GetTimeToLive();
			rows.intercepted[id] = wpro->IsBeingIntercepted();
			// PR 38g GetProjectileDamages: flatten *wpro->damages into the POD
			// DamagesSnap (the PR-31 CopyDamages helper; sets valid=1). The live
			// body dereferences *wpro->damages unconditionally, so a weapon
			// projectile always has non-null damages (valid==1).
			CopyDamages(rows.damages[id], wpro->damages);

			// same type resolution as LuaSyncedRead::GetProjectileTarget
			if (wtgt == nullptr) {
				rows.targetType[id] = 'g';
				rows.targetPos[id] = wpro->GetTargetPos();
			} else if (dynamic_cast<const CUnit*>(wtgt) != nullptr) {
				rows.targetType[id] = 'u';
				rows.targetID[id] = wtgt->id;
			} else if (dynamic_cast<const CFeature*>(wtgt) != nullptr) {
				rows.targetType[id] = 'f';
				rows.targetID[id] = wtgt->id;
			} else if (dynamic_cast<const CWeaponProjectile*>(wtgt) != nullptr) {
				rows.targetType[id] = 'p';
				rows.targetID[id] = wtgt->id;
			}
		}

		for (int at = 0; at < numAllyTeams; ++at) {
			rows.inLosAll[at * slots + id] = losHandler->InLos(p->pos, at);
			// PR 41: the CWorldObject* overload (alwaysVisible / useAirLos /
			// two-position beam test) the GetVisibleProjectiles LOS filter uses --
			// captured exactly, faithful by construction
			rows.visInLosAll[at * slots + id] = losHandler->InLos(p, at);
		}
	}
}

void SimSnapshot::ExtractFeatures(FeatureRows& rows, size_t minSlots)
{
	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	const auto& activeIDs = featureHandler.GetActiveFeatureIDs();

	// feature ids are dense but sparse-occupancy; rows grow-only to the max id
	// seen so slot indices stay stable across extractions (projectile pattern)
	int maxID = -1;
	for (const int id : activeIDs)
		maxID = std::max(maxID, id);

	// PR 43: also cover this batch's died-in-batch ids (DEAD_THIS_BATCH rows)
	const size_t wantSlots = std::max(static_cast<size_t>(maxID + 1), minSlots);

	if (rows.valid.size() < wantSlots || rows.numAllyTeams != numAllyTeams) {
		const size_t n = std::max(wantSlots, rows.valid.size());
		rows.numAllyTeams = numAllyTeams;
		rows.valid.resize(n, 0);
		rows.pos.resize(n);
		rows.midPos.resize(n);
		rows.aimPos.resize(n);
		rows.relMidPos.resize(n);
		rows.radius.resize(n);
		rows.allyTeam.resize(n);
		rows.defID.resize(n);
		rows.alwaysVisible.resize(n);
		rows.noSelect.resize(n);
		rows.inVoid.resize(n);
		rows.selVol.resize(n);
		rows.team.resize(n);
		rows.health.resize(n);
		rows.resurrectProgress.resize(n);
		rows.height.resize(n);
		rows.mass.resize(n);
		rows.speed.resize(n);
		rows.matXdir.resize(n);
		rows.matYdir.resize(n);
		rows.matZdir.resize(n);
		rows.heading.resize(n);
		rows.buildFacing.resize(n);
		rows.resources.resize(n);
		rows.defResources.resize(n);
		rows.reclaimLeft.resize(n);
		rows.reclaimTime.resize(n);
		rows.blockingBits.resize(n);
		rows.resurrectDefID.resize(n);
		rows.fireTime.resize(n);  // PR 38g (GetFeatureFireTime)
		rows.smokeTime.resize(n); // PR 38g (GetFeatureSmokeTime)
		rows.inLosAll.resize(size_t(numAllyTeams) * n);
		// ---- PR 38c: per-feature rules-params mirror (grow-only like the rest) ----
		rows.featureRulesParams.resize(n);
	}

	std::fill(rows.valid.begin(), rows.valid.end(), 0);

	rows.featureVisibility = modInfo.featureVisibility;
	rows.gaiaAllyTeam = std::max(0, teamHandler.GaiaAllyTeamID());

	const size_t slots = rows.MaxSlots();

	for (const int id : activeIDs) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr)
			continue;

		rows.valid[id] = SimSnapshotValid::ACTIVE;
		rows.pos[id] = f->pos;
		rows.midPos[id] = f->midPos;
		rows.aimPos[id] = f->aimPos;
		rows.relMidPos[id] = f->relMidPos;
		rows.radius[id] = f->radius;
		rows.allyTeam[id] = f->allyteam;
		rows.defID[id] = f->def->id;
		rows.alwaysVisible[id] = f->alwaysVisible;
		rows.noSelect[id] = f->noSelect;
		rows.inVoid[id] = f->IsInVoid();
		rows.selVol[id] = f->selectionVolume;
		rows.team[id] = f->team;
		rows.health[id] = f->health;
		rows.resurrectProgress[id] = f->resurrectProgress;
		rows.height[id] = f->height;
		rows.mass[id] = f->mass;
		rows.speed[id] = f->speed;
		{
			const CMatrix44f& fm = f->GetTransformMatrixRef();
			rows.matXdir[id] = fm.GetX();
			rows.matYdir[id] = fm.GetY();
			rows.matZdir[id] = fm.GetZ();
		}
		rows.heading[id] = f->heading;
		rows.buildFacing[id] = f->buildFacing;
		rows.resources[id] = f->resources;
		rows.defResources[id] = f->defResources;
		rows.reclaimLeft[id] = f->reclaimLeft;
		rows.reclaimTime[id] = f->reclaimTime;
		rows.blockingBits[id] = PackBlockingBits(f);
		rows.resurrectDefID[id] = (f->udef != nullptr) ? f->udef->id : -1;
		rows.fireTime[id] = f->fireTime;   // PR 38g (GetFeatureFireTime)
		rows.smokeTime[id] = f->smokeTime; // PR 38g (GetFeatureSmokeTime)

		// ---- PR 38c: per-feature rules-params (values only, like the unit copy) ----
		rows.featureRulesParams[id] = f->modParams;

		for (int at = 0; at < numAllyTeams; ++at)
			rows.inLosAll[at * slots + id] = losHandler->InLos(f->pos, at);
	}
}


// ---------------------------------------------------------------------------
// PR 43 §7.7: shell-sourced DEAD_THIS_BATCH row extraction (producer side)
//
// The shells parked in DeferredObjectDeleter are PreDestruct()ed: their PLAIN
// members (pos/health/team/losStatus/modParams/def pointers/...) stay readable
// until the step-8 ack, but the sub-objects PreDestruct destroys are gone --
// CUnit::commandAI/moveType/weapons/script (and the projectile damage array).
// The shell rows therefore carry genuine at-death values for every plain-field
// row and DOCUMENTED DEFAULTS for the destroyed sub-blocks (weapon family
// count 0, moveTypeKind 0/unknown, build-state none, estPath none, projectile
// damages nil-shape, weapon-projectile target degraded to its ground pos).
// Positional/LOS-map queries are computed against the live maps with shell
// inputs (memory-safe; the map state is post-death, the position at-death).
// KEEP THE FIELD LISTS IN SYNC with Extract/ExtractFeatures/ExtractProjectiles
// -- a field added there must be either mirrored or explicitly defaulted here,
// or a DEAD_THIS_BATCH row would serve stale slot garbage for it.
// ---------------------------------------------------------------------------

void SimSnapshot::ExtractDeadRowsFromShells(UnitRows& urows, FeatureRows& frows, ProjectileRows& prows,
	const std::vector<std::pair<int, const CUnit*>>& deadUnits,
	const std::vector<std::pair<int, const CFeature*>>& deadFeatures,
	const std::vector<std::pair<int, const CProjectile*>>& deadProjectiles)
{
	const int numAllyTeams = teamHandler.ActiveAllyTeams();

	const size_t maxUnits = urows.MaxUnits();
	for (const auto& [id, u] : deadUnits) {
		// INACTIVE guard: a slot a new object reused this same batch (already
		// ACTIVE) is never clobbered; out-of-range cannot happen (fixed maxUnits)
		if (static_cast<size_t>(id) >= maxUnits || urows.valid[id] != SimSnapshotValid::INACTIVE)
			continue;

		urows.valid[id] = SimSnapshotValid::DEAD_THIS_BATCH;
		// ---- plain-field rows (genuine at-death values off the shell) ----
		urows.pos[id] = u->pos;
		urows.midPos[id] = u->midPos;
		urows.aimPos[id] = u->aimPos;
		urows.speed[id] = u->speed;
		urows.health[id] = u->health;
		urows.maxHealth[id] = u->maxHealth;
		urows.paralyzeDamage[id] = u->paralyzeDamage;
		urows.captureProgress[id] = u->captureProgress;
		urows.team[id] = static_cast<uint8_t>(u->team);
		urows.allyTeam[id] = static_cast<uint8_t>(u->allyteam);
		urows.defID[id] = u->unitDef->id;
		urows.buildProgress[id] = u->buildProgress;
		urows.beingBuilt[id] = u->beingBuilt;
		urows.stunned[id] = u->IsStunned();
		urows.radius[id] = u->radius;
		urows.selVol[id] = u->selectionVolume;
		urows.noSelect[id] = u->noSelect;
		urows.inVoid[id] = u->IsInVoid();
		urows.isDead[id] = u->isDead;
		urows.neutral[id] = u->neutral;
		urows.activated[id] = u->activated;
		urows.isCloaked[id] = u->isCloaked;
		urows.armoredState[id] = u->armoredState;
		urows.armoredMultiple[id] = u->armoredMultiple;
		urows.heading[id] = u->heading;
		urows.buildFacing[id] = u->buildFacing;
		urows.height[id] = u->height;
		urows.mass[id] = u->mass;
		urows.maxRange[id] = u->maxRange;
		urows.seismicSignature[id] = u->seismicSignature;
		urows.experience[id] = u->experience;
		urows.limExperience[id] = u->limExperience;
		urows.selfDCountdown[id] = u->selfDCountdown;
		urows.losRadius[id] = u->losRadius;
		urows.airLosRadius[id] = u->airLosRadius;
		urows.radarRadius[id] = u->radarRadius;
		urows.sonarRadius[id] = u->sonarRadius;
		urows.seismicRadius[id] = u->seismicRadius;
		urows.jammerRadius[id] = u->jammerRadius;
		urows.sonarJamRadius[id] = u->sonarJamRadius;
		urows.moveDefID[id] = (u->moveDef != nullptr) ? static_cast<int32_t>(u->moveDef->pathType) : -1;
		urows.resourcesMake[id] = u->resourcesMake;
		urows.resourcesUse[id] = u->resourcesUse;
		urows.harvested[id] = u->harvested;
		urows.harvestStorage[id] = u->harvestStorage;
		urows.cost[id] = u->cost;
		urows.buildTime[id] = u->buildTime;
		urows.blockingBits[id] = PackBlockingBits(u);
		urows.relMidPos[id] = u->relMidPos;
		urows.frontdir[id] = u->frontdir;
		urows.updir[id] = u->updir;
		urows.rightdir[id] = u->rightdir;
		urows.posErrorVector[id] = u->posErrorVector;
		urows.leavesGhost[id] = u->leavesGhost;
		urows.fireState[id] = u->fireState;
		urows.moveState[id] = u->moveState;
		urows.wantCloak[id] = u->wantCloak;
		urows.useHighTrajectory[id] = u->useHighTrajectory;
		urows.storage[id] = u->storage;
		urows.metalExtract[id] = u->metalExtract;
		urows.buildeeRadius[id] = u->buildeeRadius;
		urows.posErrorDelta[id] = u->posErrorDelta;
		urows.nextPosErrorUpdate[id] = u->nextPosErrorUpdate;
		urows.customTooltip[id] = unitToolTipMap.GetConst(id);
		urows.unitRulesParams[id] = u->modParams;

		// ---- destroyed-sub-block defaults (PreDestruct freed the owners) ----
		urows.repairBelowHealth[id] = -1.0f;     // commandAI gone
		urows.repeatOrders[id] = 0;
		urows.lastAttackerID[id] = -1;           // dependence-severed pointer
		urows.transporterID[id] = -1;
		urows.transportees[id].clear();
		urows.builderKind[id] = 0;               // build-state family: none
		urows.curBuildID[id] = -1;
		urows.buildDistance[id] = 0.0f;
		urows.range3D[id] = 0;
		urows.inBuildStance[id] = 0;
		urows.buildPower[id] = 0.0f;
		urows.nanoPieces[id].clear();
		urows.moveTypeKind[id] = 0;              // moveType gone: "unknown"
		urows.mtMaxSpeed[id] = 0.0f;
		urows.mtMaxWantedSpeed[id] = 0.0f;
		urows.mtGoalPos[id] = ZeroVector;
		urows.mtProgressState[id] = 0;
		urows.mtAutoLand[id] = 0;
		urows.mtLoopbackAttack[id] = 0;
		urows.moveTypeBlock[id] = MoveTypeBlock{};
		urows.estPathHasPath[id] = 0;
		urows.estPathPoints[id].clear();
		urows.estPathStarts[id].clear();
		// weapon family: weapons freed in PreDestruct -> the count-0 nil shape
		// (no flat-array slots referenced); explosion damages were decref'd
		urows.weaponOffset[id] = 0;
		urows.weaponCount[id] = 0;
		urows.reloadSpeed[id] = u->reloadSpeed;
		urows.fpsNoFire[id] = 0;                 // fpsControlPlayer nulled
		urows.flankingMode[id] = u->flankingBonusMode;
		urows.flankingDir[id] = u->flankingBonusDir;
		urows.flankingMoveFactor[id] = u->flankingBonusMobilityAdd;
		urows.flankingAvgDamage[id] = u->flankingBonusAvgDamage;
		urows.flankingDifDamage[id] = u->flankingBonusDifDamage;
		urows.flankingMobility[id] = u->flankingBonusMobility;
		urows.hasStockpile[id] = 0;
		urows.stockpileNumStockpiled[id] = 0;
		urows.stockpileNumQueued[id] = 0;
		urows.stockpileBuildPercent[id] = 0.0f;
		urows.hasShieldWeapon[id] = 0;
		urows.shieldWeaponEnabled[id] = 0;
		urows.shieldWeaponPower[id] = 0.0f;
		urows.deathExpDamages[id].valid = 0;
		urows.selfdExpDamages[id].valid = 0;

		// ---- per-allyteam strides (plain losStatus bits off the shell; the
		// map-backed answers are computed with shell inputs against the live
		// post-death maps -- memory-safe, position at-death) ----
		for (int at = 0; at < numAllyTeams; ++at) {
			urows.losStatusAll[at * maxUnits + id] = u->losStatus[at];
			urows.posErrorBits[at * maxUnits + id] = u->GetPosErrorBit(at);
			urows.inRadarAll[at * maxUnits + id] = losHandler->InRadar(u, at);
			urows.unitInLosAll[at * maxUnits + id] = losHandler->InLos(u, at);
			urows.unitInAirLosAll[at * maxUnits + id] = losHandler->InAirLos(u, at);
			urows.unitInJammerAll[at * maxUnits + id] = losHandler->InJammer(u, at);
		}
	}

	const size_t featSlots = frows.MaxSlots();
	for (const auto& [id, f] : deadFeatures) {
		if (static_cast<size_t>(id) >= featSlots || frows.valid[id] != SimSnapshotValid::INACTIVE)
			continue;

		// feature shells have no destroyed sub-blocks in their row surface:
		// every field below is a plain member / immutable def pointer read
		frows.valid[id] = SimSnapshotValid::DEAD_THIS_BATCH;
		frows.pos[id] = f->pos;
		frows.midPos[id] = f->midPos;
		frows.aimPos[id] = f->aimPos;
		frows.relMidPos[id] = f->relMidPos;
		frows.radius[id] = f->radius;
		frows.allyTeam[id] = f->allyteam;
		frows.defID[id] = f->def->id;
		frows.alwaysVisible[id] = f->alwaysVisible;
		frows.noSelect[id] = f->noSelect;
		frows.inVoid[id] = f->IsInVoid();
		frows.selVol[id] = f->selectionVolume;
		frows.team[id] = f->team;
		frows.health[id] = f->health;
		frows.resurrectProgress[id] = f->resurrectProgress;
		frows.height[id] = f->height;
		frows.mass[id] = f->mass;
		frows.speed[id] = f->speed;
		{
			const CMatrix44f& fm = f->GetTransformMatrixRef();
			frows.matXdir[id] = fm.GetX();
			frows.matYdir[id] = fm.GetY();
			frows.matZdir[id] = fm.GetZ();
		}
		frows.heading[id] = f->heading;
		frows.buildFacing[id] = f->buildFacing;
		frows.resources[id] = f->resources;
		frows.defResources[id] = f->defResources;
		frows.reclaimLeft[id] = f->reclaimLeft;
		frows.reclaimTime[id] = f->reclaimTime;
		frows.blockingBits[id] = PackBlockingBits(f);
		frows.resurrectDefID[id] = (f->udef != nullptr) ? f->udef->id : -1;
		frows.fireTime[id] = f->fireTime;
		frows.smokeTime[id] = f->smokeTime;
		frows.featureRulesParams[id] = f->modParams;

		for (int at = 0; at < numAllyTeams; ++at)
			frows.inLosAll[at * featSlots + id] = losHandler->InLos(f->pos, at);
	}

	const size_t projSlots = prows.MaxSlots();
	for (const auto& [id, p] : deadProjectiles) {
		// the map is filtered to the SYNCED id namespace at record dispatch
		// (the fd41dbdd92 rule: ProjectileRows holds only synced projectiles,
		// and an unsynced destroy id must never shadow a live synced row)
		if (static_cast<size_t>(id) >= projSlots || prows.valid[id] != SimSnapshotValid::INACTIVE)
			continue;

		prows.valid[id] = SimSnapshotValid::DEAD_THIS_BATCH;
		prows.pos[id] = p->pos;
		prows.speed[id] = p->speed;
		prows.allyTeam[id] = p->GetAllyteamID();
		prows.ownerID[id] = p->GetOwnerID();
		prows.isWeapon[id] = p->weapon;
		prows.isPiece[id] = p->piece;
		prows.dir[id] = p->dir;
		prows.mygravity[id] = p->mygravity;
		prows.radius[id] = p->radius;
		prows.drawRadius[id] = p->GetDrawRadius();
		prows.hitscan[id] = p->hitscan;
		prows.teamID[id] = static_cast<int32_t>(p->GetTeamID());
		prows.weaponDefID[id] = -1;
		prows.targetType[id] = 0;
		prows.targetID[id] = 0;
		prows.targetPos[id] = ZeroVector;
		prows.ttl[id] = 0;
		prows.intercepted[id] = 0;
		prows.pieceExplFlags[id] = 0;
		prows.pieceSpinAngle[id] = 0.0f;
		prows.pieceSpinSpeed[id] = 0.0f;
		prows.pieceSpinVec[id] = ZeroVector;
		prows.pieceName[id].clear();
		// nil-shape default: the damage array was decref'd at PreDestruct
		prows.damages[id].valid = 0;

		if (p->piece) {
			const CPieceProjectile* ppro = static_cast<const CPieceProjectile*>(p);
			prows.pieceExplFlags[id] = ppro->explFlags;
			prows.pieceSpinAngle[id] = ppro->spinAngle;
			prows.pieceSpinSpeed[id] = ppro->spinSpeed;
			prows.pieceSpinVec[id] = ppro->spinVec;
			if (ppro->omp != nullptr)
				prows.pieceName[id] = ppro->omp->name;
		}

		if (p->weapon) {
			const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
			const WeaponDef* wdef = wpro->GetWeaponDef();

			prows.weaponDefID[id] = (wdef != nullptr) ? wdef->id : -1;
			prows.ttl[id] = wpro->GetTimeToLive();
			prows.intercepted[id] = wpro->IsBeingIntercepted();
			// DOCUMENTED DEFAULT: the target OBJECT pointer is dependence-
			// severed on a shell, so the at-death target degrades to its
			// stored ground position ('g' form)
			prows.targetType[id] = 'g';
			prows.targetPos[id] = wpro->GetTargetPos();
		}

		for (int at = 0; at < numAllyTeams; ++at) {
			prows.inLosAll[at * projSlots + id] = losHandler->InLos(p->pos, at);
			prows.visInLosAll[at * projSlots + id] = losHandler->InLos(p, at);
		}
	}
}

// PR 43: revert the published slot's DEAD_THIS_BATCH marks to INACTIVE at the
// barrier's step-8 window close. A linear scan of the held slot's valid[]
// arrays -- robust to any id churn; only ==DEAD_THIS_BATCH slots are touched.
static inline void ClearDeadRows(std::vector<uint8_t>& valid)
{
	for (uint8_t& v : valid) {
		if (v == SimSnapshotValid::DEAD_THIS_BATCH)
			v = SimSnapshotValid::INACTIVE;
	}
}

void SimSnapshot::ClearDeadThisBatch()
{
	const int held = HeldIdx();
	ClearDeadRows(buffers[held].valid);
	ClearDeadRows(featBuffers[held].valid);
	ClearDeadRows(projBuffers[held].valid);
}

void SimSnapshot::ExtractTeams(TeamRows& rows)
{
	const int activeTeams = teamHandler.ActiveTeams();

	// global block
	rows.activeTeams = activeTeams;
	rows.activeAllyTeams = teamHandler.ActiveAllyTeams();
	rows.gaiaTeamID = teamHandler.GaiaTeamID();
	rows.useLuaGaia = gs->useLuaGaia;
	rows.gameOver = (game != nullptr && game->IsGameOver());

	if (rows.leader.size() != static_cast<size_t>(activeTeams)) {
		rows.leader.resize(activeTeams);
		rows.isDead.resize(activeTeams);
		rows.hasAIs.resize(activeTeams);
		rows.allyTeam.resize(activeTeams);
		rows.incomeMultiplier.resize(activeTeams);
		rows.numUnits.resize(activeTeams);
		rows.color.resize(activeTeams);
		rows.origColor.resize(activeTeams);
		rows.sideName.resize(activeTeams);
		rows.currentStats.resize(activeTeams);
		rows.res.resize(activeTeams);
		rows.resStorage.resize(activeTeams);
		rows.resPrevPull.resize(activeTeams);
		rows.resPrevIncome.resize(activeTeams);
		rows.resPrevExpense.resize(activeTeams);
		rows.resShare.resize(activeTeams);
		rows.resPrevSent.resize(activeTeams);
		rows.resPrevReceived.resize(activeTeams);
		rows.resPrevExcess.resize(activeTeams);
		rows.customOpts.resize(activeTeams);
		// ---- PR 36: team-misc per-team vectors (same activeTeams sizing) ----
		rows.startPos.resize(activeTeams);
		rows.hasValidStartPos.resize(activeTeams);
		rows.maxUnits.resize(activeTeams);
		rows.hasLuaAI.resize(activeTeams);
		rows.luaAIName.resize(activeTeams);
		rows.aiHasAI.resize(activeTeams);
		rows.aiID.resize(activeTeams);
		rows.aiName.resize(activeTeams);
		rows.aiHostPlayer.resize(activeTeams);
		rows.aiIsLocal.resize(activeTeams);
		rows.aiShortName.resize(activeTeams);
		rows.aiVersion.resize(activeTeams);
		rows.aiOptions.resize(activeTeams);
		rows.statHistory.resize(activeTeams);
		// ---- PR 38: per-team rules-params mirror (same activeTeams sizing) ----
		rows.teamRulesParams.resize(activeTeams);
	}

	// PR 36: per-allyteam block (GetAllyTeamStartBox / GetAllyTeamInfo); sized
	// to activeAllyTeams, which differs from activeTeams -> its own resize guard
	const int activeAllyTeams = teamHandler.ActiveAllyTeams();
	if (rows.allyStartBox.size() != static_cast<size_t>(activeAllyTeams)) {
		rows.allyStartBox.resize(activeAllyTeams);
		rows.allyTeamOpts.resize(activeAllyTeams);
	}

	for (int t = 0; t < activeTeams; ++t) {
		const CTeam* team = teamHandler.Team(t);

		rows.leader[t] = team->GetLeader();
		rows.isDead[t] = team->isDead;
		rows.hasAIs[t] = skirmishAIHandler.HasSkirmishAIsInTeam(t);
		rows.allyTeam[t] = teamHandler.AllyTeam(t);
		rows.incomeMultiplier[t] = team->GetIncomeMultiplier();
		rows.numUnits[t] = static_cast<int32_t>(unitHandler.NumUnitsByTeam(t));
		std::memcpy(rows.color[t].data(), team->color, 4);
		std::memcpy(rows.origColor[t].data(), team->origColor, 4);
		CopyString(rows.sideName[t], team->GetSideName());
		rows.currentStats[t] = team->GetCurrentStats();
		rows.res[t] = team->res;
		rows.resStorage[t] = team->resStorage;
		rows.resPrevPull[t] = team->resPrevPull;
		rows.resPrevIncome[t] = team->resPrevIncome;
		rows.resPrevExpense[t] = team->resPrevExpense;
		rows.resShare[t] = team->resShare;
		rows.resPrevSent[t] = team->resPrevSent;
		rows.resPrevReceived[t] = team->resPrevReceived;
		rows.resPrevExcess[t] = team->resPrevExcess;
		CopyOpts(rows.customOpts[t], team->GetAllValues());

		// ---- PR 36: team-misc ----
		rows.startPos[t] = team->GetStartPos();
		rows.hasValidStartPos[t] = team->HasValidStartPos();
		rows.maxUnits[t] = static_cast<int32_t>(team->GetMaxUnits());
		// the back() entry is the mutating currentStats, so this is copied every
		// boundary (vector assign reuses capacity; the history is short)
		rows.statHistory[t] = team->statHistory;

		// ---- PR 38: per-team rules-params (values only; the Param variant is a
		// bool/float/std::string, no pointer/sim-owned container) ----
		rows.teamRulesParams[t] = team->modParams;

		// GetTeamLuaAI: first isLuaAI shortName ("" = none)
		const std::vector<uint8_t>& teamAIs = skirmishAIHandler.GetSkirmishAIsInTeam(t);
		const std::string* luaAIName = nullptr;
		for (uint8_t id: teamAIs) {
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(id);
			if (!aiData->isLuaAI)
				continue;
			luaAIName = &aiData->shortName;
			break;
		}
		rows.hasLuaAI[t] = (luaAIName != nullptr);
		CopyString(rows.luaAIName[t], (luaAIName != nullptr) ? *luaAIName : std::string());

		// GetAIInfo: teamAIs[0] block
		if (teamAIs.empty()) {
			rows.aiHasAI[t] = 0;
			rows.aiID[t] = -1;
			rows.aiHostPlayer[t] = -1;
			rows.aiIsLocal[t] = 0;
			CopyString(rows.aiName[t], std::string());
			CopyString(rows.aiShortName[t], std::string());
			CopyString(rows.aiVersion[t], std::string());
			if (!rows.aiOptions[t].empty())
				rows.aiOptions[t].clear();
		} else {
			const size_t skirmishAIId = teamAIs[0];
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(skirmishAIId);
			rows.aiHasAI[t] = 1;
			rows.aiID[t] = static_cast<int32_t>(skirmishAIId);
			rows.aiHostPlayer[t] = aiData->hostPlayer;
			CopyString(rows.aiName[t], aiData->name);
			rows.aiIsLocal[t] = skirmishAIHandler.IsLocalSkirmishAI(skirmishAIId);
			if (rows.aiIsLocal[t] != 0) {
				CopyString(rows.aiShortName[t], aiData->shortName);
				CopyString(rows.aiVersion[t], aiData->version);
				CopyOpts(rows.aiOptions[t], aiData->options);
			} else {
				CopyString(rows.aiShortName[t], std::string());
				CopyString(rows.aiVersion[t], std::string());
				if (!rows.aiOptions[t].empty())
					rows.aiOptions[t].clear();
			}
		}
	}

	// PR 36: per-allyteam start box (live float order) + custom options
	for (int at = 0; at < activeAllyTeams; ++at) {
		const AllyTeam& ally = teamHandler.GetAllyTeam(at);
		rows.allyStartBox[at] = float4(
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectLeft,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectTop,
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectRight,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectBottom);
		CopyOpts(rows.allyTeamOpts[at], ally.GetAllValues());
	}
}

// PR 36: parse the map-defined start positions once (LoadStartPositionsFromMap
// re-parses the map file on every call, so the live GetMapStartPositions cost is
// paid a single time here). Runs at the boundary with the sim parked (or single-
// threaded), the same context the live callout ran in.
void SimSnapshot::CacheMapStartPositions()
{
	if (mapStartPosCached)
		return;

	mapStartPos.assign(MAX_TEAMS, float3());
	mapStartPosValid.assign(MAX_TEAMS, uint8_t(0));

	if (gameSetup != nullptr) {
		gameSetup->LoadStartPositionsFromMap(MAX_TEAMS, [&](MapParser& mapParser, int teamNum) {
			float3 pos;
			if (!mapParser.GetStartPos(teamNum, pos))
				return false;
			if (teamNum >= 0 && teamNum < MAX_TEAMS) {
				mapStartPos[teamNum] = pos;
				mapStartPosValid[teamNum] = 1;
			}
			return true;
		});
	}

	mapStartPosCached = true;
}

void SimSnapshot::ExtractPlayers(PlayerRows& rows)
{
	const int activePlayers = static_cast<int>(playerHandler.ActivePlayers());

	rows.activePlayers = activePlayers;
	rows.hostDemo = gameSetup->hostDemo;

	if (rows.name.size() != static_cast<size_t>(activePlayers)) {
		rows.name.resize(activePlayers);
		rows.countryCode.resize(activePlayers);
		rows.team.resize(activePlayers);
		rows.rank.resize(activePlayers);
		rows.ping.resize(activePlayers);
		rows.cpuUsage.resize(activePlayers);
		rows.active.resize(activePlayers);
		rows.spectator.resize(activePlayers);
		rows.isFromDemo.resize(activePlayers);
		rows.desynced.resize(activePlayers);
		rows.customOpts.resize(activePlayers);
		// ---- PR 36: GetPlayerControlledUnit / GetPlayerStatistics ----
		rows.controlleeID.resize(activePlayers);
		rows.controlleeAllyTeam.resize(activePlayers);
		rows.currentStats.resize(activePlayers);
		// ---- PR 38c: per-player rules-params mirror (fixed-count roster) ----
		rows.playerRulesParams.resize(activePlayers);
	}

	for (int p = 0; p < activePlayers; ++p) {
		const CPlayer* player = playerHandler.Player(p);

		CopyString(rows.name[p], player->name);
		CopyString(rows.countryCode[p], player->countryCode);
		rows.team[p] = player->team;
		rows.rank[p] = player->rank;
		rows.ping[p] = player->ping;
		rows.cpuUsage[p] = player->cpuUsage;
		rows.active[p] = player->active;
		rows.spectator[p] = player->spectator;
		rows.isFromDemo[p] = player->isFromDemo;
		rows.desynced[p] = player->desynced;
		CopyOpts(rows.customOpts[p], player->GetAllValues());

		// ---- PR 36 ----
		// GetPlayerControlledUnit: the FPS-controlled unit's id + allyteam
		const CUnit* controllee = player->fpsController.GetControllee();
		rows.controlleeID[p] = (controllee != nullptr) ? controllee->id : -1;
		rows.controlleeAllyTeam[p] = (controllee != nullptr) ? controllee->allyteam : -1;
		// GetPlayerStatistics: the input/command stat block (POD copy)
		rows.currentStats[p] = player->currentStats;

		// ---- PR 38c: per-player rules-params (values only, like the unit copy) ----
		rows.playerRulesParams[p] = player->modParams;
	}
}

void SimSnapshot::ExtractGlobals(GlobalRows& rows)
{
	rows.luaSimFrame = gs->GetLuaSimFrame();

	rows.wantedSpeedFactor = gs->wantedSpeedFactor;
	rows.speedFactor = gs->speedFactor;
	rows.paused = gs->paused;

	rows.cheatEnabled = gs->cheatEnabled;
	rows.godMode = gs->godMode;
	rows.editDefsEnabled = gs->editDefsEnabled;
	rows.noHelperAIs = gs->noHelperAIs;
	rows.defsNoCost = (unitDefHandler != nullptr && unitDefHandler->GetNoCost());

	rows.doneLoading = (game != nullptr && game->IsDoneLoading());
	rows.savedGame = (game != nullptr && game->IsSavedGame());
	rows.clientPaused = (game != nullptr && game->IsClientPaused());

	rows.windVec = envResHandler.GetCurrentWindVec();
	rows.windDir = envResHandler.GetCurrentWindDir();
	rows.windStrength = envResHandler.GetCurrentWindStrength();

	rows.initMinHeight = readMap->GetInitMinHeight();
	rows.initMaxHeight = readMap->GetInitMaxHeight();
	rows.currMinHeight = readMap->GetCurrMinHeight();
	rows.currMaxHeight = readMap->GetCurrMaxHeight();

	const int numAllyTeams = teamHandler.ActiveAllyTeams();
	rows.numAllyTeams = numAllyTeams;
	rows.globalLos.resize(numAllyTeams);
	for (int at = 0; at < numAllyTeams; ++at) {
		rows.globalLos[at] = losHandler->GetGlobalLOS(at);
	}

	// ---- PR 38: game rules-params (values only; the singleton game param map,
	// mutated only by synced Spring.SetGameRulesParam, safe to read at the parked
	// barrier / single-threaded) ----
	rows.gameRulesParams = CSplitLuaHandle::GetGameParams();
}
