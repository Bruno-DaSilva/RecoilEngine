/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <functional> // PR 44b: ForEachServedCommand's scan callback
#include <memory> // PR 38f: CaptureCmdQueueEvent's opaque shared_ptr<void>
#include <vector> // sim|draw PR 44: GetServedAvailableCommands out-vector

struct lua_State;
class CUnit; // sim|draw PR 30: CompareCmdQueueSlot's live-unit argument
struct SCommandDescription; // sim|draw PR 44: GetServedAvailableCommands out-vector

/**
 * @brief LuaSnapshotServe -- snapshot-backed serving twins for draw-context Lua callouts
 *
 * PR 18 of the sim|draw decoupling plan (doc/sim-draw-thread-decoupling-research.md
 * §E / §E.1b): the hot sim-state callout families are redirected, per handle
 * context, to read the published SimSnapshot instead of live sim objects when
 * invoked from a Draw* callin. This file owns the redirection pattern every
 * later family copies:
 *
 *  - ShouldServe(L) is the redirect predicate: an *unsynced* handle context
 *    (CLuaHandle::GetHandleSynced) currently inside a Draw* callin (the PR-2
 *    ScopedDrawCallinContext TLS bracket). Sim-phase and other-context calls
 *    keep the live path -- the same callout name serves both sides (census
 *    E.1: GetGameFrame is 39% sim-phase), so the branch must be per call, not
 *    per registered function.
 *
 *  - Each redirected callout gets a serving twin here: a line-by-line mirror
 *    of the live body with every sim-object read replaced by a SimSnapshot
 *    row read (and drawer-owned reads kept pointer-free via id-keyed drawer
 *    accessors). Signatures, argument parsing, error messages, return counts
 *    and float expression order are IDENTICAL by construction; UnitDef
 *    derefs (immutable game data) stay direct. Visibility gates and
 *    errorVector masking run against the snapshot's POV-complete masking
 *    inputs via the SimSnapshot::UnitRows Pov predicates and ErrorVector
 *    helpers -- see the masking-policy block in SimSnapshot.h.
 *
 *  - Uncovered ids (dead, out of range, spawned after the last boundary --
 *    impossible mid-draw in the single-threaded tree) return the same nil
 *    shape as the live path's "no such unit", deterministically; there is NO
 *    fallback read of live sim state from a served path. Divergence between
 *    the snapshot's valid set and the live one is a contract violation whose
 *    detector is the armed SnapshotDiffGate, not a per-miss warning (widgets
 *    legitimately query dead ids constantly).
 *
 *  - Route() wires the family through the diff gate: unarmed it simply picks
 *    live or twin; armed (test runs) it executes BOTH real paths, bit-compares
 *    the actual Lua return slots (masking included), reports through the
 *    gate's counters, and returns the snapshot-served values. No third copy
 *    of any formula exists in the verifier.
 */
namespace LuaSnapshotServe {
	// serving-twin signature: mirrors the live bodies' (lua_State, caller) shape
	using ServeFn = int (*)(lua_State* L, const char* caller);

	/// the redirect predicate (see above); cheap enough for every callout entry
	bool ShouldServe(lua_State* L);

	/// entry-point glue: live path when not draw-context, twin when it is,
	/// dual-run + compare + serve-twin when the SnapshotDiffGate is armed.
	/// `caller` doubles as the gate's per-callout counter name.
	int Route(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn);

	/// PR 47: Route variant for the piece-slot-cache-backed twins. The piece
	/// caches are captured only when the split contract is enabled or the
	/// diff gate is armed (EnsurePieceCacheCaptured's flag-off skip), but
	/// Route's flag-off draw-callin rehearsal still picked the twin -- which
	/// then found an empty cache and returned NIL where the base branch
	/// returned real values (probe-found: flag-off DrawGenesis
	/// GetUnitPieceMap == nil while GameFrame == 40-piece table). Serve the
	/// LIVE leg whenever the capture is skipped; identical predicate, so twin
	/// and capture can never disagree again. Flag-on behavior unchanged.
	int RoutePieceCache(lua_State* L, const char* caller, ServeFn liveFn, ServeFn snapFn);

	// serving twins (positions/status family, E.1b order: first family)
	int GetUnitPosition(lua_State* L, const char* caller);      // also GetUnitBasePosition
	int GetUnitHealth(lua_State* L, const char* caller);
	int GetUnitIsStunned(lua_State* L, const char* caller);
	int GetUnitViewPosition(lua_State* L, const char* caller);  // LuaUnsyncedRead

	// unit-family stragglers surfaced by the E.1b census (LosState is the
	// non-spectating hot one; Visible/Icon read drawer + camera + snapshot)
	int GetUnitLosState(lua_State* L, const char* caller);
	int IsUnitVisible(lua_State* L, const char* caller);        // LuaUnsyncedRead
	int IsUnitIcon(lua_State* L, const char* caller);           // LuaUnsyncedRead

	// projectile family (E.1b order: second family)
	int GetProjectilePosition(lua_State* L, const char* caller);
	int GetProjectileVelocity(lua_State* L, const char* caller);
	int GetProjectileDefID(lua_State* L, const char* caller);
	int GetProjectileTarget(lua_State* L, const char* caller);
	int GetProjectileOwnerID(lua_State* L, const char* caller);

	// team/player-table family (PR 26; served from the SimSnapshot team/player
	// boundary copy, section E.3 field spec -- POV gates via the TeamRows
	// IsAlliedTeam mirror)
	// global-scalar family (PR 27a; served from SimSnapshot::GlobalRows --
	// the no-object callouts hot at draw time: game clock, speed, wind,
	// cheat flags, ground extremes, global LOS)
	int GetGameFrame(lua_State* L, const char* caller);
	int GetGameSeconds(lua_State* L, const char* caller);
	int GetGameSecondsInterpolated(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetGameSpeed(lua_State* L, const char* caller);               // LuaUnsyncedRead
	int GetWind(lua_State* L, const char* caller);
	int IsCheatingEnabled(lua_State* L, const char* caller);
	int IsGodModeEnabled(lua_State* L, const char* caller);
	int IsEditDefsEnabled(lua_State* L, const char* caller);
	int AreHelperAIsEnabled(lua_State* L, const char* caller);
	int IsNoCostEnabled(lua_State* L, const char* caller);
	int IsGameOver(lua_State* L, const char* caller);
	int GetGroundExtremes(lua_State* L, const char* caller);
	int GetGlobalLos(lua_State* L, const char* caller);

	// PR 27a row-backed tail: the remaining per-object callouts whose live
	// reads are fully covered by the PR-27a SimSnapshot rows (unit status/eco
	// scalars, the feature tail, the projectile tail). Same §E.2 recipe as the
	// families above; parse-gate mirrors follow each live body's parse helper.
	int ValidUnitID(lua_State* L, const char* caller);
	int GetUnitDefID(lua_State* L, const char* caller);
	int GetUnitTeam(lua_State* L, const char* caller);
	int GetUnitAllyTeam(lua_State* L, const char* caller);
	int GetUnitNeutral(lua_State* L, const char* caller);
	int GetUnitIsDead(lua_State* L, const char* caller);
	int GetUnitIsBeingBuilt(lua_State* L, const char* caller);
	int GetUnitVelocity(lua_State* L, const char* caller);
	int GetUnitDirection(lua_State* L, const char* caller);
	int GetUnitHeading(lua_State* L, const char* caller);
	int GetUnitVectors(lua_State* L, const char* caller);
	int GetUnitRadius(lua_State* L, const char* caller);
	int GetUnitHeight(lua_State* L, const char* caller);
	int GetUnitMass(lua_State* L, const char* caller);
	int GetUnitExperience(lua_State* L, const char* caller);
	int GetUnitIsActive(lua_State* L, const char* caller);
	int GetUnitIsCloaked(lua_State* L, const char* caller);
	int GetUnitMaxRange(lua_State* L, const char* caller);
	int GetUnitBuildFacing(lua_State* L, const char* caller);
	int GetUnitSensorRadius(lua_State* L, const char* caller);
	int GetUnitSeismicSignature(lua_State* L, const char* caller);
	int GetUnitSelfDTime(lua_State* L, const char* caller);
	int GetUnitArmored(lua_State* L, const char* caller);
	int GetUnitResources(lua_State* L, const char* caller);
	int GetUnitHarvestStorage(lua_State* L, const char* caller);
	int GetUnitCosts(lua_State* L, const char* caller);
	int GetUnitCostTable(lua_State* L, const char* caller);
	int GetUnitMoveDefID(lua_State* L, const char* caller);
	int GetUnitBlocking(lua_State* L, const char* caller);
	int GetUnitLeavesGhost(lua_State* L, const char* caller);
	int GetUnitSeparation(lua_State* L, const char* caller);
	int GetUnitFeatureSeparation(lua_State* L, const char* caller);
	int IsUnitInRadar(lua_State* L, const char* caller); // inRadarAll IS the live InRadar(unit, at) answer
	int IsUnitAllied(lua_State* L, const char* caller);  // LuaUnsyncedRead

	int ValidFeatureID(lua_State* L, const char* caller);
	int GetFeatureDefID(lua_State* L, const char* caller);
	int GetFeatureTeam(lua_State* L, const char* caller);
	int GetFeatureAllyTeam(lua_State* L, const char* caller);
	int GetFeatureHealth(lua_State* L, const char* caller);
	int GetFeatureHeight(lua_State* L, const char* caller);
	int GetFeatureRadius(lua_State* L, const char* caller);
	int GetFeaturePosition(lua_State* L, const char* caller);
	int GetFeatureMass(lua_State* L, const char* caller);
	int GetFeatureDirection(lua_State* L, const char* caller);
	int GetFeatureVelocity(lua_State* L, const char* caller);
	int GetFeatureHeading(lua_State* L, const char* caller);
	int GetFeatureResources(lua_State* L, const char* caller);
	int GetFeatureBlocking(lua_State* L, const char* caller);
	int GetFeatureNoSelect(lua_State* L, const char* caller);
	int GetFeatureResurrect(lua_State* L, const char* caller);
	int GetFeatureSeparation(lua_State* L, const char* caller);

	int GetProjectileDirection(lua_State* L, const char* caller);
	int GetProjectileGravity(lua_State* L, const char* caller);
	int GetProjectileTeamID(lua_State* L, const char* caller);
	int GetProjectileAllyTeamID(lua_State* L, const char* caller);
	int GetProjectileType(lua_State* L, const char* caller);
	int GetProjectileTimeToLive(lua_State* L, const char* caller);
	int GetProjectileIsIntercepted(lua_State* L, const char* caller);

	// spatial-query + team-unit-list families (PR 27b): list twins served from
	// SnapshotPickGrid rect/radius queries (spatial) and a per-boundary
	// team-unit index derived lazily from the UnitRows front buffer (lists).
	// DOCUMENTED DEVIATION: result order is ascending-id, not master's
	// quadfield-walk / creation order; the armed dual-run compares these
	// callouts' result tables as ID sets (see CompareTablesAsIdSet).
	int GetAllUnits(lua_State* L, const char* caller);
	int GetTeamUnits(lua_State* L, const char* caller);
	int GetTeamUnitsSorted(lua_State* L, const char* caller);
	int GetTeamUnitsCounts(lua_State* L, const char* caller);
	int GetTeamUnitsByDefs(lua_State* L, const char* caller);
	int GetTeamUnitDefCount(lua_State* L, const char* caller);
	int GetUnitsInRectangle(lua_State* L, const char* caller);
	int GetUnitsInBox(lua_State* L, const char* caller);
	int GetUnitsInCylinder(lua_State* L, const char* caller);
	int GetUnitsInSphere(lua_State* L, const char* caller);
	int GetFeaturesInRectangle(lua_State* L, const char* caller);
	int GetFeaturesInSphere(lua_State* L, const char* caller);
	int GetFeaturesInCylinder(lua_State* L, const char* caller);
	int GetProjectilesInRectangle(lua_State* L, const char* caller);

	// spatial remainder (PR 39, Wave 6). GetUnitsInPlanes iterates the
	// per-boundary team-unit index (ascending id per team) reproducing master's
	// per-team GetFilteredUnits counter-reset overwrite exactly; the deviation
	// (ascending-id within team; multi-team overwrite-tail) is the binding
	// Batch-1 amendment ruling. Set-compared (CompareTablesAsIdSet).
	int GetUnitsInPlanes(lua_State* L, const char* caller);

	// frustum / screen-rect / nearest spatial family (PR 40, Wave 6). The five
	// frustum/screen-rect twins are served through a draw-side per-quad
	// object-membership mirror keyed IDENTICALLY to CQuadField (GetQuads(pos,
	// radius) disc membership, rebuilt per boundary from the pos/radius rows --
	// NOT SnapshotPickGrid's selVol superset), walked by readMap->GridVisibility
	// with a snapshot-backed IQuadDrawer re-applying the live filters. The two
	// nearest scalars reproduce CGameHelper's closest-unit search over the pick
	// grid + rows (GetClosestFriendlyUnit(synced=false) precedent). RULED
	// (Batch-4): the ascending-snapshot-id tie-break is an accepted advisory-UI
	// deviation. Set-compared where they return tables (CompareTablesAsIdSet).
	int GetVisibleUnits(lua_State* L, const char* caller);
	int GetVisibleFeatures(lua_State* L, const char* caller);
	int GetVisibleProjectiles(lua_State* L, const char* caller); // PR 41 (LuaUnsyncedRead)
	int GetUnitsInScreenRectangle(lua_State* L, const char* caller);
	int GetFeaturesInScreenRectangle(lua_State* L, const char* caller);
	int GetUnitNearestAlly(lua_State* L, const char* caller);
	int GetUnitNearestEnemy(lua_State* L, const char* caller);

	// command-queue family (PR 27b serving batch 2, first family): served from
	// per-unit boundary copies of CCommandAI::commandQue / CFactoryCAI::
	// newUnitCommands (+ the CFactory bugger-off scalars), refreshed by
	// RefreshCommandQueues() below. Copies are flattened (id/tag/options +
	// params in a flat float buffer) because raw Command copies touch the
	// sim-owned cmdParamsPool (pooled >8-param commands acquire/release pool
	// pages on copy/destruct, and read pool storage the sim thread resizes).
	int GetUnitCommands(lua_State* L, const char* caller); // also serves GetCommandQueue (same live body)
	int GetUnitCommandCount(lua_State* L, const char* caller);
	int GetUnitCurrentCommand(lua_State* L, const char* caller);
	int GetFactoryCommands(lua_State* L, const char* caller);
	int GetFactoryCommandCount(lua_State* L, const char* caller);
	int GetFactoryCounts(lua_State* L, const char* caller);
	int GetFactoryBuggerOff(lua_State* L, const char* caller);
	int GetFullBuildQueue(lua_State* L, const char* caller);
	int GetRealBuildQueue(lua_State* L, const char* caller);
	// sim|draw PR 30: cmd-desc surface + builder worker-task twins (served from
	// the same per-unit boundary cache; descs are version-keyed on CCommandAI::
	// GetCmdDescVersion(), the worker-task answer is decoded at extraction)
	int GetUnitCmdDescs(lua_State* L, const char* caller);   // ParseTypedUnit gate
	int FindUnitCmdDesc(lua_State* L, const char* caller);   // ParseTypedUnit gate
	int GetUnitWorkerTask(lua_State* L, const char* caller); // ParseInLosUnit gate

	/// producer hook (PR 44a: the flip's sim frame edge into the slot being
	/// produced, or the lockstep barrier into the held slot right after the
	/// publish -- epoch-gated, so queue copies and snapshot rows always
	/// describe the same boundary): re-copies the queues of units whose
	/// CCommandQueue version changed vs ringSlot's previous content, drops
	/// entries of dead units. The caller's thread must own live sim state
	/// (sim thread at its edge / main thread with the sim parked).
	void RefreshCommandQueues(int ringSlot, uint64_t targetEpoch);

	/// PR 43 §2.1: the EpochId the cmd-queue / piece cache slots currently
	/// describe (0 = never refreshed). The 0-arg forms read the consumer-held
	/// slot (the lockstep barrier's seal); the slot forms serve the producer.
	uint64_t CmdQueueCacheEpoch();
	uint64_t CmdQueueCacheEpoch(int slot);
	uint64_t PieceCacheEpoch();
	uint64_t PieceCacheEpoch(int slot);

	/// PR 43 §2.8 (/epochstats): approximate resident bytes of the cmd-queue
	/// and piece cache channels (the two non-SimSnapshot epoch channels with
	/// draw-owned payload stores)
	void EpochChannelBytes(size_t& cmdQueueBytes, size_t& pieceBytes);

	// sim|draw PR 30 mirror-verification hook (SnapshotDiffGate::CheckCmdQueueRows):
	// bit-compare the cached command-queue/cmd-desc/worker/factory slot for a unit
	// against live sim state at the armed boundary. Test-only (armed runs). Lives
	// here so the file-static serving cache stays encapsulated. `present` reports
	// whether a cached slot exists (compare with the live unit's validity for the
	// presence field); the *Ok bools are meaningful only when present && liveUnit.
	struct CmdQueueCompareResult {
		bool present  = false;
		bool queueOk  = false;
		bool descsOk  = false;
		bool workerOk = false;
		bool factoryOk = false;
		bool pageOk   = false; // sim|draw PR 44: lastSelectedCommandPage
	};
	CmdQueueCompareResult CompareCmdQueueSlot(int unitID, const CUnit* liveUnit);

	// sim|draw PR 44 (Gap B): served analogue of CCommandAI::GetPossibleCommands()
	// + lastSelectedCommandPage, consumed by CSelectedUnitsHandler::
	// GetAvailableCommands so LayoutIcons no longer walks live commandAI under the
	// running split. Reconstructs SCommandDescriptions from the boundary cmd-desc
	// cache into `outDescs` and returns the cached page in `outPage`. Returns false
	// when unitID has no served slot (dead / never-copied) -- the caller skips the
	// unit exactly as the live path skips a null CUnit. Main-thread-only (the cache
	// is refreshed at the barrier); no POV gate (the live body reads the owner's
	// commandAI directly regardless of allyteam -- selection is the local player's).
	bool GetServedAvailableCommands(int unitID, std::vector<SCommandDescription>& outDescs, int& outPage);

	// PR 44b: C++ view over the served (consumer-held) command-queue copy for
	// engine-side draw-context readers -- CWaitCommandsAI's wait scans under
	// the running split (walking live commandAI->commandQue from the movers
	// would be a structural race once the sim runs concurrently). Invokes
	// fn(cmdID, numParams, param0, param1) per queued command in queue order
	// until fn returns false; param0/1 are 0.0f when numParams < 1/2.
	// Returns false when unitID has no served slot (dead / never copied) --
	// the caller treats that as an empty queue, exactly like the live path's
	// null-unit skip. Main thread (the cache is the held slot's).
	bool ForEachServedCommand(int unitID, const std::function<bool(int, int, float, float)>& fn);

	// sim|draw split (Stage 0): served draw-side weapon range-ring primitives for
	// the GuiHandler weapon-range park retirement. Read the published epoch
	// (trace::EpochView over the weapon rows), never a live CWeapon*. Both mirror
	// the live glBallisticCircle CWeapon* path bit-for-bit (GetRange2D is already
	// gated bit-exact epoch-vs-live by the TestRange trace dual-run). Return 0 for
	// an invalid unit/weapon (the caller then skips the ring, as the live path skips
	// an empty weapon list).
	//   SplitServedWeaponRange2D : CWeapon::GetLiveRange2D twin -- GetRange2DT(view,
	//     0, modHeightDiff); modHeightDiff already folds in weaponDef->heightmod.
	//   SplitServedWeaponHeightMod : the immutable weaponDef->heightmod for weapon 0.
	float SplitServedWeaponRange2D(int unitID, int weaponNum, float modHeightDiff);
	float SplitServedWeaponHeightMod(int unitID, int weaponNum);

	// ---- PR 34 (spatial/list remainder) ----
	// The remainder of the spatial/list family: per-team plane test + table
	// centroids over UnitRows, the whole-list projectile/feature twins over
	// ProjectileRows/FeatureRows, and the draw-owned selection/group aggregate
	// twins (defID sort keys from UnitRows; the id containers are draw-owned).
	// List-returning ones share the ascending-id order deviation and compare as
	// ID sets; centroids and the selection/group aggregates are order-exact
	// (they iterate the same Lua table / draw-owned container as the live path).
	int GetUnitArrayCentroid(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetUnitMapCentroid(lua_State* L, const char* caller);     // LuaSyncedRead
	int GetAllProjectiles(lua_State* L, const char* caller);      // LuaSyncedRead
	int GetProjectilesInSphere(lua_State* L, const char* caller); // LuaSyncedRead (projectile radius row)
	int GetAllFeatures(lua_State* L, const char* caller);         // LuaSyncedRead
	int GetSelectedUnitsSorted(lua_State* L, const char* caller); // LuaUnsyncedRead (draw-owned selection set)
	int GetSelectedUnitsCounts(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetGroupUnitsSorted(lua_State* L, const char* caller);    // LuaUnsyncedRead (draw-owned group set)
	int GetGroupUnitsCounts(lua_State* L, const char* caller);    // LuaUnsyncedRead

	/// game teardown: drop the per-generation serving caches (the team-unit
	/// index, the command-queue copies); SimSnapshot's generation counter
	/// resets across games, so a stale cache could otherwise alias a fresh
	/// generation number
	void ClearCaches();

	// ---- PR 38f: event-time command-queue presentation --------------------
	// Synced command events (UnitCommand / UnitCmdDone) dispatch to unsynced
	// Lua handlers. Under the split those handlers run DEFERRED at the
	// SimDrawBarrier, after the sim published the boundary snapshot and
	// RefreshCommandQueues copied the LAST-boundary queues -- so a UnitCommand
	// handler would not see the just-added command (gui_selfd_icons nil), a
	// UnitCmdDone handler would still see the just-removed one (unit_idle_guard
	// compare-nil). Master dispatched them synchronously, mid-sim, against the
	// event-time queue.
	//
	// CaptureCmdQueueEvent() runs at FIRE time (sim thread owns the queue) and
	// flattens the unit's event-time command AI into an opaque, refcounted
	// snapshot (shared_ptr<void>, deleter retained -- the anon-namespace slot
	// type never leaks into this header). It returns nullptr when the split is
	// off or the call is not deferring, so the flag-off / immediate-dispatch
	// path stays byte-identical (no capture, no override). The deferred
	// dispatch installs it as a per-unit override via ScopedCmdQueueEventOverride
	// around the single handler call: GetCmdQueueSlot returns the event-time
	// slot for that unit for the duration of the call, then it is cleared. The
	// whole command-queue twin family reads through that one choke point.
	//
	// UNSYNCED (draw-only): no synced write, no sync-hash impact, no gsRNG.
	std::shared_ptr<void> CaptureCmdQueueEvent(const CUnit* unit);

	struct ScopedCmdQueueEventOverride {
		ScopedCmdQueueEventOverride(int unitID, const std::shared_ptr<void>& snap);
		~ScopedCmdQueueEventOverride();
		ScopedCmdQueueEventOverride(const ScopedCmdQueueEventOverride&) = delete;
		ScopedCmdQueueEventOverride& operator=(const ScopedCmdQueueEventOverride&) = delete;
	private:
		// previous override (save/restore, defensive against any nested
		// dispatch); stored type-erased -- the slot type is anon-namespace
		const void* prevSlot;
		int prevUnitID;
	};

	// PR 38j: top-of-callout override consults. The deferred UnitCommand/
	// UnitCmdDone / UnitLeftLos handlers run at the barrier under a live
	// exception (Route() then serves the LIVE leg, which never consults the
	// event-time overrides installed above). These predicates let the specific
	// affected callouts (LuaSyncedRead GetUnitCommands/CommandCount/
	// CurrentCommand and GetUnitPosition/Direction) detect, at their TOP and
	// INDEPENDENT of the live exception, that arg#1's unit currently has an
	// event-time override installed, so they can serve via the snapshot twin
	// (where the override lives) for that one unit -- without forcing the rest of
	// the handler strict (the 38h over-reach, now reverted). Each fast-rejects on
	// its inert global before touching the lua stack; both are false flag-off and
	// during the armed diff-gate dual-run (no override is ever installed there),
	// so those paths stay byte-identical. Reads arg#1 (the unitID) non-
	// destructively.
	bool CmdQueueEventOverrideActive(lua_State* L); // command-queue family
	bool LosEventOverrideActive(lua_State* L);      // position/LOS-exit family

	// unsynced flag/drawer parse-gate family (PR 27b serving batch 2,
	// family 2): payloads are draw-owned, only the ParseUnit/ParseFeature
	// gate is snapshot-served
	int GetUnitLuaDraw(lua_State* L, const char* caller);            // LuaUnsyncedRead
	int GetUnitNoDraw(lua_State* L, const char* caller);             // LuaUnsyncedRead
	int GetUnitNoMinimap(lua_State* L, const char* caller);          // LuaUnsyncedRead
	int GetUnitNoGroup(lua_State* L, const char* caller);            // LuaUnsyncedRead
	int GetUnitNoSelect(lua_State* L, const char* caller);           // LuaUnsyncedRead (unit; feature twin is row-served)
	int GetUnitEngineDrawMask(lua_State* L, const char* caller);     // LuaUnsyncedRead
	int GetUnitAlwaysUpdateMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetUnitDrawFlag(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer-owned flag, pointer via boundary resolve cache + shell
	int UnitIconGetDraw(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer icon-state payload (drawIcon)
	int GetUnitIcon(lua_State* L, const char* caller);       // LuaUnsyncedRead: drawer icon index -> icon name
	int GetUnitIconData(lua_State* L, const char* caller);   // LuaUnsyncedRead: drawer icon index -> IconData table
	int IsUnitSelected(lua_State* L, const char* caller);    // LuaUnsyncedRead: selection set is id-keyed, no pointer needed
	int GetUnitGroup(lua_State* L, const char* caller);      // LuaUnsyncedRead: team gate from rows, uiGroupHandlers lookup is id-keyed
	int IsUnitInView(lua_State* L, const char* caller);        // LuaUnsyncedRead
	int GetUnitTransformMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead (drawPos drawer-owned; basis/error mirrored from rows)
	int GetUnitSelectionVolumeData(lua_State* L, const char* caller); // LuaUnsyncedRead (selVol payload stays live: unsynced-owned, read via IdToObject)
	int GetUnitRotation(lua_State* L, const char* caller); // unsynced drawer-matrix branch only (ShouldServe rejects synced handles)
	int GetFeatureLuaDraw(lua_State* L, const char* caller);          // LuaUnsyncedRead
	int GetFeatureNoDraw(lua_State* L, const char* caller);           // LuaUnsyncedRead
	int GetFeatureEngineDrawMask(lua_State* L, const char* caller);   // LuaUnsyncedRead
	int GetFeatureAlwaysUpdateMatrix(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetFeatureDrawFlag(lua_State* L, const char* caller);         // LuaUnsyncedRead
	int GetFeatureSelectionVolumeData(lua_State* L, const char* caller); // LuaUnsyncedRead
	int GetFeatureTransformMatrix(lua_State* L, const char* caller);  // LuaUnsyncedRead
	int GetFeatureRotation(lua_State* L, const char* caller);

	// ---- PR 28: map-layer mirror family (positional LOS + map info) ----
	// Served from DrawMapMirrors (the boundary-drained draw-owned copies of the
	// per-allyteam LOS/radar/jammer maps, the terrain-type table, the
	// smooth-height mesh, the original heightmap, and the radar-error scalars),
	// NOT from SimSnapshot rows. Same §E.2 Route()/twin recipe; the positional
	// twins mirror the CLosHandler formulas, the map-info twins mirror the
	// ground interpolation math. POV: the LOS maps are per-allyteam like the
	// source, so the twins index the requested allyteam's mirror.
	int IsPosInLos(lua_State* L, const char* caller);
	int IsPosInRadar(lua_State* L, const char* caller);
	int IsPosInAirLos(lua_State* L, const char* caller);
	int GetPositionLosState(lua_State* L, const char* caller);
	int GetRadarErrorParams(lua_State* L, const char* caller);
	int GetTerrainTypeData(lua_State* L, const char* caller);
	int GetSmoothMeshHeight(lua_State* L, const char* caller);
	int GetGroundOrigHeight(lua_State* L, const char* caller);
	// ---- sim|draw PR 38d ----
	// GetGroundInfo reads the DrawMapMirrors typemap + metal-distribution mirrors
	// (added by PR 38d) plus the PR-28 terrain-type table copy; it is a line-by-
	// line mirror of the live body with the readMap->GetTypeMapSynced() and
	// LuaMetalMap::GetMetalAmount reads swapped for the mirror queries.
	int GetGroundInfo(lua_State* L, const char* caller);    // LuaSyncedRead

	// ---- sim|draw PR 29 (blocking-map mirror + placement family) ----
	// GetGroundBlocked reads the DrawMapMirrors blocking mirror (per-square
	// cell[0] id + kind) and gates visibility through the snapshot unit/feature
	// rows. Pos2BuildPos snaps to the build grid over the already-draw-safe
	// unsynced heightmap (no sim-owned mutable state), so its twin reuses the
	// live CGameHelper helper directly with synced=false.
	int GetGroundBlocked(lua_State* L, const char* caller); // LuaSyncedRead
	int Pos2BuildPos(lua_State* L, const char* caller);     // LuaSyncedRead

	int GetGaiaTeamID(lua_State* L, const char* caller);
	int GetAllyTeamList(lua_State* L, const char* caller);
	int GetTeamList(lua_State* L, const char* caller);
	int GetPlayerList(lua_State* L, const char* caller);
	int GetTeamInfo(lua_State* L, const char* caller);
	int GetTeamAllyTeamID(lua_State* L, const char* caller);
	int GetTeamResources(lua_State* L, const char* caller);
	int GetTeamUnitStats(lua_State* L, const char* caller);
	int GetTeamResourceStats(lua_State* L, const char* caller);
	int GetTeamDamageStats(lua_State* L, const char* caller);
	int GetTeamUnitCount(lua_State* L, const char* caller);
	int GetPlayerInfo(lua_State* L, const char* caller);
	int GetTeamColor(lua_State* L, const char* caller);     // LuaUnsyncedRead
	int GetTeamOrigColor(lua_State* L, const char* caller); // LuaUnsyncedRead

	// ---- PR 33 (pieces/scripts family) ----
	// RefreshPieces() runs at the barrier (right after simSnapshot.Update(),
	// generation-gated) and captures per-object piece dynamic state + immutable
	// model metadata so the unit/feature piece twins below serve pointer-free
	// under the running split (see the serving-section header in the .cpp).
	// Piece-projectile params/name are served from SimSnapshot::ProjectileRows.
	// (PR 44a: slot-aware like RefreshCommandQueues -- see there.)
	void RefreshPieces(int ringSlot, uint64_t targetEpoch);

	int GetUnitRootPiece(lua_State* L, const char* caller);
	int GetUnitPieceMap(lua_State* L, const char* caller);
	int GetUnitPieceList(lua_State* L, const char* caller);
	int GetUnitPieceInfo(lua_State* L, const char* caller);
	int GetUnitPiecePosition(lua_State* L, const char* caller);
	int GetUnitPieceDirection(lua_State* L, const char* caller);
	int GetUnitPiecePosDir(lua_State* L, const char* caller);
	int GetUnitPieceMatrix(lua_State* L, const char* caller);
	int GetUnitScriptPiece(lua_State* L, const char* caller);
	int GetUnitScriptNames(lua_State* L, const char* caller);

	int GetFeatureRootPiece(lua_State* L, const char* caller);
	int GetFeaturePieceMap(lua_State* L, const char* caller);
	int GetFeaturePieceList(lua_State* L, const char* caller);
	int GetFeaturePieceInfo(lua_State* L, const char* caller);
	int GetFeaturePiecePosition(lua_State* L, const char* caller);
	int GetFeaturePieceDirection(lua_State* L, const char* caller);
	int GetFeaturePiecePosDir(lua_State* L, const char* caller);
	int GetFeaturePieceMatrix(lua_State* L, const char* caller);
	int GetFeatureCollisionVolumeData(lua_State* L, const char* caller);
	int GetFeaturePieceCollisionVolumeData(lua_State* L, const char* caller);
	int GetFeatureLastAttackedPiece(lua_State* L, const char* caller);

	int GetPieceProjectileParams(lua_State* L, const char* caller);
	int GetPieceProjectileName(lua_State* L, const char* caller);

	// ---- PR 31 (weapon/shield scalar family) ----
	// Served from the SimSnapshot::UnitRows weapon block (per-unit scalars + a
	// flat per-weapon SoA). All gate on the unit's ParseAllyUnit visibility
	// (PovAlliedUnit) except GetUnitShieldState (ParseInLosUnit -> PovUnitInLos).
	// Trace tests (TryTarget/TestTarget/TestRange/HaveFreeLineOfFire) are NOT
	// here -- PR 35 recomputes them against the published collision world.
	int GetUnitStockpile(lua_State* L, const char* caller);
	int GetUnitShieldState(lua_State* L, const char* caller);
	int GetUnitFlanking(lua_State* L, const char* caller);
	int GetUnitWeaponState(lua_State* L, const char* caller);
	int GetUnitWeaponDamages(lua_State* L, const char* caller);
	int GetUnitWeaponVectors(lua_State* L, const char* caller);
	int GetUnitWeaponCanFire(lua_State* L, const char* caller);
	int GetUnitWeaponTarget(lua_State* L, const char* caller);

	// ---- PR 32 (deep per-unit state) ----
	// Served from the PR-32 SimSnapshot::UnitRows deep rows + the full-table
	// moveType block; the collision-volume / last-hit-piece three reuse PR 33's
	// unit piece cache (RefreshPieces now captures colVol + lastHit for units).
	int GetUnitStates(lua_State* L, const char* caller);            // ParseAllyUnit
	int GetUnitStorage(lua_State* L, const char* caller);           // ParseAllyUnit
	int GetUnitMetalExtraction(lua_State* L, const char* caller);   // ParseAllyUnit
	int GetUnitBuildeeRadius(lua_State* L, const char* caller);     // ParseTypedUnit
	int GetUnitPosErrorParams(lua_State* L, const char* caller);    // ParseAllyUnit
	int GetUnitLastAttacker(lua_State* L, const char* caller);      // ParseUnit (attacker visibility gate)
	int GetUnitIsBuilding(lua_State* L, const char* caller);        // ParseAllyUnit
	int GetUnitBuildParams(lua_State* L, const char* caller);       // ParseAllyUnit
	int GetUnitInBuildStance(lua_State* L, const char* caller);     // ParseAllyUnit
	int GetUnitCurrentBuildPower(lua_State* L, const char* caller); // ParseAllyUnit
	int GetUnitEffectiveBuildRange(lua_State* L, const char* caller);// ParseInLosUnit
	int GetUnitNanoPieces(lua_State* L, const char* caller);        // ParseAllyUnit
	int GetUnitTransporter(lua_State* L, const char* caller);       // ParseInLosUnit
	int GetUnitIsTransporting(lua_State* L, const char* caller);    // ParseAllyUnit
	int GetUnitTooltip(lua_State* L, const char* caller);           // ParseTypedUnit
	int GetUnitMoveTypeData(lua_State* L, const char* caller);      // ParseAllyUnit
	int GetUnitCollisionVolumeData(lua_State* L, const char* caller);      // ParseInLosUnit (piece cache)
	int GetUnitPieceCollisionVolumeData(lua_State* L, const char* caller); // ParseInLosUnit (piece cache)
	int GetUnitLastAttackedPiece(lua_State* L, const char* caller);        // ParseAllyUnit (piece cache)
	int IsUnitInLos(lua_State* L, const char* caller);              // ParseTypedUnit
	int IsUnitInAirLos(lua_State* L, const char* caller);           // ParseTypedUnit
	int IsUnitInJammer(lua_State* L, const char* caller);           // ParseTypedUnit

	// team/player misc family (PR 36; TeamRows/PlayerRows extensions + the
	// SimSnapshot map-start cache -- see LuaSnapshotServe.cpp's PR-36 section)
	int GetTeamStartPosition(lua_State* L, const char* caller);
	int GetAllyTeamStartBox(lua_State* L, const char* caller);
	int GetMapStartPositions(lua_State* L, const char* caller);
	int GetTeamMaxUnits(lua_State* L, const char* caller);
	int GetTeamLuaAI(lua_State* L, const char* caller);
	int GetAIInfo(lua_State* L, const char* caller);
	int GetAllyTeamInfo(lua_State* L, const char* caller);
	int AreTeamsAllied(lua_State* L, const char* caller);
	int ArePlayersAllied(lua_State* L, const char* caller);
	int GetPlayerControlledUnit(lua_State* L, const char* caller);
	int GetTeamStatsHistory(lua_State* L, const char* caller);
	int GetPlayerStatistics(lua_State* L, const char* caller); // LuaUnsyncedRead

	// ---- PR 38 (zero-sanction flip): game+team rules-params serving ----
	// Served from the SimSnapshot GlobalRows::gameRulesParams (singleton) and
	// TeamRows::teamRulesParams (per-team) mirrors. The game twins have no POV
	// (always PRIVATE_MASK, "readable for all"); the team twins mirror the live
	// losMask computation (PovAlliedTeam private / AlliedTeams-mirror allied /
	// public) over the snapshot alliance rows. Only game+team are served here:
	// the player/unit/feature namespaces stay sanctioned (unit/feature ids reuse
	// -> need the RenderEventQueue-ordered delta mechanism; escalated).
	int GetGameRulesParam(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetGameRulesParams(lua_State* L, const char* caller);  // LuaSyncedRead
	int GetTeamRulesParam(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetTeamRulesParams(lua_State* L, const char* caller);  // LuaSyncedRead

	// ---- PR 38c (zero-sanction flip): player/unit/feature rules-params ----
	// Extends PR 38 part 1's mechanism to the per-object namespaces (PlayerRows::
	// playerRulesParams / UnitRows::unitRulesParams / FeatureRows::
	// featureRulesParams boundary mirrors, plain per-boundary full copies). The
	// twins reproduce each live callout's exact POV/losMask decision + ParseUnit/
	// ParseFeature/IsValidPlayer visibility gate over the snapshot rows.
	int GetPlayerRulesParam(lua_State* L, const char* caller);    // LuaSyncedRead
	int GetPlayerRulesParams(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetUnitRulesParam(lua_State* L, const char* caller);      // LuaSyncedRead
	int GetUnitRulesParams(lua_State* L, const char* caller);     // LuaSyncedRead
	int GetFeatureRulesParam(lua_State* L, const char* caller);   // LuaSyncedRead
	int GetFeatureRulesParams(lua_State* L, const char* caller);  // LuaSyncedRead

	// ---- PR 38g (Batch-4 P1): sanctioned-tail serving ----
	// The last serve-able survivors past the 8 Wave-6 spatial callouts. Same §E.2
	// Route()/twin recipe; each twin reproduces its live body's parse-gate POV +
	// return shape EXACTLY over the snapshot rows:
	//  - GetUnitEstimatedPath: UnitRows est-path block (per-boundary GetPathWayPoints
	//    capture, a pure const read); ParseAllyUnit POV, ground-move + pathID gate,
	//    PushPathNodes' two-table shape.
	//  - GetFeatureFireTime/SmokeTime: FeatureRows fire/smoke timers; ParseFeature POV.
	//  - GetProjectileDamages: ProjectileRows DamagesSnap (PushDamagesKeySnap, shared
	//    with GetUnitWeaponDamages); ParseProjectile POV + isWeapon gate.
	int GetUnitEstimatedPath(lua_State* L, const char* caller);  // LuaSyncedRead (ParseAllyUnit)
	int GetFeatureFireTime(lua_State* L, const char* caller);    // LuaSyncedRead (ParseFeature)
	int GetFeatureSmokeTime(lua_State* L, const char* caller);   // LuaSyncedRead (ParseFeature)
	int GetProjectileDamages(lua_State* L, const char* caller);  // LuaSyncedRead (ParseProjectile)

	// ======================= weapon trace tests =======================
	// GetUnitWeaponTryTarget/TestTarget/TestRange/HaveFreeLineOfFire served
	// DRAW-SIDE via trace::EpochView (TRACE REHOST, doc/sim-draw-trace-rehost-
	// plan.md; supersedes the PR 35 sim-side query/reply channel, retired in
	// stage 4b -- the "recompute draw-side" the plan doc's PR-35 row named all
	// along, once the read-set rows / WeaponPredicates.h / TraceEpochView.h
	// mirror made a bit-exact draw recompute tractable).
	//
	//  - Flag-OFF (no running split): RouteTraceQuery runs the LIVE predicate
	//    inline (bit-identical to master; sim==draw single-threaded, the sim is
	//    parked relative to draw). Armed: dual-runs live vs EpochView.
	//  - Flag-ON (running split): the callout evaluates the SAME templated
	//    trace:: predicate stack the sim runs (WeaponPredicates.h), instantiated
	//    over trace::EpochView (SimSnapshot per-weapon SoA + UnitRows + demand
	//    piece-cache collision volumes + object-free CCollisionHandler + CGround
	//    mirrors), synchronously (no queue). All EpochView reads are UNSYNCED
	//    (draw-only): no synced write, no gsRNG, no streflop.
	//
	// FLAG-ON DEVIATION (the recorded deviation swap): the verdict is at the
	// CURRENT query position vs a <=1-boundary-old world (the inverse of the
	// retired channel's exact-state / stale-position). TestTarget/TestRange/
	// TryTarget are bit-identical to live; HaveFreeLineOfFire carries only the
	// inherent <=1-frame ground-staleness / pick-grid-broadphase advisory
	// deviation -- the same draw-side class placement/pick-grid already carry.
	// Advisory UI only; authoritative synced targeting re-runs sim-side at fire.
	enum class TraceKind { TryTarget = 0, TestTarget = 1, TestRange = 2, HaveFreeLineOfFire = 3 };

	/// entry-point glue for the four trace tests (called from LuaSyncedRead):
	/// dispatches to `liveFn` when the split is off (bit-identical); under the
	/// running split serves the verdict DRAW-SIDE against the published epoch
	/// (trace::EpochView, TRACE REHOST); dual-runs live-vs-epoch for coverage
	/// when the diff gate is armed flag-off.
	int RouteTraceQuery(lua_State* L, const char* caller, ServeFn liveFn, TraceKind kind);

	// ================= PR 38e: placement build/move tests =================
	// TestBuildOrder/TestMoveOrder/ClosestBuildPos served by the SAME sim-side
	// QUERY/REPLY channel as PR 35's weapon trace tests (the last three
	// sanctionedLive entries; landing them unblocks PR 38b's zero-sanction flip).
	// They re-host CGameHelper::TestUnitBuildSquare / MoveDef::TestMoveSquare /
	// ClosestBuildPos + CMoveMath, whose bit-exact draw-side recompute would need
	// the whole terrain-speedmod mirror + full per-cell object capture (the PR 29
	// escalation note) -- an approximation-prone sub-project that would break
	// flag-off bit-identity (Route() serves the value even flag-off). So, like
	// PR 35:
	//
	//  - Flag-OFF (no running split): RoutePlacementQuery runs the LIVE body
	//    inline (bit-identical to master; sim==draw single-threaded, sim parked
	//    relative to draw). No queue, no defer.
	//  - Flag-ON (running split): the callout enqueues a placement query and
	//    returns the LAST boundary's sim-exact reply (documented default on
	//    first-call/miss). EvaluatePlacementQueries() drains the pending queue at
	//    the SimDrawBarrier (sim parked) and evaluates the EXACT live predicate
	//    against live sim state -- boundary-deferred (<=1 stale) but sim-exact, no
	//    approximation. Reply state is UNSYNCED (draw-only): no synced write, no
	//    gsRNG, no streflop (the predicates are const reads over the blocking map /
	//    terrain / heightmap / los, already called from unsynced widgets on master
	//    without desync).
	//
	// This request/reply channel is DISTINCT from the published snapshot ring --
	// PR 43's epoch mechanism must carry the query queue + reply map alongside the
	// ring (same flag as the PR 35 trace channel).
	//
	// FLAG-ON DEVIATION (PR 43 §7.3, operator-ruled PER-CALLOUT keying --
	// supersedes the 38e full-query-key deviation, which made a
	// cursor-following build preview a perpetual map miss returning the
	// conservative default):
	//  - TestBuildOrder: keyed by the build-grid-SNAPPED pos (canonicalized
	//    at query build). The verdict is grid-cell-quantized (Pos2BuildPos
	//    snaps x/z and recomputes y from terrain), so within-cell cursor
	//    motion hits the same key -- correct values WHILE the cursor moves;
	//    this is the correct key, not a band-aid.
	//  - ClosestBuildPos / TestMoveOrder: results depend continuously on
	//    worldPos (no snap is value-safe), so they use the STANDING-LATEST-
	//    QUERY model -- pos-agnostic reply key, the barrier evaluates the
	//    most recent registered position: 1 boundary late but never-default.
	// Enumerated deviations: (a) replies reflect a <=1-boundary-old cursor
	// position; (b) two same-frame queries sharing a standing key but
	// different positions collide, last registered wins (ruled acceptable --
	// advisory UI predicates).
	enum class PlacementKind { TestMoveOrder = 0, TestBuildOrder = 1, ClosestBuildPos = 2 };

	/// entry-point glue for the three placement tests (called from LuaSyncedRead):
	/// dispatches to `liveFn` when the split is off (bit-identical); under the
	/// running split serves the verdict DRAW-SIDE against the published epoch
	/// (placement::EpochView, PLACEMENT REHOST); dual-runs live-vs-epoch for
	/// coverage when the diff gate is armed flag-off.
	int RoutePlacementQuery(lua_State* L, const char* caller, ServeFn liveFn, PlacementKind kind);
	// PLACEMENT REHOST (stage 4) + TRACE REHOST (stage 4b): both query/reply
	// channels are retired -- EvaluatePlacementQueries/ClearPlacementQueryChannel
	// and EvaluateTraceQueries/ClearTraceQueryChannel are all gone; every callout
	// is served draw-side against the published epoch, synchronously.
}
