/*
 * mod-faction-invasion - implementation.
 *
 * One event at a time. The world tick picks a real player, their zone and one
 * of the curated anchors, then spawns a 15-18 mob wave in a ring around that
 * anchor. Each mob gets its level and stat multipliers through the core's
 * OnCreatureCalculateStatScale hook, so it is really that level (hit tables,
 * avoidance, armour DR) without changing the creature entry. Elites use a
 * generated rank-1 clone (base + 700000), so the client's dragon frame is
 * correct; rank 1 itself adds no stats because Rate.Creature.* is 1 on this
 * realm, so the elite toughness is the scale multipliers.
 *
 * Loot is replaced in MiscScript::OnAfterLootTemplateProcess, gold is written
 * in UnitScript::OnUnitDeath (after the core's generateMoneyLoot), and the
 * chest is one owner-only GameObject filled by hand. Access is the inverted
 * GlobalScript::OnAllowedToLootContainerCheck; the winner is the real player
 * with the most killing blows, ties broken by who reached that count first,
 * then by low GUID.
 *
 * The add-on protocol is INVS over the same LANG_ADDON whisper carrier
 * mod-survivor and mod-creature-affixes use. The start line carries the
 * zone's WorldMapArea bounds, so the client can place the circle and the
 * minimap arrow without shipping a DBC table.
 */

#include "faction_invasion.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* kAddonPrefix = "INVS";
    constexpr uint32 kMaxBracket = 16;
    constexpr uint32 kMaxElites = 3;
    constexpr uint32 kMobMin = 15;
    constexpr uint32 kMobMax = 18;

    FactionInvasion::Config _config;

    struct Theme
    {
        std::string key;
        std::string name;
        std::string faction;
        uint32 markerSpell = 0;
        std::string modifierStat;
        float modifierAmount = 0.0f;
        uint32 weight = 1;
    };

    struct CreatureRow
    {
        uint32 baseEntry = 0;
        uint32 eliteEntry = 0;
        uint8 minLevel = 1;
        uint8 maxLevel = 80;
        uint32 weight = 1;
    };

    struct Area
    {
        uint32 id = 0;
        uint32 zoneId = 0;
        uint32 mapId = 0;
        std::string name;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 120.0f;
        uint8 minLevel = 1;
        uint8 maxLevel = 80;
        uint32 weight = 1;
    };

    struct GroundRow
    {
        uint32 id = 0;
        std::string themeKey;
        uint32 spellId = 0;
        std::string pattern;
        uint8 patchCount = 1;
        float radius = 10.0f;
        uint32 weight = 1;
    };

    struct LootRow
    {
        uint8 bracket = 1;
        uint8 difficulty = 1;
        std::string kind;
        uint8 rollOrder = 0;
        std::string quality;
        uint32 itemId = 0;
        uint8 rolls = 1;
        float chance = 100.0f;
    };

    struct MoneyRow
    {
        uint32 mobCopper = 0;
        uint32 chestCopper = 0;
    };

    struct MobTag
    {
        uint8 bracket = 1;
        uint8 difficulty = 1;
        bool elite = false;
    };

    struct Contribution
    {
        uint32 kills = 0;
        uint32 lastKillMs = 0;
    };

    struct Event
    {
        uint64 id = 0;
        uint32 zoneId = 0;
        uint32 mapId = 0;
        int32 instanceId = 0;
        Map* map = nullptr;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 120.0f;
        std::string themeKey;
        std::string themeName;
        std::string faction;
        uint32 startedMs = 0;
        uint32 endsMs = 0;
        uint8 totalMobs = 0;
        uint8 totalElites = 0;
        uint8 difficulty = 1;
        uint8 bracket = 1;
        uint32 triggerGuid = 0;
        bool spawning = false;
        std::unordered_set<uint64> alive;
        std::unordered_map<uint64, MobTag> tags;
        std::unordered_map<uint32, Contribution> contribution;
    };

    struct ChestInfo
    {
        uint32 dbId = 0;
        uint32 winnerGuid = 0;
        uint32 moneyCopper = 0;
        uint8 bracket = 1;
        uint8 difficulty = 1;
        uint32 expiresAt = 0;
        bool paid = false;
        bool openToAll = false;
    };

    struct PendingStart
    {
        uint32 playerGuid = 0;
        uint32 zoneId = 0;
        uint32 mapId = 0;
        int32 instanceId = 0;
        uint32 requestedMs = 0;
    };

    struct PendingScale
    {
        uint32 entry = 0;
        float x = 0.0f;
        float y = 0.0f;
        CreatureStatScale scale;
    };

    thread_local std::optional<PendingScale> g_pendingScale;

    std::mutex _mutex;
    std::optional<Event> _event;
    std::optional<PendingStart> _pendingStart;
    bool _pendingStop = false;
    std::atomic<uint32> _nextChestId{0};
    std::vector<std::pair<uint32, uint64>> _expiredChests; // map id -> gameobject raw GUID
    std::unordered_map<uint64, ChestInfo> _chests;        // gameobject raw GUID -> info
    std::unordered_map<uint64, uint64> _groundCarriers;   // carrier raw GUID -> event id

    std::vector<Theme> _themes;
    std::unordered_map<std::string, std::vector<CreatureRow>> _creatures;
    std::vector<Area> _areas;
    std::unordered_map<std::string, std::vector<GroundRow>> _ground;
    std::map<std::pair<uint8, uint8>, std::vector<LootRow>> _lootMob;
    std::map<std::pair<uint8, uint8>, std::vector<LootRow>> _lootChest;
    std::map<uint8, std::map<std::string, std::vector<uint32>>> _pools;
    std::map<std::pair<uint8, uint8>, MoneyRow> _money;
    bool _dataLoaded = false;

    uint32 _nextCheckMs = 0;
    uint32 _cooldownUntilMs = 0;
    uint32 _sweepTimer = 0;

    uint32 NowMs()
    {
        return getMSTime();
    }

    uint32 NowEpoch()
    {
        return uint32(std::time(nullptr));
    }

    uint8 LevelBracket(uint8 level)
    {
        uint8 bracket = uint8((std::max<uint8>(1, std::min<uint8>(80, level)) - 1) / 5 + 1);
        return std::min<uint8>(bracket, kMaxBracket);
    }

    uint32 WeightedPick(std::vector<std::pair<uint32, uint32>> const& choices)
    {
        uint32 total = 0;
        for (auto const& choice : choices)
            total += std::max<uint32>(1, choice.second);

        if (!total)
            return 0;

        uint32 roll = urand(0, total - 1);
        for (auto const& choice : choices)
        {
            uint32 weight = std::max<uint32>(1, choice.second);
            if (roll < weight)
                return choice.first;
            roll -= weight;
        }
        return choices.back().first;
    }

    bool IsRealPlayer(Player* player)
    {
        return player && player->IsInWorld() && player->GetSession() && !player->GetSession()->IsHeadless();
    }

    float ResolveZ(Map* map, float x, float y, float hintZ)
    {
        if (!map)
            return hintZ;

        float z = map->GetHeight(PHASEMASK_NORMAL, x, y, hintZ + 50.0f, false, 100.0f);
        if (z < -10000.0f || !std::isfinite(z))
            z = hintZ;
        return z;
    }

    void SendPacket(Player* player, std::string const& body)
    {
        if (!IsRealPlayer(player))
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                     std::string(kAddonPrefix) + "\t" + body);
        player->SendDirectMessage(&data);
    }

    void SendNone(Player* player)
    {
        SendPacket(player, "1\tnone");
    }

    std::string StartBody(Event const& event)
    {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        GetZoneMapBounds(event.zoneId, left, right, top, bottom);

        uint32 remaining = event.endsMs > NowMs() ? (event.endsMs - NowMs()) / 1000 : 0;
        return Acore::StringFormat(
            "1\tstart\t{}\t{}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            event.mapId, event.zoneId, event.x, event.y, event.radius,
            left, right, top, bottom,
            event.themeName, event.faction, event.totalMobs, event.totalElites,
            event.difficulty, remaining, _config.Announce ? 1 : 0);
    }

    void SendStart(Player* player, Event const& event)
    {
        SendPacket(player, StartBody(event));
    }

    void SendCurrent(Player* player)
    {
        if (!IsRealPlayer(player))
            return;

        std::lock_guard<std::mutex> guard(_mutex);
        if (!_event || _event->zoneId != player->GetZoneId())
        {
            SendNone(player);
            return;
        }
        SendStart(player, *_event);
    }

    void BroadcastZone(uint32 zoneId, std::string const& body)
    {
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            (void)accountId;
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!IsRealPlayer(player) || player->GetZoneId() != zoneId)
                continue;
            SendPacket(player, body);
        }
    }

    void ApplyThemeModifier(CreatureStatScale& scale, Theme const& theme)
    {
        float multiplier = 1.0f + theme.modifierAmount;
        if (theme.modifierStat == "health")
            scale.health *= multiplier;
        else if (theme.modifierStat == "damage")
            scale.damage *= multiplier;
        else if (theme.modifierStat == "spell")
            scale.spellDamage *= multiplier;
        else if (theme.modifierStat == "armor")
            scale.armor *= multiplier;
    }

    uint64 SpawnMob(Map* map, Player* phaseSource, uint32 entry, float x, float y, float z,
                    uint32 durationMs, CreatureStatScale const& scale, uint32 markerSpell,
                    std::string& error)
    {
        if (!map || !entry)
        {
            error = "no map or entry";
            return 0;
        }

        float orientation = frand(0.0f, 6.28318530718f);
        g_pendingScale = PendingScale{entry, x, y, scale};
        TempSummon* summon = map->SummonCreature(entry, Position(x, y, z, orientation),
                                                 nullptr, durationMs, phaseSource);
        g_pendingScale.reset();

        if (!summon)
        {
            error = Acore::StringFormat("SummonCreature({}) returned null", entry);
            return 0;
        }

        summon->SetTempSummonType(TEMPSUMMON_TIMED_OR_DEAD_DESPAWN);
        summon->SetTimer(durationMs);
        if (markerSpell)
            summon->AddAura(markerSpell, summon);

        return summon->GetGUID().GetRawValue();
    }

    uint64 SpawnGroundCarrier(Map* map, Player* phaseSource, uint32 spellId, float x, float y, float z,
                              float radius, uint32 durationMs)
    {
        if (!_config.GroundCarrierEntry || !spellId)
            return 0;

        float angle = frand(0.0f, 6.28318530718f);
        float distance = frand(0.0f, radius);
        x += std::cos(angle) * distance;
        y += std::sin(angle) * distance;
        z = ResolveZ(map, x, y, z);

        TempSummon* carrier = map->SummonCreature(_config.GroundCarrierEntry,
                                                  Position(x, y, z, 0.0f),
                                                  nullptr, durationMs, phaseSource);
        if (!carrier)
            return 0;

        carrier->SetTempSummonType(TEMPSUMMON_TIMED_OR_DEAD_DESPAWN);
        carrier->SetTimer(durationMs);
        carrier->SetFaction(14);
        carrier->SetDisplayId(11686);
        carrier->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
        carrier->SetReactState(REACT_PASSIVE);
        carrier->SetControlled(true, UNIT_STATE_ROOT);
        carrier->CastSpell(carrier, spellId, true);

        return carrier->GetGUID().GetRawValue();
    }

    std::vector<CreatureRow const*> CreatureChoices(std::string const& themeKey, uint8 level)
    {
        std::vector<CreatureRow const*> result;
        auto it = _creatures.find(themeKey);
        if (it == _creatures.end())
            return result;

        for (CreatureRow const& row : it->second)
            if (level >= row.minLevel && level <= row.maxLevel)
                result.push_back(&row);

        return result;
    }

    Theme const* PickTheme(uint8 level)
    {
        std::vector<std::pair<uint32, uint32>> choices;
        for (uint32 i = 0; i < _themes.size(); ++i)
            if (!CreatureChoices(_themes[i].key, level).empty())
                choices.emplace_back(i, _themes[i].weight);

        if (choices.empty())
            return nullptr;

        return &_themes[WeightedPick(choices)];
    }

    Area const* PickArea(uint32 zoneId, uint32 mapId, uint8 level)
    {
        std::vector<std::pair<uint32, uint32>> choices;
        for (uint32 i = 0; i < _areas.size(); ++i)
        {
            Area const& area = _areas[i];
            if (area.zoneId == zoneId && area.mapId == mapId && level >= area.minLevel && level <= area.maxLevel)
                choices.emplace_back(i, area.weight);
        }
        if (choices.empty())
            return nullptr;
        return &_areas[WeightedPick(choices)];
    }

    GroundRow const* PickGround(std::string const& themeKey)
    {
        auto it = _ground.find(themeKey);
        if (it == _ground.end() || it->second.empty())
            return nullptr;

        std::vector<std::pair<uint32, uint32>> choices;
        for (uint32 i = 0; i < it->second.size(); ++i)
            choices.emplace_back(i, it->second[i].weight);
        return &it->second[WeightedPick(choices)];
    }

    MoneyRow MoneyFor(uint8 bracket, uint8 difficulty)
    {
        auto it = _money.find({bracket, difficulty});
        if (it != _money.end())
            return it->second;
        return {};
    }

    void AddItem(Loot& loot, uint32 itemId, uint8 count)
    {
        if (!itemId || !count)
            return;
        LootStoreItem item(itemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, count, count);
        loot.AddItem(item);
    }

    void AddGeneratedLoot(Loot& loot, uint8 bracket, uint8 difficulty, std::string const& kind)
    {
        auto const& table = kind == "chest" ? _lootChest : _lootMob;
        auto it = table.find({bracket, difficulty});
        if (it == table.end())
            return;

        auto poolIt = _pools.find(bracket);
        for (LootRow const& row : it->second)
        {
            if (frand(0.0f, 100.0f) >= row.chance)
                continue;

            if (row.quality == "fixed")
            {
                AddItem(loot, row.itemId, row.rolls);
                continue;
            }

            if (poolIt == _pools.end())
                continue;

            auto qualityIt = poolIt->second.find(row.quality);
            if (qualityIt == poolIt->second.end() || qualityIt->second.empty())
                continue;

            for (uint8 i = 0; i < row.rolls; ++i)
            {
                uint32 item = qualityIt->second[urand(0, qualityIt->second.size() - 1)];
                AddItem(loot, item, 1);
            }
        }

        if (loot.items.empty() && poolIt != _pools.end())
        {
            auto qualityIt = poolIt->second.find("common");
            if (qualityIt != poolIt->second.end() && !qualityIt->second.empty())
                AddItem(loot, qualityIt->second[urand(0, qualityIt->second.size() - 1)], 1);
        }

        loot.unlootedCount = uint8(std::min<size_t>(loot.items.size(), 255));
    }

    void FillChestLoot(GameObject* go, uint8 bracket, uint8 difficulty, ObjectGuid owner)
    {
        if (!go)
            return;

        go->loot.clear();
        go->loot.loot_type = LOOT_CORPSE;
        go->loot.sourceWorldObjectGUID = go->GetGUID();
        go->loot.sourceGameObject = go;
        go->loot.lootOwnerGUID = owner;

        AddGeneratedLoot(go->loot, bracket, difficulty, "chest");
    }

    void DespawnGround(Map* map, uint64 eventId)
    {
        std::vector<uint64> carriers;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            for (auto it = _groundCarriers.begin(); it != _groundCarriers.end();)
            {
                if (it->second == eventId)
                {
                    carriers.push_back(it->first);
                    it = _groundCarriers.erase(it);
                }
                else
                    ++it;
            }
        }

        for (uint64 raw : carriers)
        {
            ObjectGuid guid(raw);
            if (Creature* creature = map ? map->GetCreature(guid) : nullptr)
                creature->DespawnOrUnsummon();
        }
    }

    void DespawnWave(Event const& event)
    {
        for (uint64 raw : event.alive)
        {
            ObjectGuid guid(raw);
            if (Creature* creature = event.map ? event.map->GetCreature(guid) : nullptr)
                creature->DespawnOrUnsummon();
        }
        DespawnGround(event.map, event.id);
    }

    uint32 PickWinner(Event const& event)
    {
        uint32 winner = 0;
        uint32 bestKills = 0;
        uint32 bestLast = UINT32_MAX;

        for (auto const& [guid, contribution] : event.contribution)
        {
            if (contribution.kills > bestKills ||
                (contribution.kills == bestKills && contribution.lastKillMs < bestLast) ||
                (contribution.kills == bestKills && contribution.lastKillMs == bestLast && guid < winner))
            {
                winner = guid;
                bestKills = contribution.kills;
                bestLast = contribution.lastKillMs;
            }
        }

        if (!winner)
            winner = event.triggerGuid;
        return winner;
    }

    void PersistChest(GameObject* go, Event const& event, uint32 winner, uint32 moneyCopper,
                      ChestInfo& info)
    {
        uint32 created = NowEpoch();
        uint32 expires = created + _config.ChestLifetimeMinutes * 60;
        // Explicit id instead of a queued INSERT followed by LAST_INSERT_ID() on another
        // connection. The counter is seeded from the epoch in EnsureSchema, so a restart cannot
        // collide with a row the previous process wrote.
        uint32 dbId = ++_nextChestId;
        CharacterDatabase.Execute(
            "INSERT INTO `faction_invasion_chest` "
            "(`id`,`winner_guid`,`map_id`,`position_x`,`position_y`,`position_z`,`orientation`,"
            "`money_copper`,`bracket`,`difficulty`,`created_ts`,`expires_ts`,`paid`,`open_to_all`) "
            "VALUES ({},{},{},{},{},{},{},{},{},{},{},{},0,0)",
            dbId, winner, event.mapId, event.x, event.y, event.z, 0.0f, moneyCopper,
            event.bracket, event.difficulty, created, expires);

        info = ChestInfo{dbId, winner, moneyCopper, event.bracket, event.difficulty, expires, false, false};
        _chests[go->GetGUID().GetRawValue()] = info;
    }

    void FinishEvent(bool won)
    {
        Event event;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (!_event)
                return;
            event = std::move(*_event);
            _event.reset();
            _pendingStop = false;
            _cooldownUntilMs = NowMs() + _config.CooldownMinutes * 60 * 1000;
        }

        if (!won)
        {
            DespawnWave(event);
            BroadcastZone(event.zoneId, Acore::StringFormat("1\tend\ttimeout\t0\t0\t{}", _config.Announce ? 1 : 0));
            LOG_INFO("module", "mod-faction-invasion: wave {} in zone {} timed out with {} mobs alive",
                     event.id, event.zoneId, event.alive.size());
            return;
        }

        DespawnGround(event.map, event.id);

        uint32 winner = PickWinner(event);
        MoneyRow money = MoneyFor(event.bracket, event.difficulty);
        uint32 chestCopper = uint32(float(money.chestCopper) * (1.0f + 0.25f * event.totalElites));
        float z = ResolveZ(event.map, event.x, event.y, event.z);

        GameObject* chest = event.map ? event.map->SummonGameObject(_config.ChestEntry, event.x, event.y, z,
                                                                    frand(0.0f, 6.28318530718f),
                                                                    0.0f, 0.0f, 0.0f, 0.0f,
                                                                    _config.ChestLifetimeMinutes * 60)
                                      : nullptr;

        if (!chest)
        {
            LOG_ERROR("module", "mod-faction-invasion: could not summon chest {} for wave {} in map {}",
                      _config.ChestEntry, event.id, event.mapId);
            BroadcastZone(event.zoneId, Acore::StringFormat("1\tend\twin\t0\t0\t{}", _config.Announce ? 1 : 0));
            return;
        }

        ObjectGuid owner = ObjectGuid::Create<HighGuid::Player>(winner);
        chest->SetOwnerGUID(owner);
        FillChestLoot(chest, event.bracket, event.difficulty, owner);

        ChestInfo info;
        PersistChest(chest, event, winner, chestCopper, info);

        BroadcastZone(event.zoneId, Acore::StringFormat("1\tend\twin\t{}\t{}\t{}", 0, chestCopper,
                                                       _config.Announce ? 1 : 0));
        LOG_INFO("module",
                 "mod-faction-invasion: wave {} won; chest {} for player {} ({} kills, {} contributors, {} copper)",
                 event.id, chest->GetGUID().ToString(), winner,
                 event.contribution.count(winner) ? event.contribution.at(winner).kills : 0,
                 event.contribution.size(), chestCopper);
    }

    bool BuildEvent(Player* trigger, Area const& area, std::string& message)
    {
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_event)
            {
                message = "an invasion is already active";
                return false;
            }
        }

        uint8 targetLevel = uint8(std::max<int32>(1, std::min<int32>(80, int32(trigger->GetLevel()) + irand(-2, 2))));
        Theme const* theme = PickTheme(targetLevel);
        if (!theme)
        {
            message = "no theme has a creature for that level";
            return false;
        }

        std::vector<CreatureRow const*> choices = CreatureChoices(theme->key, targetLevel);
        if (choices.empty())
        {
            message = "no creature for that level";
            return false;
        }

        uint8 total = uint8(urand(kMobMin, kMobMax));
        uint8 elites = uint8(urand(0, std::min<uint32>(kMaxElites, total / 5)));
        uint8 difficulty = uint8(1 + (elites >= 1 ? 1 : 0) + (elites >= 2 ? 1 : 0));

        Event event;
        event.id = uint64(NowMs()) << 16 | uint64(urand(1, 65535));
        event.zoneId = area.zoneId;
        event.mapId = area.mapId;
        event.map = trigger->GetMap();
        event.instanceId = trigger->GetInstanceId();
        event.x = area.x;
        event.y = area.y;
        event.z = ResolveZ(event.map, area.x, area.y, area.z);
        event.radius = area.radius;
        event.themeKey = theme->key;
        event.themeName = theme->name;
        event.faction = theme->faction;
        event.startedMs = NowMs();
        event.endsMs = event.startedMs + _config.DurationMinutes * 60 * 1000;
        event.totalMobs = total;
        event.totalElites = elites;
        event.difficulty = difficulty;
        event.bracket = LevelBracket(targetLevel);
        event.triggerGuid = trigger->GetGUID().GetCounter();
        event.spawning = true;

        // Publish before the first summon: a mob that dies during the loop must find its tag and
        // the event, not fall through to stock loot. `spawning` suppresses the empty-set finish
        // until the loop has added every tag.
        {
            std::lock_guard<std::mutex> guard(_mutex);
            _event = std::move(event);
        }

        Event& active = *_event;
        uint32 durationMs = _config.DurationMinutes * 60 * 1000;
        uint32 spawned = 0;

        for (uint8 i = 0; i < total; ++i)
        {
            bool elite = i < elites;
            CreatureRow const& row = *choices[urand(0, choices.size() - 1)];
            uint32 entry = elite && row.eliteEntry ? row.eliteEntry : row.baseEntry;

            float angle = 6.28318530718f * float(i) / float(total) + frand(-0.12f, 0.12f);
            float distance = area.radius * frand(0.30f, 0.92f);
            float x = area.x + std::cos(angle) * distance;
            float y = area.y + std::sin(angle) * distance;
            float z = ResolveZ(active.map, x, y, active.z);

            CreatureStatScale scale;
            scale.level = targetLevel;
            if (elite)
            {
                scale.health = 1.9f;
                scale.damage = 1.3f;
                scale.armor = 1.2f;
                scale.spellDamage = 1.3f;
                scale.healing = 1.3f;
            }
            ApplyThemeModifier(scale, *theme);

            std::string error;
            uint64 raw = SpawnMob(active.map, trigger, entry, x, y, z, durationMs, scale,
                                  theme->markerSpell, error);
            if (!raw)
            {
                LOG_ERROR("module", "mod-faction-invasion: spawn {} ({}) failed: {}",
                          entry, elite ? "elite" : "normal", error);
                continue;
            }

            {
                std::lock_guard<std::mutex> guard(_mutex);
                active.alive.insert(raw);
                active.tags[raw] = MobTag{active.bracket, active.difficulty, elite};
            }
            ++spawned;
        }

        if (!spawned)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            _event.reset();
            message = "no mob could be spawned";
            return false;
        }

        if (GroundRow const* ground = PickGround(theme->key))
        {
            uint8 patches = ground->pattern == "cluster"
                                ? uint8(urand(2, std::max<uint32>(2, ground->patchCount)))
                                : 1;
            for (uint8 i = 0; i < patches; ++i)
            {
                uint64 raw = SpawnGroundCarrier(active.map, trigger, ground->spellId,
                                                active.x, active.y, active.z, active.radius, durationMs);
                if (!raw)
                {
                    LOG_ERROR("module", "mod-faction-invasion: ground carrier for spell {} failed",
                              ground->spellId);
                    continue;
                }
                std::lock_guard<std::mutex> guard(_mutex);
                _groundCarriers[raw] = active.id;
            }
        }

        bool allDead = false;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            active.spawning = false;
            active.totalMobs = uint8(spawned);
            active.totalElites = uint8(std::min<uint32>(elites, spawned));
            allDead = active.alive.empty();
        }

        if (allDead)
        {
            FinishEvent(true);
            message = Acore::StringFormat("invasion in {} ended during the spawn", area.name);
            return true;
        }

        BroadcastZone(active.zoneId, StartBody(active));

        LOG_INFO("module",
                 "mod-faction-invasion: wave {} started in {} (zone {}, map {}) theme={} mobs={} elites={} difficulty={} level={}",
                 active.id, area.name, active.zoneId, active.mapId, active.themeKey,
                 active.totalMobs, active.totalElites, active.difficulty, targetLevel);

        message = Acore::StringFormat("invasion started in {}: {} ({} mobs, {} elites)",
                                      area.name, theme->name, spawned, active.totalElites);
        return true;
    }

    // -------------------------------------------------------------------
    // GM surface
    // -------------------------------------------------------------------

    std::string Status()
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (!_event)
            return "no active invasion";

        Event const& event = *_event;
        uint32 remaining = event.endsMs > NowMs() ? (event.endsMs - NowMs()) / 1000 : 0;
        return Acore::StringFormat("{} in zone {} (map {}): {} mobs, {} elites, {} alive, {}s left",
                                   event.themeName, event.zoneId, event.mapId,
                                   event.totalMobs, event.totalElites, event.alive.size(), remaining);
    }
}

namespace FactionInvasion
{
    Config& GetConfig()
    {
        return _config;
    }

    void LoadConfig()
    {
        _config.Enable = sConfigMgr->GetOption<bool>("FactionInvasion.Enable", true);
        _config.CheckIntervalSec = sConfigMgr->GetOption<uint32>("FactionInvasion.CheckIntervalSec", 60);
        _config.MinPlayers = sConfigMgr->GetOption<uint32>("FactionInvasion.MinPlayers", 1);
        _config.ChancePercent = sConfigMgr->GetOption<uint32>("FactionInvasion.ChancePercent", 5);
        _config.CooldownMinutes = sConfigMgr->GetOption<uint32>("FactionInvasion.CooldownMinutes", 15);
        _config.DurationMinutes = sConfigMgr->GetOption<uint32>("FactionInvasion.DurationMinutes", 10);
        _config.ChestEntry = sConfigMgr->GetOption<uint32>("FactionInvasion.ChestEntry", 5000100);
        _config.GroundCarrierEntry = sConfigMgr->GetOption<uint32>("FactionInvasion.GroundCarrierEntry", 15214);
        _config.ChestLifetimeMinutes = sConfigMgr->GetOption<uint32>("FactionInvasion.ChestLifetimeMinutes", 30);
        _config.WinnerGraceMinutes = sConfigMgr->GetOption<uint32>("FactionInvasion.WinnerGraceMinutes", 10);
        _config.Announce = sConfigMgr->GetOption<bool>("FactionInvasion.Announce", true);
        _config.Debug = sConfigMgr->GetOption<bool>("FactionInvasion.Debug", false);
    }

    void LoadData()
    {
        _themes.clear();
        _creatures.clear();
        _areas.clear();
        _ground.clear();
        _lootMob.clear();
        _lootChest.clear();
        _pools.clear();
        _money.clear();
        _dataLoaded = false;

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `theme_key`,`name`,`faction`,`marker_spell`,`modifier_stat`,`modifier_amount`,`weight` "
                "FROM `mod_faction_invasion_theme`"))
        {
            do
            {
                Field* fields = result->Fetch();
                Theme theme;
                theme.key = fields[0].Get<std::string>();
                theme.name = fields[1].Get<std::string>();
                theme.faction = fields[2].Get<std::string>();
                theme.markerSpell = fields[3].Get<uint32>();
                theme.modifierStat = fields[4].Get<std::string>();
                theme.modifierAmount = fields[5].Get<float>();
                theme.weight = fields[6].Get<uint32>();
                _themes.push_back(std::move(theme));
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `theme_key`,`base_entry`,`elite_entry`,`min_level`,`max_level`,`weight` "
                "FROM `mod_faction_invasion_creature`"))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string themeKey = fields[0].Get<std::string>();
                CreatureRow row;
                row.baseEntry = fields[1].Get<uint32>();
                row.eliteEntry = fields[2].Get<uint32>();
                row.minLevel = fields[3].Get<uint8>();
                row.maxLevel = fields[4].Get<uint8>();
                row.weight = fields[5].Get<uint32>();
                _creatures[themeKey].push_back(row);
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `id`,`zone_id`,`map_id`,`name`,`x`,`y`,`z`,`radius`,`min_level`,`max_level`,`weight` "
                "FROM `mod_faction_invasion_area`"))
        {
            do
            {
                Field* fields = result->Fetch();
                Area area;
                area.id = fields[0].Get<uint32>();
                area.zoneId = fields[1].Get<uint32>();
                area.mapId = fields[2].Get<uint32>();
                area.name = fields[3].Get<std::string>();
                area.x = fields[4].Get<float>();
                area.y = fields[5].Get<float>();
                area.z = fields[6].Get<float>();
                area.radius = fields[7].Get<float>();
                area.minLevel = fields[8].Get<uint8>();
                area.maxLevel = fields[9].Get<uint8>();
                area.weight = fields[10].Get<uint32>();
                _areas.push_back(std::move(area));
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `id`,`theme_key`,`spell_id`,`pattern`,`patch_count`,`radius`,`weight` "
                "FROM `mod_faction_invasion_ground`"))
        {
            do
            {
                Field* fields = result->Fetch();
                GroundRow row;
                row.id = fields[0].Get<uint32>();
                row.themeKey = fields[1].Get<std::string>();
                row.spellId = fields[2].Get<uint32>();
                row.pattern = fields[3].Get<std::string>();
                row.patchCount = fields[4].Get<uint8>();
                row.radius = fields[5].Get<float>();
                row.weight = fields[6].Get<uint32>();
                _ground[row.themeKey].push_back(row);
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `bracket`,`quality`,`item_id` FROM `mod_faction_invasion_pool`"))
        {
            do
            {
                Field* fields = result->Fetch();
                _pools[fields[0].Get<uint8>()][fields[1].Get<std::string>()].push_back(fields[2].Get<uint32>());
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `bracket`,`difficulty`,`kind`,`roll_order`,`quality`,`item_id`,`rolls`,`chance` "
                "FROM `mod_faction_invasion_loot` ORDER BY `bracket`,`difficulty`,`kind`,`roll_order`"))
        {
            do
            {
                Field* fields = result->Fetch();
                LootRow row;
                row.bracket = fields[0].Get<uint8>();
                row.difficulty = fields[1].Get<uint8>();
                row.kind = fields[2].Get<std::string>();
                row.rollOrder = fields[3].Get<uint8>();
                row.quality = fields[4].Get<std::string>();
                row.itemId = fields[5].Get<uint32>();
                row.rolls = fields[6].Get<uint8>();
                row.chance = fields[7].Get<float>();
                (row.kind == "chest" ? _lootChest : _lootMob)[{row.bracket, row.difficulty}].push_back(row);
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query(
                "SELECT `bracket`,`difficulty`,`mob_copper`,`chest_copper` FROM `mod_faction_invasion_money`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 bracket = fields[0].Get<uint8>();
                uint8 difficulty = fields[1].Get<uint8>();
                _money[{bracket, difficulty}] = MoneyRow{fields[2].Get<uint32>(), fields[3].Get<uint32>()};
            } while (result->NextRow());
        }

        _dataLoaded = !_themes.empty() && !_creatures.empty() && !_areas.empty();
        if (!_dataLoaded)
            LOG_ERROR("module", "mod-faction-invasion: definition tables are empty; the event is disabled");
        else
            LOG_INFO("module", "mod-faction-invasion: loaded {} themes, {} areas, {} creature-theme entries, {} bracket pools",
                     _themes.size(), _areas.size(), _creatures.size(), _pools.size());
    }

    void EnsureSchema()
    {
        _nextChestId.store(NowEpoch());
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `faction_invasion_chest` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`winner_guid` INT UNSIGNED NOT NULL,"
            "`map_id` INT UNSIGNED NOT NULL,"
            "`position_x` FLOAT NOT NULL,"
            "`position_y` FLOAT NOT NULL,"
            "`position_z` FLOAT NOT NULL,"
            "`orientation` FLOAT NOT NULL DEFAULT 0,"
            "`money_copper` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`bracket` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`difficulty` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`created_ts` INT UNSIGNED NOT NULL,"
            "`expires_ts` INT UNSIGNED NOT NULL,"
            "`paid` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`open_to_all` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`),"
            "KEY `map_expiry` (`map_id`,`expires_ts`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    }

    void OnCreatureCalculateStatScale(Creature* creature, CreatureStatScale& scale)
    {
        if (!creature || !g_pendingScale)
            return;

        // A database spawn has a spawn id; the creature a summon request is about to create has
        // none. Without this a grid load of an existing camp inside EnsureGridLoaded could consume
        // the pending scale before Map::SummonCreature creates our mob.
        if (creature->GetSpawnId() != 0 || creature->IsPet() || creature->IsGuardian() || creature->IsTotem())
            return;

        PendingScale const& pending = *g_pendingScale;
        if (creature->GetEntry() != pending.entry)
            return;

        float dx = creature->GetPositionX() - pending.x;
        float dy = creature->GetPositionY() - pending.y;
        if (dx * dx + dy * dy > 100.0f)
            return;

        scale = pending.scale;
        g_pendingScale.reset();
    }

    bool StartEvent(Player* player, bool force, std::string& message)
    {
        if (!_config.Enable)
        {
            message = "mod-faction-invasion is disabled";
            return false;
        }

        if (!_dataLoaded)
        {
            message = "definition tables are empty";
            return false;
        }

        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_event)
            {
                message = "an invasion is already active";
                return false;
            }
        }

        Player* trigger = nullptr;
        Area const* area = nullptr;

        if (force && player)
        {
            trigger = player;
            area = PickArea(player->GetZoneId(), player->GetMapId(), uint8(player->GetLevel()));
            if (!area)
            {
                for (Area const& candidate : _areas)
                    if (candidate.zoneId == player->GetZoneId() && candidate.mapId == player->GetMapId())
                    {
                        area = &candidate;
                        break;
                    }
            }
        }
        else
        {
            struct Candidate
            {
                Player* player;
                Area const* area;
            };
            std::vector<Candidate> candidates;
            for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
            {
                (void)accountId;
                Player* candidate = session ? session->GetPlayer() : nullptr;
                if (!IsRealPlayer(candidate))
                    continue;
                if (Area const* candidateArea = PickArea(candidate->GetZoneId(), candidate->GetMapId(),
                                                         uint8(candidate->GetLevel())))
                    candidates.push_back({candidate, candidateArea});
            }

            if (candidates.empty())
            {
                message = "no player is in a supported zone";
                return false;
            }

            if (candidates.size() < _config.MinPlayers)
            {
                message = Acore::StringFormat("not enough players in a supported zone ({}/{})",
                                              candidates.size(), _config.MinPlayers);
                return false;
            }

            Candidate const& chosen = candidates[urand(0, candidates.size() - 1)];
            trigger = chosen.player;
            area = chosen.area;
        }

        if (!trigger || !area)
        {
            message = "the player's zone has no invasion anchor";
            return false;
        }

        // The world tick must not touch a Map: summoning, despawning and GameObject deletion all
        // belong to the map thread. Hand the chosen player to OnMapUpdate, which runs there.
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_event || _pendingStart)
            {
                message = "an invasion is already active";
                return false;
            }
            _pendingStart = PendingStart{trigger->GetGUID().GetCounter(), area->zoneId,
                                         area->mapId, trigger->GetInstanceId(), NowMs()};
        }

        message = Acore::StringFormat("invasion is starting in {}", area->name);
        return true;
    }

    void OnMapUpdate(Map* map, uint32 /*diff*/)
    {
        if (!map)
            return;

        std::optional<PendingStart> start;
        bool stop = false;
        std::vector<uint64> expired;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_pendingStart && _pendingStart->mapId == map->GetId() &&
                _pendingStart->instanceId == map->GetInstanceId())
            {
                start = _pendingStart;
                _pendingStart.reset();
            }

            if (_pendingStop)
            {
                if (_event && _event->mapId == map->GetId() &&
                    _event->instanceId == map->GetInstanceId())
                {
                    _pendingStop = false;
                    stop = true;
                }
                else if (!_event)
                    _pendingStop = false;
            }

            for (auto it = _expiredChests.begin(); it != _expiredChests.end();)
            {
                if (it->first == map->GetId())
                {
                    expired.push_back(it->second);
                    it = _expiredChests.erase(it);
                }
                else
                    ++it;
            }
        }

        for (uint64 raw : expired)
        {
            ObjectGuid guid(raw);
            if (GameObject* go = map->GetGameObject(guid))
                go->Delete();
            std::lock_guard<std::mutex> guard(_mutex);
            _chests.erase(raw);
        }

        if (stop)
        {
            FinishEvent(false);
            return;
        }

        if (!start)
            return;

        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(start->playerGuid));
        if (!player || player->GetMap() != map || player->GetZoneId() != start->zoneId)
        {
            LOG_DEBUG("module", "mod-faction-invasion: pending start dropped: player {} left zone {} map {}",
                      start->playerGuid, start->zoneId, start->mapId);
            return;
        }

        Area const* area = PickArea(player->GetZoneId(), player->GetMapId(), uint8(player->GetLevel()));
        if (!area)
        {
            for (Area const& candidate : _areas)
                if (candidate.zoneId == player->GetZoneId() && candidate.mapId == player->GetMapId())
                {
                    area = &candidate;
                    break;
                }
        }
        if (!area)
            return;

        std::string message;
        if (!BuildEvent(player, *area, message))
            LOG_DEBUG("module", "mod-faction-invasion: start failed: {}", message);
    }

    bool StopEvent(std::string& message)
    {
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_pendingStart && !_event)
            {
                _pendingStart.reset();
                message = "invasion start cancelled";
                return true;
            }
            if (!_event)
            {
                message = "no active invasion";
                return false;
            }
            _pendingStop = true;
        }

        message = "invasion stop requested";
        return true;
    }

    void Update(uint32 diff)
    {
        if (!_config.Enable || !_dataLoaded)
            return;

        uint32 now = NowMs();

        _sweepTimer += diff;
        if (_sweepTimer >= 60000)
        {
            _sweepTimer = 0;
            SweepExpired();
        }

        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_pendingStop)
            {
                if (!_event)
                    _pendingStop = false;
                return;
            }
            if (_pendingStart)
            {
                // A start queued for a map that unloaded before its update must not wedge the
                // module forever; the map-thread hand-off is a tick, not a lease.
                if (now - _pendingStart->requestedMs < 5000)
                    return;
                _pendingStart.reset();
            }
            if (_event)
            {
                if (now >= _event->endsMs)
                    _pendingStop = true;
                return;
            }
        }

        if (now < _nextCheckMs || now < _cooldownUntilMs)
            return;

        _nextCheckMs = now + _config.CheckIntervalSec * 1000;
        if (!_config.ChancePercent || urand(0, 99) >= _config.ChancePercent)
            return;

        std::string message;
        if (!StartEvent(nullptr, false, message) && _config.Debug)
            LOG_DEBUG("module", "mod-faction-invasion: random start skipped: {}", message);
    }

    void OnUnitDeath(Unit* unit, Unit* killer)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return;

        uint64 raw = creature->GetGUID().GetRawValue();
        MobTag tag;
        bool finish = false;
        uint32 zoneId = 0;
        uint64 eventId = 0;

        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (!_event || _event->tags.find(raw) == _event->tags.end())
                return;

            tag = _event->tags[raw];
            _event->alive.erase(raw);
            zoneId = _event->zoneId;
            eventId = _event->id;
            finish = _event->alive.empty() && !_event->spawning;
        }

        Player* credit = nullptr;
        if (killer)
            credit = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!credit)
            credit = creature->GetLootRecipient();

        if (IsRealPlayer(credit) && tag.bracket)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_event && _event->id == eventId)
            {
                Contribution& contribution = _event->contribution[credit->GetGUID().GetCounter()];
                ++contribution.kills;
                contribution.lastKillMs = NowMs();
            }
        }

        MoneyRow money = MoneyFor(tag.bracket, tag.difficulty);
        if (money.mobCopper)
            creature->loot.gold = money.mobCopper;

        if (_config.Debug)
            LOG_DEBUG("module", "mod-faction-invasion: mob {} ({}) died in zone {}; {} alive",
                      raw, tag.elite ? "elite" : "normal", zoneId, finish ? 0 : 1);

        if (finish)
            FinishEvent(true);
    }

    void OnAfterLootTemplateProcess(Loot* loot, LootStore const& store, Player* lootOwner)
    {
        if (!loot || &store != &LootTemplates_Creature)
            return;

        MobTag tag;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (!_event)
                return;
            auto it = _event->tags.find(loot->sourceWorldObjectGUID.GetRawValue());
            if (it == _event->tags.end())
                return;
            tag = it->second;
        }

        loot->clear();
        loot->loot_type = LOOT_CORPSE;
        if (lootOwner)
            loot->lootOwnerGUID = lootOwner->GetGUID();

        AddGeneratedLoot(*loot, tag.bracket, tag.difficulty, "mob");
    }

    bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source)
    {
        if (!player || !source.IsGameObject())
            return false;

        ChestInfo info;
        bool found = false;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            auto it = _chests.find(source.GetRawValue());
            if (it != _chests.end())
            {
                info = it->second;
                found = true;
            }
        }

        if (!found)
            return false;

        if (info.expiresAt <= NowEpoch())
        {
            // Do not delete a GameObject from inside the loot gate; queue it for the map thread,
            // leave the _chests entry in place so every open keeps being denied until it is gone.
            std::lock_guard<std::mutex> guard(_mutex);
            bool queued = false;
            for (auto const& [mapId, raw] : _expiredChests)
            {
                (void)mapId;
                if (raw == source.GetRawValue())
                {
                    queued = true;
                    break;
                }
            }
            if (!queued)
                _expiredChests.emplace_back(player->GetMapId(), source.GetRawValue());
            return true;
        }

        if (!info.openToAll && player->GetGUID().GetCounter() != info.winnerGuid)
            return true;

        if (!info.paid)
        {
            Player* mutablePlayer = const_cast<Player*>(player);
            if (info.moneyCopper)
                mutablePlayer->ModifyMoney(int32(info.moneyCopper));

            {
                std::lock_guard<std::mutex> guard(_mutex);
                auto it = _chests.find(source.GetRawValue());
                if (it != _chests.end())
                    it->second.paid = true;
            }

            if (info.dbId)
                CharacterDatabase.Execute("UPDATE `faction_invasion_chest` SET `paid` = 1 WHERE `id` = {}",
                                          info.dbId);
        }

        return false;
    }

    void OnPlayerLogin(Player* player)
    {
        // Login runs on the world thread; the map work waits for OnPlayerUpdateZone.
        SendCurrent(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone)
    {
        if (!player)
            return;

        if (newZone)
            SendCurrent(player);
        EnsureChestsForMap(player);
    }

    bool HandleAddonChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver)
    {
        if (!player || player != receiver || language != LANG_ADDON || type != CHAT_MSG_WHISPER)
            return true;

        if (msg.compare(0, 5, "INVS\t") != 0)
            return true;

        if (msg == "INVS\thello")
        {
            SendCurrent(player);
            return false;
        }

        return true;
    }

    void EnsureChestsForMap(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;

        uint32 now = NowEpoch();
        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`,`winner_guid`,`map_id`,`position_x`,`position_y`,`position_z`,`orientation`,"
            "`money_copper`,`bracket`,`difficulty`,`expires_ts`,`paid`,`open_to_all` "
            "FROM `faction_invasion_chest` WHERE `map_id` = {} AND `expires_ts` > {}",
            player->GetMapId(), now);

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            ChestInfo info;
            info.dbId = fields[0].Get<uint32>();
            info.winnerGuid = fields[1].Get<uint32>();
            uint32 mapId = fields[2].Get<uint32>();
            float x = fields[3].Get<float>();
            float y = fields[4].Get<float>();
            float z = fields[5].Get<float>();
            float o = fields[6].Get<float>();
            info.moneyCopper = fields[7].Get<uint32>();
            info.bracket = fields[8].Get<uint8>();
            info.difficulty = fields[9].Get<uint8>();
            info.expiresAt = fields[10].Get<uint32>();
            info.paid = fields[11].Get<uint8>() != 0;
            info.openToAll = fields[12].Get<uint8>() != 0;

            if (mapId != player->GetMapId())
                continue;

            bool live = false;
            {
                std::lock_guard<std::mutex> guard(_mutex);
                for (auto const& [raw, chest] : _chests)
                {
                    (void)raw;
                    if (chest.dbId == info.dbId)
                    {
                        live = true;
                        break;
                    }
                }
            }
            if (live)
                continue;

            Map* map = player->GetMap();
            GameObject* go = map->SummonGameObject(_config.ChestEntry, x, y, z, o, 0.0f, 0.0f, 0.0f, 0.0f,
                                                   _config.ChestLifetimeMinutes * 60);
            if (!go)
                continue;

            ObjectGuid owner = ObjectGuid::Create<HighGuid::Player>(info.winnerGuid);
            go->SetOwnerGUID(owner);
            FillChestLoot(go, info.bracket, info.difficulty, owner);

            std::lock_guard<std::mutex> guard(_mutex);
            _chests[go->GetGUID().GetRawValue()] = info;
        } while (result->NextRow());
    }

    void SweepExpired()
    {
        uint32 now = NowEpoch();

        if (_config.WinnerGraceMinutes)
        {
            QueryResult grace = CharacterDatabase.Query(
                "SELECT `id`,`winner_guid` FROM `faction_invasion_chest` "
                "WHERE `paid` = 0 AND `open_to_all` = 0 AND `created_ts` + {} <= {}",
                _config.WinnerGraceMinutes * 60, now);
            if (grace)
            {
                do
                {
                    Field* fields = grace->Fetch();
                    uint32 id = fields[0].Get<uint32>();
                    uint32 winner = fields[1].Get<uint32>();
                    Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(winner));
                    if (!player)
                    {
                        CharacterDatabase.Execute(
                            "UPDATE `faction_invasion_chest` SET `open_to_all` = 1 WHERE `id` = {}", id);
                        std::lock_guard<std::mutex> guard(_mutex);
                        for (auto& [raw, chest] : _chests)
                        {
                            (void)raw;
                            if (chest.dbId == id)
                                chest.openToAll = true;
                        }
                    }
                } while (grace->NextRow());
            }
        }

        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`,`map_id` FROM `faction_invasion_chest` WHERE `expires_ts` <= {}", now);
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            uint32 id = fields[0].Get<uint32>();
            uint32 mapId = fields[1].Get<uint32>();

            {
                std::lock_guard<std::mutex> guard(_mutex);
                for (auto const& [raw, chest] : _chests)
                {
                    if (chest.dbId == id)
                    {
                        _expiredChests.emplace_back(mapId, raw);
                        break;
                    }
                }
            }

            // The GameObject belongs to its map thread; OnMapUpdate deletes it and erases the
            // in-memory entry. Deleting it here would touch Map from the world thread.
            CharacterDatabase.Execute("DELETE FROM `faction_invasion_chest` WHERE `id` = {}", id);
        } while (result->NextRow());
    }
}

// ---------------------------------------------------------------------------
// Script registration
// ---------------------------------------------------------------------------

namespace
{
    class FactionInvasionWorldScript : public WorldScript
    {
    public:
        FactionInvasionWorldScript()
            : WorldScript("mod-faction-invasion-world",
                          { WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                            WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
                            WORLDHOOK_ON_STARTUP,
                            WORLDHOOK_ON_UPDATE })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            FactionInvasion::LoadConfig();
        }

        void OnBeforeWorldInitialized() override
        {
            FactionInvasion::LoadData();
        }

        void OnStartup() override
        {
            FactionInvasion::EnsureSchema();
            FactionInvasion::SweepExpired();
        }

        void OnUpdate(uint32 diff) override
        {
            FactionInvasion::Update(diff);
        }
    };

    class FactionInvasionMapScript : public AllMapScript
    {
    public:
        FactionInvasionMapScript() : AllMapScript("mod-faction-invasion-map") { }

        void OnMapUpdate(Map* map, uint32 diff) override
        {
            FactionInvasion::OnMapUpdate(map, diff);
        }
    };

    class FactionInvasionCreatureScript : public AllCreatureScript
    {
    public:
        FactionInvasionCreatureScript() : AllCreatureScript("mod-faction-invasion-creature") { }

        void OnCreatureCalculateStatScale(Creature* creature, CreatureStatScale& scale) override
        {
            FactionInvasion::OnCreatureCalculateStatScale(creature, scale);
        }
    };

    class FactionInvasionUnitScript : public UnitScript
    {
    public:
        FactionInvasionUnitScript()
            : UnitScript("mod-faction-invasion-unit", true, { UNITHOOK_ON_UNIT_DEATH })
        {
        }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            FactionInvasion::OnUnitDeath(unit, killer);
        }
    };

    class FactionInvasionLootScript : public MiscScript
    {
    public:
        FactionInvasionLootScript()
            : MiscScript("mod-faction-invasion-loot", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS })
        {
        }

        void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/, LootStore const& store,
                                        Player* lootOwner, bool /*personal*/, bool /*noEmptyError*/,
                                        uint16 /*lootMode*/) override
        {
            FactionInvasion::OnAfterLootTemplateProcess(loot, store, lootOwner);
        }
    };

    class FactionInvasionPlayerScript : public PlayerScript
    {
    public:
        FactionInvasionPlayerScript() : PlayerScript("mod-faction-invasion-player") { }

        void OnPlayerLogin(Player* player) override
        {
            FactionInvasion::OnPlayerLogin(player);
        }

        void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
        {
            FactionInvasion::OnPlayerUpdateZone(player, newZone);
        }

        [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                              Player* receiver) override
        {
            return FactionInvasion::HandleAddonChat(player, type, language, msg, receiver);
        }
    };

    class FactionInvasionGlobalScript : public GlobalScript
    {
    public:
        FactionInvasionGlobalScript() : GlobalScript("mod-faction-invasion-global") { }

        bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source) override
        {
            return FactionInvasion::OnAllowedToLootContainerCheck(player, source);
        }
    };

    class FactionInvasionGameObjectScript : public AllGameObjectScript
    {
    public:
        FactionInvasionGameObjectScript() : AllGameObjectScript("mod-faction-invasion-gameobject") { }

        void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* /*unit*/) override
        {
            if (!go || state != GO_JUST_DEACTIVATED)
                return;
            if (!go->loot.isLooted())
                return;

            uint64 raw = go->GetGUID().GetRawValue();
            uint32 dbId = 0;
            {
                std::lock_guard<std::mutex> guard(_mutex);
                auto it = _chests.find(raw);
                if (it == _chests.end())
                    return;
                dbId = it->second.dbId;
                _chests.erase(it);
            }

            if (dbId)
                CharacterDatabase.Execute("DELETE FROM `faction_invasion_chest` WHERE `id` = {}", dbId);
        }
    };

    class FactionInvasionCommandScript : public CommandScript
    {
    public:
        FactionInvasionCommandScript() : CommandScript("faction_invasion_commandscript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable invasionTable =
            {
                { "start",  HandleStart,  SEC_GAMEMASTER, Console::No },
                { "stop",   HandleStop,   SEC_GAMEMASTER, Console::No },
                { "status", HandleStatus, SEC_PLAYER,     Console::No },
            };

            static ChatCommandTable commandTable =
            {
                { "invasion", invasionTable },
            };

            return commandTable;
        }

        static bool HandleStart(ChatHandler* handler)
        {
            Player* player = handler->GetPlayer();
            if (!player)
                return false;

            std::string message;
            if (!FactionInvasion::StartEvent(player, true, message))
            {
                handler->SendSysMessage(message);
                handler->SetSentErrorMessage(true);
                return false;
            }

            handler->SendSysMessage(message);
            return true;
        }

        static bool HandleStop(ChatHandler* handler)
        {
            std::string message;
            if (!FactionInvasion::StopEvent(message))
            {
                handler->SendSysMessage(message);
                handler->SetSentErrorMessage(true);
                return false;
            }

            handler->SendSysMessage(message);
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            handler->SendSysMessage(FactionInvasion::Status());
            return true;
        }
    };
}

void AddFactionInvasionScripts()
{
    new FactionInvasionWorldScript();
    new FactionInvasionMapScript();
    new FactionInvasionCreatureScript();
    new FactionInvasionUnitScript();
    new FactionInvasionLootScript();
    new FactionInvasionPlayerScript();
    new FactionInvasionGlobalScript();
    new FactionInvasionGameObjectScript();
    new FactionInvasionCommandScript();
}
