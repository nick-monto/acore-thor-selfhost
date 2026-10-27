/*
 * mod-pawn: the roster, the offers and the party.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "PawnMgr.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PawnArchetype.h"
#include "PawnPersonality.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

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

    // The mount spell the pawn knows, if any. Playerbots rolls the mount once and
    // the character keeps it (spells are saved), so it is read back rather than
    // chosen: the only reliable answer to "what does this pawn ride" is the spell
    // that is actually on it. A summon spell carries the mount as an applied aura.
    uint32 FindMountSpell(Player* bot)
    {
        for (auto const& [id, playerSpell] : bot->GetSpellMap())
        {
            if (!playerSpell || !bot->HasSpell(id))
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].Effect == SPELL_EFFECT_APPLY_AURA &&
                    info->Effects[i].ApplyAuraName == SPELL_AURA_MOUNTED)
                    return id;
        }

        return 0;
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
    _config.HireFeePercent = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.HireFeePercent", 100), 0, 10000));
    _config.HireFeeHeldPercent = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.HireFeeHeldPercent", 100), 0, 1000));
    _config.GainXP = sConfigMgr->GetOption<bool>("Pawn.GainXP", false);
    _config.VariantWithholdMin = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.VariantWithholdMin", 1), 0, 5));
    _config.VariantWithholdMax = uint8(std::clamp(sConfigMgr->GetOption<int32>("Pawn.VariantWithholdMax", 2), 0, 5));
    // A minimum above the maximum would make the draw empty; the maximum wins,
    // because it is the one that can switch the variation off.
    if (_config.VariantWithholdMin > _config.VariantWithholdMax)
        _config.VariantWithholdMin = _config.VariantWithholdMax;
    _config.GearFloorPercent = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.GearFloorPercent", 85), 25, 100));
    _config.GearCapPercent = uint32(std::clamp(sConfigMgr->GetOption<int32>("Pawn.GearCapPercent", 150), 100, 300));
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

    LOG_INFO("module", "Pawn: config: enabled={} maxPawns={} levelWindow={} offersPerPage={} featured={}-{} refresh={}-{}min fee={}%+{}%/pawn gear={}-{}%",
             _config.Enabled, uint32(_config.MaxPawns), uint32(_config.LevelWindow),
             uint32(_config.OfferedPerPage), uint32(_config.FeaturedMin), uint32(_config.FeaturedMax),
             uint32(_config.PoolRefreshMinMinutes), uint32(_config.PoolRefreshMaxMinutes),
             _config.HireFeePercent, _config.HireFeeHeldPercent,
             _config.GearFloorPercent, _config.GearCapPercent);
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

    // Every deck was built against the old pool, so it is dealt out against the
    // new one. Counters are guids and survive, but a roll that added or dropped
    // characters should change who is on offer, not just who is eligible.
    _decks.clear();

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

// The roster table is created by this module, so a column added in a later build
// has to be added to a table an earlier build already made. The lookup is what
// makes that portable: ADD COLUMN IF NOT EXISTS is MariaDB-only.
static void AddMissingColumn(std::string const& column, std::string const& definition)
{
    QueryResult has = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = 'pawn_roster' AND COLUMN_NAME = '" + column + "'");
    if (has && has->Fetch()[0].Get<uint64>() == 0)
        CharacterDatabase.DirectExecute("ALTER TABLE `pawn_roster` ADD COLUMN " + definition);
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
        "`paid` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`mount_spell` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`pet_name` VARCHAR(21) NOT NULL DEFAULT '',"
        "`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "PRIMARY KEY (`owner_guid`, `bot_guid`),"
        "KEY `bot_guid` (`bot_guid`),"
        "KEY `owner_state` (`owner_guid`, `state`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    // CREATE TABLE IF NOT EXISTS does not add a column to a table an earlier
    // build created, and ADD COLUMN IF NOT EXISTS is MariaDB-only, so each column
    // is looked up first. information_schema is portable to both.
    AddMissingColumn("built", "`built` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `target_level`");
    AddMissingColumn("paid", "`paid` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `built`");
    AddMissingColumn("mount_spell", "`mount_spell` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `paid`");
    AddMissingColumn("pet_name", "`pet_name` VARCHAR(21) NOT NULL DEFAULT '' AFTER `mount_spell`");
}

void PawnMgr::LoadRoster()
{
    EnsureSchema();
    _rows.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT owner_guid, bot_guid, spec_no, role, state, target_level, built, paid, mount_spell, pet_name FROM pawn_roster");
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
        row.Paid = fields[7].Get<uint8>() == 1;
        row.MountSpell = fields[8].Get<uint32>();
        row.PetName = fields[9].Get<std::string>();

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

std::vector<PawnVariantSpell> PawnMgr::WithheldVariants(ObjectGuid owner, ObjectGuid bot, uint8 cls, uint8 spec) const
{
    std::vector<PawnVariantSpell> withheld;
    if (!_config.VariantWithholdMax)
        return withheld;

    auto const& pool = sPawnArchetype.Variants(cls, spec);
    if (pool.empty())
        return withheld;

    uint64 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);

    // How many of the pool this pawn is missing: bounded by the configuration and
    // by the pool, drawn from the same seed as everything else about the pawn.
    // The floor is what stops two pawns of one spec arriving with identical kits,
    // so it is clamped to the pool rather than to the ceiling: a pool of two with
    // a floor of three still withholds both.
    uint64 state = seed;
    uint32 const floor = std::min<uint32>(_config.VariantWithholdMin, uint32(pool.size()));
    uint32 const ceiling = std::min<uint32>(_config.VariantWithholdMax, uint32(pool.size()));
    uint32 const span = ceiling > floor ? ceiling - floor + 1 : 1;
    uint32 const count = floor + (span > 1 ? uint32(Mix(state) % span) : 0);

    // Which ones: the entries with the lowest hash of their own first id, so the
    // choice is independent per ability rather than "the first N of the list" -
    // a pool is a set, not an order the configuration means to imply.
    std::vector<std::pair<uint64, size_t>> ranked;
    ranked.reserve(pool.size());
    for (size_t i = 0; i < pool.size(); ++i)
    {
        uint64 rank = seed ^ (uint64(pool[i].Ids.front()) * 0x9E3779B97F4A7C15ull);
        ranked.emplace_back(Mix(rank), i);
    }

    std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end());

    withheld.reserve(count);
    for (uint32 i = 0; i < count; ++i)
        withheld.push_back(pool[ranked[i].second]);

    return withheld;
}

uint16 PawnMgr::GearRatio(ObjectGuid owner, ObjectGuid bot) const
{
    // A third slice of the same seed the level and the spec use, so a pawn's
    // strength is as stable as its level. Two independent bytes summed give a
    // triangular draw peaked in the middle (255), and the two halves of that draw
    // are mapped onto the two halves of the band: the mode lands exactly on 100 -
    // a pawn geared like the player - most pawns are near it, and the ends of the
    // band are as rare as a triangular draw makes them.
    uint32 const seed = owner.GetCounter() ^ (bot.GetCounter() * 2654435761u);
    int32 const draw = int32((seed >> 16) & 0xFF) + int32((seed >> 24) & 0xFF);  // 0..510, peak 255

    int32 const floor = int32(_config.GearFloorPercent);
    int32 const cap = int32(_config.GearCapPercent);
    if (draw <= 255)
        return uint16(floor + (100 - floor) * draw / 255);
    return uint16(100 + (cap - 100) * (draw - 255) / 255);
}

PawnOffer PawnMgr::MakeOffer(PawnPoolChar const& poolChar, ObjectGuid owner, uint8 ownerLevel) const
{
    PawnOffer offer;
    offer.Bot = poolChar.Bot;
    offer.Name = poolChar.Name;
    offer.Class = poolChar.Class;
    offer.Level = DeriveLevel(owner, poolChar.Bot, ownerLevel, poolChar.Class);
    offer.GearRatio = GearRatio(owner, poolChar.Bot);

    uint32 const seed = owner.GetCounter() ^ (poolChar.Bot.GetCounter() * 2654435761u);
    auto const& specs = sPawnArchetype.PveSpecs(poolChar.Class);
    if (!specs.empty())
    {
        offer.SpecNo = specs[(seed >> 8) % specs.size()];
        offer.SpecName = sPawnArchetype.SpecName(poolChar.Class, offer.SpecNo);
        offer.Role = sPawnArchetype.RoleFor(poolChar.Class, offer.SpecNo);
        offer.Withheld = WithheldVariants(owner, poolChar.Bot, poolChar.Class, offer.SpecNo);

        // The temperament is a per-spec profile, so it can only be drawn once the
        // spec is; the voice is not spec-scoped and is drawn either way.
        if (PawnProfile const* temperament =
                sPawnPersonality.Temperament(owner, poolChar.Bot, poolChar.Class, offer.SpecNo))
            offer.TemperamentLabel = temperament->Label;
    }

    if (PawnProfile const* voice = sPawnPersonality.Voice(owner, poolChar.Bot))
        offer.VoiceLabel = voice->Label;

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

    std::unordered_map<uint32, PawnPoolChar const*> byCounter;
    byCounter.reserve(candidates.size());
    for (PawnPoolChar const* candidate : candidates)
        byCounter[candidate->Bot.GetCounter()] = candidate;

    uint64 state = _drawCounter++ * 0x9E3779B97F4A7C15ull ^ (uint64(owner.GetCounter()) * 0xC2B2AE3D27D4EB4Full);

    uint32 const minCount = _config.FeaturedMin;
    uint32 const maxCount = std::max<uint32>(minCount, _config.FeaturedMax);
    uint32 const wanted = minCount + uint32(Mix(state) % (maxCount - minCount + 1));

    // Deal from the player's deck rather than drawing a fresh sample: a face that
    // has been shown is not shown again until the deck is dealt out and reshuffled,
    // which is what makes two visits to an inn feel like two different crowds
    // instead of the same twelve of fifty with a few swapped. The deck is dealt
    // without replacement, so a small pool is cycled rather than repeated.
    PawnDeck& deck = _decks[owner.GetCounter()];
    std::unordered_set<uint32> shown;

    auto const reshuffle = [&]()
    {
        deck.Order.clear();
        deck.Order.reserve(candidates.size());
        for (PawnPoolChar const* candidate : candidates)
            deck.Order.push_back(candidate->Bot.GetCounter());

        for (size_t i = deck.Order.size(); i > 1; --i)
            std::swap(deck.Order[i - 1], deck.Order[Mix(state) % i]);

        // The last visit's faces go to the back of the new order. A reshuffle is
        // the one moment a repeat could land in the very next visit, and this is
        // what stops it; they are still in the deck, just later in it.
        if (!deck.Recent.empty())
            std::stable_partition(deck.Order.begin(), deck.Order.end(), [&deck](uint32 counter)
            {
                return std::find(deck.Recent.begin(), deck.Recent.end(), counter) == deck.Recent.end();
            });

        deck.Cursor = 0;
    };

    // Never straddle a reshuffle inside one visit: if the deck cannot cover the
    // whole visit, reshuffle first. A visit that crossed the end of the deck would
    // mix the tail of one cycle with the head of the next, and the head can hold
    // faces shown two or three visits ago. The tail that is skipped is not lost,
    // it is back in the next shuffle.
    if (deck.Order.size() - deck.Cursor < std::min<size_t>(wanted, candidates.size()))
        reshuffle();

    while (offers.size() < wanted && shown.size() < candidates.size())
    {
        if (deck.Cursor >= deck.Order.size())
            reshuffle();

        // A dealt counter may have stopped being a candidate since the deck was
        // built - recruited, logged in, or dropped by a pool refresh - so it is
        // looked up again rather than trusted.
        uint32 const counter = deck.Order[deck.Cursor++];
        auto const found = byCounter.find(counter);
        if (found == byCounter.end() || !shown.insert(counter).second)
            continue;

        offers.push_back(MakeOffer(*found->second, owner, ownerLevel));
    }

    deck.Recent.clear();
    for (PawnOffer const& offer : offers)
        deck.Recent.push_back(offer.Bot.GetCounter());

    LOG_DEBUG("module", "Pawn: {} offers dealt from {} candidates for {} ({} left in the deck)",
              uint32(offers.size()), uint32(candidates.size()), player->GetName(),
              uint32(deck.Order.size() - deck.Cursor));
    return offers;
}

void PawnMgr::ForgetPlayer(ObjectGuid player)
{
    _decks.erase(player.GetCounter());
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

    // A pawn is what was hired, at the level it was hired at. It does not follow
    // the owner up: the level is pinned to the row's target, so a player who
    // outlevels a pawn carries it behind them until they exchange it at the inn.
    // That is the whole point of the pin - the roster is a set of contracts, not
    // a set of pets that grow.
    //
    // playerbots' own login pass rebuilds a type-2 bot at its master's exact level
    // when the gap is more than three, so the pin has to be applied after it:
    // this drain runs on the world thread, from WorldScript::OnUpdate, which is
    // later than the bot's login hook. A pawn the snap moved is rebuilt here at
    // the level it was hired at.
    Player* owner = ObjectAccessor::FindConnectedPlayer(row->Owner);
    uint8 const hiredAt = row->TargetLevel ? row->TargetLevel : bot->GetLevel();
    bool const rePin = !_config.GainXP && bot->GetLevel() != hiredAt;

    // The core's one XP choke point, Player::GiveXP, drops every source - kills,
    // quests, exploration, battlegrounds - for a player carrying this flag. The
    // flag is on the character and saved with it, so the pin holds even if some
    // other path would have levelled the pawn.
    if (_config.GainXP)
        bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    else
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    if (rePin || !row->Built)
    {
        uint8 const level = hiredAt;
        // The per-item score ceiling the gear pass is held to: the owner's own
        // average item score, times this pawn's gear ratio. The ratio is what makes
        // one pawn stronger - and dearer - than another offered at the same level,
        // and it runs from Pawn.GearFloorPercent to Pawn.GearCapPercent, so a pawn
        // can be geared that far below or above the player.
        uint32 const gearScoreLimit = owner
            ? uint32(uint64(PlayerbotAI::GetMixedGearScore(owner, true, false, 12)) *
                     GearRatio(row->Owner, row->Bot) / 100)
            : 0;

        if (rePin)
            LOG_INFO("module", "Pawn: pawn {}: level {} -> {} (hired at that level; owner {})",
                     bot->GetName(), uint32(bot->GetLevel()), uint32(hiredAt),
                     owner ? owner->GetName() : "offline");

        uint32 const guildBefore = bot->GetGuildId();

        // A full playerbots build, at the level the pawn is meant to have: level,
        // stats, spells, a random spec, gear, bags, food and reagents. Its gear is
        // thrown away two steps down, because it was chosen for the random spec.
        PlayerbotFactory factory(bot, level, ITEM_QUALITY_LEGENDARY, gearScoreLimit);
        factory.Randomize(false);

        // Randomize also enrols the bot in a random bot guild (Playerbots.RandomBotGuildCount,
        // default 20) and hands it a guild tabard. A pawn is the player's companion, and a guild
        // it cannot leave would stop the player inviting it to their own, so the enrolment is
        // undone. DeleteMember is the core's own removal path (Player.cpp, character deletion).
        if (bot->GetGuildId() && !guildBefore)
        {
            if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
                guild->DeleteMember(bot->GetGUID(), false, false, true);
            if (uint32 tabards = bot->GetItemCount(5976))
                bot->DestroyItemCount(5976, tabards, true);
        }

        // Pin the spec before the gear pass. InitEquipment scores every candidate
        // item with StatsWeightCalculator, whose collector type comes from the
        // bot's spec, so anything equipped before the pin is gear for whatever
        // spec Randomize rolled - a prot pawn in a fury warrior's two-hander and
        // no shield.
        sPawnArchetype.Apply(bot, bot->getClass(), row->SpecNo,
                             WithheldVariants(row->Owner, row->Bot, bot->getClass(), row->SpecNo));

        // The weight calculator does not read the talent tab: IsTank/IsHeal/
        // IsCaster with bySpec=false test STRATEGY_TYPE_* on the AI's engines,
        // and those still describe the spec Randomize rolled. Rebuild them from
        // the pinned spec first - the same call Randomize makes after it changes
        // talents - or a prot pawn is scored as a damage dealer.
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->ResetStrategies(false);

        // Randomize set glyphs for the spec it rolled, and InitGlyphs reads the
        // tab (with the PvP exceptions keyed on talent auras), so the same pass
        // has to run again now that the talents are the pinned ones. This is
        // what ChangeTalentsAction::SpecPick does after it pins a spec.
        PlayerbotFactory(bot, bot->GetLevel()).InitGlyphs(false);

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

        LOG_INFO("module", "Pawn: pawn {}: {} at level {} for {} (gear score cap {}, ratio {}%)",
                 bot->GetName(), row->Built ? "re-geared" : "geared", uint32(bot->GetLevel()),
                 sPawnArchetype.SpecName(bot->getClass(), row->SpecNo), uint32(gearScoreLimit),
                 GearRatio(row->Owner, row->Bot));

        if (!row->Built)
        {
            row->Built = true;
            Upsert(*row);
        }
    }
    else
        sPawnArchetype.Apply(bot, bot->getClass(), row->SpecNo,
                             WithheldVariants(row->Owner, row->Bot, bot->getClass(), row->SpecNo));

    // The trait has to be applied after every ResetStrategies, because that call
    // rebuilds the role defaults (tank face, behind, save mana for healers) and
    // would otherwise wipe it on the first login.
    sPawnPersonality.Apply(bot,
                           sPawnPersonality.Temperament(row->Owner, row->Bot, bot->getClass(), row->SpecNo),
                           sPawnPersonality.Voice(row->Owner, row->Bot));

    // Vanity is what the pawn *has*, not what it is promised, so it is read back
    // after the build instead of chosen: a pawn below the mount level simply
    // records 0 and the party row says nothing about how it travels. Written only
    // when it changed, because ApplyPawn runs on every login and a write per
    // login would be churn for a value that almost never moves.
    {
        uint32 const mount = FindMountSpell(bot);
        std::string petName;
        if (Pet* pet = bot->GetPet())
            petName = pet->GetName();

        if (mount != row->MountSpell || petName != row->PetName)
        {
            row->MountSpell = mount;
            row->PetName = petName;
            Upsert(*row);
        }
    }

    // With Pawn.GainXP the pawn does level in the party, so the roster follows it
    // instead of pinning it: the row's target is what the fee and the party row
    // are measured against either way, and with the pin off the bot's level is
    // the truth.
    if (_config.GainXP && bot->GetLevel() != row->TargetLevel)
    {
        row->TargetLevel = bot->GetLevel();
        Upsert(*row);
    }

    bot->SaveToDB(false, false);
}

std::string FormatMoney(uint32 copper)
{
    uint32 const gold = copper / 10000;
    uint32 const silver = (copper % 10000) / 100;
    uint32 const rest = copper % 100;

    std::string out;
    if (gold)
        out = std::to_string(gold) + "g";
    if (silver)
        out += (out.empty() ? "" : " ") + std::to_string(silver) + "s";
    if (rest || out.empty())
        out += (out.empty() ? "" : " ") + std::to_string(rest) + "c";
    return out;
}

uint32 PawnMgr::HireFee(Player* player, PawnOffer const& offer) const
{
    if (!player || !_config.HireFeePercent || !offer.Bot)
        return 0;

    // A pawn this player has already paid for at this level is free to call back:
    // the fee buys a companion, not the right to keep swapping companions. At a
    // new level it is a new hire - Recruit's sameHire test is the same one - and
    // the price is charged again.
    if (auto it = _rows.find(offer.Bot.GetCounter());
        it != _rows.end() && it->second.Owner == player->GetGUID() && it->second.Paid &&
        it->second.TargetLevel == offer.Level)
        return 0;

    // The baseline is the player's own level cubed, in copper - see the conf
    // comment for why that curve: it is what a pawn geared exactly like the player
    // costs. The pawn's gear ratio then scales it, so a pawn a fifth weaker is a
    // fifth cheaper and one half again as strong is half again dearer. The pawn's
    // own level is deliberately not in here: within the level window it moves the
    // price less than the gear does, and pricing it made a low-level draw twice as
    // wide as a high-level one. 64-bit because the percentages multiply on top.
    uint64 const level = uint64(player->GetLevel());
    uint64 fee = level * level * level;
    fee = fee * _config.HireFeePercent / 100;
    fee = fee * offer.GearRatio / 100;
    fee = fee * (100 + uint64(_config.HireFeeHeldPercent) * CountParty(player->GetGUID())) / 100;

    // 80^3 * 100x * 6 pawns is far below this; the cap is only here so a nonsense
    // percentage cannot overflow, and it stays inside int32 because the charge is
    // a negative int32.
    return uint32(std::min<uint64>(fee, 2000000000ull));
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

    // The fee is charged here, not through the gossip box: the core deducts box
    // money after the script hook has run and unconditionally, so a recruit that
    // fails on a full party or a pawn already in the world would still be paid
    // for, and an empty purse would still hire. The picker shows the price and
    // asks for confirmation instead.
    uint32 const fee = HireFee(player, offer);
    if (fee && !player->HasEnoughMoney(fee))
    {
        message = offer.Name + " asks " + FormatMoney(fee) + ", and you cannot afford that.";
        return false;
    }

    PlayerbotHolder* mgr = GET_PLAYERBOT_MGR(player);
    if (!mgr)
    {
        message = "Your pawn could not be called; no bot manager.";
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
    // earned and the fee it was already charged: without this the next login
    // would re-equip it from scratch and the next recruit would charge again.
    // Only this owner's row counts - a pawn that belonged to someone else was
    // geared against their level and paid for by them. The gear also only
    // survives while the spec is unchanged, since a different spec means the
    // items on it were scored for the old one.
    //
    // Both carry the level with them. A pawn is pinned to the level it was hired
    // at, so taking it on again at a *new* offer level is a new hire - a new
    // build, new gear, a new fee - and that is what makes exchanging companions
    // as you level cost something whether you pick a new face or the same one.
    // Re-hiring at the level it is standing at stays free.
    if (auto it = _rows.find(offer.Bot.GetCounter());
        it != _rows.end() && it->second.Owner == player->GetGUID())
    {
        bool const sameHire = it->second.TargetLevel == offer.Level;
        row.Built = it->second.Built && it->second.SpecNo == offer.SpecNo && sameHire;
        row.Paid = it->second.Paid && sameHire;
    }

    if (fee)
    {
        player->ModifyMoney(-int32(fee));
        row.Paid = true;
    }

    Upsert(row);
    _rows[offer.Bot.GetCounter()] = row;

    mgr->AddPlayerBot(offer.Bot, player->GetSession()->GetAccountId());

    LOG_INFO("module", "Pawn: {} recruited {} ({} {}, level {}) for {}", player->GetName(),
             offer.Name, offer.SpecName, RoleName(offer.Role), uint32(offer.Level),
             fee ? FormatMoney(fee) : std::string("free"));
    message = offer.Name + " joins you as a " + offer.SpecName + " " + RoleName(offer.Role) + ".";
    if (fee)
        message += " The innkeeper takes " + FormatMoney(fee) + ".";
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

    if (sPawnArchetype.IsForbidden(bot->getClass(), it->second.SpecNo, spellId))
        return true;

    // The pawn's own variant: an ability the spec may have but this pawn was not
    // dealt. Deterministic, so the gate and the build always agree.
    for (PawnVariantSpell const& variant : WithheldVariants(it->second.Owner, it->second.Bot,
                                                            bot->getClass(), it->second.SpecNo))
        if (std::find(variant.Ids.begin(), variant.Ids.end(), spellId) != variant.Ids.end())
            return true;

    return false;
}

void PawnMgr::Upsert(PawnRosterRow const& row)
{
    CharacterDatabase.Execute(
        "REPLACE INTO pawn_roster (owner_guid, bot_guid, spec_no, role, state, target_level, built, paid, mount_spell, pet_name) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        row.Owner.GetCounter(), row.Bot.GetCounter(), uint32(row.SpecNo), uint32(row.Role),
        row.Recruited ? 1 : 0, uint32(row.TargetLevel), row.Built ? 1 : 0, row.Paid ? 1 : 0,
        row.MountSpell, row.PetName);
}
