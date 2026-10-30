/*
 * mod-mercenaries: archetypes.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "MercenaryArchetype.h"

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
    // The playable classes, in the order the picker lists them. Death knights
    // are filtered by level at offer time, not here.
    constexpr uint8 kClasses[] = {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
        CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    // Premade spec numbers are contiguous from 0 in the shipped configuration
    // (warrior 0-5, druid 0-6). Eight is a hard ceiling, not a guess.
    constexpr uint8 kMaxSpecs = 8;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // One decimal field of a <class>.<spec> grid key. A class or spec number is at
    // most two digits, so a field that is not all digits - or is long enough to be
    // a typo rather than a number - is refused instead of parsed.
    bool ParseNumber(std::string const& text, uint32& out)
    {
        if (text.empty() || text.size() > 3)
            return false;

        uint32 value = 0;
        for (unsigned char c : text)
        {
            if (!std::isdigit(c))
                return false;
            value = value * 10 + uint32(c - '0');
        }

        out = value;
        return true;
    }

    // One key of a configuration grid: the keys are enumerated from the conf
    // rather than probed, so a malformed key is named and skipped instead of
    // reading as "that spec is unconfigured".
    bool ParseGridKey(std::string const& prefix, std::string const& key, uint32& cls, uint32& spec)
    {
        std::string const rest = key.substr(prefix.size());
        size_t const dot = rest.find('.');
        if (dot == std::string::npos || rest.find('.', dot + 1) != std::string::npos
            || !ParseNumber(rest.substr(0, dot), cls) || !ParseNumber(rest.substr(dot + 1), spec))
        {
            LOG_WARN("module", "Mercenary: {}: not <class>.<spec>; skipped", key);
            return false;
        }

        // The checker validates the shipped conf, but nothing validates a
        // hand-edited conf at boot, so a class or spec this module does not cover is
        // refused here rather than kept as a table entry no mercenary can reach.
        bool knownClass = false;
        for (uint8 const candidate : kClasses)
            if (uint32(candidate) == cls)
                knownClass = true;

        if (!knownClass || spec >= kMaxSpecs)
        {
            LOG_WARN("module", "Mercenary: {}: class {} or spec {} is outside the classes and specs this module covers; skipped",
                     key, cls, spec);
            return false;
        }

        return true;
    }

    // The spec names are words ("unholy pve", "double aura blood"), so the role is read
    // off whole words: a substring test reads "unholy" as "holy" and calls a death
    // knight a healer.
    bool HasWord(std::string const& name, std::string const& word)
    {
        auto const isAlpha = [](unsigned char c) { return std::isalpha(c) != 0; };
        for (size_t at = name.find(word); at != std::string::npos; at = name.find(word, at + 1))
        {
            size_t const end = at + word.size();
            bool const leftFree = at == 0 || !isAlpha(static_cast<unsigned char>(name[at - 1]));
            bool const rightFree = end == name.size() || !isAlpha(static_cast<unsigned char>(name[end]));
            if (leftFree && rightFree)
                return true;
        }
        return false;
    }
}

MercenaryArchetype& MercenaryArchetype::instance()
{
    static MercenaryArchetype instance;
    return instance;
}

void MercenaryArchetype::Load()
{
    _specNames.clear();
    _pveSpecs.clear();

    // The keys the configuration defines, enumerated rather than probed: asking
    // for a key the conf does not define is a missing-option warning, and the
    // class x spec grids define only the specs the configuration ships.
    //
    // Read from the config manager rather than sPlayerbotAIConfig: hook order
    // between modules is not guaranteed, so at this point the playerbots config
    // struct may not have parsed its spec table yet. The key is the same one that
    // table is built from.
    std::string const specPrefix = "Playerbots.PremadeSpecName.";
    uint32 pveSpecs = 0;
    for (std::string const& key : sConfigMgr->GetKeysByString(specPrefix))
    {
        uint32 cls = 0;
        uint32 spec = 0;
        if (!ParseGridKey(specPrefix, key, cls, spec))
            continue;

        // The key came out of the option map, so it exists: the default is
        // unreachable and there is nothing missing to log.
        std::string const name = sConfigMgr->GetOption<std::string>(key, "", false);
        if (name.empty())
            continue;

        _specNames[Key(uint8(cls), uint8(spec))] = name;

        // "arms pve", "bear pve": the PvE specs are the ones the picker may
        // offer. The pvp variants are a different archetype and stay out of
        // the roster.
        std::string const lower = Lower(name);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, " pve") == 0)
        {
            _pveSpecs[uint8(cls)].push_back(uint8(spec));
            ++pveSpecs;
        }
    }

    LOG_INFO("module", "Mercenary: archetypes: {} specs named, {} PvE specs offerable",
             uint32(_specNames.size()), pveSpecs);
}

std::vector<uint8> const& MercenaryArchetype::PveSpecs(uint8 cls) const
{
    static std::vector<uint8> const empty;
    auto it = _pveSpecs.find(cls);
    return it == _pveSpecs.end() ? empty : it->second;
}

std::string const& MercenaryArchetype::SpecName(uint8 cls, uint8 spec) const
{
    static std::string const unknown = "unknown";
    auto it = _specNames.find(Key(cls, spec));
    return it == _specNames.end() ? unknown : it->second;
}

MercenaryRole MercenaryArchetype::RoleFor(uint8 cls, uint8 spec) const
{
    std::string const name = Lower(SpecName(cls, spec));

    // The spec names are the module's own vocabulary ("prot pve", "bear pve",
    // "blood pve", "resto pve", "disc pve"), so the role is read off the name
    // rather than off a second table that could drift from it.
    if (HasWord(name, "prot") || HasWord(name, "bear") || HasWord(name, "blood"))
        return MERCENARY_ROLE_TANK;

    if (HasWord(name, "holy") || HasWord(name, "resto") || HasWord(name, "disc"))
        return MERCENARY_ROLE_HEALER;

    return MERCENARY_ROLE_DPS;
}

void MercenaryArchetype::Apply(Player* bot, uint8 spec) const
{
    if (!bot)
        return;

    PlayerbotFactory::InitTalentsBySpecNo(bot, spec, true);
}
