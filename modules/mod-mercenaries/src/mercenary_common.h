/*
 * mod-mercenaries: shared types.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_MERCENARY_COMMON_H
#define MOD_MERCENARY_COMMON_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <unordered_map>
#include <vector>

enum MercenaryRole : uint8
{
    MERCENARY_ROLE_NONE   = 0,
    MERCENARY_ROLE_TANK   = 1,
    MERCENARY_ROLE_HEALER = 2,
    MERCENARY_ROLE_DPS    = 3
};

struct MercenaryConfig
{
    bool   Enabled              = true;
    uint8  MaxMercenaries             = 2;
    uint8  LevelWindow          = 2;
    // What a hire costs: HireFeePercent of the player's level cubed, in copper,
    // times the mercenary's own gear ratio (below), times HireFeeHeldPercent of that
    // again for every mercenary already in the party.
    uint32 HireFeePercent       = 100;
    uint32 HireFeeHeldPercent   = 100;
    // The band of the per-mercenary gear ratio, and of the hire fee with it: a mercenary is
    // geared to between GearFloorPercent and GearCapPercent of the owner's gear
    // score, and pays in proportion. 100 is the baseline - a mercenary geared like the
    // player - and the draw is triangular around it, so most mercenaries sit near the
    // player's strength and the extremes are rare.
    uint32 GearFloorPercent     = 85;
    uint32 GearCapPercent       = 150;
    // What a repair-and-restock costs, as a percentage of player_level^2 * 10 copper.
    // It follows the player's level rather than the mercenary's gear, so the price of
    // keeping a companion serviceable stays proportional to what the player earns.
    uint32 ResupplyFeePercent   = 100;
    // Whether a hired mercenary keeps the XP it earns in the party. Off by default: a
    // mercenary that levels with its owner never needs exchanging, and the roster is
    // meant to be a set of contracts that expire as the player outgrows them.
    bool   GainXP               = false;
    // Affinity is what a mercenary earns by fighting beside its owner: a point count per
    // (owner, mercenary) that lifts the gear it is built to at its next build. It is the
    // one axis of a mercenary that is not fixed at hire time.
    bool   AffinityEnabled      = true;
    uint32 AffinityKillPoints   = 10;
    uint32 AffinityQuestPoints  = 25;
    // Points per step: the gear ceiling rises by AffinityGearPercent at every whole step,
    // up to AffinityMaxSteps steps. The cap is what the picker shows as the denominator
    // of "bonded n/N", so it is also what stops a bond running past its own reward.
    uint32 AffinityStep         = 100;
    uint32 AffinityGearPercent  = 5;
    uint8  AffinityMaxSteps     = 2;
    uint8  OfferedPerPage       = 12;
    // A picker shows a selection of the pool, not the pool: the count is drawn
    // from [FeaturedMin, FeaturedMax] when the selection is dealt, once per pool
    // cycle, and kept until the pool is re-read.
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
struct MercenaryPoolChar
{
    ObjectGuid  Bot;
    std::string Name;
    uint8       Class = 0;
    uint8       Race  = 0;
    uint8       Level = 0;
};

// One row of the picker: a pool mercenary the player may recruit, with the level and
// the spec it will have if they do. Both are derived from the two guids, so the
// same pair always produces the same offer.
struct MercenaryOffer
{
    ObjectGuid  Bot;
    std::string Name;
    std::string SpecName;
    uint8       Class    = 0;
    uint8       Level    = 0;
    uint8       SpecNo   = 0;
    MercenaryRole    Role     = MERCENARY_ROLE_NONE;
    // The profile words the picker shows, drawn from the same seed as the level
    // so a mercenary's temperament and voice are as stable as everything else about it.
    std::string TemperamentLabel;
    std::string VoiceLabel;
    // Percent of the owner's gear score this mercenary is geared to: 100 is a match,
    // the band is [GearFloorPercent, GearCapPercent]. Drawn from the same seed as
    // the level, so a mercenary is always offered at the same strength for the same
    // player, and the hire fee is this ratio applied to the level-cubed baseline.
    uint16      GearRatio = 100;
};

// One row of mercenary_roster.
struct MercenaryRosterRow
{
    ObjectGuid Owner;
    ObjectGuid Bot;
    uint8      SpecNo      = 0;
    MercenaryRole   Role        = MERCENARY_ROLE_NONE;
    bool       Recruited   = false;
    uint8      TargetLevel = 0;
    // Affinity points earned while the mercenary was in the party. Every whole
    // AffinityStep lifts the gear ceiling of its next build; the roster is the only place
    // it lives, so it survives a relog and a restart with everything else.
    uint32     Affinity    = 0;
    // Set once the mercenary has been equipped for its pinned spec at its level. A
    // fresh pool character is level 1 in a start outfit, so "not built yet" is
    // the one case that must always run the gear pass; afterwards the mercenary keeps
    // whatever it wears, including anything the player gives it.
    bool       Built       = false;
    // Set once this owner has paid the hire fee for this mercenary. Re-hiring a mercenary
    // you have already paid for is free, so swapping companions costs nothing
    // after the first time.
    bool       Paid        = false;
    // The mount spell and pet name the mercenary had when it was built, recorded
    // because the party row is drawn from the roster while the mercenary may be
    // offline. Zero and empty mean the mercenary has neither.
    uint32     MountSpell  = 0;
    std::string PetName;
};

// "51g 20s" / "20s 5c" / "0c": the price strings the picker and the recruit
// message show. Zero components are dropped, so the smallest fee is "1c".
std::string FormatMoney(uint32 copper);

#endif
