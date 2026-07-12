/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "WeaponTraceClass.h"

#include "Weapon.h"
#include "Cannon.h"
#include "MissileLauncher.h"
#include "StarburstLauncher.h"
#include "BeamLaser.h"
#include "LaserCannon.h"
#include "LightningCannon.h"
#include "BombDropper.h"
#include "TorpedoLauncher.h"
#include "MeleeWeapon.h"
#include "PlasmaRepulser.h"
#include "NoWeapon.h"
#include "Rifle.h"
#include "EmgCannon.h"
#include "FlameThrower.h"
#include "DGunWeapon.h"

namespace trace {

WeaponClass ClassifyWeapon(const CWeapon* w)
{
	return w->GetWeaponClass();
}

WeaponClass ClassifyWeaponSlow(const CWeapon* w)
{
	// Order matches the trace override matrix; every weapon subclass derives
	// directly from CWeapon (flat hierarchy), so the chain has no ambiguity.
	// Classes with no trace-reachable override fall through to Base.
	if (dynamic_cast<const CCannon*>(w))            return WeaponClass::Cannon;
	if (dynamic_cast<const CMissileLauncher*>(w))   return WeaponClass::MissileLauncher;
	if (dynamic_cast<const CStarburstLauncher*>(w)) return WeaponClass::StarburstLauncher;
	if (dynamic_cast<const CBeamLaser*>(w))         return WeaponClass::BeamLaser;
	if (dynamic_cast<const CLaserCannon*>(w))       return WeaponClass::LaserCannon;
	if (dynamic_cast<const CLightningCannon*>(w))   return WeaponClass::LightningCannon;
	if (dynamic_cast<const CBombDropper*>(w))       return WeaponClass::BombDropper;
	if (dynamic_cast<const CTorpedoLauncher*>(w))   return WeaponClass::TorpedoLauncher;
	if (dynamic_cast<const CMeleeWeapon*>(w))       return WeaponClass::MeleeWeapon;
	if (dynamic_cast<const CPlasmaRepulser*>(w))    return WeaponClass::PlasmaRepulser;
	if (dynamic_cast<const CNoWeapon*>(w))          return WeaponClass::NoWeapon;
	if (dynamic_cast<const CRifle*>(w))             return WeaponClass::Rifle;
	if (dynamic_cast<const CEmgCannon*>(w))         return WeaponClass::EmgCannon;
	if (dynamic_cast<const CFlameThrower*>(w))      return WeaponClass::FlameThrower;
	if (dynamic_cast<const CDGunWeapon*>(w))        return WeaponClass::DGunWeapon;
	return WeaponClass::Base;
}

} // namespace trace
