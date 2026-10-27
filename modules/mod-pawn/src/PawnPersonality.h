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

    [[nodiscard]] uint32 TemperamentPools() const;
    [[nodiscard]] uint32 VoiceCount() const;

private:
    static uint32 Key(uint8 cls, uint8 spec) { return (uint32(cls) << 8) | uint32(spec); }

    std::unordered_map<uint32, std::vector<PawnProfile>> _temperaments;  // Key -> pool
    std::vector<PawnProfile>                             _voices;
};

#define sPawnPersonality PawnPersonality::instance()

#endif
