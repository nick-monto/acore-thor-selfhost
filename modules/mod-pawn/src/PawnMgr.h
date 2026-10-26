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

    // One picker instance: a fresh random subsample of the pool, between
    // FeaturedMin and FeaturedMax rows. Two calls return different rows, so the
    // list is drawn once per menu and kept for as long as the player browses it.
    [[nodiscard]] std::vector<PawnOffer> DrawOffers(Player* player);
    [[nodiscard]] std::vector<PawnRosterRow> ListParty(ObjectGuid owner) const;
    [[nodiscard]] uint32 CountParty(ObjectGuid owner) const;

    // Copper a hire of this offer costs this player, right now: 0 when the fee
    // is switched off or this owner has already paid for this pawn.
    [[nodiscard]] uint32 HireFee(Player* player, PawnOffer const& offer) const;

    bool Recruit(Player* player, PawnOffer const& offer, std::string& message);
    bool Dismiss(Player* player, ObjectGuid bot, std::string& message);
    [[nodiscard]] bool IsSpellForbidden(Player* bot, uint32 spellId) const;

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

    PawnConfig                                _config;
    std::vector<PawnPoolChar>                 _pool;           // type-2 characters, offline at refresh
    uint32                                    _poolRefreshDue = 0;  // GetMSTime() when the pool is re-read
    uint64                                    _drawCounter    = 0;  // makes every picker instance differ
    std::unordered_map<uint32, PawnRosterRow> _rows;           // bot counter -> row
    std::unordered_set<uint32>                _pendingPawns;   // bot counters
    std::unordered_set<uint32>                _pendingOwners;  // player counters
};

#define sPawnMgr PawnMgr::instance()

#endif
