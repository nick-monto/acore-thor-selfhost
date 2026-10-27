/*
 * mod-pawn: the hooks.
 *
 * Three hooks do the work the playerbots pool cannot do for itself:
 *
 *   * PlayerScript::OnPlayerLogin marks a pawn so its archetype is applied once it is
 *     actually in the world, and marks a real player so their party is restored
 *     from the roster. The drain runs in WorldScript::OnUpdate, on the world
 *     thread, after playerbots' own login pass - which is what lets a pawn's
 *     level stay inside the window instead of being snapped to the player's.
 *   * PlayerScript::OnPlayerCanLearnSpell refuses a blacklisted spell. The core
 *     checks it at the top of Player::learnSpell, so no learn path can bypass
 *     it: factory initialisation, trainer, level-up maintenance or a quest.
 *   * PlayerScript::OnPlayerLogout drops the picker's per-player cache entry.
 *
 * The picker itself lives in PawnInnkeeper.cpp: a CreatureAI handed out by an
 * AllCreatureScript, so the line is on every innkeeper without a template row, a
 * spawn or a ScriptName. It cannot be a named CreatureScript: those are
 * database-bound and gossip is dispatched by the creature's own script id, and an
 * innkeeper's is the core's npc_innkeeper.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnArchetype.h"
#include "PawnMgr.h"
#include "PawnPersonality.h"

#include "Player.h"
#include "ScriptMgr.h"

void AddPawnAllCreatureScript();
void PawnMenuForgetPlayer(ObjectGuid player);

class PawnPlayerScript : public PlayerScript
{
public:
    PawnPlayerScript() : PlayerScript("PawnPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        sPawnMgr.OnPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        // The picker caches one small entry per player for the menu they are
        // browsing; drop it rather than keep it for every character that ever
        // talked to an innkeeper. The selection and the deal order are not
        // dropped: they belong to the pool cycle, and the refresh that ends the
        // cycle clears both, so a relogin finds the same crowd at the inn.
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

    void OnAfterConfigLoad(bool reload) override
    {
        sPawnMgr.LoadConfig();

        // The archetype table is built from the spell store, and the first config pass runs
        // before it exists: World::SetInitialWorldSettings calls OnAfterConfigLoad at
        // World.cpp:302 and sSpellMgr->LoadSpellInfoStore at World.cpp:407, so a build here
        // finds an empty store and every blacklist and variant name fails to resolve. The
        // first build is therefore in OnStartup, which worldserver calls once the world is
        // up (Main.cpp:398). A reload only happens on a running server, where the store is
        // already loaded.
        if (reload)
        {
            sPawnArchetype.Load();
            sPawnPersonality.Load();
        }
    }

    void OnStartup() override
    {
        sPawnArchetype.Load();
        sPawnPersonality.Load();
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
    AddPawnAllCreatureScript();
}
