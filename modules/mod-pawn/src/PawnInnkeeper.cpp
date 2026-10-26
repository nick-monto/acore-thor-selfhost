/*
 * mod-pawn: the picker, on the innkeepers.
 *
 * Every innkeeper in the world carries one extra gossip line. A CreatureScript
 * hook runs for every creature, so no template row, no ScriptName and no spawn
 * is involved: the line exists wherever an innkeeper does, and nowhere else.
 *
 * Two details decide the shape of this file.
 *
 * Returning false from OnGossipHello leaves the innkeeper's own menu alone, so
 * that is what every creature except an innkeeper gets
 * (src/server/game/Handlers/NPCHandler.cpp:188-193). For an innkeeper the menu
 * is built here instead - Player::PrepareGossipMenu with the creature's own menu
 * id, so "make this inn your home" and any quest rows survive, plus our line -
 * and Player::SendPreparedGossip sends it.
 *
 * Returning false from OnGossipSelect hands the click back to the core, which is
 * how the innkeeper's own options keep working
 * (src/server/game/Handlers/MiscHandler.cpp:201-203). The action codes below
 * therefore start at 1000: a database gossip row's action is its OptionType,
 * and GOSSIP_OPTION_MAX is 21 (src/server/game/Entities/Creature/GossipDef.h:33-57),
 * so a range based at 1 would swallow the bind option.
 *
 * Pages carry at most Pawn.OfferedPerPage offers plus the party rows and two
 * navigation rows, because the gossip packet carries at most 16 options
 * (GossipDef.cpp:199-201) and a 33rd aborts the server with an ASSERT rather
 * than clamping (GossipDef.cpp:43).
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnMgr.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Action code ranges. The action travels through the gossip menu untouched
    // and comes back in OnGossipSelect, so it is the only state the click needs
    // beyond the per-player cache below. Both indexes are page-local: a page
    // holds at most 16 rows, so a page-local index always fits its range however
    // many candidates a draw has.
    constexpr uint32 PAWN_ACTION_OPEN    = 1000;
    constexpr uint32 PAWN_ACTION_RECRUIT = 1010;  // 1010..1099 offer on this page
    constexpr uint32 PAWN_ACTION_DISMISS = 1110;  // 1110..1199 pawn in the party
    constexpr uint32 PAWN_ACTION_NEXT    = 1200;
    constexpr uint32 PAWN_ACTION_PREV    = 1201;
    constexpr uint32 PAWN_ACTION_SPAN    = 90;

    struct PawnMenuState
    {
        std::vector<PawnOffer>     Offers;   // the draw for this menu instance
        std::vector<PawnRosterRow> Party;
        uint8                      Page = 0;
        uint32                     First = 0;   // index of the first offer on the page
    };

    // Per-player, rebuilt whenever the picker is opened. A click on a menu from
    // before a restart finds no entry and is answered by reopening the picker
    // rather than acting on a guid that may no longer be offered.
    std::unordered_map<uint32, PawnMenuState> g_menus;

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Adventurer";
        }
    }

    char const* RoleName(PawnRole role)
    {
        switch (role)
        {
            case PAWN_ROLE_TANK:   return "tank";
            case PAWN_ROLE_HEALER: return "healer";
            case PAWN_ROLE_DPS:    return "dps";
            default:               return "pawn";
        }
    }

    void Notify(Player* player, std::string const& text)
    {
        if (!text.empty())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }
}

class PawnInnkeeperScript : public CreatureScript
{
public:
    PawnInnkeeperScript() : CreatureScript("npc_pawn_innkeeper") { }

    [[nodiscard]] bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!sPawnMgr.Config().Enabled || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;   // not ours: let the core send whatever this creature has

        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        // One line, on a menu the creature already has. A menu that fills the client's 16 visible
        // rows gets no line: a 17th is not shown anyway, and adding past GOSSIP_MAX_MENU_ITEMS
        // aborts the server with an ASSERT rather than clamping (GossipDef.cpp:43).
        if (player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() < 15)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Recruit companions", GOSSIP_SENDER_MAIN, PAWN_ACTION_OPEN);

        player->SendPreparedGossip(creature);
        return true;
    }

    [[nodiscard]] bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (action < PAWN_ACTION_OPEN || action > PAWN_ACTION_PREV)
            return false;   // the innkeeper's own options: bind, quests, vendor

        auto it = g_menus.find(player->GetGUID().GetCounter());
        if (it == g_menus.end())
        {
            Open(player, creature);
            return true;
        }

        PawnMenuState& state = it->second;

        if (action == PAWN_ACTION_OPEN)
        {
            Open(player, creature);
            return true;
        }

        if (action == PAWN_ACTION_NEXT)
        {
            ShowMenu(player, creature, uint8(state.Page + 1));
            return true;
        }

        if (action == PAWN_ACTION_PREV)
        {
            ShowMenu(player, creature, uint8(state.Page ? state.Page - 1 : 0));
            return true;
        }

        if (action >= PAWN_ACTION_RECRUIT && action < PAWN_ACTION_RECRUIT + PAWN_ACTION_SPAN)
        {
            // The action carries the index within the page, not within the draw:
            // a draw of several hundred would otherwise run past the dismissal
            // range and collide with the page actions.
            uint32 const index = state.First + (action - PAWN_ACTION_RECRUIT);
            if (index < state.Offers.size())
            {
                PawnOffer const offer = state.Offers[index];

                std::string message;
                if (sPawnMgr.Recruit(player, offer, message))
                    state.Offers.erase(state.Offers.begin() + index);

                Notify(player, message);
            }
            ShowMenu(player, creature, state.Page);
            return true;
        }

        uint32 const index = action - PAWN_ACTION_DISMISS;
        if (index < state.Party.size())
        {
            std::string message;
            sPawnMgr.Dismiss(player, state.Party[index].Bot, message);
            Notify(player, message);
        }
        ShowMenu(player, creature, state.Page);
        return true;
    }

private:
    // A new picker instance: draw a fresh random subsample of the pool. The draw
    // is kept until the player opens the picker again, so browsing pages or
    // dismissing a pawn does not move the rows under the pointer.
    static void Open(Player* player, Creature* creature)
    {
        PawnMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Offers = sPawnMgr.DrawOffers(player);
        state.Page = 0;
        state.First = 0;
        ShowMenu(player, creature, 0);
    }

    static void ShowMenu(Player* player, Creature* creature, uint8 page)
    {
        ClearGossipMenuFor(player);

        PawnMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Party = sPawnMgr.ListParty(player->GetGUID());

        // The party rows and the two navigation rows share the 16-option page
        // with the offers, so the page size follows from the party size rather
        // than from the configured number alone.
        uint32 const partyRows = uint32(state.Party.size());
        uint32 const navigationRows = 2;
        uint32 const maxPage = 16 > partyRows + navigationRows ? 16 - partyRows - navigationRows : 1;
        uint32 const perPage = std::max<uint32>(1, std::min<uint32>(
            std::min<uint32>(sPawnMgr.Config().OfferedPerPage, maxPage), PAWN_ACTION_SPAN));

        uint32 const pages = state.Offers.empty() ? 1 : uint32((state.Offers.size() + perPage - 1) / perPage);
        state.Page = uint8(std::min<uint32>(page, pages - 1));

        for (uint32 i = 0; i < partyRows; ++i)
        {
            PawnRosterRow const& row = state.Party[i];
            std::string name;
            sCharacterCache->GetCharacterNameByGuid(row.Bot, name);

            std::string text = "Dismiss " + (name.empty() ? std::string("your pawn") : name);
            text += " (" + std::string(RoleName(row.Role)) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, PAWN_ACTION_DISMISS + i);
        }

        uint32 const first = uint32(state.Page) * perPage;
        uint32 const last = std::min<uint32>(first + perPage, uint32(state.Offers.size()));
        state.First = first;

        for (uint32 i = first; i < last; ++i)
        {
            PawnOffer const& offer = state.Offers[i];

            std::string text = offer.Name;
            if (text.empty())
                text = "An unnamed pawn";
            text += " - " + std::string(ClassName(offer.Class)) + ", " + offer.SpecName +
                    ", level " + std::to_string(uint32(offer.Level)) +
                    " (" + RoleName(offer.Role) + ")";

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, text, GOSSIP_SENDER_MAIN,
                             PAWN_ACTION_RECRUIT + (i - first));
        }

        if (state.Offers.empty() && partyRows == 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No companion is free to travel right now",
                             GOSSIP_SENDER_MAIN, PAWN_ACTION_OPEN);

        if (pages > 1)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next page (" + std::to_string(uint32(state.Page) + 1) +
                                                       "/" + std::to_string(pages) + ")",
                             GOSSIP_SENDER_MAIN, PAWN_ACTION_NEXT);
            if (state.Page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", GOSSIP_SENDER_MAIN, PAWN_ACTION_PREV);
        }

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
    }
};

void AddPawnInnkeeperScript()
{
    new PawnInnkeeperScript();
}

void PawnMenuForgetPlayer(ObjectGuid player)
{
    g_menus.erase(player.GetCounter());
}
