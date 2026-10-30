/*
 * mod-mercenaries: archetypes.
 *
 * An archetype is one premade PvE spec, pinned when the mercenary is built so that it
 * plays like its role and is geared for it. The spec is all that is pinned: a mercenary
 * keeps every ability its class and level give it. An earlier build also withheld the
 * other specs' signature abilities and a per-mercenary subset of the spec's situational
 * ones, which read as broken rather than as different at low levels - a level-10 hunter
 * missing Concussive Shot - so the restriction was removed and is deferred.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_MERCENARY_ARCHETYPE_H
#define MOD_MERCENARY_ARCHETYPE_H

#include "mercenary_common.h"

#include <string>
#include <unordered_map>
#include <vector>

class Player;

class MercenaryArchetype
{
public:
    static MercenaryArchetype& instance();

    // Reads Playerbots.PremadeSpecName.*. Safe to call again on config reload.
    void Load();

    // The PvE spec numbers of a class, in configuration order. Empty when the
    // class has no premade specs configured, which makes it unofferable.
    [[nodiscard]] std::vector<uint8> const& PveSpecs(uint8 cls) const;

    [[nodiscard]] std::string const& SpecName(uint8 cls, uint8 spec) const;
    [[nodiscard]] MercenaryRole RoleFor(uint8 cls, uint8 spec) const;

    // Pins the spec's talents. Called on every mercenary login, after any re-levelling.
    void Apply(Player* bot, uint8 spec) const;

private:
    static uint32 Key(uint8 cls, uint8 spec) { return (uint32(cls) << 8) | uint32(spec); }

    std::unordered_map<uint32, std::string>         _specNames;   // Key -> premade spec name
    std::unordered_map<uint8, std::vector<uint8>>   _pveSpecs;    // class -> spec numbers
};

#define sMercenaryArchetype MercenaryArchetype::instance()

#endif
