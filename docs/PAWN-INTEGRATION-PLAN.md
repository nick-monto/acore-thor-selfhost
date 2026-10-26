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
| D5 | The pawn doorway is the **innkeeper line only**: `Playerbots.AddClassCommand = 0` | Makes D2 strict by construction, with no patch to the command. The pool is read directly for the same reason. |
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
| `modules/mod-pawn/conf/mod-pawn.conf.dist` | `Pawn.*` keys: master switch, cap, level window, page size, featured count and pool refresh, per-archetype blacklists |
| `modules/mod-pawn/src/pawn_common.h` | Config struct, role enum, shared types |
| `modules/mod-pawn/src/PawnArchetype.h/.cpp` | Spec choice, blacklist name→id resolution, apply and prune |
| `modules/mod-pawn/src/PawnMgr.h/.cpp` | Config load, pool refresh, featured draw, roster I/O, offer derivation, recruit/dismiss, login queue |
| `modules/mod-pawn/src/PawnInnkeeper.cpp` | `CreatureScript` with the paged gossip picker on every innkeeper |
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
  `built`        TINYINT UNSIGNED NOT NULL,      -- 1 once the gear pass has run
  `paid`         TINYINT UNSIGNED NOT NULL,      -- 1 once this owner has paid the hire fee
  `updated_at`   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`owner_guid`, `bot_guid`),
  KEY `bot_guid` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

A dismissed row is kept (`state = 0`) so the archetype, the level and the paid fee survive and
re-recruiting the same pawn gives the same pawn for nothing. `state = 1` rows are the party: they are
what the cap counts and what the login hook restores. `built` and `paid` are added to a table an
earlier build made through an `information_schema` lookup plus `ALTER`, because
`ADD COLUMN IF NOT EXISTS` is MariaDB-only.

## Config

```ini
Pawn.Enable = 1
Pawn.MaxPawns = 2                  # D2
Pawn.HireFeePercent = 100          # level^3 copper
Pawn.HireFeeHeldPercent = 100      # each pawn held adds the base fee again
Pawn.GearFloorPercent = 85         # the per-pawn gear ratio's band, and the price band
Pawn.GearCapPercent = 150
Pawn.LevelWindow = 2               # D1, in levels either way
Pawn.OfferedPerPage = 12           # gossip carries at most 16 rows per page
Pawn.FeaturedMin = 10              # per picker instance, drawn from the pool
Pawn.FeaturedMax = 15
Pawn.PoolRefreshMinMinutes = 40    # pool re-read, random inside the range
Pawn.PoolRefreshMaxMinutes = 60
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
   dismiss a pawn at the innkeeper, or recruit strangers in the world.
3. Refuse when the hire fee is unaffordable. The fee is
   `player_level^3 * Pawn.HireFeePercent / 100 * gear_ratio / 100 *
   (100 + Pawn.HireFeeHeldPercent * pawns_held) / 100` copper, where `gear_ratio` is the pawn's
   strength in percent of the owner's gear score, and it is 0 for a pawn this owner has already paid
   for. The player's level, not the pawn's, is the baseline: a pawn geared like the player costs
   `player_level^3`, and the gear ratio moves it in a band that does not widen as the player levels. Level cubed is calibrated against the loot-bag money table: 10-20 minutes of
   ordinary farming at every level that matters. It is charged *here*, after every check, and not
   through the gossip box: the core deducts box money after the script hook and unconditionally, so
   a recruit refused at step 2 would still be paid for and an empty purse would still hire.
4. Upsert the roster row with `spec_no`, `role`, `target_level`, `paid` and `state = 1`.
5. `GET_PLAYERBOT_MGR(player)->AddPlayerBot(botGuid, player->GetSession()->GetAccountId())` —
   playerbots queues the login, the group invite and the summon
   (`PlayerbotMgr.cpp:597-621`).

### Login

`OnLogin` only *marks* a pawn pending; a `WorldScript::OnUpdate` drains the mark once
`ObjectAccessor::FindConnectedPlayer(guid)->IsInWorld()`. Two reasons: the factory wants a player in
the world, and this way the pawn's own level is applied **after** playerbots' re-level pass, which
fires on login when the pawn is more than three levels from its master
(`PlayerbotMgr.cpp:639-651`). Applied work:

1. If the pawn is **outside** the window (`|bot level - owner level| > Pawn.LevelWindow`), re-level
   it to the level it would be offered at: `PlayerbotFactory factory(bot, wanted, ...)` then
   `factory.Randomize(false)`, mirroring the existing re-level block. A pawn already inside the
   window is left alone — the invariant is the window, not one exact level — and the roster then
   records the level the pawn actually has.
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

### Gear

A pawn has to arrive dressed for the spec it was pinned to, at its own level — not in the gear the
factory rolled for a spec it no longer has. The order in `PawnMgr::ApplyPawn` is therefore:

1. `PlayerbotFactory::Randomize` — on the first build and on a re-level (level, spells, talents,
   gear for whatever spec it happened to roll, bags, food, reagents);
2. `PawnArchetype::Apply` — the pinned spec's talents, and the blacklist;
3. `PlayerbotAI::ResetStrategies(false)` — the weight calculator's role test (`IsTank`/`IsHeal`/
   `IsCaster` with `bySpec=false`) reads `STRATEGY_TYPE_*` on the AI's engines, not the talent tab,
   and those strategies still describe the spec step 1 rolled. This is the same call `Randomize`
   makes after it changes talents;
4. `InitGlyphs(false)` — `Randomize` set glyphs from the rolled spec's tab, and `InitGlyphs` reads
   the tab, so it runs again against the pinned talents (`ChangeTalentsAction::SpecPick` does the
   same);
5. `DestroyEquippedGear`, because step 1's items were chosen for the other spec;
6. `InitEquipment` at the pawn's level, then ammo, enchants/gems and a durability repair.

Step 6 is `PlayerbotFactory::AutoGear`'s own sequence with one difference: the gear-score cap comes
from the **owner** — `GetMixedGearScore(owner, withBags = true, topN = 12)`, the average score of the
owner's twelve best items — times the pawn's own **gear ratio**, instead of an item level. The ratio
is drawn per pawn from the same seed as the level and the spec, triangular with its peak at 100 over
`[Pawn.GearFloorPercent, Pawn.GearCapPercent]`, so one pawn offered at a level arrives in better gear
than another and the hire fee says so. With the defaults (85, 150) a pawn can be a seventh weaker than
its owner or half again as strong. Item choice is spec-aware
because `InitEquipment` scores every candidate with `StatsWeightCalculator`, whose collector type
comes from the AI's strategies (`TankWarriorStrategy` declares `STRATEGY_TYPE_TANK`, and
`AddDefaultCombatStrategies` picks it from the talent tab) and whose tab comes from
`GetPlayerSpecTab`. Steps 3 and 4 are what make those agree with the pinned spec.

That pass runs **once**: the roster's `built` column records it, so afterwards the pawn keeps
whatever it wears, including gear the player gives it, and playerbots' own level-up maintenance
(`AutoMaintenanceOnLevelupAction` → `InitEquipment(true)`, also spec-weighted) keeps it current as
it levels in the party. A pawn that is re-levelled because it fell outside the window is rebuilt and
re-geared, which is the price of pulling it back into the window. A dismissed pawn keeps its `built`
flag, so re-recruiting it does not strip the gear it earned — unless the recruit names a different
spec, in which case the gear on it was scored for the old one and the pass runs again.

### Picker

A `CreatureScript`, not a gameobject: `OnGossipHello` returns `false` for every creature except an
innkeeper, so the core sends whatever that NPC already has, and for an innkeeper it builds the
creature's own menu with `Player::PrepareGossipMenu` — the bind option and any quest rows survive —
appends one line and sends it with `Player::SendPreparedGossip`. That is why the module ships no SQL
at all: no template row, no spawn, no `ScriptName`, and the line exists wherever an innkeeper does.
`OnGossipSelect` returns `false` outside the module's action range, which is how the innkeeper's own
options keep working (`MiscHandler.cpp:201-203`).

Action codes start at `1000`: a database gossip row's action *is* its `OptionType`, and
`GOSSIP_OPTION_MAX` is 21 (`GossipDef.h:33-57`), so the `1..99` range this plan first sketched would
have swallowed the bind option. `1000` opens the picker, `1010..1099` recruit an offer,
`1110..1199` dismiss a party pawn, `1200`/`1201` change page. Pages hold `Pawn.OfferedPerPage` rows
(12) plus navigation, under the 16-row packet cap (`GossipDef.cpp:199-201`) and far under the
32-row fatal `ASSERT` (`GossipDef.cpp:43`). The draw is cached per player for the life of one menu
instance, so a click on a stale menu reopens the picker instead of acting on a guid that is no longer
offered.

### Pool and featured draw

The pool is the type-2 "AddClass" accounts in the **playerbots** database
(`playerbots_account_type`, `account_type = 2`), joined to `characters` for guid, name, class, race
and level, offline only. It is read at startup, on a random 40-60 minute timer, and on demand when
it is still empty — a boot can finish before playerbots has created the pool characters. The module
does **not** read `sRandomPlayerbotMgr.addclassCache`: that cache is only filled when
`Playerbots.AddClassCommand` is on, and the installer turns it off (D5).

Each picker instance draws a random count in `[Pawn.FeaturedMin, Pawn.FeaturedMax]` of candidates
with a seeded Fisher-Yates shuffle (SplitMix64 seeded from the player and a per-draw counter, so the
global RNG is untouched). Same team only, the same death-knight level gate the addclass command
applies, and no candidate that is already in this player's party or online. The draw is kept for the
menu instance so paging and dismissing do not move rows; recruiting removes that row; reopening draws
again. A pawn's level and spec stay a pure function of the two guids, so the same face keeps the same
level and spec however often it is drawn. The installer widens the pool to 15 accounts (about 150
characters) precisely because a 10-15 draw from 50 would repeat too often.

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
innkeeper picker as the only doorway to a pool pawn. Strangers — random bots met in the world — are not
pool pawns and are not counted, which is exactly the intended second route to a full group.

## Wiring

| File | Change |
|---|---|
| `.github/workflows/server-aarch64.yml` | copy `modules/mod-pawn` into `core/modules/`; run `scripts/pawn/check_pawn.py` before the build; assert `$PREFIX/etc/modules/mod-pawn.conf.dist`, its blacklist and its pool keys after install; name the feature in the release notes |
| `pins.toml` | `[module.pawn]` entry, in-repo module like `mod-survivor` |
| `scripts/thor-install.sh` | fetch pattern for `--from-source`, `conf_pair modules/mod-pawn`, `Pawn.*` and the loot/`AddClassCommand`/pool-size keys, a stale `Pawn.ObeliskEntry` drop on repair, and a log line |
| `README.md`, `docs/RUNBOOK.md`, `docs/SERVER-STACK.md` | feature, operation and module-table rows |
| `docs/PAWN-SYSTEM-DESIGN.md` | point its decisions at this plan; the patch route is superseded |

## Phases

| Phase | Work | Acceptance |
|---|---|---|
| P1 | Module skeleton: conf, loader, config struct, `PawnMgr` roster I/O and schema creation | module directory complete; loader symbol `Addmod_pawnScripts` present; the roster table is created on first boot |
| P2 | Archetypes: spec choice, name resolution, apply and prune | blacklist names resolve; an unresolved name logs an error at startup |
| P3 | Picker: the innkeeper line, gossip pages, recruit and dismiss | the line appears on an innkeeper beside the bind option, pages, recruits and dismisses |
| P4 | Login queue, level window, spell gate, party restore | a recruited pawn returns after relog and after a restart |
| P5 | Installer and CI wiring, pins, docs | workflow copies and asserts the module; installer writes the keys |
| P6 | Offline checks | `scripts/pawn/check_pawn.py` passes: config keys used in code exist in the `.dist`, the installer's defaults and the conf agree both ways, the picker is a `CreatureScript` gated on the innkeeper flag with actions clear of the core's range, the pool reads the playerbots account table, the roster schema matches the columns written, the loader symbol matches the directory name, and pins/workflow/installer mention the module |

## Verification

The compile gate is the arm64 workflow, as it is for every other module in this repository: this
host has no Boost headers, so no AzerothCore translation unit can be compiled locally
(`boost/version.hpp` is absent; the build tree at `azerothcore-wotlk/build` cannot compile). What is
verifiable offline is checked by `scripts/pawn/check_pawn.py`, which the workflow runs before it
builds - the roster schema, the conf, the installer's defaults and the picker's own invariants are
cross-checked before the 40-minute build rather than after it. The in-game steps are:

1. Talk to an innkeeper: the line sits beside "make this inn your home" and the bind option still
   works. The page lists 10-15 pawns within ±2 levels of the player, each with class, spec and
   level; closing and reopening shows a different subset, and the same face keeps its level and spec.
2. Recruit one: it appears in the party, at the level the menu showed, with the spec the menu named.
3. Recruit a second; a third recruit is refused and the message names the two exits.
4. `.playerbots bot addclass warrior` as a non-GM is refused.
5. Whisper the pawn `train`-level abilities it should not have: it never learns a blacklisted spell
   (`OnPlayerCanLearnSpell` refuses), and the spell is absent from its spellbook.
6. Kill something with a drop: both pawns pass; the corpse is the player's under free-for-all.
7. Relog, then restart the world server: both pawns return without being re-recruited.
8. Level past the window with a pawn in the party: on its next login it is re-levelled into ±2.
