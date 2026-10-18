/*
 * This file is part of mod-creature-gear.
 *
 * Configuration, outfit storage (phases B and C) and phase A weapon re-rolling.
 */

#include "creature_gear_common.h"

#include <algorithm>
#include <iterator>     // std::size on the VARIANT_SLOTS array

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
// sCharSectionsStore: the pinned client data's customisation table (race, sex, section -> the
// variation/colour pairs the client can render). Loaded by the core before OnStartup.
#include "DBCStores.h"
#include "ItemTemplate.h"
// DatabaseEnv.h only forward-declares ResultSet/Field (DatabaseEnvFwd.h); the concrete types
// used by the outfit loader come from QueryResult.h (which itself includes Field.h).
#include "QueryResult.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Unit.h"

namespace
{
    // Item entries above this id are absent from the server's Item.dbc (DBCStores.cpp:655),
    // so a client would never render them.
    constexpr uint32 MaxItemEntry = 56806;

    // Deterministic field mixer for the per-spawn customisation bytes: the morph path and the packet
    // path must derive the same bytes for the same creature, so this is a pure function of the spawn
    // id - no RNG state, no cache, nothing to keep in sync.
    uint32 MixAppearanceSeed(uint32 seed)
    {
        seed ^= seed >> 16;
        seed *= 0x7feb352du;
        seed ^= seed >> 15;
        seed *= 0x846ca68bu;
        seed ^= seed >> 16;
        return seed;
    }

    // Distinct VariationIndex / ColorIndex counts for one CharSections section. Startup-time logging
    // only, so the linear scans are fine.
    void CountDistinct(std::vector<CreatureGear::AppearanceVariant> const& rows, uint32& variations, uint32& colors)
    {
        std::vector<uint32> seenVariations;
        std::vector<uint32> seenColors;

        for (CreatureGear::AppearanceVariant const& row : rows)
        {
            if (std::find(seenVariations.begin(), seenVariations.end(), row.variation) == seenVariations.end())
                seenVariations.push_back(row.variation);

            if (std::find(seenColors.begin(), seenColors.end(), row.color) == seenColors.end())
                seenColors.push_back(row.color);
        }

        variations = uint32(seenVariations.size());
        colors = uint32(seenColors.size());
    }

    // The slots `mod_creature_gear_outfit_variant` overrides, in the table's column order. Mirrors
    // VARIED_OUTFIT_SLOTS in scripts/creature-gear/gen_creature_gear.py.
    constexpr uint32 VARIANT_SLOTS[] = {
        CreatureGear::SLOT_HEAD,
        CreatureGear::SLOT_CHEST,
        CreatureGear::SLOT_LEGS,
        CreatureGear::SLOT_FEET,
        CreatureGear::SLOT_HANDS,
    };

    // Resolves one source column into the item entry and display id the outfit carries. A missing
    // item leaves the slot empty and says so once per row.
    void ResolveOutfitSlot(CreatureGear::Outfit& outfit, uint32 slot, uint32 itemEntry)
    {
        outfit.itemEntry[slot] = 0;
        outfit.displayId[slot] = 0;

        if (!itemEntry)
            return;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemEntry);
        if (!item)
        {
            LOG_ERROR("sql.sql", "mod-creature-gear: outfit for creature {} slot {} ({}) references non-existing item {}, slot left empty",
                outfit.creatureId, slot, CreatureGear::OutfitSlotName(slot), itemEntry);
            return;
        }

        outfit.itemEntry[slot] = itemEntry;
        outfit.displayId[slot] = item->DisplayInfoID;
    }

    bool IsHandInventoryType(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_WEAPON:
            case INVTYPE_SHIELD:
            case INVTYPE_RANGED:
            case INVTYPE_2HWEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
            case INVTYPE_HOLDABLE:
            case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT:
                return true;
            default:
                return false;
        }
    }

    // Re-rolls the worn weapon pool for creatures the module owns data for. Both the
    // OnCreatureAddWorld and OnCreatureSelectLevel hooks route through here: on first creation the
    // creature enters the world (AddToWorld) and SelectLevel runs, and on respawn in the core's
    // default compatibility mode only SelectLevel runs again, so a double roll on creation is
    // possible and harmless (the last one wins).
    void RerollWeaponsIfNeeded(Creature* creature)
    {
        if (!creature)
            return;

        if (CreatureGear::Manager::instance()->ShouldRerollWeapons(creature))
            creature->LoadEquipment(-1);
    }
}

namespace CreatureGear
{
    uint32 OutfitSlotFromInventoryType(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD:
                return SLOT_HEAD;
            case INVTYPE_SHOULDERS:
                return SLOT_SHOULDERS;
            case INVTYPE_BODY:
                return SLOT_BODY;
            case INVTYPE_CHEST:
                return SLOT_CHEST;
            case INVTYPE_ROBE:
                return SLOT_CHEST;
            case INVTYPE_WAIST:
                return SLOT_WAIST;
            case INVTYPE_LEGS:
                return SLOT_LEGS;
            case INVTYPE_FEET:
                return SLOT_FEET;
            case INVTYPE_WRISTS:
                return SLOT_WRISTS;
            case INVTYPE_HANDS:
                return SLOT_HANDS;
            case INVTYPE_CLOAK:
                return SLOT_BACK;
            case INVTYPE_TABARD:
                return SLOT_TABARD;
            default:
                return OUTFIT_SLOTS;
        }
    }

    char const* OutfitSlotName(uint32 slot)
    {
        switch (slot)
        {
            case SLOT_HEAD:
                return "head";
            case SLOT_SHOULDERS:
                return "shoulders";
            case SLOT_BODY:
                return "body";
            case SLOT_CHEST:
                return "chest";
            case SLOT_WAIST:
                return "waist";
            case SLOT_LEGS:
                return "legs";
            case SLOT_FEET:
                return "feet";
            case SLOT_WRISTS:
                return "wrists";
            case SLOT_HANDS:
                return "hands";
            case SLOT_BACK:
                return "back";
            case SLOT_TABARD:
                return "tabard";
            default:
                return "?";
        }
    }

    Manager* Manager::instance()
    {
        static Manager instance;
        return &instance;
    }

    void Manager::LoadConfig(bool reload)
    {
        _settings.enabled = sConfigMgr->GetOption<bool>("CreatureGear.Enable", true);
        _settings.debug = sConfigMgr->GetOption<bool>("CreatureGear.Debug", false);
        _settings.weaponsRerollOnRespawn = sConfigMgr->GetOption<bool>("CreatureGear.Weapons.RerollOnRespawn", true);

        _settings.dropEnabled = sConfigMgr->GetOption<bool>("CreatureGear.Drop.Enable", true);
        _settings.dropChance = sConfigMgr->GetOption<float>("CreatureGear.Drop.Chance", 5.0f);
        _settings.dropMaxQuality = sConfigMgr->GetOption<uint32>("CreatureGear.Drop.MaxQuality", 2);
        _settings.dropRequireBoE = sConfigMgr->GetOption<bool>("CreatureGear.Drop.RequireBindOnEquip", true);
        _settings.dropIncludeNoLootTemplate = sConfigMgr->GetOption<bool>("CreatureGear.Drop.IncludeNoLootTemplate", true);
        _settings.dropMaxItems = sConfigMgr->GetOption<uint32>("CreatureGear.Drop.MaxItemsPerCorpse", 1);
        _settings.dropLevelMargin = sConfigMgr->GetOption<uint32>("CreatureGear.Drop.LevelMargin", 3);

        _settings.appearanceEnabled = sConfigMgr->GetOption<bool>("CreatureGear.Appearance.Enable", true);
        _settings.appearanceVaryPerSpawn = sConfigMgr->GetOption<bool>("CreatureGear.Appearance.VaryPerSpawn", true);

        if (_settings.debug)
            LOG_DEBUG("module", "mod-creature-gear: effective settings (reload: {}) enabled={} appearance={} varyPerSpawn={} rerollWeapons={} drops={} chance={} maxQuality={} requireBoE={} includeNoLootTemplate={} maxItems={} levelMargin={} debug={}",
                reload, _settings.enabled, _settings.appearanceEnabled, _settings.appearanceVaryPerSpawn, _settings.weaponsRerollOnRespawn,
                _settings.dropEnabled, _settings.dropChance, _settings.dropMaxQuality, _settings.dropRequireBoE,
                _settings.dropIncludeNoLootTemplate, _settings.dropMaxItems, _settings.dropLevelMargin, _settings.debug);
    }

    void Manager::LoadOutfits()
    {
        // The option tables come from client data rather than the database, but both loaders run from
        // the same hooks (startup and `.reload config`), so they are kept together.
        LoadAppearanceOptions();

        _outfits.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT `CreatureID`, `Race`, `Gender`, `Class`, `Skin`, `Face`, `HairStyle`, `HairColor`, `FacialHair`, "
            "`ItemEntry1`, `ItemEntry2`, `ItemEntry3`, `ItemEntry4`, `ItemEntry5`, `ItemEntry6`, `ItemEntry7`, `ItemEntry8`, `ItemEntry9`, `ItemEntry10`, `ItemEntry11` "
            "FROM `mod_creature_gear_outfit`");

        size_t outfitRows = 0;

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();

                Outfit outfit;
                outfit.creatureId = fields[0].Get<uint32>();
                outfit.race = fields[1].Get<uint32>();
                outfit.gender = fields[2].Get<uint32>();
                outfit.klass = fields[3].Get<uint32>();
                outfit.skin = fields[4].Get<uint32>();
                outfit.face = fields[5].Get<uint32>();
                outfit.hairStyle = fields[6].Get<uint32>();
                outfit.hairColor = fields[7].Get<uint32>();
                outfit.facialHair = fields[8].Get<uint32>();

                for (uint32 slot = 0; slot < OUTFIT_SLOTS; ++slot)
                    ResolveOutfitSlot(outfit, slot, fields[9 + slot].Get<uint32>());

                _outfits[outfit.creatureId].push_back(outfit);
                ++outfitRows;
            } while (result->NextRow());
        }

        // Armour variants: one row per (entry, variant) overriding the five slots in VARIANT_SLOTS.
        // The variant rows replace the single set built from the outfit row above, so an entry's
        // variant count is exactly the number of rows it has here; an entry without any keeps the
        // outfit row as its one and only set.
        size_t variantRows = 0;
        QueryResult variants = WorldDatabase.Query(
            "SELECT `CreatureID`, `Variant`, `Head`, `Chest`, `Legs`, `Feet`, `Hands` "
            "FROM `mod_creature_gear_outfit_variant` ORDER BY `CreatureID`, `Variant`");

        if (variants)
        {
            do
            {
                Field* fields = variants->Fetch();
                uint32 creatureId = fields[0].Get<uint32>();
                uint32 variant = fields[1].Get<uint32>();

                auto itr = _outfits.find(creatureId);
                if (itr == _outfits.end())
                {
                    LOG_ERROR("sql.sql", "mod-creature-gear: outfit variant {} of creature entry {} has no `mod_creature_gear_outfit` row, variant skipped",
                        variant, creatureId);
                    continue;
                }

                // Variant 1 is the outfit row itself, so the first row replaces it rather than
                // stacking on top: from here the entry has as many sets as the table has rows.
                Outfit variantOutfit = itr->second.front();
                if (variant == 1)
                    itr->second.clear();

                for (uint32 column = 0; column < std::size(VARIANT_SLOTS); ++column)
                    ResolveOutfitSlot(variantOutfit, VARIANT_SLOTS[column], fields[2 + column].Get<uint32>());

                itr->second.push_back(variantOutfit);
                ++variantRows;
            } while (variants->NextRow());
        }

        if (_settings.debug)
            LOG_DEBUG("module", "mod-creature-gear: loaded {} outfit row(s) and {} variant row(s) for {} creature(s)",
                outfitRows, variantRows, OutfitCount());
    }

    void Manager::LoadAppearanceOptions()
    {
        _appearance.clear();

        for (uint32 i = 0; i < sCharSectionsStore.GetNumRows(); ++i)
        {
            CharSectionsEntry const* row = sCharSectionsStore.LookupEntry(i);
            if (!row)
                continue;

            AppearanceTable& table = _appearance[AppearanceKey(row->RaceID, row->SexID)];
            AppearanceVariant const variant{row->VariationIndex, row->ColorIndex};

            // SECTION_TYPE_UNDERWEAR and anything beyond it are never sent by this module. Flags are
            // deliberately ignored: they gate the character creator (SECTION_FLAG_PLAYER,
            // SECTION_FLAG_DEATH_KNIGHT), while a mirror image is rendered from the ids alone.
            switch (row->BaseSection)
            {
                case SECTION_TYPE_SKIN:
                    table.baseSkin.push_back(variant);
                    break;
                case SECTION_TYPE_FACE:
                    table.face.push_back(variant);
                    break;
                case SECTION_TYPE_FACIAL_HAIR:
                    table.facialHair.push_back(variant);
                    break;
                case SECTION_TYPE_HAIR:
                    table.hair.push_back(variant);
                    break;
                default:
                    break;
            }
        }

        if (!_settings.debug)
            return;

        // One line per race/gender pair: the counts are the answer to "how much variation does the
        // pinned client data actually offer?", and they are what the sampler below draws from.
        LOG_DEBUG("module", "mod-creature-gear: appearance options for {} race/gender pair(s) from CharSections.dbc", _appearance.size());

        for (auto const& [key, table] : _appearance)
        {
            uint32 faceCount = 0, faceColors = 0, hairCount = 0, hairColors = 0, facialHairCount = 0, facialHairColors = 0,
                   skinCount = 0, skinColors = 0;

            CountDistinct(table.face, faceCount, faceColors);
            CountDistinct(table.hair, hairCount, hairColors);
            CountDistinct(table.facialHair, facialHairCount, facialHairColors);
            CountDistinct(table.baseSkin, skinCount, skinColors);

            LOG_DEBUG("module", "mod-creature-gear: race {} gender {}: {} skin colour(s), {} face(s), {} hair style(s), {} hair colour(s), {} facial-hair style(s)",
                key >> 1, key & 1u, skinColors, faceCount, hairCount, hairColors, facialHairCount);
        }
    }

    Manager::Customisation Manager::GetCustomisation(Creature const* creature, Outfit const& outfit, uint32 race, uint32 gender) const
    {
        Customisation customisation;
        customisation.skin = outfit.skin;
        customisation.face = outfit.face;
        customisation.hairStyle = outfit.hairStyle;
        customisation.hairColor = outfit.hairColor;
        customisation.facialHair = outfit.facialHair;

        if (!_settings.enabled || !_settings.appearanceEnabled || !_settings.appearanceVaryPerSpawn || !creature)
            return customisation;

        auto itr = _appearance.find(AppearanceKey(race, gender));
        if (itr == _appearance.end())
            return customisation;

        AppearanceTable const& table = itr->second;

        // The spawn id makes one spawn point keep one face across restarts and respawns, while
        // different spawn points of the same entry differ. Summoned and otherwise spawn-less
        // creatures fall back to their GUID.
        uint32 seed = uint32(creature->GetSpawnId() ? creature->GetSpawnId() : creature->GetGUID().GetCounter());

        // Every field advances the seed before drawing, so the five choices are independent.
        auto nextSeed = [&seed]()
        {
            seed = MixAppearanceSeed(seed + 0x9e3779b9u);
            return seed;
        };

        auto pickAny = [](std::vector<AppearanceVariant> const& rows, uint32 index) -> AppearanceVariant const*
        {
            return rows.empty() ? nullptr : &rows[index % rows.size()];
        };

        // The index-th row carrying this colour, without allocating: section 1 (face) is not a full
        // cross product, so the skin colour and the face index have to come from one real row.
        auto pickForColor = [](std::vector<AppearanceVariant> const& rows, uint32 color, uint32 index) -> AppearanceVariant const*
        {
            uint32 matching = 0;
            for (AppearanceVariant const& row : rows)
                if (row.color == color)
                    ++matching;

            if (!matching)
                return nullptr;

            uint32 wanted = index % matching;
            for (AppearanceVariant const& row : rows)
                if (row.color == color && wanted-- == 0)
                    return &row;

            return nullptr;
        };

        if (!customisation.skin)
            if (AppearanceVariant const* variant = pickAny(table.baseSkin, nextSeed()))
                customisation.skin = variant->color;

        if (!customisation.face)
        {
            AppearanceVariant const* variant = pickForColor(table.face, customisation.skin, nextSeed());
            if (!variant)
                variant = pickAny(table.face, nextSeed());

            if (variant)
                customisation.face = variant->variation;
        }

        // Style and colour come from one row, so the pair is one the client can render.
        if (!customisation.hairStyle || !customisation.hairColor)
        {
            if (AppearanceVariant const* variant = pickAny(table.hair, nextSeed()))
            {
                if (!customisation.hairStyle)
                    customisation.hairStyle = variant->variation;

                if (!customisation.hairColor)
                    customisation.hairColor = variant->color;
            }
        }

        // Facial hair follows the hair colour; races whose data has no facial-hair rows keep 0.
        if (!customisation.facialHair)
            if (AppearanceVariant const* variant = pickForColor(table.facialHair, customisation.hairColor, nextSeed()))
                customisation.facialHair = variant->variation;

        return customisation;
    }

    // The spawn id makes one spawn point keep one set of armour across restarts and respawns, while
    // different spawn points of the same entry differ. Summoned and otherwise spawn-less creatures
    // fall back to their GUID, exactly like the appearance sampler (GetCustomisation).
    uint32 Manager::OutfitVariantIndex(Creature const* creature) const
    {
        if (!creature)
            return 0;

        auto itr = _outfits.find(creature->GetEntry());
        if (itr == _outfits.end() || itr->second.empty())
            return 0;

        uint32 seed = uint32(creature->GetSpawnId() ? creature->GetSpawnId() : creature->GetGUID().GetCounter());
        return seed % uint32(itr->second.size());
    }

    size_t Manager::OutfitVariantCount(uint32 creatureEntry) const
    {
        auto itr = _outfits.find(creatureEntry);
        return itr != _outfits.end() ? itr->second.size() : 0;
    }

    bool Manager::HasOutfit(uint32 creatureEntry) const
    {
        return OutfitVariantCount(creatureEntry) != 0;
    }

    Outfit const* Manager::GetOutfit(Creature const* creature) const
    {
        if (!creature)
            return nullptr;

        auto itr = _outfits.find(creature->GetEntry());
        if (itr == _outfits.end() || itr->second.empty())
            return nullptr;

        return &itr->second[OutfitVariantIndex(creature) % itr->second.size()];
    }

    std::vector<uint32> Manager::GetDropCandidates(Creature const* creature) const
    {
        std::vector<uint32> candidates;

        if (!_settings.enabled || !_settings.dropEnabled || !creature)
            return candidates;

        for (uint8 slot = 0; slot < MAX_EQUIPMENT_ITEMS; ++slot)
        {
            uint32 itemEntry = creature->GetVirtualItemId(slot);
            if (itemEntry)
                candidates.push_back(itemEntry);
        }

        if (Outfit const* outfit = GetOutfit(creature))
        {
            for (uint32 itemEntry : outfit->itemEntry)
                if (itemEntry)
                    candidates.push_back(itemEntry);
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        std::vector<uint32> eligible;
        eligible.reserve(candidates.size());

        for (uint32 itemEntry : candidates)
            if (IsDropEligible(creature, itemEntry))
                eligible.push_back(itemEntry);

        return eligible;
    }

    bool Manager::IsDropEligible(Creature const* creature, uint32 itemEntry) const
    {
        if (!_settings.enabled || !_settings.dropEnabled)
            return false;

        if (!creature || !itemEntry || itemEntry > MaxItemEntry)
            return false;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemEntry);
        if (!item)
            return false;

        if (!IsHandInventoryType(item->InventoryType) && OutfitSlotFromInventoryType(item->InventoryType) == OUTFIT_SLOTS)
            return false;

        if (_settings.dropRequireBoE && item->Bonding != BIND_WHEN_EQUIPPED)
            return false;

        if (item->Quality > _settings.dropMaxQuality)
            return false;

        if (item->RequiredLevel > creature->GetLevel() + _settings.dropLevelMargin)
            return false;

        return true;
    }

    bool Manager::ShouldRerollWeapons(Creature const* creature) const
    {
        return _settings.enabled
            && _settings.weaponsRerollOnRespawn
            && creature
            && HasOutfit(creature->GetEntry());
    }
}

CreatureGearWorldScript::CreatureGearWorldScript()
    : WorldScript("mod-creature-gear", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP })
{
}

void CreatureGearWorldScript::OnAfterConfigLoad(bool reload)
{
    CreatureGear::Manager::instance()->LoadConfig(reload);

    // At startup this hook runs before ObjectMgr::LoadItemTemplates, so the outfit table can only be
    // read once item templates exist: from OnStartup. A live `.reload config` re-enters here after
    // that point, so reloading the table picks up newly applied SQL.
    if (reload)
        CreatureGear::Manager::instance()->LoadOutfits();
}

void CreatureGearWorldScript::OnStartup()
{
    CreatureGear::Manager::instance()->LoadOutfits();
}

CreatureGearWeaponScript::CreatureGearWeaponScript()
    : AllCreatureScript("mod-creature-gear-weapons")
{
}

void CreatureGearWeaponScript::OnCreatureAddWorld(Creature* creature)
{
    RerollWeaponsIfNeeded(creature);
}

void CreatureGearWeaponScript::OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature)
{
    RerollWeaponsIfNeeded(creature);
}
