/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/EventHandler.h"

#include "Lua/LuaCallInCheck.h"
#include "Lua/LuaOpenGL.h"  // FIXME -- should be moved
#include "Lua/LuaSnapshotServe.h" // PR 38f: event-time command-queue presentation

#include "Sim/Units/CommandAI/Command.h" // PR 38f: std::bind copies Command by value
#include "Sim/Units/UnitHandler.h"

#include "Rendering/Common/SimSnapshot.h" // PR 38f: event-time LOS-exit visibility override

#include "System/Config/ConfigHandler.h"
#include "System/Platform/Threading.h"
#include "System/GlobalConfig.h"

#include "System/Misc/TracyDefs.h"

CEventHandler eventHandler;


/******************************************************************************/
/******************************************************************************/

void CEventHandler::SetupEvent(const std::string& eName, EventClientList* list, int props)
{
	assert(std::find_if(eventMap.cbegin(), eventMap.cend(), [&](const EventPair& p) { return (p.first == eName); }) == eventMap.cend());
	eventMap.push_back({eName, EventInfo(eName, list, props)});
}

/******************************************************************************/
/******************************************************************************/

CEventHandler::CEventHandler()
{
	ResetState();
}

void CEventHandler::ResetState()
{
	mouseOwner = nullptr;

	eventMap.clear();
	eventMap.reserve(64);
	handles.clear();
	handles.reserve(16);

	SetupEvents();
}

void CEventHandler::SetupEvents()
{
	#define SETUP_EVENT(name, props) SetupEvent(#name, &list ## name, props);
	#define SETUP_UNMANAGED_EVENT(name, props) SetupEvent(#name, NULL, props);
		#include "Events.def"
	#undef SETUP_UNMANAGED_EVENT
	#undef SETUP_EVENT

	// sort by name
	std::stable_sort(eventMap.begin(), eventMap.end(), [](const EventPair& a, const EventPair& b) { return (a.first < b.first); });
}


/******************************************************************************/
/******************************************************************************/

void CEventHandler::AddClient(CEventClient* ec)
{
	ListInsert(handles, ec);

	for (const auto& element: eventMap) {
		const EventInfo& ei = element.second;

		if (!ei.HasPropBit(MANAGED_BIT))
			continue;

		if (!ec->WantsEvent(element.first))
			continue;

		InsertEvent(ec, element.first);
	}
}

void CEventHandler::RemoveClient(CEventClient* ec)
{
	if (mouseOwner == ec)
		mouseOwner = nullptr;

	ListRemove(handles, ec);

	for (const auto& element: eventMap) {
		const EventInfo& ei = element.second;

		if (!ei.HasPropBit(MANAGED_BIT))
			continue;

		RemoveEvent(ec, element.first);
	}
}


/******************************************************************************/
/******************************************************************************/

void CEventHandler::GetEventList(std::vector<std::string>& list) const
{
	list.clear();

	for (const auto& element: eventMap) {
		list.push_back(element.first);
	}
}


bool CEventHandler::IsKnown(const std::string& eName) const
{
	// std::binary_search does not return an iterator
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{eName, {}}, comp);
	return (iter != eventMap.end() && iter->first == eName);
}


bool CEventHandler::IsManaged(const std::string& eName) const
{
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{eName, {}}, comp);
	return (iter != eventMap.end() && iter->second.HasPropBit(MANAGED_BIT) && iter->first == eName);
}


bool CEventHandler::IsUnsynced(const std::string& eName) const
{
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{eName, {}}, comp);
	return (iter != eventMap.end() && iter->second.HasPropBit(UNSYNCED_BIT) && iter->first == eName);
}


bool CEventHandler::IsController(const std::string& eName) const
{
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{eName, {}}, comp);
	return (iter != eventMap.end() && iter->second.HasPropBit(CONTROL_BIT) && iter->first == eName);
}


/******************************************************************************/

bool CEventHandler::InsertEvent(CEventClient* ec, const std::string& ciName)
{
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{ciName, {}}, comp);

	if ((iter == eventMap.end()) || (iter->second.GetList() == nullptr) || (iter->first != ciName))
		return false;

	if (ec->GetSynced() && iter->second.HasPropBit(UNSYNCED_BIT))
		return false;

	ListInsert(*iter->second.GetList(), ec);
	return true;
}


bool CEventHandler::RemoveEvent(CEventClient* ec, const std::string& ciName)
{
	const auto comp = [](const EventPair& a, const EventPair& b) { return (a.first < b.first); };
	const auto iter = std::lower_bound(eventMap.begin(), eventMap.end(), EventPair{ciName, {}}, comp);

	if ((iter == eventMap.end()) || (iter->second.GetList() == nullptr) || (iter->first != ciName))
		return false;

	ListRemove(*(iter->second.GetList()), ec);
	return true;
}


/******************************************************************************/

void CEventHandler::ListInsert(EventClientList& ecList, CEventClient* ec)
{
	for (auto it = ecList.begin(); it != ecList.end(); ++it) {
		const CEventClient* ecIt = *it;

		if (ec == ecIt)
			return; // already in the list

		if (ec->GetOrder() < ecIt->GetOrder()) {
			ecList.insert(it, ec);
			return;
		}
		// should not happen
		if ((ec->GetOrder() == ecIt->GetOrder()) && (ec->GetName() < ecIt->GetName())) {
			ecList.insert(it, ec);
			return;
		}
	}

	ecList.push_back(ec);
}

void CEventHandler::ListRemove(EventClientList& ecList, CEventClient* ec)
{
	const auto ecIt = std::find(ecList.begin(), ecList.end(), ec);

	// erase does not accept end()
	if (ecIt == ecList.end())
		return;

	ecList.erase(ecIt);
}


/******************************************************************************/
/******************************************************************************/

template<typename T, typename F, typename... A> bool ControlIterateDefTrue(T& list, const F& func, A&&... args) {
	bool result = true;

	for (size_t i = 0; i < list.size(); ) {
		CEventClient* ec = list[i];

		result &= (ec->*func)(std::forward<A>(args)...);

		// the call-in may remove itself from the list
		i += (i < list.size() && ec == list[i]);
	}

	return result;
}

template<typename T, typename F, typename... A> bool ControlIterateDefFalse(T& list, const F& func, A&&... args) {
	bool result = false;

	for (size_t i = 0; i < list.size(); ) {
		CEventClient* ec = list[i];

		result |= (ec->*func)(std::forward<A>(args)...);

		// the call-in may remove itself from the list
		i += (i < list.size() && ec == list[i]);
	}

	return result;
}



bool CEventHandler::CommandFallback(const CUnit* unit, const Command& cmd)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listCommandFallback, &CEventClient::CommandFallback, unit, cmd);
}


bool CEventHandler::AllowCommand(const CUnit* unit, const Command& cmd, int playerNum, bool fromSynced, bool fromLua)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowCommand, &CEventClient::AllowCommand, unit, cmd, playerNum, fromSynced, fromLua);
}


std::pair <bool, bool> CEventHandler::AllowUnitCreation(const UnitDef* unitDef, const CUnit* builder, const BuildInfo* buildInfo)
{
	RECOIL_DETAILED_TRACY_ZONE;

	bool allow = true;
	bool drop = true;

	for (size_t i = 0; i < listAllowUnitCreation.size(); ) {
		const auto ec = listAllowUnitCreation[i];
		const auto [a, d] = ec->AllowUnitCreation(unitDef, builder, buildInfo);
		allow &= a;
		drop  &= d;

		// the call-in may remove itself from the list
		i += (i < listAllowUnitCreation.size() && ec == listAllowUnitCreation[i]);
	}

	return {allow, drop};
}

bool CEventHandler::AllowUnitTransfer(const CUnit* unit, int newTeam, bool capture)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitTransfer, &CEventClient::AllowUnitTransfer, unit, newTeam, capture);
}

bool CEventHandler::AllowUnitBuildStep(const CUnit* builder, const CUnit* unit, float part)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitBuildStep, &CEventClient::AllowUnitBuildStep, builder, unit, part);
}

bool CEventHandler::AllowUnitCaptureStep(const CUnit* builder, const CUnit* unit, float part)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitCaptureStep, &CEventClient::AllowUnitCaptureStep, builder, unit, part);
}

bool CEventHandler::AllowUnitTransport(const CUnit* transporter, const CUnit* transportee)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitTransport, &CEventClient::AllowUnitTransport, transporter, transportee);
}

bool CEventHandler::AllowUnitTransportLoad(const CUnit* transporter, const CUnit* transportee, const float3& loadPos, bool allowed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitTransportLoad, &CEventClient::AllowUnitTransportLoad, transporter, transportee, loadPos, allowed);
}

bool CEventHandler::AllowUnitTransportUnload(const CUnit* transporter, const CUnit* transportee, const float3& unloadPos, bool allowed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitTransportUnload, &CEventClient::AllowUnitTransportUnload, transporter, transportee, unloadPos, allowed);
}

bool CEventHandler::AllowUnitCloak(const CUnit* unit, const CUnit* enemy)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitCloak, &CEventClient::AllowUnitCloak, unit, enemy);
}

bool CEventHandler::AllowUnitDecloak(const CUnit* unit, const CSolidObject* object, const CWeapon* weapon)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitDecloak, &CEventClient::AllowUnitDecloak, unit, object, weapon);
}

bool CEventHandler::AllowUnitKamikaze(const CUnit* unit, const CUnit* target, bool allowed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowUnitKamikaze, &CEventClient::AllowUnitKamikaze, unit, target, allowed);
}


bool CEventHandler::AllowFeatureCreation(const FeatureDef* featureDef, int allyTeamID, const float3& pos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowFeatureCreation, &CEventClient::AllowFeatureCreation, featureDef, allyTeamID, pos);
}


bool CEventHandler::AllowFeatureBuildStep(const CUnit* builder, const CFeature* feature, float part)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowFeatureBuildStep, &CEventClient::AllowFeatureBuildStep, builder, feature, part);
}


bool CEventHandler::AllowResourceLevel(int teamID, const std::string& type, float level)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowResourceLevel, &CEventClient::AllowResourceLevel, teamID, type, level);
}


bool CEventHandler::AllowResourceTransfer(int oldTeam, int newTeam, const char* type, float amount)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowResourceTransfer, &CEventClient::AllowResourceTransfer, oldTeam, newTeam, type, amount);
}


bool CEventHandler::AllowDirectUnitControl(int playerID, const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowDirectUnitControl, &CEventClient::AllowDirectUnitControl, playerID, unit);
}


bool CEventHandler::AllowBuilderHoldFire(const CUnit* unit, int action)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowBuilderHoldFire, &CEventClient::AllowBuilderHoldFire, unit, action);
}


bool CEventHandler::AllowStartPosition(int playerID, int teamID, unsigned char readyState, const float3& clampedPos, const float3& rawPickPos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowStartPosition, &CEventClient::AllowStartPosition, playerID, teamID, readyState, clampedPos, rawPickPos);
}



bool CEventHandler::TerraformComplete(const CUnit* unit, const CUnit* build)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefFalse(listTerraformComplete, &CEventClient::TerraformComplete, unit, build);
}


bool CEventHandler::MoveCtrlNotify(const CUnit* unit, int data)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefFalse(listMoveCtrlNotify, &CEventClient::MoveCtrlNotify, unit, data);
}


int CEventHandler::AllowWeaponTargetCheck(unsigned int attackerID, unsigned int attackerWeaponNum, unsigned int attackerWeaponDefID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	int result = -1;

	for (size_t i = 0; i < listAllowWeaponTargetCheck.size(); ) {
		CEventClient* ec = listAllowWeaponTargetCheck[i];

		result = std::max(result, ec->AllowWeaponTargetCheck(attackerID, attackerWeaponNum, attackerWeaponDefID));

		// the call-in may remove itself from the list
		i += (i < listAllowWeaponTargetCheck.size() && ec == listAllowWeaponTargetCheck[i]);
	}

	return result;
}


bool CEventHandler::AllowWeaponTarget(
	unsigned int attackerID,
	unsigned int targetID,
	unsigned int attackerWeaponNum,
	unsigned int attackerWeaponDefID,
	float* targetPriority
) {
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowWeaponTarget, &CEventClient::AllowWeaponTarget, attackerID, targetID, attackerWeaponNum, attackerWeaponDefID, targetPriority);
}

bool CEventHandler::AllowWeaponInterceptTarget(const CUnit* interceptorUnit, const CWeapon* interceptorWeapon, const CProjectile* interceptorTarget)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefTrue(listAllowWeaponInterceptTarget, &CEventClient::AllowWeaponInterceptTarget, interceptorUnit, interceptorWeapon, interceptorTarget);
}


bool CEventHandler::UnitPreDamaged(
	const CUnit* unit,
	const CUnit* attacker,
	float damage,
	int weaponDefID,
	int projectileID,
	bool paralyzer,
	float* newDamage,
	float* impulseMult
) {
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefFalse(listUnitPreDamaged, &CEventClient::UnitPreDamaged, unit, attacker, damage, weaponDefID, projectileID, paralyzer, newDamage, impulseMult);
}


bool CEventHandler::FeaturePreDamaged(
	const CFeature* feature,
	const CUnit* attacker,
	float damage,
	int weaponDefID,
	int projectileID,
	float* newDamage,
	float* impulseMult
) {
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefFalse(listFeaturePreDamaged, &CEventClient::FeaturePreDamaged, feature, attacker, damage, weaponDefID, projectileID, newDamage, impulseMult);
}


bool CEventHandler::ShieldPreDamaged(
	const CProjectile* projectile,
	const CWeapon* shieldEmitter,
	const CUnit* shieldCarrier,
	bool bounceProjectile,
	const CWeapon* beamEmitter,
	const CUnit* beamCarrier,
	const float3& startPos,
	const float3& hitPos
) {
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlIterateDefFalse(listShieldPreDamaged, &CEventClient::ShieldPreDamaged, projectile, shieldEmitter, shieldCarrier, bounceProjectile, beamEmitter, beamCarrier, startPos, hitPos);
}


bool CEventHandler::SyncedActionFallback(const std::string& line, int playerID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (size_t i = 0; i < listSyncedActionFallback.size(); ) {
		CEventClient* ec = listSyncedActionFallback[i];

		if (ec->SyncedActionFallback(line, playerID))
			return true;

		// the call-in may remove itself from the list
		i += (i < listSyncedActionFallback.size() && ec == listSyncedActionFallback[i]);
	}

	return false;
}


/******************************************************************************/
/******************************************************************************/

template<typename T, typename F, typename... A> void IterateEventClientList(T& list, const F& func, A&&... args) {
	for (size_t i = 0; i < list.size(); ) {
		CEventClient* ec = list[i];

		// PR 27b: sim-fired dispatch to an unsynced client defers to the
		// SimDrawBarrier (args copied by value into the closure); a single
		// cached-bool load when the split flag is off
		if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
			UnsyncedBoundaryQueue::DeferFor(ec, std::bind(func, ec, args...));
		} else {
			(ec->*func)(std::forward<A>(args)...);
		}

		// the call-in may remove itself from the list
		i += (i < list.size() && ec == list[i]);
	}
}

// not usable: "pasting "::" and "Save" does not give a valid preprocessing token"
// #define ITERATE_EVENTCLIENTLIST(func, ...) IterateEventClientList(list ## func, &CEventClient:: ## func, __VA_ARGS__)
#define ITERATE_EVENTCLIENTLIST_NA(func) IterateEventClientList(list ## func, &CEventClient::func)
#define ITERATE_EVENTCLIENTLIST(func, ...) IterateEventClientList(list ## func, &CEventClient::func, __VA_ARGS__)


void CEventHandler::Save(zipFile archive)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(Save, archive);
}

void CEventHandler::Load(IArchive* archive)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(Load, archive);
}


void CEventHandler::GamePreload()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(GamePreload);
}

void CEventHandler::GameStart()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(GameStart);
}

void CEventHandler::GameOver(const std::vector<unsigned char>& winningAllyTeams)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GameOver, winningAllyTeams);
}

void CEventHandler::GamePaused(int playerID, bool paused)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GamePaused, playerID, paused);
}

void CEventHandler::GameFrame(int gameFrame)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GameFrame, gameFrame);
}

void CEventHandler::GameFramePost(int gameFrame)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GameFramePost, gameFrame);
}

void CEventHandler::GameProgress(int gameFrame)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GameProgress, gameFrame);
}

void CEventHandler::GameID(const unsigned char* gameID, unsigned int numBytes)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// deferral would capture a dangling buffer pointer; copy it (fires once
	// per game, so the shared_ptr detour is free in practice)
	if (SimDrawSplit::DeferUnsyncedNow()) {
		const auto idCopy = std::make_shared<std::vector<unsigned char>>(gameID, gameID + numBytes);

		for (size_t i = 0; i < listGameID.size(); ) {
			CEventClient* ec = listGameID[i];

			if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
				UnsyncedBoundaryQueue::DeferFor(ec, [ec, idCopy, numBytes]() { ec->GameID(idCopy->data(), numBytes); });
			} else {
				ec->GameID(gameID, numBytes);
			}

			i += (i < listGameID.size() && ec == listGameID[i]);
		}

		return;
	}

	ITERATE_EVENTCLIENTLIST(GameID, gameID, numBytes);
}


/* PR 38f -- event-time presentation. UnitCommand / UnitCmdDone / UnitLeftLos are
 * synced events dispatched to unsynced handlers; under the split those handlers
 * defer to the SimDrawBarrier, where the published boundary state no longer
 * matches what master's synchronous, mid-sim dispatch saw. These three
 * dispatchers capture the event-time state at FIRE time and present it as a
 * per-target override around each deferred handler call (cleared right after),
 * then drop the override. Flag-off / immediate-dispatch paths take no capture
 * and install no override -> byte-identical (the GameID dispatcher above is the
 * out-of-line precedent). The overrides are UNSYNCED (draw-only): no synced
 * write, no sync-hash impact, no gsRNG. */

void CEventHandler::UnitCommand(const CUnit* unit, const Command& command, int playerNum, bool fromSynced, bool fromLua)
{
	ZoneScoped;
	// event-time command queue for <unit> (nullptr / no-op when the split is off
	// or this dispatch is not deferring); shared across the receiving clients
	const std::shared_ptr<void> evtQueue = LuaSnapshotServe::CaptureCmdQueueEvent(unit);
	const int evtUnitID = unit->id;

	const auto unitAllyTeam = unit->allyteam;
	for (size_t i = 0; i < listUnitCommand.size(); ) {
		CEventClient* ec = listUnitCommand[i];

		if (ec->CanReadAllyTeam(unitAllyTeam)) {
			if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
				// std::bind copies Command by value exactly as the generic
				// EVENTCLIENT_DISPATCH macro did; the outer closure installs the
				// event-time queue override for the single handler call
				auto boundFn = std::bind(&CEventClient::UnitCommand, ec, unit, command, playerNum, fromSynced, fromLua);
				UnsyncedBoundaryQueue::DeferFor(ec, [evtUnitID, evtQueue, boundFn = std::move(boundFn)]() {
					// PR 38j: install the event-time queue override for the
					// duration of this handler call. The deferred handler runs
					// under the barrier's live exception (no blanket reassert --
					// 38h's over-reach reverted); the command-queue callouts
					// (GetUnitCommands/CommandCount/CurrentCommand) consult this
					// override at their TOP, independent of the live exception, so
					// only they see the event-time queue and every other read the
					// handler makes stays on the live path.
					LuaSnapshotServe::ScopedCmdQueueEventOverride ov(evtUnitID, evtQueue);
					boundFn();
				});
			} else {
				ec->UnitCommand(unit, command, playerNum, fromSynced, fromLua);
			}
		}

		// the call-in may remove itself from the list
		i += (i < listUnitCommand.size() && ec == listUnitCommand[i]);
	}
}

void CEventHandler::UnitCmdDone(const CUnit* unit, const Command& command)
{
	ZoneScoped;
	const std::shared_ptr<void> evtQueue = LuaSnapshotServe::CaptureCmdQueueEvent(unit);
	const int evtUnitID = unit->id;

	const auto unitAllyTeam = unit->allyteam;
	for (size_t i = 0; i < listUnitCmdDone.size(); ) {
		CEventClient* ec = listUnitCmdDone[i];

		if (ec->CanReadAllyTeam(unitAllyTeam)) {
			if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
				auto boundFn = std::bind(&CEventClient::UnitCmdDone, ec, unit, command);
				UnsyncedBoundaryQueue::DeferFor(ec, [evtUnitID, evtQueue, boundFn = std::move(boundFn)]() {
					// PR 38j: event-time queue override only; the callouts consult
					// it at their top (no blanket reassert -- 38h reverted)
					LuaSnapshotServe::ScopedCmdQueueEventOverride ov(evtUnitID, evtQueue);
					boundFn();
				});
			} else {
				ec->UnitCmdDone(unit, command);
			}
		}

		i += (i < listUnitCmdDone.size() && ec == listUnitCmdDone[i]);
	}
}

void CEventHandler::UnitLeftLos(const CUnit* unit, int at)
{
	ZoneScoped;
	// LOSCAPTURE loop (fire-time capture clients dispatch immediately, everyone
	// else defers), with amendment (b) as refined by PR 38g: a deferred handler
	// runs after the boundary published the END-of-frame LOS state, past what
	// master's synchronous mid-sim handler saw. Capture the EXACT at-dispatch
	// losStatus byte here (sim thread owns the unit; SetLosStatus cleared
	// LOS_INLOS before firing, and the radar bit -- if also leaving this same
	// call -- clears only in a later block, so it is still set) and present it as
	// the residual visibility for allyteam <at> around the deferred dispatch.
	const int evtUnitID = unit->id;
	const uint8_t evtLosStatus = unit->losStatus[at];

	for (size_t i = 0; i < listUnitLeftLos.size(); ) {
		CEventClient* ec = listUnitLeftLos[i];

		if (ec->CanReadAllyTeam(at)) {
			if (ec->IsSimPhaseCaptureClient()) {
				ec->UnitLeftLos(unit, at);
			} else if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
				UnsyncedBoundaryQueue::DeferFor(ec, [ec, unit, evtUnitID, at, evtLosStatus]() {
					// PR 38j: install the residual-LOS override; GetUnitPosition/
					// GetUnitDirection consult it at their top (no blanket reassert
					// -- 38h reverted), so only those position reads see the
					// residual visibility and the handler's other reads stay live
					SimSnapshotLosEvent::ScopedVisibility ov(evtUnitID, at, evtLosStatus);
					ec->UnitLeftLos(unit, at);
				});
			} else {
				ec->UnitLeftLos(unit, at);
			}
		}

		// the call-in may remove itself from the list
		i += (i < listUnitLeftLos.size() && ec == listUnitLeftLos[i]);
	}
}


void CEventHandler::TeamDied(int teamID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(TeamDied, teamID);
}

void CEventHandler::TeamChanged(int teamID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(TeamChanged, teamID);
}


void CEventHandler::PlayerChanged(int playerID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(PlayerChanged, playerID);
}

void CEventHandler::PlayerAdded(int playerID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(PlayerAdded, playerID);
}

void CEventHandler::PlayerRemoved(int playerID, int reason)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(PlayerRemoved, playerID, reason);
}


/******************************************************************************/
/******************************************************************************/

void CEventHandler::UnitHarvestStorageFull(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const int unitAllyTeam = unit->allyteam;
	const int count = listUnitHarvestStorageFull.size();
	for (int i = 0; i < count; i++) {
		CEventClient* ec = listUnitHarvestStorageFull[i];
		if (ec->CanReadAllyTeam(unitAllyTeam)) {
			ec->UnitHarvestStorageFull(unit);
		}
	}
}

/******************************************************************************/
/******************************************************************************/

void CEventHandler::CollectGarbage(bool forced, CEventHandler::GCFilter filter)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// PR 27b: under the split, sim-side GC covers the synced lua_States only
	// (they belong to the sim thread) and the main-thread timed job covers
	// the unsynced ones -- callers pass a filter when the flag is on.
	// GC_ALL keeps the exact legacy iteration (flag-off path).
	if (filter == GC_ALL) {
		ITERATE_EVENTCLIENTLIST(CollectGarbage, forced);
		return;
	}

	for (size_t i = 0; i < listCollectGarbage.size(); ) {
		CEventClient* ec = listCollectGarbage[i];

		if (ec->GetSynced() == (filter == GC_SYNCED_ONLY))
			ec->CollectGarbage(forced);

		i += (i < listCollectGarbage.size() && ec == listCollectGarbage[i]);
	}
}


void CEventHandler::StockpileChanged(const CUnit* unit, const CWeapon* weapon, int oldCount)
{
	const auto unitAllyTeam = unit->allyteam;

	for (size_t i = 0; i < listStockpileChanged.size(); ) {
		CEventClient* ec = listStockpileChanged[i];

		if (ec->CanReadAllyTeam(unitAllyTeam)) {
			if (UnsyncedBoundaryQueue::ShouldDefer(ec)) {
				// the CWeapon* is not lifetime-protected by the deferred-
				// deletion epoch (PreDestruct frees the weapons), so drop the
				// dispatch if the unit died before the boundary: master fired
				// it pre-death, the widget just misses one final tick.
				// PR 44b §3.8: the liveness re-check reads draw-owned state
				// (see SimDrawSplit.h) -- it must also catch died-AFTER-the-
				// epoch-edge units, whose weapons are already freed.
				const int unitID = unit->id;
				UnsyncedBoundaryQueue::DeferFor(ec, [ec, unit, weapon, oldCount, unitID]() {
					if (SimDrawSplit::BoundaryUnitAliveAtDrain(unitID, unit))
						ec->StockpileChanged(unit, weapon, oldCount);
				});
			} else {
				ec->StockpileChanged(unit, weapon, oldCount);
			}
		}

		i += (i < listStockpileChanged.size() && ec == listStockpileChanged[i]);
	}
}

void CEventHandler::DbgTimingInfo(DbgTimingInfoType type, const spring_time start, const spring_time end)
{
	// thread-attribute GC slices at emit time: the dispatch to unsynced
	// clients may be deferred to the main thread (sim-fired -> boundary),
	// where the sim-phase TLS bracket reads false again
	if (type == TIMING_GC && SimDrawSplit::InSimPhase())
		type = TIMING_GC_SIM;

	ITERATE_EVENTCLIENTLIST(DbgTimingInfo, type, start, end);
}

void CEventHandler::Pong(uint8_t pingTag, const spring_time pktSendTime, const spring_time pktRecvTime)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(Pong, pingTag, pktSendTime, pktRecvTime);
}


void CEventHandler::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(Update);
}



void CEventHandler::SunChanged()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(SunChanged);
}

void CEventHandler::ViewResize()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(ViewResize);
}


#define DRAW_CALLIN(name)                                                   \
	void CEventHandler:: Draw ## name ()                                    \
	{                                                                       \
		ZoneScoped;                                                         \
		if (listDraw ## name.empty())                                       \
			return;                                                         \
                                                                            \
		LuaOpenGL::EnableDraw ## name ();                                   \
		listDraw ## name [0]->Draw ## name ();                              \
                                                                            \
		for (size_t i = 1; i < listDraw ## name.size(); ) {                 \
			LuaOpenGL::ResetDraw ## name ();                                \
			CEventClient* ec = listDraw ## name [i];                        \
			ec-> Draw ## name ();                                           \
                                                                            \
			if (i < listDraw ## name.size() && ec == listDraw ## name [i])  \
				++i;                                                        \
		}                                                                   \
                                                                            \
		LuaOpenGL::DisableDraw ## name ();                                  \
  }


DRAW_CALLIN(Genesis)
DRAW_CALLIN(World)
DRAW_CALLIN(WorldPreUnit)
DRAW_CALLIN(PreDecals)
DRAW_CALLIN(WaterPost)
DRAW_CALLIN(WorldShadow)
DRAW_CALLIN(ShadowPassTransparent)
DRAW_CALLIN(WorldReflection)
DRAW_CALLIN(WorldRefraction)
DRAW_CALLIN(GroundPreForward)
DRAW_CALLIN(GroundPostForward)
DRAW_CALLIN(GroundPreDeferred)
DRAW_CALLIN(GroundDeferred)
DRAW_CALLIN(GroundPostDeferred)
DRAW_CALLIN(UnitsPostDeferred)
DRAW_CALLIN(FeaturesPostDeferred)
DRAW_CALLIN(ScreenEffects)
DRAW_CALLIN(ScreenPost)
DRAW_CALLIN(Screen)
DRAW_CALLIN(InMiniMap)
DRAW_CALLIN(InMiniMapBackground)


#define DRAW_ENTITY_CALLIN(name, args, args2)                                 \
	bool CEventHandler:: Draw ## name args                                    \
	{                                                                         \
		ZoneScoped;                                                           \
		bool skipEngineDrawing = false;                                       \
                                                                              \
		for (size_t i = 0; i < listDraw ## name.size(); ) {                   \
			CEventClient* ec = listDraw ## name [i];                          \
			skipEngineDrawing |= ec-> Draw ## name args2 ;                    \
                                                                              \
			i += (i < listDraw ## name.size() && ec == listDraw ## name [i]); \
		}                                                                     \
                                                                              \
		return skipEngineDrawing;                                             \
  }


DRAW_ENTITY_CALLIN(Unit, (const CUnit* unit), (unit))
DRAW_ENTITY_CALLIN(Feature, (const CFeature* feature), (feature))
DRAW_ENTITY_CALLIN(Shield, (const CUnit* unit, const CWeapon* weapon), (unit, weapon))
DRAW_ENTITY_CALLIN(Projectile, (const CProjectile* projectile), (projectile))
DRAW_ENTITY_CALLIN(Material, (const LuaMaterial* material), (material))

/******************************************************************************/
/******************************************************************************/

template<typename T, typename F, typename... A> bool ControlReverseIterateDefTrue(T& list, const F& func, A&&... args) {
	for (size_t i = 0; i < list.size(); i++) {
		CEventClient* ec = list[list.size() - 1 - i];

		if ((ec->*func)(std::forward<A>(args)...))
			return true;
	}

	return false;
}

template<typename T, typename F, typename... A> std::string ControlReverseIterateDefString(T& list, const F& func, A&&... args) {
	for (size_t i = 0; i < list.size(); i++) {
		CEventClient* ec = list[list.size() - 1 - i];

		std::string str = (ec->*func)(std::forward<A>(args)...);

		if (str.empty())
			continue;

		return str;
	}

	return {};
}

void CEventHandler::ActiveCommandChanged(const SCommandDescription* cmdDesc)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(ActiveCommandChanged, cmdDesc);
}

void CEventHandler::CameraRotationChanged(const float3& rot)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(CameraRotationChanged, rot);
}

void CEventHandler::CameraPositionChanged(const float3& pos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(CameraPositionChanged, pos);
}

void CEventHandler::MiniMapRotationChanged(const float newRot, const float oldRot)
{
	ZoneScoped;
	ITERATE_EVENTCLIENTLIST(MiniMapRotationChanged, newRot, oldRot);
}

void CEventHandler::MiniMapStateChanged(const bool isMinimized, const bool isMaximized, const bool isSlaved)
{
	ZoneScoped;
	ITERATE_EVENTCLIENTLIST(MiniMapStateChanged, isMinimized, isMaximized, isSlaved);
}

void CEventHandler::MiniMapGeometryChanged(const int2 newPos, const int2 newDim, const int2 oldPos, const int2 oldDim)
{
	ZoneScoped;
	ITERATE_EVENTCLIENTLIST(MiniMapGeometryChanged, newPos, newDim, oldPos, oldDim);
}

bool CEventHandler::CommandNotify(const Command& cmd)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listCommandNotify, &CEventClient::CommandNotify, cmd);
}

bool CEventHandler::KeyMapChanged()
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listKeyMapChanged, &CEventClient::KeyMapChanged);
}

bool CEventHandler::KeyPress(int keyCode, int scanCode, bool isRepeat)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listKeyPress, &CEventClient::KeyPress, keyCode, scanCode, isRepeat);
}

bool CEventHandler::KeyRelease(int keyCode, int scanCode)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listKeyRelease, &CEventClient::KeyRelease, keyCode, scanCode);
}


bool CEventHandler::TextInput(const std::string& utf8)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listTextInput, &CEventClient::TextInput, utf8);
}

bool CEventHandler::TextEditing(const std::string& utf8, unsigned int start, unsigned int length)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listTextEditing, &CEventClient::TextEditing, utf8, start, length);
}


bool CEventHandler::MousePress(int x, int y, int button)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (size_t i = 0; i < listMousePress.size(); i++) {
		CEventClient* ec = listMousePress[listMousePress.size() - 1 - i];

		if (ec->MousePress(x, y, button)) {
			if (mouseOwner == nullptr)
				mouseOwner = ec;

			return true;
		}
	}

	return false;
}

void CEventHandler::MouseRelease(int x, int y, int button)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (mouseOwner == nullptr)
		return;

	mouseOwner->MouseRelease(x, y, button);
	mouseOwner = nullptr;
}


bool CEventHandler::MouseMove(int x, int y, int dx, int dy, int button)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (mouseOwner == nullptr)
		return false;

	return mouseOwner->MouseMove(x, y, dx, dy, button);
}

bool CEventHandler::MouseWheel(bool up, float value)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listMouseWheel, &CEventClient::MouseWheel, up, value);
}


bool CEventHandler::IsAbove(int x, int y)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listIsAbove, &CEventClient::IsAbove, x, y);
}


std::string CEventHandler::GetTooltip(int x, int y)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefString(listGetTooltip, &CEventClient::GetTooltip, x, y);
}

std::string CEventHandler::WorldTooltip(const CUnit* unit, const CFeature* feature, const float3* groundPos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefString(listWorldTooltip, &CEventClient::WorldTooltip, unit, feature, groundPos);
}


bool CEventHandler::AddConsoleLine(const std::string& msg, const std::string& section, int level)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (listAddConsoleLine.empty())
		return false;

	ITERATE_EVENTCLIENTLIST(AddConsoleLine, msg, section, level);
	return true;
}


void CEventHandler::LastMessagePosition(const float3& pos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(LastMessagePosition, pos);
}


bool CEventHandler::GroupChanged(int groupID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(GroupChanged, groupID);
	return false;
}



bool CEventHandler::GameSetup(
	const std::string& state,
	bool& ready,
	const std::vector< std::pair<int, std::string> >& playerStates
) {
	RECOIL_DETAILED_TRACY_ZONE;
	return ControlReverseIterateDefTrue(listGameSetup, &CEventClient::GameSetup, state, ready, playerStates);
}


void CEventHandler::DownloadQueued(int ID, const std::string& archiveName, const std::string& archiveType)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DownloadQueued, ID, archiveName, archiveType);
}

void CEventHandler::DownloadStarted(int ID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DownloadStarted, ID);
}

void CEventHandler::DownloadFinished(int ID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DownloadFinished, ID);
}

void CEventHandler::DownloadFailed(int ID, int errorID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DownloadFailed, ID, errorID);
}

void CEventHandler::DownloadProgress(int ID, long downloaded, long total)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DownloadProgress, ID, downloaded, total);
}


bool CEventHandler::MapDrawCmd(
	int playerID,
	int type,
	const float3* pos0,
	const float3* pos1,
	const std::string* label
) {
	return ControlReverseIterateDefTrue(listMapDrawCmd, &CEventClient::MapDrawCmd, playerID, type, pos0, pos1, label);
}


/******************************************************************************/
/******************************************************************************/

void CEventHandler::MetalMapChanged(const int x, const int z)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(MetalMapChanged, x, z);
}

void CEventHandler::DrawWorldPreParticles(bool drawAboveWater, bool drawBelowWater, bool drawReflection, bool drawRefraction)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DrawWorldPreParticles, drawAboveWater, drawBelowWater, drawReflection, drawRefraction);
}

void CEventHandler::DrawOpaqueUnitsLua(bool deferredPass, bool drawReflection, bool drawRefraction)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DrawOpaqueUnitsLua, deferredPass, drawReflection, drawRefraction);
}

void CEventHandler::DrawOpaqueFeaturesLua(bool deferredPass, bool drawReflection, bool drawRefraction)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DrawOpaqueFeaturesLua, deferredPass, drawReflection, drawRefraction);
}

void CEventHandler::DrawAlphaUnitsLua(bool drawReflection, bool drawRefraction)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DrawAlphaUnitsLua, drawReflection, drawRefraction);
}

void CEventHandler::DrawAlphaFeaturesLua(bool drawReflection, bool drawRefraction)
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST(DrawAlphaFeaturesLua, drawReflection, drawRefraction);
}

void CEventHandler::DrawShadowUnitsLua()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(DrawShadowUnitsLua);
}

void CEventHandler::DrawShadowFeaturesLua()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(DrawShadowFeaturesLua);
}

/******************************************************************************/
/******************************************************************************/

void CEventHandler::FontsChanged()
{
	RECOIL_DETAILED_TRACY_ZONE;
	ITERATE_EVENTCLIENTLIST_NA(FontsChanged);
}

/******************************************************************************/
