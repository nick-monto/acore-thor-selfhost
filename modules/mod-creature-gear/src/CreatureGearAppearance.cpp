/*
 * This file is part of mod-creature-gear.
 *
 * Phase C: the eleven-slot armour outfit over the mirror-image path.
 *
 * A creature with a row in mod_creature_gear_outfit is morphed onto the player-race model of the
 * outfit's race and flagged UNIT_FLAG2_MIRROR_IMAGE, which makes the stock 3.3.5a client ask for
 * CMSG_GET_MIRRORIMAGE_DATA; CreatureGearPacketScript answers with our own SMSG_MIRRORIMAGE_DATA
 * carrying the eleven display ids instead of the core's clone-caster handler (which would find no
 * SPELL_AURA_CLONE_CASTER and send nothing). No core patch, no client data.
 */

#include "creature_gear_common.h"

#include "Creature.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    // What ApplyAppearance resolved for one creature entry. The packet path must not derive any of
    // this from the creature again: by the time the client asks, the creature's display *is* the
    // player-race model, and ChrRaces.model_m/model_f carry ExtendedDisplayInfoID == 0
    // (CreatureDisplayInfoExtra.dbc has no row 0), so the display -> CreatureDisplayInfoExtra ->
    // DisplayRaceID walk that produced this record stops resolving for the very creature it was used
    // on. The five customisation bytes are *not* here: they may differ between two spawns of the same
    // entry, so Manager::GetCustomisation resolves them per creature from the spawn id.
    struct ResolvedAppearance
    {
        uint32 model = 0;       // the player-race display id now shown; 0 = "nothing resolved"
        uint32 race = 0;
        uint32 gender = 0;
        uint32 klass = 0;
    };

    // creature entry -> resolved appearance. An entry present with model == 0 is the cached negative
    // verdict ("no display of this entry resolves to a player race"), so neither the walk below nor
    // its log line is repeated for that entry. Only the world thread writes and reads it, so it
    // needs no lock.
    std::unordered_map<uint32, ResolvedAppearance> _resolvedAppearances;

    // ChrRaces.model_m / model_f for a race and gender, or 0 when the race has no ChrRaces row or no
    // model for that gender.
    uint32 ModelForRace(uint32 race, uint32 gender)
    {
        ChrRacesEntry const* chrRaces = sChrRacesStore.LookupEntry(race);
        if (!chrRaces)
            return 0;

        return gender ? chrRaces->model_f : chrRaces->model_m;
    }

    // Resolve the player-race model the creature is to be shown with, plus (by reference) the race
    // byte the client must be told. Returns 0 when nothing resolves, so the caller leaves the
    // creature alone.
    //
    // outfit->race != 0 pins the race from the outfit row: honour it and skip the search entirely.
    // Otherwise the creature's current display is tried first - it is the one the spawn actually
    // rolled - and then every other display the template carries, in the ascending order
    // ObjectMgr::LoadCreatureTemplateModels stored them (CreatureTemplate::Models, CreatureData.h:192,
    // loaded with `ORDER BY Idx ASC` at ObjectMgr.cpp:716). The order is fixed, so the choice is
    // deterministic even for a template with several player-race displays. The first display that
    // maps to a race with a model for the outfit's gender wins.
    uint32 ResolveRaceAndModel(Creature* creature, CreatureGear::Outfit const* outfit, uint32& race)
    {
        if (outfit->race)
        {
            race = outfit->race;
            return ModelForRace(race, outfit->gender);
        }

        uint32 const currentDisplay = creature->GetDisplayId();

        // Unit::GetDisplayRaceFromModelId (Unit.h:842, Unit.cpp:16704) is the core's own
        // CreatureDisplayInfo -> CreatureDisplayInfoExtra.DisplayRaceID lookup.
        race = static_cast<uint32>(creature->GetDisplayRaceFromModelId(currentDisplay));
        if (race)
        {
            if (uint32 model = ModelForRace(race, outfit->gender))
                return model;
        }

        for (CreatureModel const& candidate : creature->GetCreatureTemplate()->Models)
        {
            if (candidate.CreatureDisplayID == currentDisplay)
                continue;   // already tried above

            race = static_cast<uint32>(creature->GetDisplayRaceFromModelId(candidate.CreatureDisplayID));
            if (race)
            {
                if (uint32 model = ModelForRace(race, outfit->gender))
                    return model;
            }
        }

        race = 0;
        return 0;
    }
}

CreatureGearAppearanceScript::CreatureGearAppearanceScript() : AllCreatureScript("mod-creature-gear-appearance")
{
}

void CreatureGearAppearanceScript::ApplyAppearance(Creature* creature)
{
    CreatureGear::Manager* manager = CreatureGear::Manager::instance();
    if (!manager->Enabled() || !manager->GetSettings().appearanceEnabled)
        return;

    CreatureGear::Outfit const* outfit = manager->GetOutfit(creature);
    if (!outfit)
        return;

    uint32 const entry = creature->GetEntry();

    auto cached = _resolvedAppearances.find(entry);
    if (cached == _resolvedAppearances.end())
    {
        uint32 race = 0;
        uint32 model = ResolveRaceAndModel(creature, outfit, race);
        if (!model)
        {
            // Remember the failure so the walk and this line happen once per entry, not once per add.
            _resolvedAppearances.emplace(entry, ResolvedAppearance{});

            if (outfit->race)
                LOG_DEBUG("module", "mod-creature-gear: creature entry {} outfit race {} gender {} has no ChrRaces model, keeping the stock model",
                          entry, outfit->race, outfit->gender);
            else
                LOG_DEBUG("module", "mod-creature-gear: creature entry {} has no display resolving to a player race, keeping the stock model",
                          entry);
            return;
        }

        ResolvedAppearance appearance;
        appearance.model = model;
        appearance.race = race;
        appearance.gender = outfit->gender;
        appearance.klass = outfit->klass ? outfit->klass : creature->GetCreatureTemplate()->unit_class;

        cached = _resolvedAppearances.emplace(entry, appearance).first;
    }

    ResolvedAppearance const& appearance = cached->second;
    if (!appearance.model)
        return;

    // Only the two display fields are skipped when the creature already shows the race model (which
    // happens for outfit rows that pin Race/Gender): the flag is what makes the client ask, so it is
    // set whenever a model resolved.
    if (creature->GetDisplayId() != appearance.model)
    {
        creature->SetDisplayId(appearance.model);
        creature->SetNativeDisplayId(appearance.model);
    }

    creature->SetUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE);

    if (manager->GetSettings().debug)
    {
        // The bytes are resolved per creature and do not touch the object, so logging them here is
        // also the only place a run can show that two spawns of one entry got different faces. The
        // armour variant comes from the same spawn-id seed, and the five item ids it carries are
        // what a corpse of this spawn point yields, so they are logged with it.
        CreatureGear::Manager::Customisation const customisation =
            manager->GetCustomisation(creature, *outfit, appearance.race, appearance.gender);

        LOG_DEBUG("module", "mod-creature-gear: creature entry {} spawn {} now shows player-race model {} (race {} gender {}), skin {} face {} hair {} colour {} facial hair {}",
                  entry, creature->GetSpawnId(), appearance.model, appearance.race, appearance.gender,
                  customisation.skin, customisation.face, customisation.hairStyle, customisation.hairColor, customisation.facialHair);

        LOG_DEBUG("module", "mod-creature-gear: creature entry {} spawn {} wears outfit variant {}/{}: head {} chest {} legs {} feet {} hands {}",
                  entry, creature->GetSpawnId(),
                  manager->OutfitVariantIndex(creature) + 1, manager->OutfitVariantCount(entry),
                  outfit->itemEntry[CreatureGear::SLOT_HEAD], outfit->itemEntry[CreatureGear::SLOT_CHEST],
                  outfit->itemEntry[CreatureGear::SLOT_LEGS], outfit->itemEntry[CreatureGear::SLOT_FEET],
                  outfit->itemEntry[CreatureGear::SLOT_HANDS]);
    }
}

void CreatureGearAppearanceScript::OnCreatureAddWorld(Creature* creature)
{
    ApplyAppearance(creature);
}

// The core reaches a creature's appearance through SelectLevel() on every path but the first entry
// into the world: creation runs it from UpdateEntry (Creature.cpp:1683 -> :631), a
// compatibility-mode respawn re-runs it (Creature.cpp:2108) and then resets the display from
// GetNativeDisplayId() (Creature.cpp:2116) - which is the model this module pinned as native, so
// the reset agrees with us - and an entry switch runs it from UpdateEntry as well. The display is
// already the spawn's own model by then (set in Creature::Create, Creature.cpp:1225-1231), so the
// resolution is the one OnCreatureAddWorld would have made; the record cache is what turns the
// second call into a no-op instead of a re-resolution.
void CreatureGearAppearanceScript::OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature)
{
    ApplyAppearance(creature);
}

CreatureGearPacketScript::CreatureGearPacketScript()
    : ServerScript("mod-creature-gear-packet", { SERVERHOOK_CAN_PACKET_RECEIVE })
{
}

bool CreatureGearPacketScript::CanPacketReceive(WorldSession* session, WorldPacket const& packet)
{
    if (packet.GetOpcode() != CMSG_GET_MIRRORIMAGE_DATA)
        return true;

    CreatureGear::Manager* manager = CreatureGear::Manager::instance();
    if (!manager->Enabled() || !manager->GetSettings().appearanceEnabled)
        return true;

    Player* player = session->GetPlayer();
    if (!player)
        return true;

    // The client asks for the guid it sees carrying UNIT_FLAG2_MIRROR_IMAGE.
    WorldPacket pkt(packet);
    ObjectGuid guid;
    pkt >> guid;

    Unit* unit = ObjectAccessor::GetUnit(*player, guid);
    if (!unit)
        return true;

    Creature* creature = unit->ToCreature();
    if (!creature)
        return true;

    // Answer only for creatures whose appearance the add-world path actually applied, and only from
    // what it recorded then: the race cannot be re-derived from the creature here, because that path
    // has already replaced the display with a player-race model. Anything else falls through to the
    // core handler, which sends nothing without a SPELL_AURA_CLONE_CASTER.
    auto cached = _resolvedAppearances.find(creature->GetEntry());
    if (cached == _resolvedAppearances.end() || !cached->second.model)
        return true;

    ResolvedAppearance const& appearance = cached->second;

    // Answer only while the creature actually shows the recorded model: an entry switched under us
    // (a bare SetEntry by a transform or entry-swap script) would otherwise get this entry's gear
    // over a foreign body. The pinned creature_model_info leaves DisplayID_Other_Gender = 0 on every
    // ChrRaces model, so the core's respawn gender randomiser cannot move the display away from it.
    if (creature->GetDisplayId() != appearance.model)
        return true;

    // The eleven slot ids are the outfit's payload; the record above carries the appearance bytes
    // this handler must not recompute.
    CreatureGear::Outfit const* outfit = manager->GetOutfit(creature);
    if (!outfit)
        return true;

    // Same field order and widths as WorldSession::HandleMirrorImageDataRequest (SpellHandler.cpp:768-834):
    // guid(8) + display(u32) + race/gender/class(3x u8) + skin/face/hair/haircolor/facialhair(5x u8)
    // + guild(u32) + 11 display ids(u32). 8 + 4 + 3 + 5 + 4 + 44 = 68.
    CreatureGear::Manager::Customisation const customisation =
        manager->GetCustomisation(creature, *outfit, appearance.race, appearance.gender);

    WorldPacket data(SMSG_MIRRORIMAGE_DATA, 68);
    data << guid;
    data << uint32(appearance.model);
    data << uint8(appearance.race);
    data << uint8(appearance.gender);
    data << uint8(appearance.klass);
    data << uint8(customisation.skin);
    data << uint8(customisation.face);
    data << uint8(customisation.hairStyle);
    data << uint8(customisation.hairColor);
    data << uint8(customisation.facialHair);
    data << uint32(0);
    for (uint8 slot = CreatureGear::SLOT_HEAD; slot < CreatureGear::OUTFIT_SLOTS; ++slot)
        data << uint32(outfit->displayId[slot]);

    session->SendPacket(&data);
    return false;
}
