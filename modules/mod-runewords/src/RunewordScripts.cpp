/*
 * mod-runewords - the script objects.
 *
 * The sweep is per player and staggered, not one global pass: OnPlayerUpdate is
 * already called for every player in the world (PlayerUpdates.cpp), so the only
 * state this keeps is a per-player accumulator. A player who moves runes sees
 * the change within Runewords.SweepIntervalMs, and a realm with a few hundred
 * bots costs one bag scan per player per interval rather than per tick.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "runewords_common.h"

#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSessionMgr.h"

// The command table's own namespace, as mod-survivor declares it: the return type
// of GetCommands() is resolved before the function body opens.
using namespace Acore::ChatCommands;

namespace
{
    // Every online player's granted auras, dropped. Used when the module is
    // switched off and when the word table is replaced with players online: both
    // are cases where a word the bags no longer say must not stay up.
    void ClearEveryPlayer()
    {
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
            if (Player* player = session->GetPlayer())
                Runewords::ClearApplied(player);
    }

    class RunewordWorldScript : public WorldScript
    {
    public:
        RunewordWorldScript() : WorldScript("RunewordWorldScript") { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            Runewords::Config cfg;
            cfg.Enable          = sConfigMgr->GetOption<bool>("Runewords.Enable", true);
            cfg.SweepIntervalMs = sConfigMgr->GetOption<uint32>("Runewords.SweepIntervalMs", 1000);
            cfg.ApplyWhenDead   = sConfigMgr->GetOption<bool>("Runewords.ApplyWhenDead", false);
            cfg.Announce        = sConfigMgr->GetOption<bool>("Runewords.Announce", true);

            if (cfg.SweepIntervalMs < 200)
                cfg.SweepIntervalMs = 200;   // a floor: this scans every bag the realm owns

            Runewords::SetConfig(cfg);

            // Switching the module off takes back what it granted, which is what
            // the conf says it does.
            if (!cfg.Enable)
                ClearEveryPlayer();
        }

        void OnStartup() override
        {
            Runewords::EnsureSchema();
            Runewords::LoadWords();
        }
    };

    class RunewordPlayerScript : public PlayerScript
    {
    public:
        RunewordPlayerScript() : PlayerScript("RunewordPlayerScript") { }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (Runewords::DueForScan(player, diff))
                Runewords::Reconcile(player);
        }

        void OnPlayerLogin(Player* player) override
        {
            if (!player || !Runewords::GetConfig().Enable)
                return;

            // Aura rows survive a logout, and a word that broke while its holder
            // was away left one the bookkeeping here never saw. Take back every
            // spell the table names, then let the scan grant what the bags say.
            Runewords::ClearWordAuras(player);
            Runewords::Reconcile(player);   // one pass at login, not one interval
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!player)
                return;

            Runewords::ForgetScanState(player->GetGUID());
            Runewords::ForgetPlayer(player->GetGUID());
        }

        // A rune sold to a vendor keeps pace with its roll. The hook is the only
        // point a module can reach before the core pays the template price; the
        // conditions PayRollBonus repeats are the ones the sell handler checks
        // after this call.
        bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
        {
            Runewords::PayRollBonus(player, item);
            return true;
        }

        // The add-on's own whisper to itself: 'RUNE\tsync' in, the rolls out. It
        // is swallowed so no other handler - nor the chat log - ever sees it.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                Player* receiver) override
        {
            return Runewords::HandleAddonChat(player, type, language, msg, receiver);
        }
    };

    class RunewordCommandScript : public CommandScript
    {
    public:
        RunewordCommandScript() : CommandScript("RunewordCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable runewordsTable =
            {
                { "status", HandleStatus, SEC_PLAYER,        Console::No },
                { "list",   HandleList,   SEC_GAMEMASTER,    Console::No },
                { "reload", HandleReload, SEC_GAMEMASTER,    Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "runewords", runewordsTable },
            };

            return commandTable;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            Player* player = handler->GetPlayer();
            if (!player)
                return false;

            handler->SendSysMessage(Runewords::DescribePlayer(player).c_str());
            return true;
        }

        static bool HandleList(ChatHandler* handler)
        {
            handler->SendSysMessage(Runewords::DescribeWords().c_str());
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            // The auras come off before the table is replaced, so a word the new
            // table does not name cannot leave its spells behind.
            ClearEveryPlayer();

            Runewords::LoadWords();
            handler->SendSysMessage(
                Acore::StringFormat("Runewords: {} word(s) reloaded from runeword_words.",
                                    Runewords::CountWords()).c_str());
            return true;
        }
    };
}

void AddRunewordScripts()
{
    new RunewordWorldScript();
    new RunewordPlayerScript();
    new RunewordCommandScript();
}
