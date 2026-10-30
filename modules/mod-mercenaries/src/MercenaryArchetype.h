/*
 * mod-mercenaries: archetypes.
 *
 * An archetype is one premade PvE spec plus the signature abilities of the
 * *other* specs of the same class, which the mercenary may never learn. The spec
 * makes the mercenary play like its role; the blacklist makes the role real, because
 * a fury warrior that still knows Shield Wall can tank and a shadow priest that
 * still knows Resurrection can heal.
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

    // Reads Playerbots.PremadeSpecName.* and Mercenary.Blacklist.*, resolving every
    // spell name against the spell table. Safe to call again on config reload.
    void Load();

    // The PvE spec numbers of a class, in configuration order. Empty when the
    // class has no premade specs configured, which makes it unofferable.
    [[nodiscard]] std::vector<uint8> const& PveSpecs(uint8 cls) const;

    [[nodiscard]] std::string const& SpecName(uint8 cls, uint8 spec) const;
    [[nodiscard]] MercenaryRole RoleFor(uint8 cls, uint8 spec) const;
    [[nodiscard]] bool IsForbidden(uint8 cls, uint8 spec, uint32 spellId) const;

    // The variant pool of a spec: abilities a mercenary of it may or may not have.
    // Empty for a spec with no Mercenary.Variant.<class>.<spec> block.
    [[nodiscard]] std::vector<MercenaryVariantSpell> const& Variants(uint8 cls, uint8 spec) const;

    // Pins the spec and removes every blacklisted spell the mercenary still knows,
    // plus the variant abilities this mercenary was not dealt. Called on every mercenary
    // login, after any re-levelling.
    void Apply(Player* bot, uint8 cls, uint8 spec, std::vector<MercenaryVariantSpell> const& withheld = {}) const;

private:
    static uint32 Key(uint8 cls, uint8 spec) { return (uint32(cls) << 8) | uint32(spec); }

    std::unordered_map<uint32, std::vector<uint32>> _forbidden;   // Key -> spell ids
    std::unordered_map<uint32, std::vector<MercenaryVariantSpell>> _variants;  // Key -> variant pool
    std::unordered_map<uint32, std::string>         _specNames;   // Key -> premade spec name
    std::unordered_map<uint8, std::vector<uint8>>   _pveSpecs;    // class -> spec numbers
};

#define sMercenaryArchetype MercenaryArchetype::instance()

#endif
