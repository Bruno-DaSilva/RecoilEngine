/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>

#include "SimSnapshot.h"

/**
 * @brief SnapshotHash -- hierarchical, spatially-localizing hash over SimSnapshot
 *
 * PR 16 of the sim/draw decoupling plan (doc/sim-draw-thread-decoupling-research.md,
 * "Update 2026-07-05" item 6; S1 row in doc/replay-seeking-architecture.md). Its
 * job is to localize a simulation divergence in SPACE (which unit / which field),
 * not just in time. Per-sim-frame divergence *detection* already exists via
 * NETMSG_SYNCRESPONSE; this is the always-on hierarchical tier that says *where*.
 *
 * Hierarchy (bottom-up, all values hashed as fixed-width little-endian words,
 * floats as their IEEE-754 bit patterns -- no padding, no pointers, so the hash
 * is deterministic across runs and platforms for identical snapshot content):
 *
 *   unit  : one hash per *valid* SimSnapshot row, over exactly the v1 field set
 *           in a fixed order (see HashUnitRow in the .cpp): unitID, pos.xyz,
 *           speed.xyzw, health, maxHealth, {team,allyTeam,losStatus} packed,
 *           defID, buildProgress. Including unitID makes a shifted/misaligned
 *           id-set detectable; skipping invalid rows makes a validity change
 *           show up as a bucket/root difference.
 *   bucket : fixed-size id-range buckets (BUCKET_SIZE ids each). Each valid
 *           unit's hash is folded into its bucket in ascending-id order. Empty
 *           buckets are seeded by index so an emptied bucket still differs.
 *   root   : all bucket hashes folded in bucket order -> one hash per frame.
 *
 * To diff two runs at the same frame: compare root; if it differs, compare the
 * bucket lines to find the diverging id-range(s); if per-unit lines were dumped,
 * compare them to name the exact unit id and (with the snapshot row values) the
 * field. That top-down walk is what the dump format is shaped for.
 *
 * Cadence / arming:
 *  - The published SimSnapshot double-buffer is extracted once per *draw* frame
 *    (SimSnapshot::Update), so under fast-forward/catch-up it skips the sim
 *    frames that never got drawn. That would make two runs at different playback
 *    speeds produce incomparable series. So when armed we instead hash EVERY
 *    completed sim frame, driven from CGame::SimFrame via
 *    SimSnapshot::HashCompletedFrame(), which extracts a private scratch copy of
 *    the SimSnapshot rows (same Extract() as the published buffer, hence "hashes
 *    the SimSnapshot contents") without touching the front/back buffers or the
 *    generation -- so game-visible draw behavior is byte-for-byte unchanged.
 *  - Unarmed cost is a single relaxed bool load (Armed()); everything heavier is
 *    behind it. Armed cost is ~one SimSnapshot extraction (~0.02ms mean) plus the
 *    hash walk, per sim frame in the window -- acceptable for a diagnostic run.
 *
 * Rewind persistence: a checkpoint rewind destroys+recreates CGame (and LuaUI)
 * in-process, but this module's state is process-static and deliberately
 * survives that reload -- so the forward-pass rows and the post-rewind resim rows
 * accumulate into one dump. FlushPartial() (called from ~CGame, i.e. on every
 * reload and on the final quit) rewrites the whole file each time WITHOUT
 * disarming, so the file always reflects the full history and the final teardown
 * leaves the complete forward+resim series. Re-arming while active is a no-op
 * that keeps the buffer, so a driver widget may safely re-issue the command
 * after the reload.
 *
 * Adding a field to SimSnapshot: also add it to HashUnitRow's word list here (in
 * a fixed position) or the new field will silently not participate in the hash.
 */
namespace SnapshotHash {
	enum Detail : int {
		DETAIL_ROOT   = 0, // one root line per frame (cheap, whole-game safe)
		DETAIL_BUCKET = 1, // + one line per occupied id-range bucket per frame
		DETAIL_UNIT   = 2, // + one line per valid unit per frame (bounded windows only!)
	};

	// cheap gate: true while a dump is armed. Everything else is behind this.
	bool Armed();

	// arm a per-sim-frame hash dump over sim frames [startFrame, endFrame].
	// Ignored (buffer kept) if a dump is already active -- see rewind persistence.
	void StartDump(int startFrame, int endFrame, std::string path, Detail detail);
	// disarm and write the file immediately (interactive use).
	void StopDump();
	// rewrite the file from the accumulated buffer WITHOUT disarming; called from
	// ~CGame so the dump survives rewind reloads and is complete at final exit.
	void FlushPartial();

	// hash one completed sim frame's snapshot rows (units + synced projectiles +
	// features + the synced per-team block; the projectile/feature/team sections
	// fold into the root hash) and append its lines to the buffer; no-op unless
	// armed and frameNum is inside the window. Driven from
	// SimSnapshot::HashCompletedFrame (CGame::SimFrame). Player rows are not
	// hashed (net-layer state, not per-sim-frame synced state).
	void HashFrame(int frameNum, const SimSnapshot::UnitRows& rows, const SimSnapshot::ProjectileRows& projRows, const SimSnapshot::FeatureRows& featRows, const SimSnapshot::TeamRows& teamRows);
}
