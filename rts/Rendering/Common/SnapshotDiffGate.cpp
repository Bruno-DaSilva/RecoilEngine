/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotDiffGate.h"

#include <cstring>
#include <type_traits> // PR 38: RulesParamsEqual value-variant visit
#include <variant>     // PR 38: RulesParamsEqual value-variant visit
#include <vector>

#include "SimSnapshot.h"
#include "DrawMapMirrors.h"
#include "ExternalAI/SkirmishAIData.h"     // PR 36: team:misc AI-block recompute
#include "ExternalAI/SkirmishAIHandler.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/Players/PlayerStatistics.h" // PR 36: player:misc recompute
#include "Sim/Misc/AllyTeam.h"             // PR 36: team:allyInfo recompute
#include "Sim/Misc/GlobalConstants.h"      // PR 36: SQUARE_SIZE (team:allyInfo)
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Map/MapInfo.h"
#include "Map/MetalMap.h" // PR 38d: metal distribution mirror compare
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/GroundBlockingObjectMap.h" // sim|draw PR 29: blocking-mirror compare
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/Wind.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
// PR 32 (deep per-unit state): live re-derivation for the field pass
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "Sim/MoveTypes/HoverAirMoveType.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/MoveTypes/StaticMoveType.h"
#include "Sim/MoveTypes/ScriptMoveType.h"
#include "Sim/Path/IPathManager.h" // PR 38g GetUnitEstimatedPath live compare
#include "Sim/Misc/GlobalConstants.h" // GAME_SPEED
#include "Sim/Misc/NanoPieceCache.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/CommandAI/MobileCAI.h"
#include "Sim/Units/UnitToolTipMap.hpp"
#include "Sim/Units/UnitTypes/Builder.h"
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/PieceProjectile.h" // PR 33 GetPieceProjectileParams/Name
#include "Rendering/Models/3DModelPiece.hpp" // PR 33 S3DModelPiece::name (ppro->omp)
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
// PR 31 (weapon/shield scalar family): live weapon/shield/damages compare
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/BombDropper.h"
#include "Sim/Weapons/WeaponTarget.h"
#include "Sim/Misc/DamageArray.h"
#include "Game/Players/Player.h" // fpsControlPlayer gate compare
#include "Lua/LuaSnapshotServe.h" // sim|draw PR 30: command-queue serving-cache compare
#include "Lua/LuaHandleSynced.h"  // PR 38: CSplitLuaHandle::GetGameParams (game rules params compare)
#include "System/Log/ILog.h"

SnapshotDiffGate snapshotDiffGate;

// bit-exact float compare: master copies raw values into the snapshot, so the
// expected relation is bit equality, not numeric equality. memcmp treats a NaN
// as equal to the identically-encoded NaN it was copied from (whereas == would
// spuriously flag it) and -0.0 as distinct from +0.0 (a real torn-copy signal).
static bool BitEqual(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
static bool BitEqual(const float3& a, const float3& b) { return std::memcmp(&a, &b, sizeof(float3)) == 0; }
static bool BitEqual(const float4& a, const float4& b) { return std::memcmp(&a, &b, sizeof(float4)) == 0; }

// PR 31 weapon-damages comparator (defined below with the weapon-row helpers);
// forward-declared so the PR 38g projectile-damages field pass in
// CheckProjectileRows (earlier in the file) can reuse the same field-wise compare
static bool WpnDamagesEqual(const SimSnapshot::UnitRows::DamagesSnap& s, const DynDamageArray* live);

static constexpr const char* FIELD_NAMES[] = {
	"validity",
	"pos",
	"midPos",
	"aimPos",
	"speed",
	"health",
	"maxHealth",
	"paralyzeDamage",
	"captureProgress",
	"team",
	"allyTeam",
	"defID",
	"buildProgress",
	"beingBuilt",
	"stunned",
	"radius",
	"relMidPos",
	"frontdir",
	"updir",
	"rightdir",
	"posErrorVector",
	"leavesGhost",
	"losStatusAll",
	"posErrorBits",
	"globals",
	"maskedErrorVec",
	"noSelect",
	"inVoid",
	"selVol",
	"inRadarAll",
	"stateFlags",
	"heading/buildFacing",
	"unitScalars",
	"unitEco",
	"sensorRadii",
	"unitMiscInts",
	"blockingBits",
	"proj:validity",
	"proj:pos",
	"proj:speed",
	"proj:allyTeam",
	"proj:ownerID",
	"proj:isWeapon",
	"proj:weaponDefID",
	"proj:target",
	"proj:inLosAll",
	"proj:dir",
	"proj:gravity",
	"proj:teamID",
	"proj:ttlFlags",
	"feat:validity",
	"feat:pos",
	"feat:midPos",
	"feat:aimPos",
	"feat:relMidPos",
	"feat:radius",
	"feat:allyTeam",
	"feat:defID",
	"feat:flags",
	"feat:selVol",
	"feat:inLosAll",
	"feat:team",
	"feat:scalars",
	"feat:speed",
	"feat:dirMatrix",
	"feat:resources",
	"feat:blockingBits",
	"feat:resurrect",
	"feat:globals",
	"team:globals",
	"team:state",
	"team:res",
	"team:stats",
	"team:color",
	"team:strings",
	"player:globals",
	"player:info",
	"player:state",
	"player:net",
	"player:opts",
	"glob:frame",
	"glob:speed",
	"glob:flags",
	"glob:wind",
	"glob:heights",
	"glob:globalLos",
	// map-layer mirrors (PR 28)
	"map:los",
	"map:terrainTypes",
	"map:smoothMesh",
	"map:origHeight",
	"map:radarError",
	// command-queue serving cache (sim|draw PR 30)
	"cq:presence",
	"cq:queue",
	"cq:descs",
	"cq:worker",
	"cq:factory",
	// PR 33 piece/script family (appended last, matches P_PIECEPARAMS)
	"proj:pieceParams",
	// PR 34 (spatial/list remainder): appended to match the enum tail P_RADIUS
	"proj:radius",
	// sim|draw PR 29 (blocking-map mirror): appended to match the enum tail
	// MM_BLOCKING
	"map:blocking",
	// PR 31 (weapon/shield scalar family): appended to match the enum tail
	"wpn:unit",
	"wpn:unitDamages",
	"wpn:state",
	"wpn:vectors",
	"wpn:target",
	"wpn:shield",
	"wpn:damages",
	// PR 32 (deep per-unit state): appended to match the enum tail (D_*)
	"unit:states",
	"unit:eco2",
	"unit:posErr2",
	"unit:refs",
	"unit:buildState",
	"unit:moveType",
	"unit:nanoPieces",
	"unit:transportees",
	"unit:tooltip",
	"unit:losVariants",
	// PR 36 (team/player misc): appended to match the enum tail T_MISC..PL_MISC
	"team:misc",
	"team:allyInfo",
	"player:misc",
	// PR 38 (zero-sanction flip): rules-params mirror, appended to match the
	// enum tail T_RULES..G_GAMERULES
	"team:rules",
	"glob:gameRules",
	// PR 38c: player/unit/feature rules-params mirror (enum tail U_RULES..PL_RULES)
	"unit:rules",
	"feature:rules",
	"player:rules",
	// PR 38d (GetGroundInfo mirrors): appended to match the enum tail
	// MM_TYPEMAP..MM_METALMAP
	"map:typeMap",
	"map:metalMap",
	// PR 38g (Batch-4 P1): sanctioned-tail serving; appended in lockstep with the
	// enum tail FT_FIRESMOKE..D_ESTPATH
	"feat:fireSmoke",
	"proj:damages",
	"unit:estPath",
	// PR 41 (GetVisibleProjectiles): appended to match the enum tail P_DRAWRADIUS..
	// P_VISINLOS
	"proj:drawRadius",
	"proj:hitscan",
	"proj:visInLosAll",
};

// structural compare for the copied customOpts maps (emilib::HashMap has no
// operator==); sizes equal + every key of a maps to an equal value in b
static bool OptsEqual(const spring::unordered_map<std::string, std::string>& a,
                      const spring::unordered_map<std::string, std::string>& b)
{
	if (a.size() != b.size())
		return false;

	for (const auto& [key, value] : a) {
		const auto it = b.find(key);
		if (it == b.end() || it->second != value)
			return false;
	}

	return true;
}

// PR 38 (zero-sanction flip): structural compare for the copied rules-params
// maps (LuaRulesParams::Params has no operator==). Sizes equal + every key of a
// maps to a b entry with an equal los mask AND a bit-equal value; the value is a
// std::variant<bool, float, std::string>, so float legs compare bit-exact
// (master copies the raw value) and the variant index must match too (a legit
// type change reuses the key). Order-independent (map iteration order is
// irrelevant to the served Lua table, which is string-keyed).
static bool RulesParamsEqual(const LuaRulesParams::Params& a, const LuaRulesParams::Params& b)
{
	if (a.size() != b.size())
		return false;

	for (const auto& [key, param] : a) {
		const auto it = b.find(key);
		if (it == b.end())
			return false;

		const LuaRulesParams::Param& lp = it->second;
		if (param.los != lp.los)
			return false;
		if (param.value.index() != lp.value.index())
			return false;

		// bit-exact value compare (float leg via BitEqual, bool/string via ==)
		bool valueEqual = false;
		std::visit([&](auto&& av) {
			using T = std::decay_t<decltype(av)>;
			const T& bv = std::get<T>(lp.value);
			if constexpr (std::is_same_v<T, float>)
				valueEqual = BitEqual(av, bv);
			else
				valueEqual = (av == bv);
		}, param.value);

		if (!valueEqual)
			return false;
	}

	return true;
}

// GetSolidObjectBlocking's seven pushed booleans as one byte, bit i = push
// slot i; must stay identical to SimSnapshot.cpp's extraction-side twin
static uint8_t PackBlockingBits(const CSolidObject* o)
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

// field-wise CollisionVolume compare over exactly the params the hit-test reads
// (avoids memcmp padding-byte false positives between two field-wise copies)
static bool ColVolEqual(const CollisionVolume& a, const CollisionVolume& b)
{
	return BitEqual(a.GetScales(), b.GetScales())
		&& BitEqual(a.GetOffsets(), b.GetOffsets())
		&& a.GetVolumeType() == b.GetVolumeType()
		&& a.GetPrimaryAxis() == b.GetPrimaryAxis()
		&& a.GetSecondaryAxis(0) == b.GetSecondaryAxis(0)
		&& a.GetSecondaryAxis(1) == b.GetSecondaryAxis(1)
		&& a.IgnoreHits() == b.IgnoreHits()
		&& a.UseContHitTest() == b.UseContHitTest()
		&& a.DefaultToPieceTree() == b.DefaultToPieceTree()
		&& a.DefaultToFootPrint() == b.DefaultToFootPrint();
}

// ---- PR 32 (deep per-unit state) live re-derivation for the field pass ----
// A separate, independent copy of SimSnapshot's extraction chains (the
// PackBlockingBits precedent): identical logic on both sides means the compare
// catches torn/stale extraction, not a formula fork.
static bool MoveTypeBlockEqual(const SimSnapshot::MoveTypeBlock& a, const SimSnapshot::MoveTypeBlock& b)
{
	return BitEqual(a.turnRate, b.turnRate) && BitEqual(a.accRate, b.accRate) && BitEqual(a.decRate, b.decRate)
		&& BitEqual(a.maxReverseSpeed, b.maxReverseSpeed) && BitEqual(a.wantedSpeed, b.wantedSpeed)
		&& BitEqual(a.currentSpeed, b.currentSpeed) && BitEqual(a.goalRadius, b.goalRadius)
		&& BitEqual(a.currWayPoint, b.currWayPoint) && BitEqual(a.nextWayPoint, b.nextWayPoint)
		&& BitEqual(a.wantedHeight, b.wantedHeight) && a.collide == b.collide && a.useSmoothMesh == b.useSmoothMesh
		&& a.aircraftState == b.aircraftState && a.flyState == b.flyState
		&& BitEqual(a.goalDistance, b.goalDistance) && a.bankingAllowed == b.bankingAllowed && a.dontLand == b.dontLand
		&& BitEqual(a.currentBank, b.currentBank) && BitEqual(a.currentPitch, b.currentPitch)
		&& BitEqual(a.altitudeRate, b.altitudeRate) && BitEqual(a.maxDrift, b.maxDrift)
		&& BitEqual(a.myGravity, b.myGravity) && BitEqual(a.maxBank, b.maxBank) && BitEqual(a.turnRadius, b.turnRadius)
		&& BitEqual(a.maxAileron, b.maxAileron) && BitEqual(a.maxElevator, b.maxElevator) && BitEqual(a.maxRudder, b.maxRudder);
}

static void LiveMoveType(const CUnit* u, uint8_t& kind, float& maxSpeed, float& maxWanted,
                         float3& goalPos, uint8_t& progress, uint8_t& autoLand, uint8_t& loopback,
                         SimSnapshot::MoveTypeBlock& b)
{
	const AMoveType* mt = u->moveType;
	maxSpeed = mt->GetMaxSpeed() * GAME_SPEED;
	maxWanted = mt->GetMaxWantedSpeed() * GAME_SPEED;
	goalPos = mt->goalPos;
	progress = static_cast<uint8_t>(mt->progressState);
	autoLand = 0;
	loopback = 0;
	b = SimSnapshot::MoveTypeBlock{};

	if (const auto* g = dynamic_cast<const CGroundMoveType*>(mt); g != nullptr) {
		kind = 1;
		b.turnRate = g->GetTurnRate(); b.accRate = g->GetAccRate(); b.decRate = g->GetDecRate();
		b.maxReverseSpeed = g->GetMaxReverseSpeed() * GAME_SPEED;
		b.wantedSpeed = g->GetWantedSpeed() * GAME_SPEED;
		b.currentSpeed = g->GetCurrentSpeed() * GAME_SPEED;
		b.goalRadius = g->GetGoalRadius();
		b.currWayPoint = g->GetCurrWayPoint(); b.nextWayPoint = g->GetNextWayPoint();
		return;
	}
	if (const auto* h = dynamic_cast<const CHoverAirMoveType*>(mt); h != nullptr) {
		kind = 2;
		autoLand = h->autoLand;
		b.wantedHeight = h->wantedHeight; b.collide = h->collide; b.useSmoothMesh = h->useSmoothMesh;
		b.aircraftState = h->aircraftState; b.flyState = h->flyState;
		b.goalDistance = h->goalDistance; b.bankingAllowed = h->bankingAllowed;
		b.currentBank = h->currentBank; b.currentPitch = h->currentPitch;
		b.turnRate = h->turnRate; b.accRate = h->accRate; b.decRate = h->decRate;
		b.altitudeRate = h->altitudeRate; b.dontLand = h->GetAllowLanding(); b.maxDrift = h->maxDrift;
		return;
	}
	if (const auto* s = dynamic_cast<const CStrafeAirMoveType*>(mt); s != nullptr) {
		kind = 3;
		autoLand = s->autoLand;
		loopback = s->loopbackAttack;
		b.aircraftState = s->aircraftState; b.wantedHeight = s->wantedHeight;
		b.collide = s->collide; b.useSmoothMesh = s->useSmoothMesh;
		b.myGravity = s->myGravity; b.maxBank = s->maxBank; b.turnRadius = s->turnRadius;
		b.accRate = s->accRate; b.maxAileron = s->maxAileron; b.maxElevator = s->maxElevator; b.maxRudder = s->maxRudder;
		return;
	}
	if (dynamic_cast<const CStaticMoveType*>(mt) != nullptr) { kind = 4; return; }
	if (dynamic_cast<const CScriptMoveType*>(mt) != nullptr) { kind = 5; return; }
	kind = 0;
}


void SnapshotDiffGate::Arm()
{
	ResetCounters();
	armed = true;
	LOG("[SnapshotDiffGate] armed: verifying SimSnapshot-served values against live sim at each draw boundary");
}

void SnapshotDiffGate::Disarm()
{
	if (!armed) {
		LOG_L(L_WARNING, "[SnapshotDiffGate] not armed");
		return;
	}
	Report("disarm");
	armed = false;
}

void SnapshotDiffGate::Dump() const
{
	if (!armed) {
		LOG_L(L_WARNING, "[SnapshotDiffGate] not armed");
		return;
	}
	Report("dump");
}

void SnapshotDiffGate::FlushPartial()
{
	if (!armed)
		return;
	Report("game-end");
	armed = false;
}

void SnapshotDiffGate::ResetCounters()
{
	// a name/enum drift would silently mislabel every report row
	static_assert(sizeof(FIELD_NAMES) / sizeof(FIELD_NAMES[0]) == F_COUNT, "FIELD_NAMES out of sync with the field enum");

	boundaryChecks = 0;

	for (int f = 0; f < F_COUNT; ++f)
		fields[f] = FieldCounter{FIELD_NAMES[f]};

	callouts.clear();
}

bool SnapshotDiffGate::Bump(FieldCounter& fc, bool equal)
{
	fc.checked++;
	if (equal)
		return false;

	fc.mismatched++;
	if (fc.logged >= kMaxLogged)
		return false;

	fc.logged++;
	return true;
}

void SnapshotDiffGate::Report(const char* reason) const
{
	uint64_t totalMismatch = 0;
	for (const FieldCounter& fc : fields)
		totalMismatch += fc.mismatched;
	for (const auto& [name, fc] : callouts)
		totalMismatch += fc.mismatched;

	LOG("[SnapshotDiffGate] ===== report (%s) : %s =====",
		reason, (totalMismatch == 0) ? "PASS (0 mismatches)" : "FAIL");
	LOG("[SnapshotDiffGate] boundary checks=%llu, total mismatches=%llu",
		(unsigned long long)boundaryChecks, (unsigned long long)totalMismatch);

	for (const FieldCounter& fc : fields) {
		LOG("[SnapshotDiffGate]   %-28s checked=%-14llu mismatched=%llu",
			fc.name, (unsigned long long)fc.checked, (unsigned long long)fc.mismatched);
	}
	for (const auto& [name, fc] : callouts) {
		LOG("[SnapshotDiffGate]   callout:%-20s checked=%-14llu mismatched=%llu",
			name.c_str(), (unsigned long long)fc.checked, (unsigned long long)fc.mismatched);
	}
}


void SnapshotDiffGate::CountCallout(const char* callout, bool equal, const char* detail)
{
	if (!armed)
		return;

	FieldCounter& fc = callouts[callout];

	if (Bump(fc, equal))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d callout=%s %s",
			gs->frameNum, callout, detail);
}


void SnapshotDiffGate::CheckBoundary()
{
	if (!armed)
		return;

	const SimSnapshot::UnitRows& rows = simSnapshot.Read();

	// after simSnapshot.Update() the front buffer always describes the current
	// sim frame (Update() is due whenever simFrame != frameNum). If that does
	// not hold something published the wrong buffer -- bail rather than emit a
	// storm of false positives.
	if (rows.simFrame != gs->frameNum) {
		LOG_L(L_ERROR, "[SnapshotDiffGate] snapshot frame %d != sim frame %d; skipping boundary check",
			rows.simFrame, gs->frameNum);
		return;
	}

	boundaryChecks++;

	const int numAllyTeams = rows.numAllyTeams;
	const size_t maxUnits = rows.MaxUnits();

	// global block: alliance matrix + radar-error scalars (one bundled counter)
	{
		bool globalsEqual = (numAllyTeams == teamHandler.ActiveAllyTeams());
		globalsEqual = globalsEqual && BitEqual(rows.baseRadarErrorSize, losHandler->GetBaseRadarErrorSize());
		for (int at = 0; globalsEqual && at < numAllyTeams; ++at)
			globalsEqual = BitEqual(rows.radarErrorSizes[at], losHandler->GetAllyTeamRadarErrorSize(at));
		for (int a = 0; globalsEqual && a < numAllyTeams; ++a)
			for (int b = 0; globalsEqual && b < numAllyTeams; ++b)
				globalsEqual = (rows.Allied(a, b) == teamHandler.Ally(a, b));

		if (Bump(fields[F_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=globals mismatch (numAllyTeams snap=%d live=%d)",
				gs->frameNum, numAllyTeams, teamHandler.ActiveAllyTeams());
	}

	for (size_t i = 0; i < maxUnits; ++i) {
		const int id = static_cast<int>(i);
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits

		const bool snapValid = (rows.valid[i] != 0);
		const bool liveValid = (u != nullptr);

		if (Bump(fields[F_VALIDITY], snapValid == liveValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=validity snap=%d live=%d",
				gs->frameNum, id, int(snapValid), int(liveValid));

		// field comparisons only make sense where both agree the row exists;
		// a one-sided validity mismatch was already reported above
		if (!snapValid || !liveValid)
			continue;

		const auto checkF3 = [&](int f, const float3& snap, const float3& live) {
			if (Bump(fields[f], BitEqual(snap, live)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
					gs->frameNum, id, FIELD_NAMES[f], snap.x, snap.y, snap.z, live.x, live.y, live.z);
		};
		const auto checkF = [&](int f, float snap, float live) {
			if (Bump(fields[f], BitEqual(snap, live)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=%.9g live=%.9g",
					gs->frameNum, id, FIELD_NAMES[f], snap, live);
		};
		const auto checkI = [&](int f, int snap, int live) {
			if (Bump(fields[f], snap == live))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=%s snap=%d live=%d",
					gs->frameNum, id, FIELD_NAMES[f], snap, live);
		};

		checkF3(F_POS, rows.pos[i], u->pos);
		checkF3(F_MIDPOS, rows.midPos[i], u->midPos);
		checkF3(F_AIMPOS, rows.aimPos[i], u->aimPos);

		if (Bump(fields[F_SPEED], BitEqual(rows.speed[i], u->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=speed snap=(%.9g,%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.speed[i].x, rows.speed[i].y, rows.speed[i].z, rows.speed[i].w,
				u->speed.x, u->speed.y, u->speed.z, u->speed.w);

		checkF(F_HEALTH, rows.health[i], u->health);
		checkF(F_MAXHEALTH, rows.maxHealth[i], u->maxHealth);
		checkF(F_PARALYZE, rows.paralyzeDamage[i], u->paralyzeDamage);
		checkF(F_CAPTURE, rows.captureProgress[i], u->captureProgress);
		checkI(F_TEAM, rows.team[i], static_cast<uint8_t>(u->team));
		checkI(F_ALLYTEAM, rows.allyTeam[i], static_cast<uint8_t>(u->allyteam));
		checkI(F_DEFID, rows.defID[i], u->unitDef->id);
		checkF(F_BUILDPROGRESS, rows.buildProgress[i], u->buildProgress);
		checkI(F_BEINGBUILT, rows.beingBuilt[i], int(u->beingBuilt));
		checkI(F_STUNNED, rows.stunned[i], int(u->IsStunned()));
		checkF(F_RADIUS, rows.radius[i], u->radius);
		checkF3(F_RELMIDPOS, rows.relMidPos[i], u->relMidPos);
		checkF3(F_FRONTDIR, rows.frontdir[i], u->frontdir);
		checkF3(F_UPDIR, rows.updir[i], u->updir);
		checkF3(F_RIGHTDIR, rows.rightdir[i], u->rightdir);
		checkF3(F_POSERRORVEC, rows.posErrorVector[i], u->posErrorVector);
		checkI(F_LEAVESGHOST, rows.leavesGhost[i], int(u->leavesGhost));
		checkI(F_NOSELECT, int(rows.noSelect[i]), int(u->noSelect));
		checkI(F_INVOID, int(rows.inVoid[i]), int(u->IsInVoid()));

		if (Bump(fields[F_SELVOL], ColVolEqual(rows.selVol[i], u->selectionVolume)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=selVol mismatch (type snap=%d live=%d)",
				gs->frameNum, id, rows.selVol[i].GetVolumeType(), u->selectionVolume.GetVolumeType());

		// PR 27a unit rows (grouped counters, see the enum comments)
		{
			const bool stateEqual =
				(rows.isDead[i] == uint8_t(u->isDead)) &&
				(rows.neutral[i] == uint8_t(u->neutral)) &&
				(rows.activated[i] == uint8_t(u->activated)) &&
				(rows.isCloaked[i] == uint8_t(u->isCloaked)) &&
				(rows.armoredState[i] == uint8_t(u->armoredState));
			if (Bump(fields[F_STATEFLAGS], stateEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=stateFlags mismatch", gs->frameNum, id);

			const bool headingEqual =
				(rows.heading[i] == int16_t(u->heading)) &&
				(rows.buildFacing[i] == int16_t(u->buildFacing));
			if (Bump(fields[F_HEADINGFACING], headingEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=heading/buildFacing snap=%d/%d live=%d/%d",
					gs->frameNum, id, int(rows.heading[i]), int(rows.buildFacing[i]), int(u->heading), int(u->buildFacing));

			const bool scalarsEqual =
				BitEqual(rows.height[i], u->height) &&
				BitEqual(rows.mass[i], u->mass) &&
				BitEqual(rows.maxRange[i], u->maxRange) &&
				BitEqual(rows.seismicSignature[i], u->seismicSignature) &&
				BitEqual(rows.armoredMultiple[i], u->armoredMultiple) &&
				BitEqual(rows.experience[i], u->experience) &&
				BitEqual(rows.limExperience[i], u->limExperience);
			if (Bump(fields[F_UNITSCALARS], scalarsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitScalars mismatch", gs->frameNum, id);

			const auto packEqual = [](const SResourcePack& a, const SResourcePack& b) {
				return BitEqual(a.metal, b.metal) && BitEqual(a.energy, b.energy);
			};
			const bool ecoEqual =
				packEqual(rows.resourcesMake[i], u->resourcesMake) &&
				packEqual(rows.resourcesUse[i], u->resourcesUse) &&
				packEqual(rows.harvested[i], u->harvested) &&
				packEqual(rows.harvestStorage[i], u->harvestStorage) &&
				packEqual(rows.cost[i], u->cost) &&
				BitEqual(rows.buildTime[i], u->buildTime);
			if (Bump(fields[F_UNITECO], ecoEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitEco mismatch", gs->frameNum, id);

			const bool sensorsEqual =
				(rows.losRadius[i] == u->losRadius) &&
				(rows.airLosRadius[i] == u->airLosRadius) &&
				(rows.radarRadius[i] == u->radarRadius) &&
				(rows.sonarRadius[i] == u->sonarRadius) &&
				(rows.seismicRadius[i] == u->seismicRadius) &&
				(rows.jammerRadius[i] == u->jammerRadius) &&
				(rows.sonarJamRadius[i] == u->sonarJamRadius);
			if (Bump(fields[F_SENSORRADII], sensorsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=sensorRadii mismatch", gs->frameNum, id);

			const bool intsEqual =
				(rows.selfDCountdown[i] == u->selfDCountdown) &&
				(rows.moveDefID[i] == ((u->moveDef != nullptr) ? int32_t(u->moveDef->pathType) : -1));
			if (Bump(fields[F_UNITMISCINTS], intsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unitMiscInts mismatch (selfD snap=%d live=%d)",
					gs->frameNum, id, rows.selfDCountdown[i], u->selfDCountdown);

			if (Bump(fields[F_BLOCKINGBITS], rows.blockingBits[i] == PackBlockingBits(u)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=blockingBits snap=0x%02x live=0x%02x",
					gs->frameNum, id, int(rows.blockingBits[i]), int(PackBlockingBits(u)));
		}

		// ---- PR 32 (deep per-unit state) field pass ----
		{
			const CMobileCAI* mcai = dynamic_cast<const CMobileCAI*>(u->commandAI);
			const float liveRepair = (mcai != nullptr) ? mcai->repairBelowHealth : -1.0f;
			const bool statesEqual =
				(rows.fireState[i] == u->fireState) &&
				(rows.moveState[i] == u->moveState) &&
				BitEqual(rows.repairBelowHealth[i], liveRepair) &&
				(rows.repeatOrders[i] == uint8_t(u->commandAI->repeatOrders)) &&
				(rows.wantCloak[i] == uint8_t(u->wantCloak)) &&
				(rows.useHighTrajectory[i] == uint8_t(u->useHighTrajectory));
			if (Bump(fields[D_STATES], statesEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:states mismatch", gs->frameNum, id);

			const bool eco2Equal =
				BitEqual(rows.storage[i].metal, u->storage.metal) &&
				BitEqual(rows.storage[i].energy, u->storage.energy) &&
				BitEqual(rows.metalExtract[i], u->metalExtract) &&
				BitEqual(rows.buildeeRadius[i], u->buildeeRadius);
			if (Bump(fields[D_ECO2], eco2Equal))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:eco2 mismatch", gs->frameNum, id);

			const bool posErr2Equal =
				BitEqual(rows.posErrorDelta[i], u->posErrorDelta) &&
				(rows.nextPosErrorUpdate[i] == u->nextPosErrorUpdate);
			if (Bump(fields[D_POSERR2], posErr2Equal))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:posErr2 mismatch", gs->frameNum, id);

			const int32_t liveLastAtk = (u->lastAttacker != nullptr) ? u->lastAttacker->id : -1;
			const int32_t liveTransp = (u->GetTransporter() != nullptr) ? u->GetTransporter()->id : -1;
			// live curBuild (mirror the extraction's builder-then-factory order)
			uint8_t liveBuilderKind = 0;
			int32_t liveCurBuild = -1;
			float liveBuildDist = 0.0f, liveBuildPower = 0.0f;
			uint8_t liveRange3D = 0;
			std::vector<int32_t> liveNano;
			if (const CBuilder* b = dynamic_cast<const CBuilder*>(u); b != nullptr) {
				liveBuilderKind = 1;
				liveCurBuild = (b->curBuild != nullptr) ? b->curBuild->id : -1;
				liveBuildDist = b->buildDistance;
				liveRange3D = b->range3D;
				liveBuildPower = b->GetNanoPieceCache().GetBuildPower();
				const auto& np = b->GetNanoPieceCache().GetNanoPieces();
				liveNano.assign(np.begin(), np.end());
			} else if (const CFactory* f = dynamic_cast<const CFactory*>(u); f != nullptr) {
				liveBuilderKind = 2;
				liveCurBuild = (f->curBuild != nullptr) ? f->curBuild->id : -1;
				liveBuildPower = f->GetNanoPieceCache().GetBuildPower();
				const auto& np = f->GetNanoPieceCache().GetNanoPieces();
				liveNano.assign(np.begin(), np.end());
			}

			const bool refsEqual =
				(rows.lastAttackerID[i] == liveLastAtk) &&
				(rows.transporterID[i] == liveTransp) &&
				(rows.curBuildID[i] == liveCurBuild);
			if (Bump(fields[D_REFS], refsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:refs mismatch", gs->frameNum, id);

			const bool buildStateEqual =
				(rows.builderKind[i] == liveBuilderKind) &&
				BitEqual(rows.buildDistance[i], liveBuildDist) &&
				(rows.range3D[i] == liveRange3D) &&
				(rows.inBuildStance[i] == uint8_t(u->inBuildStance)) &&
				BitEqual(rows.buildPower[i], liveBuildPower);
			if (Bump(fields[D_BUILDSTATE], buildStateEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:buildState mismatch", gs->frameNum, id);

			if (Bump(fields[D_NANOPIECES], rows.nanoPieces[i] == liveNano))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:nanoPieces mismatch", gs->frameNum, id);

			std::vector<int32_t> liveTransportees;
			liveTransportees.reserve(u->transportedUnits.size());
			for (const CUnit::TransportedUnit& tu : u->transportedUnits)
				liveTransportees.push_back(tu.unit->id);
			if (Bump(fields[D_TRANSPORTEES], rows.transportees[i] == liveTransportees))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:transportees mismatch", gs->frameNum, id);

			if (Bump(fields[D_TOOLTIP], rows.customTooltip[i] == unitToolTipMap.GetConst(id)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:tooltip mismatch", gs->frameNum, id);

			uint8_t mtKind, mtProgress, mtAutoLand, mtLoopback;
			float mtMaxSpeed, mtMaxWanted;
			float3 mtGoal;
			SimSnapshot::MoveTypeBlock liveBlock;
			LiveMoveType(u, mtKind, mtMaxSpeed, mtMaxWanted, mtGoal, mtProgress, mtAutoLand, mtLoopback, liveBlock);
			const bool moveTypeEqual =
				(rows.moveTypeKind[i] == mtKind) &&
				BitEqual(rows.mtMaxSpeed[i], mtMaxSpeed) &&
				BitEqual(rows.mtMaxWantedSpeed[i], mtMaxWanted) &&
				BitEqual(rows.mtGoalPos[i], mtGoal) &&
				(rows.mtProgressState[i] == mtProgress) &&
				(rows.mtAutoLand[i] == mtAutoLand) &&
				(rows.mtLoopbackAttack[i] == mtLoopback) &&
				MoveTypeBlockEqual(rows.moveTypeBlock[i], liveBlock);
			if (Bump(fields[D_MOVETYPE], moveTypeEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:moveType mismatch (kind snap=%d live=%d)",
					gs->frameNum, id, int(rows.moveTypeKind[i]), int(mtKind));

			// PR 38g (GetUnitEstimatedPath): the estimated-path waypoint block vs a
			// live GetPathWayPoints read. hasPath==1 iff a ground move type with an
			// active pathID; points bit-compared (float3), starts exact. Pure const
			// read, matches ExtractUnitMoveType's ground branch.
			{
				uint8_t liveHasPath = 0;
				std::vector<float3> livePoints;
				std::vector<int> liveStarts;
				if (const CGroundMoveType* g = dynamic_cast<const CGroundMoveType*>(u->moveType); g != nullptr) {
					if (const unsigned int pathID = g->GetPathID(); pathID != 0) {
						liveHasPath = 1;
						pathManager->GetPathWayPoints(pathID, livePoints, liveStarts);
					}
				}
				bool estPathEqual = (rows.estPathHasPath[i] == liveHasPath)
					&& (rows.estPathPoints[i].size() == livePoints.size())
					&& (rows.estPathStarts[i].size() == liveStarts.size());
				for (size_t k = 0; estPathEqual && k < livePoints.size(); ++k)
					estPathEqual = BitEqual(rows.estPathPoints[i][k], livePoints[k]);
				for (size_t k = 0; estPathEqual && k < liveStarts.size(); ++k)
					estPathEqual = (rows.estPathStarts[i][k] == liveStarts[k]);
				if (Bump(fields[D_ESTPATH], estPathEqual))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:estPath mismatch (hasPath snap=%d live=%d, points snap=%zu live=%zu)",
						gs->frameNum, id, int(rows.estPathHasPath[i]), int(liveHasPath), rows.estPathPoints[i].size(), livePoints.size());
			}
		}

		// per-allyteam stride rows + the masked-value sweep: for every POV,
		// UnitRows::ErrorVector is the exact masking function the Lua serving
		// twins apply (SimSnapshot.h masking policy) -- diff it against the
		// live formula. The at == -1 iteration exercises the invalid-allyteam
		// branch (a fullRead-less handle without a read allyteam).
		for (int at = -1; at < numAllyTeams; ++at) {
			if (at >= 0) {
				if (Bump(fields[F_LOSSTATUS], rows.losStatusAll[at * maxUnits + i] == u->losStatus[at]))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=losStatusAll[ally %d] snap=0x%02x live=0x%02x",
						gs->frameNum, id, at, rows.losStatusAll[at * maxUnits + i], u->losStatus[at]);

				if (Bump(fields[F_POSERRORBIT], rows.posErrorBits[at * maxUnits + i] == uint8_t(u->GetPosErrorBit(at))))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=posErrorBits[ally %d] snap=%d live=%d",
						gs->frameNum, id, at, int(rows.posErrorBits[at * maxUnits + i]), int(u->GetPosErrorBit(at)));

				if (Bump(fields[F_INRADAR], (rows.inRadarAll[at * maxUnits + i] != 0) == losHandler->InRadar(u, at)))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=inRadarAll[ally %d] snap=%d live=%d",
						gs->frameNum, id, at, int(rows.inRadarAll[at * maxUnits + i]), int(losHandler->InRadar(u, at)));

				// PR 32 LOS unit variants (IsUnitInLos/InAirLos/InJammer answers)
				const bool losVarEqual =
					((rows.unitInLosAll[at * maxUnits + i] != 0) == losHandler->InLos(u, at)) &&
					((rows.unitInAirLosAll[at * maxUnits + i] != 0) == losHandler->InAirLos(u, at)) &&
					((rows.unitInJammerAll[at * maxUnits + i] != 0) == losHandler->InJammer(u, at));
				if (Bump(fields[D_LOSVARIANTS], losVarEqual))
					LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:losVariants[ally %d] mismatch", gs->frameNum, id, at);
			}

			const float3 snapErr = rows.ErrorVector(id, at);
			const float3 liveErr = u->GetErrorVector(at);

			if (Bump(fields[F_MASKEDERRVEC], BitEqual(snapErr, liveErr)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=maskedErrorVec[ally %d] snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
					gs->frameNum, id, at, snapErr.x, snapErr.y, snapErr.z, liveErr.x, liveErr.y, liveErr.z);
		}

		// ---- PR 38c: per-unit rules-params mirror vs live CUnit::modParams ----
		if (Bump(fields[U_RULES], RulesParamsEqual(rows.unitRulesParams[i], u->modParams)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=unit:rules mismatch (snap=%zu live=%zu)",
				gs->frameNum, id, rows.unitRulesParams[i].size(), u->modParams.size());
	}

	CheckProjectileRows();
	CheckFeatureRows();
	CheckTeamPlayerRows();
	CheckGlobalRows();
	CheckMapMirrors();
	CheckCmdQueueRows();
	CheckWeaponRows();
}

void SnapshotDiffGate::CheckProjectileRows()
{
	const SimSnapshot::ProjectileRows& rows = simSnapshot.ReadProjectiles();
	const size_t slots = rows.MaxSlots();
	const int numAllyTeams = rows.numAllyTeams;

	// validity both ways: every valid row must resolve to a live synced
	// projectile and every live one must have a valid row (checked via its id)
	std::vector<uint8_t> liveSeen(slots, 0);
	const auto& pc = projectileHandler.GetActiveProjectiles(true);

	for (size_t i = 0; i < pc.size(); ++i) {
		const CProjectile* p = pc[i];
		const int id = p->id;

		if (static_cast<size_t>(id) < slots)
			liveSeen[id] = 1;

		const bool snapValid = rows.Valid(id);

		if (Bump(fields[P_VALIDITY], snapValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:validity snap=0 live=1",
				gs->frameNum, id);

		if (!snapValid)
			continue;

		if (Bump(fields[P_POS], BitEqual(rows.pos[id], p->pos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:pos snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.pos[id].x, rows.pos[id].y, rows.pos[id].z, p->pos.x, p->pos.y, p->pos.z);

		if (Bump(fields[P_SPEED], BitEqual(rows.speed[id], p->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:speed", gs->frameNum, id);

		if (Bump(fields[P_ALLYTEAM], rows.allyTeam[id] == p->GetAllyteamID()))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:allyTeam snap=%d live=%d",
				gs->frameNum, id, rows.allyTeam[id], p->GetAllyteamID());

		if (Bump(fields[P_OWNERID], rows.ownerID[id] == p->GetOwnerID()))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:ownerID snap=%d live=%d",
				gs->frameNum, id, rows.ownerID[id], p->GetOwnerID());

		if (Bump(fields[P_ISWEAPON], rows.isWeapon[id] == uint8_t(p->weapon)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:isWeapon snap=%d live=%d",
				gs->frameNum, id, int(rows.isWeapon[id]), int(p->weapon));

		// PR 27a projectile rows
		if (Bump(fields[P_DIR], BitEqual(rows.dir[id], p->dir)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:dir snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.dir[id].x, rows.dir[id].y, rows.dir[id].z, p->dir.x, p->dir.y, p->dir.z);

		if (Bump(fields[P_GRAVITY], BitEqual(rows.mygravity[id], p->mygravity)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:gravity snap=%.9g live=%.9g",
				gs->frameNum, id, rows.mygravity[id], p->mygravity);

		if (Bump(fields[P_TEAMID], rows.teamID[id] == static_cast<int32_t>(p->GetTeamID())))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:teamID snap=%d live=%d",
				gs->frameNum, id, rows.teamID[id], int(p->GetTeamID()));

		// PR 34 (spatial/list remainder): projectile radius (GetProjectilesInSphere)
		if (Bump(fields[P_RADIUS], BitEqual(rows.radius[id], p->radius)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:radius snap=%.9g live=%.9g",
				gs->frameNum, id, rows.radius[id], p->radius);

		// PR 41 (GetVisibleProjectiles): draw-cull radius + hitscan membership flag
		if (Bump(fields[P_DRAWRADIUS], BitEqual(rows.drawRadius[id], p->GetDrawRadius())))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:drawRadius snap=%.9g live=%.9g",
				gs->frameNum, id, rows.drawRadius[id], p->GetDrawRadius());

		if (Bump(fields[P_HITSCAN], rows.hitscan[id] == uint8_t(p->hitscan)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:hitscan snap=%d live=%d",
				gs->frameNum, id, int(rows.hitscan[id]), int(p->hitscan));

		{
			int32_t liveTtl = 0;
			uint8_t liveIntercepted = 0;
			if (p->weapon) {
				const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
				liveTtl = wpro->GetTimeToLive();
				liveIntercepted = wpro->IsBeingIntercepted();
			}
			const bool ttlFlagsEqual =
				(rows.ttl[id] == liveTtl) &&
				(rows.intercepted[id] == liveIntercepted) &&
				(rows.isPiece[id] == uint8_t(p->piece));
			if (Bump(fields[P_TTLFLAGS], ttlFlagsEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:ttlFlags mismatch (ttl snap=%d live=%d)",
					gs->frameNum, id, rows.ttl[id], liveTtl);
		}

		// PR 38g (GetProjectileDamages): the flattened DamagesSnap vs the live
		// *wpro->damages (null for non-weapon projectiles => valid==0), field-wise
		// via WpnDamagesEqual (the unit weapon-damages comparator)
		{
			const DynDamageArray* liveDamages =
				p->weapon ? static_cast<const CWeaponProjectile*>(p)->damages : nullptr;
			if (Bump(fields[P_DAMAGES], WpnDamagesEqual(rows.damages[id], liveDamages)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:damages mismatch", gs->frameNum, id);
		}

		// PR 33 piece-projectile params (GetPieceProjectileParams/Name serving)
		{
			int32_t liveExplFlags = 0;
			float liveSpinAngle = 0.0f;
			float liveSpinSpeed = 0.0f;
			float3 liveSpinVec;
			std::string liveName;
			if (p->piece) {
				const CPieceProjectile* ppro = static_cast<const CPieceProjectile*>(p);
				liveExplFlags = ppro->explFlags;
				liveSpinAngle = ppro->spinAngle;
				liveSpinSpeed = ppro->spinSpeed;
				liveSpinVec = ppro->spinVec;
				if (ppro->omp != nullptr)
					liveName = ppro->omp->name;
			}
			const bool pieceEqual =
				(rows.pieceExplFlags[id] == liveExplFlags) &&
				BitEqual(rows.pieceSpinAngle[id], liveSpinAngle) &&
				BitEqual(rows.pieceSpinSpeed[id], liveSpinSpeed) &&
				BitEqual(rows.pieceSpinVec[id], liveSpinVec) &&
				(rows.pieceName[id] == liveName);
			if (Bump(fields[P_PIECEPARAMS], pieceEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:pieceParams mismatch (explFlags snap=%d live=%d)",
					gs->frameNum, id, rows.pieceExplFlags[id], liveExplFlags);
		}

		if (p->weapon) {
			const CWeaponProjectile* wpro = static_cast<const CWeaponProjectile*>(p);
			const WeaponDef* wdef = wpro->GetWeaponDef();
			const int liveWdefID = (wdef != nullptr) ? wdef->id : -1;

			if (Bump(fields[P_WDEFID], rows.weaponDefID[id] == liveWdefID))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:weaponDefID snap=%d live=%d",
					gs->frameNum, id, rows.weaponDefID[id], liveWdefID);

			// same type resolution as the extraction / the live callout
			const CWorldObject* wtgt = wpro->GetTargetObject();
			uint8_t liveType = 0;
			int liveID = 0;
			float3 livePos;
			if (wtgt == nullptr) {
				liveType = 'g';
				livePos = wpro->GetTargetPos();
			} else if (dynamic_cast<const CUnit*>(wtgt) != nullptr) {
				liveType = 'u'; liveID = wtgt->id;
			} else if (dynamic_cast<const CFeature*>(wtgt) != nullptr) {
				liveType = 'f'; liveID = wtgt->id;
			} else if (dynamic_cast<const CWeaponProjectile*>(wtgt) != nullptr) {
				liveType = 'p'; liveID = wtgt->id;
			}

			const bool targetEqual =
				(rows.targetType[id] == liveType) &&
				(rows.targetID[id] == liveID || liveType == 'g') &&
				(BitEqual(rows.targetPos[id], livePos) || liveType != 'g');

			if (Bump(fields[P_TARGET], targetEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:target snapType=%d liveType=%d",
					gs->frameNum, id, int(rows.targetType[id]), int(liveType));
		}

		// masking input: the extraction-time positional-LOS answer per allyteam
		for (int at = 0; at < numAllyTeams; ++at) {
			const bool snapLos = (rows.inLosAll[at * slots + id] != 0);
			const bool liveLos = losHandler->InLos(p->pos, at);

			if (Bump(fields[P_INLOS], snapLos == liveLos))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:inLosAll[ally %d] snap=%d live=%d",
					gs->frameNum, id, at, int(snapLos), int(liveLos));

			// PR 41: the CWorldObject* overload answer (GetVisibleProjectiles filter)
			const bool snapVisLos = (rows.visInLosAll[at * slots + id] != 0);
			const bool liveVisLos = losHandler->InLos(p, at);

			if (Bump(fields[P_VISINLOS], snapVisLos == liveVisLos))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:visInLosAll[ally %d] snap=%d live=%d",
					gs->frameNum, id, at, int(snapVisLos), int(liveVisLos));
		}
	}

	// reverse validity: valid rows with no live projectile
	for (size_t id = 0; id < slots; ++id) {
		if (rows.valid[id] == 0 || liveSeen[id] != 0)
			continue;

		if (Bump(fields[P_VALIDITY], false))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d proj=%d field=proj:validity snap=1 live=0",
				gs->frameNum, int(id));
	}
}

void SnapshotDiffGate::CheckFeatureRows()
{
	const SimSnapshot::FeatureRows& rows = simSnapshot.ReadFeatures();
	const size_t slots = rows.MaxSlots();
	const int numAllyTeams = rows.numAllyTeams;

	// globals the feature visibility mirror depends on
	{
		const bool globalsEqual =
			(rows.featureVisibility == modInfo.featureVisibility) &&
			(rows.gaiaAllyTeam == std::max(0, teamHandler.GaiaAllyTeamID())) &&
			(numAllyTeams == teamHandler.ActiveAllyTeams());
		if (Bump(fields[FT_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=feat:globals mismatch (featVis snap=%d live=%d)",
				gs->frameNum, rows.featureVisibility, modInfo.featureVisibility);
	}

	std::vector<uint8_t> liveSeen(slots, 0);
	const auto& activeIDs = featureHandler.GetActiveFeatureIDs();

	for (const int id : activeIDs) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr)
			continue;

		if (static_cast<size_t>(id) < slots)
			liveSeen[id] = 1;

		const bool snapValid = rows.Valid(id);
		if (Bump(fields[FT_VALIDITY], snapValid)) {
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:validity snap=0 live=1",
				gs->frameNum, id);
			continue;
		}

		if (Bump(fields[FT_POS], BitEqual(rows.pos[id], f->pos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:pos snap=(%.9g,%.9g,%.9g) live=(%.9g,%.9g,%.9g)",
				gs->frameNum, id, rows.pos[id].x, rows.pos[id].y, rows.pos[id].z, f->pos.x, f->pos.y, f->pos.z);

		if (Bump(fields[FT_MIDPOS], BitEqual(rows.midPos[id], f->midPos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:midPos", gs->frameNum, id);
		if (Bump(fields[FT_AIMPOS], BitEqual(rows.aimPos[id], f->aimPos)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:aimPos", gs->frameNum, id);

		if (Bump(fields[FT_RELMIDPOS], BitEqual(rows.relMidPos[id], static_cast<float3>(f->relMidPos))))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:relMidPos", gs->frameNum, id);

		if (Bump(fields[FT_RADIUS], BitEqual(rows.radius[id], f->radius)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:radius snap=%.9g live=%.9g",
				gs->frameNum, id, rows.radius[id], f->radius);

		if (Bump(fields[FT_ALLYTEAM], rows.allyTeam[id] == f->allyteam))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:allyTeam snap=%d live=%d",
				gs->frameNum, id, rows.allyTeam[id], f->allyteam);

		if (Bump(fields[FT_DEFID], rows.defID[id] == f->def->id))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:defID snap=%d live=%d",
				gs->frameNum, id, rows.defID[id], f->def->id);

		const bool flagsEqual =
			(int(rows.alwaysVisible[id]) == int(f->alwaysVisible)) &&
			(int(rows.noSelect[id]) == int(f->noSelect)) &&
			(int(rows.inVoid[id]) == int(f->IsInVoid()));
		if (Bump(fields[FT_FLAGS], flagsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:flags mismatch", gs->frameNum, id);

		if (Bump(fields[FT_SELVOL], ColVolEqual(rows.selVol[id], f->selectionVolume)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:selVol mismatch", gs->frameNum, id);

		// PR 27a feature rows (grouped counters, see the enum comments)
		if (Bump(fields[FT_TEAM], rows.team[id] == f->team))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:team snap=%d live=%d",
				gs->frameNum, id, rows.team[id], f->team);

		const bool scalarsEqual =
			BitEqual(rows.health[id], f->health) &&
			BitEqual(rows.resurrectProgress[id], f->resurrectProgress) &&
			BitEqual(rows.height[id], f->height) &&
			BitEqual(rows.mass[id], f->mass) &&
			(rows.heading[id] == int16_t(f->heading)) &&
			(rows.buildFacing[id] == int16_t(f->buildFacing));
		if (Bump(fields[FT_SCALARS], scalarsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:scalars mismatch", gs->frameNum, id);

		if (Bump(fields[FT_SPEED], BitEqual(rows.speed[id], f->speed)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:speed", gs->frameNum, id);

		{
			const CMatrix44f& fm = f->GetTransformMatrixRef();
			const bool dirMatEqual =
				BitEqual(rows.matXdir[id], fm.GetX()) &&
				BitEqual(rows.matYdir[id], fm.GetY()) &&
				BitEqual(rows.matZdir[id], fm.GetZ());
			if (Bump(fields[FT_DIRMAT], dirMatEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:dirMatrix mismatch", gs->frameNum, id);
		}

		const bool resourcesEqual =
			BitEqual(rows.resources[id].metal, f->resources.metal) &&
			BitEqual(rows.resources[id].energy, f->resources.energy) &&
			BitEqual(rows.defResources[id].metal, f->defResources.metal) &&
			BitEqual(rows.defResources[id].energy, f->defResources.energy) &&
			BitEqual(rows.reclaimLeft[id], f->reclaimLeft) &&
			BitEqual(rows.reclaimTime[id], f->reclaimTime);
		if (Bump(fields[FT_RESOURCES], resourcesEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:resources mismatch", gs->frameNum, id);

		if (Bump(fields[FT_BLOCKINGBITS], rows.blockingBits[id] == PackBlockingBits(f)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:blockingBits snap=0x%02x live=0x%02x",
				gs->frameNum, id, int(rows.blockingBits[id]), int(PackBlockingBits(f)));

		if (Bump(fields[FT_RESURRECT], rows.resurrectDefID[id] == ((f->udef != nullptr) ? f->udef->id : -1)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:resurrect snap=%d live=%d",
				gs->frameNum, id, rows.resurrectDefID[id], (f->udef != nullptr) ? f->udef->id : -1);

		// PR 38g GetFeatureFireTime/GetFeatureSmokeTime (int frame counts)
		if (Bump(fields[FT_FIRESMOKE], rows.fireTime[id] == f->fireTime && rows.smokeTime[id] == f->smokeTime))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:fireSmoke fire(snap=%d live=%d) smoke(snap=%d live=%d)",
				gs->frameNum, id, rows.fireTime[id], f->fireTime, rows.smokeTime[id], f->smokeTime);

		for (int at = 0; at < numAllyTeams; ++at) {
			const bool snapLos = (rows.inLosAll[at * slots + id] != 0);
			const bool liveLos = losHandler->InLos(f->pos, at);
			if (Bump(fields[FT_INLOS], snapLos == liveLos))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:inLosAll[ally %d] snap=%d live=%d",
					gs->frameNum, id, at, int(snapLos), int(liveLos));
		}

		// ---- PR 38c: per-feature rules-params mirror vs live CFeature::modParams ----
		if (Bump(fields[F_RULES], RulesParamsEqual(rows.featureRulesParams[id], f->modParams)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feature:rules mismatch (snap=%zu live=%zu)",
				gs->frameNum, id, rows.featureRulesParams[id].size(), f->modParams.size());
	}

	// reverse validity: valid rows with no live feature
	for (size_t id = 0; id < slots; ++id) {
		if (rows.valid[id] == 0 || liveSeen[id] != 0)
			continue;
		if (Bump(fields[FT_VALIDITY], false))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d feat=%d field=feat:validity snap=1 live=0",
				gs->frameNum, int(id));
	}
}

void SnapshotDiffGate::CheckTeamPlayerRows()
{
	// team/player boundary copy (PR 26): re-extracted unconditionally right
	// before this check, so every compare must trivially pass -- the pass
	// verifies the extraction copies every field the serving twins read
	const SimSnapshot::TeamRows& trows = simSnapshot.ReadTeams();
	const SimSnapshot::PlayerRows& prows = simSnapshot.ReadPlayers();

	{
		const bool globalsEqual =
			(trows.activeTeams == teamHandler.ActiveTeams()) &&
			(trows.activeAllyTeams == teamHandler.ActiveAllyTeams()) &&
			(trows.gaiaTeamID == teamHandler.GaiaTeamID()) &&
			(bool(trows.useLuaGaia) == bool(gs->useLuaGaia)) &&
			(bool(trows.gameOver) == (game != nullptr && game->IsGameOver()));
		if (Bump(fields[T_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=team:globals mismatch (activeTeams snap=%d live=%d)",
				gs->frameNum, trows.activeTeams, teamHandler.ActiveTeams());
	}

	for (int t = 0; t < trows.activeTeams && t < teamHandler.ActiveTeams(); ++t) {
		const CTeam* team = teamHandler.Team(t);

		const bool stateEqual =
			(trows.leader[t] == team->GetLeader()) &&
			(bool(trows.isDead[t]) == team->isDead) &&
			(bool(trows.hasAIs[t]) == skirmishAIHandler.HasSkirmishAIsInTeam(t)) &&
			(trows.allyTeam[t] == teamHandler.AllyTeam(t)) &&
			BitEqual(trows.incomeMultiplier[t], team->GetIncomeMultiplier()) &&
			(trows.numUnits[t] == static_cast<int32_t>(unitHandler.NumUnitsByTeam(t)));
		if (Bump(fields[T_STATE], stateEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:state mismatch", gs->frameNum, t);

		const auto packEqual = [](const SResourcePack& a, const SResourcePack& b) {
			return BitEqual(a.metal, b.metal) && BitEqual(a.energy, b.energy);
		};
		const bool resEqual =
			packEqual(trows.res[t], team->res) &&
			packEqual(trows.resStorage[t], team->resStorage) &&
			packEqual(trows.resPrevPull[t], team->resPrevPull) &&
			packEqual(trows.resPrevIncome[t], team->resPrevIncome) &&
			packEqual(trows.resPrevExpense[t], team->resPrevExpense) &&
			packEqual(trows.resShare[t], team->resShare) &&
			packEqual(trows.resPrevSent[t], team->resPrevSent) &&
			packEqual(trows.resPrevReceived[t], team->resPrevReceived) &&
			packEqual(trows.resPrevExcess[t], team->resPrevExcess);
		if (Bump(fields[T_RES], resEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:res mismatch (metal snap=%.9g live=%.9g)",
				gs->frameNum, t, trows.res[t].metal, team->res.metal);

		// TeamStatistics is #pragma pack(1): no padding, memcmp-safe
		if (Bump(fields[T_STATS], std::memcmp(&trows.currentStats[t], &team->GetCurrentStats(), sizeof(TeamStatistics)) == 0))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:stats mismatch", gs->frameNum, t);

		const bool colorEqual =
			(std::memcmp(trows.color[t].data(), team->color, 4) == 0) &&
			(std::memcmp(trows.origColor[t].data(), team->origColor, 4) == 0);
		if (Bump(fields[T_COLOR], colorEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:color mismatch", gs->frameNum, t);

		const bool stringsEqual =
			(trows.sideName[t] == team->GetSideName()) &&
			OptsEqual(trows.customOpts[t], team->GetAllValues());
		if (Bump(fields[T_STRINGS], stringsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:strings mismatch", gs->frameNum, t);

		// ---- PR 36: team-misc (startPos/hasValidStartPos/maxUnits + luaAIName +
		// teamAIs[0] block + full statHistory) ----
		const std::vector<uint8_t>& teamAIs = skirmishAIHandler.GetSkirmishAIsInTeam(t);
		std::string liveLuaAIName;
		bool liveHasLuaAI = false;
		for (uint8_t id: teamAIs) {
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(id);
			if (!aiData->isLuaAI)
				continue;
			liveLuaAIName = aiData->shortName;
			liveHasLuaAI = true;
			break;
		}
		bool miscEqual =
			BitEqual(trows.startPos[t], team->GetStartPos()) &&
			(bool(trows.hasValidStartPos[t]) == team->HasValidStartPos()) &&
			(trows.maxUnits[t] == static_cast<int32_t>(team->GetMaxUnits())) &&
			(bool(trows.hasLuaAI[t]) == liveHasLuaAI) &&
			(!liveHasLuaAI || trows.luaAIName[t] == liveLuaAIName) &&
			(trows.statHistory[t].size() == team->statHistory.size());
		if (miscEqual && !team->statHistory.empty()) // TeamStatistics is pack(1), memcmp-safe
			miscEqual = (std::memcmp(trows.statHistory[t].data(), team->statHistory.data(),
				team->statHistory.size() * sizeof(TeamStatistics)) == 0);
		if (teamAIs.empty()) {
			miscEqual = miscEqual && (trows.aiHasAI[t] == 0);
		} else {
			const size_t skirmishAIId = teamAIs[0];
			const SkirmishAIData* aiData = skirmishAIHandler.GetSkirmishAI(skirmishAIId);
			const bool isLocal = skirmishAIHandler.IsLocalSkirmishAI(skirmishAIId);
			miscEqual = miscEqual &&
				(trows.aiHasAI[t] != 0) &&
				(trows.aiID[t] == static_cast<int32_t>(skirmishAIId)) &&
				(trows.aiName[t] == aiData->name) &&
				(trows.aiHostPlayer[t] == aiData->hostPlayer) &&
				(bool(trows.aiIsLocal[t]) == isLocal);
			if (isLocal) {
				miscEqual = miscEqual &&
					(trows.aiShortName[t] == aiData->shortName) &&
					(trows.aiVersion[t] == aiData->version) &&
					OptsEqual(trows.aiOptions[t], aiData->options);
			}
		}
		if (Bump(fields[T_MISC], miscEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:misc mismatch", gs->frameNum, t);

		// ---- PR 38: per-team rules-params mirror vs live CTeam::modParams ----
		if (Bump(fields[T_RULES], RulesParamsEqual(trows.teamRulesParams[t], team->modParams)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d team=%d field=team:rules mismatch (snap=%zu live=%zu)",
				gs->frameNum, t, trows.teamRulesParams[t].size(), team->modParams.size());
	}

	// PR 36: per-allyteam start box + custom options (GetAllyTeamStartBox /
	// GetAllyTeamInfo); recomputes the live float expression bit-for-bit
	for (int at = 0; at < trows.activeAllyTeams && at < teamHandler.ActiveAllyTeams(); ++at) {
		const AllyTeam& ally = teamHandler.GetAllyTeam(at);
		const float4 liveBox(
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectLeft,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectTop,
			(mapDims.mapx * SQUARE_SIZE) * ally.startRectRight,
			(mapDims.mapy * SQUARE_SIZE) * ally.startRectBottom);
		const bool allyEqual =
			BitEqual(trows.allyStartBox[at], liveBox) &&
			OptsEqual(trows.allyTeamOpts[at], ally.GetAllValues());
		if (Bump(fields[T_ALLYINFO], allyEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d allyTeam=%d field=team:allyInfo mismatch", gs->frameNum, at);
	}

	{
		const bool globalsEqual =
			(prows.activePlayers == static_cast<int32_t>(playerHandler.ActivePlayers())) &&
			(bool(prows.hostDemo) == gameSetup->hostDemo);
		if (Bump(fields[PL_GLOBALS], globalsEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=player:globals mismatch (activePlayers snap=%d live=%d)",
				gs->frameNum, prows.activePlayers, int(playerHandler.ActivePlayers()));
	}

	for (int p = 0; p < prows.activePlayers && p < static_cast<int>(playerHandler.ActivePlayers()); ++p) {
		const CPlayer* player = playerHandler.Player(p);

		const bool infoEqual =
			(prows.name[p] == player->name) &&
			(prows.countryCode[p] == player->countryCode) &&
			(prows.rank[p] == player->rank) &&
			(bool(prows.isFromDemo[p]) == player->isFromDemo);
		if (Bump(fields[PL_INFO], infoEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:info mismatch", gs->frameNum, p);

		const bool stateEqual =
			(bool(prows.active[p]) == player->active) &&
			(bool(prows.spectator[p]) == player->spectator) &&
			(prows.team[p] == player->team) &&
			(bool(prows.desynced[p]) == player->desynced);
		if (Bump(fields[PL_STATE], stateEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:state mismatch", gs->frameNum, p);

		const bool netEqual =
			(prows.ping[p] == player->ping) &&
			BitEqual(prows.cpuUsage[p], player->cpuUsage);
		if (Bump(fields[PL_NET], netEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:net mismatch (ping snap=%d live=%d)",
				gs->frameNum, p, prows.ping[p], player->ping);

		if (Bump(fields[PL_OPTS], OptsEqual(prows.customOpts[p], player->GetAllValues())))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:opts mismatch", gs->frameNum, p);

		// ---- PR 36: GetPlayerControlledUnit + GetPlayerStatistics ----
		const CUnit* controllee = player->fpsController.GetControllee();
		const int liveControlleeID = (controllee != nullptr) ? controllee->id : -1;
		const int liveControlleeAllyTeam = (controllee != nullptr) ? controllee->allyteam : -1;
		const PlayerStatistics& lps = player->currentStats;
		const PlayerStatistics& sps = prows.currentStats[p];
		const bool miscEqual =
			(prows.controlleeID[p] == liveControlleeID) &&
			(prows.controlleeAllyTeam[p] == liveControlleeAllyTeam) &&
			(sps.mousePixels == lps.mousePixels) &&
			(sps.mouseClicks == lps.mouseClicks) &&
			(sps.keyPresses == lps.keyPresses) &&
			(sps.numCommands == lps.numCommands) &&
			(sps.unitCommands == lps.unitCommands);
		if (Bump(fields[PL_MISC], miscEqual))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:misc mismatch", gs->frameNum, p);

		// ---- PR 38c: per-player rules-params mirror vs live CPlayer::modParams ----
		if (Bump(fields[PL_RULES], RulesParamsEqual(prows.playerRulesParams[p], player->modParams)))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d player=%d field=player:rules mismatch (snap=%zu live=%zu)",
				gs->frameNum, p, prows.playerRulesParams[p].size(), player->modParams.size());
	}
}

void SnapshotDiffGate::CheckGlobalRows()
{
	// global-scalar boundary copy (PR 27a): re-extracted unconditionally right
	// before this check, so every compare must trivially pass -- the pass
	// verifies the extraction copies every scalar the serving twins read
	const SimSnapshot::GlobalRows& grows = simSnapshot.ReadGlobals();

	if (Bump(fields[G_FRAME], grows.luaSimFrame == gs->GetLuaSimFrame()))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:frame mismatch (snap=%d live=%d)",
			gs->frameNum, grows.luaSimFrame, gs->GetLuaSimFrame());

	const bool speedEqual =
		BitEqual(grows.wantedSpeedFactor, gs->wantedSpeedFactor) &&
		BitEqual(grows.speedFactor, gs->speedFactor) &&
		(bool(grows.paused) == gs->paused);
	if (Bump(fields[G_SPEED], speedEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:speed mismatch", gs->frameNum);

	const bool flagsEqual =
		(bool(grows.cheatEnabled) == gs->cheatEnabled) &&
		(grows.godMode == gs->godMode) &&
		(bool(grows.editDefsEnabled) == gs->editDefsEnabled) &&
		(bool(grows.noHelperAIs) == gs->noHelperAIs) &&
		(bool(grows.defsNoCost) == (unitDefHandler != nullptr && unitDefHandler->GetNoCost())) &&
		(bool(grows.doneLoading) == (game != nullptr && game->IsDoneLoading())) &&
		(bool(grows.savedGame) == (game != nullptr && game->IsSavedGame())) &&
		(bool(grows.clientPaused) == (game != nullptr && game->IsClientPaused()));
	if (Bump(fields[G_FLAGS], flagsEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:flags mismatch", gs->frameNum);

	const bool windEqual =
		BitEqual(grows.windVec.x, envResHandler.GetCurrentWindVec().x) &&
		BitEqual(grows.windVec.y, envResHandler.GetCurrentWindVec().y) &&
		BitEqual(grows.windVec.z, envResHandler.GetCurrentWindVec().z) &&
		BitEqual(grows.windDir.x, envResHandler.GetCurrentWindDir().x) &&
		BitEqual(grows.windDir.y, envResHandler.GetCurrentWindDir().y) &&
		BitEqual(grows.windDir.z, envResHandler.GetCurrentWindDir().z) &&
		BitEqual(grows.windStrength, envResHandler.GetCurrentWindStrength());
	if (Bump(fields[G_WIND], windEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:wind mismatch", gs->frameNum);

	const bool heightsEqual =
		BitEqual(grows.initMinHeight, readMap->GetInitMinHeight()) &&
		BitEqual(grows.initMaxHeight, readMap->GetInitMaxHeight()) &&
		BitEqual(grows.currMinHeight, readMap->GetCurrMinHeight()) &&
		BitEqual(grows.currMaxHeight, readMap->GetCurrMaxHeight());
	if (Bump(fields[G_HEIGHTS], heightsEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:heights mismatch", gs->frameNum);

	bool losEqual = (grows.numAllyTeams == teamHandler.ActiveAllyTeams());
	for (int at = 0; losEqual && at < grows.numAllyTeams; ++at)
		losEqual = (bool(grows.globalLos[at]) == losHandler->GetGlobalLOS(at));
	if (Bump(fields[G_GLOBALLOS], losEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:globalLos mismatch", gs->frameNum);

	// ---- PR 38: game rules-params mirror vs CSplitLuaHandle::GetGameParams ----
	if (Bump(fields[G_GAMERULES], RulesParamsEqual(grows.gameRulesParams, CSplitLuaHandle::GetGameParams())))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=glob:gameRules mismatch (snap=%zu live=%zu)",
			gs->frameNum, grows.gameRulesParams.size(), CSplitLuaHandle::GetGameParams().size());
}

// PR 28: the DrawMapMirrors compare pass. Drained at the barrier (before the
// snapshot publish) and nothing runs sim between there and here, so every
// mirror must bit-match the live source; a mismatch means a missed choke-point
// dirty mark (the deterministic detector the mirror-verification rule asks for).
void SnapshotDiffGate::CheckMapMirrors()
{
	if (!drawMapMirrors.Ready() || losHandler == nullptr || readMap == nullptr || mapInfo == nullptr)
		return;

	// --- LOS layers (per losType, per allyTeam whole-map memcmp) ---
	const ILosType* lts[DrawMapMirrors::LOS_MIRROR_TYPE_COUNT];
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_LOS]          = &losHandler->los;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_AIRLOS]       = &losHandler->airLos;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_RADAR]        = &losHandler->radar;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SONAR]        = &losHandler->sonar;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_JAMMER]       = &losHandler->jammer;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SEISMIC]      = &losHandler->seismic;
	lts[DrawMapMirrors::LOS_MIRROR_TYPE_SONAR_JAMMER] = &losHandler->sonarJammer;

	for (int t = 0; t < DrawMapMirrors::LOS_MIRROR_TYPE_COUNT; ++t) {
		const int nAlly = static_cast<int>(lts[t]->losMaps.size());
		for (int at = 0; at < nAlly; ++at) {
			const std::vector<uint16_t>* mm = drawMapMirrors.LosMap(t, at);
			const auto& live = lts[t]->losMaps[at].GetLosMap();

			bool eq = (mm != nullptr) && (mm->size() == live.size()) &&
				(mm->empty() || std::memcmp(mm->data(), live.data(), mm->size() * sizeof(uint16_t)) == 0);
			if (Bump(fields[MM_LOS], eq))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:los mismatch (type=%d ally=%d)",
					gs->frameNum, t, at);
		}
	}

	// --- terrain-type table ---
	bool ttEqual = (drawMapMirrors.TerrainTypeCount() == CMapInfo::NUM_TERRAIN_TYPES);
	for (int i = 0; ttEqual && i < CMapInfo::NUM_TERRAIN_TYPES; ++i) {
		const DrawMapMirrors::TerrainType& m = drawMapMirrors.TerrainTypeAt(i);
		const CMapInfo::TerrainType& l = mapInfo->terrainTypes[i];
		ttEqual =
			(m.name == l.name) &&
			BitEqual(m.hardness, l.hardness) &&
			BitEqual(m.tankSpeed, l.tankSpeed) &&
			BitEqual(m.kbotSpeed, l.kbotSpeed) &&
			BitEqual(m.hoverSpeed, l.hoverSpeed) &&
			BitEqual(m.shipSpeed, l.shipSpeed) &&
			(m.receiveTracks == l.receiveTracks);
	}
	if (Bump(fields[MM_TERRAINTYPES], ttEqual))
		LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:terrainTypes mismatch", gs->frameNum);

	// --- smooth-height mesh ---
	{
		const std::vector<float>& mm = drawMapMirrors.SmoothMeshData();
		const size_t n = static_cast<size_t>(smoothGround.GetMaxX()) * static_cast<size_t>(smoothGround.GetMaxY());
		const bool eq = (mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), smoothGround.GetMeshData(), n * sizeof(float)) == 0);
		if (Bump(fields[MM_SMOOTHMESH], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:smoothMesh mismatch", gs->frameNum);
	}

	// --- original heightmap ---
	{
		const std::vector<float>& mm = drawMapMirrors.OrigHeightMap();
		const size_t n = static_cast<size_t>(mapDims.mapxp1) * static_cast<size_t>(mapDims.mapyp1);
		const bool eq = (mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), readMap->GetOriginalHeightMapSynced(), n * sizeof(float)) == 0);
		if (Bump(fields[MM_ORIGHEIGHT], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:origHeight mismatch", gs->frameNum);
	}

	// --- radar-error scalars ---
	{
		bool eq =
			(drawMapMirrors.NumAllyTeams() == teamHandler.ActiveAllyTeams()) &&
			BitEqual(drawMapMirrors.BaseRadarErrorSize(), losHandler->GetBaseRadarErrorSize()) &&
			BitEqual(drawMapMirrors.BaseRadarErrorMult(), losHandler->GetBaseRadarErrorMult());
		for (int at = 0; eq && at < drawMapMirrors.NumAllyTeams(); ++at)
			eq = BitEqual(drawMapMirrors.AllyTeamRadarErrorSize(at), losHandler->GetAllyTeamRadarErrorSize(at));
		if (Bump(fields[MM_RADARERR], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:radarError mismatch", gs->frameNum);
	}

	// --- blocking-map mirror (PR 29): per-square cell[0] id + kind ---
	// A mismatch means a missed MarkBlockingDirty choke point (a writer that
	// changed a cell[0] without re-walking the mirror). One MM_BLOCKING counter
	// over the whole grid; report the first diverging square.
	{
		const size_t nSquares = static_cast<size_t>(mapDims.mapx) * static_cast<size_t>(mapDims.mapy);
		const std::vector<int32_t>& mid = drawMapMirrors.BlockIds();
		const std::vector<uint8_t>& mkind = drawMapMirrors.BlockKinds();

		bool eq = (mid.size() == nSquares) && (mkind.size() == nSquares);
		size_t badSq = 0;
		int liveId = -1, mirId = -1;

		for (size_t sq = 0; eq && sq < nSquares; ++sq) {
			const CSolidObject* s = groundBlockingObjectMap.GroundBlockedUnsafe(static_cast<unsigned int>(sq));

			int32_t lid = -1;
			uint8_t lkind = DrawMapMirrors::BLOCK_KIND_NONE;
			if (s != nullptr) {
				if (const CFeature* f = dynamic_cast<const CFeature*>(s)) {
					lid = f->id;
					lkind = DrawMapMirrors::BLOCK_KIND_FEATURE;
				} else if (const CUnit* u = dynamic_cast<const CUnit*>(s)) {
					lid = u->id;
					lkind = DrawMapMirrors::BLOCK_KIND_UNIT;
				}
			}

			if (mid[sq] != lid || mkind[sq] != lkind) {
				eq = false;
				badSq = sq;
				liveId = lid;
				mirId = mid[sq];
			}
		}

		if (Bump(fields[MM_BLOCKING], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:blocking mismatch (square=%zu mirror-id=%d live-id=%d)",
				gs->frameNum, badSq, mirId, liveId);
	}

	// --- typemap (PR 38d): per-square terrain-type index array ---
	// A mismatch means a missed MarkTypeMapDirty choke point (a typeMap writer
	// that did not bump the version). memcmp vs readMap->GetTypeMapSynced().
	{
		const std::vector<uint8_t>& mm = drawMapMirrors.TypeMapData();
		const size_t n = static_cast<size_t>(mapDims.hmapx) * static_cast<size_t>(mapDims.hmapy);
		const bool eq = (mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), readMap->GetTypeMapSynced(), n * sizeof(uint8_t)) == 0);
		if (Bump(fields[MM_TYPEMAP], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:typeMap mismatch", gs->frameNum);
	}

	// --- metal distribution map (PR 38d): distributionMap + sizeX/sizeZ/scale ---
	// A mismatch means a missed MarkMetalMapDirty choke point. memcmp of the
	// distribution map + a bit-compare of the Init-time sizeX/sizeZ/metalScale
	// (which the mirror also captures each drain).
	{
		const std::vector<uint8_t>& mm = drawMapMirrors.MetalDistributionData();
		const int sx = metalMap.GetSizeX();
		const int sz = metalMap.GetSizeZ();
		const size_t n = static_cast<size_t>(sx) * static_cast<size_t>(sz);
		const bool eq =
			(drawMapMirrors.MetalSizeX() == sx) &&
			(drawMapMirrors.MetalSizeZ() == sz) &&
			BitEqual(drawMapMirrors.MetalScale(), metalMap.GetMetalScale()) &&
			(mm.size() == n) &&
			(mm.empty() || std::memcmp(mm.data(), metalMap.GetDistributionMap(), n * sizeof(uint8_t)) == 0);
		if (Bump(fields[MM_METALMAP], eq))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d field=map:metalMap mismatch", gs->frameNum);
	}
}

// sim|draw PR 30 mirror-verification: the command-queue/cmd-desc serving cache is
// rebuilt (LuaSnapshotServe::RefreshCommandQueues) right before this check at the
// same boundary, so every compare must trivially pass -- a missed dirty-mark
// (queue version / cmdDescVersion / worker re-decode) becomes a deterministic
// gate failure. The per-slot compare lives in LuaSnapshotServe (where the cache
// is a file-static and the sim includes are already present, and where it binds
// live queues through const refs so the version-bumping accessors are never
// invoked); this pass just drives it over every unit id and tallies the fields.
void SnapshotDiffGate::CheckCmdQueueRows()
{
	const size_t maxUnits = unitHandler.MaxUnits();

	for (size_t id = 0; id < maxUnits; ++id) {
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits
		const bool liveValid = (u != nullptr);

		const LuaSnapshotServe::CmdQueueCompareResult r = LuaSnapshotServe::CompareCmdQueueSlot(int(id), u);

		if (Bump(fields[CQ_PRESENCE], r.present == liveValid))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:presence snap=%d live=%d",
				gs->frameNum, int(id), int(r.present), int(liveValid));

		if (!r.present || !liveValid)
			continue;

		if (Bump(fields[CQ_QUEUE], r.queueOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:queue mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_DESCS], r.descsOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:descs mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_WORKER], r.workerOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:worker mismatch", gs->frameNum, int(id));

		if (Bump(fields[CQ_FACTORY], r.factoryOk))
			LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=cq:factory mismatch", gs->frameNum, int(id));
	}
}

// PR 31 mirror-verification: the UnitRows weapon block was extracted at
// simSnapshot.Update() this same boundary, so every compare must trivially pass;
// a missed/torn weapon field becomes a deterministic gate failure. Mirrors
// SimSnapshot::Extract's two weapon passes exactly (same live reads, same order,
// same flatten). The flattened damage arrays compare field-wise against the live
// DynDamageArray (avoids the null-vs-empty and pointer-identity traps).
static bool WpnDamagesEqual(const SimSnapshot::UnitRows::DamagesSnap& s, const DynDamageArray* live)
{
	if ((s.valid != 0) != (live != nullptr))
		return false;
	if (live == nullptr)
		return true;

	if (s.paralyzeDamageTime != live->paralyzeDamageTime)  return false;
	if (!BitEqual(s.impulseFactor, live->impulseFactor))   return false;
	if (!BitEqual(s.impulseBoost, live->impulseBoost))     return false;
	if (!BitEqual(s.craterMult, live->craterMult))         return false;
	if (!BitEqual(s.craterBoost, live->craterBoost))       return false;
	if (!BitEqual(s.dynDamageExp, live->dynDamageExp))     return false;
	if (!BitEqual(s.dynDamageMin, live->dynDamageMin))     return false;
	if (!BitEqual(s.dynDamageRange, live->dynDamageRange)) return false;
	if ((s.dynDamageInverted != 0) != live->dynDamageInverted)          return false;
	if (!BitEqual(s.craterAreaOfEffect, live->craterAreaOfEffect))      return false;
	if (!BitEqual(s.damageAreaOfEffect, live->damageAreaOfEffect))      return false;
	if (!BitEqual(s.edgeEffectiveness, live->edgeEffectiveness))        return false;
	if (!BitEqual(s.explosionSpeed, live->explosionSpeed))              return false;
	if (int(s.damages.size()) != live->GetNumTypes())     return false;
	for (int i = 0; i < live->GetNumTypes(); ++i)
		if (!BitEqual(s.damages[i], live->Get(i)))        return false;

	return true;
}

void SnapshotDiffGate::CheckWeaponRows()
{
	const SimSnapshot::UnitRows& rows = simSnapshot.Read();
	const size_t maxUnits = rows.MaxUnits();

	for (size_t i = 0; i < maxUnits; ++i) {
		const int id = static_cast<int>(i);
		const CUnit* u = unitHandler.GetUnitUnsafe(id); // id < maxUnits

		// weapon rows share UnitRows validity; a validity mismatch is already
		// reported by the main unit field pass, so only cross-check both-valid
		if (u == nullptr || rows.valid[i] == 0)
			continue;

		// per-unit weapon block (flanking / stockpile / shield-default / fps)
		{
			const CPlayer* fpsPlayer = u->fpsControlPlayer;
			const bool liveFpsNoFire = (fpsPlayer != nullptr && !fpsPlayer->fpsController.mouse1 && !fpsPlayer->fpsController.mouse2);
			const CWeapon* stockpile = u->stockpileWeapon;
			const CPlasmaRepulser* shield = static_cast<const CPlasmaRepulser*>(u->shieldWeapon);

			const bool unitEqual =
				(rows.weaponCount[i] == int32_t(u->weapons.size())) &&
				BitEqual(rows.reloadSpeed[i], u->reloadSpeed) &&
				((rows.fpsNoFire[i] != 0) == liveFpsNoFire) &&
				(rows.flankingMode[i] == u->flankingBonusMode) &&
				BitEqual(rows.flankingDir[i], u->flankingBonusDir) &&
				BitEqual(rows.flankingMoveFactor[i], u->flankingBonusMobilityAdd) &&
				BitEqual(rows.flankingAvgDamage[i], u->flankingBonusAvgDamage) &&
				BitEqual(rows.flankingDifDamage[i], u->flankingBonusDifDamage) &&
				BitEqual(rows.flankingMobility[i], u->flankingBonusMobility) &&
				((rows.hasStockpile[i] != 0) == (stockpile != nullptr)) &&
				(rows.stockpileNumStockpiled[i] == (stockpile != nullptr ? stockpile->numStockpiled : 0)) &&
				(rows.stockpileNumQueued[i] == (stockpile != nullptr ? stockpile->numStockpileQued : 0)) &&
				BitEqual(rows.stockpileBuildPercent[i], (stockpile != nullptr ? stockpile->buildPercent : 0.0f)) &&
				((rows.hasShieldWeapon[i] != 0) == (shield != nullptr)) &&
				((rows.shieldWeaponEnabled[i] != 0) == (shield != nullptr ? shield->IsEnabled() : false)) &&
				BitEqual(rows.shieldWeaponPower[i], (shield != nullptr ? shield->GetCurPower() : 0.0f));
			if (Bump(fields[W_UNIT], unitEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=wpn:unit mismatch", gs->frameNum, id);

			const bool udEqual =
				WpnDamagesEqual(rows.deathExpDamages[i], u->deathExpDamages) &&
				WpnDamagesEqual(rows.selfdExpDamages[i], u->selfdExpDamages);
			if (Bump(fields[W_UNITDAMAGES], udEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d field=wpn:unitDamages mismatch", gs->frameNum, id);
		}

		// per-weapon block
		const int base = rows.weaponOffset[i];
		for (size_t w = 0; w < u->weapons.size(); ++w) {
			const CWeapon* weapon = u->weapons[w];
			const WeaponDef* wdef = weapon->weaponDef;
			const int wi = base + static_cast<int>(w);

			const bool stateEqual =
				((rows.wAngleGood[wi] != 0) == weapon->angleGood) &&
				(rows.wReloadStatus[wi] == weapon->reloadStatus) &&
				(rows.wSalvoLeft[wi] == weapon->salvoLeft) &&
				(rows.wNumStockpiled[wi] == weapon->numStockpiled) &&
				(rows.wNextSalvo[wi] == weapon->nextSalvo) &&
				(rows.wReloadTime[wi] == weapon->reloadTime) &&
				(rows.wReaimTime[wi] == weapon->reaimTime) &&
				BitEqual(rows.wAccuracyExp[wi], weapon->AccuracyExperience()) &&
				BitEqual(rows.wSprayAngleExp[wi], weapon->SprayAngleExperience()) &&
				BitEqual(rows.wSalvoError[wi], weapon->SalvoErrorExperience()) &&
				BitEqual(rows.wMoveErrorExp[wi], weapon->MoveErrorExperience()) &&
				BitEqual(rows.wRange[wi], weapon->range) &&
				BitEqual(rows.wProjectileSpeed[wi], weapon->projectileSpeed) &&
				BitEqual(rows.wAutoTargetRangeBoost[wi], weapon->autoTargetRangeBoost) &&
				(rows.wSalvoSize[wi] == weapon->salvoSize) &&
				(rows.wSalvoDelay[wi] == weapon->salvoDelay) &&
				(rows.wSalvoWindup[wi] == weapon->salvoWindup) &&
				(rows.wProjectilesPerShot[wi] == weapon->projectilesPerShot) &&
				(rows.wAvoidFlags[wi] == weapon->avoidFlags) &&
				(rows.wCollisionFlags[wi] == weapon->collisionFlags) &&
				(rows.wTtl[wi] == weapon->ttl);
			if (Bump(fields[W_STATE], stateEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d weapon=%d field=wpn:state mismatch", gs->frameNum, id, int(w));

			const bool vecEqual =
				BitEqual(rows.wMuzzlePos[wi], weapon->weaponMuzzlePos) &&
				BitEqual(rows.wWantedDir[wi], weapon->wantedDir) &&
				BitEqual(rows.wWeaponDir[wi], weapon->weaponDir) &&
				(rows.wProjectileType[wi] == int32_t(wdef->projectileType)) &&
				((rows.wDefStockpile[wi] != 0) == wdef->stockpile) &&
				((rows.wDefFireSubmersed[wi] != 0) == wdef->fireSubmersed) &&
				BitEqual(rows.wDefMaxFireAngle[wi], wdef->maxFireAngle) &&
				((rows.wIsBombDropper[wi] != 0) == (dynamic_cast<const CBombDropper*>(weapon) != nullptr)) &&
				BitEqual(rows.wAimFromPosY[wi], weapon->aimFromPos.y) &&
				BitEqual(rows.wLastRequestedDir[wi], weapon->lastRequestedDir);
			if (Bump(fields[W_VECTORS], vecEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d weapon=%d field=wpn:vectors mismatch", gs->frameNum, id, int(w));

			const SWeaponTarget& tgt = weapon->GetCurrentTarget();
			const int liveTgtUnit = (tgt.type == Target_Unit && tgt.unit != nullptr) ? tgt.unit->id : 0;
			const int liveTgtInt  = (tgt.type == Target_Intercept && tgt.intercept != nullptr) ? tgt.intercept->id : 0;
			const float3 liveTgtPos = (tgt.type == Target_Pos) ? tgt.groundPos : ZeroVector;
			const bool tgtEqual =
				(rows.wTargetType[wi] == uint8_t(tgt.type)) &&
				((rows.wTargetIsUser[wi] != 0) == tgt.isUserTarget) &&
				(rows.wTargetUnitID[wi] == liveTgtUnit) &&
				(rows.wTargetInterceptID[wi] == liveTgtInt) &&
				BitEqual(rows.wTargetGroundPos[wi], liveTgtPos);
			if (Bump(fields[W_TARGET], tgtEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d weapon=%d field=wpn:target mismatch", gs->frameNum, id, int(w));

			const CPlasmaRepulser* repulser = dynamic_cast<const CPlasmaRepulser*>(weapon);
			const bool shEqual =
				((rows.wIsShield[wi] != 0) == (repulser != nullptr)) &&
				((rows.wShieldEnabled[wi] != 0) == (repulser != nullptr ? repulser->IsEnabled() : false)) &&
				BitEqual(rows.wShieldPower[wi], repulser != nullptr ? repulser->GetCurPower() : 0.0f);
			if (Bump(fields[W_SHIELD], shEqual))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d weapon=%d field=wpn:shield mismatch", gs->frameNum, id, int(w));

			if (Bump(fields[W_DAMAGES], WpnDamagesEqual(rows.wDamages[wi], weapon->damages)))
				LOG_L(L_ERROR, "[SnapshotDiffGate] frame=%d unit=%d weapon=%d field=wpn:damages mismatch", gs->frameNum, id, int(w));
		}
	}
}
