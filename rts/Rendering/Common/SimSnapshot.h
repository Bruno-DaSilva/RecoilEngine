/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include "Sim/Misc/CollisionVolume.h"
#include "System/float3.h"
#include "System/float4.h"

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
 *
 * Validity rules:
 *  - Valid(id) mirrors membership in unitHandler's active-unit list at the
 *    stamped simFrame; dying-but-not-yet-deleted units are therefore valid,
 *    exactly as master's live reads would see them.
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

		// out-of-range ids (including any id before the first extraction ever
		// ran, when the arrays are still unsized) are part of the stale/nil
		// contract: a deterministic miss, not an error
		bool Valid(int unitID) const {
			return (static_cast<size_t>(unitID) < valid.size() && valid[unitID] != 0);
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
		// LuaUtils::IsUnitVisible / IsUnitInLos mirrors. A readAllyTeam that
		// is negative without fullRead indexes losStatus out of bounds on the
		// live path (cannot arise for real handles); here it reads as a
		// deterministic not-visible.
		bool PovUnitVisible(int unitID, int readAllyTeam, bool fullRead) const;
		bool PovUnitInLos(int unitID, int readAllyTeam, bool fullRead) const;

		// CUnit::GetErrorVector / GetLuaErrorVector mirrors
		float3 ErrorVector(int unitID, int argAllyTeam) const;
		float3 LuaErrorVector(int unitID, int readAllyTeam, bool fullRead) const {
			return (fullRead ? float3{0.0f, 0.0f, 0.0f} : ErrorVector(unitID, readAllyTeam));
		}
		// CSolidObject::GetObjectSpaceVec mirror
		float3 ObjectSpaceVec(int unitID, const float3& v) const {
			return ((frontdir[unitID] * v.z) + (rightdir[unitID] * v.x) + (updir[unitID] * v.y));
		}
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
		std::vector<uint8_t> inLosAll;    // [numAllyTeams * MaxSlots()], row-major by allyteam

		bool Valid(int projID) const {
			return (static_cast<size_t>(projID) < valid.size() && valid[projID] != 0);
		}
		size_t MaxSlots() const { return valid.size(); }

		bool InLos(int projID, int argAllyTeam) const {
			return (argAllyTeam >= 0 && argAllyTeam < numAllyTeams &&
				inLosAll[argAllyTeam * MaxSlots() + projID] != 0);
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
		std::vector<float3> relMidPos;
		std::vector<float> radius;
		std::vector<int32_t> allyTeam;   // CFeature::allyteam (may be -1)
		std::vector<int32_t> defID;
		std::vector<uint8_t> alwaysVisible;
		std::vector<uint8_t> noSelect;
		std::vector<uint8_t> inVoid;
		std::vector<CollisionVolume> selVol;
		std::vector<uint8_t> inLosAll;   // [numAllyTeams * MaxSlots()], row-major by allyteam

		bool Valid(int id) const {
			return (static_cast<size_t>(id) < valid.size() && valid[id] != 0);
		}
		size_t MaxSlots() const { return valid.size(); }

		float3 Pos(int id) const { return Valid(id) ? pos[id] : float3{}; }
		float3 MidPos(int id) const { return Valid(id) ? midPos[id] : float3{}; }
		float Radius(int id) const { return Valid(id) ? radius[id] : 0.0f; }
		int AllyTeam(int id) const { return Valid(id) ? allyTeam[id] : -1; }
		int DefID(int id) const { return Valid(id) ? defID[id] : 0; }
		bool NoSelect(int id) const { return Valid(id) && noSelect[id] != 0; }
		bool InVoid(int id) const { return Valid(id) && inVoid[id] != 0; }
		const CollisionVolume& SelVol(int id) const {
			static const CollisionVolume def;
			return Valid(id) ? selVol[id] : def;
		}

		bool InLos(int id, int argAllyTeam) const {
			return (argAllyTeam >= 0 && argAllyTeam < numAllyTeams &&
				inLosAll[argAllyTeam * MaxSlots() + id] != 0);
		}
		// CFeature::IsInLosForAllyTeam mirror; caller must have checked Valid()
		bool IsInLosForAllyTeam(int id, int argAllyTeam) const;
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

	const UnitRows& Read() const { return *front; }
	const ProjectileRows& ReadProjectiles() const { return *projFront; }
	const FeatureRows& ReadFeatures() const { return *featFront; }
	uint32_t Generation() const { return generation; }
private:
	void Extract(UnitRows& rows);
	void ExtractProjectiles(ProjectileRows& rows);
	void ExtractFeatures(FeatureRows& rows);
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

	// PR 16: scratch rows for per-sim-frame hashing; never published, kept only
	// to avoid reallocating its arrays every armed frame
	UnitRows hashScratch;
	ProjectileRows hashProjScratch;
	FeatureRows hashFeatScratch;

	uint32_t generation = 0;

	// see MarkMutatedOutsideFrame(); cleared by the extraction it forces
	bool mutatedOutsideFrame = false;

	// extraction-cost stats, reported by Clear()
	float sumExtractMs = 0.0f;
	float maxExtractMs = 0.0f;
	uint32_t numExtractions = 0;
	int32_t peakAliveCount = 0;
};

extern SimSnapshot simSnapshot;
