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

    // Called from the player login hook. A pawn is only marked; a real player's
    // party is restored from the roster once they are in the world.
    void OnPlayerLogin(Player* player);

    // Drains the marks: restores parties and applies archetypes once the
    // character is actually in the world.
    void Update(uint32 diff);

    [[nodiscard]] PawnConfig const& Config() const { return _config; }
    [[nodiscard]] bool IsPawn(ObjectGuid guid) const;
    [[nodiscard]] PawnRosterRow const* GetRow(ObjectGuid bot) const;

    [[nodiscard]] std::vector<PawnOffer> BuildOffers(Player* player) const;
    [[nodiscard]] std::vector<PawnRosterRow> ListParty(ObjectGuid owner) const;
    [[nodiscard]] uint32 CountParty(ObjectGuid owner) const;

    bool Recruit(Player* player, PawnOffer const& offer, std::string& message);
    bool Dismiss(Player* player, ObjectGuid bot, std::string& message);
    [[nodiscard]] bool IsSpellForbidden(Player* bot, uint32 spellId) const;

private:
    [[nodiscard]] uint8 DeriveLevel(ObjectGuid owner, ObjectGuid bot, uint8 ownerLevel, uint8 cls) const;
    [[nodiscard]] PawnOffer MakeOffer(uint8 cls, ObjectGuid owner, uint8 ownerLevel, ObjectGuid bot) const;
    [[nodiscard]] bool IsRecruitedBy(ObjectGuid owner, ObjectGuid bot) const;

    void EnsureSchema();
    void RestoreParty(Player* player);
    void ApplyPawn(Player* bot);
    void Upsert(PawnRosterRow const& row);

    PawnConfig                                _config;
    std::unordered_map<uint32, PawnRosterRow> _rows;          // bot counter -> row
    std::unordered_set<uint32>                _pendingPawns;  // bot counters
    std::unordered_set<uint32>                _pendingOwners; // player counters
};

#define sPawnMgr PawnMgr::instance()

#endif
