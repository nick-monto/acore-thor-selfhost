/*
 * mod-pawn: the hooks.
 *
 * Two hooks do the work the playerbots pool cannot do for itself:
 *
 *   * PlayerScript::OnLogin marks a pawn so its archetype is applied once it is
 *     actually in the world, and marks a real player so their party is restored
 *     from the roster. The drain runs in WorldScript::OnUpdate, on the world
 *     thread, after playerbots' own login pass - which is what lets a pawn's
 *     level stay inside the window instead of being snapped to the player's.
 *   * PlayerScript::OnPlayerCanLearnSpell refuses a blacklisted spell. The core
 *     checks it at the top of Player::learnSpell, so no learn path can bypass
 *     it: factory initialisation, trainer, level-up maintenance or a quest.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnArchetype.h"
#include "PawnMgr.h"

#include "Player.h"
#include "ScriptMgr.h"

void AddPawnObeliskScript();

class PawnPlayerScript : public PlayerScript
{
public:
    PawnPlayerScript() : PlayerScript("PawnPlayerScript") { }

    void OnLogin(Player* player) override
    {
        sPawnMgr.OnPlayerLogin(player);
    }

    [[nodiscard]] bool OnPlayerCanLearnSpell(Player* player, uint32 spellId) override
    {
        return !sPawnMgr.IsSpellForbidden(player, spellId);
    }
};

class PawnWorldScript : public WorldScript
{
public:
    PawnWorldScript() : WorldScript("PawnWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPawnMgr.LoadConfig();
        sPawnArchetype.Load();
    }

    void OnStartup() override
    {
        sPawnMgr.LoadRoster();
    }

    void OnUpdate(uint32 diff) override
    {
        sPawnMgr.Update(diff);
    }
};

void AddPawnScripts()
{
    new PawnPlayerScript();
    new PawnWorldScript();
    AddPawnObeliskScript();
}
