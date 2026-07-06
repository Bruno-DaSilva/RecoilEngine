/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

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
 * losStatus semantics:
 *  - One byte per unit: the losStatus row for viewAllyTeam, which is
 *    gu->myAllyTeam at extraction time (for full-view spectators that is the
 *    row of whatever allyteam they are nominally on; full-view consumers keep
 *    their live `gu->spectatingFullView` bypass exactly as on master, so the
 *    row content is moot for them). The snapshot re-extracts when
 *    gu->myAllyTeam changes, so a spectator switching viewed teams gets the
 *    new row at the next Update() even while paused.
 *  - Information surface: the losStatus row is by definition what the local
 *    client knows, so serving it does not widen anything. The other v1 fields
 *    are extracted raw and unmasked for *all* alive units -- safe while the
 *    snapshot is CPU-side and every consumer applies the same visibility
 *    gates master applies (v1's only consumer reads losStatus itself). The
 *    first consumer that could leak a hidden-enemy value (Lua callout
 *    serving, PR 18) must settle the masking policy: either mask fields at
 *    extraction by this losStatus row, or gate at the serving layer -- cf.
 *    the LOS gate precedent in UpdateObjectUniforms / ExtractTransforms.
 *
 * Extraction timing:
 *  - Update() runs at the start of the draw side of the frame -- in
 *    CGame::Draw, right after renderEventQueue.Drain() and
 *    AckDrainedDestroys() -- so the snapshot and the drawer containers agree
 *    on the same completed sim frame N.
 *  - Zero new sim frames since the last extraction = no-op. A catch-up burst
 *    of N sim frames produces one extraction (intermediate frames are
 *    unobservable, same as master's rendering). Extraction also re-runs when
 *    gu->myAllyTeam changes (row selection above), when the alive-unit count
 *    changes outside the frame cadence (objects spawned before the first sim
 *    frame advances -- same edge ExtractTransforms handles via its pending
 *    flag), when sim marks a between-frames mutation (net-message-driven team
 *    transfers, see MarkMutatedOutsideFrame), and on any frameNum mismatch
 *    including backwards jumps (checkpoint load / replay rewind).
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
 *  3. add an accessor with the invalid-id default;
 *  4. convert consumers from the live dereference to the accessor, one small
 *     PR per consumer, keeping any gu->spectatingFullView bypass live.
 */
class SimSnapshot
{
public:
	struct UnitRows {
		int32_t simFrame = -1;      // sim frame this buffer was extracted at
		int32_t viewAllyTeam = -1;  // allyteam whose losStatus row was extracted
		int32_t aliveCount = 0;

		std::vector<uint8_t> valid;
		std::vector<float3> pos;
		std::vector<float4> speed;
		std::vector<float> health;
		std::vector<float> maxHealth;
		std::vector<uint8_t> team;
		std::vector<uint8_t> allyTeam;
		std::vector<int32_t> defID;
		std::vector<float> buildProgress;
		std::vector<uint8_t> losStatus;

		// out-of-range ids (including any id before the first extraction ever
		// ran, when the arrays are still unsized) are part of the stale/nil
		// contract: a deterministic miss, not an error
		bool Valid(int unitID) const {
			return (static_cast<size_t>(unitID) < valid.size() && valid[unitID] != 0);
		}

		// accessors return a deterministic default for invalid ids (stale/nil contract)
		uint8_t LosStatus(int unitID) const { return Valid(unitID) ? losStatus[unitID] : 0; }
		float3 Pos(int unitID) const { return Valid(unitID) ? pos[unitID] : float3{}; }
		float4 Speed(int unitID) const { return Valid(unitID) ? speed[unitID] : float4{}; }
		float Health(int unitID) const { return Valid(unitID) ? health[unitID] : 0.0f; }
		float MaxHealth(int unitID) const { return Valid(unitID) ? maxHealth[unitID] : 0.0f; }
		int Team(int unitID) const { return Valid(unitID) ? team[unitID] : -1; }
		int AllyTeam(int unitID) const { return Valid(unitID) ? allyTeam[unitID] : -1; }
		int DefID(int unitID) const { return Valid(unitID) ? defID[unitID] : 0; }
		float BuildProgress(int unitID) const { return Valid(unitID) ? buildProgress[unitID] : 0.0f; }
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
	uint32_t Generation() const { return generation; }
private:
	void Extract(UnitRows& rows);
	static void Resize(UnitRows& rows, size_t maxUnits);
private:
	UnitRows buffers[2];
	UnitRows* front = &buffers[0];
	UnitRows* back = &buffers[1];

	// PR 16: scratch rows for per-sim-frame hashing; never published, kept only
	// to avoid reallocating its arrays every armed frame
	UnitRows hashScratch;

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
