/*
 * mod-mercenaries: the picker, on the innkeepers.
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
 * Pages carry at most Mercenary.OfferedPerPage offers plus the party rows and two
 * navigation rows, because the gossip packet carries at most 16 options
 * (GossipDef.cpp:199-201) and a 33rd aborts the server with an ASSERT rather
 * than clamping (GossipDef.cpp:43).
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "MercenaryMgr.h"

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
    // Mercenary.MaxMercenaries, which is why its range can be ten wide.
    constexpr uint32 MERCENARY_ACTION_OPEN    = 1000;
    constexpr uint32 MERCENARY_ACTION_RECRUIT = 1010;  // 1010..1099 offer on this page
    constexpr uint32 MERCENARY_ACTION_PARTY   = 1110;  // 1110..1119 mercenary page for party row n
    constexpr uint32 MERCENARY_ACTION_NEXT    = 1200;
    constexpr uint32 MERCENARY_ACTION_PREV    = 1201;
    constexpr uint32 MERCENARY_ACTION_SPAN    = 90;

    // A mercenary action is the one code that is not page-local: the mercenary page holds a
    // single mercenary, so its rows carry that mercenary's roster index and a verb rather
    // than a position on a page. Mercenary.MaxMercenaries is clamped to 4
    // (MercenaryMgr.cpp:162), which is ten verbs for each of four mercenaries - 1300..1339 -
    // and the base sits above the recruit span so no party row can be read as a
    // hire. Every verb below has a row on the mercenary page: 0 retrain, 1 restock,
    // 2 follow, 3 break off, 4 dismiss and 5 back. The two rows that remain are recall
    // orders; how the mercenary engages is its temperament's business.
    constexpr uint32 MERCENARY_ACTION_MERCENARY      = 1300;
    constexpr uint32 MERCENARY_ACTION_MERCENARY_SPAN = 40;

    constexpr uint32 MERCENARY_VERB_RETRAIN  = 0;
    constexpr uint32 MERCENARY_VERB_RESUPPLY = 1;
    constexpr uint32 MERCENARY_VERB_FOLLOW   = 2;
    constexpr uint32 MERCENARY_VERB_FLEE     = 3;
    constexpr uint32 MERCENARY_VERB_DISMISS  = 4;
    constexpr uint32 MERCENARY_VERB_BACK     = 5;

    // The options this AI answers. Everything else belongs to the innkeeper - its
    // bind, its quests, its vendor rows - and is left alone: the core dispatches
    // it after sGossipSelect returns (MiscHandler.cpp:199-200), so claiming an
    // option outside these ranges would swallow it instead.
    bool IsPickerAction(uint32 option)
    {
        return (option >= MERCENARY_ACTION_OPEN && option <= MERCENARY_ACTION_PREV)
            || (option >= MERCENARY_ACTION_PARTY && option < MERCENARY_ACTION_PARTY + 10)
            || (option >= MERCENARY_ACTION_MERCENARY && option < MERCENARY_ACTION_MERCENARY + MERCENARY_ACTION_MERCENARY_SPAN);
    }

    struct MercenaryMenuState
    {
        std::vector<MercenaryOffer>     Offers;   // the cycle's selection, as this visit shows it
        std::vector<MercenaryRosterRow> Party;
        uint8                      Page = 0;
        uint32                     First = 0;   // index of the first offer on the page
        // The mercenary page is a view of one party row, not a page of the offers: the
        // sixteen offer rows are already spoken for, so the orders and the
        // dismissal live here, and Page is kept untouched while it is open so Back
        // returns to the offers page the player came from.
        bool                       MercenaryPage = false;
        uint8                      MercenaryIndex = 0;
    };

    // Per-player, rebuilt whenever the picker is opened. A click on a menu from
    // before a restart finds no entry and is answered by reopening the picker
    // rather than acting on a guid that may no longer be offered.
    std::unordered_map<uint32, MercenaryMenuState> g_menus;

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

    char const* RoleName(MercenaryRole role)
    {
        switch (role)
        {
            case MERCENARY_ROLE_TANK:   return "tank";
            case MERCENARY_ROLE_HEALER: return "healer";
            case MERCENARY_ROLE_DPS:    return "dps";
            default:               return "mercenary";
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
class MercenaryInnkeeperMenu
{
public:
    // True once the menu is sent, which is how the hook tells the core not to
    // build one of its own (NPCHandler.cpp:188-193). The menu built here is the
    // core's own call with the core's own arguments, so "make this inn your home"
    // and any quest rows are the rows they would have been.
    static bool Hello(Player* player, Creature* creature)
    {
        if (!sMercenaryMgr.Config().Enabled || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;   // not ours: let the core send whatever this creature has

        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        // One line, on top of the menu just prepared. A menu that fills the
        // client's 16 visible rows gets no line: a 17th is not shown anyway, and
        // adding past GOSSIP_MAX_MENU_ITEMS aborts the server with an ASSERT rather
        // than clamping (GossipDef.cpp:43).
        if (player->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() < 15)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Recruit companions", GOSSIP_SENDER_MAIN, MERCENARY_ACTION_OPEN);

        player->SendPreparedGossip(creature);
        return true;
    }

    // True when the click was ours, which suppresses the core's own
    // gossip_menu_option dispatch (MiscHandler.cpp:201) for that row and no other:
    // a row outside our ranges returns false and the innkeeper keeps it.
    static bool Select(Player* player, Creature* creature, uint32 option)
    {
        if (!sMercenaryMgr.Config().Enabled || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;

        if (!IsPickerAction(option))
            return false;   // the innkeeper's own options: bind, quests, vendor - the core runs them

        auto it = g_menus.find(player->GetGUID().GetCounter());
        if (it == g_menus.end())
        {
            Open(player, creature);
            return true;
        }

        MercenaryMenuState& state = it->second;

        if (option == MERCENARY_ACTION_OPEN)
        {
            Open(player, creature);
            return true;
        }

        if (option == MERCENARY_ACTION_NEXT)
        {
            ShowMenu(player, creature, uint8(state.Page + 1));
            return true;
        }

        if (option == MERCENARY_ACTION_PREV)
        {
            ShowMenu(player, creature, uint8(state.Page ? state.Page - 1 : 0));
            return true;
        }

        if (option >= MERCENARY_ACTION_RECRUIT && option < MERCENARY_ACTION_RECRUIT + MERCENARY_ACTION_SPAN)
        {
            // The option carries the index within the page, not within the draw:
            // a draw of several hundred would otherwise run past the party range
            // and collide with the page actions.
            uint32 const index = state.First + (option - MERCENARY_ACTION_RECRUIT);
            if (index < state.Offers.size())
            {
                MercenaryOffer const offer = state.Offers[index];

                std::string message;
                if (sMercenaryMgr.Recruit(player, offer, message))
                    state.Offers.erase(state.Offers.begin() + index);

                Notify(player, message);
            }
            ShowMenu(player, creature, state.Page);
            return true;
        }

        if (option >= MERCENARY_ACTION_MERCENARY && option < MERCENARY_ACTION_MERCENARY + MERCENARY_ACTION_MERCENARY_SPAN)
        {
            HandleMercenaryAction(player, creature, state, option);
            return true;
        }

        // A party row opens that mercenary's page. Dismissal moved there with the
        // orders: the offers page has no row to spare, and a confirmation box on
        // the row next to the hire rows would put an irreversible click one
        // mis-tap from a purchase.
        uint32 const index = option - MERCENARY_ACTION_PARTY;
        if (index >= state.Party.size())
        {
            // The roster changed under a menu that was already sent: there is no
            // mercenary to page to, so the offers page is what is left.
            state.MercenaryPage = false;
            ShowMenu(player, creature, state.Page);
            return true;
        }

        state.MercenaryPage = true;
        state.MercenaryIndex = uint8(index);
        ShowMenu(player, creature, state.Page);
        return true;
    }

private:
    // The rows come from the selection dealt for this pool cycle, not from a draw
    // per menu: opening the picker again shows the same mercenaries, and only the page
    // is reset. A new crowd arrives when the pool is re-read or the server
    // restarts.
    static void Open(Player* player, Creature* creature)
    {
        MercenaryMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Offers = sMercenaryMgr.SelectionFor(player);
        state.Page = 0;
        state.First = 0;
        state.MercenaryPage = false;
        state.MercenaryIndex = 0;
        ShowMenu(player, creature, 0);
    }

    static void ShowMenu(Player* player, Creature* creature, uint8 page)
    {
        ClearGossipMenuFor(player);

        MercenaryMenuState& state = g_menus[player->GetGUID().GetCounter()];
        state.Party = sMercenaryMgr.ListParty(player->GetGUID());

        // The mercenary page is a view of one party row, not a page of the offers. It
        // is drawn before the offer arithmetic so a click on it neither reads nor
        // moves the offers page the player came from - Back lands where they left.
        if (state.MercenaryPage)
        {
            if (uint32(state.MercenaryIndex) < state.Party.size())
            {
                ShowMercenaryPage(player, creature, state);
                return;
            }

            // The mercenary left the party since its page was drawn. There is nothing
            // left to page to, so the offers page is the only honest place to be.
            state.MercenaryPage = false;
        }

        // The party rows and the two navigation rows share the 16-option page
        // with the offers, so the page size follows from the party size rather
        // than from the configured number alone.
        uint32 const partyRows = uint32(state.Party.size());
        uint32 const navigationRows = 2;
        uint32 const maxPage = 16 > partyRows + navigationRows ? 16 - partyRows - navigationRows : 1;
        uint32 const perPage = std::max<uint32>(1, std::min<uint32>(
            std::min<uint32>(sMercenaryMgr.Config().OfferedPerPage, maxPage), MERCENARY_ACTION_SPAN));

        uint32 const pages = state.Offers.empty() ? 1 : uint32((state.Offers.size() + perPage - 1) / perPage);
        state.Page = uint8(std::min<uint32>(page, pages - 1));

        for (uint32 i = 0; i < partyRows; ++i)
        {
            MercenaryRosterRow const& row = state.Party[i];
            std::string name;
            sCharacterCache->GetCharacterNameByGuid(row.Bot, name);

            // The row describes the mercenary and opens its page; the actions are on
            // that page because this one has no rows to spare.
            std::string text = name.empty() ? std::string("your mercenary") : name;
            text += " (" + std::string(RoleName(row.Role)) + ", level " +
                    std::to_string(uint32(row.TargetLevel)) + ")";

            // A mercenary is pinned to the level it was hired at, so the party row is
            // where a player sees one fall behind - and the offers below are the
            // inn's answer to it.
            if (row.TargetLevel + sMercenaryMgr.Config().LevelWindow < player->GetLevel())
                text += " - fallen behind";

            // Vanity comes from the roster, not the mercenary: the mercenary is usually
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
            // mercenary that a fight changes without the player doing anything: the
            // count is what the mercenary has learned, and the cap is the most any mercenary
            // could learn, so "bonded 2/2" reads as complete rather than as a
            // fraction of something the player was never told. A mercenary that has
            // learned nothing keeps a bare row, because the suffix is a reward the
            // player has earned and not a baseline to explain.
            uint32 const steps = sMercenaryMgr.AffinitySteps(row);
            if (steps > 0)
                text += " - bonded " + std::to_string(steps) + "/" + std::to_string(sMercenaryMgr.AffinityCap());

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, MERCENARY_ACTION_PARTY + i);
        }

        uint32 const first = uint32(state.Page) * perPage;
        uint32 const last = std::min<uint32>(first + perPage, uint32(state.Offers.size()));
        state.First = first;

        for (uint32 i = first; i < last; ++i)
        {
            MercenaryOffer const& offer = state.Offers[i];

            std::string const name = offer.Name.empty() ? std::string("An unnamed mercenary") : offer.Name;
            uint32 const fee = sMercenaryMgr.HireFee(player, offer);

            std::string text = name + " - " + std::string(ClassName(offer.Class)) + ", " +
                               DisplaySpec(offer.SpecName) + ", level " +
                               std::to_string(uint32(offer.Level)) + " (" + RoleName(offer.Role) + ")";

            // The gear score itself is invisible in the vanilla client, so the ratio
            // is what explains why two mercenaries of the same level cost different money:
            // the dearer one arrives in better gear, the cheaper one in worse.
            if (offer.GearRatio != 100)
                text += ", " + std::string(offer.GearRatio > 100 ? "+" : "-") +
                        std::to_string(offer.GearRatio > 100 ? uint32(offer.GearRatio) - 100
                                                             : 100 - uint32(offer.GearRatio)) + "% gear";

            // How this mercenary fights and how it talks: the two axes the level, the
            // spec and the gear do not cover, and the reason two mercenaries of the same
            // build can still feel like different companions.
            if (!offer.TemperamentLabel.empty())
                text += ", " + offer.TemperamentLabel;
            if (!offer.VoiceLabel.empty())
                text += ", " + offer.VoiceLabel;

            text += " - " + (fee ? FormatMoney(fee) : std::string("free"));

            // The price is in the row so offers can be compared before clicking,
            // and in the confirmation box so a hire is a decision. The box money
            // stays 0: the core would deduct it after the recruit has already run,
            // and MercenaryMgr::Recruit charges it only once every check has passed.
            std::string const popup = fee ? "Hire " + name + " for " + FormatMoney(fee) + "?"
                                          : "Hire " + name + " for nothing?";

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, text, GOSSIP_SENDER_MAIN,
                             MERCENARY_ACTION_RECRUIT + (i - first), popup, 0, false);
        }

        if (state.Offers.empty() && partyRows == 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No companion is free to travel right now",
                             GOSSIP_SENDER_MAIN, MERCENARY_ACTION_OPEN);

        if (pages > 1)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next page (" + std::to_string(uint32(state.Page) + 1) +
                                                       "/" + std::to_string(pages) + ")",
                             GOSSIP_SENDER_MAIN, MERCENARY_ACTION_NEXT);
            if (state.Page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", GOSSIP_SENDER_MAIN, MERCENARY_ACTION_PREV);
        }

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
    }

    // One mercenary, its retraining, its orders, its upkeep and its dismissal. Every
    // row names the mercenary because the gossip title does not: a page of "follow me"
    // rows would otherwise be indistinguishable from the same page for the other
    // companion.
    static void ShowMercenaryPage(Player* player, Creature* creature, MercenaryMenuState const& state)
    {
        MercenaryRosterRow const& row = state.Party[state.MercenaryIndex];

        std::string name;
        sCharacterCache->GetCharacterNameByGuid(row.Bot, name);
        if (name.empty())
            name = "your mercenary";

        uint32 const base = MERCENARY_ACTION_MERCENARY + uint32(state.MercenaryIndex) * 10;

        // Retraining changes the mercenary itself rather than telling it what to do, so
        // it is the first row and the orders read as what it is at the moment the
        // player sees them. The row is drawn even when the mercenary already stands at
        // the level the offer names: a row that appears and disappears under a
        // menu sent a moment earlier is worse than an honest refusal from
        // MercenaryMgr::Retrain, which is also where the fee is charged, so the box
        // money stays 0 and the click is confirmed like the restock below.
        MercenaryOffer const offer = sMercenaryMgr.OfferFor(player, row);
        uint32 const retrainFee = sMercenaryMgr.HireFee(player, offer);
        std::string const retrainFeeText = retrainFee ? FormatMoney(retrainFee) : std::string("free");
        std::string const retrainPopup =
            retrainFee
                ? "Retrain " + name + " to level " + std::to_string(uint32(offer.Level)) + " for " + FormatMoney(retrainFee) + "?"
                : "Retrain " + name + " to level " + std::to_string(uint32(offer.Level)) + " for nothing?";
        // The retrain row rebuilds the mercenary from its bond as much as from the
        // offer, so the bond is repeated here: the steps are what the mercenary has
        // learned and the cap is what it could learn, which is what makes the row
        // read as the rebuild it is about to perform. A mercenary with no affinity
        // leaves the label exactly as it was.
        std::string retrainLabel = name + ": retrain for level " + std::to_string(uint32(offer.Level)) +
                                   " - " + retrainFeeText;
        uint32 const retrainSteps = sMercenaryMgr.AffinitySteps(row);
        if (retrainSteps > 0)
            retrainLabel += ", bonded " + std::to_string(retrainSteps) + "/" +
                            std::to_string(sMercenaryMgr.AffinityCap());
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, retrainLabel,
                         GOSSIP_SENDER_MAIN, base + MERCENARY_VERB_RETRAIN, retrainPopup, 0, false);

        // The two orders are playerbots' own whisper words (ChatCommandHandlerStrategy.cpp)
        // and both of them recall the mercenary: how it engages is its temperament's
        // business, drawn from Mercenary.Temperament.<class>.<spec_no>, so there is no
        // "hold here" and no "attack my target" to fight the manner it was hired with.
        // Neither needs a confirmation box - an order can be given again - so only the
        // rows that spend something (the retrain above, the restock below and the
        // dismissal) ask before they act.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": follow me", GOSSIP_SENDER_MAIN,
                         base + MERCENARY_VERB_FOLLOW);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": break off", GOSSIP_SENDER_MAIN,
                         base + MERCENARY_VERB_FLEE);

        // Repair and restock cost real money, so the fee is in the row and the
        // click is confirmed. The box money stays 0 for the same reason the hire
        // box does: MercenaryMgr::Resupply is the only place that charges, once every
        // check has passed, and the core would deduct whatever was put here.
        uint32 const fee = sMercenaryMgr.ResupplyFee(player);
        std::string const feeText = fee ? FormatMoney(fee) : std::string("free");
        std::string const restockPopup = fee ? "Restock " + name + " for " + FormatMoney(fee) + "?"
                                            : "Restock " + name + " for nothing?";
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + ": restock and repair - " + feeText,
                         GOSSIP_SENDER_MAIN, base + MERCENARY_VERB_RESUPPLY, restockPopup, 0, false);

        // The box money stays 0 for the same reason the hire box does: a dismissal
        // charges nothing, and the core would deduct whatever was put here.
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Dismiss " + name, GOSSIP_SENDER_MAIN,
                         base + MERCENARY_VERB_DISMISS, "Dismiss " + name + "?", 0, false);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, base + MERCENARY_VERB_BACK);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
    }

    // One click on the mercenary page. The index and the verb both travel in the
    // option, so a menu that outlived the party it was drawn for is re-checked
    // here against the live roster rather than trusted.
    static void HandleMercenaryAction(Player* player, Creature* creature, MercenaryMenuState& state, uint32 option)
    {
        uint32 const offset = option - MERCENARY_ACTION_MERCENARY;
        uint32 const index = offset / 10;
        uint32 const verb = offset % 10;

        if (index >= state.Party.size())
        {
            // The mercenary left the party since its page was drawn. Nothing is left to
            // act on, so the offers page is the only honest place to go.
            state.MercenaryPage = false;
            ShowMenu(player, creature, state.Page);
            return;
        }

        ObjectGuid const bot = state.Party[index].Bot;
        std::string message;

        switch (verb)
        {
            case MERCENARY_VERB_RETRAIN:
                // The row is drawn even for a mercenary already at that level, so the
                // refusal is Retrain's to phrase; the page stays because the level
                // it names is re-derived when the menu is drawn again.
                sMercenaryMgr.Retrain(player, bot, message);
                Notify(player, message);
                break;
            case MERCENARY_VERB_FOLLOW:
            case MERCENARY_VERB_FLEE:
            {
                // The strings are the whisper words playerbots already
                // implements; MercenaryMgr::Order refuses anything else rather than
                // sending it, so the verb and the row cannot drift apart silently.
                sMercenaryMgr.Order(player, bot, verb == MERCENARY_VERB_FOLLOW ? "follow" : "flee", message);
                Notify(player, message);
                break;
            }
            case MERCENARY_VERB_RESUPPLY:
                sMercenaryMgr.Resupply(player, bot, message);
                Notify(player, message);
                // The page stays where it is: a restock is repeatable, and the
                // player wants to read the result before deciding on anything
                // else on this mercenary.
                break;
            case MERCENARY_VERB_DISMISS:
                sMercenaryMgr.Dismiss(player, bot, message);
                Notify(player, message);
                // The page was about a mercenary that is no longer in the party: the
                // offers page is where the player stands after it.
                state.MercenaryPage = false;
                break;
            case MERCENARY_VERB_BACK:
                state.MercenaryPage = false;
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
class MercenaryAllCreatureScript : public AllCreatureScript
{
public:
    MercenaryAllCreatureScript() : AllCreatureScript("MercenaryAllCreatureScript") { }

    [[nodiscard]] bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        return MercenaryInnkeeperMenu::Hello(player, creature);
    }

    [[nodiscard]] bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        return MercenaryInnkeeperMenu::Select(player, creature, action);
    }
};

void AddMercenaryAllCreatureScript()
{
    new MercenaryAllCreatureScript();
}

void MercenaryMenuForgetPlayer(ObjectGuid player)
{
    g_menus.erase(player.GetCounter());
}
