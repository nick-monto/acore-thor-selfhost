/*
 * mod-creature-affixes - initial slice.
 *
 * Three proof modifiers and the independent hidden size layer:
 *   - Vicious (prefix)       +12% physical damage
 *   - of the Bear (affix)    +15% maximum health
 *   - Gilded (prefix)        one extra creature loot roll
 *   - Tiny / Giant           hidden size layer, outdoor world only
 *
 * The marker auras are stock client-known dummy spells with
 * SPELL_ATTR0_DO_NOT_DISPLAY. The client receives them but hides the aura bar
 * entry; the client add-on maps their spell names to the modifier labels. No
 * client patch is involved. Size never gets a marker or add-on data.
 *
 * Modifier definitions live in `mod_creature_affixes_modifier` and are loaded
 * once at world startup. Per-spawn state stays in memory.
 */

#ifndef MOD_CREATURE_AFFIXES_H
#define MOD_CREATURE_AFFIXES_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AllCreatureScript.h"
#include "ObjectGuid.h"
#include "MiscScript.h"
#include "UnitScript.h"
#include "WorldScript.h"

class Creature;
class CreatureTemplate;
class Loot;
class Player;
class Unit;
struct LootStore;
struct LootTemplate;
struct SpellInfo;

namespace CreatureAffixes
{
    enum class Kind : uint8
    {
        Prefix,
        Affix,
    };

    enum class Effect : uint8
    {
        None,
        Vicious,
        Bear,
        Gilded,
    };

    enum class Size : uint8
    {
        None,
        Tiny,
        Giant,
    };

    struct Modifier
    {
        std::string key;
        Kind kind;
        std::string label;
        uint32 carrierSpell;
        uint8 influence;
        Effect effect;
        float amount;
    };

    struct Settings
    {
        bool enabled = true;
        bool debug = false;
        float prefixChance = 10.0f;
        float affixChance = 10.0f;
        float sizeChance = 10.0f;
        uint32 maxCombinedInfluence = 6;
        bool sizeOutdoorOnly = true;
        float tinyMin = 0.70f;
        float tinyMax = 0.85f;
        float giantMin = 1.15f;
        float giantMax = 1.30f;
    };

    struct State
    {
        int8 prefix = -1;
        int8 affix = -1;
        Size size = Size::None;
        float sizeFactor = 1.0f;
        uint32 baseMaxHealth = 0;
        bool selectLevelApplied = false;
        bool markersApplied = false;
        bool markersPending = false;
        bool statsPending = false;
        bool sizePending = false;
    };

    class Manager
    {
    public:
        static Manager& instance();

        void LoadConfig();
        void LoadModifiers();

        void OnCreatureSelectLevel(Creature* creature);
        void OnCreatureAddWorld(Creature* creature);
        void OnCreatureRemoveWorld(Creature* creature);
        void OnDisplayIdChange(Unit* unit);
        void OnUnitDeath(Unit* unit);
        void OnUnitUpdate(Unit* unit);

        void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage);
        void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo);

        void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store,
                                       Player* lootOwner, uint16 lootMode);

        // GM test surface: force a state on the selected creature. Keys are the
        // Modifier::key values or "none"; size is "tiny", "giant" or "none".
        bool Force(Creature* creature, std::string const& prefixKey, std::string const& affixKey,
                   std::string const& sizeKey, std::string& message);
        std::string Describe(Creature* creature) const;

        Modifier const* PrefixAt(int8 index) const;
        Modifier const* AffixAt(int8 index) const;

    private:
        Manager() = default;

        bool Eligible(Creature const* creature) const;
        bool Outdoor(Creature const* creature) const;
        bool RollPrefix(State& state) const;
        bool RollAffix(State& state) const;
        void RollSize(State& state) const;
        void Apply(Creature* creature, State& state);
        void ApplyStats(Creature* creature, State const& state);
        void ApplySize(Creature* creature, State const& state) const;
        void ApplyMarkers(Creature* creature, State& state);
        void RemoveMarkers(Creature* creature, State const& state) const;
        State GetState(ObjectGuid guid) const;
        void StoreState(ObjectGuid guid, State const& state);
        void QueueUpdate(ObjectGuid guid);
        void MarkPending(ObjectGuid guid, State& state);
        void EraseState(ObjectGuid guid);
        Modifier const* Find(std::string const& key, Kind kind) const;
        std::string KeyList(Kind kind) const;

        Settings _settings;
        std::vector<Modifier> _modifiers;
        mutable std::mutex _mutex;
        std::unordered_map<uint64, State> _states;
        std::unordered_set<uint64> _pendingUpdates;
        std::atomic<uint32> _pendingCount{0};
    };

    // Scripts are instantiated once by AddCreatureAffixesScripts().
    class WorldScriptImpl : public WorldScript
    {
    public:
        WorldScriptImpl();
        void OnAfterConfigLoad(bool reload) override;
        void OnBeforeWorldInitialized() override;
    };

    class SpawnScript : public AllCreatureScript
    {
    public:
        SpawnScript();
        void OnCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature) override;
        void OnCreatureAddWorld(Creature* creature) override;
        void OnCreatureRemoveWorld(Creature* creature) override;
    };

    class CombatScript : public UnitScript
    {
    public:
        CombatScript();
        void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override;
        void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override;
        void OnUnitDeath(Unit* unit, Unit* killer) override;
        void OnUnitUpdate(Unit* unit, uint32 diff) override;
    };

    class DisplayScript : public UnitScript
    {
    public:
        DisplayScript();
        void OnDisplayIdChange(Unit* unit, uint32 displayId) override;
    };

    class LootScript : public MiscScript
    {
    public:
        LootScript();
        void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store,
                                        Player* lootOwner, bool personal, bool noEmptyError,
                                        uint16 lootMode) override;
    };
}

#endif
