/*
 * mod-pawn: personality.
 *
 * The roster already varies what a pawn *is*: level, spec, gear ratio, which
 * abilities it never learned. Personality varies how it *acts*: a temperament
 * shifts the combat strategy of a spec, a voice shifts the non-combat strategy
 * of the character. Both are named profiles written in strategy syntax, so the
 * configuration can be reviewed against the playerbots strategy vocabulary.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_PAWN_PERSONALITY_H
#define MOD_PAWN_PERSONALITY_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <unordered_map>
#include <vector>

class Player;

struct PawnProfile
{
    std::string Label;    // "Hot-headed"
    std::string Changes;  // "+aggressive,+close" - exactly what Engine::ChangeStrategy takes
};

class PawnPersonality
{
public:
    static PawnPersonality& instance();

    // Reads Pawn.Voice and Pawn.Temperament.<class>.<spec>, validating every
    // token so that what is applied later is exactly what was checked. Safe to
    // call again on config reload.
    void Load();

    // The seeded profile of a pawn. Null when the pool is empty, or when the
    // class/spec has no pool at all. The pointer stays valid until the next
    // Load(), which is the config reload and nothing else.
    [[nodiscard]] PawnProfile const* Temperament(ObjectGuid owner, ObjectGuid bot, uint8 cls, uint8 spec) const;
    [[nodiscard]] PawnProfile const* Voice(ObjectGuid owner, ObjectGuid bot) const;

    // Applies the pawn's temperament to the combat engine and its voice to the
    // non-combat engine, drawn from the same (owner, bot) seed as everything else.
    // Authoritative over the dials its own pool names: playerbots replays the
    // strategy list it saved at the last logout, which still carries the trait the
    // pawn had before the pool was edited, so a dial another profile of the pool
    // sets and the chosen one does not is undone before the chosen dials are
    // applied.
    void Apply(Player* bot, ObjectGuid owner, uint8 cls, uint8 spec) const;

    // The line this pawn's voice would say for one of the events the module speaks
    // ("kill", "death", "dismiss"), or nullptr when its voice has none. Drawn from the
    // same (owner, bot) seed as its level and its temperament, so a pawn says the same
    // thing at the same moment every time.
    [[nodiscard]] std::string const* Line(ObjectGuid owner, ObjectGuid bot, char const* event) const;

    // Says it: resolves the pawn's voice, applies Pawn.VoiceKillChancePercent to a kill and
    // the per-pawn, per-event Pawn.VoiceCooldownSeconds to every event, and speaks in the
    // pawn's own faction language. Silent when there is nothing to say, so a call site can
    // call it unconditionally.
    void SayLine(Player* bot, ObjectGuid owner, char const* event) const;

    [[nodiscard]] uint32 TemperamentPools() const;
    [[nodiscard]] uint32 VoiceCount() const;

private:
    static uint32 Key(uint8 cls, uint8 spec) { return (uint32(cls) << 8) | uint32(spec); }

    std::unordered_map<uint32, std::vector<PawnProfile>> _temperaments;  // Key -> pool
    std::vector<PawnProfile>                             _voices;

    // One voice's lines: voice label -> event -> the texts it may say. Keyed by label
    // rather than by pool index because the configuration names a voice, and a label the
    // pool does not define is a line no pawn can ever draw.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<std::string>>> _lines;

    // When each pawn last spoke each event, in getMSTime(). Mutable because speaking is a
    // read of the pool plus a cooldown bookkeeping step, and the callers hold the manager
    // through its singleton.
    mutable std::unordered_map<uint64, uint32> _spoken;   // (bot counter << 8) | event id

    // How often a pawn speaks, and how long it then stays quiet, in milliseconds. Read in
    // Load() rather than through PawnMgr's config struct: pacing speech is the personality's
    // own concern, and the keys reload with the pools.
    uint32 _killChancePercent = 10;
    uint32 _cooldownMs = 120000;
};

#define sPawnPersonality PawnPersonality::instance()

#endif
