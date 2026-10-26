# Pawn system: integration plan

The feasibility study is [PAWN-SYSTEM-DESIGN.md](PAWN-SYSTEM-DESIGN.md). This is the plan that
replaces its "what has to be written" section: the design decisions taken since, the route change
that removes the patch from the pawn work, the exact components, and the order to build them in.

Every claim about existing behaviour carries a `path:line` reference against
`mod-playerbots@d750b4b` and `azerothcore-wotlk@8a4e76e`.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | Pawns are offered at **player level ± 2** | A pawn you cannot use at your level is noise; a pawn that snaps to your exact level erases the choice. |
| D2 | A party holds **the player plus two pawns**; a third requires strangers from the world | Keeps the pawns special and makes the ambient crowd useful. |
| D3 | Each pawn is a **restricted archetype**: one premade PvE spec plus a hard spellbook blacklist | The player must swap pawns to get a toolkit, not just a body. |
| D4 | Pawns **never compete for loot**: they pass on every roll and never loot a free-for-all corpse | Loot bags are this realm's progression system. |
| D5 | The pawn doorway is the **obelisk only**: `Playerbots.AddClassCommand = 0` | Makes D2 strict by construction, with no patch to the command. |
| D6 | The pawn layer is a **new module**, `modules/mod-pawn`, not an extension of the playerbots patch | See "Route" below: the two facts the study called unverified are now verified. |

## Route: a module, not a patch

The study recommended extending `patches/playerbots-player-centric.patch` because a separate module
"would have to reach those headers from a different CMake target, which is unverified". That is now
verified, and it is safe:

- Every module's `src` directory is added to one shared include list, and static modules are
  compiled into a single library: `CollectIncludeDirectories(${MODULE_SOURCE_PATH} PUBLIC_INCLUDES)`
  and `add_library(modules STATIC ... ${PRIVATE_SOURCES_MODULES})` in
  `modules/CMakeLists.txt:270-300`; the build uses `-DMODULES=static`
  (`.github/workflows/server-aarch64.yml:132`). So `#include "Bot/PlayerbotMgr.h"` from
  `modules/mod-pawn/src` resolves, and `-DMOD_PLAYERBOTS` reaches the same target through
  `game-interface` (`modules/CMakeLists.txt:74-80`).
- Module SQL applies automatically: `UpdateFetcher.cpp:159-186` scans
  `modules/<name>/data/sql/<dir>` for every module in the compiled `AC_MODULES_LIST`
  (`src/server/apps/worldserver/Main.cpp:447`), matching the database name inside the directory
  name. `modules/mod-survivor/data/sql/world/base/` and
  `modules/mod-creature-gear/data/sql/world/base/` are the working precedents.

Nothing in the pawn feature needs to be *inside* a playerbots function, because four hooks already
exist:

| Need | Hook | Evidence |
|---|---|---|
| Know when a pawn logs in | `PlayerScript::OnLogin` — bots log in through the normal path | `botSession->HandlePlayerLoginFromDB(holder)` at `PlayerbotMgr.cpp:256` → `sScriptMgr->OnPlayerLogin(pCurrChar)` at `CharacterHandler.cpp:1126` |
| Defer work until the bot is in the world | `PlayerbotWorldThreadProcessor::QueueOperation(std::unique_ptr<PlayerbotOperation>)`, a public API over an abstract `PlayerOperation::Execute()` | `PlayerbotWorldThreadProcessor.h:62`, `PlayerbotOperation.h:26-39` |
| Refuse a spell | `PlayerScript::OnPlayerCanLearnSpell` is checked at the top of `Player::learnSpell` and returns bool | `Player.cpp:3428-3431` |
| Recruit and dismiss | `PlayerbotHolder::AddPlayerBot(guid, masterAccountId)` / `LogoutPlayerBot(guid)` | `PlayerbotMgr.cpp:88-150`, `:394` |

The player-centric patch is untouched by this work.

## Components

| File | Responsibility |
|---|---|
| `modules/mod-pawn/conf/mod-pawn.conf.dist` | `Pawn.*` keys: master switch, cap, level window, page size, obelisk entry, per-archetype blacklists |
| `modules/mod-pawn/data/sql/world/base/pawn_10_obelisk.sql` | `gameobject_template` 5000010 (type 10 GOOBER, displayId 7017 `Obelisk`), its addon row, and two spawns |
| `modules/mod-pawn/src/pawn_common.h` | Config struct, role enum, shared types |
| `modules/mod-pawn/src/PawnArchetype.h/.cpp` | Spec choice, blacklist name→id resolution, apply and prune |
| `modules/mod-pawn/src/PawnMgr.h/.cpp` | Config load, roster I/O, offer derivation, recruit/dismiss, login queue |
| `modules/mod-pawn/src/PawnObelisk.cpp` | `GameObjectScript` with the paged gossip picker |
| `modules/mod-pawn/src/PawnScripts.cpp` | `PlayerScript` login queue, spell gate; `WorldScript` that drains the queue |
| `modules/mod-pawn/src/pawn_loader.cpp` | `Addmod_pawnScripts()`, the symbol the loader generator looks up |
| `modules/mod-pawn/README.md` | What it does, how to configure it |

## Data model

Characters database, because every row belongs to a character pair and joins to `characters`. The
table is created **programmatically** (`PawnMgr::EnsureSchema`, `CREATE TABLE IF NOT EXISTS`), not by
module SQL: a failing module SQL file aborts the worldserver boot on this fork
(`docs/SERVER-STACK.md` §13, which is why mod-survivor creates its three character tables the same
way).

```sql
CREATE TABLE IF NOT EXISTS `pawn_roster` (
  `owner_guid`   INT UNSIGNED NOT NULL,          -- the real player's character
  `bot_guid`     INT UNSIGNED NOT NULL,          -- the pawn
  `spec_no`      TINYINT UNSIGNED NOT NULL,      -- Playerbots.PremadeSpecLink.<class>.<spec_no>
  `role`         TINYINT UNSIGNED NOT NULL,      -- 1 tank, 2 healer, 3 dps
  `state`        TINYINT UNSIGNED NOT NULL,      -- 1 in the party, 0 dismissed
  `target_level` TINYINT UNSIGNED NOT NULL,      -- the level the pawn was recruited at
  `updated_at`   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`owner_guid`, `bot_guid`),
  KEY `bot_guid` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

A dismissed row is kept (`state = 0`) so the archetype and level survive and re-recruiting the same
pawn gives the same pawn. `state = 1` rows are the party: they are what the cap counts and what the
login hook restores.

## Config

```ini
Pawn.Enable = 1
Pawn.MaxPawns = 2                  # D2
Pawn.LevelWindow = 2               # D1, in levels either way
Pawn.OfferedPerPage = 12           # gossip carries at most 16 rows per page
Pawn.ObeliskEntry = 5000010
Pawn.Debug = 0

# Archetype blacklists: the signature abilities of the *other* specs of the same class,
# by spell name. <class> is the ChrClasses id (1 warrior, 2 paladin, 3 hunter, 4 rogue,
# 5 priest, 6 death knight, 7 shaman, 8 mage, 9 warlock, 11 druid); <spec_no> is the
# Playerbots.PremadeSpecLink number. Names are resolved against the spell table at startup and
# an unresolved name is logged, never silently ignored.
Pawn.Blacklist.1.0 = Shield Wall, Last Stand, Taunt, Devastate, Shield Slam
```

Spell names, not ids: the server has no name→id table, but `SpellMgr::GetSpellInfo` and
`GetSpellInfoStoreSize()` (`SpellMgr.h:742`) make one scan at startup cheap, and a name is
reviewable in a config file where a bare id is not.

## Behaviour

### Offers

`RandomPlayerbotMgr::addclassCache` (`RandomPlayerbotMgr.h:172`) already holds the type-2 pool
per team and class, keyed by `GetTeamClassIdx(isAlliance, claz)` (`:168`). For a player, the offers
are that pool filtered to pawns that are offline, not in a real guild, and not already in this
player's party — the same filters the `addclass` command applies (`PlayerbotMgr.cpp:1200-1225`).

Each offer carries a **derived** level and spec, stable for the pair:

```text
seed   = ownerGuid ^ (botGuid * 2654435761)
offset = seed % (2 * LevelWindow + 1) - LevelWindow      # -2 .. +2
level  = clamp(ownerLevel + offset, 1, 80)
spec   = the class's PvE spec numbers, chosen by (seed >> 8) % count
```

Death knights are excluded while the player is below the heroic start level
(`CONFIG_START_HEROIC_PLAYER_LEVEL`), which is the same rule the `addclass` command applies
(`PlayerbotMgr.cpp:1206-1210`). The picker shows `Name — Class, spec name, level N`, where the spec
name is `Playerbots.PremadeSpecName.<class>.<spec_no>`.

### Recruit

1. Refuse when `Pawn.Enable = 0`, when the player is in combat, or when the pawn is already online.
2. Refuse when the party already holds `Pawn.MaxPawns` pawns (D2). The message names both exits:
   dismiss a pawn at the obelisk, or recruit strangers in the world.
3. Upsert the roster row with `spec_no`, `role`, `target_level` and `state = 1`.
4. `GET_PLAYERBOT_MGR(player)->AddPlayerBot(botGuid, player->GetSession()->GetAccountId())` —
   playerbots queues the login, the group invite and the summon
   (`PlayerbotMgr.cpp:597-621`).

### Login

`OnLogin` only *marks* a pawn pending; a `WorldScript::OnUpdate` drains the mark once
`ObjectAccessor::FindConnectedPlayer(guid)->IsInWorld()`. Two reasons: the factory wants a player in
the world, and this way the pawn's own level is applied **after** playerbots' re-level pass, which
fires on login when the pawn is more than three levels from its master
(`PlayerbotMgr.cpp:639-651`). Applied work:

1. If `bot->GetLevel() != target_level`: `PlayerbotFactory factory(bot, target_level, ...)` then
   `factory.Randomize(false)`, mirroring the existing re-level block.
2. `PlayerbotFactory::InitTalentsBySpecNo(bot, spec_no, true)` — the archetype's spec, pinned
   (`PlayerbotFactory.cpp:1802`).
3. Prune every blacklisted spell the pawn still has (`Player::removeSpell`).
4. `bot->SaveToDB(false, false)` so the next login starts from the right state.

The player's own login restores `state = 1` rows by calling `AddPlayerBot` for each, which is what
makes a party survive a relog and a restart.

### Spell gate

`OnPlayerCanLearnSpell` returns false for a blacklisted spell on a pawn. That one gate covers every
learn path — factory initialisation (`PlayerbotFactory.cpp:899-923`), trainer
(`TrainerAction.cpp:166`), level-up maintenance (`AutoMaintenanceOnLevelupAction.cpp:50-68`) and
quest rewards — so a restriction cannot be undone by any of them.

### Picker

A `GameObjectScript` on entry 5000010, the ICC teleporter pattern
(`icecrown_citadel_teleport.cpp:44-88`): `OnGossipHello` builds the page, `OnGossipSelect` routes
the click, both return `true`. Pages hold `Pawn.OfferedPerPage` rows (12) plus navigation, under the
16-row packet cap (`GossipDef.cpp:199-201`) and far under the 32-row fatal `ASSERT`
(`GossipDef.cpp:43`). Action codes: `1..99` recruit an offer, `101..199` dismiss a party pawn,
`200..` change page. The offer list is cached per player and rebuilt on every hello, so a click on a
stale menu reopens the current one instead of acting on a guid that is no longer offered.

### Loot (D4)

Config only, in the installer:

| Key | Value | Effect |
|---|---|---|
| `Playerbots.LootNeedRollLevel` | `0` | a bot's NEED becomes PASS (`LootRollAction.cpp:88`) |
| `Playerbots.LootGreedRollLevel` | `0` | a bot's GREED becomes PASS (`:93`) |
| `Playerbots.FreeMethodLoot` | `0` | bots do not loot free-for-all corpses (`LootAction.cpp:57`) |

With all three, a pawn passes on every roll under any loot method and leaves the corpse alone.

### Party cap (D2, D5)

The cap is enforced in the recruit path by counting `state = 1` rows. The command-level bypass is
closed by configuration rather than by code: the installer writes `Playerbots.AddClassCommand = 0`,
which restricts `.playerbots bot addclass` to GMs (`PlayerbotMgr.cpp:1133-1137`), leaving the
obelisk as the only doorway to a pool pawn. Strangers — random bots met in the world — are not
pool pawns and are not counted, which is exactly the intended second route to a full group.

## Wiring

| File | Change |
|---|---|
| `.github/workflows/server-aarch64.yml` | copy `modules/mod-pawn` into `core/modules/`; assert `$PREFIX/etc/modules/mod-pawn.conf.dist` after install; copy and count `data/sql` in the SQL step; name the feature in the release notes |
| `pins.toml` | `[module.pawn]` entry, in-repo module like `mod-survivor` |
| `scripts/thor-install.sh` | fetch pattern for `--from-source`, `conf_pair modules/mod-pawn`, `Pawn.*` and the loot/`AddClassCommand`/pool keys, and a log line |
| `README.md`, `docs/RUNBOOK.md`, `docs/SERVER-STACK.md` | feature, operation and module-table rows |
| `docs/PAWN-SYSTEM-DESIGN.md` | point its decisions at this plan; the patch route is superseded |

## Phases

| Phase | Work | Acceptance |
|---|---|---|
| P1 | Module skeleton: conf, loader, config struct, `PawnMgr` roster I/O and schema creation | module directory complete; loader symbol `Addmod_pawnScripts` present; the roster table is created on first boot |
| P2 | Archetypes: spec choice, name resolution, apply and prune | blacklist names resolve; an unresolved name logs an error at startup |
| P3 | Obelisk: world SQL, gossip pages, recruit and dismiss | the menu opens, pages, recruits and dismisses |
| P4 | Login queue, level window, spell gate, party restore | a recruited pawn returns after relog and after a restart |
| P5 | Installer and CI wiring, pins, docs | workflow copies and asserts the module; installer writes the keys |
| P6 | Offline checks | `scripts/pawn/check_pawn.py` passes: config keys used in code exist in the `.dist`, the obelisk entry and script name agree across conf/SQL/code, the roster schema matches the columns written, the loader symbol matches the directory name, and pins/workflow/installer mention the module |

## Verification

The compile gate is the arm64 workflow, as it is for every other module in this repository: this
host has no Boost headers, so no AzerothCore translation unit can be compiled locally
(`boost/version.hpp` is absent; the build tree at `azerothcore-wotlk/build` cannot compile). What is
verifiable offline is checked by `scripts/pawn/check_pawn.py`, and the in-game steps are:

1. Open the obelisk: the page lists pawns within ±2 levels of the player, each with class, spec and
   level; the list is stable between visits.
2. Recruit one: it appears in the party, at the level the menu showed, with the spec the menu named.
3. Recruit a second; a third recruit is refused and the message names the two exits.
4. `.playerbots bot addclass warrior` as a non-GM is refused.
5. Whisper the pawn `train`-level abilities it should not have: it never learns a blacklisted spell
   (`OnPlayerCanLearnSpell` refuses), and the spell is absent from its spellbook.
6. Kill something with a drop: both pawns pass; the corpse is the player's under free-for-all.
7. Relog, then restart the world server: both pawns return without being re-recruited.
8. Level past the window with a pawn in the party: on its next login it is re-levelled into ±2.
