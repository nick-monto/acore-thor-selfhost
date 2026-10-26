/*
 * mod-pawn: the roster, the offers and the party.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnMgr.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PawnArchetype.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SharedDefines.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace
{
    uint8 const kClasses[] = {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
        CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    char const* RoleName(PawnRole role)
    {
        switch (role)
        {
            case PAWN_ROLE_TANK:   return "tank";
            case PAWN_ROLE_HEALER: return "healer";
            case PAWN_ROLE_DPS:    return "dps";
            default:               return "unknown";
        }
    }

    // The alliance races, as RandomPlayerbotMgr::PrepareAddclassCache buckets them.
    bool IsAllianceRace(uint8 race)
    {
        return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF ||
               race == RACE_GNOME || race == RACE_DRAENEI;
    }

    // SplitMix64: a seeded shuffle that leaves the global RNG alone, so the draw
    // is reproducible from (player, instance) and cannot be perturbed by anything
    // else on the world thread.
    uint64 Mix(uint64& state)
    {
        state ^= state >> 30;
        state *= 0xBF58476D1CE4E5B9ull;
        state ^= state >> 27;
        state *= 0x94D049BB133111EBull;
        state ^= state >> 31;
        return state;
    }
}

PawnMgr& PawnMgr::instance()
{
    static PawnMgr instance;
    return instance;
}

void PawnMgr::LoadConfig()
{
    _config.Enabled = sConfigMgr->GetOption<bool>("Pawn.Enable", true);
    _config.MaxPawns = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.MaxPawns", 2), 0, 4));
    _config.LevelWindow = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.LevelWindow", 2), 0, 10));
    _config.OfferedPerPage = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.OfferedPerPage", 12), 1, 14));
    _config.FeaturedMin = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.FeaturedMin", 10), 1, 50));
    _config.FeaturedMax = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.FeaturedMax", 15), 1, 50));
    _config.PoolRefreshMinMinutes = uint16(std::clamp(sConfigMgr->GetOption<int32>("Pawn.PoolRefreshMinMinutes", 40), 1, 1440));
    _config.PoolRefreshMaxMinutes = uint16(std::clamp(sConfigMgr->GetOption<int32>("Pawn.PoolRefreshMaxMinutes", 60), 1, 1440));
    _config.Debug = sConfigMgr->GetOption<bool>("Pawn.Debug", false);

    if (_config.FeaturedMax < _config.FeaturedMin)
        std::swap(_config.FeaturedMax, _config.FeaturedMin);
    if (_config.PoolRefreshMaxMinutes < _config.PoolRefreshMinMinutes)
        std::swap(_config.PoolRefreshMaxMinutes, _config.PoolRefreshMinMinutes);

    LOG_INFO("module", "Pawn: config: enabled={} maxPawns={} levelWindow={} offersPerPage={} featured={}-{} refresh={}-{}min",
             _config.Enabled, uint32(_config.MaxPawns), uint32(_config.LevelWindow),
             uint32(_config.OfferedPerPage), uint32(_config.FeaturedMin), uint32(_config.FeaturedMax),
             uint32(_config.PoolRefreshMinMinutes), uint32(_config.PoolRefreshMaxMinutes));
}

void PawnMgr::RefreshPool()
{
    // The pool is the playerbots "AddClass" account set (account_type 2 in the
    // playerbots database). Reading it here rather than from playerbots' own
    // addclass cache keeps the module working with
    // Playerbots.AddClassCommand = 0 - the chat command is a way around the
    // party cap, so the installer turns it off - and gives the picker a fresh
    // level and online flag on every roll instead of once per boot.
    uint32 const now = GetMSTime();
    uint32 const minutes = urand(_config.PoolRefreshMinMinutes, _config.PoolRefreshMaxMinutes);
    _poolRefreshDue = now + minutes * 60 * 1000;
    _pool.clear();

    QueryResult accounts = PlayerbotsDatabase.Query("SELECT account_id FROM playerbots_account_type WHERE account_type = 2");
    if (!accounts)
    {
        LOG_WARN("module", "Pawn: pool: no type-2 AddClass accounts; the picker is empty");
        return;
    }

    std::string accountList;
    do
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accounts->Fetch()[0].Get<uint32>());
    } while (accounts->NextRow());

    QueryResult characters = CharacterDatabase.Query(
        "SELECT guid, name, class, race, level FROM characters WHERE online = 0 AND account IN ({})", accountList);
    if (!characters)
    {
        LOG_WARN("module", "Pawn: pool: no offline characters on the {} AddClass accounts", accountList);
        return;
    }

    do
    {
        Field* fields = characters->Fetch();

        PawnPoolChar poolChar;
        poolChar.Bot = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
        poolChar.Name = fields[1].Get<std::string>();
        poolChar.Class = fields[2].Get<uint8>();
        poolChar.Race = fields[3].Get<uint8>();
        poolChar.Level = fields[4].Get<uint8>();
        _pool.push_back(poolChar);
    } while (characters->NextRow());

    LOG_INFO("module", "Pawn: pool: {} offline characters, next roll in {} min", uint32(_pool.size()), minutes);
}

void PawnMgr::EnsureSchema()
{
    // Created here rather than by module SQL on purpose: a failing module SQL file aborts the
    // worldserver boot on this fork (docs/SERVER-STACK.md section 13), and a table this module
    // cannot work without is not worth that risk. IF NOT EXISTS makes it a no-op afterwards.
    // DirectExecute, not Execute: the roster is read on the next line of
    // LoadRoster, and the asynchronous form only queues the statement.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `pawn_roster` ("
        "`owner_guid` INT UNSIGNED NOT NULL,"
        "`bot_guid` INT UNSIGNED NOT NULL,"
        "`spec_no` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`role` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`state` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`built` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "PRIMARY KEY (`owner_guid`, `bot_guid`),"
        "KEY `bot_guid` (`bot_guid`),"
        "KEY `owner_state` (`owner_guid`, `state`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    // CREATE TABLE IF NOT EXISTS does not add a column to a table an earlier
    // build created, and ADD COLUMN IF NOT EXISTS is MariaDB-only, so the column
    // is looked up first. information_schema is portable to both.
    QueryResult hasBuilt = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = 'pawn_roster' AND COLUMN_NAME = 'built'");
    if (hasBuilt && hasBuilt->Fetch()[0].Get<uint64>() == 0)
        CharacterDatabase.DirectExecute(
            "ALTER TABLE `pawn_roster` ADD COLUMN `built` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `state`");
}

void PawnMgr::LoadRoster()
{
    EnsureSchema();
    _rows.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT owner_guid, bot_guid, spec_no, role, state, target_level, built FROM pawn_roster");
    if (!result)
    {
        LOG_INFO("module", "Pawn: roster: empty");
        return;
    }

    uint32 recruited = 0;
    do
    {
        Field* fields = result->Fetch();

        PawnRosterRow row;
        row.Owner = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
        row.Bot = ObjectGuid(HighGuid::Player, fields[1].Get<uint32>());
        row.SpecNo = fields[2].Get<uint8>();
        row.Role = PawnRole(fields[3].Get<uint8>());
        row.Recruited = fields[4].Get<uint8>() == 1;
        row.TargetLevel = fields[5].Get<uint8>();
        row.Built = fields[6].Get<uint8>() == 1;

        if (row.Recruited)
            ++recruited;

        _rows[row.Bot.GetCounter()] = row;
    } while (result->NextRow());

    LOG_INFO("module", "Pawn: roster: {} rows, {} in a party", uint32(_rows.size()), recruited);
}

bool PawnMgr::IsPawn(ObjectGuid guid) const
{
    return _rows.find(guid.GetCounter()) != _rows.end();
}

bool PawnMgr::IsRecruitedBy(ObjectGuid owner, ObjectGuid bot) const
{
    auto it = _rows.find(bot.GetCounter());
    return it != _rows.end() && it->second.Recruited && it->second.Owner == owner;
}

uint32 PawnMgr::CountParty(ObjectGuid owner) const
{
    uint32 count = 0;
    for (auto const& [bot, row] : _rows)
    {
        if (row.Recruited && row.Owner == owner)
            ++count;
    }
    return count;
}

std::vector<PawnRosterRow> PawnMgr::ListParty(ObjectGuid owner) const
{
    std::vector<PawnRosterRow> party;
    for (auto const& [bot, row] : _rows)
    {
        if (row.Recruited && row.Owner == owner)
            party.push_back(row);
    }

    std::sort(party.begin(), party.end(), [](PawnRosterRow const& a, PawnRosterRow const& b)
    {
        return a.Bot.GetRawValue() < b.Bot.GetRawValue();
    });
    return party;
}

uint8 PawnMgr::DeriveLevel(ObjectGuid owner, ObjectGuid bot, uint8 ownerLevel, uint8 cls) const
{
    // The level and the spec are a pure function of the two guids, so the same
    // pawn is offered at the same level and with the same spec every visit, for
    // this player, and it keeps that level as the player levels.
    uint32 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);
    int const window = int(_config.LevelWindow);
    int const span = window * 2 + 1;
    int const offset = window ? int(seed % uint32(span)) - window : 0;

    int level = int(ownerLevel) + offset;
    level = std::max(1, std::min(int(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)), level));
    if (cls == CLASS_DEATH_KNIGHT)
        level = std::max(level, int(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL)));
    return uint8(level);
}

PawnOffer PawnMgr::MakeOffer(PawnPoolChar const& poolChar, ObjectGuid owner, uint8 ownerLevel) const
{
    PawnOffer offer;
    offer.Bot = poolChar.Bot;
    offer.Name = poolChar.Name;
    offer.Class = poolChar.Class;
    offer.Level = DeriveLevel(owner, poolChar.Bot, ownerLevel, poolChar.Class);

    uint32 const seed = owner.GetCounter() ^ (poolChar.Bot.GetCounter() * 2654435761u);
    auto const& specs = sPawnArchetype.PveSpecs(poolChar.Class);
    if (!specs.empty())
    {
        offer.SpecNo = specs[(seed >> 8) % specs.size()];
        offer.SpecName = sPawnArchetype.SpecName(poolChar.Class, offer.SpecNo);
        offer.Role = sPawnArchetype.RoleFor(poolChar.Class, offer.SpecNo);
    }

    return offer;
}

std::vector<PawnOffer> PawnMgr::DrawOffers(Player* player)
{
    std::vector<PawnOffer> offers;
    if (!player)
        return offers;

    // A boot can finish before playerbots has created the pool characters, and
    // the timer is 40-60 minutes away; draw from an empty pool by reading it now.
    if (_pool.empty())
        RefreshPool();

    ObjectGuid const owner = player->GetGUID();
    uint8 const ownerLevel = player->GetLevel();
    bool const alliance = player->GetTeamId() == TEAM_ALLIANCE;
    uint32 const heroicStart = sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);

    std::vector<PawnPoolChar const*> candidates;
    candidates.reserve(_pool.size());
    for (PawnPoolChar const& poolChar : _pool)
    {
        // The same rules the addclass command applies: same team as the player
        // (its pool is bucketed by team, and a pawn of the other faction cannot
        // be grouped or summoned), no death knight before the heroic start
        // level, and no class without a premade PvE spec.
        if (IsAllianceRace(poolChar.Race) != alliance)
            continue;
        if (poolChar.Class == CLASS_DEATH_KNIGHT && ownerLevel < heroicStart)
            continue;
        if (sPawnArchetype.PveSpecs(poolChar.Class).empty())
            continue;
        if (IsRecruitedBy(owner, poolChar.Bot))
            continue;
        if (ObjectAccessor::FindConnectedPlayer(poolChar.Bot))
            continue;

        candidates.push_back(&poolChar);
    }

    if (candidates.empty())
        return offers;

    uint64 state = _drawCounter++ * 0x9E3779B97F4A7C15ull ^ (uint64(owner.GetCounter()) * 0xC2B2AE3D27D4EB4Full);
    for (size_t i = candidates.size(); i > 1; --i)
        std::swap(candidates[i - 1], candidates[Mix(state) % i]);

    uint32 const minCount = _config.FeaturedMin;
    uint32 const maxCount = std::max<uint32>(minCount, _config.FeaturedMax);
    uint32 const wanted = minCount + uint32(Mix(state) % (maxCount - minCount + 1));
    uint32 const take = std::min<uint32>(wanted, uint32(candidates.size()));

    offers.reserve(take);
    for (uint32 i = 0; i < take; ++i)
        offers.push_back(MakeOffer(*candidates[i], owner, ownerLevel));

    LOG_DEBUG("module", "Pawn: {} offers drawn from {} candidates for {}", take, uint32(candidates.size()), player->GetName());
    return offers;
}

void PawnMgr::OnPlayerLogin(Player* player)
{
    if (!_config.Enabled || !player)
        return;

    if (IsPawn(player->GetGUID()))
    {
        _pendingPawns.insert(player->GetGUID().GetCounter());
        return;
    }

    _pendingOwners.insert(player->GetGUID().GetCounter());
}

void PawnMgr::Update(uint32 /*diff*/)
{
    if (_config.Enabled && GetMSTime() >= _poolRefreshDue)
        RefreshPool();

    if (_pendingOwners.empty() && _pendingPawns.empty())
        return;

    for (auto it = _pendingOwners.begin(); it != _pendingOwners.end();)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, *it));
        if (!player)
        {
            it = _pendingOwners.erase(it);
            continue;
        }

        if (!player->IsInWorld())
        {
            ++it;
            continue;
        }

        RestoreParty(player);
        it = _pendingOwners.erase(it);
    }

    for (auto it = _pendingPawns.begin(); it != _pendingPawns.end();)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, *it));
        if (!bot)
        {
            it = _pendingPawns.erase(it);
            continue;
        }

        if (!bot->IsInWorld())
        {
            ++it;
            continue;
        }

        ApplyPawn(bot);
        it = _pendingPawns.erase(it);
    }
}

void PawnMgr::RestoreParty(Player* player)
{
    uint32 const owner = player->GetGUID().GetCounter();

    for (auto const& [botCounter, row] : _rows)
    {
        if (!row.Recruited || row.Owner.GetCounter() != owner)
            continue;

        if (ObjectAccessor::FindConnectedPlayer(row.Bot))
            continue;

        PlayerbotHolder* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
        {
            LOG_ERROR("module", "Pawn: no playerbot manager for {}; party not restored", player->GetName());
            return;
        }

        LOG_DEBUG("module", "Pawn: restoring pawn {} for {}", row.Bot.ToString(), player->GetName());
        mgr->AddPlayerBot(row.Bot, player->GetSession()->GetAccountId());
    }
}

void PawnMgr::ApplyPawn(Player* bot)
{
    PawnRosterRow* row = nullptr;
    if (auto it = _rows.find(bot->GetGUID().GetCounter()); it != _rows.end())
        row = &it->second;

    if (!row || !row->Recruited)
        return;

    // The invariant is the window, not one exact level: a pawn anywhere inside
    // +/- Pawn.LevelWindow of its owner is left alone, which also keeps
    // playerbots' own login pass (it snaps a type-2 bot to its master's exact
    // level when the gap is more than three) from being undone for no reason.
    // Outside the window the pawn is re-levelled to the level it would be
    // offered at, so a player who outlevels a pawn pulls it back in on its next
    // login instead of carrying a liability.
    Player* owner = ObjectAccessor::FindConnectedPlayer(row->Owner);
    uint8 const wanted = owner ? DeriveLevel(row->Owner, row->Bot, owner->GetLevel(), bot->getClass()) : 0;
    bool const reLevel = owner && bot->GetLevel() != wanted &&
                         std::abs(int(bot->GetLevel()) - int(owner->GetLevel())) > int(_config.LevelWindow);

    if (reLevel || !row->Built)
    {
        uint8 const level = reLevel ? wanted : bot->GetLevel();
        uint32 const gearScoreLimit = owner
            ? uint32(PlayerbotAI::GetMixedGearScore(owner, true, false, 12) *
                     sPlayerbotAIConfig.AutoInitEquipLevelLimitRatio)
            : 0;

        if (reLevel)
            LOG_INFO("module", "Pawn: pawn {}: level {} -> {} (owner {})", bot->GetName(),
                     uint32(bot->GetLevel()), uint32(wanted), uint32(owner->GetLevel()));

        // A full playerbots build, at the level the pawn is meant to have: level,
        // stats, spells, a random spec, gear, bags, food and reagents. Its gear is
        // thrown away two steps down, because it was chosen for the random spec.
        PlayerbotFactory factory(bot, level, ITEM_QUALITY_LEGENDARY, gearScoreLimit);
        factory.Randomize(false);

        // Pin the spec before the gear pass. InitEquipment scores every candidate
        // item with StatsWeightCalculator, whose collector type comes from the
        // bot's spec, so anything equipped before the pin is gear for whatever
        // spec Randomize rolled - a prot pawn in a fury warrior's two-hander and
        // no shield.
        sPawnArchetype.Apply(bot, bot->getClass(), row->SpecNo);

        // The weight calculator does not read the talent tab: IsTank/IsHeal/
        // IsCaster with bySpec=false test STRATEGY_TYPE_* on the AI's engines,
        // and those still describe the spec Randomize rolled. Rebuild them from
        // the pinned spec first - the same call Randomize makes after it changes
        // talents - or a prot pawn is scored as a damage dealer.
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->ResetStrategies(false);

        // Then gear the pawn for the spec it now has, at its level, with the
        // owner's gear score as the ceiling so a pawn is never better equipped
        // than the player it follows. This is PlayerbotFactory::AutoGear's
        // sequence with that ceiling, plus DestroyEquippedGear because the items
        // Randomize just equipped were chosen for the other spec.
        PlayerbotFactory::DestroyEquippedGear(bot);
        PlayerbotFactory gear(bot, level, ITEM_QUALITY_LEGENDARY, gearScoreLimit);
        gear.InitEquipment(false, sPlayerbotAIConfig.TwoRoundsGearInit);
        gear.InitAmmo();
        if (bot->GetLevel() >= sPlayerbotAIConfig.MinEnchantingBotLevel)
            gear.ApplyEnchantAndGemsNew();
        bot->DurabilityRepairAll(false, 1.0f, false);

        LOG_INFO("module", "Pawn: pawn {}: {} at level {} for {} (gear score cap {})",
                 bot->GetName(), row->Built ? "re-geared" : "geared", uint32(bot->GetLevel()),
                 sPawnArchetype.SpecName(bot->getClass(), row->SpecNo), uint32(gearScoreLimit));

        if (!row->Built)
        {
            row->Built = true;
            Upsert(*row);
        }
    }
    else
        sPawnArchetype.Apply(bot, bot->getClass(), row->SpecNo);

    // The roster records the level the pawn actually has, so the fallback used
    // when the owner is offline is the truth rather than an old target.
    if (bot->GetLevel() != row->TargetLevel)
    {
        row->TargetLevel = bot->GetLevel();
        Upsert(*row);
    }

    bot->SaveToDB(false, false);
}

bool PawnMgr::Recruit(Player* player, PawnOffer const& offer, std::string& message)
{
    if (!_config.Enabled)
    {
        message = "No innkeeper will vouch for a companion today.";
        return false;
    }

    if (!player || !offer.Bot)
    {
        message = "That pawn is not available.";
        return false;
    }

    if (player->IsInCombat())
    {
        message = "Not while you are fighting.";
        return false;
    }

    if (ObjectAccessor::FindConnectedPlayer(offer.Bot))
    {
        message = offer.Name + " is already walking the world.";
        return false;
    }

    if (CountParty(player->GetGUID()) >= _config.MaxPawns)
    {
        message = "Your party already holds " + std::to_string(uint32(_config.MaxPawns)) +
                  " pawns. Dismiss one here first, or recruit strangers out in the world.";
        return false;
    }

    PawnRosterRow row;
    row.Owner = player->GetGUID();
    row.Bot = offer.Bot;
    row.SpecNo = offer.SpecNo;
    row.Role = offer.Role;
    row.Recruited = true;
    row.TargetLevel = offer.Level;

    // A dismissed pawn keeps its row, so a returning one keeps the gear it
    // earned: without this the next login would re-equip it from scratch.
    if (auto it = _rows.find(offer.Bot.GetCounter()); it != _rows.end())
        row.Built = it->second.Built;

    Upsert(row);
    _rows[offer.Bot.GetCounter()] = row;

    PlayerbotHolder* mgr = GET_PLAYERBOT_MGR(player);
    if (!mgr)
    {
        message = "Your pawn could not be called; no bot manager.";
        return false;
    }

    mgr->AddPlayerBot(offer.Bot, player->GetSession()->GetAccountId());

    LOG_INFO("module", "Pawn: {} recruited {} ({} {}, level {})", player->GetName(), offer.Name,
             offer.SpecName, RoleName(offer.Role), uint32(offer.Level));
    message = offer.Name + " joins you as a " + offer.SpecName + " " + RoleName(offer.Role) + ".";
    return true;
}

bool PawnMgr::Dismiss(Player* player, ObjectGuid bot, std::string& message)
{
    auto it = _rows.find(bot.GetCounter());
    if (!player || it == _rows.end() || it->second.Owner != player->GetGUID() || !it->second.Recruited)
    {
        message = "That pawn is not in your party.";
        return false;
    }

    // LogoutPlayerBot deletes the Player object, so nothing may touch it after
    // the call. The roster row is the only state that matters afterwards.
    if (ObjectAccessor::FindConnectedPlayer(bot))
    {
        if (PlayerbotHolder* mgr = GET_PLAYERBOT_MGR(player))
            mgr->LogoutPlayerBot(bot);
    }

    it->second.Recruited = false;
    Upsert(it->second);

    message = "Your pawn is dismissed.";
    LOG_INFO("module", "Pawn: {} dismissed {}", player->GetName(), bot.ToString());
    return true;
}

bool PawnMgr::IsSpellForbidden(Player* bot, uint32 spellId) const
{
    if (!bot)
        return false;

    auto it = _rows.find(bot->GetGUID().GetCounter());
    if (it == _rows.end())
        return false;

    return sPawnArchetype.IsForbidden(bot->getClass(), it->second.SpecNo, spellId);
}

void PawnMgr::Upsert(PawnRosterRow const& row)
{
    CharacterDatabase.Execute(
        "REPLACE INTO pawn_roster (owner_guid, bot_guid, spec_no, role, state, target_level, built) "
        "VALUES ({}, {}, {}, {}, {}, {}, {})",
        row.Owner.GetCounter(), row.Bot.GetCounter(), uint32(row.SpecNo), uint32(row.Role),
        row.Recruited ? 1 : 0, uint32(row.TargetLevel), row.Built ? 1 : 0);
}
