/*
 * mod-runewords - the word table, the pattern matcher and the aura carrier.
 *
 * A word is a line of runes across or down, matched as a maximal run, so the
 * same rune can serve a word across and a word down at once.
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
        // The client draws every bag four columns wide (FrameXML ContainerFrame.lua:
        // NUM_CONTAINER_COLUMNS = 4). A pattern that assumes any other width would address the
        // cells the player is not looking at.
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
            std::vector<uint32> entries;   // the cells' entries, in reading order
            bool horizontal = true;        // the axis the line is written on
            std::array<uint32, 3> spells{};
            uint16 scale = 100;
            uint8 requiredLevel = 1;
        };

        // What one word is currently granting. A player holds as many of these as
        // their bags form at once.
        struct AppliedWord
        {
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
        // axis and entries -> the word written that way. A word *is* its line, so this is a
        // whole lookup rather than a shape to be searched for.
        std::unordered_map<std::string, Word const*> g_lines;
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

        // player -> (word id -> what we cast for it). Several at once is the point: the
        // bags are the budget, so filling them with runes is meant to pay off.
        std::unordered_map<ObjectGuid, std::unordered_map<uint32, AppliedWord>> g_applied;
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

        // How many rows of four the client draws for a container of `size` slots.
        uint32 GridRowCount(uint32 size)
        {
            return (size + GridColumns - 1) / GridColumns;
        }

        // The slot the client draws at (row, col), with row 0 the top row and col 0 the left
        // column. Measured from the container frames' own buttons: a bag fills from the bottom
        // right, leftwards and upwards, so a container whose slot count is not a multiple of four
        // keeps its short row at the *top*, right-aligned, and (rows-1-row)*4 + (3-col) is the
        // whole of the layout. Cells the container does not have answer with a slot it never
        // fills, so a caller only ever has to check emptiness.
        uint32 SlotAt(uint32 size, uint32 row, uint32 col)
        {
            uint32 const rows = GridRowCount(size);
            if (row >= rows || col >= GridColumns)
                return size;
            return (rows - 1 - row) * GridColumns + (GridColumns - 1 - col);
        }

        // What a line is looked up by: the axis and the runes in reading order. A word is a
        // straight line across or down, so that pair is the whole of its shape.
        std::string LineKey(bool horizontal, std::vector<uint32> const& entries)
        {
            std::string key(horizontal ? "H" : "V");
            for (uint32 entry : entries)
                key += Acore::StringFormat(":{}", entry);
            return key;
        }

        // Every word one container forms. A cell may stand in a word across and in a word down at
        // the same time - sharing a rune between two words is the point of the table - so nothing
        // here consumes or marks a cell, and two words are read off the same grid independently.
        void ScanGrid(RuneGrid const& grid, uint8 playerLevel,
                      std::unordered_map<uint32, std::pair<Word const*, uint8>>& matched)
        {
            uint32 const rows = GridRowCount(grid.size);

            auto ItemAt = [&grid](uint32 row, uint32 col) -> Item*
            {
                auto const found = grid.cells.find(uint16(SlotAt(grid.size, row, col)));
                return found == grid.cells.end() ? nullptr : found->second;
            };

            // One maximal run, read off the grid in its own reading order: left to right across,
            // top to bottom down. The roll average is the same rule the add-on applies.
            auto TryRun = [&](bool horizontal, uint32 row, uint32 col, uint32 length)
            {
                std::vector<uint32> entries;
                entries.reserve(length);
                uint32 rollSum = 0;
                for (uint32 step = 0; step < length; ++step)
                {
                    Item* const item = horizontal ? ItemAt(row, col + step) : ItemAt(row + step, col);
                    entries.push_back(item->GetEntry());
                    rollSum += RollFor(item->GetGUID());
                }

                auto const line = g_lines.find(LineKey(horizontal, entries));
                if (line == g_lines.end())
                    return;
                Word const& word = *line->second;
                if (playerLevel < word.requiredLevel)
                    return;

                uint8 const value = uint8(std::min<uint32>(100, (rollSum + length / 2) / length));
                auto& slot = matched[word.id];
                if (!slot.first || value > slot.second)
                    slot = {&word, value};
            };

            // Only a *maximal* run is a word: a line may not carry another rune past either end,
            // or a two-rune word would form inside a longer line the player drew for another
            // word. That is also what makes sharing legible - two crossing lines, each exactly
            // its own word, and one rune where they meet.
            for (uint32 row = 0; row < rows; ++row)
            {
                uint32 col = 0;
                while (col < GridColumns)
                {
                    if (!ItemAt(row, col))
                    {
                        ++col;
                        continue;
                    }
                    uint32 end = col;
                    while (end < GridColumns && ItemAt(row, end))
                        ++end;
                    if (end - col >= 2)
                        TryRun(true, row, col, end - col);
                    col = end + 1;      // the cell at `end` is empty, or past the last column
                }
            }

            for (uint32 col = 0; col < GridColumns; ++col)
            {
                uint32 row = 0;
                while (row < rows)
                {
                    if (!ItemAt(row, col))
                    {
                        ++row;
                        continue;
                    }
                    uint32 end = row;
                    while (end < rows && ItemAt(end, col))
                        ++end;
                    if (end - row >= 2)
                        TryRun(false, row, col, end - row);
                    row = end + 1;
                }
            }
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
        void ApplyWord(Player* player, Word const& word, uint8 pct, AppliedWord& applied)
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

            applied.pct = pct;
        }

        void RemoveWord(Player* player, AppliedWord& applied)
        {
            for (uint32 spellId : applied.spells)
                player->RemoveAura(spellId);

            applied.spells.clear();
            applied.pct = 0;
        }

        void RemoveAllWords(Player* player, std::unordered_map<uint32, AppliedWord>& applied)
        {
            for (auto& entry : applied)
                RemoveWord(player, entry.second);
            applied.clear();
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
        g_lines.clear();
        g_runeEntries.clear();
        g_minRequiredLevel = 1;
        g_applied.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT `id`, `name`, `pattern`, `spell_1`, `spell_2`, `spell_3`, "
            "`scale`, `required_level` FROM `runeword_words` ORDER BY `id`");
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

            word.requiredLevel = fields[7].Get<uint8>();

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

            // A word is a straight line. Two runes on a diagonal are a guess, and a shape the
            // player cannot read is one they cannot aim for, so anything that is not one row or
            // one column, contiguous and in reading order, is refused here rather than matched
            // as something else.
            if (word.cells.size() < 2)
            {
                LOG_ERROR("module", "Runewords: word {} ('{}') has fewer than two runes and is skipped",
                          word.id, word.name);
                continue;
            }
            Cell const& first = word.cells[0];
            bool const horizontal = first.row == word.cells[1].row;
            bool straight = true;
            for (size_t step = 0; step < word.cells.size() && straight; ++step)
            {
                Cell const& cell = word.cells[step];
                straight = horizontal
                    ? (cell.row == first.row && cell.col == first.col + step)
                    : (cell.col == first.col && cell.row == first.row + step);
            }
            if (!straight)
            {
                LOG_ERROR("module", "Runewords: word {} ('{}') is not one line across or down and is skipped",
                          word.id, word.name);
                continue;
            }
            word.horizontal = horizontal;
            word.entries.clear();
            for (Cell const& cell : word.cells)
                word.entries.push_back(cell.entry);

            for (Cell const& cell : word.cells)
                g_runeEntries.insert(cell.entry);

            g_minRequiredLevel = first ? word.requiredLevel : std::min(g_minRequiredLevel, word.requiredLevel);
            first = false;
            g_words.push_back(std::move(word));
        } while (result->NextRow());

        // A second pass, because the map holds pointers into g_words and that vector must not grow
        // again while they are in use. Words arrive in id order, so when two are somehow the same
        // line the lower id keeps it.
        for (Word const& word : g_words)
        {
            if (word.entries.empty())
                continue;
            auto const inserted = g_lines.emplace(LineKey(word.horizontal, word.entries), &word);
            if (!inserted.second)
                LOG_ERROR("module", "Runewords: words {} ('{}') and {} ('{}') are the same line of "
                                    "runes; only the first is kept",
                          inserted.first->second->id, inserted.first->second->name, word.id, word.name);
        }

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

        auto& applied = g_applied[player->GetGUID()];

        if (!player->IsAlive() && !g_config.ApplyWhenDead)
        {
            RemoveAllWords(player, applied);
            return;
        }
        if (player->GetLevel() < g_minRequiredLevel)
        {
            RemoveAllWords(player, applied);
            return;
        }

        // Strongest fit for every word the bags form, deduped by word id: two copies of
        // one word in different containers are the same word, not twice the power, and
        // which copy is picked is the one that rolled highest.
        std::unordered_map<uint32, std::pair<Word const*, uint8>> matched;
        std::vector<RuneGrid> grids;
        CollectRuneGrids(player, grids);
        for (RuneGrid const& grid : grids)
            ScanGrid(grid, player->GetLevel(), matched);

        // Which words were already up before this pass. A word whose roll changed is
        // removed and recast below, and that must not read as a formation: swapping one
        // copy of Bulwark for a better-rolled one changes the amount, not the word.
        std::unordered_set<uint32> const wasActive = [&applied]
        {
            std::unordered_set<uint32> ids;
            for (auto const& entry : applied)
                ids.insert(entry.first);
            return ids;
        }();

        // Drop what the bags no longer hold, or whose roll changed: a word is recast
        // when its percentage moves, because the amount is baked into the aura.
        for (auto it = applied.begin(); it != applied.end();)
        {
            auto const found = matched.find(it->first);
            if (found == matched.end() || found->second.second != it->second.pct)
            {
                RemoveWord(player, it->second);
                it = applied.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // A dispel, a purge or a shapeshift can strip an aura without the pattern
        // moving, and this is the one cheap check that notices. A word that grants no
        // spells has nothing to check.
        for (auto it = applied.begin(); it != applied.end();)
        {
            if (!it->second.spells.empty() && !player->HasAura(it->second.spells.front()))
            {
                RemoveWord(player, it->second);
                it = applied.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Ascending id, so two words that share a stock spell (Frenzy and Call to Arms
        // both grant Increased Strength 46) end on the same amount whatever order the
        // hash walked the matches in.
        std::vector<uint32> ids;
        ids.reserve(matched.size());
        for (auto const& [wordId, entry] : matched)
            ids.push_back(wordId);
        std::sort(ids.begin(), ids.end());

        std::vector<std::pair<std::string, uint8>> formed;
        for (uint32 wordId : ids)
        {
            auto const& [word, pct] = matched[wordId];
            if (applied.count(wordId))
                continue;

            AppliedWord& record = applied[wordId];
            ApplyWord(player, *word, pct, record);
            if (!wasActive.count(wordId))
                formed.emplace_back(word->name, pct);

            LOG_DEBUG("module", "Runewords: player {} formed '{}' at {}% with {} spell(s)",
                      player->GetGUID().ToString(), word->name, uint32(pct), record.spells.size());
        }

        if (formed.empty() || !g_config.Announce)
            return;

        WorldSession* session = player->GetSession();
        if (!session)
            return;

        if (formed.size() > 3)
        {
            // A bag full of words is a login, not a moment: one line instead of twenty.
            // Acore::StringFormat is fmt (braces), not printf: a %s here printed the
            // format string back at the player.
            std::string names;
            for (size_t k = 0; k < formed.size() && k < 6; ++k)
                names += (k ? ", " : "") + formed[k].first;
            if (formed.size() > 6)
                names += Acore::StringFormat(", and {} more", formed.size() - 6);
            ChatHandler(session).PSendSysMessage("|cffd8b000Runewords formed:|r {} words ({})",
                                                 formed.size(), names);
        }
        else
        {
            for (auto const& [name, pct] : formed)
                ChatHandler(session).PSendSysMessage("|cffd8b000Runeword formed:|r {} |cff808080({}%)|r",
                                                     name, uint32(pct));
        }
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
        {
            RemoveAllWords(player, it->second);
            g_applied.erase(it);
        }
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
            out += Acore::StringFormat("#{} {:<19} {:>2} rune(s) {:<6} level {:>2}  scale {:>3}%  spells",
                                      word.id, word.name, word.entries.size(),
                                      word.horizontal ? "across" : "down",
                                      uint32(word.requiredLevel), uint32(word.scale));
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

    namespace
    {
        // ---------------------------------------------------------------------------------
        // What a word is worth, in the words a player uses
        // ---------------------------------------------------------------------------------
        //
        // '.runewords status' answers the same question the add-on panel draws, so it computes
        // the same numbers the same way: the stock aura's amount scaled by the word's `scale`
        // and the rune roll. None of this is state - a word's amount is a pure function of the
        // spell, the word and the roll, which is exactly why ApplyWord can be recast safely.

        char const* StatLabel(int32 misc)
        {
            switch (misc)
            {
                case 0: return "Strength";
                case 1: return "Agility";
                case 2: return "Stamina";
                case 3: return "Intellect";
                case 4: return "Spirit";
                default: return nullptr;
            }
        }

        char const* RatingLabel(int32 misc)
        {
            switch (misc)
            {
                case 1: case 4: return "Dodge";
                case 2: return "Defense";
                case 3: return "Parry";
                case 16: return "Block";
                default: return nullptr;
            }
        }

        char const* SchoolLabel(int32 mask)
        {
            switch (mask)
            {
                case 2: return "Holy";
                case 4: return "Fire";
                case 8: return "Nature";
                case 16: return "Frost";
                case 32: return "Shadow";
                case 64: return "Arcane";
                default: return nullptr;
            }
        }

        // The amount the aura ends up carrying at this word's scale and roll - the same
        // arithmetic ApplyWord feeds to CastCustomSpell, read back for the report.
        int32 GrantedAmount(SpellEffectInfo const& effect, double factor)
        {
            double const average = effect.DieSides == 0 ? 0.0 : (double(effect.DieSides) + 1.0) / 2.0;
            return int32(std::lround((double(effect.BasePoints) + average) * factor));
        }

        // What to call an aura: the family comes from the aura type, the member from its misc
        // value. Anything unnamed falls back to the stock spell's name, which is still the
        // truth, just not a friendly one.
        std::string AuraLabel(SpellEffectInfo const& effect, SpellInfo const* info)
        {
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_RESISTANCE:
                    if (effect.MiscValue > 64)
                        return "All res";
                    if (effect.MiscValue == 1)
                        return "Armor";
                    if (char const* school = SchoolLabel(effect.MiscValue))
                        return Acore::StringFormat("{} res", school);
                    break;
                case SPELL_AURA_MOD_STAT:
                    if (char const* stat = StatLabel(effect.MiscValue))
                        return stat;
                    break;
                case SPELL_AURA_MOD_RATING:
                    if (char const* rating = RatingLabel(effect.MiscValue))
                        return rating;
                    break;
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    if (char const* school = SchoolLabel(effect.MiscValue))
                        return Acore::StringFormat("{} damage", school);
                    break;
                case SPELL_AURA_DAMAGE_SHIELD:
                    return "Damage shield";
                case SPELL_AURA_MOD_POWER_REGEN:
                    return "Mana regen";
                case SPELL_AURA_MOD_ATTACK_POWER:
                    return "Attack power";
                case SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT:
                    return "Health regen";
                default:
                    break;
            }
            return info->SpellName[0];
        }

        // A proc carries its payload in the spell it triggers, and the word cannot scale that,
        // so the number to print lives in the trigger and not in the word.
        std::string DescribeProc(SpellInfo const* info)
        {
            uint32 const chance = info->ProcChance;
            SpellInfo const* trigger = sSpellMgr->GetSpellInfo(info->Effects[0].TriggerSpell);
            if (!trigger)
                return Acore::StringFormat("{} ({}%)", info->SpellName[0], chance);

            SpellEffectInfo const& effect = trigger->Effects[0];
            if (effect.BasePoints < 0)
                return Acore::StringFormat("{} (weapon {}%)", trigger->SpellName[0], chance);
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE && effect.BasePoints > 0)
                return Acore::StringFormat("{} {} ({}%)", trigger->SpellName[0], effect.BasePoints, chance);
            return Acore::StringFormat("{} ({}%)", trigger->SpellName[0], chance);
        }

        // One word as a player reads it: the name, the roll that set it, and the numbers it
        // actually applies.
        std::string DescribeWord(Word const& word, uint8 pct)
        {
            double const factor = double(word.scale) / 100.0 * double(pct) / 100.0;

            std::string effects;
            for (uint32 spellId : word.spells)
            {
                if (!spellId)
                    continue;

                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info)
                    continue;

                SpellEffectInfo const& effect = info->Effects[0];
                std::string const part = effect.ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL
                    ? DescribeProc(info)
                    : Acore::StringFormat("{} {}", AuraLabel(effect, info), GrantedAmount(effect, factor));
                effects += (effects.empty() ? "" : ", ") + part;
            }

            // Nineteen characters is the longest word in the table ("Breath of the Dying"), so
            // the percentage column lines up for every row.
            return Acore::StringFormat("{:<19} {:>3}%  {}", word.name, uint32(pct), effects);
        }
    }

    std::string DescribePlayer(Player* player, bool detail)
    {
        std::lock_guard<std::recursive_mutex> guard(StateMutex());
        if (!player)
            return "no player";

        auto const applied = g_applied.find(player->GetGUID());
        std::string out;
        if (applied == g_applied.end() || applied->second.empty())
        {
            out = "no active runeword\n";
        }
        else
        {
            // Ascending id, the order the panel uses: the same bags read the same way twice.
            std::vector<std::pair<uint32, uint8>> active;
            active.reserve(applied->second.size());
            for (auto const& [wordId, record] : applied->second)
                active.emplace_back(wordId, record.pct);
            std::sort(active.begin(), active.end());

            out = Acore::StringFormat("{} active runeword(s)\n", active.size());
            for (auto const& [wordId, pct] : active)
            {
                auto const word = std::find_if(g_words.begin(), g_words.end(),
                                               [wordId](Word const& candidate) { return candidate.id == wordId; });
                if (word != g_words.end())
                    out += "  " + DescribeWord(*word, pct) + "\n";
            }
        }

        // The per-cell dump is a diagnostic - which slot holds which rune, its roll and what a
        // vendor pays - so only a game master asking for it sees it.
        if (!detail)
            return out;

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
