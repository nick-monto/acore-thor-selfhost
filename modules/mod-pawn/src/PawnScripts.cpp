/*
 * mod-pawn: the hooks.
 *
 * These hooks do the work the playerbots pool cannot do for itself:
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
 *   * PlayerScript::OnPlayerCreatureKillCredit and OnPlayerCompleteQuest credit affinity
 *     to the owner's pawns. Kills are resolved back to the owner when a pawn landed
 *     them, so the party the player actually fights beside is the one that bonds with
 *     them. The credit hook is the only award site because it is the one call the core
 *     makes for every creature death with a responsible player.
 *   * PlayerScript::OnPlayerCreatureKill and OnPlayerJustDied let a pawn's voice speak
 *     at the two moments the module sees for itself, before anything else about the
 *     pawn has changed. The kill hook carries no credit of its own.
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

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        if (!killer)
            return;

        // Saying the kill line is this hook's only job. The award lives in the credit
        // hook, which is the one call the core makes for every creature death with a
        // responsible player - owner, pawn, pet or totem - so awarding there is what
        // makes a pet's kill count without counting a player's kill twice. Resolving
        // the pawn back to its owner is still done here because that is who the line
        // is said to. The voice speaks only for a pawn: a real player's own kills are
        // the player's to narrate, and SayLine is a no-op when the voice has no line,
        // when the kill chance misses, or while the cooldown holds.
        ObjectGuid const owner = sPawnMgr.IsPawn(killer->GetGUID())
            ? sPawnMgr.OwnerOf(killer->GetGUID())
            : killer->GetGUID();

        if (sPawnMgr.IsPawn(killer->GetGUID()))
            sPawnPersonality.SayLine(killer, owner, "kill");
    }

    void OnPlayerCreatureKillCredit(Player* player, Creature* /*killed*/) override
    {
        if (!player)
            return;

        // This is the only place affinity is credited for a kill: the core fires it once
        // per creature death with the responsible player, whether that is the owner, a
        // pawn, a pet or a totem. OnPlayerCreatureKill fires only when the killer is
        // itself a player, so awarding there would double-count every ordinary kill and
        // miss every kill a pet landed. Resolving a pawn back to its owner is what makes
        // a companion's blows bond with the character it follows.
        ObjectGuid const owner = sPawnMgr.IsPawn(player->GetGUID())
            ? sPawnMgr.OwnerOf(player->GetGUID())
            : player->GetGUID();
        sPawnMgr.AwardAffinity(owner, sPawnMgr.Config().AffinityKillPoints);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
    {
        if (!player)
            return;

        // No owner resolution here: a quest is only ever turned in by a real player, so the
        // hander is the owner by construction.
        sPawnMgr.AwardAffinity(player->GetGUID(), sPawnMgr.Config().AffinityQuestPoints);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!player || !sPawnMgr.IsPawn(player->GetGUID()))
            return;

        // The line is said here, before the pawn is released, because this hook is the last
        // moment the pawn is still a player in the world: a line said from a ghost would
        // arrive after the player has already read the death.
        sPawnPersonality.SayLine(player, sPawnMgr.OwnerOf(player->GetGUID()), "death");
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
