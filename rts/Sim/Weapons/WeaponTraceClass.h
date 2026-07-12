/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

/**
 * WeaponClass -- the draw-side virtual-dispatch discriminator for the sim/draw
 * split trace re-host. The sim thread runs the real virtual CWeapon predicates;
 * the epoch backend has no live CWeapon* to dispatch through, so the extractor
 * classifies each weapon into this enum (SimSnapshot's wWeaponClass row) and the
 * templated predicate stack (WeaponPredicates.h) switches on it to select the
 * right override body. Only classes that override a trace-reachable predicate
 * (TestTarget / TestRange / HaveFreeLineOfFire / GetRange2D /
 * GetPredictedImpactTime / GetAimFromPos) need distinct handling; the rest fall
 * through to Base behaviour.
 */

class CWeapon;

namespace trace {

enum class WeaponClass : uint8_t {
	Base = 0,
	Cannon,
	MissileLauncher,
	StarburstLauncher,
	BeamLaser,
	LightningCannon,
	LaserCannon,
	BombDropper,
	TorpedoLauncher,
	MeleeWeapon,
	PlasmaRepulser,
	NoWeapon,
	Rifle,
	EmgCannon,
	FlameThrower,
	DGunWeapon,
};

// Map a live CWeapon* to its WeaponClass. ClassifyWeapon reads the construction-
// time tag stamped by each subclass ctor (a single byte load); it is the hot path
// used by the epoch extractor + the live trace predicates. ClassifyWeaponSlow
// walks the dynamic_cast chain (defined in WeaponTraceClass.cpp, which pulls in
// every subclass header) and is retained as RTTI ground-truth for the diff gate.
WeaponClass ClassifyWeapon(const CWeapon* w);
WeaponClass ClassifyWeaponSlow(const CWeapon* w);

} // namespace trace
