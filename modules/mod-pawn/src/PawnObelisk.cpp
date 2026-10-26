/*
 * mod-pawn: the obelisk.
 *
 * A gameobject with a ScriptName is the supported way to build a gossip menu in
 * code: GameObject::Use calls sScriptMgr->OnGossipHello for every gameobject
 * type before its own type switch, and a GameObjectScript bound to the entry's
 * ScriptName owns the interaction by returning true
 * (src/server/game/Entities/GameObject/GameObject.cpp:1499-1513). The Icecrown
 * Citadel teleporter is the in-tree precedent
 * (icecrown_citadel_teleport.cpp:44-88).
 *
 * Returning true matters twice: it stops the core's fallback path, which
 * rejects any option above GOSSIP_OPTION_QUESTGIVER for a gameobject
 * (PlayerGossip.cpp:279-291), and it keeps a code-built menu - one with no
 * GossipMenuItemData - from silently swallowing the click.
 *
 * Pages carry at most Pawn.OfferedPerPage offers plus the party rows and two
 * navigation rows, because the gossip packet carries at most 16 options
 * (GossipDef.cpp:199-201) and a 33rd aborts the server with an ASSERT rather
 * than clamping (GossipDef.cpp:43).
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnArchetype.h"
#include "PawnMgr.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "GameObject.h"
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
    // beyond the per-player cache below.
    constexpr uint32 PAWN_ACTION_RECRUIT = 1;    // 1..99   index into the offers
    constexpr uint32 PAWN_ACTION_DISMISS = 101;  // 101..199 index into the party
    constexpr uint32 PAWN_ACTION_NEXT    = 201;
    constexpr uint32 PAWN_ACTION_PREV    = 202;
    constexpr uint32 PAWN_ACTION_SPAN    = 100;

    struct PawnMenuState
    {
        std::vector<PawnOffer>     Offers;
        std::vector<PawnRosterRow> Party;
        uint8                      Page = 0;
    };

    // Per-player, rebuilt on every hello. A click on a menu from before a
    // restart finds no entry and is answered by reopening the menu rather than
    // acting on a guid that may no longer be offered.
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

class go_pawn_obelisk : public GameObjectScript
{
public:
    go_pawn_obelisk() : GameObjectScript("go_pawn_obelisk") { }

    [[nodiscard]] bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (!sPawnMgr.Config().Enabled)
        {
            Notify(player, "The obelisk is dormant.");
            CloseGossipMenuFor(player);
            return true;
        }

        ShowMenu(player, go, 0);
        return true;
    }

    [[nodiscard]] bool OnGossipSelect(Player* player, GameObject* go, uint32 /*sender*/, uint32 action) override
    {
        auto it = g_menus.find(player->GetGUID().GetCounter());
        if (it == g_menus.end())
        {
            ShowMenu(player, go, 0);
            return true;
        }

        PawnMenuState& state = it->second;

        if (action == PAWN_ACTION_NEXT)
        {
            ShowMenu(player, go, uint8(state.Page + 1));
            return true;
        }

        if (action == PAWN_ACTION_PREV)
        {
            ShowMenu(player, go, uint8(state.Page ? state.Page - 1 : 0));
            return true;
        }

        if (action >= PAWN_ACTION_RECRUIT && action < PAWN_ACTION_RECRUIT + PAWN_ACTION_SPAN)
        {
            uint32 const index = action - PAWN_ACTION_RECRUIT;
            if (index < state.Offers.size())
            {
                std::string message;
                sPawnMgr.Recruit(player, state.Offers[index], message);
                Notify(player, message);
            }
            ShowMenu(player, go, state.Page);
            return true;
        }

        if (action >= PAWN_ACTION_DISMISS && action < PAWN_ACTION_DISMISS + PAWN_ACTION_SPAN)
        {
            uint32 const index = action - PAWN_ACTION_DISMISS;
            if (index < state.Party.size())
            {
                std::string message;
                sPawnMgr.Dismiss(player, state.Party[index].Bot, message);
                Notify(player, message);
            }
            ShowMenu(player, go, state.Page);
            return true;
        }

        ShowMenu(player, go, 0);
        return true;
    }

private:
    static void ShowMenu(Player* player, GameObject* go, uint8 page)
    {
        ClearGossipMenuFor(player);

        PawnMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Offers = sPawnMgr.BuildOffers(player);
        state.Party = sPawnMgr.ListParty(player->GetGUID());

        // The party rows and the two navigation rows share the 16-option page
        // with the offers, so the page size follows from the party size rather
        // than from the configured number alone.
        uint32 const partyRows = uint32(state.Party.size());
        uint32 const navigationRows = 2;
        uint32 const maxPage = 16 > partyRows + navigationRows ? 16 - partyRows - navigationRows : 1;
        uint32 const perPage = std::max<uint32>(1, std::min<uint32>(sPawnMgr.Config().OfferedPerPage, maxPage));

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

        for (uint32 i = first; i < last; ++i)
        {
            PawnOffer const& offer = state.Offers[i];

            std::string text = offer.Name;
            if (text.empty())
                text = "An unnamed pawn";
            text += " - " + std::string(ClassName(offer.Class)) + ", " + offer.SpecName +
                    ", level " + std::to_string(uint32(offer.Level)) +
                    " (" + RoleName(offer.Role) + ")";

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, text, GOSSIP_SENDER_MAIN, PAWN_ACTION_RECRUIT + i);
        }

        if (pages > 1)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next page (" + std::to_string(uint32(state.Page) + 1) +
                                                       "/" + std::to_string(pages) + ")",
                             GOSSIP_SENDER_MAIN, PAWN_ACTION_NEXT);
            if (state.Page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", GOSSIP_SENDER_MAIN, PAWN_ACTION_PREV);
        }

        SendGossipMenuFor(player, player->GetGossipTextId(go), go->GetGUID());
    }
};

void AddPawnObeliskScript()
{
    new go_pawn_obelisk();
}
