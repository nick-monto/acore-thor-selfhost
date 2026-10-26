/*
 * mod-pawn: archetypes.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnArchetype.h"

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

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

    std::string Trim(std::string s)
    {
        auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::vector<std::string> SplitNames(std::string const& list)
    {
        std::vector<std::string> out;
        std::string current;
        for (char c : list)
        {
            if (c == ',')
            {
                std::string name = Trim(current);
                if (!name.empty())
                    out.push_back(name);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        std::string name = Trim(current);
        if (!name.empty())
            out.push_back(name);
        return out;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Spell names are the only reviewable key the configuration can carry: the
    // server keeps no name -> id table, so build one once. SpellName[0] is enUS
    // and is what the .dist comments are written against.
    std::unordered_map<std::string, std::vector<uint32>> BuildNameIndex()
    {
        std::unordered_map<std::string, std::vector<uint32>> index;
        uint32 const count = sSpellMgr->GetSpellInfoStoreSize();
        for (uint32 id = 1; id < count; ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->SpellName[0] || !info->SpellName[0][0])
                continue;
            index[info->SpellName[0]].push_back(id);
        }
        return index;
    }
}

PawnArchetype& PawnArchetype::instance()
{
    static PawnArchetype instance;
    return instance;
}

void PawnArchetype::Load()
{
    _forbidden.clear();
    _specNames.clear();
    _pveSpecs.clear();

    for (uint8 cls : kClasses)
    {
        for (uint8 spec = 0; spec < kMaxSpecs; ++spec)
        {
            // Read from the config manager rather than sPlayerbotAIConfig: hook
            // order between modules is not guaranteed, so at this point the
            // playerbots config struct may not have parsed its spec table yet.
            // The key is the same one that table is built from.
            std::string const key = "Playerbots.PremadeSpecName." + std::to_string(cls) + "." + std::to_string(spec);
            std::string const name = sConfigMgr->GetOption<std::string>(key, "");
            if (name.empty())
                continue;

            _specNames[Key(cls, spec)] = name;

            // "arms pve", "bear pve": the PvE specs are the ones the picker may
            // offer. The pvp variants are a different archetype and stay out of
            // the roster.
            std::string const lower = Lower(name);
            if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, " pve") == 0)
                _pveSpecs[cls].push_back(spec);
        }
    }

    auto const nameIndex = BuildNameIndex();
    uint32 resolved = 0;
    uint32 missing = 0;

    for (uint8 cls : kClasses)
    {
        for (uint8 spec = 0; spec < kMaxSpecs; ++spec)
        {
            std::string const key = "Pawn.Blacklist." + std::to_string(cls) + "." + std::to_string(spec);
            std::string const list = sConfigMgr->GetOption<std::string>(key, "");
            if (list.empty())
                continue;

            std::vector<uint32>& ids = _forbidden[Key(cls, spec)];
            for (std::string const& name : SplitNames(list))
            {
                auto it = nameIndex.find(name);
                if (it == nameIndex.end())
                {
                    ++missing;
                    LOG_ERROR("module", "Pawn: blacklist {}.{}: no spell is named '{}'; that restriction does nothing",
                              uint32(cls), uint32(spec), name);
                    continue;
                }

                ids.insert(ids.end(), it->second.begin(), it->second.end());
                resolved += uint32(it->second.size());
            }

            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
    }

    LOG_INFO("module", "Pawn: archetypes: {} specs named, {} PvE specs offerable, {} blacklisted spells resolved, {} names unresolved",
             uint32(_specNames.size()), uint32(_pveSpecs.size()), resolved, missing);
}

std::vector<uint8> const& PawnArchetype::PveSpecs(uint8 cls) const
{
    static std::vector<uint8> const empty;
    auto it = _pveSpecs.find(cls);
    return it == _pveSpecs.end() ? empty : it->second;
}

std::string const& PawnArchetype::SpecName(uint8 cls, uint8 spec) const
{
    static std::string const unknown = "unknown";
    auto it = _specNames.find(Key(cls, spec));
    return it == _specNames.end() ? unknown : it->second;
}

PawnRole PawnArchetype::RoleFor(uint8 cls, uint8 spec) const
{
    std::string const name = Lower(SpecName(cls, spec));

    // The spec names are the module's own vocabulary ("prot pve", "bear pve",
    // "blood pve", "resto pve", "disc pve"), so the role is read off the name
    // rather than off a second table that could drift from it.
    if (name.find("prot") != std::string::npos || name.find("bear") != std::string::npos ||
        name.find("blood") != std::string::npos)
        return PAWN_ROLE_TANK;

    if (name.find("holy") != std::string::npos || name.find("resto") != std::string::npos ||
        name.find("disc") != std::string::npos)
        return PAWN_ROLE_HEALER;

    return PAWN_ROLE_DPS;
}

bool PawnArchetype::IsForbidden(uint8 cls, uint8 spec, uint32 spellId) const
{
    auto it = _forbidden.find(Key(cls, spec));
    if (it == _forbidden.end())
        return false;
    return std::binary_search(it->second.begin(), it->second.end(), spellId);
}

void PawnArchetype::Apply(Player* bot, uint8 cls, uint8 spec) const
{
    if (!bot)
        return;

    PlayerbotFactory::InitTalentsBySpecNo(bot, spec, true);

    auto it = _forbidden.find(Key(cls, spec));
    if (it == _forbidden.end())
        return;

    uint32 pruned = 0;
    for (uint32 spellId : it->second)
    {
        if (!bot->HasSpell(spellId))
            continue;
        bot->removeSpell(spellId, SPEC_MASK_ALL, false, true);
        ++pruned;
    }

    LOG_DEBUG("module", "Pawn: pawn {}: spec {} applied, {} blacklisted spells pruned", bot->GetName(), uint32(spec), pruned);
}
