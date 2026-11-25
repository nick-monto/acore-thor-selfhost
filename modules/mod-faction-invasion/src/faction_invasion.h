/*
 * mod-faction-invasion - shared declarations.
 *
 * A random open-world event: a themed faction (Scourge, Burning Legion,
 * Scarlet Crusade, ...) spawns a 15-18 mob wave in the zone a player is
 * already in. Mobs are the player's level +/-2, with a 20% elite budget capped
 * at three. Killing the whole wave summons one owner-only reward chest whose
 * contents scale with the wave's difficulty and elite count. A short-lived
 * faction ground effect marks the area.
 *
 * The module is data-driven: the theme roster, level-band creature pool, area
 * anchors, ground spells, loot pools and money are rows in
 * mod_faction_invasion_* tables under data/sql/world/base. The SQL is generated
 * by scripts/faction-invasion/gen_invasion.py except for the hand-written
 * schema and the chest/carrier templates.
 */

#ifndef MOD_FACTION_INVASION_H
#define MOD_FACTION_INVASION_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

class Creature;
class Loot;
class LootStore;
class Map;
class Player;
class Unit;
struct CreatureStatScale;

namespace FactionInvasion
{
    struct Config
    {
        bool Enable                  = true;
        uint32_t CheckIntervalSec     = 60;
        uint32_t MinPlayers           = 1;
        uint32_t ChancePercent        = 5;
        uint32_t CooldownMinutes      = 15;
        uint32_t DurationMinutes      = 10;
        uint32_t ChestEntry           = 5000100;
        uint32_t GroundCarrierEntry   = 15214;   // Invisible Stalker; flags/faction set at spawn
        uint32_t ChestLifetimeMinutes = 30;
        uint32_t WinnerGraceMinutes   = 10;
        bool Announce                 = true;
        bool Debug                    = false;
    };

    Config& GetConfig();
    void LoadConfig();

    // World thread, once after the module SQL has been applied.
    void LoadData();
    // World thread, at startup: creates the per-character chest table.
    void EnsureSchema();

    // WorldScript::OnUpdate tick. Selects a start/timeout and sweeps; the map
    // thread performs the actual spawn/despawn in OnMapUpdate.
    void Update(uint32 diff);

    // AllMapScript::OnMapUpdate: the map-thread half of the event. Consumes a
    // pending start or stop, and deletes expired chests.
    void OnMapUpdate(Map* map, uint32 diff);

    // AllCreatureScript::OnCreatureCalculateStatScale: consumes the one-shot
    // pending scale a spawn request left for this creature.
    void OnCreatureCalculateStatScale(Creature* creature, CreatureStatScale& scale);

    // Unit death: kills counted per real player, gold set after the core's
    // generateMoneyLoot, chest spawned when the wave is empty.
    void OnUnitDeath(Unit* unit, Unit* killer);

    // MiscScript::OnAfterLootTemplateProcess: replaces the stock mob loot with
    // the generated pool for this bracket and difficulty.
    void OnAfterLootTemplateProcess(Loot* loot, LootStore const& store, Player* lootOwner);

    // GlobalScript::OnAllowedToLootContainerCheck, inverted: true = deny.
    bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source);

    // PlayerScript::OnPlayerLogin / OnPlayerUpdateZone.
    void OnPlayerLogin(Player* player);
    void OnPlayerUpdateZone(Player* player, uint32 newZone);

    // PlayerScript::OnPlayerCanUseChat for the INVS addon prefix.
    bool HandleAddonChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver);

    // WorldScript sweep: expire chest rows, re-summon persisted chests.
    void SweepExpired();
    void EnsureChestsForMap(Player* player);

    // GM command surface.
    bool StartEvent(Player* player, bool force, std::string& message);
    bool StopEvent(std::string& message);
    std::string Status();
}

#endif // MOD_FACTION_INVASION_H
