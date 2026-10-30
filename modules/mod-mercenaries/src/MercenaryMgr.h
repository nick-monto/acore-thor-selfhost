/*
 * mod-mercenaries: the roster, the offers and the party.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_MERCENARY_MGR_H
#define MOD_MERCENARY_MGR_H

#include "mercenary_common.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

class MercenaryMgr
{
public:
    static MercenaryMgr& instance();

    void LoadConfig();
    void LoadRoster();   // creates the roster table when it is absent, then reads it

    // Reads the type-2 account pool: which characters exist, their class, race
    // and level, and which of them are offline. Called at startup, on the
    // configured refresh timer and on demand when the pool is still empty.
    void RefreshPool();

    // Called from the player login hook. A mercenary is only marked; a real player's
    // party is restored from the roster once they are in the world.
    void OnPlayerLogin(Player* player);

    // Drains the marks: restores parties and applies archetypes once the
    // character is actually in the world. Also rolls the pool over.
    void Update(uint32 diff);

    [[nodiscard]] MercenaryConfig const& Config() const { return _config; }
    [[nodiscard]] bool IsMercenary(ObjectGuid guid) const;

    // The picker's rows for this player: the selection dealt for the current pool
    // cycle, between FeaturedMin and FeaturedMax rows. It is dealt the first time
    // the player opens the picker after a pool refresh or a restart and kept
    // afterwards, so every open shows the same crowd until RefreshPool() clears it.
    // The rows themselves are rebuilt from the selection on each call, so a mercenary's
    // level, gear and fee follow the player's own level instead of freezing at the
    // level they were first shown at. A selection with nothing still eligible left
    // on it is dealt again, because a menu with no rows cannot be browsed.
    [[nodiscard]] std::vector<MercenaryOffer> SelectionFor(Player* player);
    [[nodiscard]] std::vector<MercenaryRosterRow> ListParty(ObjectGuid owner) const;
    [[nodiscard]] uint32 CountParty(ObjectGuid owner) const;

    // Copper a hire of this offer costs this player, right now: 0 when the fee
    // is switched off or this owner has already paid for this mercenary.
    [[nodiscard]] uint32 HireFee(Player* player, MercenaryOffer const& offer) const;

    bool Recruit(Player* player, MercenaryOffer const& offer, std::string& message);
    bool Dismiss(Player* player, ObjectGuid bot, std::string& message);

    // Forwards one order to an online mercenary of this player. The verb is the whisper
    // vocabulary playerbots already implements - follow, stay, attack, flee - and is
    // handed to the same entry point a whisper reaches, so a click at the innkeeper and
    // a typed whisper cannot diverge. Anything else is refused here rather than sent.
    bool Order(Player* player, ObjectGuid bot, char const* verb, std::string& message);

    // Copper a repair-and-restock of one mercenary costs this player: a percentage of
    // player_level^2 * 10, so the price follows the player rather than the mercenary's gear.
    // 0 when the fee is switched off.
    [[nodiscard]] uint32 ResupplyFee(Player* player) const;

    // Repairs an online mercenary and replaces the food, potions, reagents and ammunition it
    // carries. The four consumable classes are purged first: the factory's Init* calls
    // append, so restocking a mercenary that still has its food would fill its bags.
    bool Resupply(Player* player, ObjectGuid bot, std::string& message);

    // The offer this roster row would be at the player's current level: the same bot and
    // the same spec, the level re-derived through DeriveLevel, so a mercenary the player has
    // outgrown can be brought forward without being exchanged for another face.
    [[nodiscard]] MercenaryOffer OfferFor(Player* player, MercenaryRosterRow const& row) const;

    // Rebuilds an online mercenary of this player at the level it would be offered at now. It
    // keeps the character, its spec, its temperament and its affinity; only the level, the
    // fee and the gear pass change, which is what a re-hire at a new level does anyway.
    bool Retrain(Player* player, ObjectGuid bot, std::string& message);

    // How many whole steps of affinity this mercenary has earned, clamped to the most it can
    // use: one step returns one withheld variant, and a mercenary cannot be handed back more
    // than the draw could have withheld. The step count is the same function everywhere
    // it matters - the build, the spell gate and the picker - so they cannot disagree.
    [[nodiscard]] uint32 AffinitySteps(MercenaryRosterRow const& row) const;

    // The most steps a mercenary can use: the configured withhold maximum, because that is the
    // largest number of variants a draw can take away. The picker shows the bond against it.
    [[nodiscard]] uint32 AffinityCap() const;

    // Credits affinity points to every mercenary this owner has in the party. Called from the
    // kill and quest hooks, so it runs once per kill: the row is updated in memory every
    // time and written to the database only when a step count moves. Keyed by owner
    // rather than by Player* because a mercenary can land a kill while its owner is loading.
    void AwardAffinity(ObjectGuid owner, uint32 points);

    // The owner of this mercenary, or an empty guid when the character is not one. The hooks
    // need it to credit a kill a mercenary landed to the player it follows.
    [[nodiscard]] ObjectGuid OwnerOf(ObjectGuid mercenary) const;

    [[nodiscard]] bool IsSpellForbidden(Player* bot, uint32 spellId) const;

    // The variant abilities this mercenary is missing: a bounded, deterministic
    // subset of its spec's variant pool, drawn from the same seed as its level,
    // spec and gear ratio, so two mercenaries of one spec differ and one mercenary does not
    // change under its owner.
    [[nodiscard]] std::vector<MercenaryVariantSpell> WithheldVariants(ObjectGuid owner, ObjectGuid bot,
                                                                uint8 cls, uint8 spec) const;

private:
    [[nodiscard]] uint8 DeriveLevel(ObjectGuid owner, ObjectGuid bot, uint8 ownerLevel, uint8 cls) const;
    // The gear ratio a mercenary is offered (and geared) at, in percent of the owner's
    // gear score. A pure function of the two guids, like the level and the spec.
    [[nodiscard]] uint16 GearRatio(ObjectGuid owner, ObjectGuid bot) const;
    [[nodiscard]] MercenaryOffer MakeOffer(MercenaryPoolChar const& poolChar, ObjectGuid owner, uint8 ownerLevel) const;
    [[nodiscard]] bool IsRecruitedBy(ObjectGuid owner, ObjectGuid bot) const;

    void EnsureSchema();
    void RestoreParty(Player* player);
    void ApplyMercenary(Player* bot);
    void Upsert(MercenaryRosterRow const& row);

    // A player's deck: the eligible pool in a random deal order, dealt without
    // replacement. It is dealt when the player's selection is dealt - once per
    // pool cycle - so no face comes round until the deck has been dealt out, and
    // the faces of the previous selection are dealt last after a reshuffle, so
    // none of them lands in the very next selection. Counters, not pointers: the
    // pool is re-read under it, and an entry that is no longer a candidate is
    // skipped when it comes up.
    struct MercenaryDeck
    {
        std::vector<uint32> Order;
        size_t              Cursor = 0;
        // The faces the last selection showed. A reshuffle deals them last, so
        // the one moment a repeat could land in the very next selection does not.
        std::vector<uint32> Recent;
    };

    MercenaryConfig                                _config;
    std::vector<MercenaryPoolChar>                 _pool;           // type-2 characters, offline at refresh
    uint32                                    _poolRefreshDue = 0;  // getMSTime() when the pool is re-read
    uint64                                    _drawCounter    = 0;  // makes every picker instance differ
    std::unordered_map<uint32, MercenaryRosterRow> _rows;           // bot counter -> row
    std::unordered_set<uint32>                _pendingMercenaries;   // bot counters
    std::unordered_set<uint32>                _pendingOwners;  // player counters
    std::unordered_map<uint32, MercenaryDeck>      _decks;          // player counter -> deal order
    // The mercenaries on offer to a player for the current pool cycle, in the order the
    // deck dealt them. Cleared with the decks when the pool is re-read, and never
    // by a menu: opening the picker again shows the same selection.
    std::unordered_map<uint32, std::vector<uint32>> _selections;   // player counter -> bot counters
};

#define sMercenaryMgr MercenaryMgr::instance()

#endif
