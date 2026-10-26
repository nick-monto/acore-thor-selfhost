# Pawn system (Dragon's Dogma style)

A solo realm should feel like a party game, not a summoning game. The request: walk up to an
obelisk in a town, pick which bots travel with you, and have those bots fight, level and gear
alongside you as a stable roster rather than as anonymous bodies that happen to be nearby.

This document is a feasibility study, not an implementation. It states what the pinned
`mod-playerbots` and core already provide, what a pawn layer would have to add, and where the
real risks are. Every claim about existing behaviour carries a `path:line` reference against
`mod-playerbots@d750b4b` and `azerothcore-wotlk@8a4e76e`; claims that were not verified in
source are marked **[INFERENCE]**.

## Problem

The realm currently has 50 random bots and one real player. `patches/playerbots-player-centric.patch`
assigns those random bots to the player's zone so the world is not empty, but the relationship is
ambient: the player did not choose those bots, cannot keep them, and they are replaced as the zone
tier reshuffles. Nothing in the current setup gives the player a *party* with identity.

What a pawn system needs, in the game's terms:

1. A roster of named, persistent bots with a class and a role.
2. A place in town to swap them, and a UI to choose from.
3. Bots that stay in the group, follow the player, and level with them.
4. Bots that come back as they were, after a relog or a server restart.

## What the pinned playerbots already provides

This is the reason the feature is feasible at all: items 1, 3 and 4 exist upstream, and only the
roster and the UI are missing.

| Need | Existing mechanism | Evidence |
|---|---|---|
| Named, persistent bots with a class | "AddClass" bots: real characters on accounts marked type 2 in `playerbots_account_type`, one character per class per account. Created and levelled like any character, saved in `characters`. | `data/sql/playerbots/base/playerbots_account_type.sql`; pool collected at `src/Bot/RandomPlayerbotMgr.cpp:1769-1800`; accounts assigned at `src/Bot/RandomPlayerbotMgr.cpp:495-545` |
| A pool to pick from | `Playerbots.AddClassAccountPoolSize` (default 50) accounts × 10 classes = 500 characters, created at first boot | `conf/playerbots.conf.dist:99-102`; count math `src/Bot/Factory/RandomPlayerbotFactory.cpp:332-440`; creation `:630-780` |
| Recruit one specific bot | `PlayerbotHolder::AddPlayerBot(guid, masterAccountId)` — logs the character in as a bot; type-2 characters bypass the same-account check, so any player may recruit them | `src/Bot/PlayerbotMgr.cpp:88-150` |
| Chat-level command surface | `.playerbots bot add <name>`, `.playerbots bot addclass <class>`, `.playerbots bot remove <name>`, reachable by a non-GM player (`AddClassCommand = 1`). The root is `.playerbots`: there is no top-level `.bot` in this commit | `src/Bot/PlayerbotMgr.cpp:732-770`, `:1131-1253`, `:946-1000`; `conf/playerbots.conf.dist:140`; registration `src/Script/PlayerbotCommandScript.cpp:31-49` |
| Join the party and be summoned | `GroupInviteOperation` is queued on login and adds the bot to the master's group, converting a full party to a raid; `SummonWhenGroup = 1` summons it to the player | `src/Bot/PlayerbotMgr.cpp:606-621`; `conf/playerbots.conf.dist:193` |
| Level with the player | Type-2 bots are re-levelled to the master's level, and re-geared to the master's gearscore, whenever they log in more than 3 levels away | `src/Bot/PlayerbotMgr.cpp:639-651` |
| Gain XP alongside | Bots in a group with a real player are excluded from the randombot XP multiplier, so they earn XP at the normal rate | `src/Script/Playerbots.cpp:363-388` |
| Stay in the group | `KeepAltsInGroup` prevents bots from leaving when the master leaves | `conf/playerbots.conf.dist:152-155` |
| Caps | `MaxAddedBots` (default 40) limits added bots per player; `GroupInvitationPermission` gates invites | `src/Bot/PlayerbotMgr.cpp:115-136`; `conf/playerbots.conf.dist:136,150` |

Two consequences worth stating plainly:

- **Levelling needs no new code.** A pawn is a real character in a real group. It earns group XP,
  and it is re-levelled and re-geared on the next login if it fell behind. There is no "sync pawn
  level" feature to write.
- **The pawn pool already exists on the device, most likely.** The installer sets
  `Playerbots.MinRandomBots`/`MaxRandomBots` (`scripts/thor-install.sh:1266-1267`) but never touches
  `AddClassAccountPoolSize`, so the shipped default of 50 accounts applies. **[INFERENCE]** the
  realm therefore holds ~500 type-2 characters today, all level 1 and never logged in. Verify on
  the device before designing around it:
  ```sql
  SELECT account_type, COUNT(*) FROM acore_playerbots.playerbots_account_type GROUP BY account_type;
  SELECT COUNT(*) FROM acore_characters.characters
   WHERE account IN (SELECT account_id FROM acore_playerbots.playerbots_account_type WHERE account_type = 2);
  ```

## What is missing, and why

Three things, and only three:

1. **A roster.** Nothing records *which* bots belong to the player. `BotAutologin` exists, but it
   logs in every character on the player's own account (`src/Bot/PlayerbotMgr.cpp:1710-1760`), and
   pool pawns live on `rndbot*` accounts, not the player's. So today a recruited pawn is gone at
   the next relog and must be re-recruited by name. A roster table plus a login hook fixes that.
2. **A picker.** No surface exists to browse the pool or see a pawn's class, level and role.
3. **The obelisk.** Nothing in town triggers any of it.

## The obelisk

The obelisk is a gameobject with a `ScriptName`, and the menu is built in code. This is a supported
core path with an in-tree precedent, not a hack:

- `GameObject::Use` calls `sScriptMgr->OnGossipHello(player, go)` for **any** gameobject type,
  before the type switch, and a `GameObjectScript` bound to the go's script id owns the interaction
  by returning `true`: `src/server/game/Entities/GameObject/GameObject.cpp:1499-1513`,
  `src/server/game/Scripting/ScriptDefines/GameObjectScript.cpp:29-62`.
- Precedent: the Icecrown Citadel teleporter builds its whole gossip menu in `OnGossipHello` and
  routes clicks in `OnGossipSelect`: `src/server/scripts/Northrend/IcecrownCitadel/icecrown_citadel_teleport.cpp:44-88`.
- Menu construction helpers: `ClearGossipMenuFor` / `AddGossipItemFor` / `SendGossipMenuFor` /
  `CloseGossipMenuFor` in `src/server/game/AI/ScriptedAI/ScriptedGossip.h:85-101`. The overload the
  picker wants is the free-form text one at `:88` (`icon, text, sender, action`); the ICC script uses
  the DB-menu variant at `:94` because its rows exist.
- A click lands back in `OnGossipSelect(player, go, sender, action)` and must return `true`; for a
  gameobject source the core rejects any `action > GOSSIP_OPTION_QUESTGIVER`, and a code-built menu
  has no `GossipMenuItemData`, so returning `false` silently swallows the click:
  `src/server/game/Handlers/MiscHandler.cpp:206-212`, `src/server/game/Entities/Player/PlayerGossip.cpp:279-291`.

Limits that shape the UI: 16 options per page (the packet carries `// max count 0x10`,
`src/server/game/Entities/Creature/GossipDef.cpp:199-201`), and a hard ceiling of 32 per menu
enforced by a fatal `ASSERT`, not a clamp (`GossipDef.h:30`, `GossipDef.cpp:43`). A 500-entry pool
is therefore paged, not listed.

### Two surfaces, one protocol

The realm already ships custom client addons: the launcher installs `EInteract` and `Survivor`
into `Interface/AddOns` on every launch (`acore-thor-launcher`, `installLocalAddons`,
`app/src/main/java/app/gamenative/ui/screen/wow/WoWForeverScreen.kt:2029-2080`), and both talk to
their server module over the addon channel — `SendAddonMessage(prefix, payload, "WHISPER", ownName)`
(`app/src/main/assets/einteract/EInteract.lua:73`).

That gives two ways to present the picker, and the cheap plan is to build the small one first:

| Surface | Reaches | Effort | Limit |
|---|---|---|---|
| Gossip menu on the obelisk | any client, no addon | small: one `GameObjectScript` | 16 options/page, plain text only, no sorting or filtering |
| `PAWN` addon channel + a `Pawn` addon | this launcher only | medium: addon frame plus the same server calls | none worth worrying about; a list frame with class icons and role buttons |

Recommendation: server-side protocol first (`PAWN\tlist` → rows; `PAWN\trecruit <guid>`;
`PAWN\tdismiss <guid>`), with the gossip menu as a thin renderer over the same calls. The addon then
replaces the renderer without touching the model. This mirrors how `EInteract` is layered: the
module answers whispers, the addon only presents them.

## The pawn model

**A pawn is a type-2 addclass character, recruited into the player's party and recorded in a
roster.** Two sources are worth supporting:

1. **Pool pawns** — the ~500 existing type-2 characters. Instant to use, arrive levelled and geared
   to the player, not customisable, and the roster can offer a stable subset (say 12) so the obelisk
   reads like a rift rather than a database dump.
2. **Own-account pawns** — characters the player creates on their own account and then recruits.
   `AllowAccountBots = 1` already permits this (`conf/playerbots.conf.dist:161`), and `BotAutologin = 0`
   means they are not dragged in automatically. These are the ones a player would actually invest in:
   they keep their gear, their talents and their name, and they level purely by playing.

Both are the same roster row; the difference is only where the character came from.

**Composition.** A party holds 5, so 4 pawns beside the player (Dragon's Dogma gives 3). A raid
group is available but should not be the default: `GroupInviteOperation` converts a full party to a
raid automatically (`src/Bot/PlayerbotMgr.cpp:600-618`), so a 5th recruit silently becomes a raid.

**Roles.** Playerbots expresses behaviour as strategies, which is what the picker should expose:
tank, healer, dps. A pawn's class already suggests a role; the picker sets the strategy preset, and
the roster remembers it.

**Loot.** This needs a decision, because it is the one place where pawns can annoy a solo player.
Bots roll by config (`Playerbots.LootNeedRollLevel = 1`, `Playerbots.LootGreedRollLevel = 0`,
`conf/playerbots.conf.dist:316,321`) and keep looting under free-for-all only when
`Playerbots.FreeMethodLoot` is enabled (`:312`). With loot bags driving progression on this realm,
the recommended defaults are: group loot set to free-for-all, `FreeMethodLoot = 0`, so pawns fight
and do not compete for drops.

**Dismissal.** `.playerbots bot remove` logs the bot out (`src/Bot/PlayerbotMgr.cpp:754-770`), the character
saves itself, and the roster row is marked released. Re-recruiting later restores it as it was,
including gear, because it is the same character.

**Gearing a pawn.** No new code: a pawn is a player character, so the trade window works. The bot
accepts a trade from its master or from a group member and refuses everyone else ("I'm kind of busy
now", `src/Ai/Base/Actions/TradeStatusAction.cpp:25-38`), and when the trade completes
(`TRADE_STATUS_TRADE_ACCEPT`) `EquipUpgradesPacketAction` scans its bags and equips what is an
upgrade — defined as a score above `Playerbots.EquipUpgradeThreshold` (default 1.1×) of the item
already in that slot (`EquipAction.cpp:369-403`, `ItemUsageValue.cpp:290-310`). A sidegrade stays in
the bag. To force one specific item past that threshold, whisper the pawn `e <item link>`: the
`e` trigger runs the `equip` action, which parses `|Hitem:` links and equips exactly those
(`ChatCommandHandlerStrategy.cpp:42`, `ChatHelper::parseItems`). `.playerbots bot init=<quality>`
remains the blunt instrument that re-rolls the whole set.

Two config keys gate this, and both default to the useful value: `Playerbots.AutoEquipUpgradeLoot = 1`
(`conf/playerbots.conf.dist:291-293`) and `Playerbots.EnableRandomBotTrading = 1` (`:788-790`). The
trading key matters specifically for pawns because type-2 bots are treated as randombots by that
gate: `0` disables trading with them outright, `2` ("only buy") still accepts gifts, and `3`
("only sell") refuses them (`TradeStatusAction.cpp:40-49`, `:235-248`).

## What has to be written

### Roster schema

One table in the playerbots schema, so it lives beside the bot data it describes:

```sql
CREATE TABLE IF NOT EXISTS `playerbots_pawn_roster` (
  `owner_guid`  INT UNSIGNED NOT NULL,   -- the real player's character guid
  `bot_guid`    INT UNSIGNED NOT NULL,   -- the pawn's character guid
  `role`        TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 0 auto, 1 tank, 2 healer, 3 dps
  `state`       TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 0 offered, 1 recruited, 2 released
  `recruited_at` DATETIME NULL,
  PRIMARY KEY (`owner_guid`, `bot_guid`)
);
```

No `characters` change and no `world` change beyond the obelisk spawn.

### Patch surface

The work belongs in the playerbots patch, beside `playerbots-player-centric.patch`, not in a new
module: the code needs `PlayerbotMgr`/`RandomPlayerbotMgr` internals, and the patch route is the one
this repository has already proven builds and applies on the pinned commit. A separate module would
have to reach those headers from a different CMake target, which is unverified.

The patch adds:

1. `PawnRoster` — load, save, and `Recruit(Player*, ObjectGuid)` / `Dismiss(Player*, ObjectGuid)`
   wrapping `AddPlayerBot` and `LogoutPlayerBot`, plus the role-to-strategy preset.
2. `PlayerbotMgr::OnPlayerLogin` extension — re-recruit every `state = 1` row for that character
   before falling through to `BotAutologin` (`src/Bot/PlayerbotMgr.cpp:1710-1760`).
3. The obelisk `GameObjectScript`: `OnGossipHello` builds the page, `OnGossipSelect` routes recruit,
   dismiss and paging, both returning `true`.
4. The `PAWN` addon-whisper protocol, answered the way `mod-e-interact` answers `EINT`
   (`modules/mod-e-interact/src/e_interact.cpp:1-20` documents the pattern).

### Client side

A `Pawn` addon in the launcher's assets, installed by `installLocalAddons` like `EInteract` and
`Survivor`, with `LOCAL_ADDON_VERSION` bumped so existing installs pick it up. It shows the offered
pawns (name, class icon, level, role), the current party, and recruit/dismiss buttons.

### Wiring

Adding this touches, in order: `patches/` (new or extended patch), `sql/custom/db_world/`
(obelisk `gameobject_template` + spawns), `conf` assertions in `scripts/thor-install.sh`
(the `mod-survivor` handling at `:1345-1362` and `:1627-1631` is the template),
`.github/workflows/server-aarch64.yml` (patch application is already wired; SQL trees at `:316-329`,
release notes at `:396-400`), `pins.toml` if a patch file is added, and the launcher's assets plus
`installLocalAddons`.

## Interaction with the zone-tier random bots

`patches/playerbots-player-centric.patch` already skips bots that are grouped with a real player
(`:174`, `:193`, `:344` of the patch), so a recruited pawn is never teleported or reassigned by the
zone tier. The two features compose: pawns are *added* bots (`PlayerbotMgr`), the ambient crowd is
*world* bots (`RandomPlayerbotMgr`). The crowd cap (`RandomBotNearPlayersMaxPerPlayer`) still governs
how many strangers are around while the pawns are in the party.

## Decisions to take before coding

1. **Pool size.** 50 accounts (500 characters) is the shipped default and probably too much: it
   bloats the characters database and makes the picker meaningless. 3 accounts gives 30 pawns,
   enough for a rift-like roster. This is a one-line installer change.
2. **Offered roster.** Random stable subset of the pool per player (e.g. 12, keyed by character
   guid), or the whole pool paged by class. The first reads like Dragon's Dogma; the second is more
   useful for "I want a healer now".
3. **Own-account pawns allowed?** Cheap to allow (`AllowAccountBots` already does), and it is the
   only way to have a pawn the player actually built.
4. **Loot sharing** — see above. Recommend: pawns never compete.
5. **Death.** Pawns die and are resurrected by normal playerbots behaviour. Whether a dead pawn is
   auto-released from the roster, or waits in the group, is a taste decision.

## Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| `ASSERT` on gossip overflow | a 33rd menu item aborts the server, it does not clamp (`GossipDef.cpp:43`) | page at 16, never build a menu from an unbounded query |
| Goblin-level CMake for a module that includes playerbots headers | unverified on the pinned commit | keep the logic inside the patch |
| Pawns competing for loot bags | undermines the progression economy this realm is built around | free-for-all + `FreeMethodLoot = 0` |
| 500 unused characters | DB size, slower character cache, meaningless roster | reduce `AddClassAccountPoolSize` |
| Recruit during combat or teleport | `AddPlayerBot` queues a login; a bot joining mid-fight can arrive in a bad state | the obelisk is a town object; refuse while `IsInCombat()` |
| Addon-only picker excludes a stock client | the launcher installs the addon, a vanilla client would not have it | gossip fallback over the same server calls |

## Verification plan

None of this can be claimed done without a live server, because every mechanism above is a
playerbots runtime behaviour rather than a compile-time contract.

1. **Pool exists** — the two SQL queries in "What is missing" return non-zero type-2 rows.
2. **Recruit by command, before any code** — this proves the model with zero new C++:
   ```text
   .playerbots bot addclass warrior   # recruits a pool pawn, levelled and geared to you
   .playerbots bot list               # it is in your party
   .playerbots bot remove <name>      # it leaves
   ```
   If that works in game, the pawn model is sound and everything else is UI and persistence.
3. **Level alongside** — level the player 3+ levels with the pawn in the party; the pawn should have
   gained group XP, and on its next login it should be re-levelled and re-geared if it lagged.
4. **Roster** — recruit, relog, confirm the pawn returns; restart the world server, confirm again.
5. **Obelisk** — the gossip menu opens from the object, pages, and recruiting from it produces the
   same result as the command in step 2.
6. **Addon** — the frame lists the same rows the gossip menu shows, and both routes agree.

## Effort

Ordered by cost, and each step is independently useful:

| Step | Work | Size |
|---|---|---|
| 1 | Prove the model in game with `.playerbots bot addclass` (no code) | minutes |
| 2 | Roster table + recruit/dismiss + relog persistence in the patch | ~350 lines C++, 1 SQL file |
| 3 | Obelisk: `gameobject_template` row, spawns, `GameObjectScript` with a paged menu | ~250 lines C++, 2 SQL files |
| 4 | `PAWN` protocol + `Pawn` addon + launcher wiring | ~250 lines Lua, ~30 lines Kotlin |
| 5 | Config keys, installer and workflow wiring, runbook | ~100 lines |

The feature is feasible. The uncertainty is concentrated in step 3 and 4 presentation, not in the
model: the party, the levelling, the persistence and the pool are upstream behaviour that only
needs a roster and a doorway in front of it.
