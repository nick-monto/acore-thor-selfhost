/*
 * mod-pawn: personality.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnPersonality.h"

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Every class a pawn can be, in the order the picker lists them.
    constexpr uint8 kClasses[] = {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
        CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    // Spec numbers are contiguous from 0 in the shipped configuration (warrior
    // 0-5, druid 0-6). Eight is the ceiling the archetype tables use too.
    constexpr uint8 kMaxSpecs = 8;

    // SplitMix64: the same shuffle PawnMgr uses. It leaves the global RNG alone,
    // so a pawn's temperament and voice are reproducible from (owner, bot) and
    // cannot be perturbed by anything else running on the world thread.
    uint64 Mix(uint64& state)
    {
        state ^= state >> 30;
        state *= 0xBF58476D1CE4E5B9ull;
        state ^= state >> 27;
        state *= 0x94D049BB133111EBull;
        state ^= state >> 31;
        return state;
    }

    std::string Trim(std::string s)
    {
        auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::vector<std::string> Split(std::string const& text, char sep)
    {
        std::vector<std::string> out;
        std::string current;
        for (char c : text)
        {
            if (c == sep)
            {
                out.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        out.push_back(current);
        return out;
    }

    // One profile of a pool: "Label: +a, -b". Anything malformed is dropped here
    // rather than at apply time, because Engine::addStrategy silently ignores an
    // unknown name, so a typo in the configuration would otherwise do nothing and
    // say nothing.
    void ParseProfile(std::string const& key, std::string const& chunk, std::vector<PawnProfile>& out)
    {
        size_t const colon = chunk.find(':');
        if (colon == std::string::npos)
        {
            LOG_ERROR("module", "Pawn: {}: profile '{}' has no label; dropped", key, Trim(chunk));
            return;
        }

        std::string const label = Trim(chunk.substr(0, colon));
        if (label.empty())
        {
            LOG_ERROR("module", "Pawn: {}: profile '{}' has an empty label; dropped", key, Trim(chunk));
            return;
        }

        std::string changes;
        for (std::string const& raw : Split(chunk.substr(colon + 1), ','))
        {
            std::string const token = Trim(raw);
            if (token.empty())
                continue;  // a trailing comma is a formatting slip, not a strategy

            std::string const name = token.size() > 1 ? Trim(token.substr(1)) : std::string();
            if (name.empty() || (token[0] != '+' && token[0] != '-'))
            {
                LOG_ERROR("module", "Pawn: {}: '{}' is not a +strategy or -strategy", key, token);
                continue;
            }

            if (!changes.empty())
                changes.push_back(',');
            changes.push_back(token[0]);
            changes += name;
        }

        if (changes.empty())
        {
            LOG_ERROR("module", "Pawn: {}: profile '{}' kept no strategies; dropped", key, label);
            return;
        }

        // Rebuilt from the surviving tokens, so what Apply hands the engine is
        // exactly the list that was validated above.
        out.push_back(PawnProfile{ label, changes });
    }

    std::vector<PawnProfile> ParsePool(std::string const& key, std::string const& value)
    {
        std::vector<PawnProfile> pool;
        for (std::string const& chunk : Split(value, '|'))
        {
            if (Trim(chunk).empty())
                continue;
            ParseProfile(key, chunk, pool);
        }
        return pool;
    }

    // The same (owner, bot) seed as level, spec, gear and withheld variants, so a
    // pawn's temperament and voice never change under its owner. The salt keeps
    // the draws independent of one another and of the other per-pawn axes.
    PawnProfile const* Draw(std::vector<PawnProfile> const& pool, ObjectGuid owner, ObjectGuid bot, uint64 salt)
    {
        if (pool.empty())
            return nullptr;

        uint32 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);
        uint64 state = uint64(seed) ^ salt;
        return &pool[Mix(state) % pool.size()];
    }

    // A trait that names a strategy the bot does not know is a configuration
    // mistake, and the same mistake is shared by every pawn that drew it, so warn
    // once per (engine, name) rather than once per login.
    std::set<std::string> g_unknownStrategies;

    void ApplyProfile(PlayerbotAI* ai, Player* bot, PawnProfile const* profile, BotState state, char const* kind)
    {
        if (!profile)
            return;

        for (std::string const& token : Split(profile->Changes, ','))
        {
            if (token.size() < 2)
                continue;

            std::string const name = Trim(token.substr(1));
            ai->ChangeStrategy(token, state);

            // A '-name' whose strategy is absent has already achieved its intent.
            if (token[0] != '+')
                continue;

            if (ai->HasStrategy(name, state))
                continue;

            std::string const key = std::string(kind) + ':' + name;
            if (g_unknownStrategies.insert(key).second)
                LOG_ERROR("module", "Pawn: personality: no {} strategy named '{}' for {}; that trait does nothing",
                          kind, name, bot->GetName());
        }
    }

    // The tokens of one profile, already validated by ParseProfile: "+name" or
    // "-name", no spaces.
    std::vector<std::string> Tokens(PawnProfile const& profile)
    {
        std::vector<std::string> out;
        for (std::string const& token : Split(profile.Changes, ','))
            if (token.size() > 1)
                out.push_back(token);
        return out;
    }

    // Applies one trait and undoes the rest of its pool first.
    //
    // A pool describes the whole dial space of one engine, but playerbots replays
    // the strategy list it saved at the learner's last logout, so the engine the
    // trait lands on still carries the dials of whatever profile was applied
    // before the pool was edited. Setting only the chosen tokens would leave those
    // stale dials in place: the pawn would keep last session's temperament. So
    // every token another profile of the pool names - and the chosen one does not
    // - is inverted, then the chosen tokens are applied on top.
    void ApplyPool(PlayerbotAI* ai, Player* bot, std::vector<PawnProfile> const& pool,
                   PawnProfile const* chosen, BotState state, char const* kind)
    {
        // A spec with no pool keeps whatever it already has: nothing to undo,
        // nothing to set.
        if (!chosen)
            return;

        std::vector<std::string> const wanted = Tokens(*chosen);

        // Does the chosen profile already decide this dial? Then its own pass
        // below puts it where it belongs and the inverse must not run.
        auto const decidedByChosen = [&wanted](std::string const& name)
        {
            for (std::string const& token : wanted)
                if (token.substr(1) == name)
                    return true;
            return false;
        };

        for (PawnProfile const& other : pool)
        {
            if (&other == chosen)
                continue;

            for (std::string const& token : Tokens(other))
            {
                std::string const name = token.substr(1);
                if (decidedByChosen(name))
                    continue;

                std::string inverse = token;
                inverse[0] = (token[0] == '+') ? '-' : '+';
                ai->ChangeStrategy(inverse, state);
            }
        }

        ApplyProfile(ai, bot, chosen, state, kind);
    }
}

PawnPersonality& PawnPersonality::instance()
{
    static PawnPersonality instance;
    return instance;
}

void PawnPersonality::Load()
{
    _temperaments.clear();
    _voices.clear();

    _voices = ParsePool("Pawn.Voice", sConfigMgr->GetOption<std::string>("Pawn.Voice", "Talkative: +emote | Quiet: -emote"));

    // Only the keys that are actually set become pools: an empty value means the
    // class/spec has no temperament at all, which is what keeps a spec the
    // configuration has not thought about untouched rather than crowded with
    // random traits.
    uint32 specs = 0;
    for (uint8 const cls : kClasses)
    {
        for (uint8 spec = 0; spec < kMaxSpecs; ++spec)
        {
            ++specs;
            std::string const key = "Pawn.Temperament." + std::to_string(cls) + '.' + std::to_string(spec);
            std::string const value = sConfigMgr->GetOption<std::string>(key, "");
            if (value.empty())
                continue;

            // A block that survived parsing with no profile left is not a pool:
            // storing it would count towards TemperamentPools() and read as a
            // temperament the class does not actually have.
            std::vector<PawnProfile> pool = ParsePool(key, value);
            if (!pool.empty())
                _temperaments[Key(cls, spec)] = pool;
        }
    }

    // _temperaments only holds non-empty pools, so the two counts together read
    // as "how much of the grid is populated".
    LOG_INFO("module", "Pawn: personality: {} temperament pools over {} specs, {} voices",
             _temperaments.size(), specs, _voices.size());
}

PawnProfile const* PawnPersonality::Temperament(ObjectGuid owner, ObjectGuid bot, uint8 cls, uint8 spec) const
{
    auto const it = _temperaments.find(Key(cls, spec));
    if (it == _temperaments.end())
        return nullptr;

    return Draw(it->second, owner, bot, 0x9E3779B97F4A7C15ull);
}

PawnProfile const* PawnPersonality::Voice(ObjectGuid owner, ObjectGuid bot) const
{
    return Draw(_voices, owner, bot, 0xC2B2AE3D27D4EB4Full);
}

void PawnPersonality::Apply(Player* bot, ObjectGuid owner, uint8 cls, uint8 spec) const
{
    if (!bot)
        return;

    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;

    // The split is the point: how a pawn fights is a combat-engine decision, how
    // it behaves in town is a non-combat one, and mixing them would let a voice
    // change win a fight.
    if (PawnProfile const* temperament = Temperament(owner, bot->GetGUID(), cls, spec))
    {
        auto const it = _temperaments.find(Key(cls, spec));
        if (it != _temperaments.end())
            ApplyPool(ai, bot, it->second, temperament, BOT_STATE_COMBAT, "combat");
    }

    if (PawnProfile const* voice = Voice(owner, bot->GetGUID()))
        ApplyPool(ai, bot, _voices, voice, BOT_STATE_NON_COMBAT, "non-combat");
}

uint32 PawnPersonality::TemperamentPools() const
{
    return uint32(_temperaments.size());
}

uint32 PawnPersonality::VoiceCount() const
{
    return uint32(_voices.size());
}
