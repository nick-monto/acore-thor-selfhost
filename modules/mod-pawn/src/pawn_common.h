/*
 * mod-pawn: shared types.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_PAWN_COMMON_H
#define MOD_PAWN_COMMON_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <unordered_map>
#include <vector>

enum PawnRole : uint8
{
    PAWN_ROLE_NONE   = 0,
    PAWN_ROLE_TANK   = 1,
    PAWN_ROLE_HEALER = 2,
    PAWN_ROLE_DPS    = 3
};

struct PawnConfig
{
    bool   Enabled              = true;
    uint8  MaxPawns             = 2;
    uint8  LevelWindow          = 2;
    uint8  OfferedPerPage       = 12;
    // A picker instance shows a random subsample of the pool, not the pool: the
    // count is drawn from [FeaturedMin, FeaturedMax] every time the menu opens.
    uint8  FeaturedMin          = 10;
    uint8  FeaturedMax          = 15;
    // The pool itself (which characters exist, their level and whether they are
    // online) is re-read on this many minutes, drawn fresh from the range each
    // cycle, and once at startup.
    uint16 PoolRefreshMinMinutes = 40;
    uint16 PoolRefreshMaxMinutes = 60;
    bool   Debug                = false;
};

// One character of the type-2 (AddClass) account pool, as the module last read
// it. Names and levels come from the characters table, so the picker does not
// depend on sCharacterCache having seen a freshly created pool character.
struct PawnPoolChar
{
    ObjectGuid  Bot;
    std::string Name;
    uint8       Class = 0;
    uint8       Race  = 0;
    uint8       Level = 0;
};

// One row of the picker: a pool pawn the player may recruit, with the level and
// the spec it will have if they do. Both are derived from the two guids, so the
// same pair always produces the same offer.
struct PawnOffer
{
    ObjectGuid  Bot;
    std::string Name;
    std::string SpecName;
    uint8       Class    = 0;
    uint8       Level    = 0;
    uint8       SpecNo   = 0;
    PawnRole    Role     = PAWN_ROLE_NONE;
};

// One row of pawn_roster.
struct PawnRosterRow
{
    ObjectGuid Owner;
    ObjectGuid Bot;
    uint8      SpecNo      = 0;
    PawnRole   Role        = PAWN_ROLE_NONE;
    bool       Recruited   = false;
    uint8      TargetLevel = 0;
};

#endif
