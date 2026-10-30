/*
 * mod-mercenaries: the hooks.
 *
 * These hooks do the work the playerbots pool cannot do for itself:
 *
 *   * PlayerScript::OnPlayerLogin marks a mercenary so its archetype is applied once it is
 *     actually in the world, and marks a real player so their party is restored
 *     from the roster. The drain runs in WorldScript::OnUpdate, on the world
 *     thread, after playerbots' own login pass - which is what lets a mercenary's
 *     level stay inside the window instead of being snapped to the player's.
 *   * PlayerScript::OnPlayerLogout drops the picker's per-player cache entry.
 *   * PlayerScript::OnPlayerCreatureKillCredit and OnPlayerCompleteQuest credit affinity
 *     to the owner's mercenaries. Kills are resolved back to the owner when a mercenary landed
 *     them, so the party the player actually fights beside is the one that bonds with
 *     them. The credit hook is the only award site because it is the one call the core
 *     makes for every creature death with a responsible player.
 *   * PlayerScript::OnPlayerCreatureKill and OnPlayerJustDied let a mercenary's voice speak
 *     at the two moments the module sees for itself, before anything else about the
 *     mercenary has changed. The kill hook carries no credit of its own.
 *
 * The picker itself lives in MercenaryInnkeeper.cpp: two AllCreatureScript gossip hooks,
 * so the line is on every innkeeper without a template row, a spawn or a ScriptName, and
 * an innkeeper that carries an AI of its own keeps it. It cannot be a named
 * CreatureScript: those are database-bound and gossip is dispatched by the creature's own
 * script id, and an innkeeper's is the core's npc_innkeeper.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "MercenaryArchetype.h"
#include "MercenaryMgr.h"
#include "MercenaryPersonality.h"

#include "Player.h"
#include "ScriptMgr.h"

void AddMercenaryAllCreatureScript();
void MercenaryMenuForgetPlayer(ObjectGuid player);

class MercenaryPlayerScript : public PlayerScript
{
public:
    MercenaryPlayerScript() : PlayerScript("MercenaryPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        sMercenaryMgr.OnPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        // The picker caches one small entry per player for the menu they are
        // browsing; drop it rather than keep it for every character that ever
        // talked to an innkeeper. The selection and the deal order are not
        // dropped: they belong to the pool cycle, and the refresh that ends the
        // cycle clears both, so a relogin finds the same crowd at the inn.
        MercenaryMenuForgetPlayer(player->GetGUID());
    }

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        if (!killer)
            return;

        // Saying the kill line is this hook's only job. The award lives in the credit
        // hook, which is the one call the core makes for every creature death with a
        // responsible player - owner, mercenary, pet or totem - so awarding there is what
        // makes a pet's kill count without counting a player's kill twice. Resolving
        // the mercenary back to its owner is still done here because that is who the line
        // is said to. The voice speaks only for a mercenary: a real player's own kills are
        // the player's to narrate, and SayLine is a no-op when the voice has no line,
        // when the kill chance misses, or while the cooldown holds.
        ObjectGuid const owner = sMercenaryMgr.IsMercenary(killer->GetGUID())
            ? sMercenaryMgr.OwnerOf(killer->GetGUID())
            : killer->GetGUID();

        if (sMercenaryMgr.IsMercenary(killer->GetGUID()))
            sMercenaryPersonality.SayLine(killer, owner, "kill");
    }

    void OnPlayerCreatureKillCredit(Player* player, Creature* /*killed*/) override
    {
        if (!player)
            return;

        // This is the only place affinity is credited for a kill: the core fires it once
        // per creature death with the responsible player, whether that is the owner, a
        // mercenary, a pet or a totem. OnPlayerCreatureKill fires only when the killer is
        // itself a player, so awarding there would double-count every ordinary kill and
        // miss every kill a pet landed. Resolving a mercenary back to its owner is what makes
        // a companion's blows bond with the character it follows.
        ObjectGuid const owner = sMercenaryMgr.IsMercenary(player->GetGUID())
            ? sMercenaryMgr.OwnerOf(player->GetGUID())
            : player->GetGUID();
        sMercenaryMgr.AwardAffinity(owner, sMercenaryMgr.Config().AffinityKillPoints);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
    {
        if (!player)
            return;

        // No owner resolution here: a quest is only ever turned in by a real player, so the
        // hander is the owner by construction.
        sMercenaryMgr.AwardAffinity(player->GetGUID(), sMercenaryMgr.Config().AffinityQuestPoints);
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!player || !sMercenaryMgr.IsMercenary(player->GetGUID()))
            return;

        // The line is said here, before the mercenary is released, because this hook is the last
        // moment the mercenary is still a player in the world: a line said from a ghost would
        // arrive after the player has already read the death.
        sMercenaryPersonality.SayLine(player, sMercenaryMgr.OwnerOf(player->GetGUID()), "death");
    }
};

class MercenaryWorldScript : public WorldScript
{
public:
    MercenaryWorldScript() : WorldScript("MercenaryWorldScript") { }

    void OnAfterConfigLoad(bool reload) override
    {
        sMercenaryMgr.LoadConfig();

        // The archetype table is built from the spell store, and the first config pass runs
        // before it exists: World::SetInitialWorldSettings calls OnAfterConfigLoad at
        // World.cpp:302 and sSpellMgr->LoadSpellInfoStore at World.cpp:407, so a build here
        // finds an empty store and every blacklist and variant name fails to resolve. The
        // first build is therefore in OnStartup, which worldserver calls once the world is
        // up (Main.cpp:398). A reload only happens on a running server, where the store is
        // already loaded.
        if (reload)
        {
            sMercenaryArchetype.Load();
            sMercenaryPersonality.Load();
        }
    }

    void OnStartup() override
    {
        sMercenaryArchetype.Load();
        sMercenaryPersonality.Load();
        sMercenaryMgr.LoadRoster();
    }

    void OnUpdate(uint32 diff) override
    {
        sMercenaryMgr.Update(diff);
    }
};

void AddMercenaryScripts()
{
    new MercenaryPlayerScript();
    new MercenaryWorldScript();
    AddMercenaryAllCreatureScript();
}
