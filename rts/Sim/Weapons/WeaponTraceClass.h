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

// Map a live CWeapon* to its WeaponClass (defined in WeaponTraceClass.cpp, which
// pulls in every subclass header for the dynamic_cast chain). Used by the epoch
// extractor + the diff gate; bounded by the live weapon count, extraction-only.
WeaponClass ClassifyWeapon(const CWeapon* w);

} // namespace trace
