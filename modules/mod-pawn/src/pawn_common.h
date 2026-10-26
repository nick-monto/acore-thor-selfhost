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
    bool   Enabled        = true;
    uint8  MaxPawns       = 2;
    uint8  LevelWindow    = 2;
    uint8  OfferedPerPage = 12;
    uint32 ObeliskEntry   = 5000010;
    bool   Debug          = false;
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
