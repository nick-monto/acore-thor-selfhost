/*
 * mod-survivor
 *
 * Opt-in "death is expensive" ruleset, the counterpart of mod-hardcore's
 * permadeath: the character survives the death, but everything it carried is
 * lifted off the body and left in a lootable container at the death position.
 *
 *   - '.survivor on' flags the character (one-way, no '.survivor off'), and
 *     refuses Hardcore-flagged characters, so the two rulesets can never
 *     coexist - a Hardcore death is final, there would be nothing to loot.
 *   - OnPlayerJustDied snapshots every equipped item and every item in the
 *     bags (and the bags themselves) into survivor_corpse_items, destroys the
 *     live items, and summons one chest at the death spot holding one loot
 *     LootItem per saved instance.
 *   - The chest's loot is filled by hand and the gameobject template has
 *     lootId = 0, which is what stops Player::SendLoot from clearing and
 *     regenerating it; consumable = 1 plus the summon respawn time is what
 *     makes the core delete the chest once it has been emptied.
 *   - Any Survivor-flagged character may open it, and nobody else may
 *     (OnAllowedToLootContainerCheck, whose boolean
 *     is inverted in this core), and looting an entry puts the *same* item
 *     instance back - the row is re-inserted under its old guid and loaded
 *     with Item::LoadFromDB, which is the only lossless path (StoreNewItem
 *     regenerates enchants, gems and durability).
 *   - The position is also sent to the player's addon ('SURV\tsite ...'), which
 *     draws the map pin; the addon outlives the corpse and the resurrection.
 *   - The character is then revived at the closest graveyard on its next update:
 *     at full health, without resurrection sickness and without a ghost, so a
 *     death costs the gear and the walk back, never a corpse run.
 *   - Bot-controlled characters are stripped and re-rolled with playerbots'
 *     own factory: same bot, fresh kit for its class, level and specs.
 *
 * The bank, the mail, the gold and the quest log are untouched: the ruleset is
 * gear and bags only.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

// Playerbots fork headers: recognising a headless session, and the factory the
// bot path uses to rebuild a bot's gear. Every module subdirectory is on the
// include path (modules/CMakeLists.txt), which is how mod-hardcore includes
// Playerbots.h as well.
#include "PlayerbotFactory.h"
#include "Playerbots.h"

#include "Survivor.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Survivor
{
    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }
}

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* kAddonPrefix = "SURV\t";

    // One item as it is shown in the container's loot window. The instance
    // itself lives in survivor_corpse_items; LootItem has no room for enchants,
    // gems or durability, so this is display and ordering only.
    struct LootSnapshot
    {
        uint32 itemId = 0;
        uint32 count = 0;
        int32 randomPropertyId = 0;
        uint32 randomSuffix = 0;
    };

    // bag/slot pair of an inventory slot that is about to be taken.
    using SlotRef = std::pair<uint8, uint8>;

    // A container the module created and that is still alive in memory.
    struct LiveContainer
    {
        ObjectGuid guid;        // full gameobject guid (entry + low guid)
        uint32 goLowGuid = 0;   // same value as the GoToCorpse() key
        uint32 mapId = 0;
        uint32 instanceId = 0;
        bool noMail = false;    // rots at expiry: a bot's or a hardcore death's
    };

    //
    // In-memory caches. They are shared between the world/session thread
    // (chat commands through WorldSessionMgr::UpdateSessions, the expiry sweep
    // in WorldScript::OnUpdate) and the map updater thread(s) (the PlayerScript
    // death/update/loot hooks and the gameobject hooks, MapMgr::Update ->
    // MapUpdater, where MapUpdate.Threads may be above its default of 1), so
    // every read and write below is guarded by CacheMutex().
    //
    // The mutex is recursive because the entry points call each other:
    // DenyForeignContainerLoot reads two caches, the container release helpers
    // call ForgetContainer, and HandleBotDeath calls SpawnContainer.
    //
    // Where a cache is only needed to resolve a key, the key (or the value) is
    // copied out under the lock and the database, item and map work runs
    // outside it. Where the cache is the whole point of the call - Flag,
    // LoadFlags, IsFlaggedGuid, DenyForeignContainerLoot - the lock is held for
    // the call, and none of those touch the database or the map while holding
    // it. The database layer itself is already thread-safe.
    //
    std::recursive_mutex& CacheMutex()
    {
        static std::recursive_mutex mutex;
        return mutex;
    }

    std::unordered_set<uint32>& FlaggedGuids()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    // gameobject low guid -> survivor_corpses.id
    std::unordered_map<uint32, uint32>& GoToCorpse()
    {
        static std::unordered_map<uint32, uint32> links;
        return links;
    }

    // survivor_corpses.id -> the live object
    std::unordered_map<uint32, LiveContainer>& CorpseToLive()
    {
        static std::unordered_map<uint32, LiveContainer> live;
        return live;
    }

    // Bots whose gear was taken and that are waiting for their re-roll.
    std::unordered_set<uint32>& PendingBotGear()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    // Survivors whose death was handled and that are waiting for the revive at
    // the closest graveyard. The entry is kept until the character is found
    // again (the next update, a login, or the ghost release), never dropped on
    // a timer: a character that logs out mid-tick is picked up by HandleLogin,
    // and the set can never hold more than one entry per character.
    std::unordered_set<uint32>& PendingRevive()
    {
        static std::unordered_set<uint32> guids;
        return guids;
    }

    // The only message a revived survivor sees, at every entry point.
    // Says only what is true of every survivor death: a death with nothing
    // left to drop wakes here too, and then there is no container to point at.
    // The container hint is the death announcement, which already fires at the
    // place of death.
    constexpr char const* kReviveMessage =
        "|cffff8000[Survivor]|r You wake at the nearest graveyard.";

    uint32 NowSeconds()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    // Seconds a container stands before its items are mailed back.
    uint32 ContainerLifetimeSeconds()
    {
        return Survivor::GetConfig().ContainerLifetimeDays * 86400u;
    }

    // Single-quoted, escaped SQL literal for the few text columns we write.
    std::string Quote(std::string const& value)
    {
        std::string escaped = value;
        CharacterDatabase.EscapeString(escaped);
        return "'" + escaped + "'";
    }

    void SendAddon(Player* player, std::string const& what)
    {
        if (!player || !Survivor::GetConfig().MarkOnMap)
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(kAddonPrefix) + what);
        player->SendDirectMessage(&data);
    }

    // The map marker payload: raw world coordinates, the zone so the addon can
    // label it, and the row id so a later 'none' can clear exactly this pin.
    void SendSite(Player* player, uint32 mapId, float x, float y, uint32 zoneId, uint32 corpseId)
    {
        SendAddon(player, Acore::StringFormat("site {} {} {} {} {}", mapId, x, y, zoneId, corpseId));
    }

    // Everything the ruleset takes off this character, in the order it is
    // taken: the contents of each equipped bag first (so the items inside are
    // still addressable), then the equipment slots (which includes the equipped
    // bags), then the backpack.
    std::vector<SlotRef> CollectDroppableItems(Player* player, Survivor::Config const& cfg)
    {
        std::vector<SlotRef> slots;

        if (cfg.DropBags)
        {
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            {
                Item* bagItem = player->GetItemByPos(uint8(INVENTORY_SLOT_BAG_0), bagSlot);
                if (!bagItem)
                    continue;

                Bag* bag = bagItem->ToBag();
                if (!bag)
                    continue;

                uint32 size = bag->GetBagSize();
                for (uint8 slot = 0; slot < size; ++slot)
                    if (bag->GetItemByPos(slot))
                        slots.emplace_back(bagSlot, slot);
            }
        }

        if (cfg.DropEquipped)
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                // The equipped bags belong to the DropBags half of the ruleset:
                // destroying a bag also destroys what it holds, so with
                // DropBags = 0 they are kept and left out of the sweep.
                if (!cfg.DropBags && slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END)
                    continue;

                if (player->GetItemByPos(uint8(INVENTORY_SLOT_BAG_0), slot))
                    slots.emplace_back(uint8(INVENTORY_SLOT_BAG_0), slot);
            }
        }

        if (cfg.DropBags)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (player->GetItemByPos(uint8(INVENTORY_SLOT_BAG_0), slot))
                    slots.emplace_back(uint8(INVENTORY_SLOT_BAG_0), slot);
        }

        return slots;
    }

    // Inserts the survivor_corpses row and returns its id, or 0 when the row
    // could not be read back.
    uint32 CreateCorpseRow(Player* player, Survivor::Config const& cfg, bool noMail)
    {
        uint32 guidLow = player->GetGUID().GetCounter();
        uint32 now = NowSeconds();

        CharacterDatabase.DirectExecute(
            "INSERT INTO `survivor_corpses` (`guid`, `name`, `map`, `x`, `y`, `z`, `o`, `zone`, `level`, `created_ts`, `expires_ts`, `no_mail`) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
            guidLow, Quote(player->GetName()), player->GetMapId(),
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(),
            player->GetZoneId(), uint32(player->GetLevel()), now, now + ContainerLifetimeSeconds(), noMail);

        // The insert above is a DirectExecute (one pooled connection) and this
        // is a separate query, so the id is read back by guid instead of
        // LAST_INSERT_ID(): the newest row of this character is the one just
        // written, which is also the row a second pre-recovery death wants.
        // created_ts pins the read-back to this very death, so a failed insert
        // can never make the new items land on an older corpse row.
        QueryResult result = CharacterDatabase.Query(
            "SELECT `id` FROM `survivor_corpses` WHERE `guid` = {} AND `created_ts` = {} ORDER BY `id` DESC LIMIT 1",
            guidLow, now);

        if (!result)
        {
            LOG_ERROR("module", "Survivor: could not read back the corpse row of {} (guid {}).", player->GetName(), guidLow);
            return 0;
        }

        return result->Fetch()[0].Get<uint32>();
    }

    // Snapshots and removes the given slots. The item_instance row is written
    // first, the copy into survivor_corpse_items is made from that row, and
    // only then is the live item destroyed - all three in one characters-DB
    // transaction, so a crash can neither lose the instance nor destroy an
    // item that was never saved.
    std::vector<LootSnapshot> SnapshotAndDestroy(Player* player, std::vector<SlotRef> const& slots, uint32 corpseId)
    {
        std::vector<LootSnapshot> snapshot;
        snapshot.reserve(slots.size());

        uint32 slotIndex = 0;
        for (SlotRef const& ref : slots)
        {
            Item* item = player->GetItemByPos(ref.first, ref.second);
            if (!item)
                continue;

            LootSnapshot entry;
            entry.itemId = item->GetEntry();
            entry.count = item->GetCount();
            entry.randomPropertyId = item->GetItemRandomPropertyId();
            entry.randomSuffix = item->GetItemSuffixFactor();

            auto trans = CharacterDatabase.BeginTransaction();
            item->SaveToDB(trans);
            CharacterDatabase.ExecuteOrAppend(trans, Acore::StringFormat(
                "INSERT INTO `survivor_corpse_items` (`corpse_id`, `slot_index`, `item_guid`, `itemEntry`, "
                "`creatorGuid`, `giftCreatorGuid`, `count`, `duration`, `charges`, `flags`, `enchantments`, "
                "`randomPropertyId`, `durability`, `playedTime`, `text`) "
                "SELECT {}, {}, `guid`, `itemEntry`, `creatorGuid`, `giftCreatorGuid`, `count`, `duration`, "
                "`charges`, `flags`, `enchantments`, `randomPropertyId`, `durability`, `playedTime`, `text` "
                "FROM `item_instance` WHERE `guid` = {}",
                corpseId, slotIndex, item->GetGUID().GetCounter()));
            CharacterDatabase.DirectCommitTransaction(trans);

            player->DestroyItem(ref.first, ref.second, true);

            snapshot.push_back(entry);
            ++slotIndex;
        }

        return snapshot;
    }

    // Removes the given slots without keeping anything. Used by the bot path,
    // which destroys rather than drops.
    void DestroyDroppedItems(Player* player, std::vector<SlotRef> const& slots)
    {
        for (SlotRef const& ref : slots)
            if (player->GetItemByPos(ref.first, ref.second))
                player->DestroyItem(ref.first, ref.second, true);
    }

    // One LootItem per saved instance. itemIndex must match the vector index
    // (NotifyItemRemoved maps the loot slot back with it) and unlootedCount has
    // to be incremented by hand - the core only does that inside Loot::AddItem,
    // and isLooted() is "gold == 0 && unlootedCount == 0".
    void FillContainerLoot(GameObject* go, std::vector<LootSnapshot> const& snapshot)
    {
        go->loot.clear();
        go->loot.loot_type = LOOT_CORPSE;
        go->loot.sourceWorldObjectGUID = go->GetGUID();
        go->loot.sourceGameObject = go;

        uint32 index = 0;
        for (LootSnapshot const& entry : snapshot)
        {
            LootItem lootItem;
            lootItem.itemid = entry.itemId;
            lootItem.count = entry.count;
            lootItem.randomPropertyId = entry.randomPropertyId;
            lootItem.randomSuffix = entry.randomSuffix;
            lootItem.itemIndex = index;
            lootItem.is_looted = false;
            lootItem.is_blocked = false;
            lootItem.freeforall = false;
            lootItem.is_underthreshold = false;
            lootItem.is_counted = false;
            lootItem.needs_quest = false;
            lootItem.follow_loot_rules = false;
            lootItem.groupid = 0;

            go->loot.items.push_back(lootItem);
            ++index;
        }

        go->loot.unlootedCount = uint8(std::min<size_t>(snapshot.size(), std::numeric_limits<uint8>::max()));
    }

    // Summons the container, hands it to the owner and fills its loot. Returns
    // nullptr when the template is missing or the map refused the summon.
    GameObject* SpawnContainer(Map* map, ObjectGuid owner, uint32 corpseId, float x, float y, float z, float o,
                               std::vector<LootSnapshot> const& snapshot, bool noMail)
    {
        Survivor::Config const& cfg = Survivor::GetConfig();
        if (!map || !cfg.ContainerEntry || snapshot.empty())
            return nullptr;

        GameObject* go = map->SummonGameObject(cfg.ContainerEntry, x, y, z, o, 0.0f, 0.0f, 0.0f, 0.0f,
                                               ContainerLifetimeSeconds());
        if (!go)
        {
            LOG_ERROR("module", "Survivor: could not summon container entry {} for corpse {}.", cfg.ContainerEntry, corpseId);
            return nullptr;
        }

        // Ownership is enforced in code (OnAllowedToLootContainerCheck); no
        // gameobject flag can express "this player and nobody else".
        go->SetOwnerGUID(owner);
        FillContainerLoot(go, snapshot);

        // The two maps are the only cached state here; register under the lock
        // after the summon, so no map or database call runs while holding it.
        LiveContainer live;
        live.guid = go->GetGUID();
        live.goLowGuid = go->GetGUID().GetCounter();
        live.mapId = map->GetId();
        live.instanceId = map->GetInstanceId();
        live.noMail = noMail;

        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            GoToCorpse()[live.goLowGuid] = corpseId;
            CorpseToLive()[corpseId] = live;
        }

        return go;
    }

    void ForgetContainer(uint32 goLowGuid, uint32 corpseId)
    {
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        GoToCorpse().erase(goLowGuid);
        CorpseToLive().erase(corpseId);
    }

    // Loads the remaining instances of a corpse row into loot order. The
    // persisted table has no suffix factor column (item_instance has none
    // either), so a re-summoned container shows the item without one.
    std::vector<LootSnapshot> LoadSnapshot(uint32 corpseId)
    {
        std::vector<LootSnapshot> snapshot;

        QueryResult items = CharacterDatabase.Query(
            "SELECT `itemEntry`, `count`, `randomPropertyId` FROM `survivor_corpse_items` "
            "WHERE `corpse_id` = {} ORDER BY `slot_index` ASC", corpseId);

        if (!items)
            return snapshot;

        do
        {
            Field* fields = items->Fetch();
            LootSnapshot entry;
            entry.itemId = fields[0].Get<uint32>();
            entry.count = fields[1].Get<uint32>();
            entry.randomPropertyId = fields[2].Get<int16>();
            entry.randomSuffix = 0;
            snapshot.push_back(entry);
        } while (items->NextRow());

        return snapshot;
    }

    // Re-creates one saved instance in item_instance and returns it, or
    // nullptr. The row is rebuilt with SQL (a straight column-for-column copy
    // out of survivor_corpse_items, so nothing has to be escaped) and then
    // loaded with Item::LoadFromDB - the lossless path the login code uses.
    Item* RestoreItemInstance(uint32 corpseId, uint32 slotIndex, uint32 ownerGuid, uint32& itemGuidOut)
    {
        QueryResult keys = CharacterDatabase.Query(
            "SELECT `item_guid`, `itemEntry` FROM `survivor_corpse_items` WHERE `corpse_id` = {} AND `slot_index` = {}",
            corpseId, slotIndex);

        if (!keys)
            return nullptr;

        Field* keyFields = keys->Fetch();
        uint32 itemGuid = keyFields[0].Get<uint32>();
        uint32 itemEntry = keyFields[1].Get<uint32>();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
        {
            LOG_ERROR("module", "Survivor: saved item entry {} has no item template (corpse {}, slot {}).",
                      itemEntry, corpseId, slotIndex);
            return nullptr;
        }

        CharacterDatabase.DirectExecute(
            "REPLACE INTO `item_instance` (`itemEntry`, `owner_guid`, `creatorGuid`, `giftCreatorGuid`, `count`, "
            "`duration`, `charges`, `flags`, `enchantments`, `randomPropertyId`, `durability`, `playedTime`, `text`, `guid`) "
            "SELECT `itemEntry`, {}, `creatorGuid`, `giftCreatorGuid`, `count`, `duration`, `charges`, `flags`, "
            "`enchantments`, `randomPropertyId`, `durability`, `playedTime`, `text`, `item_guid` "
            "FROM `survivor_corpse_items` WHERE `corpse_id` = {} AND `slot_index` = {}",
            ownerGuid, corpseId, slotIndex);

        QueryResult loaded = CharacterDatabase.Query(
            "SELECT `creatorGuid`, `giftCreatorGuid`, `count`, `duration`, `charges`, `flags`, `enchantments`, "
            "`randomPropertyId`, `durability`, `playedTime`, `text` FROM `item_instance` WHERE `guid` = {}", itemGuid);

        if (!loaded)
        {
            LOG_ERROR("module", "Survivor: item_instance row {} was not restored (corpse {}, slot {}).",
                      itemGuid, corpseId, slotIndex);
            return nullptr;
        }

        Field* fields = loaded->Fetch();

        Item* item = NewItemOrBag(proto);
        if (!item->LoadFromDB(itemGuid, ObjectGuid::Create<HighGuid::Player>(ownerGuid), fields, itemEntry))
        {
            LOG_ERROR("module", "Survivor: Item::LoadFromDB refused item {} (entry {}) of corpse {}.", itemGuid, itemEntry, corpseId);
            delete item;
            return nullptr;
        }

        itemGuidOut = itemGuid;
        return item;
    }

    // Mails a recovered instance through the characters DB, the way
    // Player::SendItemRetrievalMail does: the item is saved first, because the
    // mail only stores a reference to it. The draft owns the Item afterwards.
    void MailRestoredItem(uint32 ownerGuid, Item* item)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->SaveToDB(trans);
        MailDraft("Recovered belongings",
                  "Your remains could not be carried in your bags and were recovered by the postmaster.$B$B"
                  "Please find your belongings enclosed.")
            .AddItem(item)
            .SendMailTo(trans, MailReceiver(ObjectGuid::LowType(ownerGuid)), MailSender(MAIL_CREATURE, 34337 /* The Postmaster */));
        CharacterDatabase.CommitTransaction(trans);
    }

    // Deletes the corpse row once nothing is left in it, and tells the addon
    // to clear its pin.
    void CheckCorpseEmptied(Player* player, uint32 corpseId)
    {
        QueryResult remaining = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `survivor_corpse_items` WHERE `corpse_id` = {}", corpseId);

        if (!remaining)
            return;

        if (remaining->Fetch()[0].Get<uint64>() > 0)
            return;

        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpse_items` WHERE `corpse_id` = {}", corpseId);
        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpses` WHERE `id` = {}", corpseId);

        SendAddon(player, "none");
    }

    // The bot path: same strip, no drop (unless the realm asked for one), and a
    // re-roll queued for the next update - calling PlayerbotFactory from inside
    // Player::Kill would mutate a bot whose AI is still mid-action.
    void HandleBotDeath(Player* bot, Survivor::Config const& cfg)
    {
        std::vector<SlotRef> slots = CollectDroppableItems(bot, cfg);
        if (slots.empty())
            return;                     // a naked bot has nothing to take and nothing to re-roll

        if (cfg.BotDropContainer)
        {
            uint32 corpseId = CreateCorpseRow(bot, cfg, true);
            if (corpseId)
            {
                std::vector<LootSnapshot> snapshot = SnapshotAndDestroy(bot, slots, corpseId);
                if (!snapshot.empty())
                    SpawnContainer(bot->GetMap(), bot->GetGUID(), corpseId, bot->GetPositionX(), bot->GetPositionY(),
                                   bot->GetPositionZ(), bot->GetOrientation(), snapshot, true);
            }
            else
                DestroyDroppedItems(bot, slots);
        }
        else
            DestroyDroppedItems(bot, slots);

        if (cfg.BotRegenOnDeath)
        {
            // Queued last, and only the queue insert is under the lock: the
            // strip above already did its database and item work.
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            PendingBotGear().insert(bot->GetGUID().GetCounter());
        }
    }

    // Takes a guid out of the revive queue. True when it was queued.
    bool TakePendingRevive(uint32 guidLow)
    {
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        return PendingRevive().erase(guidLow) > 0;
    }

    // Queues the revive of a survivor whose death was just handled. Only the
    // insert runs under the lock; the death itself is over by now.
    void QueueRevive(uint32 guidLow)
    {
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        PendingRevive().insert(guidLow);
    }

    // Replaces the ghost the core would leave behind with a live character at
    // the closest graveyard, at full health and without resurrection sickness.
    // Does nothing when there is no death to undo.
    //
    // The order is the one Player::RepopAtGraveyard uses for an unreachable
    // death: resurrect first (it fires OnPlayerCanResurrect and removes the
    // ghost auras), then turn the corpse into bones while it is still on the
    // map that created it, and only then teleport. RepopAtGraveyard goes last
    // for the same reason the core does it last: it fires
    // OnPlayerCanRepopAtGraveyard and only sends the spirit-healer marker to a
    // dead player, which we no longer are.
    bool ReviveAtGraveyard(Player* player)
    {
        if (!player || !player->IsInWorld() || !player->isDead())
            return false;

        // A battleground or arena death keeps the core's own handling (the
        // resurrection at the end of the match).
        if (player->InBattleground() || player->InArena())
            return false;

        // Permadeath supersedes the survivor ruleset: a character promoted to
        // Hardcore must never be revived, whatever this process still has in
        // its flag cache.
        if (Survivor::IsHardcoreFlagged(player->GetGUID().GetCounter()))
            return false;

        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
        player->RepopAtGraveyard();

        player->SendSystemMessage(kReviveMessage);
        return true;
    }

    // Re-summons the unclaimed containers standing on the map this player is on,
    // whoever they belong to. A container is a world object now that the loot
    // gate asks about the looter rather than the owner, and the owner may never
    // come back at all - a bot that has moved on, or a hardcore character that
    // fell - so keying the restore on the owner's guid would lose those
    // containers on every restart.
    void RestoreContainers(Player* player)
    {
        Survivor::Config const& cfg = Survivor::GetConfig();
        if (!player || !cfg.Enable || !cfg.ContainerEntry || !player->IsInWorld())
            return;

        // Rows are only ever created for a flagged character (the flag is
        // one-way), so this hook does no work for anybody else - which matters
        // because it runs on every zone change, for every player including the
        // bots.
        if (!Survivor::IsFlagged(player))
            return;

        Map* map = player->GetMap();
        if (!map)
            return;

        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`, `guid`, `x`, `y`, `z`, `o`, `no_mail` FROM `survivor_corpses` WHERE `map` = {} ORDER BY `id` ASC",
            map->GetId());

        if (!result)
            return;

        std::vector<std::tuple<uint32, uint32, float, float, float, float, bool>> rows;
        do
        {
            Field* fields = result->Fetch();
            rows.emplace_back(fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<float>(),
                              fields[3].Get<float>(), fields[4].Get<float>(), fields[5].Get<float>(),
                              fields[6].Get<uint8>() != 0);
        } while (result->NextRow());

        for (auto const& [corpseId, ownerGuid, x, y, z, o, noMail] : rows)
        {
            bool alreadyLive;
            {
                std::lock_guard<std::recursive_mutex> guard(CacheMutex());
                alreadyLive = CorpseToLive().find(corpseId) != CorpseToLive().end();
            }

            if (alreadyLive)
                continue;               // already standing on this map

            std::vector<LootSnapshot> snapshot = LoadSnapshot(corpseId);
            if (snapshot.empty())
                continue;               // emptied elsewhere; the sweep will drop the row

            SpawnContainer(map, ObjectGuid::Create<HighGuid::Player>(ownerGuid), corpseId, x, y, z, o, snapshot,
                           noMail);
        }
    }

    // Sends the site of the newest unclaimed container, or 'none' when there is
    // nothing left to point at.
    void SendNewestSite(Player* player)
    {
        if (!player || !Survivor::GetConfig().MarkOnMap)
            return;

        // Only a flagged character can own a row, and the flag is one-way, so a
        // character that never opted in can never have a site: answer the addon
        // without paying for the query. The addon re-asks while it has no site,
        // so it still needs an answer.
        if (!Survivor::IsFlagged(player))
        {
            SendAddon(player, "none");
            return;
        }

        QueryResult result = CharacterDatabase.Query(
            "SELECT `map`, `x`, `y`, `zone`, `id` FROM `survivor_corpses` WHERE `guid` = {} ORDER BY `id` DESC LIMIT 1",
            player->GetGUID().GetCounter());

        if (!result)
        {
            SendAddon(player, "none");
            return;
        }

        Field* fields = result->Fetch();
        SendSite(player, fields[0].Get<uint32>(), fields[1].Get<float>(), fields[2].Get<float>(),
                 fields[3].Get<uint32>(), fields[4].Get<uint32>());
    }

    // Expiry: mail what is left to the owner, drop the rows and the object.
    void ExpireCorpse(uint32 corpseId, uint32 ownerGuid, bool noMail)
    {
        QueryResult items = CharacterDatabase.Query(
            "SELECT `slot_index` FROM `survivor_corpse_items` WHERE `corpse_id` = {} ORDER BY `slot_index` ASC", corpseId);

        // A container that nobody can be mailed to rots: a bot is re-geared by
        // Survivor.BotRegenOnDeath, and a hardcore character is a ghost for
        // good, so a mailbox would only hide the gear instead of letting it go.
        // Its saved instances go with it, or the item_instance rows outlive
        // every reference to them.
        if (items && !noMail)
        {
            std::vector<uint32> slotIndexes;
            do
            {
                slotIndexes.push_back(items->Fetch()[0].Get<uint32>());
            } while (items->NextRow());

            for (uint32 slotIndex : slotIndexes)
            {
                uint32 itemGuid = 0;
                Item* item = RestoreItemInstance(corpseId, slotIndex, ownerGuid, itemGuid);
                if (!item)
                    continue;

                MailRestoredItem(ownerGuid, item);
            }
        }
        else if (items)
        {
            CharacterDatabase.DirectExecute(
                "DELETE FROM `item_instance` WHERE `guid` IN "
                "(SELECT `item_guid` FROM `survivor_corpse_items` WHERE `corpse_id` = {})", corpseId);
        }

        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpse_items` WHERE `corpse_id` = {}", corpseId);
        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpses` WHERE `id` = {}", corpseId);

        // Copy the live record out under the lock, then forget it and touch the
        // map outside: FindMap/GetGameObject/Delete are not ours to hold a
        // module lock across.
        bool haveLive = false;
        LiveContainer record;
        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            auto live = CorpseToLive().find(corpseId);
            if (live != CorpseToLive().end())
            {
                record = live->second;
                haveLive = true;
            }
        }

        if (!haveLive)
            return;

        ForgetContainer(record.goLowGuid, corpseId);

        if (Map* map = sMapMgr->FindMap(record.mapId, record.instanceId))
            if (GameObject* go = map->GetGameObject(record.guid))
                go->Delete();
    }
}

namespace Survivor
{
    void EnsureSchema()
    {
        // Deliberately NOT a SQL update file: on this fork a failing module SQL
        // aborts the whole worldserver boot, so the tables are created
        // programmatically and a failure is tolerated at runtime instead - the
        // same approach mod-hardcore uses for hardcore_flags.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `survivor_flags` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `survivor_corpses` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`guid` INT UNSIGNED NOT NULL, "
            "`name` VARCHAR(12) NOT NULL, "
            "`map` INT UNSIGNED NOT NULL, "
            "`x` FLOAT NOT NULL, "
            "`y` FLOAT NOT NULL, "
            "`z` FLOAT NOT NULL, "
            "`o` FLOAT NOT NULL, "
            "`zone` INT UNSIGNED NOT NULL, "
            "`level` TINYINT UNSIGNED NOT NULL, "
            "`created_ts` INT UNSIGNED NOT NULL, "
            "`expires_ts` INT UNSIGNED NOT NULL, "
            "`no_mail` TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`id`), "
            "KEY `idx_survivor_corpses_guid` (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // CREATE TABLE IF NOT EXISTS leaves an install that predates the column
        // alone, so add it when it is missing. Asked, not assumed: a duplicate
        // ALTER would log an error on every boot.
        if (!CharacterDatabase.Query(
                "SELECT 1 FROM `information_schema`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() "
                "AND `TABLE_NAME` = 'survivor_corpses' AND `COLUMN_NAME` = 'no_mail' LIMIT 1"))
        {
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `survivor_corpses` ADD COLUMN `no_mail` TINYINT UNSIGNED NOT NULL DEFAULT 0");
        }

        // Column for column the same names and types as item_instance (minus
        // owner_guid, plus the loot ordering), so recovery is a straight
        // re-insert and the payload stays auditable with plain SQL.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `survivor_corpse_items` ("
            "`corpse_id` INT UNSIGNED NOT NULL, "
            "`slot_index` SMALLINT UNSIGNED NOT NULL, "
            "`item_guid` INT UNSIGNED NOT NULL, "
            "`itemEntry` INT UNSIGNED NOT NULL, "
            "`creatorGuid` INT UNSIGNED NOT NULL, "
            "`giftCreatorGuid` INT UNSIGNED NOT NULL, "
            "`count` INT UNSIGNED NOT NULL, "
            "`duration` INT NOT NULL, "
            "`charges` TEXT NOT NULL, "
            "`flags` INT UNSIGNED NOT NULL, "
            "`enchantments` TEXT NOT NULL, "
            "`randomPropertyId` SMALLINT NOT NULL, "
            "`durability` SMALLINT UNSIGNED NOT NULL, "
            "`playedTime` INT UNSIGNED NOT NULL, "
            "`text` TEXT NOT NULL, "
            "PRIMARY KEY (`corpse_id`, `slot_index`), "
            "KEY `idx_survivor_corpse_items_guid` (`item_guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }

    void LoadFlags()
    {
        // The query fills a local set first, so the lock is only taken for the
        // swap and never spans a database call.
        std::unordered_set<uint32> loaded;

        if (QueryResult result = CharacterDatabase.Query("SELECT `guid` FROM `survivor_flags` WHERE `enabled` = 1"))
        {
            do
            {
                loaded.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        FlaggedGuids().swap(loaded);
    }

    bool IsFlaggedGuid(uint32_t guidLow)
    {
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        std::unordered_set<uint32> const& guids = FlaggedGuids();
        return guids.find(guidLow) != guids.end();
    }

    // True when mod-hardcore has this character flagged. Reads hardcore_flags
    // directly - no link against the other module - and a missing table simply
    // means "not flagged", so this module builds and runs without mod-hardcore.
    //
    // Deliberately not cached: every caller is cold (a death, a revival, a
    // command), never a per-update hook, so one query there costs nothing and a
    // cache would have to be invalidated when '.hardcore on' runs in the other
    // module.
    bool IsHardcoreFlagged(uint32 guidLow)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT `enabled` FROM `hardcore_flags` WHERE `guid` = {} AND `enabled` = 1", guidLow);

        return bool(result);
    }

    bool IsFlagged(Player* player)
    {
        if (!player)
            return false;

        // Recursive: IsFlaggedGuid takes the same mutex.
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        return IsFlaggedGuid(player->GetGUID().GetCounter());
    }

    bool IsBotControlled(Player* player)
    {
        if (!player)
            return false;

        WorldSession* session = player->GetSession();
        return session && session->IsHeadless();
    }

    void Flag(Player* player)
    {
        if (!player || IsBotControlled(player))
            return;

        uint32 guidLow = player->GetGUID().GetCounter();

        // The write is queued to the characters DB first (the database layer is
        // thread-safe on its own); only the cache insert needs the mutex.
        CharacterDatabase.Execute(
            "INSERT INTO `survivor_flags` (`guid`, `enabled`) VALUES ({}, 1) "
            "ON DUPLICATE KEY UPDATE `enabled` = 1", guidLow);

        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        FlaggedGuids().insert(guidLow);
    }

    // The drop half of the ruleset: snapshot the items, destroy them, put a
    // container at the death spot, announce it and pin it on the map. Returns
    // early when there is nothing to take - a character who already lost
    // everything still dies, it just leaves no second container.
    void DropGearAtDeath(Player* player, Survivor::Config const& cfg, bool noMail)
    {
        std::vector<SlotRef> slots = CollectDroppableItems(player, cfg);
        if (slots.empty())
            return;

        uint32 corpseId = CreateCorpseRow(player, cfg, noMail);
        if (!corpseId)
            return;

        std::vector<LootSnapshot> snapshot = SnapshotAndDestroy(player, slots, corpseId);

        if (snapshot.empty())
        {
            // Every slot vanished between the scan and the snapshot; the row
            // would only be an empty marker.
            CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpses` WHERE `id` = {}", corpseId);
            return;
        }

        if (!SpawnContainer(player->GetMap(), player->GetGUID(), corpseId, player->GetPositionX(), player->GetPositionY(),
                            player->GetPositionZ(), player->GetOrientation(), snapshot, noMail))
        {
            // The template is missing (SQL not applied) or the summon failed.
            // Nothing to walk back to, so neither the announcement nor the map
            // pin may claim otherwise; the rows stay, so the expiry sweep still
            // mails the instances back.
            player->SendSystemMessage("|cffff8000[Survivor]|r Your belongings could not be placed where you fell; they will be mailed back to you.");
            return;
        }

        if (cfg.AnnounceDeaths)
        {
            sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING,
                Acore::StringFormat("|cffff8000[Survivor]|r {} died and left everything at the place of death.",
                    player->GetName()));
        }

        SendSite(player, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                 player->GetZoneId(), corpseId);
    }

    void HandleDeath(Player* player)
    {
        Config const& cfg = GetConfig();

        if (!cfg.Enable || !player)
            return;

        // A container in a battleground would be unreachable after the match,
        // and the core restores the character at the end of it.
        if (player->InBattleground() || player->InArena())
            return;

        if (IsBotControlled(player))
        {
            if (cfg.ApplyToBots)
                HandleBotDeath(player, cfg);

            return;
        }

        // A Hardcore character that was promoted out of Survivor still carries
        // the flag in this process's cache until the next boot, so the hardcore
        // table is the authority here. A hardcore death leaves its gear in a
        // container for whoever plays the ruleset, and never comes back: the
        // character is a ghost for good.
        if (IsHardcoreFlagged(player->GetGUID().GetCounter()))
        {
            DropGearAtDeath(player, cfg, true);
            return;
        }

        if (!IsFlagged(player))
            return;

        DropGearAtDeath(player, cfg, false);

        // Queued for every survivor death, not only the ones that dropped
        // something: a character who already lost everything still comes back
        // at the graveyard instead of ghosting. Deferred, because
        // OnPlayerJustDied runs inside Player::Kill, before the corpse exists
        // and before the death sequence finishes; the next update of this
        // player brings it back.
        QueueRevive(player->GetGUID().GetCounter());
    }

    void HandleBotUpdate(Player* player)
    {
        Config const& cfg = GetConfig();
        if (!cfg.Enable || !cfg.ApplyToBots || !cfg.BotRegenOnDeath || !player)
            return;

        // The queue is only consulted and drained under the lock; the factory
        // work below runs outside it, so nothing else waits on a gear re-roll.
        bool runRegen = false;
        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            std::unordered_set<uint32>& pending = PendingBotGear();
            auto it = pending.find(player->GetGUID().GetCounter());
            if (it == pending.end())
                return;

            if (!IsBotControlled(player))
            {
                pending.erase(it);      // the session is not a bot any more
                return;
            }

            // Wait for the bot to be alive again, out of a battleground, and
            // fully in the world: the factory stores items, which needs a live
            // player.
            if (!player->IsAlive() || !player->IsInWorld() || player->InBattleground() || player->InArena())
                return;

            pending.erase(it);
            runRegen = true;
        }

        if (!runRegen)
            return;

        // Deliberately not the factory's full re-roll: that also resets
        // talents, skills, spells, quests and level. InitBags + AutoGear give
        // the same bot a fresh kit for its class, level and current specs.
        PlayerbotFactory factory(player, player->GetLevel());
        factory.InitBags(true);
        PlayerbotFactory::AutoGear(player, 0, 0, false);
    }

    void HandleReviveUpdate(Player* player)
    {
        if (!GetConfig().Enable || !player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();

        // Peek under the lock, act outside it: the guid is only consumed by
        // the update that can actually revive.
        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            if (PendingRevive().find(guidLow) == PendingRevive().end())
                return;
        }

        if (!player->isDead())
        {
            // Someone else resurrected the character between the death and this
            // tick; there is no ghost to replace.
            TakePendingRevive(guidLow);
            return;
        }

        // Mid-teleport or mid-logout: keep the entry and use the next tick that
        // finds the character fully in the world.
        if (!player->IsInWorld())
            return;

        // Battlegrounds and arenas keep the core's resurrection; the entry
        // waits for a tick that finds the character out of them.
        if (player->InBattleground() || player->InArena())
            return;

        TakePendingRevive(guidLow);
        ReviveAtGraveyard(player);
    }

    void HandleLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid)
    {
        Config const& cfg = GetConfig();
        if (!cfg.Enable || !player || !item || !lootGuid.IsGameObject())
            return;

        // Resolve the container to its corpse row under the lock, then do the
        // database and item work without it.
        uint32 corpseId = 0;
        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            auto link = GoToCorpse().find(lootGuid.GetCounter());
            if (link == GoToCorpse().end())
                return;                 // not one of our containers

            corpseId = link->second;
        }

        // The first unconsumed saved instance that matches what the core just
        // created (the loot window only carries entry, count and property).
        QueryResult saved = CharacterDatabase.Query(
            "SELECT `slot_index` FROM `survivor_corpse_items` "
            "WHERE `corpse_id` = {} AND `itemEntry` = {} AND `count` = {} AND `randomPropertyId` = {} "
            "ORDER BY `slot_index` ASC LIMIT 1",
            corpseId, item->GetEntry(), count, item->GetItemRandomPropertyId());

        if (!saved)
        {
            LOG_ERROR("module", "Survivor: looted entry {} (x{}) has no saved instance in corpse {}.",
                      item->GetEntry(), count, corpseId);
            return;
        }

        uint32 slotIndex = saved->Fetch()[0].Get<uint32>();

        uint32 itemGuid = 0;
        Item* restored = RestoreItemInstance(corpseId, slotIndex, player->GetGUID().GetCounter(), itemGuid);
        if (!restored)
            return;                     // nothing was touched on the character

        // The core already stored a template-level copy in the slot the player
        // looted into; take it back out before putting the real instance in.
        uint8 freedBag = item->GetBagSlot();
        uint8 freedSlot = item->GetSlot();
        uint32 createdCount = item->GetCount();
        uint32 restoredCount = restored->GetCount();

        bool merged = createdCount > count;
        if (merged)
        {
            // The loot merged into an existing stack: take only the looted
            // units back, never the player's own.
            player->DestroyItemCount(item, count, true);
            freedBag = NULL_BAG;
            freedSlot = NULL_SLOT;
        }
        else
            player->DestroyItem(freedBag, freedSlot, true);

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreItem(freedBag, freedSlot, dest, restored->GetEntry(), restoredCount, restored);
        if (msg != EQUIP_ERR_OK)
        {
            dest.clear();
            msg = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, restored->GetEntry(), restoredCount, restored);
        }

        if (msg == EQUIP_ERR_OK)
            player->StoreItem(dest, restored, true);
        else
            MailRestoredItem(player->GetGUID().GetCounter(), restored);

        CharacterDatabase.DirectExecute(
            "DELETE FROM `survivor_corpse_items` WHERE `corpse_id` = {} AND `slot_index` = {}", corpseId, slotIndex);

        CheckCorpseEmptied(player, corpseId);
    }

    void HandleContainerLootState(GameObject* go, uint32_t state)
    {
        if (!go || state != uint32(GO_JUST_DEACTIVATED))
            return;

        // Drop the in-memory link first (under the lock) and copy the corpse id
        // out, so the database work below runs without holding the mutex.
        uint32 corpseId = 0;
        {
            std::lock_guard<std::recursive_mutex> guard(CacheMutex());
            auto link = GoToCorpse().find(go->GetGUID().GetCounter());
            if (link == GoToCorpse().end())
                return;

            corpseId = link->second;
            ForgetContainer(go->GetGUID().GetCounter(), corpseId);
        }

        // The object is deleted by the core right after this (despawn at action
        // plus a summon respawn time). Anything still unsaved here belongs to
        // an unfinished container: keep the row, the sweep mails it at expiry.
        QueryResult remaining = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `survivor_corpse_items` WHERE `corpse_id` = {}", corpseId);

        if (remaining && remaining->Fetch()[0].Get<uint64>() > 0)
            return;

        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpse_items` WHERE `corpse_id` = {}", corpseId);
        CharacterDatabase.DirectExecute("DELETE FROM `survivor_corpses` WHERE `id` = {}", corpseId);
    }

    void HandleContainerRemoved(GameObject* go)
    {
        if (!go)
            return;

        std::lock_guard<std::recursive_mutex> guard(CacheMutex());
        auto link = GoToCorpse().find(go->GetGUID().GetCounter());
        if (link == GoToCorpse().end())
            return;

        // The object went away with its map (or was deleted by the core). Drop
        // the in-memory link only; the persisted row is what re-summons it.
        ForgetContainer(go->GetGUID().GetCounter(), link->second);
    }

    void HandleLogin(Player* player)
    {
        if (!player || !GetConfig().Enable)
            return;

        // Before the revive below moves the character to the graveyard, so the
        // container is re-summoned on the map the character logs in on.
        RestoreContainers(player);

        if (IsBotControlled(player))
            return;

        // A survivor who died and logged out before the update tick must not
        // come back as a ghost. Every death of a flagged character outside a
        // battleground or arena is handled by this module, so the flag alone
        // decides it - including a death whose container has already been
        // swept, where there is nothing left to recover but the ghost is still
        // wrong. The battleground and arena exclusions live in
        // ReviveAtGraveyard.
        if (player->isDead() && IsFlagged(player))
        {
            uint32 guidLow = player->GetGUID().GetCounter();
            TakePendingRevive(guidLow);

            // A login that bounces the character to another map leaves it out of
            // the world here; queue it so the first update that finds it in the
            // world finishes the job.
            if (!ReviveAtGraveyard(player))
                QueueRevive(guidLow);
        }

        SendNewestSite(player);
    }

    void HandleUpdateZone(Player* player)
    {
        if (!player || !GetConfig().Enable)
            return;

        RestoreContainers(player);
    }

    void HandleReleasedGhost(Player* player)
    {
        if (!player || !GetConfig().Enable || IsBotControlled(player))
            return;

        // A release that beats the update tick: revive here instead of letting
        // the ghost run. Only a death this module queued is affected - an
        // ordinary ghost, or a Hardcore one, is left to the core.
        uint32 guidLow = player->GetGUID().GetCounter();
        if (TakePendingRevive(guidLow) && !ReviveAtGraveyard(player))
            QueueRevive(guidLow);

        SendNewestSite(player);
    }

    bool HandleAddonChat(Player* player, uint32_t type, uint32_t language, std::string& msg, Player* receiver)
    {
        if (!player || language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
            return true;

        if (msg.rfind(kAddonPrefix, 0) != 0)
            return true;

        if (msg == std::string(kAddonPrefix) + "hello")
        {
            SendNewestSite(player);
            return false;
        }

        return true;
    }

    bool DenyForeignContainerLoot(Player const* player, ObjectGuid source)
    {
        if (!player || !source.IsGameObject())
            return false;

        // Pure memory reads: the lock is held for the whole lookup, and this
        // function never touches the database or the map.
        std::lock_guard<std::recursive_mutex> guard(CacheMutex());

        if (GoToCorpse().find(source.GetCounter()) == GoToCorpse().end())
            return false;               // not ours: leave the core's verdict alone

        // A death container belongs to whoever plays the ruleset, not to the
        // character that died: any Survivor-flagged character may open any of
        // them, whoever the death was. A character that never opted in may open
        // none - which is also what stops a normal alt from farming the
        // containers a Survivor left behind.
        //
        // Inverted hook: the core denies the loot when this returns false, and
        // the wrapper does not negate, so "true" here means "deny".
        return !IsFlaggedGuid(player->GetGUID().GetCounter());
    }

    void SweepExpired()
    {
        if (!GetConfig().Enable)
            return;

        QueryResult expired = CharacterDatabase.Query(
            "SELECT `id`, `guid`, `no_mail` FROM `survivor_corpses` WHERE `expires_ts` <= {} ORDER BY `id` ASC", NowSeconds());

        if (!expired)
            return;

        std::vector<std::tuple<uint32, uint32, bool>> rows;
        do
        {
            Field* fields = expired->Fetch();
            rows.emplace_back(fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint8>() != 0);
        } while (expired->NextRow());

        for (auto const& [corpseId, ownerGuid, noMail] : rows)
            ExpireCorpse(corpseId, ownerGuid, noMail);
    }
}

// =====================================================================
//  CommandScript: `.survivor on|status`
// =====================================================================
class survivor_commandscript : public CommandScript
{
public:
    survivor_commandscript() : CommandScript("survivor_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable survivorTable =
        {
            { "on",     HandleSurvivorOnCommand,     SEC_PLAYER, Console::No },
            { "status", HandleSurvivorStatusCommand, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "survivor", survivorTable },
        };

        return commandTable;
    }

    // Opt-in is deliberately one-way: there is no '.survivor off', because a
    // ruleset you can switch off after dying is not a ruleset.
    static bool HandleSurvivorOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (Survivor::IsBotControlled(player))
        {
            handler->SendSysMessage("Survivor mode is not available for bot-controlled characters.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Survivor::Config const& cfg = Survivor::GetConfig();
        if (!cfg.Enable)
        {
            handler->SendSysMessage("Survivor mode is currently disabled on this server.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Hardcore first: it supersedes Survivor, and a character promoted out
        // of Survivor still carries this process's cached flag.
        if (Survivor::IsHardcoreFlagged(player->GetGUID().GetCounter()))
        {
            handler->SendSysMessage(
                "This character is flagged Hardcore. Hardcore is terminal: it cannot be combined with Survivor.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (Survivor::IsFlagged(player))
        {
            handler->SendSysMessage("This character is already flagged Survivor.");
            return true;
        }

        if (player->GetLevel() < cfg.MinLevelToFlag)
        {
            handler->PSendSysMessage("You must be at least level {} to opt into Survivor mode.", cfg.MinLevelToFlag);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Survivor::Flag(player);
        handler->PSendSysMessage(
            "Survivor mode ENABLED for {}. Your gear stays behind where you die until you recover it.",
            player->GetName());
        return true;
    }

    static bool HandleSurvivorStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        // Hardcore first: it supersedes Survivor, and the in-process flag cache
        // can still carry a Survivor opt-in that '.hardcore on' already dropped
        // from the database.
        if (Survivor::IsHardcoreFlagged(player->GetGUID().GetCounter()))
        {
            handler->SendSysMessage(
                "Survivor: superseded - this character is flagged Hardcore, and Hardcore is terminal.");
            return true;
        }

        if (!Survivor::GetConfig().Enable)
            handler->SendSysMessage("Survivor: disabled on this server.");
        else if (Survivor::IsFlagged(player))
            handler->SendSysMessage(
                "Survivor: ENABLED (a death drops your equipment and bags where you fall, and any Survivor may loot any container).");
        else
            handler->SendSysMessage("Survivor: disabled.");
        return true;
    }
};

// =====================================================================
//  PlayerScript: death, loot recovery, bot re-roll, container re-summon,
//  and the addon channel.
// =====================================================================
class SurvivorPlayerScript : public PlayerScript
{
public:
    SurvivorPlayerScript() : PlayerScript("Survivor_PlayerScript") { }

    void OnPlayerJustDied(Player* player) override
    {
        Survivor::HandleDeath(player);
    }

    void OnPlayerReleasedGhost(Player* player) override
    {
        Survivor::HandleReleasedGhost(player);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
    {
        Survivor::HandleLootItem(player, item, count, lootguid);
    }

    // Every update. The bot queue and the survivor revive queue are both empty
    // for any character with nothing pending.
    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        Survivor::HandleBotUpdate(player);
        Survivor::HandleReviveUpdate(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        Survivor::HandleLogin(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        Survivor::HandleUpdateZone(player);
    }

    [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
                                          Player* receiver) override
    {
        return Survivor::HandleAddonChat(player, type, language, msg, receiver);
    }
};

// =====================================================================
//  GlobalScript: who may open a container.
// =====================================================================
class SurvivorGlobalScript : public GlobalScript
{
public:
    SurvivorGlobalScript() : GlobalScript("Survivor_GlobalScript") { }

    // Last gate before SMSG_LOOT_RESPONSE. Returning true here means DENY.
    bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source) override
    {
        return Survivor::DenyForeignContainerLoot(player, source);
    }
};

// =====================================================================
//  GameObjectScript: container lifecycle (deactivation, map unload).
//
//  AllGameObjectScript, not GameObjectScript: the latter is only dispatched to
//  the script named by the gameobject template's ScriptName, and our template
//  deliberately has none.
// =====================================================================
class SurvivorGameObjectScript : public AllGameObjectScript
{
public:
    SurvivorGameObjectScript() : AllGameObjectScript("Survivor_GameObjectScript") { }

    void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* /*unit*/) override
    {
        Survivor::HandleContainerLootState(go, state);
    }

    void OnGameObjectRemoveWorld(GameObject* go) override
    {
        Survivor::HandleContainerRemoved(go);
    }
};

// =====================================================================
//  WorldScript: config load, schema/cache bootstrap, expiry sweep.
// =====================================================================
class SurvivorWorldScript : public WorldScript
{
public:
    SurvivorWorldScript() : WorldScript("Survivor_WorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Survivor::Config& cfg = Survivor::GetConfig();

        cfg.Enable = sConfigMgr->GetOption<bool>("Survivor.Enable", true);
        cfg.MinLevelToFlag = sConfigMgr->GetOption<uint32>("Survivor.MinLevelToFlag", 10);
        cfg.AnnounceDeaths = sConfigMgr->GetOption<bool>("Survivor.AnnounceDeaths", true);
        cfg.DropEquipped = sConfigMgr->GetOption<bool>("Survivor.DropEquipped", true);
        cfg.DropBags = sConfigMgr->GetOption<bool>("Survivor.DropBags", true);
        cfg.ContainerLifetimeDays = sConfigMgr->GetOption<uint32>("Survivor.ContainerLifetimeDays", 7);
        cfg.MarkOnMap = sConfigMgr->GetOption<bool>("Survivor.MarkOnMap", true);
        cfg.ContainerEntry = sConfigMgr->GetOption<uint32>("Survivor.ContainerEntry", 5000000);
        cfg.ApplyToBots = sConfigMgr->GetOption<bool>("Survivor.ApplyToBots", true);
        cfg.BotRegenOnDeath = sConfigMgr->GetOption<bool>("Survivor.BotRegenOnDeath", true);
        cfg.BotDropContainer = sConfigMgr->GetOption<bool>("Survivor.BotDropContainer", false);
    }

    void OnStartup() override
    {
        Survivor::EnsureSchema();
        Survivor::LoadFlags();
        Survivor::SweepExpired();
    }

    void OnUpdate(uint32 diff) override
    {
        _sweepTimer += diff;

        if (_sweepTimer < SweepIntervalMs)
            return;

        _sweepTimer = 0;
        Survivor::SweepExpired();
    }

private:
    static constexpr uint32 SweepIntervalMs = 30000;
    uint32 _sweepTimer = 0;
};

// =====================================================================
//  Registration
// =====================================================================
void AddSurvivorScripts()
{
    new survivor_commandscript();
    new SurvivorPlayerScript();
    new SurvivorGlobalScript();
    new SurvivorGameObjectScript();
    new SurvivorWorldScript();
}
