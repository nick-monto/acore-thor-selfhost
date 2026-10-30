/*
 * mod-mercenaries: personality.
 *
 * The roster already varies what a mercenary *is*: level, spec, gear ratio, which
 * abilities it never learned. Personality varies how it *acts*: a temperament
 * shifts the combat strategy of a spec, a voice shifts the non-combat strategy
 * of the character. Both are named profiles written in strategy syntax, so the
 * configuration can be reviewed against the playerbots strategy vocabulary.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_MERCENARY_PERSONALITY_H
#define MOD_MERCENARY_PERSONALITY_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <unordered_map>
#include <vector>

class Player;

struct MercenaryProfile
{
    std::string Label;    // "Hot-headed"
    std::string Changes;  // "+aggressive,+close" - exactly what Engine::ChangeStrategy takes
};

class MercenaryPersonality
{
public:
    static MercenaryPersonality& instance();

    // Reads Mercenary.Voice and Mercenary.Temperament.<class>.<spec>, validating every
    // token so that what is applied later is exactly what was checked. Safe to
    // call again on config reload.
    void Load();

    // The seeded profile of a mercenary. Null when the pool is empty, or when the
    // class/spec has no pool at all. The pointer stays valid until the next
    // Load(), which is the config reload and nothing else.
    [[nodiscard]] MercenaryProfile const* Temperament(ObjectGuid owner, ObjectGuid bot, uint8 cls, uint8 spec) const;
    [[nodiscard]] MercenaryProfile const* Voice(ObjectGuid owner, ObjectGuid bot) const;

    // Applies the mercenary's temperament to the combat engine and its voice to the
    // non-combat engine, drawn from the same (owner, bot) seed as everything else.
    // Authoritative over the dials its own pool names: playerbots replays the
    // strategy list it saved at the last logout, which still carries the trait the
    // mercenary had before the pool was edited, so a dial another profile of the pool
    // sets and the chosen one does not is undone before the chosen dials are
    // applied.
    void Apply(Player* bot, ObjectGuid owner, uint8 cls, uint8 spec) const;

    // The line this mercenary's voice would say for one of the events the module speaks
    // ("kill", "death", "dismiss"), or nullptr when its voice has none. Drawn from the
    // same (owner, bot) seed as its level and its temperament, so a mercenary says the same
    // thing at the same moment every time.
    [[nodiscard]] std::string const* Line(ObjectGuid owner, ObjectGuid bot, char const* event) const;

    // Says it: resolves the mercenary's voice, applies Mercenary.VoiceKillChancePercent to a kill and
    // the per-mercenary, per-event Mercenary.VoiceCooldownSeconds to every event, and speaks in the
    // mercenary's own faction language. Silent when there is nothing to say, so a call site can
    // call it unconditionally.
    void SayLine(Player* bot, ObjectGuid owner, char const* event) const;

    [[nodiscard]] uint32 TemperamentPools() const;
    [[nodiscard]] uint32 VoiceCount() const;

private:
    static uint32 Key(uint8 cls, uint8 spec) { return (uint32(cls) << 8) | uint32(spec); }

    std::unordered_map<uint32, std::vector<MercenaryProfile>> _temperaments;  // Key -> pool
    std::vector<MercenaryProfile>                             _voices;

    // One voice's lines: voice label -> event -> the texts it may say. Keyed by label
    // rather than by pool index because the configuration names a voice, and a label the
    // pool does not define is a line no mercenary can ever draw.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<std::string>>> _lines;

    // When each mercenary last spoke each event, in getMSTime(). Mutable because speaking is a
    // read of the pool plus a cooldown bookkeeping step, and the callers hold the manager
    // through its singleton.
    mutable std::unordered_map<uint64, uint32> _spoken;   // (bot counter << 8) | event id

    // How often a mercenary speaks, and how long it then stays quiet, in milliseconds. Read in
    // Load() rather than through MercenaryMgr's config struct: pacing speech is the personality's
    // own concern, and the keys reload with the pools.
    uint32 _killChancePercent = 10;
    uint32 _cooldownMs = 120000;
};

#define sMercenaryPersonality MercenaryPersonality::instance()

#endif
