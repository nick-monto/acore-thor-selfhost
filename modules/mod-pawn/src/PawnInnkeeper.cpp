/*
 * mod-pawn: the picker, on the innkeepers.
 *
 * Every innkeeper in the world carries one extra gossip line. The picker is a
 * CreatureAI, handed out by an AllCreatureScript to innkeepers that have no AI
 * of their own. An AllCreatureScript is code-only: it registers through
 * ScriptRegistry<AllCreatureScript> without ever being matched against
 * script_name (src/server/game/Scripting/ScriptDefines/AllCreatureScript.cpp:84),
 * and ScriptMgr::GetCreatureAI walks that registry directly
 * (src/server/game/Scripting/ScriptDefines/CreatureScript.cpp:156-168). So no
 * template row, no spawn and no ScriptName is involved: the line exists wherever
 * an innkeeper does, and nowhere else.
 *
 * A named CreatureScript cannot be used here. CreatureScript::IsDatabaseBound()
 * is true unconditionally
 * (src/server/game/Scripting/ScriptDefines/CreatureScript.h:30), so the manager
 * looks the name up in script_name, logs "Script named ... is not assigned in
 * the database" when there is no row, and registers nothing
 * (src/server/game/Scripting/ScriptMgr.h:817-885). And even with a row, gossip is
 * dispatched by the creature's own script id, GetScriptById(creature->GetScriptId())
 * (src/server/game/Scripting/ScriptDefines/CreatureScript.cpp:24-41); an
 * innkeeper's creature_template.ScriptName is the core's npc_innkeeper, so a
 * module script under any other name is never reached for an innkeeper.
 *
 * Two details decide the shape of the rest of this file.
 *
 * The menu is the core's, not ours: NPCHandler.cpp:188-193 calls
 * Player::PrepareGossipMenu with the creature's own menu id - so "make this inn
 * your home" and any quest rows survive - and Player::SendPreparedGossip, and
 * only then runs UnitAI::sGossipHello. So the hello below appends its one line to
 * that prepared menu and re-sends it rather than building a menu of its own.
 *
 * The gossip select hands the AI the gossip list index, not the action, so the
 * action is read back with
 * Player::PlayerTalkClass->GetGossipOptionAction - the same call the core makes
 * for its own dispatch (MiscHandler.cpp:199). An option outside our range is
 * ignored here, and the core still runs the innkeeper's own script afterwards
 * (MiscHandler.cpp:199-200), which is how bind and quests keep working. The
 * action codes below therefore start at 1000: a database gossip row's action is
 * its OptionType, and GOSSIP_OPTION_MAX is 21
 * (src/server/game/Entities/Creature/GossipDef.h:33-57), so a range based at 1
 * would swallow the bind option.
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
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Action code ranges. The action travels through the gossip menu untouched
    // and comes back as PlayerTalkClass->GetGossipOptionAction(gossipListId) in
    // sGossipSelect, so it is the only state the click needs beyond the
    // per-player cache below. Both indexes are page-local: a page
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

    // Playerbots.PremadeSpecName carries its own vocabulary ("prot pve", "shadow
    // pve"). The player does not need the suffix - the row already names the
    // role - so it is trimmed for display only; the stored name is what RoleFor
    // and the blacklist keys use.
    std::string DisplaySpec(std::string spec)
    {
        static std::string const suffix = " pve";
        if (spec.size() > suffix.size() &&
            spec.compare(spec.size() - suffix.size(), suffix.size(), suffix) == 0)
            spec.erase(spec.size() - suffix.size());
        return spec;
    }

    void Notify(Player* player, std::string const& text)
    {
        if (!text.empty())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }
}

class PawnInnkeeperAI : public NullCreatureAI
{
public:
    explicit PawnInnkeeperAI(Creature* creature) : NullCreatureAI(creature) { }

    void sGossipHello(Player* player) override
    {
        if (!sPawnMgr.Config().Enabled || !me->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return;   // not ours: let the core send whatever this creature has

        // One line, on the menu the core has already prepared and sent (NPCHandler.cpp:188-193);
        // building it again here with PrepareGossipMenu would drop that send. A menu that fills the
        // client's 16 visible rows gets no line: a 17th is not shown anyway, and adding past
        // GOSSIP_MAX_MENU_ITEMS aborts the server with an ASSERT rather than clamping (GossipDef.cpp:43).
        if (player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() < 15)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Recruit companions", GOSSIP_SENDER_MAIN, PAWN_ACTION_OPEN);

        player->SendPreparedGossip(me);
    }

    void sGossipSelect(Player* player, uint32 /*sender*/, uint32 action) override
    {
        // The AI is handed the gossip list index, not the action, so the action is read back the
        // way the core does it for its own dispatch (MiscHandler.cpp:199).
        uint32 const option = player->PlayerTalkClass->GetGossipOptionAction(action);
        if (option < PAWN_ACTION_OPEN || option > PAWN_ACTION_PREV)
            return;   // the innkeeper's own options: bind, quests, vendor - the core runs them

        auto it = g_menus.find(player->GetGUID().GetCounter());
        if (it == g_menus.end())
        {
            Open(player, me);
            return;
        }

        PawnMenuState& state = it->second;

        if (option == PAWN_ACTION_OPEN)
        {
            Open(player, me);
            return;
        }

        if (option == PAWN_ACTION_NEXT)
        {
            ShowMenu(player, me, uint8(state.Page + 1));
            return;
        }

        if (option == PAWN_ACTION_PREV)
        {
            ShowMenu(player, me, uint8(state.Page ? state.Page - 1 : 0));
            return;
        }

        if (option >= PAWN_ACTION_RECRUIT && option < PAWN_ACTION_RECRUIT + PAWN_ACTION_SPAN)
        {
            // The option carries the index within the page, not within the draw:
            // a draw of several hundred would otherwise run past the dismissal
            // range and collide with the page actions.
            uint32 const index = state.First + (option - PAWN_ACTION_RECRUIT);
            if (index < state.Offers.size())
            {
                PawnOffer const offer = state.Offers[index];

                std::string message;
                if (sPawnMgr.Recruit(player, offer, message))
                    state.Offers.erase(state.Offers.begin() + index);

                Notify(player, message);
            }
            ShowMenu(player, me, state.Page);
            return;
        }

        uint32 const index = option - PAWN_ACTION_DISMISS;
        if (index < state.Party.size())
        {
            std::string message;
            sPawnMgr.Dismiss(player, state.Party[index].Bot, message);
            Notify(player, message);
        }
        ShowMenu(player, me, state.Page);
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
            text += " (" + std::string(RoleName(row.Role)) + ", level " +
                    std::to_string(uint32(row.TargetLevel)) + ")";

            // A pawn is pinned to the level it was hired at, so the party row is
            // where a player sees one fall behind - and the offers below are the
            // inn's answer to it.
            if (row.TargetLevel + sPawnMgr.Config().LevelWindow < player->GetLevel())
                text += " - fallen behind";

            // Vanity comes from the roster, not the pawn: the pawn is usually
            // offline while the player is browsing, so the row only knows what was
            // recorded at build time. A null spell or an empty recorded value adds
            // nothing rather than a broken clause.
            bool rides = false;
            if (row.MountSpell)
            {
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(row.MountSpell))
                {
                    std::string mountName = info->SpellName[0] ? info->SpellName[0] : "";
                    static std::string const summonPrefix = "Summon ";
                    if (mountName.compare(0, summonPrefix.size(), summonPrefix) == 0)
                        mountName.erase(0, summonPrefix.size());
                    if (!mountName.empty())
                    {
                        text += " - rides " + mountName;
                        rides = true;
                    }
                }
            }

            if (!row.PetName.empty())
                text += (rides ? ", with " : " - with ") + row.PetName;

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, PAWN_ACTION_DISMISS + i);
        }

        uint32 const first = uint32(state.Page) * perPage;
        uint32 const last = std::min<uint32>(first + perPage, uint32(state.Offers.size()));
        state.First = first;

        for (uint32 i = first; i < last; ++i)
        {
            PawnOffer const& offer = state.Offers[i];

            std::string const name = offer.Name.empty() ? std::string("An unnamed pawn") : offer.Name;
            uint32 const fee = sPawnMgr.HireFee(player, offer);

            std::string text = name + " - " + std::string(ClassName(offer.Class)) + ", " +
                               DisplaySpec(offer.SpecName) + ", level " +
                               std::to_string(uint32(offer.Level)) + " (" + RoleName(offer.Role) + ")";

            // The gear score itself is invisible in the vanilla client, so the ratio
            // is what explains why two pawns of the same level cost different money:
            // the dearer one arrives in better gear, the cheaper one in worse.
            if (offer.GearRatio != 100)
                text += ", " + std::string(offer.GearRatio > 100 ? "+" : "-") +
                        std::to_string(offer.GearRatio > 100 ? uint32(offer.GearRatio) - 100
                                                             : 100 - uint32(offer.GearRatio)) + "% gear";

            // How this pawn fights and how it talks: the two axes the level, the
            // spec and the gear do not cover, and the reason two pawns of the same
            // build can still feel like different companions.
            if (!offer.TemperamentLabel.empty())
                text += ", " + offer.TemperamentLabel;
            if (!offer.VoiceLabel.empty())
                text += ", " + offer.VoiceLabel;

            // What this pawn of that spec does not bring. Two fury warriors are
            // different companions partly by which of these they are missing, and
            // the player is buying the difference, so it is on the row.
            if (!offer.Withheld.empty())
            {
                text += ", no ";
                for (size_t i = 0; i < offer.Withheld.size(); ++i)
                {
                    if (i)
                        text += "/";
                    text += offer.Withheld[i].Name;
                }
            }

            text += " - " + (fee ? FormatMoney(fee) : std::string("free"));

            // The price is in the row so offers can be compared before clicking,
            // and in the confirmation box so a hire is a decision. The box money
            // stays 0: the core would deduct it after the recruit has already run,
            // and PawnMgr::Recruit charges it only once every check has passed.
            std::string const popup = fee ? "Hire " + name + " for " + FormatMoney(fee) + "?"
                                          : "Hire " + name + " for nothing?";

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, text, GOSSIP_SENDER_MAIN,
                             PAWN_ACTION_RECRUIT + (i - first), popup, 0, false);
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

// Handed the picker only where it belongs: an innkeeper with no AI of its own. A
// null return leaves the core's own AIName selection alone, so a creature with a
// real AI keeps it.
class PawnAllCreatureScript : public AllCreatureScript
{
public:
    PawnAllCreatureScript() : AllCreatureScript("PawnAllCreatureScript") { }

    [[nodiscard]] CreatureAI* GetCreatureAI(Creature* creature) const override
    {
        if (!creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) || !creature->GetCreatureTemplate()->AIName.empty())
            return nullptr;

        return new PawnInnkeeperAI(creature);
    }
};

void AddPawnAllCreatureScript()
{
    new PawnAllCreatureScript();
}

void PawnMenuForgetPlayer(ObjectGuid player)
{
    g_menus.erase(player.GetCounter());
}
