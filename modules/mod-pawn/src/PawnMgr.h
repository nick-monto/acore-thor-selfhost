/*
 * mod-pawn: the roster, the offers and the party.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#ifndef MOD_PAWN_MGR_H
#define MOD_PAWN_MGR_H

#include "pawn_common.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

class PawnMgr
{
public:
    static PawnMgr& instance();

    void LoadConfig();
    void LoadRoster();   // creates the roster table when it is absent, then reads it

    // Reads the type-2 account pool: which characters exist, their class, race
    // and level, and which of them are offline. Called at startup, on the
    // configured refresh timer and on demand when the pool is still empty.
    void RefreshPool();

    // Called from the player login hook. A pawn is only marked; a real player's
    // party is restored from the roster once they are in the world.
    void OnPlayerLogin(Player* player);

    // Drains the marks: restores parties and applies archetypes once the
    // character is actually in the world. Also rolls the pool over.
    void Update(uint32 diff);

    [[nodiscard]] PawnConfig const& Config() const { return _config; }
    [[nodiscard]] bool IsPawn(ObjectGuid guid) const;

    // The picker's rows for this player: the selection dealt for the current pool
    // cycle, between FeaturedMin and FeaturedMax rows. It is dealt the first time
    // the player opens the picker after a pool refresh or a restart and kept
    // afterwards, so every open shows the same crowd until RefreshPool() clears it.
    // The rows themselves are rebuilt from the selection on each call, so a pawn's
    // level, gear and fee follow the player's own level instead of freezing at the
    // level they were first shown at. A selection with nothing still eligible left
    // on it is dealt again, because a menu with no rows cannot be browsed.
    [[nodiscard]] std::vector<PawnOffer> SelectionFor(Player* player);
    [[nodiscard]] std::vector<PawnRosterRow> ListParty(ObjectGuid owner) const;
    [[nodiscard]] uint32 CountParty(ObjectGuid owner) const;

    // Copper a hire of this offer costs this player, right now: 0 when the fee
    // is switched off or this owner has already paid for this pawn.
    [[nodiscard]] uint32 HireFee(Player* player, PawnOffer const& offer) const;

    bool Recruit(Player* player, PawnOffer const& offer, std::string& message);
    bool Dismiss(Player* player, ObjectGuid bot, std::string& message);
    [[nodiscard]] bool IsSpellForbidden(Player* bot, uint32 spellId) const;

    // The variant abilities this pawn is missing: a bounded, deterministic
    // subset of its spec's variant pool, drawn from the same seed as its level,
    // spec and gear ratio, so two pawns of one spec differ and one pawn does not
    // change under its owner.
    [[nodiscard]] std::vector<PawnVariantSpell> WithheldVariants(ObjectGuid owner, ObjectGuid bot,
                                                                uint8 cls, uint8 spec) const;

private:
    [[nodiscard]] uint8 DeriveLevel(ObjectGuid owner, ObjectGuid bot, uint8 ownerLevel, uint8 cls) const;
    // The gear ratio a pawn is offered (and geared) at, in percent of the owner's
    // gear score. A pure function of the two guids, like the level and the spec.
    [[nodiscard]] uint16 GearRatio(ObjectGuid owner, ObjectGuid bot) const;
    [[nodiscard]] PawnOffer MakeOffer(PawnPoolChar const& poolChar, ObjectGuid owner, uint8 ownerLevel) const;
    [[nodiscard]] bool IsRecruitedBy(ObjectGuid owner, ObjectGuid bot) const;

    void EnsureSchema();
    void RestoreParty(Player* player);
    void ApplyPawn(Player* bot);
    void Upsert(PawnRosterRow const& row);

    // A player's deck: the eligible pool in a random deal order, dealt without
    // replacement. It is dealt when the player's selection is dealt - once per
    // pool cycle - so no face comes round until the deck has been dealt out, and
    // the faces of the previous selection are dealt last after a reshuffle, so
    // none of them lands in the very next selection. Counters, not pointers: the
    // pool is re-read under it, and an entry that is no longer a candidate is
    // skipped when it comes up.
    struct PawnDeck
    {
        std::vector<uint32> Order;
        size_t              Cursor = 0;
        // The faces the last selection showed. A reshuffle deals them last, so
        // the one moment a repeat could land in the very next selection does not.
        std::vector<uint32> Recent;
    };

    PawnConfig                                _config;
    std::vector<PawnPoolChar>                 _pool;           // type-2 characters, offline at refresh
    uint32                                    _poolRefreshDue = 0;  // getMSTime() when the pool is re-read
    uint64                                    _drawCounter    = 0;  // makes every picker instance differ
    std::unordered_map<uint32, PawnRosterRow> _rows;           // bot counter -> row
    std::unordered_set<uint32>                _pendingPawns;   // bot counters
    std::unordered_set<uint32>                _pendingOwners;  // player counters
    std::unordered_map<uint32, PawnDeck>      _decks;          // player counter -> deal order
    // The pawns on offer to a player for the current pool cycle, in the order the
    // deck dealt them. Cleared with the decks when the pool is re-read, and never
    // by a menu: opening the picker again shows the same selection.
    std::unordered_map<uint32, std::vector<uint32>> _selections;   // player counter -> bot counters
};

#define sPawnMgr PawnMgr::instance()

#endif
