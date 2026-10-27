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
            LOG_WARN("module", "Pawn: {}: not <class>.<spec>; skipped", key);
            return false;
        }

        // The checker validates the shipped conf, but nothing validates a
        // hand-edited conf at boot, so a class or spec this module does not cover is
        // refused here rather than kept as a table entry no pawn can reach.
        bool knownClass = false;
        for (uint8 const candidate : kClasses)
            if (uint32(candidate) == cls)
                knownClass = true;

        if (!knownClass || spec >= kMaxSpecs)
        {
            LOG_WARN("module", "Pawn: {}: class {} or spec {} is outside the classes and specs this module covers; skipped",
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

    std::string Fold(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return name;
    }

    // Spell names are the only reviewable key the configuration can carry: the
    // server keeps no name -> id table, so build one once. SpellName[0] is enUS
    // and is what the .dist comments are written against.
    //
    // The folded index is the second chance for a hand-typed name: the table's own
    // spelling is title case ("Hunger For Blood"), and a name that differs only in
    // case is a misspelling of a spell that exists, not a different spell.
    struct NameIndex
    {
        std::unordered_map<std::string, std::vector<uint32>> exact;
        std::unordered_map<std::string, std::vector<std::string>> folded;
    };

    NameIndex BuildNameIndex()
    {
        NameIndex index;
        uint32 const count = sSpellMgr->GetSpellInfoStoreSize();
        for (uint32 id = 1; id < count; ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->SpellName[0] || !info->SpellName[0][0])
                continue;
            index.exact[info->SpellName[0]].push_back(id);
        }

        for (auto const& entry : index.exact)
            index.folded[Fold(entry.first)].push_back(entry.first);
        return index;
    }

    // The ids a configured name resolves to, or nullptr with the reason logged. `kind`,
    // `cls` and `spec` are the block the name came from, for the log line.
    std::vector<uint32> const* ResolveName(NameIndex const& index, std::string const& name,
                                           char const* kind, uint32 cls, uint32 spec)
    {
        auto const exact = index.exact.find(name);
        if (exact != index.exact.end())
            return &exact->second;

        auto const folded = index.folded.find(Fold(name));
        if (folded == index.folded.end())
        {
            LOG_ERROR("module", "Pawn: {} {}.{}: no spell is named '{}'; that restriction does nothing",
                      kind, cls, spec, name);
            return nullptr;
        }

        if (folded->second.size() > 1)
        {
            std::string candidates;
            for (std::string const& candidate : folded->second)
                candidates += (candidates.empty() ? "'" : ", '") + candidate + "'";
            LOG_ERROR("module", "Pawn: {} {}.{}: '{}' is not a spell name, and {} spells differ only in case ({}); spell one of them exactly",
                      kind, cls, spec, name, uint32(folded->second.size()), candidates);
            return nullptr;
        }

        std::string const& real = folded->second.front();
        LOG_WARN("module", "Pawn: {} {}.{}: '{}' is spelled '{}' in the spell table; using that",
                 kind, cls, spec, name, real);
        return &index.exact.find(real)->second;
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
    _variants.clear();
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

    auto const nameIndex = BuildNameIndex();
    uint32 blacklisted = 0;
    uint32 missing = 0;

    std::string const blacklistPrefix = "Pawn.Blacklist.";
    for (std::string const& key : sConfigMgr->GetKeysByString(blacklistPrefix))
    {
        uint32 cls = 0;
        uint32 spec = 0;
        if (!ParseGridKey(blacklistPrefix, key, cls, spec))
            continue;

        std::string const list = sConfigMgr->GetOption<std::string>(key, "", false);
        if (list.empty())
            continue;

        std::vector<uint32>& ids = _forbidden[Key(uint8(cls), uint8(spec))];
        for (std::string const& name : SplitNames(list))
        {
            std::vector<uint32> const* spells = ResolveName(nameIndex, name, "blacklist", cls, spec);
            if (!spells)
            {
                ++missing;
                continue;
            }

            ids.insert(ids.end(), spells->begin(), spells->end());
        }

        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        blacklisted += uint32(ids.size());
    }

    // The variant pools: abilities a pawn of the spec may or may not arrive with,
    // kept as name + every id it resolves to, because the picker names them and
    // the learn gate has to match the ids.
    uint32 variantNames = 0;
    std::string const variantPrefix = "Pawn.Variant.";
    for (std::string const& key : sConfigMgr->GetKeysByString(variantPrefix))
    {
        uint32 cls = 0;
        uint32 spec = 0;
        if (!ParseGridKey(variantPrefix, key, cls, spec))
            continue;

        std::string const list = sConfigMgr->GetOption<std::string>(key, "", false);
        if (list.empty())
            continue;

        std::vector<PawnVariantSpell>& pool = _variants[Key(uint8(cls), uint8(spec))];
        for (std::string const& name : SplitNames(list))
        {
            std::vector<uint32> const* spells = ResolveName(nameIndex, name, "variant", cls, spec);
            if (!spells)
            {
                ++missing;
                continue;
            }

            pool.push_back(PawnVariantSpell{name, *spells});
            ++variantNames;
        }
    }

    LOG_INFO("module", "Pawn: archetypes: {} specs named, {} PvE specs offerable, {} blacklisted ids, {} variant abilities over {} specs, {} names unresolved",
             uint32(_specNames.size()), pveSpecs, blacklisted, variantNames,
             uint32(_variants.size()), missing);
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
    if (HasWord(name, "prot") || HasWord(name, "bear") || HasWord(name, "blood"))
        return PAWN_ROLE_TANK;

    if (HasWord(name, "holy") || HasWord(name, "resto") || HasWord(name, "disc"))
        return PAWN_ROLE_HEALER;

    return PAWN_ROLE_DPS;
}

std::vector<PawnVariantSpell> const& PawnArchetype::Variants(uint8 cls, uint8 spec) const
{
    static std::vector<PawnVariantSpell> const empty;
    auto it = _variants.find(Key(cls, spec));
    return it == _variants.end() ? empty : it->second;
}

bool PawnArchetype::IsForbidden(uint8 cls, uint8 spec, uint32 spellId) const
{
    auto it = _forbidden.find(Key(cls, spec));
    if (it == _forbidden.end())
        return false;
    return std::binary_search(it->second.begin(), it->second.end(), spellId);
}

void PawnArchetype::Apply(Player* bot, uint8 cls, uint8 spec, std::vector<PawnVariantSpell> const& withheld) const
{
    if (!bot)
        return;

    PlayerbotFactory::InitTalentsBySpecNo(bot, spec, true);

    uint32 pruned = 0;
    auto const prune = [&](std::vector<uint32> const& ids)
    {
        for (uint32 spellId : ids)
        {
            if (!bot->HasSpell(spellId))
                continue;
            bot->removeSpell(spellId, SPEC_MASK_ALL, false, true);
            ++pruned;
        }
    };

    if (auto it = _forbidden.find(Key(cls, spec)); it != _forbidden.end())
        prune(it->second);

    // The pawn's own withheld variants, so a rebuild does not leave one behind:
    // the learn gate would refuse it anyway, but a spell already in the book is
    // not a learn attempt.
    for (PawnVariantSpell const& variant : withheld)
        prune(variant.Ids);

    LOG_DEBUG("module", "Pawn: pawn {}: spec {} applied, {} spells pruned ({} variant{})", bot->GetName(),
              uint32(spec), pruned, uint32(withheld.size()), withheld.size() == 1 ? "" : "s");
}
