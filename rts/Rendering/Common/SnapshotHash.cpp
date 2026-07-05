/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotHash.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "System/Log/ILog.h"
#include "lib/xxhash/xxh3.h"

namespace SnapshotHash {

// Fixed seeds define the hash and must never change (would break cross-run/
// cross-build comparability of dumps). 64-bit XXH3 keeps root/bucket rollup
// collisions negligible over a full 40k-frame game.
static constexpr uint64_t UNIT_SEED   = 0x53696d55'6e697448ull; // "SimUnitH"
static constexpr uint64_t BUCKET_SEED = 0x42756b65'74483136ull; // "BuketH16"
static constexpr uint64_t ROOT_SEED   = 0x526f6f74'48313600ull; // "RootH16"

// number of dense unit ids folded into one bucket. Powers of two keep id/bucket
// cheap; 1024 gives ~a few dozen buckets at BAR's late-game unit counts.
static constexpr uint32_t BUCKET_SIZE = 1024;

static inline uint64_t Mix(const void* p, size_t n, uint64_t seed)
{
	return XXH3_64bits_withSeed(p, n, static_cast<XXH64_hash_t>(seed));
}

// Per-unit row hash over exactly the SimSnapshot v1 field set, in a fixed order,
// each value as a fixed-width word (floats via their IEEE-754 bit pattern). No
// padding and no pointers are hashed, so identical content hashes identically on
// every run and platform. If a field is added to SimSnapshot, add a word here.
static inline uint64_t HashUnitRow(const SimSnapshot::UnitRows& r, int id)
{
	uint32_t w[13];
	w[0]  = static_cast<uint32_t>(id);
	w[1]  = std::bit_cast<uint32_t>(r.pos[id].x);
	w[2]  = std::bit_cast<uint32_t>(r.pos[id].y);
	w[3]  = std::bit_cast<uint32_t>(r.pos[id].z);
	w[4]  = std::bit_cast<uint32_t>(r.speed[id].x);
	w[5]  = std::bit_cast<uint32_t>(r.speed[id].y);
	w[6]  = std::bit_cast<uint32_t>(r.speed[id].z);
	w[7]  = std::bit_cast<uint32_t>(r.speed[id].w);
	w[8]  = std::bit_cast<uint32_t>(r.health[id]);
	w[9]  = std::bit_cast<uint32_t>(r.maxHealth[id]);
	w[10] = static_cast<uint32_t>(r.team[id])
	      | (static_cast<uint32_t>(r.allyTeam[id])  << 8)
	      | (static_cast<uint32_t>(r.losStatus[id]) << 16);
	w[11] = static_cast<uint32_t>(r.defID[id]);
	w[12] = std::bit_cast<uint32_t>(r.buildProgress[id]);
	return Mix(w, sizeof(w), UNIT_SEED);
}


static std::atomic<bool> armed{false};

static bool dumpActive = false;
static int dumpStartFrame = 0;
static int dumpEndFrame = 0;
static Detail dumpDetail = DETAIL_ROOT;
static std::string dumpPath;
static std::string rowBuf; // all emitted lines (header included), persists across rewind reloads

static constexpr const char* FILE_HEADER =
	"# SnapshotHash dump (PR 16). Tab-separated, tag-prefixed lines.\n"
	"# R\\t<frame>\\t<alive>\\t<rootHash>            one per sim frame\n"
	"# B\\t<frame>\\t<bucketIndex>\\t<bucketHash>     occupied id-range buckets (detail>=bucket)\n"
	"# U\\t<frame>\\t<unitID>\\t<unitHash>            valid units (detail>=unit)\n"
	"# A frame appears once per pass; a rewind re-emits the same frame numbers, so\n"
	"# grouping by frame and finding the first frame whose R hashes disagree gives\n"
	"# the first diverging sim frame; the B/U lines localize it in space.\n";


bool Armed()
{
	return armed.load(std::memory_order_relaxed);
}

void StartDump(int startFrame, int endFrame, std::string path, Detail detail)
{
	if (dumpActive) {
		// keep the accumulated buffer (rewind re-arm is intentional, see header)
		LOG_L(L_WARNING, "[SnapshotHash::StartDump] a dump is already active (-> %s); ignoring re-arm", dumpPath.c_str());
		return;
	}
	if (endFrame < startFrame) {
		LOG_L(L_ERROR, "[SnapshotHash::StartDump] end frame %d precedes start frame %d", endFrame, startFrame);
		return;
	}

	dumpActive = true;
	dumpStartFrame = startFrame;
	dumpEndFrame = endFrame;
	dumpDetail = detail;
	dumpPath = std::move(path);

	rowBuf.clear();
	rowBuf.reserve(1 << 20);
	rowBuf += FILE_HEADER;

	armed.store(true, std::memory_order_relaxed);

	LOG("[SnapshotHash::StartDump] armed: sim frames %d..%d detail=%d -> %s",
		startFrame, endFrame, static_cast<int>(detail), dumpPath.c_str());
}

static void WriteOut()
{
	if (dumpPath.empty())
		return;

	std::ofstream f(dumpPath);
	if (!f.good()) {
		LOG_L(L_ERROR, "[SnapshotHash] cannot open %s for writing", dumpPath.c_str());
		return;
	}
	f << rowBuf;
	LOG("[SnapshotHash] wrote snapshot hashes (frames %d..%d) to %s",
		dumpStartFrame, dumpEndFrame, dumpPath.c_str());
}

void StopDump()
{
	if (!dumpActive)
		return;

	dumpActive = false;
	armed.store(false, std::memory_order_relaxed);

	WriteOut();

	rowBuf.clear();
	rowBuf.shrink_to_fit();
}

void FlushPartial()
{
	if (!dumpActive)
		return;

	// Rewrite the file with everything collected so far but do NOT disarm or drop
	// the buffer: ~CGame runs on every rewind reload, and we want the forward and
	// post-rewind passes to accumulate. The final teardown leaves a complete file.
	WriteOut();
}

void HashFrame(int frameNum, const SimSnapshot::UnitRows& rows)
{
	if (!dumpActive)
		return;
	if (frameNum < dumpStartFrame || frameNum > dumpEndFrame)
		return;

	const size_t maxUnits = rows.valid.size();
	const uint32_t numBuckets = static_cast<uint32_t>((maxUnits + BUCKET_SIZE - 1) / BUCKET_SIZE);

	// seed every bucket by its index so an emptied bucket still differs from a
	// never-populated one, and empty buckets contribute deterministically
	std::vector<uint64_t> bucketHash(numBuckets);
	std::vector<uint32_t> bucketCount(numBuckets, 0);
	for (uint32_t b = 0; b < numBuckets; ++b)
		bucketHash[b] = Mix(&b, sizeof(b), BUCKET_SEED);

	char ub[64];
	uint32_t alive = 0;

	// ascending id order => deterministic fold order within each bucket
	for (size_t id = 0; id < maxUnits; ++id) {
		if (rows.valid[id] == 0)
			continue;

		++alive;
		const uint64_t uh = HashUnitRow(rows, static_cast<int>(id));
		const uint32_t b = static_cast<uint32_t>(id) / BUCKET_SIZE;
		bucketHash[b] = Mix(&uh, sizeof(uh), bucketHash[b]);
		++bucketCount[b];

		if (dumpDetail >= DETAIL_UNIT) {
			const int n = std::snprintf(ub, sizeof(ub), "U\t%d\t%d\t%016llx\n",
				frameNum, static_cast<int>(id), static_cast<unsigned long long>(uh));
			rowBuf.append(ub, n);
		}
	}

	uint64_t rootHash = ROOT_SEED;
	for (uint32_t b = 0; b < numBuckets; ++b)
		rootHash = Mix(&bucketHash[b], sizeof(bucketHash[b]), rootHash);

	char rb[64];
	const int rn = std::snprintf(rb, sizeof(rb), "R\t%d\t%u\t%016llx\n",
		frameNum, alive, static_cast<unsigned long long>(rootHash));
	rowBuf.append(rb, rn);

	if (dumpDetail >= DETAIL_BUCKET) {
		char bb[64];
		for (uint32_t b = 0; b < numBuckets; ++b) {
			if (bucketCount[b] == 0)
				continue;
			const int bn = std::snprintf(bb, sizeof(bb), "B\t%d\t%u\t%016llx\n",
				frameNum, b, static_cast<unsigned long long>(bucketHash[b]));
			rowBuf.append(bb, bn);
		}
	}
}

} // namespace SnapshotHash
