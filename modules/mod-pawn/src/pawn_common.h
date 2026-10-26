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
    // What a hire costs: HireFeePercent of the player's level cubed, in copper,
    // times the pawn's own gear ratio (below), times HireFeeHeldPercent of that
    // again for every pawn already in the party.
    uint32 HireFeePercent       = 100;
    uint32 HireFeeHeldPercent   = 100;
    // The band of the per-pawn gear ratio, and of the hire fee with it: a pawn is
    // geared to between GearFloorPercent and GearCapPercent of the owner's gear
    // score, and pays in proportion. 100 is the baseline - a pawn geared like the
    // player - and the draw is triangular around it, so most pawns sit near the
    // player's strength and the extremes are rare.
    uint32 GearFloorPercent     = 85;
    uint32 GearCapPercent       = 150;
    // Whether a hired pawn keeps the XP it earns in the party. Off by default: a
    // pawn that levels with its owner never needs exchanging, and the roster is
    // meant to be a set of contracts that expire as the player outgrows them.
    bool   GainXP               = false;
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
    // Percent of the owner's gear score this pawn is geared to: 100 is a match,
    // the band is [GearFloorPercent, GearCapPercent]. Drawn from the same seed as
    // the level, so a pawn is always offered at the same strength for the same
    // player, and the hire fee is this ratio applied to the level-cubed baseline.
    uint16      GearRatio = 100;
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
    // Set once the pawn has been equipped for its pinned spec at its level. A
    // fresh pool character is level 1 in a start outfit, so "not built yet" is
    // the one case that must always run the gear pass; afterwards the pawn keeps
    // whatever it wears, including anything the player gives it.
    bool       Built       = false;
    // Set once this owner has paid the hire fee for this pawn. Re-hiring a pawn
    // you have already paid for is free, so swapping companions costs nothing
    // after the first time.
    bool       Paid        = false;
};

// "51g 20s" / "20s 5c" / "0c": the price strings the picker and the recruit
// message show. Zero components are dropped, so the smallest fee is "1c".
std::string FormatMoney(uint32 copper);

#endif
