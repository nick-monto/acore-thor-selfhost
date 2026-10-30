/*
 * mod-pawn: the picker, on the innkeepers.
 *
 * Every innkeeper in the world carries one extra gossip line. The picker hangs it
 * on AllCreatureScript::CanCreatureGossipHello and CanCreatureGossipSelect
 * (src/server/game/Scripting/ScriptDefines/AllCreatureScript.h:67,79), which
 * ScriptMgr::OnGossipHello and OnGossipSelect walk before any CreatureScript
 * (src/server/game/Scripting/ScriptDefines/CreatureScript.cpp:24-42,44-62). An
 * AllCreatureScript is code-only: it registers through
 * ScriptRegistry<AllCreatureScript> without ever being matched against
 * script_name, so no template row, no spawn and no ScriptName is involved: the
 * line exists wherever an innkeeper does, and nowhere else.
 *
 * The line was hung on a CreatureAI handed out by GetCreatureAI before, and that
 * was the bug: the hook has to leave a creature's own AI alone, so it refused
 * every innkeeper whose creature_template.AIName is set - and 65 of the 125
 * innkeeper-flagged templates are SmartAI, Razor Hill's Innkeeper Grosk (6928)
 * among them. The gossip hooks have no such guard: they run for every creature
 * whatever AI it carries, and the innkeeper's own AI still gets its sGossipHello
 * and sGossipSelect afterwards (NPCHandler.cpp:194, MiscHandler.cpp:199).
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
 * The menu is the core's, not ours: the core builds it with
 * Player::PrepareGossipMenu and the creature's own menu id - so "make this inn
 * your home" and any quest rows survive - and sends it with
 * Player::SendPreparedGossip (NPCHandler.cpp:188-193). A hello hook that returns
 * true suppresses exactly that, so the hook below makes the same call itself and
 * appends its one line before sending, rather than building a menu of its own.
 *
 * The select hook is handed the sender and the action, not the gossip list index
 * (MiscHandler.cpp:200), so the action codes below are read straight off it. An
 * option outside our range returns false, which leaves the innkeeper's own rows
 * to the core (MiscHandler.cpp:201); that is how bind and quests keep working.
 * The action codes below therefore start at 1000: a database gossip row's action is
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
    // per-player cache below. The offer index is page-local: a page holds at most
    // 16 rows, so it always fits its span however many candidates a draw has. The
    // party index is not local to anything - it is the roster index, bounded by
    // Pawn.MaxPawns, which is why its range can be ten wide.
    constexpr uint32 PAWN_ACTION_OPEN    = 1000;
    constexpr uint32 PAWN_ACTION_RECRUIT = 1010;  // 1010..1099 offer on this page
    constexpr uint32 PAWN_ACTION_PARTY   = 1110;  // 1110..1119 pawn page for party row n
    constexpr uint32 PAWN_ACTION_NEXT    = 1200;
    constexpr uint32 PAWN_ACTION_PREV    = 1201;
    constexpr uint32 PAWN_ACTION_SPAN    = 90;

    // A pawn action is the one code that is not page-local: the pawn page holds a
    // single pawn, so its rows carry that pawn's roster index and a verb rather
    // than a position on a page. Pawn.MaxPawns is clamped to 4
    // (PawnMgr.cpp:160), which is ten verbs for each of four pawns - 1300..1339 -
    // and the base sits above the recruit span so no party row can be read as a
    // hire. Every verb below has a row on the pawn page: 0 retrain, 1 restock,
    // 2 follow, 3 stay, 4 attack, 5 dismiss, 6 back and 7 break off.
    constexpr uint32 PAWN_ACTION_PAWN      = 1300;
    constexpr uint32 PAWN_ACTION_PAWN_SPAN = 40;

    constexpr uint32 PAWN_VERB_RETRAIN  = 0;
    constexpr uint32 PAWN_VERB_RESUPPLY = 1;
    constexpr uint32 PAWN_VERB_FOLLOW   = 2;
    constexpr uint32 PAWN_VERB_STAY     = 3;
    constexpr uint32 PAWN_VERB_ATTACK   = 4;
    constexpr uint32 PAWN_VERB_DISMISS  = 5;
    constexpr uint32 PAWN_VERB_BACK     = 6;
    constexpr uint32 PAWN_VERB_FLEE     = 7;

    // The options this AI answers. Everything else belongs to the innkeeper - its
    // bind, its quests, its vendor rows - and is left alone: the core dispatches
    // it after sGossipSelect returns (MiscHandler.cpp:199-200), so claiming an
    // option outside these ranges would swallow it instead.
    bool IsPickerAction(uint32 option)
    {
        return (option >= PAWN_ACTION_OPEN && option <= PAWN_ACTION_PREV)
            || (option >= PAWN_ACTION_PARTY && option < PAWN_ACTION_PARTY + 10)
            || (option >= PAWN_ACTION_PAWN && option < PAWN_ACTION_PAWN + PAWN_ACTION_PAWN_SPAN);
    }

    struct PawnMenuState
    {
        std::vector<PawnOffer>     Offers;   // the cycle's selection, as this visit shows it
        std::vector<PawnRosterRow> Party;
        uint8                      Page = 0;
        uint32                     First = 0;   // index of the first offer on the page
        // The pawn page is a view of one party row, not a page of the offers: the
        // sixteen offer rows are already spoken for, so the orders and the
        // dismissal live here, and Page is kept untouched while it is open so Back
        // returns to the offers page the player came from.
        bool                       PawnPage = false;
        uint8                      PawnIndex = 0;
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

// The picker's menu, hung off the two AllCreatureScript gossip hooks. It is not a
// CreatureAI any more: GetCreatureAI could only be handed to an innkeeper whose
// AIName is empty, which is 60 of the 125 innkeeper-flagged templates.
class PawnInnkeeperMenu
{
public:
    // True once the menu is sent, which is how the hook tells the core not to
    // build one of its own (NPCHandler.cpp:188-193). The menu built here is the
    // core's own call with the core's own arguments, so "make this inn your home"
    // and any quest rows are the rows they would have been.
    static bool Hello(Player* player, Creature* creature)
    {
        if (!sPawnMgr.Config().Enabled || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;   // not ours: let the core send whatever this creature has

        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        // One line, on top of the menu just prepared. A menu that fills the
        // client's 16 visible rows gets no line: a 17th is not shown anyway, and
        // adding past GOSSIP_MAX_MENU_ITEMS aborts the server with an ASSERT rather
        // than clamping (GossipDef.cpp:43).
        if (player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() < 15)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Recruit companions", GOSSIP_SENDER_MAIN, PAWN_ACTION_OPEN);

        player->SendPreparedGossip(creature);
        return true;
    }

    // True when the click was ours, which suppresses the core's own
    // gossip_menu_option dispatch (MiscHandler.cpp:201) for that row and no other:
    // a row outside our ranges returns false and the innkeeper keeps it.
    static bool Select(Player* player, Creature* creature, uint32 option)
    {
        if (!sPawnMgr.Config().Enabled || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;

        if (!IsPickerAction(option))
            return false;   // the innkeeper's own options: bind, quests, vendor - the core runs them

        auto it = g_menus.find(player->GetGUID().GetCounter());
        if (it == g_menus.end())
        {
            Open(player, creature);
            return true;
        }

        PawnMenuState& state = it->second;

        if (option == PAWN_ACTION_OPEN)
        {
            Open(player, creature);
            return true;
        }

        if (option == PAWN_ACTION_NEXT)
        {
            ShowMenu(player, creature, uint8(state.Page + 1));
            return true;
        }

        if (option == PAWN_ACTION_PREV)
        {
            ShowMenu(player, creature, uint8(state.Page ? state.Page - 1 : 0));
            return true;
        }

        if (option >= PAWN_ACTION_RECRUIT && option < PAWN_ACTION_RECRUIT + PAWN_ACTION_SPAN)
        {
            // The option carries the index within the page, not within the draw:
            // a draw of several hundred would otherwise run past the party range
            // and collide with the page actions.
            uint32 const index = state.First + (option - PAWN_ACTION_RECRUIT);
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

        if (option >= PAWN_ACTION_PAWN && option < PAWN_ACTION_PAWN + PAWN_ACTION_PAWN_SPAN)
        {
            HandlePawnAction(player, creature, state, option);
            return true;
        }

        // A party row opens that pawn's page. Dismissal moved there with the
        // orders: the offers page has no row to spare, and a confirmation box on
        // the row next to the hire rows would put an irreversible click one
        // mis-tap from a purchase.
        uint32 const index = option - PAWN_ACTION_PARTY;
        if (index >= state.Party.size())
        {
            // The roster changed under a menu that was already sent: there is no
            // pawn to page to, so the offers page is what is left.
            state.PawnPage = false;
            ShowMenu(player, creature, state.Page);
            return true;
        }

        state.PawnPage = true;
        state.PawnIndex = uint8(index);
        ShowMenu(player, creature, state.Page);
        return true;
    }

private:
    // The rows come from the selection dealt for this pool cycle, not from a draw
    // per menu: opening the picker again shows the same pawns, and only the page
    // is reset. A new crowd arrives when the pool is re-read or the server
    // restarts.
    static void Open(Player* player, Creature* creature)
    {
        PawnMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Offers = sPawnMgr.SelectionFor(player);
        state.Page = 0;
        state.First = 0;
        state.PawnPage = false;
        state.PawnIndex = 0;
        ShowMenu(player, creature, 0);
    }

    static void ShowMenu(Player* player, Creature* creature, uint8 page)
    {
        ClearGossipMenuFor(player);

        PawnMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Party = sPawnMgr.ListParty(player->GetGUID());

        // The pawn page is a view of one party row, not a page of the offers. It
        // is drawn before the offer arithmetic so a click on it neither reads nor
        // moves the offers page the player came from - Back lands where they left.
        if (state.PawnPage)
        {
            if (uint32(state.PawnIndex) < state.Party.size())
            {
                ShowPawnPage(player, creature, state);
                return;
            }

            // The pawn left the party since its page was drawn. There is nothing
            // left to page to, so the offers page is the only honest place to be.
            state.PawnPage = false;
        }

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

            // The row describes the pawn and opens its page; the actions are on
            // that page because this one has no rows to spare.
            std::string text = name.empty() ? std::string("your pawn") : name;
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

            // The bond belongs on the row because it is the one thing about the
            // pawn that a fight changes without the player doing anything: the
            // count is what the pawn has learned, and the cap is the most any pawn
            // could learn, so "bonded 2/2" reads as complete rather than as a
            // fraction of something the player was never told. A pawn that has
            // learned nothing keeps a bare row, because the suffix is a reward the
            // player has earned and not a baseline to explain.
            uint32 const steps = sPawnMgr.AffinitySteps(row);
            if (steps > 0)
                text += " - bonded " + std::to_string(steps) + "/" + std::to_string(sPawnMgr.AffinityCap());

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, PAWN_ACTION_PARTY + i);
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

    // One pawn, its retraining, its orders, its upkeep and its dismissal. Every
    // row names the pawn because the gossip title does not: a page of "follow me"
    // rows would otherwise be indistinguishable from the same page for the other
    // companion.
    static void ShowPawnPage(Player* player, Creature* creature, PawnMenuState const& state)
    {
        PawnRosterRow const& row = state.Party[state.PawnIndex];

        std::string name;
        sCharacterCache->GetCharacterNameByGuid(row.Bot, name);
        if (name.empty())
            name = "your pawn";

        uint32 const base = PAWN_ACTION_PAWN + uint32(state.PawnIndex) * 10;

        // Retraining changes the pawn itself rather than telling it what to do, so
        // it is the first row and the orders read as what it is at the moment the
        // player sees them. The row is drawn even when the pawn already stands at
        // the level the offer names: a row that appears and disappears under a
        // menu sent a moment earlier is worse than an honest refusal from
        // PawnMgr::Retrain, which is also where the fee is charged, so the box
        // money stays 0 and the click is confirmed like the restock below.
        PawnOffer const offer = sPawnMgr.OfferFor(player, row);
        uint32 const retrainFee = sPawnMgr.HireFee(player, offer);
        std::string const retrainFeeText = retrainFee ? FormatMoney(retrainFee) : std::string("free");
        std::string const retrainPopup =
            retrainFee
                ? "Retrain " + name + " to level " + std::to_string(uint32(offer.Level)) + " for " + FormatMoney(retrainFee) + "?"
                : "Retrain " + name + " to level " + std::to_string(uint32(offer.Level)) + " for nothing?";
        // The retrain row rebuilds the pawn from its bond as much as from the
        // offer, so the bond is repeated here: the steps are what the pawn has
        // learned and the cap is what it could learn, which is what makes the row
        // read as the rebuild it is about to perform. A pawn with no affinity
        // leaves the label exactly as it was.
        std::string retrainLabel = name + ": retrain for level " + std::to_string(uint32(offer.Level)) +
                                   " - " + retrainFeeText;
        uint32 const retrainSteps = sPawnMgr.AffinitySteps(row);
        if (retrainSteps > 0)
            retrainLabel += ", bonded " + std::to_string(retrainSteps) + "/" +
                            std::to_string(sPawnMgr.AffinityCap());
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, retrainLabel,
                         GOSSIP_SENDER_MAIN, base + PAWN_VERB_RETRAIN, retrainPopup, 0, false);

        // The four orders are playerbots' own whisper words
        // (ChatCommandHandlerStrategy.cpp); the picker only spares the player the
        // syntax. None of them needs a confirmation box - an order can be given
        // again - so only the rows that spend something (the retrain above, the
        // restock below and the dismissal) ask before they act.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": follow me", GOSSIP_SENDER_MAIN,
                         base + PAWN_VERB_FOLLOW);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": hold here", GOSSIP_SENDER_MAIN,
                         base + PAWN_VERB_STAY);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": attack my target", GOSSIP_SENDER_MAIN,
                         base + PAWN_VERB_ATTACK);
        // Breaking off sits next to the attack row because it is the same order
        // pointed the other way: the player sends the pawn in, and this is how it
        // is called back out without a dismiss in between.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": break off", GOSSIP_SENDER_MAIN,
                         base + PAWN_VERB_FLEE);

        // Repair and restock cost real money, so the fee is in the row and the
        // click is confirmed. The box money stays 0 for the same reason the hire
        // box does: PawnMgr::Resupply is the only place that charges, once every
        // check has passed, and the core would deduct whatever was put here.
        uint32 const fee = sPawnMgr.ResupplyFee(player);
        std::string const feeText = fee ? FormatMoney(fee) : std::string("free");
        std::string const restockPopup = fee ? "Restock " + name + " for " + FormatMoney(fee) + "?"
                                            : "Restock " + name + " for nothing?";
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": restock and repair - " + feeText,
                         GOSSIP_SENDER_MAIN, base + PAWN_VERB_RESUPPLY, restockPopup, 0, false);

        // The box money stays 0 for the same reason the hire box does: a dismissal
        // charges nothing, and the core would deduct whatever was put here.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Dismiss " + name, GOSSIP_SENDER_MAIN,
                         base + PAWN_VERB_DISMISS, "Dismiss " + name + "?", 0, false);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, base + PAWN_VERB_BACK);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
    }

    // One click on the pawn page. The index and the verb both travel in the
    // option, so a menu that outlived the party it was drawn for is re-checked
    // here against the live roster rather than trusted.
    static void HandlePawnAction(Player* player, Creature* creature, PawnMenuState& state, uint32 option)
    {
        uint32 const offset = option - PAWN_ACTION_PAWN;
        uint32 const index = offset / 10;
        uint32 const verb = offset % 10;

        if (index >= state.Party.size())
        {
            // The pawn left the party since its page was drawn. Nothing is left to
            // act on, so the offers page is the only honest place to go.
            state.PawnPage = false;
            ShowMenu(player, creature, state.Page);
            return;
        }

        ObjectGuid const bot = state.Party[index].Bot;
        std::string message;

        switch (verb)
        {
            case PAWN_VERB_RETRAIN:
                // The row is drawn even for a pawn already at that level, so the
                // refusal is Retrain's to phrase; the page stays because the level
                // it names is re-derived when the menu is drawn again.
                sPawnMgr.Retrain(player, bot, message);
                Notify(player, message);
                break;
            case PAWN_VERB_FOLLOW:
            case PAWN_VERB_STAY:
            case PAWN_VERB_ATTACK:
            case PAWN_VERB_FLEE:
            {
                // The strings are the whisper words playerbots already
                // implements; PawnMgr::Order refuses anything else rather than
                // sending it, so the verb and the row cannot drift apart silently.
                char const* const order = verb == PAWN_VERB_FOLLOW ? "follow"
                                        : verb == PAWN_VERB_STAY   ? "stay"
                                        : verb == PAWN_VERB_ATTACK ? "attack"
                                                                   : "flee";
                sPawnMgr.Order(player, bot, order, message);
                Notify(player, message);
                break;
            }
            case PAWN_VERB_RESUPPLY:
                sPawnMgr.Resupply(player, bot, message);
                Notify(player, message);
                // The page stays where it is: a restock is repeatable, and the
                // player wants to read the result before deciding on anything
                // else on this pawn.
                break;
            case PAWN_VERB_DISMISS:
                sPawnMgr.Dismiss(player, bot, message);
                Notify(player, message);
                // The page was about a pawn that is no longer in the party: the
                // offers page is where the player stands after it.
                state.PawnPage = false;
                break;
            case PAWN_VERB_BACK:
                state.PawnPage = false;
                break;
            default:
                // A menu drawn before a verb was added can still carry a number
                // this build does not answer; ignoring it leaves the page where
                // it is rather than guessing what was meant.
                break;
        }

        ShowMenu(player, creature, state.Page);
    }
};

// The two gossip hooks, on every creature. Nothing is matched by name, by
// script id or by AI: the guard inside each hook is the innkeeper flag, and an
// innkeeper that carries its own AI keeps it and still receives its own
// sGossipHello and sGossipSelect (NPCHandler.cpp:194, MiscHandler.cpp:199).
class PawnAllCreatureScript : public AllCreatureScript
{
public:
    PawnAllCreatureScript() : AllCreatureScript("PawnAllCreatureScript") { }

    [[nodiscard]] bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        return PawnInnkeeperMenu::Hello(player, creature);
    }

    [[nodiscard]] bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        return PawnInnkeeperMenu::Select(player, creature, action);
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
