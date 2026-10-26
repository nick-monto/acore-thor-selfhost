# Survivor mode

A second opt-in ruleset, mutually exclusive with Hardcore: dying costs you everything you
carry, but it does not end the character. On death the equipped items and the contents of
the bags are lifted off the body and placed in a lootable container at the death position,
and that position is marked on the player's map until the container is emptied.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported,
**[I]** inference.

## Why a second ruleset

Hardcore (`modules/mod-hardcore`, `.hardcore on`) is permadeath: one death and the
character is finished. That is a different game from "death is expensive". Survivor keeps
the character and makes the death itself the punishment, which is what a solo realm needs:
losing the gear you spent the evening gathering hurts, and the run back to the container is
the content.

The two rulesets cannot coexist. A Hardcore character's death is final, so there is nothing
to loot and no run back; a Survivor character's death must not be final, or the container
would be a memorial rather than a recovery. Each command therefore refuses the other flag,
and both refuse bot-controlled sessions.

## Ruleset

`.survivor on` flags the character. From then on, each death:

1. Snapshots every equipped item and every item inside the bags (and the bags themselves)
   into the characters DB, with the exact instance state - enchantments, gems, durability,
   charges, random property, creator, text.
2. Removes those items from the character. The bank, the mail, the gold and the
   quest log are untouched.
3. Spawns one lootable container at the death position, named after the character, holding
   exactly those items.
4. Marks the position on the player's map (addon message; see §7). There is no ghost phase
   and no corpse marker to fall back on, so the addon pin is the only pointer at the
   container.
5. Wakes the character **alive at the closest graveyard**, at full health, with no
   resurrection sickness, and turns the corpse into bones (§5.9). There is no release and no
   run back to a body: the walk back is to the container, which stays where the character
   died.
6. Looting the container returns each item as the *same* instance it was, into a free bag
   slot; anything that does not fit is mailed, never dropped on the floor.

Opt-in is one-way, like Hardcore: there is no `.survivor off`. `.survivor status` reports
the flag.

## Mechanisms, with the evidence

### 5.1 When the snapshot runs

`PlayerScript::OnPlayerJustDied(Player*)` - declared `Scripting/ScriptDefines/PlayerScript.h:248`,
fired from `Player::Kill` at `Entities/Player/Player.cpp:4671`, **before** the corpse is
created (`Player.cpp:4671-4672`) [V]. It runs on the map thread (it is reached through the
map update), so no session-thread assumptions are allowed in the handler [V].

That is the only hook that sees the items still equipped. `OnPlayerReleasedGhost`
(`Player.cpp:4561`, at the end of `BuildPlayerRepop`) fires after the corpse exists and is
reached from both threads [V]; a survivor never gets there, because it is revived inside the
death hook (§5.9) and never becomes a ghost.

### 5.2 The item snapshot is the item_instance row

`item_instance` (pinned core, `data/sql/base/db_characters/item_instance.sql:23-39`) carries
everything that makes an item an instance:

```
guid, itemEntry, owner_guid, creatorGuid, giftCreatorGuid, count, duration, charges,
flags, enchantments, randomPropertyId, durability, playedTime, text
```

`enchantments` is a space-separated `id duration charges` stream for all twelve slots, gems
included (`SOCK_ENCHANTMENT_SLOT` 2-4, `Item.h:171-173`), written at `Item.cpp:365-374` and
parsed at `Item.cpp:480-483` [V]. `Item::LoadFromDB(guid, owner, Field* fields, entry)`
(`Item.h:242`, `Item.cpp:420-433`) reconstructs the instance from exactly the eleven columns
starting at `creatorGuid` [V], which is what the login path itself does
(`PlayerStorage.cpp:6107-6109`) [V].

So the module stores those eleven columns verbatim in `survivor_corpse_items`, destroys the
live item (which deletes its `item_instance` row through `Item::SaveToDB`'s `ITEM_REMOVED`
branch, `Item.cpp:343-408` [V]), and on recovery inserts the row back under the same guid
and loads it with `LoadFromDB`.

`StoreNewItem` is **not** usable for recovery: it calls `Item::CreateItem` with the entry and
the random property only (`PlayerStorage.cpp:2642-2646`, `Item.cpp:1094-1123`), so enchants,
gems and durability are silently regenerated [V]. Recovery must go through
`LoadFromDB` + `StoreItem` (`PlayerStorage.cpp:2690-2714`) [V].

What is lifted, in order (bags first, so the items inside them are still addressable):
`INVENTORY_SLOT_BAG_START..INVENTORY_SLOT_BAG_END` inner items, then
`EQUIPMENT_SLOT_START..EQUIPMENT_SLOT_END` (which includes the equipped bags), then the
backpack `INVENTORY_SLOT_ITEM_START..INVENTORY_SLOT_ITEM_END`. Removal is
`Player::DestroyItem(bag, slot, true)` (`Player.h:1368`) [V].

### 5.3 The container is a summoned chest with a pre-filled loot

`Map::SummonGameObject(entry, pos, rotation, respawnTime)` (`Maps/Map.h:346-347`, impl
`Entities/Object/Object.cpp:2401-2425`) creates a gameobject the map owns, with
`SetSpawnedByDefault(false)` and, for a non-zero respawn time, `SetSpellId(1)` [V].

Two facts make this the right container:

- `Player::SendLoot` clears and regenerates loot only when the template has a non-zero
  `chest.lootId` (`Player.cpp:8040`, `8053-8055`, `8064`) [V]. A template with
  `lootId = 0` therefore preserves a loot the module filled beforehand - which is the only
  way to show real player items in a loot window, because `LootItem` has no enchantment,
  gem or durability field and no `Item*` constructor (`Loot/LootMgr.h:153-182`) [V].
- A chest with `IsDespawnAtAction()` true (that is `chest.consumable = 1`) and a spell id is
  deleted when its loot is released: `GO_JUST_DEACTIVATED` -> `GetOwnerGUID() || GetSpellId()`
  -> `SetRespawnTime(0); Delete();` (`GameObject.cpp:853-876`) [V]. No manual cleanup timer.

The module fills `go->loot` directly: one `LootItem` per saved instance with `itemid`,
`count`, `randomSuffix`, `randomPropertyId`, `itemIndex` set to its vector index, and
`unlootedCount` incremented by hand - the core only does that inside `Loot::AddItem`
(`LootMgr.cpp:535`) and `isLooted()` is `gold == 0 && unlootedCount == 0`
(`LootMgr.h:368`), so a hand-filled loot that forgot the counter would deactivate on the
first release [V]. `LootItem::itemIndex` must match the vector index because
`NotifyItemRemoved` maps slot to index with it (`LootMgr.cpp:743-756`) [V].

The template (module SQL, `data/sql/world/base/survivor_10_gameobject.sql`):

| column | value | why |
|---|---|---|
| entry | `5000000` | free in the pinned base dump (checked against all 21581 entries) |
| type | `3` (CHEST) | the only type whose loot window `SendLoot` fills and whose deactivation rules we can use |
| displayId | `4093` | "Mangled Human Remains" - the model the stock game uses for a corpse pile |
| name | `Remains` | the client shows this above the object |
| Data0 (lootId) | `0` | keeps the module-filled loot (§5.3) |
| Data2 (consumable) | `1` | despawn at action -> deleted after looting |
| Data8 (groupLootRules) | `0` | solo realm: no group rolls |
| Data9/Data10 (min/max gold) | `0` | `SendLoot` runs `generateMoneyLoot` for every gameobject regardless of lootId (`Player.cpp:8089`), so non-zero gold here would inject money into every container |

`gameobject_template_addon` gets `faction = 0` and `flags = 0` - the container must be
visible and clickable by the owner only, and ownership is enforced in code (§5.6), not by a
gameobject flag: no `GAMEOBJECT_FLAGS` value expresses a player identity
(`SharedDefines.h:1605-1616`) [V].

### 5.4 Recovery is lossless

`PlayerScript::OnPlayerLootItem(Player*, Item*, uint32 count, ObjectGuid lootguid)`
(`PlayerScript.h:458`, fired from `Player::StoreLootItem` at `Player.cpp:13954`) is the
hook. The core has already created and stored a template-level item by then (with the entry
and random property, no enchants), so the module:

1. resolves `lootguid` to a `survivor_corpses` row and takes the first unconsumed saved
   instance whose `(itemEntry, count, randomPropertyId)` matches the created item;
2. destroys the created item (`DestroyItem(item->GetBagSlot(), item->GetSlot(), true)`),
   freeing the slot the player just looted into;
3. inserts the saved `item_instance` row back, loads it with `Item::LoadFromDB`, and stores
   it into the freed slot with `CanStoreItem` + `StoreItem`;
4. if nothing fits, mails it with the core's own recovery pattern: `item->SaveToDB(trans)`
   first, then `MailDraft(...).AddItem(item).SendMailTo(trans, MailReceiver(...),
   MailSender(...))` (`PlayerMisc.cpp:512-519`; `Mail.cpp:261-270` writes only the
   reference) [V].

The item guid is reused, which is safe because the destroy deleted the row.

### 5.5 The container outlives the corpse, and the server

The corpse object is not a container: `Player::SendLoot`'s corpse branch only removes a
battleground insignia (`Player.cpp:8187`) [V], and the survivor death path calls
`SpawnCorpseBones` as it revives, so there is not even a corpse left to loot
(`Player.cpp:4782-4798`) [V]. The chest is independent, so the revival at the graveyard, a
logout, or a server restart all leave the loot where it was.

Persistence: `survivor_corpses` holds `map, x, y, z, o, zone, created_ts, lifetime` and the
owner guid. A summoned gameobject is never saved (`SummonGameObject` sets neither
`m_spawnId` nor a DB row, `Object.cpp:2401-2425`; `SaveRespawnTime` is guarded on
`m_goData && m_spawnedByDefault`, `GameObject.cpp:1236-1241`) [V], so the module re-summons
from the row when the owner enters the map and on login, and sweeps expired rows at startup
and on `OnUpdate`.

`Survivor.ContainerLifetimeDays` (default 7) becomes the `respawnTime` handed to
`SummonGameObject`; expiry destroys the object and the module mails the remaining items to
the owner rather than losing them.

### 5.6 Owner-only access

`GlobalScript::OnAllowedToLootContainerCheck(Player const*, ObjectGuid source)`
(`ScriptDefines/GlobalScript.h:101`) is the last gate before `SMSG_LOOT_RESPONSE`
(`Player.cpp:8373-8377`) [V]. **Its boolean is inverted**: the wrapper uses
`CALL_ENABLED_BOOLEAN_HOOKS` without negation (`GlobalScript.cpp:112-115`,
`ScriptMgrMacros.h:69-73`) and the core denies on `false`, so returning `true` from the hook
means deny [V]. The module returns deny for every player except the owner of the corpse row
and allow (false) otherwise, including for gameobjects it does not own.

Bots: a randombot must never flag itself and must never loot a container. Both are guid/session
checks, matching the hardcore module's `IsBotControlled` (`modules/mod-hardcore/src/Hardcore.cpp`
`WorldSession::IsHeadless`).

### 5.7 The map marker

There is no server-side "draw a pin" opcode: `SMSG_CORPSE_LOCATION` and
`SendCorpseQueryToPlayer` do not exist in this core, and neither does
`Player::SendAddonMessage` [V]. What exists is the addon-message channel the interact module
already uses:

```cpp
WorldPacket data;
ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, "SURV\t<what>");
player->SendDirectMessage(&data);
```

(`Chat/Chat.h:43-45`, `Chat.cpp:273-275`; `Player::SendDirectMessage` `Player.h:2281`;
`CHAT_MSG_ADDON` `SharedDefines.h:3384`, `LANG_ADDON` `SharedDefines.h:741`) [V]. The
interact module ships the same pattern at `modules/mod-e-interact/src/e_interact.cpp:119-120`
and its addon reads it back from `CHAT_MSG_ADDON` with `arg1` as the prefix [V].

Messages:

| message | payload | when |
|---|---|---|
| `SURV\tsite` | `<mapId> <x> <y> <zoneId> <corpseId>` | on death, on login with an unclaimed container, and as a reply to `SURV\thello` |
| `SURV\tnone` | - | after the container is emptied |

The addon (`acore-thor-launcher/app/src/main/assets/survivor/`) registers the prefix,
stores the site in SavedVariables, draws a pin on the world map when the matching map is
shown and a distance arrow on the minimap, and clears it on `none` or `/surv clear`.

The client's own corpse marker is not an option any more: a survivor is revived and its
corpse turned to bones in the same death hook, so `MSG_CORPSE_QUERY` has nothing to point
at. The addon message is the only source of the position.

### 5.8 Bots: strip and re-roll

A solo realm is mostly bots, and a bot that never loses its gear makes the ruleset look
arbitrary. With `Survivor.ApplyToBots = 1` (default) a bot-controlled session follows the
same rules with one difference: its gear is **destroyed, not dropped**, and replaced
immediately with a fresh random set for its class, level and current specialization.

Why destroyed rather than dropped: a container full of randombot gear would be a free-gear
faucet for the one real player (`Survivor.BotDropContainer = 0` keeps that shut; it exists
only if a realm ever wants bot corpses as loot).

The regeneration uses playerbots' own factory, not a hand-rolled item roll:

- `PlayerbotFactory::DestroyEquippedGear(Player*)` - `mod-playerbots/src/Bot/Factory/PlayerbotFactory.cpp:3978-3987`
  - is exactly the strip primitive (it skips tabard and body, which the client cannot
  re-equip) [V].
- `PlayerbotFactory::AutoGear(Player* bot, uint32 itemQuality, uint32 ilvl, bool incremental,
  bool secondChance = false, bool applyFinishers = true)` - `PlayerbotFactory.cpp:3989-4003`
  - builds a `PlayerbotFactory(bot, bot->GetLevel())`, calls `InitEquipment(false)`, then
  `InitAmmo()`, then `ApplyEnchantAndGemsNew()` when the bot is at or above
  `sPlayerbotAIConfig.MinEnchantingBotLevel`, then `DurabilityRepairAll` [V].
- `InitEquipment` rolls from the class/level item pools and filters with `CanEquipItem` /
  `CheckItemStats`, which read the bot's talents - so the set matches the spec the bot
  already has, and falls back to the class default when it has no talents yet
  (`PlayerbotFactory.cpp:2104-2172`, `2342`) [V].
- `InitBags(true)` re-issues bags, which the survivor ruleset took.

`PlayerbotFactory::Randomize(false)` is deliberately **not** used: it also calls
`resetTalents`, `ClearSkills`, `ClearSpells`, `ResetQuests`, `ClearAllItems` and
`GiveLevel` (`PlayerbotFactory.cpp:826-880`) [V], which would re-roll the bot's identity
rather than replace its gear - the opposite of "same bot, new kit".

Precedent for calling the factory from a script hook: the player-centric randombot patch
already does `PlayerbotFactory factory(bot, level); factory.Randomize(false);` from
`RandomPlayerbotMgr::AdjustBotLevel`, i.e. the same map-thread context
(`patches/playerbots-player-centric.patch`) [V].

When it runs: the strip happens in `OnPlayerJustDied`, the re-roll does **not**. A
`PlayerbotFactory` call inside `Player::Kill` would mutate a bot whose AI is mid-action, so
the guid is queued and the re-roll runs on the next `PlayerScript::OnPlayerUpdate`
(`Scripting/ScriptDefines/PlayerScript.h:302`, fired from `Entities/Player/PlayerUpdates.cpp:314`
inside `Player::Update`, i.e. the map thread) once the bot is alive again. Until then the bot
is naked, which is the ruleset working.

Bots are skipped in battlegrounds and arenas, exactly like players: the core restores them
at the end of the match, and re-rolling gear on every BG death would churn the DB for
nothing.

### 5.9 Waking at the graveyard

A survivor death leaves no ghost and no corpse run. In the same hook that lifts the gear,
the character is revived **alive at the closest graveyard**, at full health, with no
resurrection sickness, and the corpse is turned into bones. The container stays at the death
position, so the walk back is to the container, not to a body. Paths below are relative to
`src/server/game/` in the pinned core:

- `Player::RepopAtGraveyard()` already does exactly this for the deaths the core treats as
  unreachable (a fly-only zone, a death under the map): it calls
  `ResurrectPlayer(0.5f); SpawnCorpseBones();` (`Entities/Player/Player.cpp:5024-5030`) and
  then teleports to `sGraveyard->GetClosestGraveyard(this, GetTeamId())`
  (`Player.cpp:5034-5049`) [V]. The survivor path generalizes a path the realm already runs,
  rather than inventing one; it passes `1.0f` where the core passes `0.5f`, because the gear
  is already the penalty and a half-health walk back with no equipment only invites a second
  death.
- `ResurrectPlayer(float restore_percent, bool applySickness = false)` (`Player.h:2074`, impl
  `Player.cpp:4564`) turns the `1.0f` into full health, applies no sickness by default,
  removes the ghost auras, sets `DeathState::Alive`, clears `m_deathTimer`, and sends
  `SMSG_DEATH_RELEASE_LOC` with `-1`, which drops the spirit-healer marker [V].
- `SpawnCorpseBones(bool triggerSave = true)` (`Player.h:2066`, impl `Player.cpp:4782-4798`)
  clears `_corpseLocation` and converts the corpse to bones [V].
- `RepopAtGraveyard()` may be called while alive - its own comment says so - and it stops the
  repop countdown; it sends the spirit-healer position only when the player is dead, so the
  resurrection comes first and the repop second [V].
- The destination is script-overridable: `Graveyard::GetClosestGraveyard(Player*, TeamId,
  bool nearCorpse)` calls `OnPlayerBeforeChooseGraveyard(player, teamId, nearCorpse,
  uint32& graveyardOverride)` first (`Misc/GameGraveyard.cpp:97-122`) [V], so a later ruleset
  can move the destination without a core patch.
- An instance death resolves through that map's `graveyard_zone` rows, so it lands at the
  instance's own graveyard and the player can walk back in to the container [V].

Why the closest graveyard and not the bound inn: `m_homebindMapId`, `m_homebindX`,
`m_homebindY` and `m_homebindZ` are public on `Player` (`Player.h:2420-2424`) [V], so
teleporting to the bound inn would be a two-line change - but the hearthstone already does
exactly that on demand, and the inn is not on the way to the container: it adds a travel leg
before the recovery walk without adding a decision. A *nearest* inn would be worse again:
the core only exposes innkeepers as a per-unit flag (`Unit::IsInnkeeper()`,
`Entities/Unit/Unit.h:814`) [V], so it would need its own faction-filtered scan of the
innkeeper spawns. The closest graveyard is the destination the core can already compute
for every map and instance.

## Schema

Three tables, created programmatically at startup the way mod-hardcore creates its own
(`Hardcore::EnsureSchema`, deliberately not a SQL update file: a failing module SQL aborts
the worldserver boot on this fork [V]).

```sql
CREATE TABLE IF NOT EXISTS `survivor_flags` (
  `guid` INT UNSIGNED NOT NULL,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `survivor_corpses` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid` INT UNSIGNED NOT NULL,
  `name` VARCHAR(12) NOT NULL,
  `map` INT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL, `y` FLOAT NOT NULL, `z` FLOAT NOT NULL, `o` FLOAT NOT NULL,
  `zone` INT UNSIGNED NOT NULL,
  `level` TINYINT UNSIGNED NOT NULL,
  `created_ts` INT UNSIGNED NOT NULL,
  `expires_ts` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_survivor_corpses_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `survivor_corpse_items` (
  `corpse_id` INT UNSIGNED NOT NULL,
  `slot_index` SMALLINT UNSIGNED NOT NULL,   -- loot slot order, 0..n
  `item_guid` INT UNSIGNED NOT NULL,
  `itemEntry` INT UNSIGNED NOT NULL,
  `creatorGuid` INT UNSIGNED NOT NULL,
  `giftCreatorGuid` INT UNSIGNED NOT NULL,
  `count` INT UNSIGNED NOT NULL,
  `duration` INT NOT NULL,
  `charges` TEXT NOT NULL,
  `flags` INT UNSIGNED NOT NULL,
  `enchantments` TEXT NOT NULL,
  `randomPropertyId` SMALLINT NOT NULL,
  `durability` SMALLINT UNSIGNED NOT NULL,
  `playedTime` INT UNSIGNED NOT NULL,
  `text` TEXT NOT NULL,
  PRIMARY KEY (`corpse_id`, `slot_index`),
  KEY `idx_survivor_corpse_items_guid` (`item_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

`survivor_corpse_items` mirrors `item_instance` column for column (same names, same types,
`itemEntry` capitalised like the core's dump) so recovery is a straight re-insert and the
payload is auditable with plain SQL.

## Configuration

`conf/mod-survivor.conf.dist` -> `etc/modules/mod-survivor.conf`:

| key | default | meaning |
|---|---|---|
| `Survivor.Enable` | `1` | master switch; with `0` the command reports disabled and no death does anything |
| `Survivor.MinLevelToFlag` | `10` | keeps a brand-new character from opting in before it owns anything |
| `Survivor.AnnounceDeaths` | `1` | server-wide message on each survivor death |
| `Survivor.DropEquipped` | `1` | include the equipped slots (including the equipped bags) |
| `Survivor.DropBags` | `1` | include the backpack and the bag contents |
| `Survivor.ContainerLifetimeDays` | `7` | how long the container stands before its items are mailed back |
| `Survivor.MarkOnMap` | `1` | send the `SURV` addon message |
| `Survivor.ContainerEntry` | `5000000` | the gameobject template entry (the SQL above) |
| `Survivor.ApplyToBots` | `1` | the ruleset also applies to bot-controlled characters (§5.8) |
| `Survivor.BotRegenOnDeath` | `1` | re-roll a bot's gear from the playerbots factory on death; `0` leaves it naked |
| `Survivor.BotDropContainer` | `0` | give a bot death a lootable container too; off by default because it is a free-gear faucet |

## Activation and exclusivity

- `.survivor on` / `.survivor status`, `SEC_PLAYER`, `Console::No` - the same shape as
  `.hardcore on|status` (`modules/mod-hardcore/src/Hardcore.cpp:376-460`).
- `.survivor on` refuses when the character is Hardcore-flagged. The check reads
  `hardcore_flags` directly (`SELECT enabled FROM hardcore_flags WHERE guid = ?`), so
  mod-survivor does not link against mod-hardcore and stays buildable if it is removed.
- `.hardcore on` refuses when the character is Survivor-flagged: a small addition to the
  vendored mod-hardcore that reads `survivor_flags` the same way, tolerating the table being
  absent.
- Both refuse a headless (bot) session.

## Edge cases

| case | behaviour | why |
|---|---|---|
| death in an instance | container is created in the instance and the character wakes at that instance's closest graveyard, so it can walk back in; the container can only be reached while the instance exists | the row records the instance map and expires; items are mailed on expiry; the instance's `graveyard_zone` row supplies the destination (§5.9) |
| death in a battleground or arena | **no drop and no revival** - the core's own death handling runs, and the character is restored at the end of the match | a container in a BG would be unreachable after the match, and a graveyard teleport would fight the match logic |
| death while duelling | drop happens, same as any death; the character wakes at the closest graveyard like any other survivor death | duel deaths already run the full death path |
| under-map / fatigue death | drop happens at the death position; the character is revived at the closest graveyard exactly as for any other survivor death | same path the core already takes for unreachable deaths (`Player.cpp:5024-5030`); the container is not the corpse |
| logout after a death, before recovering | nothing special: the character is already alive and the container is a DB row, not a live object | §5.5 |
| server restart | container re-summons on the owner's next login/map entry | §5.5 |
| bags full on recovery | item is mailed with the core's recovery mail | §5.4 |
| bank / mail / gold / quest items | untouched | the ruleset is gear and bags only |
| second death before recovering the first | a second container is created; both are independent and both marked (the addon keeps the newest) | rows are keyed by id, not by guid |
| bot death | gear and bags destroyed, a fresh set rolled for class/level/spec, no container, no map message | §5.8 |
| bot death in a battleground/arena | skipped | the core restores the bot at match end |
| bot with no gear (level 1, freshly spawned) | skip the re-roll, nothing to take | avoids a pointless DB churn |
| container map unloaded | row survives; re-summons on next entry | §5.5 |
| identical items with different enchants | the loot window shows the same name for both; recovery matches `(entry, count, randomPropertyId)` in slot order, so the player can receive the other one of the pair | bounded, documented; the two are interchangeable in name and suffix |

## Failure modes and mitigations

| risk | mitigation |
|---|---|
| `LootItem` cannot carry an item instance | the instance lives in the DB row; the loot window is display + ordering only (§5.4) |
| a hand-filled `go->loot` is cleared by `SendLoot` | `lootId = 0` in the template (§5.3) |
| a summoned chest that never despawns | `consumable = 1` + non-zero respawn time -> core deletes it on release (`GameObject.cpp:872-876`) |
| loot lost when the container expires | mail the remaining items at expiry |
| a player loots another player's container | `OnAllowedToLootContainerCheck`, inverted boolean handled explicitly (§5.6) |
| the item row is lost between destroy and insert | both happen in one characters-DB transaction; the snapshot row is written *before* the item is destroyed |
| double-flagging | both commands check the other table; both tolerate a missing table |
| a module SQL failure aborting boot | no module SQL for the tables (programmatic creation), only the gameobject template row |

## Verification

What was run, and what it proves:

- **Compiles.** Both translation units (`Survivor.cpp`, `survivor_loader.cpp`) build clean against
  the pinned core and playerbots headers, using the exact compile line CMake generated for an
  existing module in `azerothcore-wotlk/build` (same defines, same 233 include dirs, `-std=gnu++20`),
  with only `-I modules/mod-survivor/src` added. `Hardcore.cpp` compiles the same way with the
  survivor guard in it.
- **SQL.** Checked against the base dumps: entry `5000000` has no stock row in either table, the
  INSERT column lists equal the schema column lists in order and arity (35 and 9), `Data0 = 0`,
  `Data2 = 1`, `Data8 = 0`, all gold columns 0, and display 4093 is a stock model. The file starts
  with `DELETE`, so re-applying it is safe.
- **Installer.** `bash -n` passes. The eleven `SURVIVOR_CONF_DEFAULTS` equal the shipped
  `.dist` key for key. `survivor_value_ok` accepts every shipped default and a deliberate `0`,
  and rejects `Enable = 7`, `MinLevelToFlag = 0`, `ContainerLifetimeDays = 366`, `ContainerEntry = 0`
  and an empty entry. The `run_checks` block was run against a conf with two broken keys: `--status`
  reports both, `--repair` restores both, and the re-check passes (`inside_repair` resets
  `PROBLEMS` and re-runs `run_checks 0`).
- **Add-on.** `Survivor.lua` compiles under a Lua interpreter and was exercised against a 3.3.5 API
  shim (login `hello` + throttle, `site`/`none` parsing, world→map fraction, minimap bearing and
  distance, `/surv clear`, foreign-prefix filtering). The launcher's `LocalAddonsInstallTest`
  passes with the new assertions (`./gradlew --offline testDebugUnitTest`).
- **Pin gating.** The world-map pin is shown by comparing the frame's continent index with the
  site's continent index, both through the same table, so it does not depend on whether the client
  counts continents from 0 or 1. An earlier draft read a map id out of `GetMapInfo()`; that
  return is a height in 3.3.5 and would never have matched, so it was removed.
- The bot path compiles against the playerbots headers: a module sees every subdirectory of
  every module as an include directory (`modules/CMakeLists.txt:208` `CollectIncludeDirectories`
  -> `:244-248` `target_include_directories(... PUBLIC ...)`), so `#include
  "PlayerbotFactory.h"` resolves the same way mod-hardcore's `#include "Playerbots.h"` does.
- **Graveyard revival, compile-level only.** What was checked is the core contract the path
  relies on, not a running server: `ResurrectPlayer` is the call the core's own unreachable-death
  branch makes (`Player.cpp:5024-5030`, at `0.5f`; the module passes `1.0f`), `applySickness`
  defaults to `false` (`Player.h:2074`), `SpawnCorpseBones` converts the corpse and clears
  `_corpseLocation` (`Player.cpp:4782-4798`), and the destination is
  `sGraveyard->GetClosestGraveyard(this, GetTeamId())` (`Player.cpp:5034-5049`). The module
  still compiles against the pinned headers. Nothing in this path was run in a client or on
  the Thor.
- **Not verifiable here:** the in-game feel - that the container is visible at the death
  spot, that the loot window lists the items, that the map pin lands on the right coordinates,
  that a looted item comes back enchanted, and that the character wakes alive at the closest
  graveyard at full health, with no resurrection sickness and bones where the corpse was.
  Those need a client session on the Thor.
