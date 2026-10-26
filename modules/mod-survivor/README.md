# mod-survivor

A second opt-in ruleset next to [mod-hardcore](../mod-hardcore), one rung below it on a
one-way ladder: dying costs you everything you carry, but it does not end the character.

`.survivor on` flags the character. From then on, each death:

1. snapshots every equipped item and every item inside the bags (and the bags
   themselves) into the characters database, with the exact instance state -
   enchantments, gems, durability, charges, random property, creator, text;
2. removes those items from the character;
3. summons one lootable container at the death position holding exactly those
   items;
4. marks that position on the player's map through the `SURV` addon message;
5. revives the character at the closest graveyard on its next update - full
   health, no resurrection sickness, no ghost and no corpse run: you wake at the
   graveyard and walk back to the container. This is independent of the drop: a
   survivor who dies with nothing left to take still wakes up there;
6. returns each item, as the *same* instance, when it is looted. Anything that
   does not fit into the bags is mailed, never dropped on the floor.

The container is independent of the corpse, so nothing about the revival can
lose it: an unclaimed one stays where it fell until it is looted or expires.
Battleground and arena deaths keep the core's own handling - no drop and no
revival there. A character promoted to Hardcore is the other exception: its
death still drops a container, but it is never revived (see *The mode ladder*).

The bank, the mail, the gold and the quest log are never touched. Opt-in is
one-way, like Hardcore: there is no `.survivor off`. `.survivor status` reports
the flag.

## The mode ladder

The two rulesets are one-way steps on a single ladder: normal -> Survivor -> Hardcore, and
normal -> Hardcore.

- `.hardcore on` **accepts** a Survivor-flagged character: it deletes the `survivor_flags` row
  (so exactly one ruleset is stored) and reports "Survivor mode is dropped". That is the only
  way either flag is ever removed.
- `.survivor on` refuses a character that `hardcore_flags` reports as enabled - "Hardcore is
  terminal: it cannot be combined with Survivor". Nothing walks back down the ladder, and
  there is no `.hardcore off` either.
- `.survivor status` on a Hardcore character answers "Survivor: superseded", because this
  process's flag cache can still hold the opt-in the promotion removed.
- Both commands refuse bot-controlled (headless) sessions. mod-survivor reads
  `hardcore_flags` with a plain query and tolerates the table being absent, so it neither
  links against mod-hardcore nor depends on it being installed.

A Hardcore death still leaves a container. `OnPlayerJustDied` checks `hardcore_flags` before
the Survivor flag: the equipped items and bags are lifted into a container at the death spot
(announced when `Survivor.AnnounceDeaths = 1`) and nothing is revived - the character stays
dead under `Hardcore.DeathPolicy`. The row is written with `no_mail = 1`, so the container
rots at expiry instead of mailing gear to a character that can no longer be played. The
fallen character is no longer Survivor-flagged and cannot open it; any Survivor-flagged
character can.

## Configuration

`conf/mod-survivor.conf.dist` -> `etc/modules/mod-survivor.conf`.

| key | default | meaning |
|---|---|---|
| `Survivor.Enable` | `1` | master switch; with `0` the command reports disabled and no death does anything |
| `Survivor.MinLevelToFlag` | `10` | keeps a brand-new character from opting in before it owns anything |
| `Survivor.AnnounceDeaths` | `1` | server-wide message on each survivor death |
| `Survivor.DropEquipped` | `1` | include the equipped slots (including the equipped bags) |
| `Survivor.DropBags` | `1` | include the backpack, the equipped bags and the bag contents |
| `Survivor.ContainerLifetimeDays` | `7` | how long the container stands before its items are mailed back |
| `Survivor.MarkOnMap` | `1` | send the `SURV` addon message |
| `Survivor.ContainerEntry` | `5000000` | the gameobject template the module summons |
| `Survivor.ApplyToBots` | `1` | the ruleset also applies to bot-controlled characters |
| `Survivor.BotRegenOnDeath` | `1` | re-roll a bot's gear from the playerbots factory; `0` leaves it naked |
| `Survivor.BotDropContainer` | `1` | a bot death leaves a lootable container any Survivor-flagged character may take; `0` destroys the gear |

With `Survivor.DropBags = 0` the equipped bags are kept even when
`Survivor.DropEquipped = 1`, because destroying a bag destroys what it holds.

Unclaimed containers are swept every 30 s and at startup: once
`ContainerLifetimeDays` has passed, the remaining instances are mailed to the
owner - or deleted outright when the row has `no_mail = 1` (a bot's container, a Hardcore
death container) - then the rows and the object are deleted. A container also survives a
logout and a server restart: it is re-summoned from its row when any Survivor-flagged
character enters that map or logs in on it, because the character that died may never come
back.

## Tables

Created programmatically at startup (`Survivor::EnsureSchema`), not by a module
SQL file: on this fork a failing module SQL aborts the worldserver boot.

| table | contents |
|---|---|
| `survivor_flags` | `guid`, `enabled` - who opted in |
| `survivor_corpses` | one row per death: `id`, `guid`, `name`, `map`, `instance`, `x`, `y`, `z`, `o`, `zone`, `level`, `created_ts`, `expires_ts`, `no_mail` (`1` = rots at expiry instead of being mailed) |
| `survivor_corpse_items` | one row per saved instance: `corpse_id`, `slot_index`, `item_guid`, then `itemEntry`, `creatorGuid`, `giftCreatorGuid`, `count`, `duration`, `charges`, `flags`, `enchantments`, `randomPropertyId`, `durability`, `playedTime`, `text` - column for column the same names and types as `item_instance`, so recovery is a straight re-insert and the payload is auditable with plain SQL |

## SQL

`data/sql/world/base/survivor_10_gameobject.sql` creates entry `5000000`: a
CHEST named "Remains", display 4093 (the stock corpse pile), `Data0` (lootId)
`0` so the hand-filled loot is not cleared by `Player::SendLoot`, `Data2`
(consumable) `1` so the core deletes it once it is emptied, and no gold on the
addon row because `SendLoot` runs `generateMoneyLoot` for every gameobject
regardless of its lootId. The file is re-runnable: both rows are deleted first.

## Loader

Folder name `mod-survivor` -> the loader symbol the playerbots fork looks up is
exactly `Addmod_survivorScripts`, defined in
`src/survivor_loader.cpp`, which calls `AddSurvivorScripts()`.

## How it works

* `PlayerScript::OnPlayerJustDied` (before the corpse exists; usually the map
  thread, but the release-before-the-update and logout paths reach it from the
  session thread) takes
  the instance rows, destroys the live items and summons the container.
  - Every item is written as `item_instance` -> `survivor_corpse_items` -> the
    live item is destroyed, all three in one characters-DB transaction, so a
    crash can neither lose the instance nor destroy one that was never saved.
* `PlayerScript::OnPlayerUpdate` revives a survivor queued by the death. The
  revive cannot run inside `OnPlayerJustDied` (it is inside `Player::Kill`, before
  the corpse exists), so the guid waits in an in-memory set and the next update
  calls `Player::ResurrectPlayer(1.0f)`, `Player::SpawnCorpseBones()` and
  `Player::RepopAtGraveyard()` - the same sequence the core uses for a death it
  treats as unreachable. The guid is queued for every survivor death outside a
  battleground, so a character with nothing left to drop still revives. A logout
  that beats the tick is covered by `HandleLogin` (flagged and dead is enough - no
  container row needed), a Release by
  `OnPlayerReleasedGhost`, and a battleground or arena death is left alone.
* The container is a summoned chest with the dying character's GUID set as its owner (core
  bookkeeping only - access is decided by the loot hook, not by that GUID) and a hand-filled
  `GameObject::loot`: one `LootItem` per saved instance, `itemIndex` equal to the
  vector index and `unlootedCount` incremented by hand (the core only does that
  inside `Loot::AddItem`).
* `GlobalScript::OnAllowedToLootContainerCheck` is the access gate, and its boolean is
  inverted in this core - returning `true` means *deny*. The module denies every character
  that is not Survivor-flagged and allows everything else, so any Survivor may open any death
  container (their own, another player's, a bot's, a fallen Hardcore character's) and a
  character that never opted in can open none.
* `PlayerScript::OnPlayerLootItem` puts the saved instance back: the
  template-level copy the core created is destroyed, the row is re-inserted under
  its old guid, loaded with `Item::LoadFromDB` (the only lossless path -
  `StoreNewItem` regenerates enchants, gems and durability) and stored into the
  freed slot, or mailed when nothing fits.
* `AllGameObjectScript` receives the container's loot-state changes (the
  per-template `GameObjectScript` variant only fires for a template that names a
  script, and ours deliberately does not) and drops the in-memory link; empty
  containers lose their rows.
* The map marker is the addon whisper `SURV\tsite <mapId> <x> <y> <zoneId>
  <corpseId>` on death, on ghost release, on login with an unclaimed container,
  and in reply to `SURV\thello`; `SURV\tnone` clears it once the container is
  emptied. `Survivor.MarkOnMap = 0` turns the messages off.

## Bots

With `Survivor.ApplyToBots = 1` a bot-controlled character follows the same
rules: its equipment and bags are taken, and with `Survivor.BotDropContainer = 1`
(default) they are left in a lootable container where it fell. Any Survivor-flagged
character may open a bot's container - access is not tied to the bot - because the bot does
not need the gear back: the factory re-rolls it, so on a solo realm the container is a source
of level-appropriate gear. A bot container nobody loots rots at expiry and its items are
dropped, never mailed (`no_mail = 1`). With `Survivor.BotDropContainer = 0` the gear is destroyed outright.
The strip happens in `OnPlayerJustDied`; the re-roll is deferred to the
next `OnPlayerUpdate`, once the bot is alive and out of any battleground, because
calling the playerbots factory from inside `Player::Kill` would mutate a bot whose
AI is mid-action:

```cpp
PlayerbotFactory factory(bot, bot->GetLevel());
factory.InitBags(true);
PlayerbotFactory::AutoGear(bot, 0, 0, false);
```

`PlayerbotFactory::Randomize` is deliberately not used: it also resets talents,
skills, spells, quests and level, which would re-roll the bot's identity instead
of replacing its kit. A naked bot (nothing to take) is skipped entirely.
