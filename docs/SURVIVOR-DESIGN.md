# Survivor mode

A second opt-in ruleset, one rung below Hardcore on a one-way ladder: dying costs you
everything you carry, but it does not end the character. On death the equipped items and the
contents of the bags are lifted off the body and placed in a lootable container at the death
position, and that position is marked on the player's map until the container is emptied.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported,
**[I]** inference.

## Why a second ruleset

Hardcore (`modules/mod-hardcore`, `.hardcore on`) is permadeath: one death and the
character is finished. That is a different game from "death is expensive". Survivor keeps
the character and makes the death itself the punishment, which is what a solo realm needs:
losing the gear you spent the evening gathering hurts, and the run back to the container is
the content.

The two rulesets stack in one direction only. The ladder is normal -> Survivor -> Hardcore,
and normal -> Hardcore. A Survivor character may be promoted to Hardcore, and the promotion
drops the Survivor flag: a death that is already final must not become recoverable, so the
two rulesets are never both set for one character (§5.9). The other direction is refused -
`.survivor on` rejects a Hardcore character, because Hardcore is terminal. Both commands
refuse bot-controlled sessions.

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
   resurrection sickness, and turns the corpse into bones (§5.10). This is not conditional on
   the drop: a survivor who dies with nothing left to take still wakes at the graveyard.
   There is no release and no run back to a body: the walk back is to the container, which
   stays where the character died.
6. Looting the container returns each item as the *same* instance it was, into a free bag
   slot; anything that does not fit is mailed, never dropped on the floor.

Opt-in is one-way, like Hardcore: there is no `.survivor off`. `.survivor status` reports
the flag.

## Mechanisms, with the evidence

### 5.1 When the snapshot runs

`PlayerScript::OnPlayerJustDied(Player*)` - declared `Scripting/ScriptDefines/PlayerScript.h:248`,
fired from `Player::Kill` at `Entities/Player/Player.cpp:4671`, **before** the corpse is
created (`Player.cpp:4671-4672`) [V]. It is **not** map-thread-only: `Player::KillPlayer` is
also reached from the session thread by `WorldSession::HandleRepopRequestOpcode`
(`Handlers/MiscHandler.cpp:80`, the release-before-the-update case), by the under-map death
path (`Handlers/MovementHandler.cpp:520`) and by logout (`Server/WorldSession.cpp:715`), while
the ordinary death comes through the map update (`PlayerUpdates.cpp:322`). Every shared cache
is mutex-guarded for that reason (§5.5).

That is the only hook that sees the items still equipped. `OnPlayerReleasedGhost`
(`Player.cpp:4561`, at the end of `BuildPlayerRepop`) fires after the corpse exists and is
reached from both threads [V]. A survivor normally never gets there, because the revive runs
on the next update - but a release that beats that tick does reach it, and the handler
revives there instead of letting the ghost run (§5.10).

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
visible and clickable, and who may open it is enforced in code (§5.6), not by a gameobject
flag: no `GAMEOBJECT_FLAGS` value expresses a player identity
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

Persistence: `survivor_corpses` holds `map, instance, x, y, z, o, zone, created_ts, expires_ts`,
the owner guid (the character that died) and `no_mail`. A summoned gameobject is never saved
(`SummonGameObject` sets neither `m_spawnId` nor a DB row, `Object.cpp:2401-2425`;
`SaveRespawnTime` is guarded on `m_goData && m_spawnedByDefault`, `GameObject.cpp:1236-1241`)
[V], so the module re-summons from the row when **any** Survivor-flagged character enters the
map, and on that character's login. The select is not keyed on the owner - a container belongs
to whoever plays the ruleset, and its owner may never come back at all, a bot that moved on or
a Hardcore character that fell (§5.9) - but it is keyed on the instance: `WHERE map = ? AND
instance = ?`. Without the instance id a row written in one instance of a dungeon would be
re-summoned into whichever instance the next Survivor walked into, at coordinates from another
run. World maps are always instance 0, so the filter is exact there. The hook still does
nothing for a character that is not flagged, so it costs non-Survivors no query.

The claim on a row is taken under the module's mutex before the snapshot is loaded, because
this runs from the login handler (world thread) and from the zone-change hook (map thread):
two characters entering the same map at the same time must not both summon the same row, which
would leave two chests holding one saved slot - and the second loot of that slot would find no
row left, so the looter would keep the core's template-level copy instead of the real
instance.

`Survivor.ContainerLifetimeDays` (default 7) becomes the `respawnTime` handed to
`SummonGameObject`; expiry destroys the object. Expired rows are swept at startup and every
30 s from `WorldScript::OnUpdate` (`SweepIntervalMs`). The remaining items are then mailed to
the owner - unless the row's `no_mail` is set, in which case the `item_instance` rows are deleted
and the gear is gone: a bot is re-geared by the factory, and a Hardcore character is a ghost
for good, so a mailbox would only hide the gear instead of letting it go (§5.8, §5.9).
`no_mail` is a schema column, not a config key.

### 5.6 Whoever plays the ruleset may open any container

`GlobalScript::OnAllowedToLootContainerCheck(Player const*, ObjectGuid source)`
(`ScriptDefines/GlobalScript.h:101`) is the last gate before `SMSG_LOOT_RESPONSE`
(`Player.cpp:8373-8377`) [V]. **Its boolean is inverted**: the wrapper uses
`CALL_ENABLED_BOOLEAN_HOOKS` without negation (`GlobalScript.cpp:112-115`,
`ScriptMgrMacros.h:69-73`) and the core denies on `false`, so returning `true` from the hook
means deny [V].

The rule is the ruleset, not the owner. Any character flagged Survivor may open **any** death
container on the realm - its own, another player's, a bot's, a fallen Hardcore character's -
and a character that never opted in may open none. The module returns deny (true) only when
the looter is not flagged; everything else, including gameobjects it does not manage, is
left at `false` so the core's own verdict stands:

- A container belongs to whoever plays the ruleset. Its owner may never come back - a bot
  moves on and is re-geared by the factory, a Hardcore character is finished - and a
  container nobody can open is litter, not content.
- It stops a normal alt from farming the containers a Survivor left behind. Opting in is the
  price of the gear, and the flag is one-way (§5.9), so the access cannot be borrowed for one
  loot.

The lookup is two in-memory maps under the module mutex (`GoToCorpse`, `FlaggedGuids`); the
hook never touches the database or the map.

Bots never flag themselves: `.survivor on` refuses a headless session and `Survivor::Flag`
refuses one too, matching the hardcore module's `IsBotControlled`
(`modules/mod-hardcore/src/Hardcore.cpp`, `WorldSession::IsHeadless`). A bot is therefore
never flagged, so the same check denies it - there is no separate bot branch in the gate.

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
same rules: it drops what it carries and is replaced with a fresh random set for its class,
level and current specialization.

`Survivor.BotDropContainer = 1` (default) is what makes the drop reachable: a bot death
leaves a lootable container at the death spot like a player death does, and **any
Survivor-flagged character may loot it** (§5.6) - the bot that dropped it is not the owner of
the access. That is deliberate on a solo realm: the bot does not need its gear back (the
factory re-rolls it), so the container is a source of level-appropriate gear and bags for the
one real player, for the same reason the loot bags exist. The row is written with
`no_mail = 1`, so a bot container that nobody loots rots at `Survivor.ContainerLifetimeDays`
and its items are **not** mailed anywhere: a mailbox no bot reads would only hide the gear
instead of letting it go.

The alternative, `Survivor.BotDropContainer = 0`, destroys a bot's gear outright and leaves
nothing behind. Worth knowing when tuning a realm's economy: with `1` every bot death is a
renewable gear faucet, and the faucet's rate is the bot death rate.

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

### 5.9 The mode ladder, and the hardcore death container

The two rulesets are one-way steps on a single ladder: normal -> Survivor -> Hardcore, and
normal -> Hardcore. A character can only move up. That is what keeps the pair from
contradicting each other - a death that is final must not be recoverable - without forbidding
the promotion a player may want.

- `.hardcore on` on a Survivor-flagged character is **allowed**. The command reads
  `survivor_flags` with a plain query (a missing table means "not flagged"), deletes the row
  (`ClearSurvivorFlag`) so exactly one ruleset is stored for the character, then flags
  Hardcore and prints "Hardcore ENABLED for X. Survivor mode is dropped: from here on this
  character permanently falls on death."
- `.survivor on` still refuses a Hardcore character: "Hardcore is terminal: it cannot be
  combined with Survivor." Nothing can walk back down the ladder.
- Neither flag can be removed: there is no `.survivor off` and no `.hardcore off`. The one
  and only removal is the promotion dropping the Survivor opt-in.

The promotion is immediate in the database, but mod-survivor keeps its own in-process copy of
the flag until the next boot, so its cold paths ask the hardcore table rather than trusting
the cache: `HandleDeath` runs `SELECT enabled FROM hardcore_flags WHERE guid = ?` (one query,
once per death) **before** it looks at the Survivor flag, `ReviveAtGraveyard` returns without
reviving a Hardcore-flagged character, and `.survivor status` answers "Survivor: superseded -
this character is flagged Hardcore, and Hardcore is terminal." None of these paths links
against mod-hardcore; a missing table simply means "not Hardcore".

A Hardcore death therefore leaves a container too, and it leaves no revival at all:

- `HandleDeath` sees `hardcore_flags` first and calls `DropGearAtDeath(player, cfg,
  /*noMail=*/true)`. The items are lifted into `survivor_corpses` / `survivor_corpse_items`
  exactly as for a survivor, the container is summoned at the death spot, and the same `SURV`
  map pin goes to the dying character (§5.7). It is governed by the same `Survivor.Enable`,
  `Survivor.DropEquipped`, `Survivor.DropBags` and `Survivor.ContainerEntry` switches as a
  survivor drop, and announced server-wide when `Survivor.AnnounceDeaths = 1`.
- Then it returns: no revive is queued and no ghost handling changes. Hardcore's own policy
  runs as before (`modules/mod-hardcore/README.md`), which by default makes the character a
  ghost that can never be resurrected.
- The row is written with `no_mail = 1`, so when the container expires its instances are
  deleted instead of mailed to a character that can no longer be played (§5.5).
- The fallen character is not Survivor-flagged any more, so it cannot open its own container.
  Any Survivor-flagged character can (§5.6) - which is the point on a solo realm: the gear of
  a finished run stays reachable for the next one.
- `Hardcore.DeathPolicy = archive` and `delete` kick and rename or delete on logout, and the
  container still simply rots at expiry, because `no_mail` is already set.

### 5.10 Waking at the graveyard

A survivor death leaves no ghost and no corpse run. Every survivor death outside a
battleground or arena is queued for a revival - whether or not the drop found anything to
take - and the character comes back **alive at the closest graveyard**, at full health, with
no resurrection sickness, any corpse it left behind turned into bones. The container, when there is one,
stays at the death position, so the walk back is to the container, not to a body. Paths below
are relative to `src/server/game/` in the pinned core:

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
  `instance` INT UNSIGNED NOT NULL DEFAULT 0,
  `x` FLOAT NOT NULL, `y` FLOAT NOT NULL, `z` FLOAT NOT NULL, `o` FLOAT NOT NULL,
  `zone` INT UNSIGNED NOT NULL,
  `level` TINYINT UNSIGNED NOT NULL,
  `created_ts` INT UNSIGNED NOT NULL,
  `expires_ts` INT UNSIGNED NOT NULL,
  `no_mail` TINYINT UNSIGNED NOT NULL DEFAULT 0,
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

`survivor_corpses.no_mail` is a column, not a knob: `1` means "this container rots at expiry
instead of being mailed to its owner". It is set for bot containers and for Hardcore death
containers (§5.5, §5.8, §5.9). Rows existing before the column was added get it through a
guarded `ALTER` - the module asks `information_schema` first, so a duplicate `ALTER` never
logs an error on boot.

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
| `Survivor.BotDropContainer` | `1` | a bot death leaves a lootable container that any Survivor-flagged character may take; `0` destroys the gear outright (§5.8) |

## Activation and the ladder

- `.survivor on` / `.survivor status`, `SEC_PLAYER`, `Console::No` - the same shape as
  `.hardcore on|status` (`modules/mod-hardcore/src/Hardcore.cpp:376-460`).
- `.survivor on` refuses a character that `hardcore_flags` reports as enabled. The check reads
  the table directly (`SELECT enabled FROM hardcore_flags WHERE guid = ?`), so mod-survivor
  does not link against mod-hardcore and stays buildable if it is removed.
- `.hardcore on` **accepts** a Survivor-flagged character and drops the Survivor flag: it
  deletes the `survivor_flags` row and reports that Survivor mode is dropped. This is the only
  way either flag is ever removed (§5.9).
- The ladder is therefore normal -> Survivor -> Hardcore, and normal -> Hardcore. Nothing moves
  down: there is no `.survivor off` and no `.hardcore off`, and a Hardcore character can never
  become a Survivor.
- `.survivor status` on a Hardcore character reports "Survivor: superseded - this character is
  flagged Hardcore, and Hardcore is terminal.", because this process's flag cache may still
  carry the opt-in the promotion removed.
- Both commands refuse a headless (bot) session.

## Edge cases

| case | behaviour | why |
|---|---|---|
| death in an instance | container is created in the instance and the character wakes at that instance's closest graveyard, so it can walk back in; the container can only be reached while the instance exists | the row records the instance map and expires; items are mailed on expiry; the instance's `graveyard_zone` row supplies the destination (§5.10) |
| death in a battleground or arena | **no drop and no revival** - the core's own death handling runs, and the character is restored at the end of the match | a container in a BG would be unreachable after the match, and a graveyard teleport would fight the match logic |
| death while duelling | drop happens, same as any death; the character wakes at the closest graveyard like any other survivor death | duel deaths already run the full death path |
| under-map / fatigue death | drop happens at the death position; the character is revived at the closest graveyard exactly as for any other survivor death | same path the core already takes for unreachable deaths (`Player.cpp:5024-5030`); the container is not the corpse |
| death with nothing left to drop (a survivor who already lost everything) | no second container, no announcement and no map pin, but the revival is the same: the character wakes at the closest graveyard | the revival is queued for every survivor death outside a battleground; the drop is a separate half of the ruleset (§5.10) |
| death, then logout before the revive tick | the logout itself releases the ghost, which is the same revive: the character leaves alive and comes back alive | `WorldSession::LogoutPlayer` calls `BuildPlayerRepop` for a character with a death timer (`Server/WorldSession.cpp:708`), which fires `OnPlayerReleasedGhost`, and the handler revives (§5.10) |
| death, then a crash or restart before the revive tick | the character logs in alive at the death spot, at half health, standing on its own container - not a ghost, but not the graveyard wake either | the queued revive is in memory; on login the core sees a dead character with no saved corpse and resurrects it in place at `0.5f` (`Player::LoadCorpse`, `Entities/Player/PlayerStorage.cpp:5948-5962`) before `OnPlayerLogin` runs. A one-tick window, and the outcome keeps the container under the player |
| logout after a death, before recovering | nothing special: the character is already alive and the container is a DB row, not a live object | §5.5 |
| server restart | container re-summons when any Survivor-flagged character enters that map (or logs in on it) | §5.5 |
| bags full on recovery | item is mailed with the core's recovery mail | §5.4 |
| bank / mail / gold / quest items | untouched | the ruleset is gear and bags only |
| second death before recovering the first | a second container is created; both are independent and both marked (the addon keeps the newest) | rows are keyed by id, not by guid |
| bot death | gear and bags destroyed, a lootable container left where it fell, a fresh set rolled for class/level/spec, no map message | §5.8 |
| bot container nobody loots | rots at expiry; its items are dropped, not mailed (`no_mail = 1`) | §5.8 |
| any Survivor-flagged character finds a container | may open it - its own, another player's, a bot's, a fallen Hardcore character's; there is no owner check | §5.6 |
| a character that never opted in finds a container | cannot open it, whatever its level or relation to the character that died | §5.6 |
| container whose owner never returns (a bot that moved on, a fallen Hardcore character) | stays openable and marked for whoever plays the ruleset; rots at expiry instead of being mailed (`no_mail = 1`) | §5.5, §5.6 |
| a Hardcore character dies | gear and bags go into a container at the death spot, announced if `Survivor.AnnounceDeaths = 1`; no revival, and the Hardcore death policy applies unchanged | §5.9 |
| a Survivor is promoted to Hardcore (`.hardcore on`) | the `survivor_flags` row is deleted, the character falls permanently on its next death, and its later containers are `no_mail` ones | §5.9 |
| a fallen Hardcore character tries `.survivor on` | refused - "Hardcore is terminal: it cannot be combined with Survivor" | §5.9 |
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
| a non-flagged character opens a container | `OnAllowedToLootContainerCheck` denies every looter that is not Survivor-flagged; inverted boolean handled explicitly (§5.6) |
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
