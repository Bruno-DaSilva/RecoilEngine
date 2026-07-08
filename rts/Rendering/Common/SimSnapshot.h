/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "Game/Players/PlayerStatistics.h"
#include "Lua/LuaRulesParams.h" // PR 38: game+team rules-params mirror (Params map)
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/Resource.h"
#include "Sim/Misc/TeamStatistics.h"
#include "System/UnorderedMap.hpp"
#include "System/SimDrawSplit.h" // PR 38b: DEAD_THIS_BATCH validity consults the drain-window flag
#include "System/float3.h"
#include "System/float4.h"

// PR 38b (zero-sanction flip): the tri-state row-validity enum. The valid[]
// arrays hold one of these per id. DEAD_THIS_BATCH is a retained-but-dead row
// (the object's destroy record is in the batch just published): Extract left it
// invalid, so its data rows still hold the last-boundary (~at-death) values, and
// Valid() promotes it to true ONLY inside the boundary dispatch-drain window
// (SimDrawSplit::BoundaryDrainWindowActive) -- so the deferred death/LOS/command
// handlers read the object at its at-death state (as master's synchronous
// mid-frame dispatch did) and everything else reads the dead-id nil shape.
namespace SimSnapshotValid {
	inline constexpr uint8_t INACTIVE        = 0; // no object / stale garbage
	inline constexpr uint8_t ACTIVE          = 1; // live at the stamped simFrame
	inline constexpr uint8_t DEAD_THIS_BATCH = 2; // destroyed this batch; row retained
}

/**
 * @brief SimSnapshot -- the extracted, flat, render-side copy of hot observable sim state
 *
 * Coupling inventory section C of doc/sim-draw-thread-decoupling-research.md
 * (PR 15): draw-side code that today dereferences live sim objects reads this
 * snapshot instead. Together with RenderEventQueue (the event half of the
 * sim->draw boundary) this is the data half: sim state crosses the boundary
 * only as plain values, never as pointers. The layout doubles as the keyframe
 * payload of the recorded observable stream (doc/replay-seeking-architecture.md),
 * so changes here propagate into the stream format.
 *
 * Layout and indexing:
 *  - Structure-of-arrays: one parallel array per field, all indexed by unitID.
 *    Unit IDs are dense [0, unitHandler.MaxUnits()) and are already the
 *    Lua-facing handle, so no id->slot indirection exists or is wanted.
 *  - v1 fields: validity, pos, speed (.w = |velocity|), health, maxHealth,
 *    team, allyTeam, defID, buildProgress, losStatus. Deliberately excluded:
 *    piece transforms (extracted separately since PR 8, see
 *    CModelDrawerDataBase::ExtractTransforms) and command queues / paths
 *    (unbounded; dirty-versioned copies in a later PR).
 *  - PR 18 (positions/status callout family) additions: midPos, aimPos,
 *    paralyzeDamage, captureProgress, beingBuilt, stunned, plus the masking
 *    inputs (see below): relMidPos + frontdir/updir/rightdir (object-space
 *    math for the drawer-based midpos variants), posErrorVector, leavesGhost,
 *    per-allyteam stride rows losStatusAll / posErrorBits, and a per-buffer
 *    global block (numAllyTeams, radar-error scalars, alliance matrix).
 *    Second family (projectiles) added radius (IsUnitVisible default) and the
 *    ProjectileRows namespace below: synced projectiles keyed by synced
 *    projectile id (free-list ints, bounded by the high-water concurrent
 *    count -- rows grow-only to the max id seen), same buffer/publish
 *    lifecycle as the unit rows. Unsynced projectiles are draw-side state and
 *    are not Lua-visible through the synced callouts, so they have no rows.
 *  - PR 25 (draw-side picking, section D) additions: the object read set of
 *    GuiTraceRay + box-select. Units gained selVol (the live per-object
 *    selectionVolume, copied by value -- an unsynced widget can mutate it at
 *    runtime, so this is the current value, never the def), noSelect and
 *    inVoid (the two hit-test gates). The FeatureRows namespace below is the
 *    first non-unit/non-projectile family (features are pickable): dense but
 *    sparse-occupancy feature ids, rows grow-only to the max id seen like the
 *    projectile rows, carrying validity/pos/midPos/relMidPos/radius/allyTeam/
 *    defID + the same selVol/noSelect/inVoid picking gates + per-allyteam
 *    positional-LOS bytes (inLosAll) and the two IsInLosForAllyTeam globals
 *    (featureVisibility mode, gaiaAllyTeam) for the visibility mirror.
 *    Piece-tree selection volumes (usePieceSelectionVolumes) are deliberately
 *    NOT served here -- unused in BAR; picking falls back to a live sim read
 *    for those objects, to be removed at split-enable (see TraceRay.cpp).
 *  - PR 27a (split-contract serving pass) additions: the remaining
 *    row-backable per-object callout reads. Units gained the status/eco
 *    scalar tail (isDead/neutral/activated/isCloaked, armoredState +
 *    armoredMultiple, heading/buildFacing, height/mass/maxRange/
 *    seismicSignature, experience/limExperience, selfDCountdown, the seven
 *    sensor radii, moveDefID, resourcesMake/Use, harvested/harvestStorage,
 *    cost/buildTime, blockingBits). Features gained their live tail (team,
 *    health/resurrectProgress, height/mass, speed, heading/buildFacing, the
 *    transMatrix direction columns, resources/defResources + reclaimLeft/
 *    reclaimTime, blockingBits, resurrectDefID). Projectiles gained dir,
 *    mygravity, teamID, isPiece and the weapon-projectile ttl/intercepted
 *    pair. All of it is synced sim state (hashed + diff-gated).
 *
 * Validity rules:
 *  - Valid(id) mirrors membership in unitHandler's active-unit list at the
 *    stamped simFrame; dying-but-not-yet-deleted units are therefore valid,
 *    exactly as master's live reads would see them.
 *  - PR 38b tri-state: valid[] holds SimSnapshotValid::{INACTIVE,ACTIVE,
 *    DEAD_THIS_BATCH}. A row destroyed in the batch just published is marked
 *    DEAD_THIS_BATCH (its data rows retain the last-boundary/at-death values,
 *    since Extract overwrites only ACTIVE slots). Valid(id) returns true for
 *    DEAD_THIS_BATCH ONLY while the boundary dispatch-drain window is open
 *    (SimDrawSplit::BoundaryDrainWindowActive), so the deferred death/LOS/
 *    command handlers replaying at the barrier see the object at its at-death
 *    state -- reproducing master's "the handler sees the object still alive"
 *    from rows. Outside the drain (and flag-off, where DEAD_THIS_BATCH is never
 *    set) it reads exactly like INACTIVE: the dead-id nil shape.
 *  - Rows of invalid ids hold stale garbage and must never be read directly;
 *    the field accessors below return a deterministic default (0) for invalid
 *    ids -- this is the documented stale/nil contract for consumers that hold
 *    an id the snapshot does not cover (e.g. a unit created after the last
 *    extraction): the miss is identical every time, never a torn read.
 *  - A recycled unitID describes the *current* owner of the slot as of the
 *    stamped simFrame. A consumer resolving a live CUnit* against snapshot
 *    rows can be one boundary stale (see below); if a future consumer needs
 *    to detect reuse across that window, add a per-slot generation tag --
 *    do not widen the staleness contract instead.
 *
 * losStatus / masking semantics (masking policy settled in PR 18):
 *  - losStatusAll holds one byte per (allyTeam, unit), laid out as numAllyTeams
 *    consecutive rows of maxUnits bytes (index at * maxUnits + unitID);
 *    posErrorBits has the identical layout (CUnit::GetPosErrorBit per
 *    allyteam). LosStatus(unitID, allyTeam) is the row accessor -- consumers
 *    that used the old single-row form pass gu->myAllyTeam and read exactly
 *    the byte they read before. Rows are POV-complete: no re-extraction is
 *    needed when the viewed allyteam changes (/specteam), the accessor just
 *    reads a different row.
 *  - DECIDED (PR 18): masking happens at the SERVING layer, not at
 *    extraction. The snapshot stores raw synced values plus the masking
 *    *inputs* for every allyteam (losStatusAll, posErrorBits, posErrorVector,
 *    leavesGhost, the alliance matrix and the radar-error scalars), and the
 *    POV helpers below (PovAlliedUnit / PovUnitVisible / PovUnitInLos /
 *    ErrorVector / LuaErrorVector) replicate the live gate + errorVector
 *    formulas (LuaUtils::IsUnitVisible / IsUnitInLos, CUnit::GetErrorVector)
 *    bit-for-bit from those inputs. Rationale: one snapshot serves every
 *    handle POV at once (LuaUI player POV, LuaRules-unsynced, spectators
 *    flipping /specteam between boundaries), and the result is directly
 *    bit-comparable against the live path (SnapshotDiffGate). Since the
 *    snapshot is CPU-side process memory -- which already holds the full
 *    lockstep sim -- storing raw rows widens no information surface; the
 *    leak boundary is what values cross into Lua, and the serving-layer
 *    gates are the same gates master applies. Extraction-time masking
 *    remains MANDATORY for GPU-visible surfaces (Lua-shader-readable SSBOs;
 *    see the LOS gate in UpdateObjectUniforms / ExtractTransforms) -- do not
 *    copy this policy there.
 *
 * Extraction timing:
 *  - Update() runs at the start of the draw side of the frame -- in
 *    CGame::Draw, right after renderEventQueue.Drain() and
 *    AckDrainedDestroys() -- so the snapshot and the drawer containers agree
 *    on the same completed sim frame N.
 *  - Zero new sim frames since the last extraction = no-op. A catch-up burst
 *    of N sim frames produces one extraction (intermediate frames are
 *    unobservable, same as master's rendering). Extraction also re-runs when
 *    the alive-unit count changes outside the frame cadence (objects spawned
 *    before the first sim frame advances -- same edge ExtractTransforms
 *    handles via its pending flag), when sim marks a between-frames mutation
 *    (net-message-driven team transfers, see MarkMutatedOutsideFrame), and on
 *    any frameNum mismatch including backwards jumps (checkpoint load /
 *    replay rewind). Viewed-allyteam changes need no re-extraction since the
 *    per-allyteam rows became POV-complete (PR 18).
 *  - Consumers called outside CGame::Draw (input handlers, e.g. minimap
 *    select) read the previous boundary's snapshot: at most one draw frame of
 *    staleness, the same pick-latency semantics decided for boundary picking
 *    (research doc section D).
 *
 * Generation / swap semantics:
 *  - Double-buffered: extraction fills the back buffer, then publishes it by
 *    swapping the front/back pointers and bumping Generation(). Read() is
 *    valid until the next Update(); today everything is on one thread, and
 *    under the future split the swap happens inside the extract barrier while
 *    draw-side readers hold the front buffer for the whole draw frame.
 *  - Each buffer is stamped with the simFrame it was extracted at and the
 *    viewAllyTeam its losStatus row belongs to.
 *
 * Adding a field (every later consumer conversion follows this recipe):
 *  1. add the parallel array to UnitRows and size it in Resize();
 *  2. copy the value in Extract()'s per-unit loop (values only -- never a
 *     pointer, never lazy recompute of sim-side caches);
 *  3. add an accessor with the invalid-id default, and a word for the field
 *     in SnapshotHash::HashUnitRow (fixed order);
 *  4. add the field compare to SnapshotDiffGate::CheckBoundary;
 *  5. convert consumers from the live dereference to the accessor, one small
 *     PR per consumer, keeping any gu->spectatingFullView bypass live.
 */
class SimSnapshot
{
public:
	// ---- PR 32 (deep per-unit state): GetUnitMoveTypeData full-table block ----
	// The deep, subtype-specific AMoveType fields GetUnitMoveTypeData reads
	// beyond the flat base rows (mtMaxSpeed/mtMaxWantedSpeed/mtGoalPos/
	// mtProgressState/moveTypeKind). One fixed-shape struct per unit: the table
	// is bounded per unit (every field is a scalar, the shape is fixed by
	// moveTypeKind), so this is a flat AoS row -- extracted for ALL units in the
	// per-unit loop like every other row, NOT dirty-versioned or interest-copied.
	// DECISION 2 (PR 32) DEVIATION: the plan recommended interest-flagged copy;
	// full-copy-for-all is decision 2's explicit "full copy for all" alternative
	// and is used here because (a) the block is bounded-per-unit (not truly
	// unbounded) so the cost is bounded/measurable, and (b) interest-flagging's
	// one-boundary first-query latency is incompatible with the armed dual-run's
	// exact-equality pass criterion (a partial first-query table would flag a
	// mismatch every unit's first query, so the gate could never reach 0). The
	// full copy is bit-exact with the live table every boundary. Default-zero for
	// units whose moveType is not one of the four dynamic subtypes (the twin then
	// serves only the base rows + name, matching the live "static"/"script"/
	// "unknown" branches which push name only). Values only, via the live
	// accessors, so the served table is bit-identical.
	struct MoveTypeBlock {
		// ground + hover shared (GetTurnRate/GetAccRate/GetDecRate)
		float turnRate = 0.0f;
		float accRate = 0.0f;
		float decRate = 0.0f;
		// ground (CGroundMoveType)
		float maxReverseSpeed = 0.0f; // * GAME_SPEED at extraction
		float wantedSpeed = 0.0f;     // * GAME_SPEED at extraction
		float currentSpeed = 0.0f;    // * GAME_SPEED at extraction
		float goalRadius = 0.0f;
		float3 currWayPoint;
		float3 nextWayPoint;
		// hover/strafe air shared
		float wantedHeight = 0.0f;
		uint8_t collide = 0;
		uint8_t useSmoothMesh = 0;
		int32_t aircraftState = 0;    // AAirMoveType::AircraftState enum value
		// hover air (CHoverAirMoveType)
		int32_t flyState = 0;         // CHoverAirMoveType::FlyState enum value
		float goalDistance = 0.0f;
		uint8_t bankingAllowed = 0;
		uint8_t dontLand = 0;         // GetAllowLanding()
		float currentBank = 0.0f;
		float currentPitch = 0.0f;
		float altitudeRate = 0.0f;
		float maxDrift = 0.0f;
		// strafe air (CStrafeAirMoveType)
		float myGravity = 0.0f;
		float maxBank = 0.0f;
		float turnRadius = 0.0f;
		float maxAileron = 0.0f;
		float maxElevator = 0.0f;
		float maxRudder = 0.0f;
	};

	struct UnitRows {
		int32_t simFrame = -1;      // sim frame this buffer was extracted at
		int32_t aliveCount = 0;

		// global block: fixed-at-gamestart sim tables the masking formulas
		// need; re-extracted (cheap) every boundary like everything else
		int32_t numAllyTeams = 0;
		float baseRadarErrorSize = 0.0f;
		std::vector<float> radarErrorSizes;  // [numAllyTeams]
		std::vector<uint8_t> allied;         // [numAllyTeams^2], teamHandler.Ally(a,b)

		std::vector<uint8_t> valid;
		std::vector<float3> pos;
		std::vector<float3> midPos;
		std::vector<float3> aimPos;
		std::vector<float4> speed;
		std::vector<float> health;
		std::vector<float> maxHealth;
		std::vector<float> paralyzeDamage;
		std::vector<float> captureProgress;
		std::vector<uint8_t> team;
		std::vector<uint8_t> allyTeam;
		std::vector<int32_t> defID;
		std::vector<float> buildProgress;
		std::vector<uint8_t> beingBuilt;
		std::vector<uint8_t> stunned;        // CUnit::IsStunned()
		std::vector<float> radius;           // IsUnitVisible's default sphere

		// picking (PR 25): the live per-object selection volume (copied by
		// value; an unsynced widget can mutate it) plus the two MouseHit gates
		std::vector<CollisionVolume> selVol;
		std::vector<uint8_t> noSelect;       // CSolidObject::noSelect
		std::vector<uint8_t> inVoid;         // CSolidObject::IsInVoid()

		// PR 27a: remaining row-backable per-unit callout reads
		std::vector<uint8_t> isDead;         // CUnit::isDead (dying units stay valid, see the validity contract)
		std::vector<uint8_t> neutral;
		std::vector<uint8_t> activated;
		std::vector<uint8_t> isCloaked;
		std::vector<uint8_t> armoredState;
		std::vector<float> armoredMultiple;
		std::vector<int16_t> heading;        // CSolidObject::heading (SyncedSshort)
		std::vector<int16_t> buildFacing;    // CSolidObject::buildFacing (SyncedSshort)
		std::vector<float> height;
		std::vector<float> mass;
		std::vector<float> maxRange;
		std::vector<float> seismicSignature;
		std::vector<float> experience;
		std::vector<float> limExperience;
		std::vector<int32_t> selfDCountdown;
		std::vector<int32_t> losRadius;
		std::vector<int32_t> airLosRadius;
		std::vector<int32_t> radarRadius;
		std::vector<int32_t> sonarRadius;
		std::vector<int32_t> seismicRadius;
		std::vector<int32_t> jammerRadius;
		std::vector<int32_t> sonarJamRadius;
		std::vector<int32_t> moveDefID;      // moveDef ? pathType : -1 (the name is immutable MoveDef data)
		std::vector<SResourcePack> resourcesMake;
		std::vector<SResourcePack> resourcesUse;
		std::vector<SResourcePack> harvested;
		std::vector<SResourcePack> harvestStorage;
		std::vector<SResourcePack> cost;
		std::vector<float> buildTime;
		// GetSolidObjectBlocking's seven pushed booleans, bit i = push slot i:
		// blocking, solidObjectsCollidable, projectilesCollidable,
		// raySegmentsCollidable, crushable, blockEnemyPushing, blockHeightChanges
		std::vector<uint8_t> blockingBits;

		// object-space basis + relative midpoint (drawer midpos math, GetUnitVectors-class reads)
		std::vector<float3> relMidPos;
		std::vector<float3> frontdir;
		std::vector<float3> updir;
		std::vector<float3> rightdir;

		// masking inputs (see the masking-policy block above)
		std::vector<float3> posErrorVector;
		std::vector<uint8_t> leavesGhost;
		std::vector<uint8_t> losStatusAll;   // [numAllyTeams * maxUnits], row-major by allyteam
		std::vector<uint8_t> posErrorBits;   // same layout; CUnit::GetPosErrorBit(at)
		// picking (PR 25): the per-allyteam InRadar answer (GuiTraceRay's radar
		// gate). InRadar folds sonar/jammer/water logic, so -- like the
		// projectile/feature inLosAll rows -- we store the computed answer, not
		// the inputs. Same [numAllyTeams * maxUnits] stride layout.
		std::vector<uint8_t> inRadarAll;

		// ================= PR 32 (deep per-unit state) BEGIN =================
		// Deep per-unit reads: GetUnitStates, GetUnitStorage, GetUnitMetalExtraction,
		// GetUnitBuildeeRadius, GetUnitPosErrorParams, GetUnitLastAttacker, the
		// build-state family (GetUnitIsBuilding/BuildParams/InBuildStance/
		// CurrentBuildPower/EffectiveBuildRange/NanoPieces), the transport pair
		// (GetUnitTransporter/IsTransporting), GetUnitTooltip, GetUnitMoveTypeData.
		// All synced sim state (moveType/CAI/second-object derefs) captured by
		// value at extraction; the twins never touch a CUnit*.
		// -- GetUnitStates (ParseAllyUnit) --
		std::vector<int32_t> fireState;
		std::vector<int32_t> moveState;
		std::vector<float> repairBelowHealth;   // CMobileCAI::repairBelowHealth, -1 if not a CMobileCAI
		std::vector<uint8_t> repeatOrders;       // commandAI->repeatOrders
		std::vector<uint8_t> wantCloak;
		std::vector<uint8_t> useHighTrajectory;
		// -- GetUnitStorage / MetalExtraction / BuildeeRadius (ParseAlly/Typed) --
		std::vector<SResourcePack> storage;
		std::vector<float> metalExtract;
		std::vector<float> buildeeRadius;
		// -- GetUnitPosErrorParams (posErrorVector + posErrorBits already exist) --
		std::vector<float3> posErrorDelta;
		std::vector<int32_t> nextPosErrorUpdate;
		// -- GetUnitLastAttacker (visibility of the attacker gated in the twin) --
		std::vector<int32_t> lastAttackerID;     // -1 = none
		// -- GetUnitTransporter --
		std::vector<int32_t> transporterID;      // -1 = none
		// -- build-state family --
		// 0 = neither builder nor factory, 1 = CBuilder, 2 = CFactory
		std::vector<uint8_t> builderKind;
		std::vector<int32_t> curBuildID;         // builder/factory curBuild->id, -1 = none
		std::vector<float> buildDistance;        // CBuilder::buildDistance (0 if !builder)
		std::vector<uint8_t> range3D;            // CBuilder::range3D
		std::vector<uint8_t> inBuildStance;      // CUnit::inBuildStance (returned only for builders)
		std::vector<float> buildPower;           // NanoPieceCache::GetBuildPower() (builder|factory)
		// -- GetUnitTooltip custom string (unitToolTipMap.Get(id)) --
		std::vector<std::string> customTooltip;
		// -- GetUnitMoveTypeData base fields (flat; deep fields in moveTypeBlock) --
		// 0 = other/unknown, 1 = ground, 2 = hover-air, 3 = strafe-air, 4 = static, 5 = script
		std::vector<uint8_t> moveTypeKind;
		std::vector<float> mtMaxSpeed;           // GetMaxSpeed() * GAME_SPEED
		std::vector<float> mtMaxWantedSpeed;     // GetMaxWantedSpeed() * GAME_SPEED
		std::vector<float3> mtGoalPos;
		std::vector<uint8_t> mtProgressState;    // 0 done, 1 active, 2 failed
		std::vector<uint8_t> mtAutoLand;         // hover/strafe autoLand (GetUnitStates AMT branch)
		std::vector<uint8_t> mtLoopbackAttack;   // strafe loopbackAttack (0 for hover)
		std::vector<MoveTypeBlock> moveTypeBlock;
		// -- variable-size per-unit blocks (bounded: model/capacity-fixed) --
		std::vector<std::vector<int32_t>> nanoPieces;   // NanoPieceCache::GetNanoPieces() (model-fixed)
		std::vector<std::vector<int32_t>> transportees; // transportedUnits ids (capacity-bounded)
		// -- IsUnitInLos/InAirLos/InJammer unit variants (batch-1 reassignment):
		// the computed per-(unit,allyteam) answer, exactly like inRadarAll (the
		// gates fold cloak/stealth/water/globalLOS logic, so we store the answer
		// losHandler->InLos/InAirLos/InJammer(unit, at), not the inputs). Same
		// [numAllyTeams * maxUnits] stride layout.
		std::vector<uint8_t> unitInLosAll;
		std::vector<uint8_t> unitInAirLosAll;
		std::vector<uint8_t> unitInJammerAll;
		// ================== PR 32 (deep per-unit state) END ==================

		// ===== PR 38c (zero-sanction flip): per-unit rules-params serving =====
		// Per-unit LuaRulesParams::Params (CUnit::modParams, via CSolidObject)
		// mirror, serving GetUnitRulesParam/GetUnitRulesParams from draw context.
		// EXTENDS PR 38 part 1's game+team mechanism to the per-object namespace:
		// a plain per-boundary FULL copy indexed by unitID, exactly like the
		// team modParams copy (values only -- the Param variant is bool/float/
		// std::string, no pointer or sim-owned container). A full copy has NO
		// id-reuse ordering hazard (the deferral note only applied to an
		// incremental RenderEventQueue-ordered DELTA scheme): each boundary the
		// mirror is the current modParams of whatever unit holds the id, so a
		// died-then-reused id just reflects the new unit next boundary. SYNCED
		// state (Spring.SetUnitRulesParam is synced ctrl) but EXCLUDED from the
		// SnapshotHash like sideName/customOpts/statHistory (an order-independent
		// fold buys only marginal desync localization); correctness is covered by
		// the SnapshotDiffGate unit:rules field pass + the serving dual-run.
		std::vector<LuaRulesParams::Params> unitRulesParams; // [maxUnits]

		// ===== PR 38g (Batch-4 P1): GetUnitEstimatedPath serving =====
		// The unit's own estimated path waypoints, exactly as
		// LuaPathFinder::PushPathNodes reads them from pathManager->GetPathWayPoints
		// (a pure CONST read -- unlike PathFinder::Next it does NOT advance/mutate
		// the path). Captured per boundary for ground-move units with an active
		// pathID (moveTypeKind==1 && pathID!=0). Variable-size synced state, so --
		// like nanoPieces/transportees -- it is EXCLUDED from the SnapshotHash
		// (an order-dependent path fold buys only marginal desync localization; any
		// path divergence is preceded by a hashed goalPos/moveType divergence, and
		// the demo-stream sync hash is the real detector) and covered instead by the
		// SnapshotDiffGate unit:estPath field pass + the serving dual-run. points is
		// the concatenated max/med/low-res waypoint list; starts holds the 3 segment
		// offsets (mirrors the live vectors<float3>/vector<int>). hasPath==0 (pathID
		// 0 or non-ground) reproduces PushPathNodes' 0-return (no tables).
		std::vector<uint8_t> estPathHasPath;            // [maxUnits]; 1 iff ground move type with pathID!=0
		std::vector<std::vector<float3>> estPathPoints; // [maxUnits]; GetPathWayPoints points
		std::vector<std::vector<int32_t>> estPathStarts;// [maxUnits]; GetPathWayPoints segment starts

		// out-of-range ids (including any id before the first extraction ever
		// ran, when the arrays are still unsized) are part of the stale/nil
		// contract: a deterministic miss, not an error
		bool Valid(int unitID) const {
			if (static_cast<size_t>(unitID) >= valid.size())
				return false;
			const uint8_t v = valid[unitID];
			return (v == SimSnapshotValid::ACTIVE) ||
			       (v == SimSnapshotValid::DEAD_THIS_BATCH && SimDrawSplit::BoundaryDrainWindowActive());
		}

		size_t MaxUnits() const { return valid.size(); }

		// accessors return a deterministic default for invalid ids (stale/nil contract)
		uint8_t LosStatus(int unitID, int argAllyTeam) const {
			return (Valid(unitID) && argAllyTeam >= 0 && argAllyTeam < numAllyTeams) ?
				losStatusAll[argAllyTeam * MaxUnits() + unitID] : 0;
		}
		bool InRadar(int unitID, int argAllyTeam) const {
			return (Valid(unitID) && argAllyTeam >= 0 && argAllyTeam < numAllyTeams) &&
				inRadarAll[argAllyTeam * MaxUnits() + unitID] != 0;
		}
		float3 Pos(int unitID) const { return Valid(unitID) ? pos[unitID] : float3{}; }
		float3 MidPos(int unitID) const { return Valid(unitID) ? midPos[unitID] : float3{}; }
		float4 Speed(int unitID) const { return Valid(unitID) ? speed[unitID] : float4{}; }
		float Health(int unitID) const { return Valid(unitID) ? health[unitID] : 0.0f; }
		float MaxHealth(int unitID) const { return Valid(unitID) ? maxHealth[unitID] : 0.0f; }
		int Team(int unitID) const { return Valid(unitID) ? team[unitID] : -1; }
		int AllyTeam(int unitID) const { return Valid(unitID) ? allyTeam[unitID] : -1; }
		int DefID(int unitID) const { return Valid(unitID) ? defID[unitID] : 0; }
		float BuildProgress(int unitID) const { return Valid(unitID) ? buildProgress[unitID] : 0.0f; }
		float Radius(int unitID) const { return Valid(unitID) ? radius[unitID] : 0.0f; }
		// object-space basis vectors (drawer-midpos / camera-orientation consumers, PR 24)
		float3 Frontdir(int unitID) const { return Valid(unitID) ? frontdir[unitID] : float3{}; }
		float3 Updir(int unitID) const { return Valid(unitID) ? updir[unitID] : float3{}; }
		float3 Rightdir(int unitID) const { return Valid(unitID) ? rightdir[unitID] : float3{}; }

		// picking accessors (PR 25); stale/nil contract: invalid ids read the
		// default-constructed sphere volume / no-select / not-in-void
		bool NoSelect(int unitID) const { return Valid(unitID) && noSelect[unitID] != 0; }
		bool InVoid(int unitID) const { return Valid(unitID) && inVoid[unitID] != 0; }
		const CollisionVolume& SelVol(int unitID) const {
			static const CollisionVolume def;
			return Valid(unitID) ? selVol[unitID] : def;
		}

		// PR 27a accessors (stale/nil contract defaults)
		bool IsDead(int unitID) const { return Valid(unitID) && isDead[unitID] != 0; }
		bool Neutral(int unitID) const { return Valid(unitID) && neutral[unitID] != 0; }
		bool Activated(int unitID) const { return Valid(unitID) && activated[unitID] != 0; }
		bool IsCloaked(int unitID) const { return Valid(unitID) && isCloaked[unitID] != 0; }
		bool ArmoredState(int unitID) const { return Valid(unitID) && armoredState[unitID] != 0; }
		float ArmoredMultiple(int unitID) const { return Valid(unitID) ? armoredMultiple[unitID] : 0.0f; }
		int Heading(int unitID) const { return Valid(unitID) ? heading[unitID] : 0; }
		int BuildFacing(int unitID) const { return Valid(unitID) ? buildFacing[unitID] : 0; }
		float Height(int unitID) const { return Valid(unitID) ? height[unitID] : 0.0f; }
		float Mass(int unitID) const { return Valid(unitID) ? mass[unitID] : 0.0f; }
		float MaxRange(int unitID) const { return Valid(unitID) ? maxRange[unitID] : 0.0f; }
		float SeismicSignature(int unitID) const { return Valid(unitID) ? seismicSignature[unitID] : 0.0f; }
		float Experience(int unitID) const { return Valid(unitID) ? experience[unitID] : 0.0f; }
		float LimExperience(int unitID) const { return Valid(unitID) ? limExperience[unitID] : 0.0f; }
		int SelfDCountdown(int unitID) const { return Valid(unitID) ? selfDCountdown[unitID] : 0; }
		int LosRadius(int unitID) const { return Valid(unitID) ? losRadius[unitID] : 0; }
		int AirLosRadius(int unitID) const { return Valid(unitID) ? airLosRadius[unitID] : 0; }
		int RadarRadius(int unitID) const { return Valid(unitID) ? radarRadius[unitID] : 0; }
		int SonarRadius(int unitID) const { return Valid(unitID) ? sonarRadius[unitID] : 0; }
		int SeismicRadius(int unitID) const { return Valid(unitID) ? seismicRadius[unitID] : 0; }
		int JammerRadius(int unitID) const { return Valid(unitID) ? jammerRadius[unitID] : 0; }
		int SonarJamRadius(int unitID) const { return Valid(unitID) ? sonarJamRadius[unitID] : 0; }
		// -1 doubles as the "no moveDef" encoding the live body maps to false
		int MoveDefID(int unitID) const { return Valid(unitID) ? moveDefID[unitID] : -1; }
		SResourcePack ResourcesMake(int unitID) const { return Valid(unitID) ? resourcesMake[unitID] : SResourcePack{}; }
		SResourcePack ResourcesUse(int unitID) const { return Valid(unitID) ? resourcesUse[unitID] : SResourcePack{}; }
		SResourcePack Harvested(int unitID) const { return Valid(unitID) ? harvested[unitID] : SResourcePack{}; }
		SResourcePack HarvestStorage(int unitID) const { return Valid(unitID) ? harvestStorage[unitID] : SResourcePack{}; }
		SResourcePack Cost(int unitID) const { return Valid(unitID) ? cost[unitID] : SResourcePack{}; }
		float BuildTime(int unitID) const { return Valid(unitID) ? buildTime[unitID] : 0.0f; }
		uint8_t BlockingBits(int unitID) const { return Valid(unitID) ? blockingBits[unitID] : uint8_t(0); }

		// PR 32 LOS-variant accessors: the computed per-(unit,allyteam) answer
		// (stale/nil contract: invalid ids / out-of-range allyteams read false).
		// Callers mirror the live IsUnitInLos/InAirLos/InJammer bodies exactly.
		bool UnitInLos(int unitID, int argAllyTeam) const {
			return (Valid(unitID) && argAllyTeam >= 0 && argAllyTeam < numAllyTeams) &&
				unitInLosAll[argAllyTeam * MaxUnits() + unitID] != 0;
		}
		bool UnitInAirLos(int unitID, int argAllyTeam) const {
			return (Valid(unitID) && argAllyTeam >= 0 && argAllyTeam < numAllyTeams) &&
				unitInAirLosAll[argAllyTeam * MaxUnits() + unitID] != 0;
		}
		bool UnitInJammer(int unitID, int argAllyTeam) const {
			return (Valid(unitID) && argAllyTeam >= 0 && argAllyTeam < numAllyTeams) &&
				unitInJammerAll[argAllyTeam * MaxUnits() + unitID] != 0;
		}

		// ---- serving-layer masking helpers (PR 18) ----
		// Bit-for-bit mirrors of the live formulas, computed from extracted
		// inputs only; the Lua serving twins (LuaSnapshotServe) and the diff
		// gate both call these. readAllyTeam/fullRead are the handle POV
		// (CLuaHandle::GetHandleReadAllyTeam/GetHandleFullRead).

		// teamHandler.Ally(a, b) mirror; out-of-range reads false
		bool Allied(int a, int b) const {
			return (a >= 0 && a < numAllyTeams && b >= 0 && b < numAllyTeams &&
				allied[a * numAllyTeams + b] != 0);
		}
		// LuaUtils::IsAlliedAllyTeam / IsAllyUnit mirror; caller must have
		// checked Valid(unitID)
		bool PovAlliedUnit(int unitID, int readAllyTeam, bool fullRead) const {
			if (readAllyTeam < 0)
				return fullRead;
			return (allyTeam[unitID] == readAllyTeam);
		}
		// LuaUtils::IsUnitVisible / IsUnitInLos / IsUnitTyped mirrors. A
		// readAllyTeam that is negative without fullRead indexes losStatus out
		// of bounds on the live path (cannot arise for real handles); here it
		// reads as a deterministic not-visible.
		bool PovUnitVisible(int unitID, int readAllyTeam, bool fullRead) const;
		bool PovUnitInLos(int unitID, int readAllyTeam, bool fullRead) const;
		bool PovUnitTyped(int unitID, int readAllyTeam, bool fullRead) const;

		// CUnit::GetErrorVector / GetLuaErrorVector mirrors
		float3 ErrorVector(int unitID, int argAllyTeam) const;
		float3 LuaErrorVector(int unitID, int readAllyTeam, bool fullRead) const {
			return (fullRead ? float3{0.0f, 0.0f, 0.0f} : ErrorVector(unitID, readAllyTeam));
		}
		// CSolidObject::GetObjectSpaceVec mirror
		float3 ObjectSpaceVec(int unitID, const float3& v) const {
			return ((frontdir[unitID] * v.z) + (rightdir[unitID] * v.x) + (updir[unitID] * v.y));
		}

		// ================= PR 31: weapon/shield scalar family =================
		// Per-unit weapon-family scalars + a flat per-weapon SoA (bounded:
		// numWeapons is def-fixed). Keyed off the same unitID space and the same
		// UnitRows::Valid()/simFrame/POV as the rest of the struct -- the weapon
		// twins gate on the unit's ParseAllyUnit/ParseInLosUnit visibility, so no
		// separate namespace or validity row is needed. The flat arrays are
		// indexed weaponOffset[unitID] + weaponNum and sized (in Extract, not
		// Resize -- the total depends on the live weapon count) to the sum of
		// weaponCount over live units. Serves GetUnitWeaponState/Damages/Vectors/
		// Target/CanFire, GetUnitShieldState, GetUnitStockpile, GetUnitFlanking.
		// Trace tests (TryTarget/TestTarget/TestRange/HaveFreeLineOfFire) are NOT
		// here -- they recompute against the published collision world in PR 35.
		//
		// DynDamageArray is Lua-mutable (Spring.SetUnitWeaponDamages ->
		// DynDamageArray::GetMutable clones a per-weapon heap array), so a raw
		// shared pointer is neither immutable nor lifetime-stable across the
		// boundary under the split. Rather than refcount-share a delicate
		// member/heap-hybrid type across the double buffer + hash scratch, the
		// damage arrays are FLATTENED into POD (numTypes floats + the scalar
		// fields) -- the command-queue-flatten discipline (don't hold sim-owned
		// mutable storage across the boundary). The float vector reuses capacity
		// across boundaries (numArmorTypes is game-fixed), so steady state is a
		// memcpy. valid==0 reproduces the live "damages == nullptr" nil shape.
		struct DamagesSnap {
			uint8_t valid = 0;              // 1 iff the source DynDamageArray* was non-null
			int32_t paralyzeDamageTime = 0;
			float impulseFactor = 0.0f;
			float impulseBoost = 0.0f;
			float craterMult = 0.0f;
			float craterBoost = 0.0f;
			float dynDamageExp = 0.0f;
			float dynDamageMin = 0.0f;
			float dynDamageRange = 0.0f;
			uint8_t dynDamageInverted = 0;
			float craterAreaOfEffect = 0.0f;
			float damageAreaOfEffect = 0.0f;
			float edgeEffectiveness = 0.0f;
			float explosionSpeed = 0.0f;
			std::vector<float> damages;    // per armor type (Get(i) / GetNumTypes())
		};

		// per-unit weapon-family rows (sized MaxUnits in Resize; only set for
		// live units, read only behind a Valid()+POV gate like every other field)
		std::vector<int32_t> weaponOffset;   // start index into the flat arrays
		std::vector<int32_t> weaponCount;    // unit->weapons.size()
		std::vector<float> reloadSpeed;      // unit->reloadSpeed (WeaponState reloadTimeXP)
		std::vector<uint8_t> fpsNoFire;      // CanFire fps gate: fpsControlPlayer && !mouse1 && !mouse2
		// GetUnitFlanking (all per-unit)
		std::vector<int32_t> flankingMode;
		std::vector<float3> flankingDir;
		std::vector<float> flankingMoveFactor;  // flankingBonusMobilityAdd
		std::vector<float> flankingAvgDamage;
		std::vector<float> flankingDifDamage;
		std::vector<float> flankingMobility;    // flankingBonusMobility
		// GetUnitStockpile (unit->stockpileWeapon; hasStockpile==0 => nil shape)
		std::vector<uint8_t> hasStockpile;
		std::vector<int32_t> stockpileNumStockpiled;
		std::vector<int32_t> stockpileNumQueued;
		std::vector<float> stockpileBuildPercent;
		// GetUnitShieldState default case (unit->shieldWeapon; static_cast in the
		// live path, so a non-null shieldWeapon is always a CPlasmaRepulser)
		std::vector<uint8_t> hasShieldWeapon;
		std::vector<uint8_t> shieldWeaponEnabled;
		std::vector<float> shieldWeaponPower;
		// GetUnitWeaponDamages explosion arrays (unit-level; flattened POD)
		std::vector<DamagesSnap> deathExpDamages;
		std::vector<DamagesSnap> selfdExpDamages;

		// flat per-weapon arrays (index = weaponOffset[unitID] + weaponNum; sized
		// to the total live weapon count in Extract). GetUnitWeaponState:
		std::vector<uint8_t> wAngleGood;
		std::vector<int32_t> wReloadStatus;
		std::vector<int32_t> wSalvoLeft;
		std::vector<int32_t> wNumStockpiled;
		std::vector<int32_t> wNextSalvo;
		std::vector<int32_t> wReloadTime;
		std::vector<int32_t> wReaimTime;
		std::vector<float> wAccuracyExp;      // AccuracyExperience()
		std::vector<float> wSprayAngleExp;    // SprayAngleExperience()
		std::vector<float3> wSalvoError;      // SalvoErrorExperience()
		std::vector<float> wMoveErrorExp;     // MoveErrorExperience()
		std::vector<float> wRange;
		std::vector<float> wProjectileSpeed;
		std::vector<float> wAutoTargetRangeBoost;
		std::vector<int32_t> wSalvoSize;
		std::vector<int32_t> wSalvoDelay;
		std::vector<int32_t> wSalvoWindup;
		std::vector<int32_t> wProjectilesPerShot;
		std::vector<uint32_t> wAvoidFlags;
		std::vector<uint32_t> wCollisionFlags;
		std::vector<int32_t> wTtl;
		// GetUnitWeaponVectors:
		std::vector<float3> wMuzzlePos;       // weaponMuzzlePos
		std::vector<float3> wWantedDir;
		std::vector<float3> wWeaponDir;
		std::vector<int32_t> wProjectileType; // weaponDef->projectileType (Vectors dir switch)
		// GetUnitWeaponCanFire inputs (immutable def scalars copied by value +
		// the per-weapon runtime state; the frame comparisons use rows.simFrame):
		std::vector<uint8_t> wDefStockpile;
		std::vector<uint8_t> wDefFireSubmersed;
		std::vector<float> wDefMaxFireAngle;
		std::vector<uint8_t> wIsBombDropper;  // CBombDropper::CanFire override (ignoreAngleGood/RequestedDir)
		std::vector<float> wAimFromPosY;
		std::vector<float3> wLastRequestedDir;
		// GetUnitWeaponTarget (SWeaponTarget):
		std::vector<uint8_t> wTargetType;     // TargetType 0 none / 1 unit / 2 pos / 3 intercept
		std::vector<uint8_t> wTargetIsUser;
		std::vector<int32_t> wTargetUnitID;
		std::vector<float3> wTargetGroundPos;
		std::vector<int32_t> wTargetInterceptID;
		// GetUnitShieldState explicit-weapon case (dynamic_cast in the live path):
		std::vector<uint8_t> wIsShield;
		std::vector<uint8_t> wShieldEnabled;
		std::vector<float> wShieldPower;
		// GetUnitWeaponDamages per-weapon (flattened POD):
		std::vector<DamagesSnap> wDamages;
	};

	/**
	 * Synced-projectile rows (second callout family). Keyed by synced
	 * projectile id; slots grow-only to the max id seen (ids are free-list
	 * ints bounded by the high-water concurrent count). Same validity and
	 * stale/nil contract as UnitRows. Masking input is inLosAll -- the
	 * positional LOS-map answer losHandler->InLos(pro->pos, allyTeam)
	 * captured at extraction for every allyteam -- plus allyTeam for the
	 * own-projectile bypass; PovVisible mirrors LuaUtils::IsProjectileVisible.
	 */
	struct ProjectileRows {
		int32_t numAllyTeams = 0;

		std::vector<uint8_t> valid;
		std::vector<float3> pos;
		std::vector<float4> speed;
		std::vector<int32_t> allyTeam;    // CProjectile::GetAllyteamID(); may be -1
		std::vector<int32_t> ownerID;     // CProjectile::GetOwnerID(), raw (serving replicates the range check)
		std::vector<uint8_t> isWeapon;    // CProjectile::weapon
		std::vector<int32_t> weaponDefID; // -1 = no WeaponDef (nil shape); only meaningful when isWeapon
		std::vector<uint8_t> targetType;  // 0 = none/not-a-weapon; 'g'/'u'/'f'/'p' as in GetProjectileTarget
		std::vector<int32_t> targetID;    // for 'u'/'f'/'p'
		std::vector<float3> targetPos;    // for 'g'
		// PR 27a: remaining row-backable per-projectile callout reads
		std::vector<uint8_t> isPiece;     // CProjectile::piece
		std::vector<float3> dir;
		std::vector<float> mygravity;
		std::vector<int32_t> teamID;      // CProjectile::GetTeamID()
		std::vector<int32_t> ttl;         // CWeaponProjectile::GetTimeToLive(); 0 when !isWeapon
		std::vector<uint8_t> intercepted; // CWeaponProjectile::IsBeingIntercepted(); 0 when !isWeapon
		// ---- PR 33 (piece/script family): CPieceProjectile params ----
		// GetPieceProjectileParams/Name reads. explFlags/spinSpeed/spinVec are
		// creation-fixed, spinAngle animates per frame; all are synced state
		// (CPieceProjectile is spawned with isSynced=true). pieceName mirrors
		// ppro->omp->name (the spawning model piece); empty when !isPiece.
		std::vector<int32_t> pieceExplFlags; // 0 when !isPiece
		std::vector<float> pieceSpinAngle;
		std::vector<float> pieceSpinSpeed;
		std::vector<float3> pieceSpinVec;
		std::vector<std::string> pieceName;  // empty when !isPiece
		// PR 34 (spatial/list remainder): CProjectile::radius, the sphere-test
		// input GetProjectilesInSphere's quadfield filter reads
		// (pos.SqDistance(p->pos) >= Square(radius + p->radius))
		std::vector<float> radius;
		std::vector<uint8_t> inLosAll;    // [numAllyTeams * MaxSlots()], row-major by allyteam

		// ===== PR 38g (Batch-4 P1): GetProjectileDamages serving =====
		// The weapon projectile's *wpro->damages (DynDamageArray) flattened into the
		// POD DamagesSnap -- the SAME flatten discipline (and CopyDamages helper) the
		// PR-31 unit weapon damages use (DynDamageArray is a member/heap hybrid that
		// is neither immutable nor lifetime-stable across the boundary, so it is
		// flattened, not pointer-shared). valid==0 for non-weapon projectiles (the
		// twin gates on isWeapon first, exactly like the live body). NOT hashed --
		// same as the unit wDamages precedent (weaponDef-derived; weaponDefID IS
		// hashed) -- covered by the SnapshotDiffGate proj:damages field pass + the
		// serving dual-run.
		std::vector<UnitRows::DamagesSnap> damages; // [MaxSlots()]

		// ===== PR 41 (GetVisibleProjectiles serving) =====
		// drawRadius: the projectile's draw-authored cull state (p->GetDrawRadius()),
		// the radius arg of the live camera->InView(p->pos, p->GetDrawRadius()) filter.
		// Draw-authored (mutable CWorldObject::drawRadius), but the ONLY draw-rate
		// writer is CBitmapMuzzleFlame::Draw -- an UNSYNCED projectile, which the
		// callout filters out (!p->synced). Every SYNCED projectile sets drawRadius
		// only in its ctor / sim-rate Update(), so a sim-boundary snapshot equals the
		// call-time live value bit-for-bit. It is a snapshot row (not a drawer-owned
		// array like the PR-40 unit/feature drawRadius) because projectiles have no
		// id-keyed drawer draw-radius store -- drawRadius lives on the sim object.
		std::vector<float> drawRadius;
		// hitscan: CProjectile::hitscan (synced, CR_MEMBER). Selects the quad-
		// membership rule mirroring CQuadField::AddProjectile: a hitscan projectile
		// keys a RAY (GetQuadsOnRay(pos, dir, speed.w)); a non-hitscan projectile keys
		// the SINGLE cell WorldPosToQuadFieldIdx(pos). (speed.w = ray length is
		// already in the float4 `speed` row; dir already exists above.)
		std::vector<uint8_t> hitscan;
		// visInLosAll: [numAllyTeams * MaxSlots()], row-major by allyteam. The
		// extraction-time answer of losHandler->InLos(p, at) -- the CWorldObject*
		// overload (alwaysVisible / useAirLos / two-position beam test) the callout's
		// LOS filter uses. DISTINCT from inLosAll above, which is the positional
		// losHandler->InLos(p->pos, at) single-point overload (a different function).
		std::vector<uint8_t> visInLosAll;

		bool Valid(int projID) const {
			if (static_cast<size_t>(projID) >= valid.size())
				return false;
			const uint8_t v = valid[projID];
			return (v == SimSnapshotValid::ACTIVE) ||
			       (v == SimSnapshotValid::DEAD_THIS_BATCH && SimDrawSplit::BoundaryDrainWindowActive());
		}
		size_t MaxSlots() const { return valid.size(); }
		// PR 34 stale/nil contract default (invalid ids read 0)
		float Radius(int projID) const { return Valid(projID) ? radius[projID] : 0.0f; }

		bool InLos(int projID, int argAllyTeam) const {
			return (argAllyTeam >= 0 && argAllyTeam < numAllyTeams &&
				inLosAll[argAllyTeam * MaxSlots() + projID] != 0);
		}
		// PR 41: GetVisibleProjectiles LOS filter -- the losHandler->InLos(p, at)
		// (CWorldObject* overload) answer, distinct from the positional InLos above
		bool VisInLos(int projID, int argAllyTeam) const {
			return (argAllyTeam >= 0 && argAllyTeam < numAllyTeams &&
				visInLosAll[argAllyTeam * MaxSlots() + projID] != 0);
		}
		// LuaUtils::IsProjectileVisible mirror; caller must have checked Valid()
		bool PovVisible(int projID, int readAllyTeam, bool fullRead) const {
			if (readAllyTeam < 0)
				return fullRead;
			return !((readAllyTeam != allyTeam[projID]) && !InLos(projID, readAllyTeam));
		}
	};

	/**
	 * Feature rows (PR 25, first non-unit/non-projectile family). Keyed by
	 * feature id; ids are dense but sparse-occupancy (SimObjectIDPool, bounded
	 * by MAX_FEATURES), so rows grow-only to the max id seen exactly like the
	 * projectile rows. Carries the GuiTraceRay feature read set. Feature
	 * visibility is positional like projectiles (losHandler->InLos(pos, at))
	 * but with CFeature::IsInLosForAllyTeam's mod-config branches, so inLosAll
	 * holds the per-allyteam positional-LOS answer and the two globals
	 * (featureVisibility, gaiaAllyTeam) drive the InLosForAllyTeam mirror.
	 */
	struct FeatureRows {
		int32_t numAllyTeams = 0;
		int32_t featureVisibility = 0;  // CModInfo::featureVisibility (FEATURELOS_*)
		int32_t gaiaAllyTeam = 0;       // std::max(0, teamHandler.GaiaAllyTeamID())

		std::vector<uint8_t> valid;
		std::vector<float3> pos;
		std::vector<float3> midPos;
		// aimPos can diverge from midPos (Spring.SetFeatureMidAndAimPos); the
		// GetFeaturePosition twin's optional third return needs the real value
		std::vector<float3> aimPos;
		std::vector<float3> relMidPos;
		std::vector<float> radius;
		std::vector<int32_t> allyTeam;   // CFeature::allyteam (may be -1)
		std::vector<int32_t> defID;
		std::vector<uint8_t> alwaysVisible;
		std::vector<uint8_t> noSelect;
		std::vector<uint8_t> inVoid;
		std::vector<CollisionVolume> selVol;
		// PR 27a: remaining row-backable per-feature callout reads
		std::vector<int32_t> team;           // CSolidObject::team
		std::vector<float> health;
		std::vector<float> resurrectProgress;
		std::vector<float> height;
		std::vector<float> mass;
		std::vector<float4> speed;
		// CFeature::transMatrix direction columns, exactly as GetFeatureDirection
		// reads them (front = Z, right = X, up = Y)
		std::vector<float3> matXdir;
		std::vector<float3> matYdir;
		std::vector<float3> matZdir;
		std::vector<int16_t> heading;        // CSolidObject::heading (SyncedSshort)
		std::vector<int16_t> buildFacing;    // CSolidObject::buildFacing (SyncedSshort)
		std::vector<SResourcePack> resources;
		std::vector<SResourcePack> defResources;
		std::vector<float> reclaimLeft;
		std::vector<float> reclaimTime;
		std::vector<uint8_t> blockingBits;   // same bit layout as UnitRows::blockingBits
		std::vector<int32_t> resurrectDefID; // udef ? udef->id : -1 (the name is immutable UnitDef data)
		// ===== PR 38g (Batch-4 P1): GetFeatureFireTime/GetFeatureSmokeTime =====
		// CFeature::fireTime/smokeTime (int frame counts); the twins push
		// value * INV_GAME_SPEED exactly like the live bodies. Synced state, hashed.
		std::vector<int32_t> fireTime;   // CFeature::fireTime
		std::vector<int32_t> smokeTime;  // CFeature::smokeTime
		std::vector<uint8_t> inLosAll;   // [numAllyTeams * MaxSlots()], row-major by allyteam

		// ===== PR 38c (zero-sanction flip): per-feature rules-params serving =====
		// Per-feature LuaRulesParams::Params (CFeature::modParams, via
		// CSolidObject) mirror, serving GetFeatureRulesParam/GetFeatureRulesParams
		// from draw context. Same plain per-boundary FULL copy (indexed by feature
		// id) as the per-unit mirror above -- see the UnitRows comment for the
		// id-reuse-hazard and SnapshotHash-exclusion rationale.
		std::vector<LuaRulesParams::Params> featureRulesParams; // [MaxSlots()]

		bool Valid(int id) const {
			if (static_cast<size_t>(id) >= valid.size())
				return false;
			const uint8_t v = valid[id];
			return (v == SimSnapshotValid::ACTIVE) ||
			       (v == SimSnapshotValid::DEAD_THIS_BATCH && SimDrawSplit::BoundaryDrainWindowActive());
		}
		size_t MaxSlots() const { return valid.size(); }

		float3 Pos(int id) const { return Valid(id) ? pos[id] : float3{}; }
		float3 MidPos(int id) const { return Valid(id) ? midPos[id] : float3{}; }
		float3 AimPos(int id) const { return Valid(id) ? aimPos[id] : float3{}; }
		float Radius(int id) const { return Valid(id) ? radius[id] : 0.0f; }
		int AllyTeam(int id) const { return Valid(id) ? allyTeam[id] : -1; }
		int DefID(int id) const { return Valid(id) ? defID[id] : 0; }
		bool NoSelect(int id) const { return Valid(id) && noSelect[id] != 0; }
		bool InVoid(int id) const { return Valid(id) && inVoid[id] != 0; }
		const CollisionVolume& SelVol(int id) const {
			static const CollisionVolume def;
			return Valid(id) ? selVol[id] : def;
		}

		// PR 27a accessors (stale/nil contract defaults)
		int Team(int id) const { return Valid(id) ? team[id] : -1; }
		float Health(int id) const { return Valid(id) ? health[id] : 0.0f; }
		float ResurrectProgress(int id) const { return Valid(id) ? resurrectProgress[id] : 0.0f; }
		float Height(int id) const { return Valid(id) ? height[id] : 0.0f; }
		float Mass(int id) const { return Valid(id) ? mass[id] : 0.0f; }
		float4 Speed(int id) const { return Valid(id) ? speed[id] : float4{}; }
		float3 MatXdir(int id) const { return Valid(id) ? matXdir[id] : float3{}; }
		float3 MatYdir(int id) const { return Valid(id) ? matYdir[id] : float3{}; }
		float3 MatZdir(int id) const { return Valid(id) ? matZdir[id] : float3{}; }
		int Heading(int id) const { return Valid(id) ? heading[id] : 0; }
		int BuildFacing(int id) const { return Valid(id) ? buildFacing[id] : 0; }
		SResourcePack Resources(int id) const { return Valid(id) ? resources[id] : SResourcePack{}; }
		SResourcePack DefResources(int id) const { return Valid(id) ? defResources[id] : SResourcePack{}; }
		float ReclaimLeft(int id) const { return Valid(id) ? reclaimLeft[id] : 0.0f; }
		float ReclaimTime(int id) const { return Valid(id) ? reclaimTime[id] : 0.0f; }
		uint8_t BlockingBits(int id) const { return Valid(id) ? blockingBits[id] : uint8_t(0); }
		int ResurrectDefID(int id) const { return Valid(id) ? resurrectDefID[id] : -1; }
		// PR 38g stale/nil contract defaults (invalid ids read 0)
		int FireTime(int id) const { return Valid(id) ? fireTime[id] : 0; }
		int SmokeTime(int id) const { return Valid(id) ? smokeTime[id] : 0; }

		bool InLos(int id, int argAllyTeam) const {
			return (argAllyTeam >= 0 && argAllyTeam < numAllyTeams &&
				inLosAll[argAllyTeam * MaxSlots() + id] != 0);
		}
		// CFeature::IsInLosForAllyTeam mirror; caller must have checked Valid()
		bool IsInLosForAllyTeam(int id, int argAllyTeam) const;
	};

	/**
	 * Team boundary copy (PR 26, the section-E.3 player/team field spec).
	 * Serves the (b)-tier team-table callouts (GetTeamInfo/GetTeamList/
	 * GetTeamResources/GetTeamUnitCount fast path/the TeamStatistics stats
	 * callouts/GetTeamColor/GetGaiaTeamID) via the LuaSnapshotServe twins.
	 * Indexed by teamID in [0, activeTeams); the team set is fixed at game
	 * start, so there is no validity row -- ValidTeam() mirrors
	 * teamHandler.IsValidTeam and team slots are never null.
	 *
	 * Unlike the unit/projectile/feature rows this copy is NOT due-checked:
	 * net messages mutate the tables *between* sim frames (share/resign
	 * transfers, PLAYERINFO ping/cpu at net rate -- the class of mutation
	 * MarkMutatedOutsideFrame() was added for), so Update() re-extracts it
	 * unconditionally every boundary; the whole copy is KBs.
	 *
	 * Deliberately NOT copied (class (d), stale/nil or dirty-versioned
	 * later): modParams (GetTeamRulesParams) and statHistory
	 * (GetTeamStatsHistory) -- unbounded containers.
	 */
	struct TeamRows {
		// global block
		int32_t activeTeams = 0;
		int32_t activeAllyTeams = 0;
		int32_t gaiaTeamID = -1;   // teamHandler.GaiaTeamID()
		uint8_t useLuaGaia = 0;    // gs->useLuaGaia (GetGaiaTeamID gate)
		uint8_t gameOver = 0;      // game->IsGameOver() (stats callouts' spectator gate)

		// per-team rows
		std::vector<int32_t> leader;
		std::vector<uint8_t> isDead;
		std::vector<uint8_t> hasAIs;             // skirmishAIHandler.HasSkirmishAIsInTeam
		std::vector<int32_t> allyTeam;           // teamHandler.AllyTeam(t)
		std::vector<float> incomeMultiplier;
		std::vector<int32_t> numUnits;           // unitHandler.NumUnitsByTeam(t)
		// UNSYNCED-mutable (Spring.SetTeamColor); excluded from SnapshotHash
		// like selVol, covered by the diff gate
		std::vector<std::array<uint8_t, 4>> color;
		std::vector<std::array<uint8_t, 4>> origColor;
		std::vector<std::string> sideName;
		std::vector<TeamStatistics> currentStats; // team->GetCurrentStats() (statHistory.back())
		// the GetTeamResources pack set, in its push order
		std::vector<SResourcePack> res;
		std::vector<SResourcePack> resStorage;
		std::vector<SResourcePack> resPrevPull;
		std::vector<SResourcePack> resPrevIncome;
		std::vector<SResourcePack> resPrevExpense;
		std::vector<SResourcePack> resShare;
		std::vector<SResourcePack> resPrevSent;
		std::vector<SResourcePack> resPrevReceived;
		std::vector<SResourcePack> resPrevExcess;
		std::vector<spring::unordered_map<std::string, std::string>> customOpts;

		// ---- PR 36 (pathing + team/player misc + misc tail): team/player misc ----
		// Serves GetTeamStartPosition/GetTeamMaxUnits/GetTeamLuaAI/GetAIInfo/
		// GetTeamStatsHistory (per-team) and GetAllyTeamStartBox/GetAllyTeamInfo
		// (per-allyteam). Same unconditional per-boundary re-extraction as the
		// rest of this struct. startPos/maxUnits are synced (hashed); the AI
		// short-name/version/options and luaAIName are the LOCAL machine's view
		// (GetAIInfo returns SYNCED_* for synced handles), so like sideName they
		// are excluded from the SnapshotHash.
		std::vector<float3> startPos;            // TeamBase::GetStartPos()
		std::vector<uint8_t> hasValidStartPos;   // TeamBase::HasValidStartPos()
		std::vector<int32_t> maxUnits;           // CTeam::GetMaxUnits()
		std::vector<uint8_t> hasLuaAI;           // GetTeamLuaAI: any isLuaAI in the team (distinguishes nil from an empty shortName)
		std::vector<std::string> luaAIName;      // GetTeamLuaAI: first isLuaAI shortName
		// GetAIInfo per-team first-AI block (teamAIs[0]); aiID/aiName/aiHostPlayer
		// are the "synced AI info" the live body pushes unconditionally,
		// aiIsLocal/aiShortName/aiVersion/aiOptions the local unsynced view
		std::vector<uint8_t> aiHasAI;            // !GetSkirmishAIsInTeam(t).empty()
		std::vector<int32_t> aiID;               // teamAIs[0]
		std::vector<std::string> aiName;
		std::vector<int32_t> aiHostPlayer;
		std::vector<uint8_t> aiIsLocal;          // skirmishAIHandler.IsLocalSkirmishAI
		std::vector<std::string> aiShortName;
		std::vector<std::string> aiVersion;
		std::vector<spring::unordered_map<std::string, std::string>> aiOptions;
		// GetTeamStatsHistory: the full append-only statHistory copy (grows only
		// at the stats interval). currentStats == statHistory.back() is already
		// its own row; the whole vector is copied so the range form is served.
		std::vector<std::vector<TeamStatistics>> statHistory;
		// per-allyteam global block (sized to activeAllyTeams). GetAllyTeamStartBox
		// stores the pre-computed corners in the live float-expression order
		// ((mapDims.mapx * SQUARE_SIZE) * startRect...), so the twin push is
		// bit-identical without a mapDims dependency; allyTeamOpts is the custom
		// options map GetAllyTeamInfo returns.
		std::vector<float4> allyStartBox;        // [activeAllyTeams] {xMin,zMin,xMax,zMax}
		std::vector<spring::unordered_map<std::string, std::string>> allyTeamOpts; // [activeAllyTeams]

		// ===== PR 38 (zero-sanction flip): game+team rules-params serving =====
		// Per-team LuaRulesParams::Params (CTeam::modParams) mirror, serving
		// GetTeamRulesParam/GetTeamRulesParams from draw context. Rules params are
		// SYNCED state (Spring.SetTeamRulesParam is synced ctrl), but -- like
		// sideName/customOpts/statHistory above -- they are EXCLUDED from the
		// SnapshotHash: an order-independent fold of an unordered_map<string,
		// variant> (string bytes + float bits) buys only marginal desync
		// localization (any unit-row or demo-stream divergence pinpoints better),
		// so correctness is covered instead by the SnapshotDiffGate field pass
		// (team:rules) + the serving dual-run. Re-copied unconditionally every
		// boundary like the rest of TeamRows. Teams are fixed-count and never
		// recreated mid-game, so there is NO id-reuse ordering hazard -- the
		// unit/feature/player rules-params namespaces (unit/feature ids DO reuse)
		// stay sanctioned and are deferred to the RenderEventQueue-ordered delta
		// mechanism (see the PR-38 escalation note in the commit message).
		std::vector<LuaRulesParams::Params> teamRulesParams; // [activeTeams]

		// teamHandler.IsValidTeam mirror
		bool ValidTeam(int teamID) const { return (teamID >= 0 && teamID < activeTeams); }
		// teamHandler.ValidAllyTeam mirror (the ally-team callouts' own gate)
		bool ValidAllyTeam(int allyTeamID) const { return (allyTeamID >= 0 && allyTeamID < activeAllyTeams); }

		// LuaUtils::IsAlliedTeam mirror; readAllyTeam/fullRead are the handle POV
		bool PovAlliedTeam(int teamID, int readAllyTeam, bool fullRead) const {
			if (readAllyTeam < 0)
				return fullRead;

			return (allyTeam[teamID] == readAllyTeam);
		}
	};

	/**
	 * Player boundary copy (PR 26, section E.3). Serves GetPlayerInfo /
	 * GetPlayerList. Indexed by playerID in [0, activePlayers); player slots
	 * are never null (playerHandler.Player returns &players[id]). Same
	 * unconditional per-boundary re-extraction as TeamRows: ping/cpuUsage
	 * arrive via NETMSG_PLAYERINFO between sim frames. Entirely excluded from
	 * the synced-desync SnapshotHash -- player state is net-layer, not
	 * per-sim-frame synced state.
	 */
	struct PlayerRows {
		int32_t activePlayers = 0;
		uint8_t hostDemo = 0;   // gameSetup->hostDemo (the IsPlayerUnsynced gate input)

		std::vector<std::string> name;
		std::vector<std::string> countryCode;
		std::vector<int32_t> team;
		std::vector<int32_t> rank;
		std::vector<int32_t> ping;
		std::vector<float> cpuUsage;
		std::vector<uint8_t> active;
		std::vector<uint8_t> spectator;
		std::vector<uint8_t> isFromDemo;
		std::vector<uint8_t> desynced;
		std::vector<spring::unordered_map<std::string, std::string>> customOpts;

		// ---- PR 36: GetPlayerControlledUnit + GetPlayerStatistics ----
		// controlleeID is the FPS-controlled unit's id (-1 = none) and
		// controlleeAllyTeam its allyteam (the access-gate input, captured so the
		// twin needs no live unit deref); currentStats is the input/command stat
		// block GetPlayerStatistics returns. Net-layer state, excluded from the
		// SnapshotHash like the rest of PlayerRows.
		std::vector<int32_t> controlleeID;
		std::vector<int32_t> controlleeAllyTeam;
		std::vector<PlayerStatistics> currentStats;

		// ===== PR 38c (zero-sanction flip): per-player rules-params serving =====
		// Per-player LuaRulesParams::Params (CPlayer::modParams) mirror, serving
		// GetPlayerRulesParam/GetPlayerRulesParams from draw context. Plain
		// per-boundary FULL copy indexed by playerID; the player roster is fixed
		// at gamestart (no id reuse at all), so this is the simplest of the three.
		// SNAPSHOT-hash-excluded like the rest of PlayerRows; correctness via the
		// SnapshotDiffGate player:rules field pass + the serving dual-run.
		std::vector<LuaRulesParams::Params> playerRulesParams; // [activePlayers]

		// playerHandler.IsValidPlayer mirror
		bool ValidPlayer(int playerID) const { return (playerID >= 0 && playerID < activePlayers); }
	};

	/**
	 * Global-scalar boundary copy (PR 27a, the split-contract serving pass):
	 * the sim-global reads the hot no-object callouts need at draw time --
	 * GetGameFrame/GetGameSeconds(Interpolated) (census: 3.2 + 0.9 per draw
	 * frame), GetGameSpeed/GetGameState, GetWind, the gs cheat/debug flags,
	 * GetGroundExtremes, GetGlobalLos. Same lifecycle as TeamRows/PlayerRows:
	 * re-extracted unconditionally every boundary (net messages mutate
	 * speed/pause/cheat state between sim frames; wind moves per frame), the
	 * whole copy is a few dozen bytes. Excluded from SnapshotHash: every
	 * synced field here is a pure global whose divergence localizes nothing
	 * (any unit-row divergence pinpoints better, and frameNum is the dump key
	 * itself); correctness is covered by the diff gate's field pass + the
	 * serving dual-runs.
	 */
	struct GlobalRows {
		// GetGameFrame / GetGameSeconds / GetGameSecondsInterpolated
		int32_t luaSimFrame = 0;         // gs->GetLuaSimFrame()
		// GetGameSpeed + the gs inputs of GetGameState's IsSimLagging
		float wantedSpeedFactor = 1.0f;  // gs->wantedSpeedFactor
		float speedFactor = 1.0f;        // gs->speedFactor
		uint8_t paused = 0;              // gs->paused
		// synced cheat/debug flags
		uint8_t cheatEnabled = 0;        // gs->cheatEnabled
		int32_t godMode = 0;             // gs->godMode (GODMODE_*_BIT mask)
		uint8_t editDefsEnabled = 0;     // gs->editDefsEnabled
		uint8_t noHelperAIs = 0;         // gs->noHelperAIs
		uint8_t defsNoCost = 0;          // unitDefHandler->GetNoCost()
		// GetGameState flags (doneLoading/savedGame are load-time-stable,
		// clientPaused arrives via net)
		uint8_t doneLoading = 0;         // game->IsDoneLoading()
		uint8_t savedGame = 0;           // game->IsSavedGame()
		uint8_t clientPaused = 0;        // game->IsClientPaused()
		// GetWind (current values move every sim frame)
		float3 windVec;                  // envResHandler.GetCurrentWindVec()
		float3 windDir;                  // envResHandler.GetCurrentWindDir()
		float windStrength = 0.0f;       // envResHandler.GetCurrentWindStrength()
		// GetGroundExtremes (init pair is constant; captured so the twin is
		// snapshot-only, curr pair mutates on terraform)
		float initMinHeight = 0.0f;
		float initMaxHeight = 0.0f;
		float currMinHeight = 0.0f;
		float currMaxHeight = 0.0f;
		// GetGlobalLos, one byte per allyteam
		int32_t numAllyTeams = 0;
		std::vector<uint8_t> globalLos;

		// ===== PR 38 (zero-sanction flip): game rules-params serving =====
		// The global game rules-params map (CSplitLuaHandle::GetGameParams(), the
		// singleton behind GetGameRulesParam/GetGameRulesParams). SYNCED state, but
		// EXCLUDED from the SnapshotHash for the same reason as the per-team mirror
		// in TeamRows (see there); verified by the SnapshotDiffGate glob:gameRules
		// field pass + the serving dual-run. Re-copied unconditionally every
		// boundary like the rest of GlobalRows.
		LuaRulesParams::Params gameRulesParams;

		// teamHandler.IsValidAllyTeam mirror
		bool ValidAllyTeam(int allyTeam) const { return (allyTeam >= 0 && allyTeam < numAllyTeams); }
	};
public:
	/// extract-if-due + publish; called once per draw frame from CGame::Draw,
	/// after the render-event drain (see the timing contract above)
	void Update();

	/// sim-side notification: a v1 field of a live unit changed *between* sim
	/// frames, invisibly to the frameNum/aliveCount due-checks. Sole caller is
	/// CUnit::ChangedTeam -- net-message-driven transfers (resign/share/take)
	/// run from ClientReadNet outside any sim frame and rewrite team/allyteam/
	/// losStatus (found by the armed SnapshotDiffGate: one-boundary-stale team
	/// rows at a mid-game resign). Makes the next Update() re-extract even
	/// though frameNum is unchanged. Sync-safe by the render-event-queue
	/// precedent: sim only writes a render-side bool, nothing synced reads it.
	void MarkMutatedOutsideFrame() { mutatedOutsideFrame = true; }

	/// game teardown (CGame::KillRendering); resets the stamps so the next
	/// game's first Update() extracts, and logs the extraction-cost stats
	void Clear();

	/// PR 16: while a SnapshotHash dump is armed, hash this completed sim frame.
	/// Called once per sim frame from CGame::SimFrame (NOT draw time) so every
	/// sim frame is hashed regardless of the draw/catch-up rate. Extracts into a
	/// private scratch buffer and hands it to SnapshotHash without touching the
	/// published front/back buffers or the generation, so draw-side behavior is
	/// unchanged. No-op (single relaxed bool load) unless armed.
	void HashCompletedFrame(int frameNum);

	// PR 38b: after the boundary publish, mark the ids destroyed in this batch
	// (from RenderEventQueue's drain lists) as DEAD_THIS_BATCH in the published
	// front buffers -- only where Extract left the slot INACTIVE, so a slot
	// reused by a new object this same batch stays ACTIVE. The retained data
	// rows (Extract overwrites only ACTIVE slots) then serve the object's
	// last-boundary state to the deferred handlers while the drain window is
	// open. ClearDeadThisBatch reverts those marks to INACTIVE at the barrier
	// ack (pre-epoch), so DEAD_THIS_BATCH never persists past its own drain.
	void MarkDeadThisBatch(const std::vector<int>& deadUnitIDs,
	                       const std::vector<int>& deadFeatureIDs,
	                       const std::vector<int>& deadProjectileIDs);
	void ClearDeadThisBatch();

	const UnitRows& Read() const { return *front; }
	const ProjectileRows& ReadProjectiles() const { return *projFront; }
	const FeatureRows& ReadFeatures() const { return *featFront; }
	const TeamRows& ReadTeams() const { return *teamFront; }
	const PlayerRows& ReadPlayers() const { return *playerFront; }
	const GlobalRows& ReadGlobals() const { return *globFront; }
	uint32_t Generation() const { return generation; }

	// PR 36: GetMapStartPositions -- the map-defined start positions are
	// immutable map data, so they are parsed once (LoadStartPositionsFromMap is
	// expensive) into a SimSnapshot-level cache, not a double-buffered TeamRows
	// field, and reused every boundary. Not hashed; verified by the Route
	// dual-run (the twin's table vs the live re-parse). Accessors below serve
	// the twin.
	int MapStartPosCount() const { return static_cast<int>(mapStartPos.size()); }
	bool MapStartPosValid(int teamNum) const {
		return (static_cast<size_t>(teamNum) < mapStartPosValid.size() && mapStartPosValid[teamNum] != 0);
	}
	float3 MapStartPos(int teamNum) const {
		return MapStartPosValid(teamNum) ? mapStartPos[teamNum] : float3{};
	}
private:
	void Extract(UnitRows& rows);
	void ExtractProjectiles(ProjectileRows& rows);
	void ExtractFeatures(FeatureRows& rows);
	void ExtractTeams(TeamRows& rows);
	void ExtractPlayers(PlayerRows& rows);
	void ExtractGlobals(GlobalRows& rows);
	// PR 36: fill mapStartPos/mapStartPosValid once (LoadStartPositionsFromMap)
	void CacheMapStartPositions();
	static void Resize(UnitRows& rows, size_t maxUnits, int numAllyTeams);
private:
	UnitRows buffers[2];
	UnitRows* front = &buffers[0];
	UnitRows* back = &buffers[1];

	ProjectileRows projBuffers[2];
	ProjectileRows* projFront = &projBuffers[0];
	ProjectileRows* projBack = &projBuffers[1];

	FeatureRows featBuffers[2];
	FeatureRows* featFront = &featBuffers[0];
	FeatureRows* featBack = &featBuffers[1];

	TeamRows teamBuffers[2];
	TeamRows* teamFront = &teamBuffers[0];
	TeamRows* teamBack = &teamBuffers[1];

	PlayerRows playerBuffers[2];
	PlayerRows* playerFront = &playerBuffers[0];
	PlayerRows* playerBack = &playerBuffers[1];

	GlobalRows globBuffers[2];
	GlobalRows* globFront = &globBuffers[0];
	GlobalRows* globBack = &globBuffers[1];

	// PR 16: scratch rows for per-sim-frame hashing; never published, kept only
	// to avoid reallocating its arrays every armed frame
	UnitRows hashScratch;
	ProjectileRows hashProjScratch;
	FeatureRows hashFeatScratch;
	TeamRows hashTeamScratch;

	uint32_t generation = 0;

	// PR 36: GetMapStartPositions cache (immutable map data, parsed once)
	std::vector<float3> mapStartPos;
	std::vector<uint8_t> mapStartPosValid;
	bool mapStartPosCached = false;

	// see MarkMutatedOutsideFrame(); cleared by the extraction it forces
	bool mutatedOutsideFrame = false;

	// extraction-cost stats, reported by Clear()
	float sumExtractMs = 0.0f;
	float maxExtractMs = 0.0f;
	uint32_t numExtractions = 0;
	int32_t peakAliveCount = 0;
};

extern SimSnapshot simSnapshot;


// ---- PR 38f/38g: event-time LOS-exit visibility override -------------------
// Amendment (b) of the PR-38 event-time mechanism, refined by PR 38g (operator
// ruling: option (b) = NO behavior change vs master). A synced UnitLeftLos event
// dispatches to unsynced Lua handlers; under the split those handlers run
// DEFERRED at the SimDrawBarrier, by which point the published snapshot reflects
// the END-of-frame LOS state -- which may have cleared the unit's radar/LOS bits
// for the leaving allyteam past what master's SYNCHRONOUS mid-sim handler saw --
// so the UnitRows Pov gates nil out Spring.GetUnitPosition and the handler (e.g.
// unit_ghostradar_gl4) errors.
//
// Master's real behavior (CUnit::SetLosStatus, Unit.cpp): LOS_INLOS is cleared
// BEFORE eventHandler.UnitLeftLos fires; the radar bit, if also leaving this
// same call, clears only in a LATER block, so it is still set at dispatch. The
// synchronous handler thus observes the POST-transition losStatus, and
// GetUnitPosition -> GetErrorVector returns a FUZZY radar-error-offset position
// (AllyTeamRadarErrorSize on radar, BaseRadarErrorSize*2 when neither radar nor
// ghost, zero error only when seenGhost).
//
// PR 38g captures the EXACT at-dispatch losStatus byte at FIRE time (the sim
// thread owns the unit; unit->losStatus[at] there == master's observed value)
// and presents it as a per-(unit, allyTeam) override for the deferred handler
// call. The UnitRows::PovUnit{Visible,InLos,Typed} gates and ErrorVector, when
// the override matches, substitute this residual byte for the row's end-of-frame
// losStatusAll byte and run their UNCHANGED logic. So the read gate re-opens
// exactly when master's did (residual radar/ghost) -- fixing the nil -- and the
// bit-for-bit GetErrorVector mirror reproduces master's radar-error position
// EXACTLY, with NO forced in-LOS and NO forced zero error. If the residual byte
// is neither-visible, the gate stays closed -> nil, matching master (no
// over-disclosure). PR 38f previously forced in-LOS + zero error, over-
// disclosing exact positions; 38g removes both.
//
// Main-thread dispatch-window only -- never installed flag-off or during the
// diff-gate dual-run (both dispatch immediately at fire time), so the Pov reads
// are inert there and behavior stays byte-identical. UNSYNCED (draw-only): the
// fire-time unit->losStatus[at] access is a READ, stored into draw-side state;
// no synced write, no sync-hash impact, no gsRNG/streflop.
namespace SimSnapshotLosEvent {
	struct ScopedVisibility {
		ScopedVisibility(int unitID, int allyTeam, uint8_t losStatus);
		~ScopedVisibility();
		ScopedVisibility(const ScopedVisibility&) = delete;
		ScopedVisibility& operator=(const ScopedVisibility&) = delete;
	private:
		int prevUnitID;
		int prevAllyTeam;
		uint8_t prevLosStatus;
	};

	// consulted by the UnitRows Pov gates + ErrorVector in SimSnapshot.cpp:
	// Active() reports whether the override matches (unit, allyTeam); when it
	// does, LosStatus() is master's captured at-dispatch losStatus byte, used in
	// place of the row's end-of-frame losStatusAll byte.
	bool Active(int unitID, int allyTeam);
	uint8_t LosStatus();

	// PR 38j: Installed() is a cheap flag-off/inert fast-reject (true only while
	// a deferred UnitLeftLos handler is dispatching). ActiveForUnit() is the
	// allyTeam-agnostic unit match used by the position/direction callouts to
	// decide, at their top, whether to serve arg#1's unit via the snapshot twin
	// (whose Pov gates then apply the precise per-allyTeam Active() check). The
	// override globals are inert (-1) flag-off and during the diff-gate dual-run.
	bool Installed();
	bool ActiveForUnit(int unitID);
}
