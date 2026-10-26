/*
 * mod-pawn: the roster, the offers and the party.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnMgr.h"

#include "CharacterCache.h"
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
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdint>

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
    _config.ObeliskEntry = sConfigMgr->GetOption<uint32>("Pawn.ObeliskEntry", 5000010);
    _config.Debug = sConfigMgr->GetOption<bool>("Pawn.Debug", false);

    LOG_INFO("module", "Pawn: config: enabled={} maxPawns={} levelWindow={} offersPerPage={} obelisk={}",
             _config.Enabled, uint32(_config.MaxPawns), uint32(_config.LevelWindow),
             uint32(_config.OfferedPerPage), _config.ObeliskEntry);
}

void PawnMgr::EnsureSchema()
{
    // Created here rather than by module SQL on purpose: a failing module SQL file aborts the
    // worldserver boot on this fork (docs/SERVER-STACK.md section 13), and a table this module
    // cannot work without is not worth that risk. IF NOT EXISTS makes it a no-op afterwards.
    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS `pawn_roster` ("
        "`owner_guid` INT UNSIGNED NOT NULL,"
        "`bot_guid` INT UNSIGNED NOT NULL,"
        "`spec_no` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`role` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`state` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`target_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "PRIMARY KEY (`owner_guid`, `bot_guid`),"
        "KEY `bot_guid` (`bot_guid`),"
        "KEY `owner_state` (`owner_guid`, `state`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void PawnMgr::LoadRoster()
{
    EnsureSchema();
    _rows.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT owner_guid, bot_guid, spec_no, role, state, target_level FROM pawn_roster");
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

PawnRosterRow const* PawnMgr::GetRow(ObjectGuid bot) const
{
    auto it = _rows.find(bot.GetCounter());
    return it == _rows.end() ? nullptr : &it->second;
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

PawnOffer PawnMgr::MakeOffer(uint8 cls, ObjectGuid owner, uint8 ownerLevel, ObjectGuid bot) const
{
    PawnOffer offer;
    offer.Bot = bot;
    offer.Class = cls;
    offer.Level = DeriveLevel(owner, bot, ownerLevel, cls);

    uint32 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);
    auto const& specs = sPawnArchetype.PveSpecs(cls);
    if (!specs.empty())
    {
        offer.SpecNo = specs[(seed >> 8) % specs.size()];
        offer.SpecName = sPawnArchetype.SpecName(cls, offer.SpecNo);
        offer.Role = sPawnArchetype.RoleFor(cls, offer.SpecNo);
    }

    sCharacterCache->GetCharacterNameByGuid(bot, offer.Name);
    return offer;
}

std::vector<PawnOffer> PawnMgr::BuildOffers(Player* player) const
{
    std::vector<PawnOffer> offers;
    if (!player)
        return offers;

    ObjectGuid const owner = player->GetGUID();
    uint8 const ownerLevel = player->GetLevel();
    bool const alliance = player->GetTeamId() == TEAM_ALLIANCE;
    uint32 const heroicStart = sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);

    for (uint8 cls : kClasses)
    {
        // The same rule the addclass command applies: no death knight before
        // the heroic start level.
        if (cls == CLASS_DEATH_KNIGHT && ownerLevel < heroicStart)
            continue;

        if (sPawnArchetype.PveSpecs(cls).empty())
            continue;

        auto it = sRandomPlayerbotMgr.addclassCache.find(RandomPlayerbotMgr::GetTeamClassIdx(alliance, cls));
        if (it == sRandomPlayerbotMgr.addclassCache.end())
            continue;

        for (ObjectGuid const& bot : it->second)
        {
            if (ObjectAccessor::FindConnectedPlayer(bot))
                continue;
            if (IsRecruitedBy(owner, bot))
                continue;

            offers.push_back(MakeOffer(cls, owner, ownerLevel, bot));
        }
    }

    // The pool is an unordered_set per class: sort so the order does not change
    // between visits, and so a page is a stable slice of the pool.
    std::sort(offers.begin(), offers.end(), [](PawnOffer const& a, PawnOffer const& b)
    {
        return a.Bot.GetRawValue() < b.Bot.GetRawValue();
    });

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

    // The target follows the owner's current level, so a pawn never drifts out
    // of the window: the player can outlevel it, and the next login pulls it
    // back in. The roster's target_level is the fallback for the case where the
    // owner is not resolvable, and is refreshed whenever it changes.
    uint8 target = row->TargetLevel;
    if (Player* owner = ObjectAccessor::FindConnectedPlayer(row->Owner))
    {
        target = DeriveLevel(row->Owner, row->Bot, owner->GetLevel(), bot->getClass());

        uint32 gearScore = uint32(PlayerbotAI::GetMixedGearScore(owner, true, false, 12) *
                                  sPlayerbotAIConfig.AutoInitEquipLevelLimitRatio);

        if (target && bot->GetLevel() != target)
        {
            LOG_INFO("module", "Pawn: pawn {}: level {} -> {}", bot->GetName(), uint32(bot->GetLevel()), uint32(target));
            PlayerbotFactory factory(bot, target, ITEM_QUALITY_LEGENDARY, gearScore);
            factory.Randomize(false);
        }
    }

    if (target && target != row->TargetLevel)
    {
        row->TargetLevel = target;
        Upsert(*row);
    }

    sPawnArchetype.Apply(bot, bot->getClass(), row->SpecNo);
    bot->SaveToDB(false, false);
}

bool PawnMgr::Recruit(Player* player, PawnOffer const& offer, std::string& message)
{
    if (!_config.Enabled)
    {
        message = "The obelisk is dormant.";
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
        "REPLACE INTO pawn_roster (owner_guid, bot_guid, spec_no, role, state, target_level) "
        "VALUES ({}, {}, {}, {}, {}, {})",
        row.Owner.GetCounter(), row.Bot.GetCounter(), uint32(row.SpecNo), uint32(row.Role),
        row.Recruited ? 1 : 0, uint32(row.TargetLevel));
}
