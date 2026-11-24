/*
 * mod-creature-affixes - initial implementation.
 *
 * This file deliberately contains only the three proof modifiers plus the
 * hidden size layer. The remaining modifiers from the design are not listed
 * here yet; adding a row to the modifier table and implementing its Effect is the
 * extension point.
 */

#include "creature_affixes.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "LootMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
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
        CreatureAffixes::Manager const& manager = CreatureAffixes::Manager::instance();
        CreatureAffixes::Modifier const* modifier = manager.PrefixAt(state.prefix);
        return modifier && modifier->effect == CreatureAffixes::Effect::Gilded;
    }

    // The add-on reads the marker through UnitAura, so the carrier must be a
    // permanent, client-visible dummy. SPELL_ATTR0_DO_NOT_DISPLAY is not usable
    // here: the client filters that aura out of UnitAura as well as the UI.
    bool IsVisibleDummyCarrier(SpellInfo const* carrier)
    {
        if (!carrier || carrier->IsPassive() || carrier->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
            return false;
        if (carrier->GetMaxDuration() >= 0)
            return false;

        bool dummy = false;
        for (SpellEffectInfo const& effect : carrier->GetEffects())
        {
            if (effect.Effect == 0)
                continue;
            if (effect.Effect != SPELL_EFFECT_APPLY_AURA || effect.ApplyAuraName != SPELL_AURA_DUMMY)
                return false;
            dummy = true;
        }
        return dummy;
    }

    bool ParseEffect(std::string const& text, CreatureAffixes::Effect& effect)
    {
        if (text == "vicious")
            effect = CreatureAffixes::Effect::Vicious;
        else if (text == "gilded")
            effect = CreatureAffixes::Effect::Gilded;
        else if (text == "bear")
            effect = CreatureAffixes::Effect::Bear;
        else if (text == "none")
            effect = CreatureAffixes::Effect::None;
        else
            return false;
        return true;
    }

    bool ParseEffectRole(std::string const& text, CreatureAffixes::EffectRole& role)
    {
        if (text == "aura")
        {
            role = CreatureAffixes::EffectRole::Aura;
            return true;
        }
        if (text == "visual")
        {
            role = CreatureAffixes::EffectRole::Visual;
            return true;
        }
        return false;
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

        LOG_INFO("module", "mod-creature-affixes: enabled={} prefix={}% affix={}% size={}%",
                 _settings.enabled, _settings.prefixChance, _settings.affixChance, _settings.sizeChance);
    }

    void Manager::LoadModifiers()
    {
        std::vector<Modifier> loaded;
        QueryResult result = WorldDatabase.Query(
            "SELECT `modifier_key`, `kind`, `label`, `carrier_spell`, `influence`, `effect`, `amount` "
            "FROM `mod_creature_affixes_modifier` ORDER BY `id`");
        if (!result)
        {
            _modifiers.clear();
            LOG_ERROR("module", "mod-creature-affixes: `mod_creature_affixes_modifier` is missing or empty; "
                                "apply `data/sql/world/base/creature_affixes_10_schema.sql` and "
                                "`creature_affixes_20_modifiers.sql`.");
            return;
        }

        do
        {
            Field* fields = result->Fetch();

            Modifier modifier;
            modifier.key = fields[0].Get<std::string>();
            std::string kind = fields[1].Get<std::string>();
            modifier.label = fields[2].Get<std::string>();
            modifier.carrierSpell = fields[3].Get<uint32>();
            modifier.influence = static_cast<uint8>(std::clamp<uint32>(fields[4].Get<uint32>(), 1, 5));
            modifier.effect = Effect::None;
            modifier.amount = fields[6].Get<float>();

            if (kind == "prefix")
                modifier.kind = Kind::Prefix;
            else if (kind == "affix")
                modifier.kind = Kind::Affix;
            else
            {
                LOG_ERROR("module", "mod-creature-affixes: modifier '{}' has unknown kind '{}'; skipped",
                          modifier.key, kind);
                continue;
            }

            std::string effect = fields[5].Get<std::string>();
            if (!ParseEffect(effect, modifier.effect))
            {
                LOG_ERROR("module", "mod-creature-affixes: modifier '{}' has unknown effect '{}'; skipped",
                          modifier.key, effect);
                continue;
            }

            if (modifier.key.empty() || modifier.label.empty() || !modifier.carrierSpell)
            {
                LOG_ERROR("module", "mod-creature-affixes: modifier row has an empty key/label or no carrier spell; skipped");
                continue;
            }

            SpellInfo const* carrier = sSpellMgr->GetSpellInfo(modifier.carrierSpell);
            if (!carrier)
            {
                LOG_ERROR("module", "mod-creature-affixes: modifier '{}' carrier spell {} does not exist; skipped",
                          modifier.key, modifier.carrierSpell);
                continue;
            }

            if (!IsVisibleDummyCarrier(carrier))
            {
                LOG_ERROR("module", "mod-creature-affixes: modifier '{}' carrier spell {} is not a "
                                    "client-visible permanent dummy; row skipped",
                          modifier.key, modifier.carrierSpell);
                continue;
            }

            loaded.push_back(std::move(modifier));
        } while (result->NextRow());

        std::unordered_map<std::string, std::vector<EffectSpell>> effectSpells;
        QueryResult effectsResult = WorldDatabase.Query(
            "SELECT `modifier_key`, `role`, `spell_id` "
            "FROM `mod_creature_affixes_modifier_effect` ORDER BY `modifier_key`, `sort_order`");
        if (!effectsResult)
        {
            LOG_ERROR("module", "mod-creature-affixes: `mod_creature_affixes_modifier_effect` is missing; "
                                "candidate modifiers will load without their effect auras.");
        }
        else
        {
            do
            {
                Field* fields = effectsResult->Fetch();
                if (!fields)
                    break;
                std::string modifierKey = fields[0].Get<std::string>();
                std::string roleText = fields[1].Get<std::string>();
                uint32 spellId = fields[2].Get<uint32>();

                EffectRole role;
                if (!ParseEffectRole(roleText, role))
                {
                    LOG_ERROR("module", "mod-creature-affixes: effect for '{}' has unknown role '{}'; skipped",
                              modifierKey, roleText);
                    continue;
                }
                if (!spellId)
                {
                    LOG_ERROR("module", "mod-creature-affixes: effect for '{}' has no spell; skipped", modifierKey);
                    continue;
                }

                SpellInfo const* effectSpell = sSpellMgr->GetSpellInfo(spellId);
                if (!effectSpell)
                {
                    LOG_ERROR("module", "mod-creature-affixes: effect for '{}' references missing spell {}; skipped",
                              modifierKey, spellId);
                    continue;
                }
                if (effectSpell->IsPassive())
                {
                    LOG_ERROR("module", "mod-creature-affixes: effect for '{}' spell {} is passive; skipped",
                              modifierKey, spellId);
                    continue;
                }
                if (effectSpell->GetMaxDuration() >= 0)
                {
                    LOG_ERROR("module", "mod-creature-affixes: effect for '{}' spell {} is not permanent; skipped",
                              modifierKey, spellId);
                    continue;
                }

                effectSpells[modifierKey].push_back({ role, spellId });
            } while (effectsResult->NextRow());
        }

        std::unordered_map<std::string, size_t> indexes;
        for (size_t index = 0; index < loaded.size(); ++index)
            indexes[loaded[index].key] = index;
        for (auto& [modifierKey, effects] : effectSpells)
        {
            auto found = indexes.find(modifierKey);
            if (found == indexes.end())
            {
                LOG_ERROR("module", "mod-creature-affixes: effect rows reference unknown modifier '{}'; skipped",
                          modifierKey);
                continue;
            }
            loaded[found->second].effects = std::move(effects);
        }

        if (loaded.empty())
        {
            _modifiers.clear();
            LOG_ERROR("module", "mod-creature-affixes: no usable modifier rows loaded; no affixes will roll.");
            return;
        }

        size_t prefixes = std::count_if(loaded.begin(), loaded.end(), [](Modifier const& modifier)
        {
            return modifier.kind == Kind::Prefix;
        });
        size_t affixes = loaded.size() - prefixes;

        _modifiers = std::move(loaded);
        LOG_INFO("module", "mod-creature-affixes: loaded {} modifier(s): {} prefix, {} affix",
                 _modifiers.size(), prefixes, affixes);
    }

    Modifier const* Manager::Find(std::string const& key, Kind kind) const
    {
        for (Modifier const& modifier : _modifiers)
            if (modifier.kind == kind && modifier.key == key)
                return &modifier;
        return nullptr;
    }

    Modifier const* Manager::PrefixAt(int8 index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= _modifiers.size())
            return nullptr;
        Modifier const& modifier = _modifiers[index];
        return modifier.kind == Kind::Prefix ? &modifier : nullptr;
    }

    Modifier const* Manager::AffixAt(int8 index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= _modifiers.size())
            return nullptr;
        Modifier const& modifier = _modifiers[index];
        return modifier.kind == Kind::Affix ? &modifier : nullptr;
    }

    std::string Manager::KeyList(Kind kind) const
    {
        std::string keys;
        for (Modifier const& modifier : _modifiers)
        {
            if (modifier.kind != kind)
                continue;
            if (!keys.empty())
                keys += ", ";
            keys += modifier.key;
        }
        return keys.empty() ? "none" : keys;
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
        for (Modifier const& modifier : _modifiers)
            if (modifier.kind == Kind::Prefix)
                total += Weight(modifier.influence);
        if (!total)
            return false;

        uint32 roll = urand(1, total);
        uint32 seen = 0;
        for (size_t i = 0; i < _modifiers.size(); ++i)
        {
            Modifier const& modifier = _modifiers[i];
            if (modifier.kind != Kind::Prefix)
                continue;
            seen += Weight(modifier.influence);
            if (roll <= seen)
            {
                state.prefix = static_cast<int8>(i);
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
        for (Modifier const& modifier : _modifiers)
            if (modifier.kind == Kind::Affix)
                total += Weight(modifier.influence);
        if (!total)
            return false;

        uint32 roll = urand(1, total);
        uint32 seen = 0;
        for (size_t i = 0; i < _modifiers.size(); ++i)
        {
            Modifier const& modifier = _modifiers[i];
            if (modifier.kind != Kind::Affix)
                continue;
            seen += Weight(modifier.influence);
            if (roll <= seen)
            {
                state.affix = static_cast<int8>(i);
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
            RemoveEffects(creature, old);
            if (!old.baseName.empty())
            {
                State cleared;
                cleared.baseName = old.baseName;
                ApplyName(creature, cleared);
            }
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
        state.baseName = old.baseName.empty() ? creature->GetName() : old.baseName;
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
        RemoveEffects(creature, old);
        Apply(creature, state);
        StoreState(creature->GetGUID(), state);

        if (_settings.debug)
            LOG_DEBUG("module", "mod-creature-affixes: spawn {} entry {} prefix={} affix={} size={:.2f}",
                      creature->GetGUID().ToString(), creature->GetEntry(),
                      PrefixAt(state.prefix) ? PrefixAt(state.prefix)->label : "none",
                      AffixAt(state.affix) ? AffixAt(state.affix)->label : "none",
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
        ApplyName(creature, state);
        ApplyStats(creature, state);
        state.statsPending = false;
        if (state.size != Size::None)
        {
            ApplySize(creature, state);
            state.sizePending = false;
        }
        if (creature->IsAlive())
        {
            ApplyMarkers(creature, state);
            ApplyEffects(creature, state);
        }
        else
            MarkPending(creature->GetGUID(), state);
        StoreState(creature->GetGUID(), state);
    }

    void Manager::OnCreatureRemoveWorld(Creature* creature)
    {
        if (!creature)
            return;
        State state = GetState(creature->GetGUID());
        if (!state.baseName.empty() && creature->GetName() != state.baseName)
            creature->SetName(state.baseName);
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
        if (!state.selectLevelApplied && !state.markersApplied && !state.markersPending &&
            !state.effectsApplied && !state.effectsPending)
            return;
        RemoveEffects(creature, state);
        state.selectLevelApplied = false;
        state.markersApplied = false;
        state.markersPending = false;
        state.effectsApplied = false;
        state.effectsPending = false;
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
        if (state.effectsPending)
            ApplyEffects(creature, state);
        StoreState(creature->GetGUID(), state);

        std::lock_guard<std::mutex> lock(_mutex);
        if (!state.statsPending && !state.sizePending && !state.markersPending && !state.effectsPending &&
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
        ApplyName(creature, state);
        ApplyStats(creature, state);
        if (state.size != Size::None)
            ApplySize(creature, state);
        if (creature->IsAlive() && creature->IsInWorld())
        {
            ApplyMarkers(creature, state);
            ApplyEffects(creature, state);
        }
        else
            MarkPending(creature->GetGUID(), state);
    }

    void Manager::ApplyName(Creature* creature, State& state)
    {
        if (!creature)
            return;

        if (state.baseName.empty())
            state.baseName = creature->GetName();

        std::string name = state.baseName;
        if (Modifier const* prefix = PrefixAt(state.prefix))
            name = prefix->label + " " + name;
        if (Modifier const* affix = AffixAt(state.affix))
            name += " " + affix->label;

        if (name == creature->GetName())
            return;

        // Nameplates are built from the creature's create block, not a unit
        // field. A respawn picks the composed name up before the object is sent;
        // a GM-forced reroll after spawn needs the create block sent again.
        creature->SetName(name);
        if (!creature->IsInWorld())
            return;
        for (auto const& ref : creature->GetMap()->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (player && player->IsInWorld() && player->CanSeeOrDetect(creature))
                creature->SendUpdateToPlayer(player);
        }
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
        bool applied = true;
        if (Modifier const* prefix = PrefixAt(state.prefix))
            applied = creature->AddAura(prefix->carrierSpell, creature) != nullptr;
        if (Modifier const* affix = AffixAt(state.affix))
            applied = creature->AddAura(affix->carrierSpell, creature) != nullptr;
        if (!applied)
        {
            LOG_WARN("module", "mod-creature-affixes: could not apply markers to {} ({})",
                     creature->GetName(), creature->GetGUID().ToString());
            state.markersApplied = false;
            state.markersPending = false;
            return;
        }
        state.markersApplied = true;
        state.markersPending = false;
    }

    void Manager::RemoveMarkers(Creature* creature, State const& state) const
    {
        if (!creature)
            return;
        // Remove only the aura this creature cast as our carrier. A natural
        // aura with the same spell id on an NPC has another caster and must
        // survive. The caster check is also sufficient after death: a carrier
        // that survives the corpse is still ours to remove.
        if (Modifier const* prefix = PrefixAt(state.prefix))
            creature->RemoveAura(prefix->carrierSpell, creature->GetGUID());
        if (Modifier const* affix = AffixAt(state.affix))
            creature->RemoveAura(affix->carrierSpell, creature->GetGUID());
    }

    void Manager::ApplyEffects(Creature* creature, State& state)
    {
        if (!creature)
            return;

        bool applied = true;
        std::unordered_set<uint32> seen;
        auto apply = [&](Modifier const* modifier)
        {
            if (!modifier)
                return;
            for (EffectSpell const& effect : modifier->effects)
            {
                if (!seen.insert(effect.spellId).second)
                    continue;
                if (!creature->AddAura(effect.spellId, creature))
                {
                    LOG_WARN("module", "mod-creature-affixes: could not apply effect spell {} to {} ({})",
                             effect.spellId, creature->GetName(), creature->GetGUID().ToString());
                    applied = false;
                }
            }
        };
        apply(PrefixAt(state.prefix));
        apply(AffixAt(state.affix));

        state.effectsApplied = applied;
        state.effectsPending = !applied;
    }

    void Manager::RemoveEffects(Creature* creature, State const& state) const
    {
        if (!creature)
            return;

        std::unordered_set<uint32> seen;
        auto remove = [&](Modifier const* modifier)
        {
            if (!modifier)
                return;
            for (EffectSpell const& effect : modifier->effects)
            {
                if (seen.insert(effect.spellId).second)
                    creature->RemoveAura(effect.spellId, creature->GetGUID());
            }
        };
        remove(PrefixAt(state.prefix));
        remove(AffixAt(state.affix));
    }

    void Manager::MarkPending(ObjectGuid guid, State& state)
    {
        if (!PrefixAt(state.prefix) && !AffixAt(state.affix))
            return;
        state.markersApplied = false;
        state.markersPending = true;
        state.effectsApplied = false;
        state.effectsPending = true;
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
            prefix = Find(Lower(prefixKey), Kind::Prefix);
            if (!prefix)
            {
                message = "Unknown prefix '" + prefixKey + "'. Configured prefixes: " + KeyList(Kind::Prefix) + ", or none.";
                return false;
            }
        }
        if (Lower(affixKey) != "none")
        {
            affix = Find(Lower(affixKey), Kind::Affix);
            if (!affix)
            {
                message = "Unknown affix '" + affixKey + "'. Configured affixes: " + KeyList(Kind::Affix) + ", or none.";
                return false;
            }
        }

        State old = GetState(creature->GetGUID());
        State state;
        state.baseName = old.baseName.empty() ? creature->GetName() : old.baseName;
        state.baseMaxHealth = old.baseMaxHealth ? old.baseMaxHealth : creature->GetMaxHealth();
        state.prefix = prefix ? static_cast<int8>(prefix - _modifiers.data()) : -1;
        state.affix = affix ? static_cast<int8>(affix - _modifiers.data()) : -1;
        state.selectLevelApplied = true;

        std::string size = sizeKey.empty() ? "none" : Lower(sizeKey);
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
        RemoveEffects(creature, old);
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
            << " name=\"" << creature->GetName() << "\""
            << " baseName=\"" << state.baseName << "\""
            << " prefix=" << (PrefixAt(state.prefix) ? PrefixAt(state.prefix)->label : "none")
            << " affix=" << (AffixAt(state.affix) ? AffixAt(state.affix)->label : "none")
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

    bool Manager::Labels(ObjectGuid guid, std::string& prefix, std::string& affix) const
    {
        prefix.clear();
        affix.clear();
        if (!_settings.enabled || !guid)
            return false;

        State state = GetState(guid);
        Modifier const* prefixModifier = PrefixAt(state.prefix);
        Modifier const* affixModifier = AffixAt(state.affix);
        if (!prefixModifier && !affixModifier)
            return false;
        if (prefixModifier)
            prefix = prefixModifier->label;
        if (affixModifier)
            affix = affixModifier->label;
        return true;
    }
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

namespace CreatureAffixes
{
    WorldScriptImpl::WorldScriptImpl()
        : WorldScript("mod-creature-affixes", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED })
    {
    }

    void WorldScriptImpl::OnAfterConfigLoad(bool /*reload*/)
    {
        Manager::instance().LoadConfig();
    }

    void WorldScriptImpl::OnBeforeWorldInitialized()
    {
        // The grid preload runs after this hook; loading in OnStartup would leave
        // every preloaded spawn with an empty modifier pool and no affix roll.
        Manager::instance().LoadModifiers();
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

namespace
{
    constexpr char const* kAddonPrefix = "CAFF";

    void SendAddon(Player* player, std::string const& what)
    {
        if (!player)
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                     std::string(kAddonPrefix) + "\t" + what);
        player->SendDirectMessage(&data);
    }

    bool HandleAddonChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver)
    {
        if (!player || player != receiver || language != LANG_ADDON || type != CHAT_MSG_WHISPER)
            return true;

        constexpr char const* request = "CAFF\tlabels\t";
        static constexpr size_t requestLength = sizeof("CAFF\tlabels\t") - 1;
        if (msg.compare(0, requestLength, request) != 0)
            return true;

        std::string_view rest(msg);
        rest.remove_prefix(requestLength);
        unsigned replies = 0;
        while (!rest.empty() && replies < 16)
        {
            size_t const tab = rest.find('\t');
            std::string const token(rest.substr(0, tab));
            rest.remove_prefix(tab == std::string_view::npos ? rest.size() : tab + 1);
            if (token.empty())
                continue;

            Optional<uint64> raw = Acore::StringTo<uint64>(token, 0);
            if (!raw || !*raw)
                continue;

            std::string prefix;
            std::string affix;
            CreatureAffixes::Manager::instance().Labels(ObjectGuid(*raw), prefix, affix);
            SendAddon(player, Acore::StringFormat("label\t{}\t{}\t{}", token, prefix, affix));
            ++replies;
        }
        return false;
    }
}

class CreatureAffixesAddonScript : public PlayerScript
{
public:
    CreatureAffixesAddonScript() : PlayerScript("CreatureAffixes_AddonScript") { }

    [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                          Player* receiver) override
    {
        return HandleAddonChat(player, type, language, msg, receiver);
    }
};

class ZombifiedMiasmaAuraScript : public AuraScript
{
    PrepareAuraScript(ZombifiedMiasmaAuraScript);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        if (!target)
            return;

        uint32 level = target->GetLevel();
        uint32 percent = level >= 20 ? 6 : level >= 10 ? 4 : 2;

        if (AuraEffect* effect = GetEffect(aurEff->GetEffIndex()))
            effect->ChangeAmount(int32(percent), false);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(ZombifiedMiasmaAuraScript::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
    }
};

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
            handler->SendSysMessage("usage: .affix show | .affix set <prefix-key|none> <affix-key|none> [tiny|giant|none]");
            return false;
        }

        std::string prefix, affix, size;
        if (!(input >> prefix >> affix))
        {
            handler->SendSysMessage("usage: .affix set <prefix-key|none> <affix-key|none> [tiny|giant|none]");
            return false;
        }
        input >> size;

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
    new CreatureAffixesAddonScript();
    new creature_affixes_commandscript();
    RegisterSpellScript(ZombifiedMiasmaAuraScript);
}
