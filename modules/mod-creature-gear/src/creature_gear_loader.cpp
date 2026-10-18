/*
 * This file is part of mod-creature-gear.
 *
 * Entry point called by the core's generated ModulesLoader.cpp.
 */

#include "creature_gear_common.h"

void Addmod_creature_gearScripts()
{
    new CreatureGearWorldScript();
    new CreatureGearWeaponScript();
    new CreatureGearLootScript();
    new CreatureGearDeathScript();
    new CreatureGearAppearanceScript();
    new CreatureGearPacketScript();
}
