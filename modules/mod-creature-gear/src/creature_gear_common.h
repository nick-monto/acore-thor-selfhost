/*
 * This file is part of mod-creature-gear.
 *
 * Shared interfaces for the three halves of the module:
 *   - CreatureGearMgr.cpp         configuration, outfit/loadout storage, drop candidate selection
 *   - creature_gear_loader.cpp    Addmod_creature_gearScripts(): instantiates every script below
 *   - CreatureGearDrop.cpp        phase B: loot injection ("drops what it wears")
 *   - CreatureGearAppearance.cpp  phase C: the eleven-slot armour outfit over the mirror-image packet
 *
 * Data model (see docs/CREATURE-GEAR-DESIGN.md):
 *   - phase A holds weapons: creature_equip_template pools + creature.equipment_id (core tables).
 *   - phases B and C read `mod_creature_gear_outfit`, one row per creature entry, whose eleven
 *     ItemEntry columns are in the same order as the SMSG_MIRRORIMAGE_DATA display list.
 *   - `mod_creature_gear_outfit_variant` overrides five of those slots - HEAD, CHEST, LEGS, FEET
 *     and HANDS - with one row per variant. The module picks a variant per spawn point, so the
 *     armour a creature shows and drops differs between spawn points while the other six slots
 *     (shoulders, shirt, belt, bracers, cloak, tabard) stay per entry.
 */

#ifndef MOD_CREATURE_GEAR_COMMON_H
#define MOD_CREATURE_GEAR_COMMON_H

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "AllCreatureScript.h"
#include "MiscScript.h"
#include "ServerScript.h"
#include "UnitScript.h"
#include "WorldScript.h"

class Creature;
class Loot;
class Player;
class Unit;
class WorldPacket;
class WorldSession;
struct LootStore;
struct LootTemplate;

namespace CreatureGear
{
    // The eleven display slots of SMSG_MIRRORIMAGE_DATA, in packet order.
    // (SpellHandler.cpp:789-801 uses HEAD, SHOULDERS, BODY, CHEST, WAIST, LEGS, FEET, WRISTS, HANDS, BACK, TABARD.)
    enum OutfitSlot : uint8
    {
        SLOT_HEAD = 0,
        SLOT_SHOULDERS,
        SLOT_BODY,
        SLOT_CHEST,
        SLOT_WAIST,
        SLOT_LEGS,
        SLOT_FEET,
        SLOT_WRISTS,
        SLOT_HANDS,
        SLOT_BACK,
        SLOT_TABARD,
        OUTFIT_SLOTS
    };

    // item_template.InventoryType -> OutfitSlot, or OUTFIT_SLOTS when the item is not part of an outfit.
    // 1 head, 3 shoulders, 4 body/shirt, 5 chest, 6 waist, 7 legs, 8 feet, 9 wrists, 10 hands,
    // 16 back, 19 tabard; 20 robe also maps to SLOT_CHEST. Everything else is not an outfit item.
    uint32 OutfitSlotFromInventoryType(uint32 inventoryType);

    // "head", "shoulders", ... for logs and generated SQL comments. Never null.
    char const* OutfitSlotName(uint32 slot);

    // One creature entry's appearance loadout.
    struct Outfit
    {
        uint32 creatureId = 0;
        // 0 = derive at runtime: race from the creature's own baked display
        // (CreatureDisplayInfo -> CreatureDisplayInfoExtra.DisplayRaceID -> ChrRaces.model_m/model_f),
        // class from creature_template.unit_class.
        uint32 race = 0;
        uint32 gender = 0;   // 0 male, 1 female
        uint32 klass = 0;
        // Customisation bytes. A non-zero value pins that field for the whole entry; 0 lets
        // Manager::GetCustomisation sample it per spawn from the pinned client data.
        uint32 skin = 0;
        uint32 face = 0;
        uint32 hairStyle = 0;
        uint32 hairColor = 0;
        uint32 facialHair = 0;
        std::array<uint32, OUTFIT_SLOTS> itemEntry{};   // item_template.entry, 0 = bare slot
        std::array<uint32, OUTFIT_SLOTS> displayId{};   // resolved ItemDisplayInfo id, 0 = bare slot
    };

    // Every option lives in conf/mod-creature-gear.conf.dist under the CreatureGear.* prefix.
    struct Settings
    {
        bool enabled = true;
        bool appearanceEnabled = true;
        // Sample the five customisation bytes per spawn instead of showing the outfit row's values
        // for every instance of the entry (see Manager::GetCustomisation).
        bool appearanceVaryPerSpawn = true;
        bool weaponsRerollOnRespawn = true;
        bool dropEnabled = true;
        bool dropRequireBoE = true;
        bool dropIncludeNoLootTemplate = true;
        float dropChance = 5.0f;
        uint32 dropMaxQuality = 2;
        uint32 dropMaxItems = 1;
        uint32 dropLevelMargin = 3;
        bool debug = false;
    };

    // One CharSections.dbc row for a race, sex and section. The client resolves the mirror-image
    // customisation bytes against exactly these rows, so only the pairs below are safe to send: the
    // face section is not a full cross product of faces and skin colours, which is why both values
    // travel together here.
    struct AppearanceVariant
    {
        uint32 variation = 0;   // CharSections.VariationIndex: face index, hair style, facial-hair style
        uint32 color = 0;       // CharSections.ColorIndex: skin colour, hair colour
    };

    // What the pinned CharSections.dbc holds for one race and sex. An empty vector means the client
    // data has no rows for that section - facial hair on most female races, for instance.
    struct AppearanceTable
    {
        std::vector<AppearanceVariant> baseSkin;    // SECTION_TYPE_SKIN
        std::vector<AppearanceVariant> face;        // SECTION_TYPE_FACE
        std::vector<AppearanceVariant> facialHair;  // SECTION_TYPE_FACIAL_HAIR
        std::vector<AppearanceVariant> hair;        // SECTION_TYPE_HAIR
    };

    class Manager
    {
    public:
        static Manager* instance();

        // Reads every CreatureGear.* option; safe to call on every config load
        // (WorldScript::OnAfterConfigLoad(bool)) and on `.reload config`.
        void LoadConfig(bool reload);

        // SELECTs `mod_creature_gear_outfit` and `mod_creature_gear_outfit_variant` and resolves each
        // row's display ids from item_template. Requires the world database, so call it from a
        // config-load or startup hook, never from a constructor.
        void LoadOutfits();

        Settings const& GetSettings() const { return _settings; }
        bool Enabled() const { return _settings.enabled; }

        // The outfit variant this creature wears, or null when its entry has no outfit row. The
        // variant is a pure function of the spawn id, the same seed the appearance bytes use, so one
        // spawn point keeps one set of armour across restarts and respawns while different spawn
        // points of an entry differ. The appearance and the drop both read this, so a corpse yields
        // the armour its spawn point shows.
        Outfit const* GetOutfit(Creature const* creature) const;

        // 0-based index of the variant above, and how many variants the entry has (0 without a row).
        uint32 OutfitVariantIndex(Creature const* creature) const;
        size_t OutfitVariantCount(uint32 creatureEntry) const;

        // True when the entry has at least one outfit variant.
        bool HasOutfit(uint32 creatureEntry) const;

        size_t OutfitCount() const { return _outfits.size(); }

        // Item entries that may drop for this creature: its three worn weapons plus every
        // item in its outfit, filtered by the CreatureGear.Drop.* options. Empty when drops
        // are disabled or nothing qualifies.
        std::vector<uint32> GetDropCandidates(Creature const* creature) const;

        // True when the worn weapon slots should be re-rolled for this creature: a hit means the
        // creature's entry is covered by the module's own outfit/weapon-pool data, whose spawn rows
        // the generators set to `creature.equipment_id = -1`. Testing that `-1` sentinel directly is
        // not possible on this core pin: ObjectMgr::LoadCreatureData resolves it in place at
        // worldserver start, so Creature::GetOriginalEquipmentId() already holds the concrete
        // variant index. Clear the entry's outfit row to opt the creature out.
        bool ShouldRerollWeapons(Creature const* creature) const;

        // Whether a single item entry passes the CreatureGear.Drop.* filters for this creature.
        bool IsDropEligible(Creature const* creature, uint32 itemEntry) const;

        // The five customisation bytes SMSG_MIRRORIMAGE_DATA carries, in the order the packet writes
        // them. Values are straight out of CharSections.dbc, so the client has a row for each one.
        struct Customisation
        {
            uint32 skin = 0;
            uint32 face = 0;
            uint32 hairStyle = 0;
            uint32 hairColor = 0;
            uint32 facialHair = 0;
        };

        // Resolves one creature's customisation bytes. A field the outfit row pins (non-zero) is taken
        // from the row; the rest is sampled from the table above under
        // CreatureGear.Appearance.VaryPerSpawn, seeded with the creature's spawn id, so one spawn
        // point keeps one face while different spawn points differ. Callers must pass the race and
        // gender they morph the creature onto: those two decide which rows are legal.
        Customisation GetCustomisation(Creature const* creature, Outfit const& outfit, uint32 race, uint32 gender) const;

    private:
        Manager() = default;

        // Fills _appearance from sCharSectionsStore. The store is loaded by the core before the
        // WorldScript::OnStartup hook that calls this, so no DBC loading is needed here.
        void LoadAppearanceOptions();

        static uint32 AppearanceKey(uint32 race, uint32 gender) { return (race << 1) | (gender ? 1u : 0u); }

        Settings _settings;
        // creature entry -> its outfit variants, ascending by variant and never empty; a creature
        // without a variant row keeps the single-element vector built from its outfit row.
        std::unordered_map<uint32, std::vector<Outfit>> _outfits;
        std::unordered_map<uint32, AppearanceTable> _appearance;
    };
}

// ---------------------------------------------------------------------------
// Scripts. Each is instantiated exactly once by Addmod_creature_gearScripts().
// ---------------------------------------------------------------------------

// Configuration and outfit storage (phase A/B/C). WorldScript.h:53 / :24.
class CreatureGearWorldScript : public WorldScript
{
public:
    CreatureGearWorldScript();
    void OnAfterConfigLoad(bool reload) override;
    // Loads the outfit table. It cannot happen during OnAfterConfigLoad at startup: that hook runs
    // from World::LoadConfigSettings (World.cpp:302) *before* ObjectMgr::LoadItemTemplates
    // (World.cpp:530), so every GetItemTemplate would return nullptr. OnStartup runs from
    // worldserver/Main.cpp:398, after SetInitialWorldSettings, so item templates exist.
    void OnStartup() override;
};

// Phase A: re-roll the worn weapon pool on respawn/entry into the world. Both hooks call the same
// gated re-roll: OnCreatureAddWorld covers a creature entering the world, and OnCreatureSelectLevel
// covers the respawn path, which in the core's default compatibility mode re-runs SelectLevel()
// (Creature.cpp:2108) without ever calling AddToWorld again.
class CreatureGearWeaponScript : public AllCreatureScript
{
public:
    CreatureGearWeaponScript();
    void OnCreatureAddWorld(Creature* creature) override;
    void OnCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature) override;
};

// Phase B, hook 1: creatures that have a loot template, injected before the loot is finalised.
class CreatureGearLootScript : public MiscScript
{
public:
    CreatureGearLootScript();
    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* tab, LootStore const& store, Player* lootOwner,
                                   bool personal, bool noEmptyError, uint16 lootMode) override;
};

// Phase B, hook 2: creatures without a loot template (lootid == 0), which hook 1 never sees.
class CreatureGearDeathScript : public UnitScript
{
public:
    CreatureGearDeathScript();
    void OnUnitDeath(Unit* unit, Unit* killer) override;
};

// Phase C: give an outfitted creature a player-race model and the mirror-image flag. Both hooks run
// the same application: OnCreatureSelectLevel covers creation (Creature::Create -> UpdateEntry ->
// SelectLevel, Creature.cpp:1683/:631), respawns (the compatibility respawn path re-runs
// SelectLevel, Creature.cpp:2108) and entry switches, while OnCreatureAddWorld covers the creature
// actually entering the world. The per-entry record below, not the hook order, is what keeps the
// two hooks in agreement.
class CreatureGearAppearanceScript : public AllCreatureScript
{
public:
    CreatureGearAppearanceScript();
    void OnCreatureAddWorld(Creature* creature) override;
    void OnCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature) override;

private:
    void ApplyAppearance(Creature* creature);
};

// Phase C: answer CMSG_GET_MIRRORIMAGE_DATA with our own eleven-slot outfit.
class CreatureGearPacketScript : public ServerScript
{
public:
    CreatureGearPacketScript();
    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override;
};

#endif  // MOD_CREATURE_GEAR_COMMON_H
