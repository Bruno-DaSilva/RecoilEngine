/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

struct lua_State;

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
}
