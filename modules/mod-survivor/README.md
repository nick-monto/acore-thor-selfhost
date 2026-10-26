# mod-survivor

A second opt-in ruleset next to [mod-hardcore](../mod-hardcore): dying costs you
everything you carry, but it does not end the character.

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
revival there.

The bank, the mail, the gold and the quest log are never touched. Opt-in is
one-way, like Hardcore: there is no `.survivor off`. `.survivor status` reports
the flag.

## Exclusivity

`.survivor on` refuses a character that `hardcore_flags` reports as enabled, and
`.hardcore on` refuses a Survivor-flagged character. Both commands refuse
bot-controlled (headless) sessions. mod-survivor reads `hardcore_flags` with a
plain query and tolerates the table being absent, so it neither links against
mod-hardcore nor depends on it being installed.

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
| `Survivor.BotDropContainer` | `0` | give a bot death a container too; off because it is a free-gear faucet |

With `Survivor.DropBags = 0` the equipped bags are kept even when
`Survivor.DropEquipped = 1`, because destroying a bag destroys what it holds.

Unclaimed containers are swept every 30 s and at startup: once
`ContainerLifetimeDays` has passed, the remaining instances are mailed to the
owner, then the rows and the object are deleted. A container also survives a
logout and a server restart - it is re-summoned from its row when the owner
re-enters that map.

## Tables

Created programmatically at startup (`Survivor::EnsureSchema`), not by a module
SQL file: on this fork a failing module SQL aborts the worldserver boot.

| table | contents |
|---|---|
| `survivor_flags` | `guid`, `enabled` - who opted in |
| `survivor_corpses` | one row per death: `id`, `guid`, `name`, `map`, `x`, `y`, `z`, `o`, `zone`, `level`, `created_ts`, `expires_ts` |
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
* The container is a summoned chest with `SetOwnerGUID` and a hand-filled
  `GameObject::loot`: one `LootItem` per saved instance, `itemIndex` equal to the
  vector index and `unlootedCount` incremented by hand (the core only does that
  inside `Loot::AddItem`).
* `GlobalScript::OnAllowedToLootContainerCheck` is the owner gate. Its boolean is
  inverted in this core - returning `true` means *deny* - so the module returns
  true only for another player trying to open one of its containers.
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
rules with one difference: its equipment and bags are **destroyed, not dropped**
(`Survivor.BotDropContainer = 0`), so a randombot death never feeds the one real
player. The strip happens in `OnPlayerJustDied`; the re-roll is deferred to the
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
