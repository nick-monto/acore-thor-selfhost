/*
 * mod-runewords - the word table, the pattern matcher and the aura carrier.
 *
 * Nothing here is per-player-tick work: RunewordScripts drives Reconcile on a
 * configurable interval, and Reconcile re-derives everything from the bags. That
 * is deliberate. A missed event - a bag swap, a mailed rune, an item destroyed by
 * a GM command - can then never leave a stale aura behind, because the applied
 * state is compared against the desired state, not against a log of what
 * happened.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

#include "runewords_common.h"

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Runewords
{
    namespace
    {
        // The client draws every bag four columns wide and fills it row-major
        // (FrameXML ContainerFrame.lua: NUM_CONTAINER_COLUMNS = 4, `rows =
        // ceil(size / columns)`, a new row every fourth slot). A pattern that
        // assumes any other width would address the cells the player is not
        // looking at.
        constexpr uint8 GridColumns = 4;

        // CONTAINER_FIELD_SLOT_1 is 72 fields: eighteen rows of four is the
        // tallest grid a pattern can address.
        constexpr uint8 GridRows = 18;

        // The add-on's carrier, as mod-survivor spells its own: the prefix names
        // the module, and the tab is what separates it from the payload.
        constexpr char const* kAddonPrefix = "RUNE\t";

        struct Cell
        {
            uint8 row = 0;
            uint8 col = 0;
            uint32 entry = 0;
        };

        struct Word
        {
            uint32 id = 0;
            std::string name;
            std::vector<Cell> cells;
            std::array<uint32, 3> spells{};
            uint16 scale = 100;
            uint8 minPct = 50;
            uint8 requiredLevel = 1;
        };

        struct Applied
        {
            uint32 wordId = 0;
            uint8 pct = 0;
            std::vector<uint32> spells;
        };

        // A container's rune cells: slot index within that container -> item.
        // Only rune items are kept; everything else in the bag is irrelevant to
        // the matcher and is never touched.
        struct RuneGrid
        {
            uint32 size = 0;
            uint8 bag = 0;      // the client's numbering: 0 backpack, 1..4 bags
            std::unordered_map<uint16, Item*> cells;
        };

        Config g_config;
        std::vector<Word> g_words;
        std::unordered_set<uint32> g_runeEntries;
        uint8 g_minRequiredLevel = 1;

        std::unordered_set<uint32> g_warned;                 // spells already reported

        // One line per offending spell, not per player per scan: a bad word table
        // would otherwise flood the log for every player every interval.
        template <typename... Args>
        void WarnOnce(uint32 spellId, char const* format, Args const&... args)
        {
            if (g_warned.insert(spellId).second)
                LOG_ERROR("module", format, args...);
        }

        std::unordered_map<ObjectGuid, Applied> g_applied;   // player -> auras we cast
        std::unordered_map<ObjectGuid, uint8> g_rolls;       // rune item -> roll 1..100
        std::unordered_map<ObjectGuid, uint32> g_sweepAccumulator;   // player -> ms since last scan

        // Every piece of state above is touched from more than one thread, and not only when
        // MapUpdate.Threads is raised: OnPlayerUpdate runs on a map-update worker, while
        // OnPlayerLogout and '.runewords reload' run on the world thread. Concurrent
        // operator[] or erase can rehash under a reader, and LoadWords can clear g_words while
        // a scan is walking it - a crash, not a glitch. Recursive because the entry points
        // call each other; mod-survivor guards its own cache the same way.
        std::recursive_mutex& StateMutex()
        {
            static std::recursive_mutex mutex;
            return mutex;
        }

        bool ParsePattern(std::string const& text, std::vector<Cell>& out)
        {
            out.clear();
            size_t begin = 0;
            while (begin <= text.size())
            {
                size_t end = text.find(',', begin);
                std::string const cell =
                    text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!cell.empty())
                {
                    unsigned row = 0, col = 0, entry = 0;
                    if (std::sscanf(cell.c_str(), "%u:%u:%u", &row, &col, &entry) != 3)
                    {
                        LOG_ERROR("module", "Runewords: word pattern cell '{}' is not row:col:entry", cell);
                        return false;
                    }
                    if (row >= GridRows || col >= GridColumns)
                    {
                        LOG_ERROR("module", "Runewords: word pattern cell '{}' is outside the {}x{} grid",
                                  cell, GridRows, GridColumns);
                        return false;
                    }
                    out.push_back(Cell{uint8(row), uint8(col), uint32(entry)});
                }
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            return !out.empty();
        }

        // The roll of one rune instance. Read from the characters database once,
        // then kept; a rune seen for the first time gets a roll and keeps it for
        // the item's whole life, including through the auction house and the mail.
        uint8 RollFor(ObjectGuid itemGuid)
        {
            auto cached = g_rolls.find(itemGuid);
            if (cached != g_rolls.end())
                return cached->second;

            uint8 roll = 0;
            if (QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                    "SELECT `roll` FROM `character_runeword_rune` WHERE `itemGuid` = {} LIMIT 1",
                    itemGuid.GetRawValue())))
            {
                roll = result->Fetch()[0].Get<uint8>();
            }

            if (!roll || roll > 100)
            {
                roll = uint8(urand(1, 100));
                CharacterDatabase.DirectExecute(Acore::StringFormat(
                    "INSERT IGNORE INTO `character_runeword_rune` (`itemGuid`, `roll`, `rolled`) "
                    "VALUES ({}, {}, UNIX_TIMESTAMP())",
                    itemGuid.GetRawValue(), uint32(roll)));
            }

            g_rolls[itemGuid] = roll;
            return roll;
        }

        bool IsRuneEntry(uint32 entry)
        {
            return g_runeEntries.find(entry) != g_runeEntries.end();
        }

        void CollectRuneGrids(Player* player, std::vector<RuneGrid>& grids)
        {
            grids.clear();

            RuneGrid backpack;
            backpack.size = uint32(INVENTORY_SLOT_ITEM_END) - uint32(INVENTORY_SLOT_ITEM_START);
            backpack.bag = 0;
            for (uint16 index = 0; index < backpack.size; ++index)
            {
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(INVENTORY_SLOT_ITEM_START) + uint8(index)))
                    if (IsRuneEntry(item->GetEntry()))
                        backpack.cells[index] = item;
            }
            if (!backpack.cells.empty())
                grids.push_back(std::move(backpack));

            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            {
                Bag* bag = player->GetBagByPos(slot);
                if (!bag)
                    continue;

                RuneGrid grid;
                grid.size = bag->GetBagSize();
                // Equipment slot 19..22 is client container 1..4, in that order.
                grid.bag = uint8(slot - INVENTORY_SLOT_BAG_START + 1);
                for (uint16 index = 0; index < grid.size; ++index)
                {
                    if (Item* item = bag->GetItemByPos(uint8(index)))
                        if (IsRuneEntry(item->GetEntry()))
                            grid.cells[index] = item;
                }
                if (!grid.cells.empty())
                    grids.push_back(std::move(grid));
            }
        }

        // Try one word against one container. `pct` comes back as the average of
        // its rune rolls, floored by the word's own minimum.
        bool MatchWord(Word const& word, RuneGrid const& grid, uint8& pct)
        {
            bool matched = false;
            for (auto const& [slot, item] : grid.cells)
            {
                // Which cell of the word could this rune be? Trying each in turn
                // means a word is found wherever the player drew it: nothing
                // anchors the pattern to a corner.
                for (size_t anchor = 0; anchor < word.cells.size(); ++anchor)
                {
                    Cell const& cell = word.cells[anchor];
                    if (cell.entry != item->GetEntry())
                        continue;

                    // The grid is four wide, so a flat index alone does not describe a shape:
                    // slot 3 is the right edge of a row and slot 4 the left edge of the next,
                    // and a pattern that ran from one to the other would form a word the
                    // player never drew. The anchor fixes the origin's column, and every cell
                    // has to stay inside that column's row.
                    int32 const originCol = int32(slot % GridColumns) - int32(cell.col);
                    if (originCol < 0)
                        continue;

                    int32 const origin = int32(slot) - int32(cell.row) * GridColumns - int32(cell.col);
                    if (origin < 0)
                        continue;

                    bool complete = true;
                    uint32 rollSum = 0;
                    for (Cell const& expected : word.cells)
                    {
                        int32 const index = origin + int32(expected.row) * GridColumns + int32(expected.col);
                        if (index < 0 || index >= int32(grid.size) ||
                            originCol + int32(expected.col) >= int32(GridColumns))
                        {
                            complete = false;      // the pattern would run off the grid
                            break;
                        }
                        auto const found = grid.cells.find(uint16(index));
                        if (found == grid.cells.end() || found->second->GetEntry() != expected.entry)
                        {
                            complete = false;
                            break;
                        }
                        rollSum += RollFor(found->second->GetGUID());
                    }

                    if (complete)
                    {
                        // The word is worth the average of its runes, rounded
                        // half up: one poor rune costs a share of the word
                        // rather than all of it, and two perfect runes are worth
                        // more than one perfect and one poor. `minPct` stays a
                        // floor, so a word of all-1 rolls is still not nothing.
                        uint32 const count = uint32(word.cells.size());
                        uint8 const average = uint8(std::min<uint32>(100, (rollSum + count / 2) / count));
                        uint8 const value = std::max(word.minPct, average);

                        // One word can fit a grid in more than one place. Taking
                        // the strongest placement makes the amount the player
                        // gets independent of the order the cells were walked
                        // in, which is a hash order and nothing else.
                        if (!matched || value > pct)
                        {
                            pct = value;
                            matched = true;
                        }
                    }
                }
            }
            return matched;
        }

        // An add-on whisper to the player themselves, the same carrier
        // mod-survivor uses for its map pin: 3.3.5 has no other channel the
        // server can push to a single client on.
        void SendAddon(Player* player, std::string const& what)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
                                         std::string(kAddonPrefix) + what);
            player->SendDirectMessage(&data);
        }

        // Every rune in the player's containers with its roll. Chunked because
        // an add-on message is one chat line and a bag can hold twenty slots;
        // the add-on merges the lines, having cleared them when it asked.
        void SendSlots(Player* player)
        {
            std::vector<RuneGrid> grids;
            CollectRuneGrids(player, grids);

            std::string line = "slots";
            bool sent = false;
            auto flush = [&]()
            {
                SendAddon(player, line);
                line = "slots";
                sent = true;
            };

            for (RuneGrid const& grid : grids)
                for (auto const& [slot, item] : grid.cells)
                {
                    std::string token = Acore::StringFormat(" {}:{}:{}:{}", uint32(grid.bag), uint32(slot),
                                                            uint32(RollFor(item->GetGUID())), item->GetEntry());
                    if (line.size() + token.size() > 200)
                        flush();
                    line += token;
                }

            if (line != "slots" || !sent)
                flush();
        }

        // Cast the word's stock auras scaled by `scale` and the rune roll.
        //
        // SpellEffectInfo::CalcValue takes custom basepoints in place of the DBC
        // value and then applies the same die roll the DBC would have
        // (SpellInfo.cpp:412 and the `case 1: basePoints += 1` at 437), so the
        // reference value here is basepoints plus the average die roll, and the
        // cast subtracts that average again. The result is a spell whose average
        // output is exactly the stock value scaled by the two percentages.
        void ApplyWord(Player* player, Word const& word, uint8 pct, Applied& applied)
        {
            double const factor = double(word.scale) / 100.0 * double(pct) / 100.0;

            for (uint32 spellId : word.spells)
            {
                if (!spellId)
                    continue;

                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info)
                {
                    WarnOnce(spellId, "Runewords: word '{}' names spell {}, which is not in Spell.dbc",
                             word.name, spellId);
                    continue;
                }

                // Only a permanent aura belongs here. A word is granted once and
                // re-derived only when the pattern changes, so a spell with a
                // duration would lapse and never be recast - the word would appear
                // to stop working a minute after it formed.
                if (info->GetDuration() != -1)
                {
                    WarnOnce(spellId, "Runewords: word '{}' names spell {} ({}), whose duration is not permanent; "
                                      "it would lapse and never be recast, so it is not granted",
                             word.name, spellId, info->SpellName[0]);
                    continue;
                }

                int32 basepoints[3] = {0, 0, 0};
                for (uint8 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
                {
                    SpellEffectInfo const& info2 = info->Effects[effect];
                    // What CalcValue adds to a custom basepoint: nothing for DieSides 0, +1 for
                    // DieSides 1, and a roll of 1..DieSides otherwise (SpellInfo.cpp:435-455).
                    // The mean of that roll is subtracted back out below, so the value the cast
                    // ends up with is the stock average scaled by `factor` and nothing else.
                    double const average = info2.DieSides == 0 ? 0.0 : (double(info2.DieSides) + 1.0) / 2.0;
                    double const reference = double(info2.BasePoints) + average;
                    basepoints[effect] = int32(std::lround(reference * factor - average));
                }

                // Remove before casting: a word's auras are saved in
                // character_aura with the player, so after a relog the same spell
                // may already be up - with the previous roll's amount. Removing
                // first makes ApplyWord idempotent and makes the amount always
                // the one this roll produces.
                player->RemoveAura(spellId);
                player->CastCustomSpell(player, spellId, &basepoints[0], &basepoints[1], &basepoints[2], true);
                applied.spells.push_back(spellId);
            }

            applied.wordId = word.id;
            applied.pct = pct;
        }

        void RemoveWord(Player* player, Applied& applied)
        {
            for (uint32 spellId : applied.spells)
                player->RemoveAura(spellId);

            applied.spells.clear();
            applied.wordId = 0;
            applied.pct = 0;
        }
    }

    Config GetConfig()
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        return g_config;
    }

    void SetConfig(Config const& config)
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        g_config = config;
    }

    bool DueForScan(Player* player, uint32 diff)
    {
        if (!player)
            return false;

        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        if (!g_config.Enable)
            return false;

        uint32& elapsed = g_sweepAccumulator[player->GetGUID()];
        elapsed += diff;
        if (elapsed < g_config.SweepIntervalMs)
            return false;
        elapsed = 0;
        return true;
    }

    void ForgetScanState(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        g_sweepAccumulator.erase(guid);
    }

    void EnsureSchema()
    {
        // Same reasoning as mod-survivor's tables: a module SQL file that fails
        // aborts the whole boot on this fork, while a plain CREATE TABLE IF NOT
        // EXISTS at startup only logs. The row is written the first time a rune
        // is seen, so an item looted before the module was deployed still gets
        // its roll on the next scan.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_runeword_rune` ("
            "`itemGuid` BIGINT UNSIGNED NOT NULL, "
            "`roll` TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "`rolled` INT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`itemGuid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }

    void LoadWords()
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        g_words.clear();
        g_runeEntries.clear();
        g_minRequiredLevel = 1;
        g_applied.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT `id`, `name`, `pattern`, `spell_1`, `spell_2`, `spell_3`, "
            "`scale`, `min_pct`, `required_level` FROM `runeword_words` ORDER BY `id`");
        if (!result)
        {
            LOG_WARN("module", "Runewords: runeword_words is empty; no word can form. "
                               "Apply sql/custom/db_world/runewords_20_words.sql.");
            return;
        }

        bool first = true;
        do
        {
            Field* fields = result->Fetch();
            Word word;
            word.id = fields[0].Get<uint32>();
            word.name = fields[1].Get<std::string>();
            word.spells = {fields[3].Get<uint32>(), fields[4].Get<uint32>(), fields[5].Get<uint32>()};
            word.scale = fields[6].Get<uint16>();
            word.minPct = fields[7].Get<uint8>();
            word.requiredLevel = fields[8].Get<uint8>();

            if (!ParsePattern(fields[2].Get<std::string>(), word.cells))
            {
                LOG_ERROR("module", "Runewords: word {} ('{}') has an unusable pattern and is skipped",
                          word.id, word.name);
                continue;
            }
            if (!word.spells[0] && !word.spells[1] && !word.spells[2])
            {
                LOG_ERROR("module", "Runewords: word {} ('{}') grants no spells and is skipped",
                          word.id, word.name);
                continue;
            }

            for (Cell const& cell : word.cells)
                g_runeEntries.insert(cell.entry);

            g_minRequiredLevel = first ? word.requiredLevel : std::min(g_minRequiredLevel, word.requiredLevel);
            first = false;
            g_words.push_back(std::move(word));
        } while (result->NextRow());

        LOG_INFO("module", "Runewords: loaded {} words over {} distinct rune items (minimum level {})",
                 g_words.size(), g_runeEntries.size(), uint32(g_minRequiredLevel));
    }

    void Reconcile(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;

        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        if (!g_config.Enable || g_words.empty())
            return;

        Applied& applied = g_applied[player->GetGUID()];

        if (!player->IsAlive() && !g_config.ApplyWhenDead)
        {
            RemoveWord(player, applied);
            return;
        }
        if (player->GetLevel() < g_minRequiredLevel)
        {
            RemoveWord(player, applied);
            return;
        }

        Word const* best = nullptr;
        uint8 bestPct = 0;

        std::vector<RuneGrid> grids;
        CollectRuneGrids(player, grids);
        for (RuneGrid const& grid : grids)
        {
            for (Word const& word : g_words)
            {
                if (player->GetLevel() < word.requiredLevel)
                    continue;

                uint8 pct = 0;
                if (!MatchWord(word, grid, pct))
                    continue;

                // Longest wins, then lowest id: independent of the order the
                // containers and slots happen to be walked in.
                if (!best || word.cells.size() > best->cells.size() ||
                    (word.cells.size() == best->cells.size() && word.id < best->id))
                {
                    best = &word;
                    bestPct = pct;
                }
            }
        }

        if (!best)
        {
            RemoveWord(player, applied);
            return;
        }

        // Already exactly what the bags say - but only trust that while the aura is
        // still there: a dispel, a purge or a shapeshift can strip it without the
        // pattern moving, and this is the one cheap check that notices.
        if (applied.wordId == best->id && applied.pct == bestPct && !applied.spells.empty() &&
            player->HasAura(applied.spells.front()))
            return;

        RemoveWord(player, applied);
        ApplyWord(player, *best, bestPct, applied);

        if (g_config.Announce && applied.wordId)
        {
            if (WorldSession* session = player->GetSession())
                // Acore::StringFormat is fmt (braces), not printf: a %s here
                // printed the format string back at the player.
                ChatHandler(session).PSendSysMessage("|cffd8b000Runeword formed:|r {} |cff808080({}%)|r",
                                                     best->name, uint32(bestPct));
        }

        LOG_DEBUG("module", "Runewords: player {} formed '{}' at {}% with {} spell(s)",
                  player->GetGUID().ToString(), best->name, uint32(bestPct), applied.spells.size());
    }

    void ForgetPlayer(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        g_applied.erase(guid);
    }

    void ClearApplied(Player* player)
    {
        if (!player)
            return;

        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        auto it = g_applied.find(player->GetGUID());
        if (it != g_applied.end())
            RemoveWord(player, it->second);
    }

    bool HandleAddonChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver)
    {
        if (!player || language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
            return true;

        std::lock_guard<std::recursive_mutex> guard(StateMutex());

        if (msg.rfind(kAddonPrefix, 0) != 0)
            return true;

        if (msg == std::string(kAddonPrefix) + "sync")
            SendSlots(player);

        return false;
    }

    void ClearWordAuras(Player* player)
    {
        if (!player)
            return;

        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        for (Word const& word : g_words)
            for (uint32 spellId : word.spells)
                if (spellId)
                    player->RemoveAura(spellId);
    }

    std::string DescribeWords()
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        if (g_words.empty())
            return "no words loaded";

        std::string out = Acore::StringFormat("{} word(s) over {} rune items\n", g_words.size(),
                                             g_runeEntries.size());
        for (Word const& word : g_words)
        {
            out += Acore::StringFormat("#{} {:<10} {:2} rune(s)  level {:>2}  scale {:>3}%  min {:>3}%  spells",
                                      word.id, word.name, word.cells.size(), uint32(word.requiredLevel),
                                      uint32(word.scale), uint32(word.minPct));
            for (uint32 spellId : word.spells)
                if (spellId)
                    out += Acore::StringFormat(" {}", spellId);
            out += "\n";
        }
        return out;
    }

    size_t CountWords()
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        return g_words.size();
    }

    // What one rune instance is worth at the vendor: the template's SellPrice is
    // the rarity's floor, and the roll pays up to that floor again, so a perfect
    // rune is worth twice a zero-roll one of its colour.
    void PayRollBonus(Player* player, Item* item)
    {
        if (!player || !item || !g_config.Enable)
            return;

        {
            std::lock_guard<std::recursive_mutex> guard(StateMutex());
            if (!g_runeEntries.count(item->GetEntry()))
                return;
        }

        // The core checks the same conditions before it pays the template price -
        // this hook runs first, so anything that would make the sale fail has to
        // fail here too, or the roll would have paid for a sale that never was.
        if (player->GetGUID() != item->GetOwnerGUID() || item->IsNotEmptyBag() || item->IsRefundable() ||
            player->GetLootGUID() == item->GetGUID())
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !proto->SellPrice)
            return;

        uint32 count = item->GetCount();
        if (!count)
            return;

        uint8 roll = 0;
        {
            std::lock_guard<std::recursive_mutex> guard(StateMutex());
            roll = RollFor(item->GetGUID());
        }
        uint64 const base = uint64(proto->SellPrice) * count;
        uint64 const bonus = base * roll / 100;
        if (player->GetMoney() + base + bonus > MAX_MONEY_AMOUNT)
            return;                                  // the core refuses this sale anyway

        player->ModifyMoney(int32(bonus));
        LOG_DEBUG("module", "Runewords: {} sold rune {} x{} at {}% for a {} copper roll bonus",
                  player->GetGUID().ToString(), item->GetEntry(), count, uint32(roll), bonus);
    }

    std::string DescribePlayer(Player* player)
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        if (!player)
            return "no player";

        auto const applied = g_applied.find(player->GetGUID());
        std::string out;
        if (applied == g_applied.end() || !applied->second.wordId)
            out = "no active runeword\n";
        else
            out = Acore::StringFormat("active word #{} at {}% with {} spell(s)\n", applied->second.wordId,
                                      uint32(applied->second.pct), applied->second.spells.size());

        std::vector<RuneGrid> grids;
        CollectRuneGrids(player, grids);
        if (grids.empty())
            return out + "no runes in the bags\n";

        // A legend, because the cells are diagnostics: the index is the client's own container
        // slot minus one (the numbering the pattern is written in), and the two numbers after the
        // item are the roll that scales the word and what a vendor would pay for it.
        out += "cells are container:slot (0-based) name entry@roll% (vendor price)\n";
        for (size_t index = 0; index < grids.size(); ++index)
        {
            out += Acore::StringFormat("container {} of {} slot(s):", index + 1, grids[index].size);
            for (auto const& [slot, item] : grids[index].cells)
            {
                uint8 const roll = RollFor(item->GetGUID());
                ItemTemplate const* proto = item->GetTemplate();
                uint32 const price = proto ? proto->SellPrice : 0;
                out += Acore::StringFormat(" [{}]={} {}@{}% ({}c)", uint32(slot),
                                          proto ? proto->Name1 : "?", item->GetEntry(), uint32(roll),
                                          price + price * uint32(roll) / 100);
            }
            out += "\n";
        }
        return out;
    }
}
