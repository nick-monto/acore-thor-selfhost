/*
 * This file is part of mod-creature-gear.
 *
 * Phase B, "drops what it wears": two hooks that inject the worn weapon/outfit items into a
 * creature's loot. What they add is distinct from the loot table's own roll - the worn set of the
 * spawn point, and never a second copy of an item the template already produced for that corpse.
 *   - CreatureGearLootScript (MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS, MiscScript.h:41,
 *     called from LootMgr.cpp:563) covers creatures that have a loot template; the injection
 *     happens while FillLoot() already runs, so the loot window sees it.
 *   - CreatureGearDeathScript (UNITHOOK_ON_UNIT_DEATH, UnitScript.h:45, called from
 *     Unit.cpp:13925) covers creatures with creature_template.lootid == 0, whose loot the core
 *     never fills. The two hooks cannot both fire for one creature: hook 1 only runs when a
 *     lootid existed.
 *
 * Selection of the candidate items (IsDropEligible / GetDropCandidates) lives in CreatureGearMgr.cpp.
 */

#include "creature_gear_common.h"

#include <algorithm>

#include "Creature.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"

namespace
{
    // Injects up to CreatureGear.Drop.MaxItemsPerCorpse distinct drop candidates into the loot
    // object of `creature`. Shared by both hooks. Returns true when at least one item was added.
    //
    // The chance is rolled once per corpse, and a candidate is erased from the candidate list as
    // soon as it is picked, so one corpse can never receive the same item twice.
    bool FillCorpseWithWornGear(Creature* creature, Loot& loot)
    {
        CreatureGear::Settings const& settings = CreatureGear::Manager::instance()->GetSettings();
        if (!settings.dropEnabled)
            return false;

        std::vector<uint32> candidates = CreatureGear::Manager::instance()->GetDropCandidates(creature);
        if (candidates.empty())
            return false;

        // The worn-gear drop is meant to be distinct from what the creature's own loot table rolled:
        // for a creature with a template this hook runs after FillLoot, so `loot` already holds the
        // natural drops, and a candidate the template produced is dropped from the list instead of
        // being added as a second copy of the same item. For a creature without a template the loot
        // object is empty here, so nothing is filtered out.
        auto rolledByTemplate = [&loot](uint32 itemEntry)
        {
            return std::any_of(loot.items.begin(), loot.items.end(),
                [itemEntry](LootItem const& item) { return item.itemid == itemEntry; });
        };

        size_t const worn = candidates.size();
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), rolledByTemplate), candidates.end());

        if (settings.debug && candidates.size() != worn)
            LOG_DEBUG("module", "mod-creature-gear: creature {} (level {}) wears {} item(s) its loot template already rolled, left out of the injection",
                      creature->GetEntry(), creature->GetLevel(), worn - candidates.size());

        if (candidates.empty())
            return false;

        if (!roll_chance_f(settings.dropChance))
            return false;

        bool injected = false;

        for (uint32 i = 0; i < settings.dropMaxItems && !candidates.empty(); ++i)
        {
            if (loot.items.size() >= MAX_NR_LOOT_ITEMS)
                break;

            uint32 index = urand(0, uint32(candidates.size()) - 1);
            uint32 itemEntry = candidates[index];
            candidates.erase(candidates.begin() + index);

            // reference must be 0 and chance is inert: the roll already happened above.
            loot.AddItem(LootStoreItem(itemEntry, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
            injected = true;

            if (settings.debug)
                LOG_DEBUG("module", "mod-creature-gear: creature {} (level {}) drops worn item {}",
                          creature->GetEntry(), creature->GetLevel(), itemEntry);
        }

        return injected;
    }
}

CreatureGearLootScript::CreatureGearLootScript()
    : MiscScript("mod-creature-gear-loot", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS })
{
}

void CreatureGearLootScript::OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/,
                                                        LootStore const& store, Player* lootOwner,
                                                        bool /*personal*/, bool /*noEmptyError*/,
                                                        uint16 /*lootMode*/)
{
    // The hook fires for every FillLoot against the creature's loot object, including the separate
    // objects filled for pickpocketing (Player.cpp:8243) and skinning (Player.cpp:8305); only the
    // corpse loot store may receive the worn gear.
    if (&store != &LootTemplates_Creature)
        return;

    if (!loot || !lootOwner)
        return;

    // The hook also fires for game objects and other loot sources, so the source has to resolve
    // to a creature for the worn gear to mean anything.
    Creature* creature = ObjectAccessor::GetCreature(*lootOwner, loot->sourceWorldObjectGUID);
    if (!creature)
        return;

    FillCorpseWithWornGear(creature, *loot);
}

CreatureGearDeathScript::CreatureGearDeathScript()
    : UnitScript("mod-creature-gear-death", true, { UNITHOOK_ON_UNIT_DEATH })
{
}

void CreatureGearDeathScript::OnUnitDeath(Unit* unit, Unit* /*killer*/)
{
    Creature* creature = unit->ToCreature();
    if (!creature)
        return;

    CreatureGear::Settings const& settings = CreatureGear::Manager::instance()->GetSettings();
    if (!settings.dropEnabled || !settings.dropIncludeNoLootTemplate)
        return;

    // A loot template means hook 1 already ran for this corpse; this is the whole de-duplication.
    if (creature->GetCreatureTemplate()->lootid != 0)
        return;

    // Unit.cpp:13819 decides both whether the corpse is lootable at all and whether the flag below
    // is ours to set; the same conditions are reproduced here.
    if (creature->IsPet() || creature->GetLootMode() == 0)
        return;

    Loot& loot = creature->loot;

    // Loot::clear() does not reset lootOwnerGUID (LootMgr.h:334-365), so from the second death of
    // this creature object onwards it still holds the previous killer; Loot::AddItem only counts a
    // fresh drop towards unlootedCount when that GUID resolves to an online player (LootMgr.cpp:501),
    // so a stale offline owner would leave the corpse permanently unlooted. A corpse with no
    // recipient is not lootable by design, hence the fresh (possibly empty) recipient GUID.
    loot.lootOwnerGUID = creature->GetLootRecipientGUID();

    FillCorpseWithWornGear(creature, loot);

    // Unit::Kill decided the lootable flag (Unit.cpp:13819-13830) before this hook runs; a corpse
    // with no stock loot was left unmarked, and Player::SendLoot refuses such a corpse.
    if (!loot.isLooted())
        creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
}
