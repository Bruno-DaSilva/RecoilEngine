/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

class CUnit;

// WS-3 (write-through mirrors, doc/sim-draw-split-optimization/
// write-through-mirrors.md): sim-side mutation-choke notifications into
// SimSnapshot's producer-owned live column store. Free functions (implemented
// in SimSnapshot.cpp) so choke sites carry no SimSnapshot.h include; each
// helper recomputes its column family from the unit's authoritative members
// and bumps the touched per-column mutation counters. All callers execute in
// synced-sim / net-consumption context (single-writer, see the LiveUnitStore
// threading comment in SimSnapshot.h); none of this is synced state -- the
// store is render-side and nothing synced reads it.
namespace SimSnapshotWT {
	// unit-creation completion choke (end of CUnitLoader::LoadUnit, after
	// PostInit): full-row init of every migrated column for this id. Also the
	// id-reuse story -- a reused id's creation rewrites all its entries and
	// bumps every column counter before the new object can be published.
	void UnitCreated(const CUnit* unit);

	// creg/checkpoint load: creg-constructed units bypass UnitCreated, so the
	// whole store is rebuilt from the live objects (after all PostLoads).
	void RebuildLiveStore();

	// per-family chokes
	void NoteFlanking(const CUnit* unit);         // all six flanking columns (rare sites)
	void NoteFlankingMobility(const CUnit* unit); // per-frame-hot single column (CUnit::Update)
	void NoteReloadSpeed(const CUnit* unit);
	void NoteFpsControl(const CUnit* unit);       // fpsNoFire recompute at its inputs' chokes
	void NoteStockpile(const CUnit* unit);        // stockpile block recompute (weapon->owner)
	void NoteShieldState(const CUnit* unit);      // shield block recompute (weapon->owner)
	void NoteWeaponDamages(const CUnit* unit);    // deep pair: store copy + per-slot pending push
}
