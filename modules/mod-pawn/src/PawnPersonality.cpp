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
#include "Util.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
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

    // One decimal field of a Pawn.Temperament.<class>.<spec> key. A class or spec
    // number is at most two digits, so a field that is not all digits - or is long
    // enough to be a typo rather than a number - is refused instead of parsed.
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

    // The three moments the module speaks for a pawn. Kept as a list so the loader
    // and the speaker agree on exact spellings rather than on three literals typed
    // in three places.
    constexpr char const* kEvents[] = { "kill", "death", "dismiss" };

    // One event's small, stable id: the same integer salts the line draw and keys the
    // cooldown, so a pawn's kill line cannot collide with its death line in either
    // place. Masked to eight bits because the cooldown key packs the pawn's counter and
    // the event into one integer.
    uint32 PawnEventId(char const* event)
    {
        uint32 id = 0;
        for (char const* c = event; c && *c; ++c)
            id = id * 131 + uint32(static_cast<unsigned char>(*c));
        return id & 0xFF;
    }

    bool IsEvent(std::string const& event)
    {
        for (char const* known : kEvents)
            if (event == known)
                return true;
        return false;
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
    _lines.clear();
    _spoken.clear();

    // Pacing is reloaded with the pools rather than read from PawnMgr's config struct:
    // it is the personality's own dial, and the keys travel with the pools. The cooldown
    // bookkeeping is dropped with them, because a reload may have removed the event a
    // pawn was last timed on, and a timestamp for a line that no longer exists would
    // silence a line that does.
    _killChancePercent = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.VoiceKillChancePercent", 10), 0, 100));
    _cooldownMs = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.VoiceCooldownSeconds", 120), 0, 3600)) * 1000;

    _voices = ParsePool("Pawn.Voice", sConfigMgr->GetOption<std::string>("Pawn.Voice", "Talkative: +emote | Quiet: -emote"));

    // The line pools are read after the voices because a line is keyed by a voice label,
    // and a label Pawn.Voice no longer defines has to be told apart from one it does.
    //
    // Enumerated, never probed: asking for a key the conf does not define is a missing-
    // option warning at boot, and the set of voices is the admin's, so the lines a
    // shipped conf mentions are found by reading what is there rather than by asking for
    // labels this build happens to know.
    std::string const linePrefix = "Pawn.VoiceLine.";
    for (std::string const& key : sConfigMgr->GetKeysByString(linePrefix))
    {
        std::string const label = Trim(key.substr(linePrefix.size()));

        // A pawn's voice is always one of the pool's labels, so a line keyed by anything
        // else can never be drawn: it is named and dropped rather than kept as a block no
        // pawn can reach.
        bool knownVoice = false;
        for (PawnProfile const& voice : _voices)
            if (voice.Label == label)
                knownVoice = true;

        if (!knownVoice)
        {
            LOG_WARN("module", "Pawn: {}: '{}' is not a label Pawn.Voice defines; voice lines skipped", key, label);
            continue;
        }

        // The key came out of the option map, so it exists: the default is unreachable
        // and there is nothing missing to log.
        std::string const value = sConfigMgr->GetOption<std::string>(key, "", false);

        std::unordered_map<std::string, std::vector<std::string>> events;
        for (std::string const& raw : Split(value, '|'))
        {
            std::string const chunk = Trim(raw);
            if (chunk.empty())
                continue;  // a trailing separator is a formatting slip, not a line

            // The event name ends at the first colon: a text keeps any colons of its own,
            // which is the only way an emote like "*spits:* not you" survives the format.
            size_t const colon = chunk.find(':');
            std::string const event = Trim(chunk.substr(0, colon));
            std::string const text = colon == std::string::npos ? std::string() : Trim(chunk.substr(colon + 1));

            if (!IsEvent(event))
            {
                LOG_WARN("module", "Pawn: {}: '{}' is not an event the module speaks; line dropped", key, event);
                continue;
            }

            if (text.empty())
            {
                LOG_WARN("module", "Pawn: {}: the {} line is empty; dropped", key, event);
                continue;
            }

            events[event].push_back(text);
        }

        // A block that kept no line is not a block: storing it would make the logged
        // block count read as lines the pawn does not have.
        if (!events.empty())
            _lines[label] = std::move(events);
    }

    // The keys the configuration defines, enumerated rather than probed: asking for
    // a key the conf does not define is a missing-option warning, and the class x
    // spec grid is mostly undefined by design.
    //
    // Only the keys that are actually set become pools: an empty value means the
    // class/spec has no temperament at all, which is what keeps a spec the
    // configuration has not thought about untouched rather than crowded with
    // random traits.
    std::string const prefix = "Pawn.Temperament.";
    uint32 keysFound = 0;
    for (std::string const& key : sConfigMgr->GetKeysByString(prefix))
    {
        ++keysFound;

        std::vector<std::string> const parts = Split(key.substr(prefix.size()), '.');
        uint32 cls = 0;
        uint32 spec = 0;

        // A key that is not exactly <class>.<spec> is named and skipped, never
        // ignored: an ignored typo would read as "that spec has no temperament".
        if (parts.size() != 2 || !ParseNumber(parts[0], cls) || !ParseNumber(parts[1], spec))
        {
            LOG_WARN("module", "Pawn: {}: not <class>.<spec>; skipped", key);
            continue;
        }

        // The checker validates the shipped conf, but nothing validates a
        // hand-edited conf at boot, so a class or spec this module does not cover is
        // refused here rather than kept as a pool no pawn can ever draw.
        bool knownClass = false;
        for (uint8 const candidate : kClasses)
            if (uint32(candidate) == cls)
                knownClass = true;

        if (!knownClass || spec >= kMaxSpecs)
        {
            LOG_WARN("module", "Pawn: {}: class {} or spec {} is outside the classes and specs this module covers; skipped",
                     key, cls, spec);
            continue;
        }

        // The key came out of the option map, so it exists: the default is
        // unreachable and there is nothing missing to log.
        std::string const value = sConfigMgr->GetOption<std::string>(key, "", false);
        if (value.empty())
            continue;

        // A block that survived parsing with no profile left is not a pool:
        // storing it would count towards TemperamentPools() and read as a
        // temperament the class does not actually have.
        std::vector<PawnProfile> pool = ParsePool(key, value);
        if (!pool.empty())
            _temperaments[Key(uint8(cls), uint8(spec))] = pool;
    }

    // How many lines the pools actually hold, not how many the conf spells: a block
    // whose every line was dropped is not counted, so this number matches what a pawn
    // can say.
    size_t lineTotal = 0;
    for (auto const& block : _lines)
        for (auto const& pool : block.second)
            lineTotal += pool.second.size();

    // _temperaments only holds non-empty pools, so the two counts together read
    // as "how much of the enumerated keys is populated".
    LOG_INFO("module", "Pawn: personality: {} temperament pools over {} temperament keys, {} voices, {} voice-line blocks, {} lines",
             _temperaments.size(), keysFound, _voices.size(), _lines.size(), lineTotal);
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

std::string const* PawnPersonality::Line(ObjectGuid owner, ObjectGuid bot, char const* event) const
{
    if (!event)
        return nullptr;

    PawnProfile const* voice = Voice(owner, bot);
    if (!voice)
        return nullptr;

    auto const voiceIt = _lines.find(voice->Label);
    if (voiceIt == _lines.end())
        return nullptr;

    auto const eventIt = voiceIt->second.find(event);
    if (eventIt == voiceIt->second.end() || eventIt->second.empty())
        return nullptr;

    // Drawn from the same (owner, bot) seed as the voice itself and salted with the
    // event, so a pawn says the same thing at the same moment every time and its death
    // line is not simply its kill line again. The global RNG is never consulted here:
    // the draw has to be reproducible from the two guids alone, because the same pawn
    // resolves its line on every call and a hundred other things move the world RNG.
    uint32 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);
    uint64 state = uint64(seed) ^ uint64(PawnEventId(event));

    std::vector<std::string> const& pool = eventIt->second;
    return &pool[Mix(state) % pool.size()];
}

void PawnPersonality::SayLine(Player* bot, ObjectGuid owner, char const* event) const
{
    if (!bot)
        return;

    std::string const* text = Line(owner, bot->GetGUID(), event);
    if (!text)
        return;

    // A pawn that comments on every corpse is a nuisance, so the kill line is a chance
    // rather than a certainty: 0 never speaks, 100 always does, and urand(1, 100) never
    // exceeds 100. The other events are rare enough to be said every time.
    if (PawnEventId(event) == PawnEventId("kill") && urand(1, 100) > _killChancePercent)
        return;

    // Paced per pawn and per event, in the same packed shape the header documents. The
    // cooldown is why a pawn pulled into a fast fight cannot fill the chat with one line
    // repeated: the second kill inside the window says nothing.
    uint64 const key = (uint64(bot->GetGUID().GetCounter()) << 8) | uint64(PawnEventId(event));
    uint32 const now = getMSTime();

    auto const it = _spoken.find(key);
    // Unsigned subtraction, so the cooldown survives the millisecond counter wrapping.
    if (it != _spoken.end() && (now - it->second) < _cooldownMs)
        return;

    _spoken[key] = now;

    // The same language choice playerbots' own say action makes, so a line reads as the
    // pawn's own speech rather than as universal babble the other faction is not meant
    // to understand.
    bot->Say(*text, bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH);
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
