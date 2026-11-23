/*
 * mod-creature-affixes - initial implementation.
 *
 * This file deliberately contains only the three proof modifiers plus the
 * hidden size layer. The remaining modifiers from the design are not listed
 * here yet; adding a row to Modifiers[] and implementing its Effect is the
 * extension point.
 */

#include "creature_affixes.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "LootMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "Unit.h"

using namespace Acore::ChatCommands;

namespace
{
    uint32 Weight(uint8 influence)
    {
        switch (influence)
        {
            case 1: return 40;
            case 2: return 25;
            case 3: return 12;
            case 4: return 5;
            case 5: return 2;
            default: return 1;
        }
    }

    bool IsGilded(CreatureAffixes::State const& state)
    {
        CreatureAffixes::Modifier const* modifier = CreatureAffixes::Manager::PrefixAt(state.prefix);
        return modifier && modifier->effect == CreatureAffixes::Effect::Gilded;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    }
}

namespace CreatureAffixes
{
    Manager& Manager::instance()
    {
        static Manager manager;
        return manager;
    }

    void Manager::LoadConfig()
    {
        _settings.enabled = sConfigMgr->GetOption<bool>("CreatureAffixes.Enable", true);
        _settings.debug = sConfigMgr->GetOption<bool>("CreatureAffixes.Debug", false);
        _settings.prefixChance = std::clamp(sConfigMgr->GetOption<float>("CreatureAffixes.PrefixChance", 10.0f), 0.0f, 100.0f);
        _settings.affixChance = std::clamp(sConfigMgr->GetOption<float>("CreatureAffixes.AffixChance", 10.0f), 0.0f, 100.0f);
        _settings.sizeChance = std::clamp(sConfigMgr->GetOption<float>("CreatureAffixes.SizeChance", 10.0f), 0.0f, 100.0f);
        _settings.maxCombinedInfluence = sConfigMgr->GetOption<uint32>("CreatureAffixes.MaxCombinedInfluence", 6);
        _settings.sizeOutdoorOnly = sConfigMgr->GetOption<bool>("CreatureAffixes.Size.OutdoorOnly", true);
        _settings.tinyMin = sConfigMgr->GetOption<float>("CreatureAffixes.Size.TinyMin", 0.70f);
        _settings.tinyMax = sConfigMgr->GetOption<float>("CreatureAffixes.Size.TinyMax", 0.85f);
        _settings.giantMin = sConfigMgr->GetOption<float>("CreatureAffixes.Size.GiantMin", 1.15f);
        _settings.giantMax = sConfigMgr->GetOption<float>("CreatureAffixes.Size.GiantMax", 1.30f);

        if (_settings.tinyMin > _settings.tinyMax)
            std::swap(_settings.tinyMin, _settings.tinyMax);
        if (_settings.giantMin > _settings.giantMax)
            std::swap(_settings.giantMin, _settings.giantMax);
        _settings.tinyMin = std::clamp(_settings.tinyMin, 0.05f, 1.0f);
        _settings.tinyMax = std::clamp(_settings.tinyMax, 0.05f, 1.0f);
        _settings.giantMin = std::clamp(_settings.giantMin, 1.0f, 2.0f);
        _settings.giantMax = std::clamp(_settings.giantMax, 1.0f, 2.0f);

        LOG_INFO("module", "mod-creature-affixes: enabled={} prefix={}% affix={}% size={}% rows={}",
                 _settings.enabled, _settings.prefixChance, _settings.affixChance, _settings.sizeChance,
                 sizeof(Modifiers) / sizeof(Modifiers[0]));
    }

    Modifier const* Manager::Find(char const* key, Kind kind)
    {
        if (!key)
            return nullptr;
        for (Modifier const& modifier : Modifiers)
            if (modifier.kind == kind && std::string(modifier.key) == key)
                return &modifier;
        return nullptr;
    }

    Modifier const* Manager::PrefixAt(int8 index)
    {
        uint32 count = sizeof(Modifiers) / sizeof(Modifiers[0]);
        if (index < 0 || static_cast<uint32>(index) >= count || Modifiers[index].kind != Kind::Prefix)
            return nullptr;
        return &Modifiers[index];
    }

    Modifier const* Manager::AffixAt(int8 index)
    {
        uint32 count = sizeof(Modifiers) / sizeof(Modifiers[0]);
        if (index < 0 || static_cast<uint32>(index) >= count || Modifiers[index].kind != Kind::Affix)
            return nullptr;
        return &Modifiers[index];
    }

    bool Manager::Eligible(Creature const* creature) const
    {
        if (!creature || !creature->GetSpawnId())
            return false;

        CreatureTemplate const* info = creature->GetCreatureTemplate();
        if (!info)
            return false;

        if (creature->IsCritter() || creature->IsCivilian() || creature->IsQuestGiver() ||
            creature->IsVendor() || creature->IsTrainer() || creature->IsTotem() ||
            creature->IsVehicle() || creature->IsPet() || creature->IsDungeonBoss())
            return false;

        if (info->rank == CREATURE_ELITE_WORLDBOSS)
            return false;

        return true;
    }

    bool Manager::Outdoor(Creature const* creature) const
    {
        Map const* map = creature ? creature->GetMap() : nullptr;
        return map && !map->IsDungeon() && !map->IsBattlegroundOrArena();
    }

    bool Manager::RollPrefix(State& state) const
    {
        if (frand(0.0f, 100.0f) >= _settings.prefixChance)
            return false;

        uint32 total = 0;
        for (Modifier const& modifier : Modifiers)
            if (modifier.kind == Kind::Prefix)
                total += Weight(modifier.influence);
        if (!total)
            return false;

        uint32 roll = urand(1, total);
        uint32 seen = 0;
        for (Modifier const& modifier : Modifiers)
        {
            if (modifier.kind != Kind::Prefix)
                continue;
            seen += Weight(modifier.influence);
            if (roll <= seen)
            {
                state.prefix = static_cast<int8>(&modifier - Modifiers);
                return true;
            }
        }
        return false;
    }

    bool Manager::RollAffix(State& state) const
    {
        if (frand(0.0f, 100.0f) >= _settings.affixChance)
            return false;

        uint32 total = 0;
        for (Modifier const& modifier : Modifiers)
            if (modifier.kind == Kind::Affix)
                total += Weight(modifier.influence);
        if (!total)
            return false;

        uint32 roll = urand(1, total);
        uint32 seen = 0;
        for (Modifier const& modifier : Modifiers)
        {
            if (modifier.kind != Kind::Affix)
                continue;
            seen += Weight(modifier.influence);
            if (roll <= seen)
            {
                state.affix = static_cast<int8>(&modifier - Modifiers);
                return true;
            }
        }
        return false;
    }

    void Manager::RollSize(State& state) const
    {
        if (frand(0.0f, 100.0f) >= _settings.sizeChance)
            return;

        if (urand(0, 1) == 0)
        {
            state.size = Size::Tiny;
            state.sizeFactor = frand(_settings.tinyMin, _settings.tinyMax);
        }
        else
        {
            state.size = Size::Giant;
            state.sizeFactor = frand(_settings.giantMin, _settings.giantMax);
        }
    }

    State Manager::GetState(ObjectGuid guid) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto found = _states.find(guid.GetRawValue());
        return found == _states.end() ? State{} : found->second;
    }

    void Manager::StoreState(ObjectGuid guid, State const& state)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _states[guid.GetRawValue()] = state;
    }

    void Manager::EraseState(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _states.erase(guid.GetRawValue());
    }

    void Manager::QueueUpdate(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pendingUpdates.insert(guid.GetRawValue()).second)
            _pendingCount.fetch_add(1, std::memory_order_relaxed);
    }

    void Manager::OnCreatureSelectLevel(Creature* creature)
    {
        if (!creature)
            return;

        State old = GetState(creature->GetGUID());
        if (!_settings.enabled || !Eligible(creature))
        {
            if (old.size != Size::None)
                creature->SetObjectScale(creature->GetNativeObjectScale());
            RemoveMarkers(creature, old);
            EraseState(creature->GetGUID());
            return;
        }

        // A respawn can call SelectLevel twice (UpdateEntry followed by Respawn's own
        // SelectLevel). OnUnitDeath clears this flag once, so only the first call
        // rolls the new spawn; an entry switch on a living creature never rerolls.
        // A second call still has to settle stats after UpdateAllStats, so queue the
        // existing state for the next update instead of returning silently.
        if (old.selectLevelApplied)
        {
            old.statsPending = true;
            old.sizePending = old.size != Size::None;
            StoreState(creature->GetGUID(), old);
            QueueUpdate(creature->GetGUID());
            return;
        }

        State state;
        state.baseMaxHealth = creature->GetMaxHealth();
        state.selectLevelApplied = true;

        bool hasPrefix = RollPrefix(state);
        bool hasAffix = RollAffix(state);
        if (_settings.maxCombinedInfluence && hasPrefix && hasAffix)
        {
            Modifier const* prefix = PrefixAt(state.prefix);
            Modifier const* affix = AffixAt(state.affix);
            if (prefix && affix && uint32(prefix->influence) + affix->influence > _settings.maxCombinedInfluence)
            {
                state.affix = -1;
                hasAffix = false;
            }
        }
        if (!_settings.sizeOutdoorOnly || Outdoor(creature))
            RollSize(state);

        // UpdateAllStats and the respawn display reset can overwrite both stats
        // and scale after this hook; keep them pending for OnCreatureAddWorld /
        // the next creature update to settle as the final values.
        state.statsPending = true;
        state.sizePending = state.size != Size::None;
        QueueUpdate(creature->GetGUID());

        if (old.size != Size::None && state.size == Size::None)
            creature->SetObjectScale(creature->GetNativeObjectScale());

        StoreState(creature->GetGUID(), state);
        RemoveMarkers(creature, old);
        Apply(creature, state);
        StoreState(creature->GetGUID(), state);

        if (_settings.debug)
            LOG_DEBUG("module", "mod-creature-affixes: spawn {} entry {} prefix={} affix={} size={:.2f}",
                      creature->GetGUID().ToString(), creature->GetEntry(),
                      PrefixAt(state.prefix) ? PrefixAt(state.prefix)->name : "none",
                      AffixAt(state.affix) ? AffixAt(state.affix)->name : "none",
                      state.sizeFactor);
    }

    void Manager::OnCreatureAddWorld(Creature* creature)
    {
        if (!creature || !_settings.enabled)
            return;
        State state = GetState(creature->GetGUID());
        if (!PrefixAt(state.prefix) && !AffixAt(state.affix) && state.size == Size::None)
            return;

        // UpdateEntry can run UpdateAllStats after SelectLevel, which recomputes
        // health from the unmodified base; reapply here as the final word for a
        // freshly loaded creature.
        ApplyStats(creature, state);
        state.statsPending = false;
        if (state.size != Size::None)
        {
            ApplySize(creature, state);
            state.sizePending = false;
        }
        if (creature->IsAlive())
            ApplyMarkers(creature, state);
        else
            MarkPending(creature->GetGUID(), state);
        StoreState(creature->GetGUID(), state);
    }

    void Manager::OnCreatureRemoveWorld(Creature* creature)
    {
        if (!creature)
            return;
        std::lock_guard<std::mutex> lock(_mutex);
        _states.erase(creature->GetGUID().GetRawValue());
        if (_pendingUpdates.erase(creature->GetGUID().GetRawValue()))
            _pendingCount.fetch_sub(1, std::memory_order_relaxed);
    }

    void Manager::OnUnitDeath(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return;
        State state = GetState(creature->GetGUID());
        if (!state.selectLevelApplied && !state.markersApplied && !state.markersPending)
            return;
        state.selectLevelApplied = false;
        state.markersApplied = false;
        state.markersPending = false;
        state.statsPending = false;
        state.sizePending = false;
        StoreState(creature->GetGUID(), state);
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pendingUpdates.erase(creature->GetGUID().GetRawValue()))
            _pendingCount.fetch_sub(1, std::memory_order_relaxed);
    }

    void Manager::OnUnitUpdate(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive())
            return;

        if (_pendingCount.load(std::memory_order_relaxed) == 0)
            return;

        uint64 key = creature->GetGUID().GetRawValue();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_pendingUpdates.find(key) == _pendingUpdates.end())
                return;
        }

        State state = GetState(creature->GetGUID());
        if (state.statsPending)
        {
            ApplyStats(creature, state);
            state.statsPending = false;
        }
        if (state.sizePending && state.size != Size::None)
        {
            ApplySize(creature, state);
            state.sizePending = false;
        }
        if (state.markersPending)
            ApplyMarkers(creature, state);
        StoreState(creature->GetGUID(), state);

        std::lock_guard<std::mutex> lock(_mutex);
        if (!state.statsPending && !state.sizePending && !state.markersPending &&
            _pendingUpdates.erase(key))
            _pendingCount.fetch_sub(1, std::memory_order_relaxed);
    }

    void Manager::OnDisplayIdChange(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !_settings.enabled)
            return;
        State state = GetState(creature->GetGUID());
        if (state.size != Size::None)
        {
            // Creature::SetDisplayId calls SetObjectScale after Unit::SetDisplayId,
            // so an immediate reapply from this hook would be overwritten. Defer it
            // to the creature's next update, after the display call has returned.
            state.sizePending = true;
            StoreState(creature->GetGUID(), state);
            QueueUpdate(creature->GetGUID());
        }
    }

    void Manager::Apply(Creature* creature, State& state)
    {
        ApplyStats(creature, state);
        if (state.size != Size::None)
            ApplySize(creature, state);
        if (creature->IsAlive() && creature->IsInWorld())
            ApplyMarkers(creature, state);
        else
            MarkPending(creature->GetGUID(), state);
    }

    void Manager::ApplyStats(Creature* creature, State const& state)
    {
        if (!creature)
            return;

        uint32 base = state.baseMaxHealth ? state.baseMaxHealth : creature->GetMaxHealth();
        uint32 target = base;
        Modifier const* affix = AffixAt(state.affix);
        if (affix && affix->effect == Effect::Bear)
            target = uint32(float(base) * (1.0f + affix->amount));

        if (target != creature->GetMaxHealth())
        {
            creature->SetMaxHealth(target);
            if (creature->IsAlive())
                creature->SetHealth(target);
        }
    }

    void Manager::ApplySize(Creature* creature, State const& state) const
    {
        if (!creature || state.size == Size::None)
            return;
        float native = creature->GetNativeObjectScale();
        if (native <= 0.0f)
            native = 1.0f;
        creature->SetObjectScale(native * state.sizeFactor);
    }

    void Manager::ApplyMarkers(Creature* creature, State& state)
    {
        if (!creature)
            return;
        if (Modifier const* prefix = PrefixAt(state.prefix))
            creature->AddAura(prefix->carrierSpell, creature);
        if (Modifier const* affix = AffixAt(state.affix))
            creature->AddAura(affix->carrierSpell, creature);
        state.markersApplied = true;
        state.markersPending = false;
    }

    void Manager::RemoveMarkers(Creature* creature, State const& state) const
    {
        if (!creature)
            return;
        // Remove only the aura this creature cast as our carrier. A natural
        // "Mind Whispers" or "Out of Rockets" on an NPC has another caster and
        // must survive. The caster check is also sufficient after death: a
        // carrier that survives the corpse is still ours to remove.
        if (Modifier const* prefix = PrefixAt(state.prefix))
            creature->RemoveAura(prefix->carrierSpell, creature->GetGUID());
        if (Modifier const* affix = AffixAt(state.affix))
            creature->RemoveAura(affix->carrierSpell, creature->GetGUID());
    }

    void Manager::MarkPending(ObjectGuid guid, State& state)
    {
        if (!PrefixAt(state.prefix) && !AffixAt(state.affix))
            return;
        state.markersApplied = false;
        state.markersPending = true;
        QueueUpdate(guid);
    }

    void Manager::ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage)
    {
        if (!attacker || !damage)
            return;
        State state = GetState(attacker->GetGUID());
        Modifier const* prefix = PrefixAt(state.prefix);
        if (!prefix || prefix->effect != Effect::Vicious)
            return;
        damage = uint32(float(damage) * (1.0f + prefix->amount));
    }

    void Manager::ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
    {
        if (!attacker || !spellInfo || damage <= 0)
            return;
        if (!(spellInfo->SchoolMask & SPELL_SCHOOL_MASK_NORMAL))
            return;
        State state = GetState(attacker->GetGUID());
        Modifier const* prefix = PrefixAt(state.prefix);
        if (!prefix || prefix->effect != Effect::Vicious)
            return;
        damage = int32(float(damage) * (1.0f + prefix->amount));
    }

    void Manager::OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store,
                                             Player* lootOwner, uint16 lootMode)
    {
        if (!_settings.enabled || &store != &LootTemplates_Creature || !loot || !tab || !lootOwner)
            return;

        Creature* creature = ObjectAccessor::GetCreature(*lootOwner, loot->sourceWorldObjectGUID);
        if (!creature)
            return;

        State state = GetState(creature->GetGUID());
        if (!IsGilded(state))
            return;

        tab->Process(*loot, store, lootMode, lootOwner, 0, true);
        if (_settings.debug)
            LOG_DEBUG("module", "mod-creature-affixes: extra loot roll for {} ({})", creature->GetName(), creature->GetGUID().ToString());
    }

    bool Manager::Force(Creature* creature, std::string const& prefixKey, std::string const& affixKey,
                        std::string const& sizeKey, std::string& message)
    {
        if (!creature)
        {
            message = "Select a creature first.";
            return false;
        }

        Modifier const* prefix = nullptr;
        Modifier const* affix = nullptr;
        if (Lower(prefixKey) != "none")
        {
            prefix = Find(Lower(prefixKey).c_str(), Kind::Prefix);
            if (!prefix)
            {
                message = "Unknown prefix '" + prefixKey + "'. Use vicious, gilded or none.";
                return false;
            }
        }
        if (Lower(affixKey) != "none")
        {
            affix = Find(Lower(affixKey).c_str(), Kind::Affix);
            if (!affix)
            {
                message = "Unknown affix '" + affixKey + "'. Use bear or none.";
                return false;
            }
        }

        State old = GetState(creature->GetGUID());
        State state;
        state.baseMaxHealth = old.baseMaxHealth ? old.baseMaxHealth : creature->GetMaxHealth();
        state.prefix = prefix ? static_cast<int8>(prefix - Modifiers) : -1;
        state.affix = affix ? static_cast<int8>(affix - Modifiers) : -1;
        state.selectLevelApplied = true;

        std::string size = Lower(sizeKey);
        if (size == "tiny")
        {
            state.size = Size::Tiny;
            state.sizeFactor = frand(_settings.tinyMin, _settings.tinyMax);
        }
        else if (size == "giant")
        {
            state.size = Size::Giant;
            state.sizeFactor = frand(_settings.giantMin, _settings.giantMax);
        }
        else if (size != "none")
        {
            message = "Unknown size '" + sizeKey + "'. Use tiny, giant or none.";
            return false;
        }
        if (state.size != Size::None && _settings.sizeOutdoorOnly && !Outdoor(creature))
        {
            message = "The hidden size layer is outdoor-only on this realm.";
            return false;
        }

        if (old.size != Size::None && state.size == Size::None)
            creature->SetObjectScale(creature->GetNativeObjectScale());

        RemoveMarkers(creature, old);
        StoreState(creature->GetGUID(), state);
        Apply(creature, state);
        StoreState(creature->GetGUID(), state);
        message = Describe(creature);
        return true;
    }

    std::string Manager::Describe(Creature* creature) const
    {
        if (!creature)
            return "no target";

        State state = GetState(creature->GetGUID());
        std::ostringstream out;
        out << "entry=" << creature->GetEntry()
            << " prefix=" << (PrefixAt(state.prefix) ? PrefixAt(state.prefix)->name : "none")
            << " affix=" << (AffixAt(state.affix) ? AffixAt(state.affix)->name : "none")
            << " size=" << (state.size == Size::Tiny ? "tiny" : state.size == Size::Giant ? "giant" : "none")
            << " factor=" << state.sizeFactor
            << " baseMaxHealth=" << state.baseMaxHealth
            << " maxHealth=" << creature->GetMaxHealth()
            << " nativeScale=" << creature->GetNativeObjectScale()
            << " objectScale=" << creature->GetObjectScale()
            << " radius=" << creature->GetBoundaryRadius()
            << " reach=" << creature->GetCombatReach();
        return out.str();
    }
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

namespace CreatureAffixes
{
    WorldScriptImpl::WorldScriptImpl()
        : WorldScript("mod-creature-affixes", { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void WorldScriptImpl::OnAfterConfigLoad(bool /*reload*/)
    {
        Manager::instance().LoadConfig();
    }

    SpawnScript::SpawnScript()
        : AllCreatureScript("mod-creature-affixes-spawn")
    {
    }

    void SpawnScript::OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature)
    {
        Manager::instance().OnCreatureSelectLevel(creature);
    }

    void SpawnScript::OnCreatureAddWorld(Creature* creature)
    {
        Manager::instance().OnCreatureAddWorld(creature);
    }

    void SpawnScript::OnCreatureRemoveWorld(Creature* creature)
    {
        Manager::instance().OnCreatureRemoveWorld(creature);
    }

    CombatScript::CombatScript()
        : UnitScript("mod-creature-affixes-combat", true,
                     { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                       UNITHOOK_ON_UNIT_DEATH, UNITHOOK_ON_UNIT_UPDATE })
    {
    }

    void CombatScript::ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage)
    {
        Manager::instance().ModifyMeleeDamage(target, attacker, damage);
    }

    void CombatScript::ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
    {
        Manager::instance().ModifySpellDamageTaken(target, attacker, damage, spellInfo);
    }

    void CombatScript::OnUnitDeath(Unit* unit, Unit* /*killer*/)
    {
        Manager::instance().OnUnitDeath(unit);
    }

    void CombatScript::OnUnitUpdate(Unit* unit, uint32 /*diff*/)
    {
        Manager::instance().OnUnitUpdate(unit);
    }

    DisplayScript::DisplayScript()
        : UnitScript("mod-creature-affixes-display", true, { UNITHOOK_ON_DISPLAYID_CHANGE })
    {
    }

    void DisplayScript::OnDisplayIdChange(Unit* unit, uint32 /*displayId*/)
    {
        Manager::instance().OnDisplayIdChange(unit);
    }

    LootScript::LootScript()
        : MiscScript("mod-creature-affixes-loot", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS })
    {
    }

    void LootScript::OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store,
                                                Player* lootOwner, bool /*personal*/, bool /*noEmptyError*/,
                                                uint16 lootMode)
    {
        Manager::instance().OnAfterLootTemplateProcess(loot, tab, store, lootOwner, lootMode);
    }
}

class creature_affixes_commandscript : public CommandScript
{
public:
    creature_affixes_commandscript() : CommandScript("creature_affixes_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "affix", HandleAffixCommand, SEC_GAMEMASTER, Console::Yes },
        };
        return commandTable;
    }

    static bool HandleAffixCommand(ChatHandler* handler, char const* args)
    {
        if (!handler->GetPlayer())
            return false;

        Creature* creature = handler->GetPlayer()->GetSelectedUnit()
            ? handler->GetPlayer()->GetSelectedUnit()->ToCreature()
            : nullptr;

        std::istringstream input(args ? args : "");
        std::string command;
        input >> command;
        command = Lower(command);

        if (command.empty() || command == "show")
        {
            handler->SendSysMessage(CreatureAffixes::Manager::instance().Describe(creature));
            return true;
        }

        if (command != "set")
        {
            handler->SendSysMessage("usage: .affix show | .affix set <vicious|gilded|none> <bear|none> <tiny|giant|none>");
            return false;
        }

        std::string prefix, affix, size;
        if (!(input >> prefix >> affix >> size))
        {
            handler->SendSysMessage("usage: .affix set <vicious|gilded|none> <bear|none> <tiny|giant|none>");
            return false;
        }

        std::string message;
        if (!CreatureAffixes::Manager::instance().Force(creature, prefix, affix, size, message))
        {
            handler->SendSysMessage(message);
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->SendSysMessage(message);
        return true;
    }
};

void AddCreatureAffixesScripts()
{
    new CreatureAffixes::WorldScriptImpl();
    new CreatureAffixes::SpawnScript();
    new CreatureAffixes::CombatScript();
    new CreatureAffixes::DisplayScript();
    new CreatureAffixes::LootScript();
    new creature_affixes_commandscript();
}
