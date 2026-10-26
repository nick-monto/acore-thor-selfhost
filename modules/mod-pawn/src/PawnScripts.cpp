/*
 * mod-pawn: the hooks.
 *
 * Three hooks do the work the playerbots pool cannot do for itself:
 *
 *   * PlayerScript::OnLogin marks a pawn so its archetype is applied once it is
 *     actually in the world, and marks a real player so their party is restored
 *     from the roster. The drain runs in WorldScript::OnUpdate, on the world
 *     thread, after playerbots' own login pass - which is what lets a pawn's
 *     level stay inside the window instead of being snapped to the player's.
 *   * PlayerScript::OnPlayerCanLearnSpell refuses a blacklisted spell. The core
 *     checks it at the top of Player::learnSpell, so no learn path can bypass
 *     it: factory initialisation, trainer, level-up maintenance or a quest.
 *   * PlayerScript::OnPlayerLogout drops the picker's per-player cache entry.
 *
 * The picker itself lives in PawnInnkeeper.cpp: a CreatureScript, so the line is
 * on every innkeeper without a template row, a spawn or a ScriptName.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnArchetype.h"
#include "PawnMgr.h"

#include "Player.h"
#include "ScriptMgr.h"

void AddPawnInnkeeperScript();
void PawnMenuForgetPlayer(ObjectGuid player);

class PawnPlayerScript : public PlayerScript
{
public:
    PawnPlayerScript() : PlayerScript("PawnPlayerScript") { }

    void OnLogin(Player* player) override
    {
        sPawnMgr.OnPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        // The picker caches one small entry per player; drop it rather than keep
        // it for every character that ever talked to an innkeeper.
        PawnMenuForgetPlayer(player->GetGUID());
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
    AddPawnInnkeeperScript();
}
