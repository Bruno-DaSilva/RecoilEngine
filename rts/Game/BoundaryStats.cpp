/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "BoundaryStats.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Rendering/Models/ModelsMemStorage.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"

namespace BoundaryStats {

// plain (non-atomic) copy of every cumulative counter at the previously
// sampled frame, so each row carries per-frame deltas
struct CounterSnap {
	uint64_t traChecked, traChanged, traForced;
	uint64_t pieceSampled, pieceChanged, objSampled, objPieceChanged, objMoved;
	uint64_t cmdPushBack, cmdPushFront, cmdInsert, cmdPopBack, cmdPopFront, cmdErase, cmdClearCmds;
	uint64_t projSpawnPiece, projSpawnHitscan, projSpawnGuided, projSpawnBallistic, projSpawnSyncedOther, projSpawnUnsynced;
	uint64_t projDespawnSynced, projDespawnUnsynced;
	uint64_t unitCreated, unitDestroyed, featCreated, featDestroyed;
	uint64_t losEnterLos, losLeaveLos, losEnterRadar, losLeaveRadar;
};

static CounterSnap Snap()
{
	const auto rd = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
	CounterSnap s;
	s.traChecked = rd(ctr.traChecked); s.traChanged = rd(ctr.traChanged); s.traForced = rd(ctr.traForced);
	s.pieceSampled = rd(ctr.pieceSampled); s.pieceChanged = rd(ctr.pieceChanged);
	s.objSampled = rd(ctr.objSampled); s.objPieceChanged = rd(ctr.objPieceChanged); s.objMoved = rd(ctr.objMoved);
	s.cmdPushBack = rd(ctr.cmdPushBack); s.cmdPushFront = rd(ctr.cmdPushFront); s.cmdInsert = rd(ctr.cmdInsert);
	s.cmdPopBack = rd(ctr.cmdPopBack); s.cmdPopFront = rd(ctr.cmdPopFront); s.cmdErase = rd(ctr.cmdErase);
	s.cmdClearCmds = rd(ctr.cmdClearCmds);
	s.projSpawnPiece = rd(ctr.projSpawnPiece); s.projSpawnHitscan = rd(ctr.projSpawnHitscan);
	s.projSpawnGuided = rd(ctr.projSpawnGuided); s.projSpawnBallistic = rd(ctr.projSpawnBallistic);
	s.projSpawnSyncedOther = rd(ctr.projSpawnSyncedOther); s.projSpawnUnsynced = rd(ctr.projSpawnUnsynced);
	s.projDespawnSynced = rd(ctr.projDespawnSynced); s.projDespawnUnsynced = rd(ctr.projDespawnUnsynced);
	s.unitCreated = rd(ctr.unitCreated); s.unitDestroyed = rd(ctr.unitDestroyed);
	s.featCreated = rd(ctr.featCreated); s.featDestroyed = rd(ctr.featDestroyed);
	s.losEnterLos = rd(ctr.losEnterLos); s.losLeaveLos = rd(ctr.losLeaveLos);
	s.losEnterRadar = rd(ctr.losEnterRadar); s.losLeaveRadar = rd(ctr.losLeaveRadar);
	return s;
}

// last-sampled-frame copy of the Lua-serving hot fields of one unit, for the
// exact-change (delta-encoding) rate; indexed by unitID
struct PrevHotFields {
	int stamp = -1; // sim frame this entry was last written at
	float pos[3];
	float speed[4];
	float health;
	float build;
	uint16_t heading;
};

static bool dumpActive = false;
static bool baselineSeeded = false;
static int dumpStartFrame = 0;
static int dumpEndFrame = 0;
static std::string dumpPath;
static std::string rowBuf;         // completed CSV rows (header included)
static CounterSnap prevSnap;
static std::vector<PrevHotFields> prevHot;

static constexpr const char* CSV_HEADER =
	"frame,sample_cost_ms,"
	"units_active,feats_live,projs_synced,projs_unsynced,particles,"
	"tra_elems,tra_bytes,uni_elems,uni_bytes,"
	"d_tra_checked,d_tra_changed,d_tra_forced,"
	"d_piece_sampled,d_piece_changed,d_obj_sampled,d_obj_piecechanged,d_obj_moved,"
	"d_cmd_pushback,d_cmd_pushfront,d_cmd_insert,d_cmd_popback,d_cmd_popfront,d_cmd_erase,d_cmd_clear,"
	"cmdq_cmds_total,cmdq_nonempty,cmdq_max,cmdq_len_0,cmdq_len_1,cmdq_len_2_4,cmdq_len_5_16,cmdq_len_17_64,cmdq_len_65p,"
	"d_proj_spawn_piece,d_proj_spawn_hitscan,d_proj_spawn_guided,d_proj_spawn_ballistic,d_proj_spawn_syncother,d_proj_spawn_unsynced,"
	"d_proj_despawn_synced,d_proj_despawn_unsynced,"
	"d_unit_created,d_unit_destroyed,d_feat_created,d_feat_destroyed,"
	"d_los_enter_los,d_los_leave_los,d_los_enter_radar,d_los_leave_radar,"
	"hf_units_compared,hf_units_new,hf_pos_changed,hf_speed_changed,hf_health_changed,hf_heading_changed,hf_build_changed\n";


void StartDump(int startFrame, int endFrame, std::string path)
{
	if (dumpActive) {
		LOG_L(L_WARNING, "[BoundaryStats::StartDump] a dump is already active (-> %s)", dumpPath.c_str());
		return;
	}
	if (endFrame < startFrame) {
		LOG_L(L_ERROR, "[BoundaryStats::StartDump] end frame %d precedes start frame %d", endFrame, startFrame);
		return;
	}

	dumpActive = true;
	baselineSeeded = false;
	dumpStartFrame = startFrame;
	dumpEndFrame = endFrame;
	dumpPath = std::move(path);

	rowBuf.clear();
	rowBuf.reserve(1 << 20);
	rowBuf += CSV_HEADER;

	prevHot.clear();
	prevHot.resize(unitHandler.MaxUnits());

	active.store(true, std::memory_order_relaxed);

	LOG("[BoundaryStats::StartDump] armed: sim frames %d..%d -> %s", startFrame, endFrame, dumpPath.c_str());
}

static void WriteOut()
{
	std::ofstream f(dumpPath);
	if (!f.good()) {
		LOG_L(L_ERROR, "[BoundaryStats] cannot open %s for writing", dumpPath.c_str());
		return;
	}
	f << rowBuf;
	LOG("[BoundaryStats] wrote boundary-size samples (frames %d..%d) to %s",
		dumpStartFrame, dumpEndFrame, dumpPath.c_str());
}

static void StopDump()
{
	if (!dumpActive)
		return;

	dumpActive = false;
	active.store(false, std::memory_order_relaxed);

	WriteOut();

	rowBuf.clear();
	rowBuf.shrink_to_fit();
	prevHot.clear();
	prevHot.shrink_to_fit();
}

void FlushPartial()
{
	if (!dumpActive)
		return;

	LOG("[BoundaryStats] game ending before frame %d; flushing partial dump", dumpEndFrame);
	StopDump();
}

void SampleFrame(int frameNum)
{
	if (!dumpActive)
		return;
	if (frameNum < dumpStartFrame)
		return;

	const spring_time t0 = spring_gettime();

	const CounterSnap cur = Snap();

	// first sampled frame only seeds the delta baseline and the hot-field
	// table stamps (its "changes" would otherwise be measured against the
	// pre-dump past / garbage)
	const bool seeding = !baselineSeeded;

	// command-queue length distribution + hot-field change rates, one pass
	uint64_t cmdqTotal = 0, cmdqNonEmpty = 0, cmdqMax = 0;
	uint64_t cmdqHist[6] = {0, 0, 0, 0, 0, 0}; // 0, 1, 2-4, 5-16, 17-64, 65+
	uint64_t hfCompared = 0, hfNew = 0;
	uint64_t hfPos = 0, hfSpeed = 0, hfHealth = 0, hfHeading = 0, hfBuild = 0;

	for (const CUnit* u : unitHandler.GetActiveUnits()) {
		const size_t ql = (u->commandAI != nullptr) ? u->commandAI->commandQue.size() : 0;
		cmdqTotal += ql;
		cmdqNonEmpty += (ql > 0);
		cmdqMax = std::max<uint64_t>(cmdqMax, ql);
		if      (ql ==  0) cmdqHist[0]++;
		else if (ql ==  1) cmdqHist[1]++;
		else if (ql <=  4) cmdqHist[2]++;
		else if (ql <= 16) cmdqHist[3]++;
		else if (ql <= 64) cmdqHist[4]++;
		else               cmdqHist[5]++;

		PrevHotFields& ph = prevHot[u->id];
		if (ph.stamp == frameNum - 1) {
			hfCompared++;
			hfPos     += (ph.pos[0] != u->pos.x || ph.pos[1] != u->pos.y || ph.pos[2] != u->pos.z);
			hfSpeed   += (ph.speed[0] != u->speed.x || ph.speed[1] != u->speed.y ||
			              ph.speed[2] != u->speed.z || ph.speed[3] != u->speed.w);
			hfHealth  += (ph.health != u->health);
			hfHeading += (ph.heading != uint16_t(u->heading));
			hfBuild   += (ph.build != u->buildProgress);
		} else {
			hfNew++;
		}
		ph.stamp = frameNum;
		ph.pos[0] = u->pos.x; ph.pos[1] = u->pos.y; ph.pos[2] = u->pos.z;
		ph.speed[0] = u->speed.x; ph.speed[1] = u->speed.y; ph.speed[2] = u->speed.z; ph.speed[3] = u->speed.w;
		ph.health = u->health;
		ph.build = u->buildProgress;
		ph.heading = uint16_t(u->heading);
	}

	if (seeding) {
		baselineSeeded = true;
		prevSnap = cur;
		return;
	}

	const uint64_t unitsActive = unitHandler.GetActiveUnits().size();
	const uint64_t featsLive = cur.featCreated - cur.featDestroyed;
	const uint64_t projsSynced = projectileHandler.GetActiveProjectiles(true).size();
	const uint64_t projsUnsynced = projectileHandler.GetActiveProjectiles(false).size();
	const uint64_t particles = projectileHandler.GetCurrentParticles();

	const uint64_t traElems = transformsMemStorage.GetSize();
	const uint64_t uniElems = modelUniformsStorage.GetSize();
	const uint64_t traBytes = traElems * sizeof(TransformsMemStorage::MyType);
	const uint64_t uniBytes = uniElems * sizeof(ModelUniformData);

	char row[1536];
	char* w = row;
	const auto put = [&w](uint64_t v) { w += sprintf(w, ",%llu", (unsigned long long)v); };

	// frame + placeholder-free prefix
	w += sprintf(w, "%d,%.3f", frameNum, (spring_gettime() - t0).toMilliSecsf());

	put(unitsActive); put(featsLive); put(projsSynced); put(projsUnsynced); put(particles);
	put(traElems); put(traBytes); put(uniElems); put(uniBytes);

	put(cur.traChecked - prevSnap.traChecked);
	put(cur.traChanged - prevSnap.traChanged);
	put(cur.traForced - prevSnap.traForced);

	put(cur.pieceSampled - prevSnap.pieceSampled);
	put(cur.pieceChanged - prevSnap.pieceChanged);
	put(cur.objSampled - prevSnap.objSampled);
	put(cur.objPieceChanged - prevSnap.objPieceChanged);
	put(cur.objMoved - prevSnap.objMoved);

	put(cur.cmdPushBack - prevSnap.cmdPushBack);
	put(cur.cmdPushFront - prevSnap.cmdPushFront);
	put(cur.cmdInsert - prevSnap.cmdInsert);
	put(cur.cmdPopBack - prevSnap.cmdPopBack);
	put(cur.cmdPopFront - prevSnap.cmdPopFront);
	put(cur.cmdErase - prevSnap.cmdErase);
	put(cur.cmdClearCmds - prevSnap.cmdClearCmds);

	put(cmdqTotal); put(cmdqNonEmpty); put(cmdqMax);
	put(cmdqHist[0]); put(cmdqHist[1]); put(cmdqHist[2]); put(cmdqHist[3]); put(cmdqHist[4]); put(cmdqHist[5]);

	put(cur.projSpawnPiece - prevSnap.projSpawnPiece);
	put(cur.projSpawnHitscan - prevSnap.projSpawnHitscan);
	put(cur.projSpawnGuided - prevSnap.projSpawnGuided);
	put(cur.projSpawnBallistic - prevSnap.projSpawnBallistic);
	put(cur.projSpawnSyncedOther - prevSnap.projSpawnSyncedOther);
	put(cur.projSpawnUnsynced - prevSnap.projSpawnUnsynced);
	put(cur.projDespawnSynced - prevSnap.projDespawnSynced);
	put(cur.projDespawnUnsynced - prevSnap.projDespawnUnsynced);

	put(cur.unitCreated - prevSnap.unitCreated);
	put(cur.unitDestroyed - prevSnap.unitDestroyed);
	put(cur.featCreated - prevSnap.featCreated);
	put(cur.featDestroyed - prevSnap.featDestroyed);

	put(cur.losEnterLos - prevSnap.losEnterLos);
	put(cur.losLeaveLos - prevSnap.losLeaveLos);
	put(cur.losEnterRadar - prevSnap.losEnterRadar);
	put(cur.losLeaveRadar - prevSnap.losLeaveRadar);

	put(hfCompared); put(hfNew);
	put(hfPos); put(hfSpeed); put(hfHealth); put(hfHeading); put(hfBuild);

	*w++ = '\n';
	rowBuf.append(row, w - row);

	prevSnap = cur;

	if (frameNum >= dumpEndFrame)
		StopDump();
}

} // namespace BoundaryStats
