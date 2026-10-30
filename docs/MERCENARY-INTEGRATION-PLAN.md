# Mercenary system: integration plan

The feasibility study is [MERCENARY-SYSTEM-DESIGN.md](MERCENARY-SYSTEM-DESIGN.md). This is the plan that
replaces its "what has to be written" section: the design decisions taken since, the route change
that removes the patch from the mercenary work, the exact components, and the order to build them in.

The five later features — the innkeeper's per-mercenary page and its orders, repair and restock,
retraining, affinity and voice lines — are specified and shipped per
[MERCENARY-EXPANSION-PLAN.md](MERCENARY-EXPANSION-PLAN.md). This document remains the record of the original
integration and of the decisions that route it.

Every claim about existing behaviour carries a `path:line` reference against
`mod-playerbots@d750b4b` and `azerothcore-wotlk@8a4e76e`.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | Mercenaries are offered at **player level ± 2** | A mercenary you cannot use at your level is noise; a mercenary that snaps to your exact level erases the choice. |
| D2 | A party holds **the player plus two mercenaries**; a third requires strangers from the world | Keeps the mercenaries special and makes the ambient crowd useful. |
| D3 | Each mercenary is a **pinned archetype**: one premade PvE spec, and nothing else restricted | The spec is what the gear pass and the role read from. The spellbook blacklist and the per-mercenary variant that were built with it are **removed** — see "The spellbook restriction" below — because at low levels they took away abilities a player expects a companion to have. |
| D4 | Mercenaries **never compete for loot**: they pass on every roll and never loot a free-for-all corpse | Loot bags are this realm's progression system. |
| D5 | The mercenary doorway is the **innkeeper line only**: `Playerbots.AddClassCommand = 0` | Makes D2 strict by construction, with no patch to the command. The pool is read directly for the same reason. |
| D6 | The mercenary layer is a **new module**, `modules/mod-mercenaries`, not an extension of the playerbots patch | See "Route" below: the two facts the study called unverified are now verified. |

## Route: a module, not a patch

The study recommended extending `patches/playerbots-player-centric.patch` because a separate module
"would have to reach those headers from a different CMake target, which is unverified". That is now
verified, and it is safe:

- Every module's `src` directory is added to one shared include list, and static modules are
  compiled into a single library: `CollectIncludeDirectories(${MODULE_SOURCE_PATH} PUBLIC_INCLUDES)`
  and `add_library(modules STATIC ... ${PRIVATE_SOURCES_MODULES})` in
  `modules/CMakeLists.txt:270-300`; the build uses `-DMODULES=static`
  (`.github/workflows/server-aarch64.yml:132`). So `#include "Bot/PlayerbotMgr.h"` from
  `modules/mod-mercenaries/src` resolves, and `-DMOD_PLAYERBOTS` reaches the same target through
  `game-interface` (`modules/CMakeLists.txt:74-80`).
- Module SQL applies automatically: `UpdateFetcher.cpp:159-186` scans
  `modules/<name>/data/sql/<dir>` for every module in the compiled `AC_MODULES_LIST`
  (`src/server/apps/worldserver/Main.cpp:447`), matching the database name inside the directory
  name. `modules/mod-survivor/data/sql/world/base/` and
  `modules/mod-creature-gear/data/sql/world/base/` are the working precedents.

Nothing in the mercenary feature needs to be *inside* a playerbots function, because four hooks already
exist:

| Need | Hook | Evidence |
|---|---|---|
| Know when a mercenary logs in | `PlayerScript::OnPlayerLogin` — bots log in through the normal path | `botSession->HandlePlayerLoginFromDB(holder)` at `PlayerbotMgr.cpp:256` → `sScriptMgr->OnPlayerLogin(pCurrChar)` at `CharacterHandler.cpp:1126` |
| Defer work until the bot is in the world | `PlayerbotWorldThreadProcessor::QueueOperation(std::unique_ptr<PlayerbotOperation>)`, a public API over an abstract `PlayerOperation::Execute()` | `PlayerbotWorldThreadProcessor.h:62`, `PlayerbotOperation.h:26-39` |
| Recruit and dismiss | `PlayerbotHolder::AddPlayerBot(guid, masterAccountId)` / `LogoutPlayerBot(guid)` | `PlayerbotMgr.cpp:88-150`, `:394` |

The player-centric patch is untouched by this work.

## Components

| File | Responsibility |
|---|---|
| `modules/mod-mercenaries/conf/mod-mercenaries.conf.dist` | `Mercenary.*` keys: master switch, cap, level window, page size, featured count and pool refresh, and one temperament pool per spec |
| `modules/mod-mercenaries/src/mercenary_common.h` | Config struct, role enum, shared types |
| `modules/mod-mercenaries/src/MercenaryArchetype.h/.cpp` | The premade spec table, the role read off a spec name, and pinning a spec's talents |
| `modules/mod-mercenaries/src/MercenaryMgr.h/.cpp` | Config load, pool refresh, featured draw, roster I/O, offer derivation, recruit/dismiss, login queue |
| `modules/mod-mercenaries/src/MercenaryInnkeeper.cpp` | the two `AllCreatureScript` gossip hooks, with the paged gossip picker on every innkeeper |
| `modules/mod-mercenaries/src/MercenaryScripts.cpp` | `PlayerScript` login queue; `WorldScript` that drains the queue |
| `modules/mod-mercenaries/src/mercenary_loader.cpp` | `Addmod_mercenariesScripts()`, the symbol the loader generator looks up |
| `modules/mod-mercenaries/README.md` | What it does, how to configure it |

## Data model

Characters database, because every row belongs to a character pair and joins to `characters`. The
table is created **programmatically** (`MercenaryMgr::EnsureSchema`, `CREATE TABLE IF NOT EXISTS`), not by
module SQL: a failing module SQL file aborts the worldserver boot on this fork
(`docs/SERVER-STACK.md` §13, which is why mod-survivor creates its three character tables the same
way).

```sql
CREATE TABLE IF NOT EXISTS `mercenary_roster` (
  `owner_guid`   INT UNSIGNED NOT NULL,          -- the real player's character
  `bot_guid`     INT UNSIGNED NOT NULL,          -- the mercenary
  `spec_no`      TINYINT UNSIGNED NOT NULL,      -- Playerbots.PremadeSpecLink.<class>.<spec_no>
  `role`         TINYINT UNSIGNED NOT NULL,      -- 1 tank, 2 healer, 3 dps
  `state`        TINYINT UNSIGNED NOT NULL,      -- 1 in the party, 0 dismissed
  `target_level` TINYINT UNSIGNED NOT NULL,      -- the level the mercenary was recruited at
  `built`        TINYINT UNSIGNED NOT NULL,      -- 1 once the gear pass has run
  `paid`         TINYINT UNSIGNED NOT NULL,      -- 1 once this owner has paid the hire fee
  `updated_at`   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`owner_guid`, `bot_guid`),
  KEY `bot_guid` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

A dismissed row is kept (`state = 0`) so the archetype, the level and the paid fee survive and
re-recruiting the same mercenary gives the same mercenary for nothing. `state = 1` rows are the party: they are
what the cap counts and what the login hook restores. `built` and `paid` are added to a table an
earlier build made through an `information_schema` lookup plus `ALTER`, because
`ADD COLUMN IF NOT EXISTS` is MariaDB-only.

## Config

```ini
Mercenary.Enable = 1
Mercenary.MaxMercenaries = 2                  # D2
Mercenary.HireFeePercent = 100          # level^3 copper
Mercenary.HireFeeHeldPercent = 100      # each mercenary held adds the base fee again
Mercenary.GearFloorPercent = 85         # the per-mercenary gear ratio's band, and the price band
Mercenary.GearCapPercent = 150
Mercenary.LevelWindow = 2               # D1, in levels either way
Mercenary.GainXP = 0                    # a mercenary is pinned to the level it was hired at
Mercenary.OfferedPerPage = 12           # gossip carries at most 16 rows per page
Mercenary.FeaturedMin = 10              # per selection, one per pool cycle
Mercenary.FeaturedMax = 15
Mercenary.PoolRefreshMinMinutes = 40    # pool re-read, random inside the range
Mercenary.PoolRefreshMaxMinutes = 60
Mercenary.Debug = 0

# Temperaments: how a mercenary of this spec fights, by playerbots strategy name. <class> is the
# ChrClasses id (1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 6 death knight, 7 shaman,
# 8 mage, 9 warlock, 11 druid); <spec_no> is the Playerbots.PremadeSpecLink number.
Mercenary.Temperament.1.0 = Relentless: +aggressive,+close | Patient: +wait for attack | Wary: +avoid aoe
```

### The spellbook restriction

The first build also carried `Mercenary.Blacklist.<class>.<spec>` — the other specs' signature
abilities, refused at `Player::learnSpell` and pruned from the book on every build — and
`Mercenary.Variant.<class>.<spec>`, a pool from which each mercenary was dealt a subset it was
missing. Both were removed. At a low level the restriction took away abilities a player expects a
companion to have (a level-10 hunter missing Concussive Shot), so a mercenary read as broken rather
than as different, and the spec alone is what the module pins now. The names resolved against the
spell table once at startup, which is why the check script still looks for an `archetypes:` line
with no unresolved names. Varying a toolkit without taking abilities away is deferred.

## Behaviour

### Offers

`RandomPlayerbotMgr::addclassCache` (`RandomPlayerbotMgr.h:172`) already holds the type-2 pool
per team and class, keyed by `GetTeamClassIdx(isAlliance, claz)` (`:168`). For a player, the offers
are that pool filtered to mercenaries that are offline, not in a real guild, and not already in this
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

1. Refuse when `Mercenary.Enable = 0`, when the player is in combat, or when the mercenary is already online.
2. Refuse when the party already holds `Mercenary.MaxMercenaries` mercenaries (D2). The message names both exits:
   dismiss a mercenary at the innkeeper, or recruit strangers in the world.
3. Refuse when the hire fee is unaffordable. The fee is
   `player_level^3 * Mercenary.HireFeePercent / 100 * gear_ratio / 100 *
   (100 + Mercenary.HireFeeHeldPercent * mercenaries_held) / 100` copper, where `gear_ratio` is the mercenary's
   strength in percent of the owner's gear score, and it is 0 for a mercenary this owner has already paid
   for. The player's level, not the mercenary's, is the baseline: a mercenary geared like the player costs
   `player_level^3`, and the gear ratio moves it in a band that does not widen as the player levels. Level cubed is calibrated against the loot-bag money table: 10-20 minutes of
   ordinary farming at every level that matters. It is charged *here*, after every check, and not
   through the gossip box: the core deducts box money after the script hook and unconditionally, so
   a recruit refused at step 2 would still be paid for and an empty purse would still hire.
4. Upsert the roster row with `spec_no`, `role`, `target_level`, `paid` and `state = 1`.
5. `GET_PLAYERBOT_MGR(player)->AddPlayerBot(botGuid, player->GetSession()->GetAccountId())` —
   playerbots queues the login, the group invite and the summon
   (`PlayerbotMgr.cpp:597-621`).

### Login

`OnPlayerLogin` only *marks* a mercenary pending; a `WorldScript::OnUpdate` drains the mark once
`ObjectAccessor::FindConnectedPlayer(guid)->IsInWorld()`. Two reasons: the factory wants a player in
the world, and this way the mercenary's own level is applied **after** playerbots' re-level pass, which
fires on login when the mercenary is more than three levels from its master
(`PlayerbotMgr.cpp:639-651`). Applied work:

1. If the mercenary's level is not the one it was hired at (`mercenary_roster.target_level`), rebuild it
   there: `PlayerbotFactory factory(bot, target, ...)` then `factory.Randomize(false)`. This is what
   undoes playerbots' login pass, which rebuilds a type-2 bot at its master's exact level when the
   gap is more than three — a mercenary does not follow its owner up. It also sets
   `PLAYER_FLAGS_NO_XP_GAIN` unless `Mercenary.GainXP`, so the level cannot drift between logins.
2. `PlayerbotFactory::InitTalentsBySpecNo(bot, spec_no, true)` — the archetype's spec, pinned
   (`PlayerbotFactory.cpp:1802`).
3. `bot->SaveToDB(false, false)` so the next login starts from the right state.

The player's own login restores `state = 1` rows by calling `AddPlayerBot` for each, which is what
makes a party survive a relog and a restart.

### Gear

A mercenary has to arrive dressed for the spec it was pinned to, at its own level — not in the gear the
factory rolled for a spec it no longer has. The order in `MercenaryMgr::ApplyMercenary` is therefore:

1. `PlayerbotFactory::Randomize` — on the first build and on a re-level (level, spells, talents,
   gear for whatever spec it happened to roll, bags, food, reagents);
2. `MercenaryArchetype::Apply` — the pinned spec's talents;
3. `PlayerbotAI::ResetStrategies(false)` — the weight calculator's role test (`IsTank`/`IsHeal`/
   `IsCaster` with `bySpec=false`) reads `STRATEGY_TYPE_*` on the AI's engines, not the talent tab,
   and those strategies still describe the spec step 1 rolled. This is the same call `Randomize`
   makes after it changes talents;
4. `InitGlyphs(false)` — `Randomize` set glyphs from the rolled spec's tab, and `InitGlyphs` reads
   the tab, so it runs again against the pinned talents (`ChangeTalentsAction::SpecPick` does the
   same);
5. `DestroyEquippedGear`, because step 1's items were chosen for the other spec;
6. `InitEquipment` at the mercenary's level, then ammo, enchants/gems and a durability repair.

Step 6 is `PlayerbotFactory::AutoGear`'s own sequence with one difference: the gear-score cap comes
from the **owner** — `GetMixedGearScore(owner, withBags = true, topN = 12)`, the average score of the
owner's twelve best items — times the mercenary's own **gear ratio**, instead of an item level. The ratio
is drawn per mercenary from the same seed as the level and the spec, triangular with its peak at 100 over
`[Mercenary.GearFloorPercent, Mercenary.GearCapPercent]`, so one mercenary offered at a level arrives in better gear
than another and the hire fee says so. With the defaults (85, 150) a mercenary can be a seventh weaker than
its owner or half again as strong. Item choice is spec-aware
because `InitEquipment` scores every candidate with `StatsWeightCalculator`, whose collector type
comes from the AI's strategies (`TankWarriorStrategy` declares `STRATEGY_TYPE_TANK`, and
`AddDefaultCombatStrategies` picks it from the talent tab) and whose tab comes from
`GetPlayerSpecTab`. Steps 3 and 4 are what make those agree with the pinned spec.

That pass runs **once**: the roster's `built` column records it, so afterwards the mercenary keeps
whatever it wears, including gear the player gives it. A mercenary is pinned to the level it was hired at,
so it does not level in the party and there is no maintenance pass to follow it up. The pass runs
again only when the pin has to be restored — playerbots rebuilt the bot at its master's level on
login, or the mercenary was re-hired at a new offer level — and then it re-gears at that level with the
owner's gear score as the ceiling. A dismissed mercenary keeps its `built` flag, so re-recruiting it at
the same level does not strip the gear it earned — unless the recruit names a different spec or a
new level, in which case the gear on it was scored for the old build and the pass runs again.

### Picker

Two `AllCreatureScript` gossip hooks, `CanCreatureGossipHello` and `CanCreatureGossipSelect`, not a
gameobject: both run for every creature and gate themselves on `UNIT_NPC_FLAG_INNKEEPER`, so the core
keeps whatever AI an innkeeper already had and still gets the line. A `CreatureAI` handed out by
`GetCreatureAI` was the first shape and could not do it: that hook has to leave a creature's own AI
alone, so it had to skip every innkeeper whose `AIName` is set, and 65 of the 125 innkeeper-flagged
templates are `SmartAI` — Razor Hill's Innkeeper Grosk (6928) among them, which is where the line was
first reported missing. The hello hook builds the menu the core would have built — `PrepareGossipMenu`
with the creature's own menu id, so the bind option and any quest rows survive — appends one line and
sends it with `Player::SendPreparedGossip`; returning true is what suppresses the core's own build and
send (`NPCHandler.cpp:188-193`). That is why the module ships no SQL at all: no template row, no
spawn, no `ScriptName`, and the line exists wherever an innkeeper does. `CanCreatureGossipSelect` is
handed the resolved action (`MiscHandler.cpp:200`) and returns false for anything outside the module's
range, which is how the innkeeper's own options keep working (`MiscHandler.cpp:201`).

A named `CreatureScript` cannot serve this, though it carries the same two hooks: it is database-bound
(`CreatureScript.h:30`), so `ScriptMgr` looks its name up in `script_name` and registers nothing when
there is no row (`ScriptMgr.h:817-885`), and even with a row gossip is dispatched by the creature's
own script id (`CreatureScript.cpp:24-41`) — an innkeeper's is the core's `npc_innkeeper`.

Action codes start at `1000`: a database gossip row's action *is* its `OptionType`, and
`GOSSIP_OPTION_MAX` is 21 (`GossipDef.h:33-57`), so the `1..99` range this plan first sketched would
have swallowed the bind option. `1000` opens the picker, `1010..1099` recruit an offer,
`1110..1199` dismiss a party mercenary, `1200`/`1201` change page. Pages hold `Mercenary.OfferedPerPage` rows
(12) plus navigation, under the 16-row packet cap (`GossipDef.cpp:199-201`) and far under the
32-row fatal `ASSERT` (`GossipDef.cpp:43`). The selection is cached per player for the life of one
pool cycle and the rows shown are rebuilt from it, so a click on a stale menu reopens the picker
instead of acting on a guid that is no longer offered.

### Pool and featured draw

The pool is the type-2 "AddClass" accounts in the **playerbots** database
(`playerbots_account_type`, `account_type = 2`), joined to `characters` for guid, name, class, race
and level, offline only. It is read at startup, on a random 40-60 minute timer, and on demand when
it is still empty — a boot can finish before playerbots has created the pool characters. The module
does **not** read `sRandomPlayerbotMgr.addclassCache`: that cache is only filled when
`Playerbots.AddClassCommand` is on, and the installer turns it off (D5).

A player's **selection** is a random count in `[Mercenary.FeaturedMin, Mercenary.FeaturedMax]` of candidates,
dealt off a per-player **deck** rather than sampled: the eligible pool is shuffled with Fisher-Yates
(SplitMix64 seeded from the player and a per-draw counter, so the global RNG is untouched), the next
rows are handed out, and the cursor carries across cycles until the deck is exhausted — then it is
reshuffled, with the previous selection's faces dealt last so the reshuffle cannot put a repeat in
the very next selection, and with a selection never straddling a reshuffle (the skipped tail goes
into the next shuffle). No face repeats until the whole deck has been dealt, and a pool refresh
reshuffles every deck.

The selection is dealt once per pool cycle and **not** per visit: the first open after a refresh (or
a restart) deals it, and every open until the next refresh shows the same mercenaries at the same spec, so
the picker cannot be rerolled by walking out of the inn and back in. Recruiting a mercenary takes it out
of the selection; dismissing it does not put it back; a selection with nothing eligible left on it
(all recruited, logged in or dropped from the pool) is dealt again, because a menu with no rows
cannot be browsed.

Same team only, the same death-knight level gate the addclass command applies, and no candidate that
is already in this player's party or online; a dealt counter that stopped being a candidate is
skipped when it comes up. The rows are rebuilt from the selection on every open, so level, gear ratio
and hire fee follow the player's own level; spec stays a pure function of the two guids, so the same
face keeps the same spec however often it comes round. The installer widens the pool to 15 accounts
(about 150 characters, roughly 75 per team): 12 dealt from 75 is about six cycles before a repeat,
where 12 sampled from 75 repeats two faces on every visit.

### Loot (D4)

Config only, in the installer:

| Key | Value | Effect |
|---|---|---|
| `Playerbots.LootNeedRollLevel` | `0` | a bot's NEED becomes PASS (`LootRollAction.cpp:88`) |
| `Playerbots.LootGreedRollLevel` | `0` | a bot's GREED becomes PASS (`:93`) |
| `Playerbots.FreeMethodLoot` | `0` | bots do not loot free-for-all corpses (`LootAction.cpp:57`) |

With all three, a mercenary passes on every roll under any loot method and leaves the corpse alone.

### Party cap (D2, D5)

The cap is enforced in the recruit path by counting `state = 1` rows. The command-level bypass is
closed by configuration rather than by code: the installer writes `Playerbots.AddClassCommand = 0`,
which restricts `.playerbots bot addclass` to GMs (`PlayerbotMgr.cpp:1133-1137`), leaving the
innkeeper picker as the only doorway to a pool mercenary. Strangers — random bots met in the world — are not
pool mercenaries and are not counted, which is exactly the intended second route to a full group.

## Wiring

| File | Change |
|---|---|
| `.github/workflows/server-aarch64.yml` | copy `modules/mod-mercenaries` into `core/modules/`; run `scripts/mercenaries/check_mercenaries.py` before the build; assert `$PREFIX/etc/modules/mod-mercenaries.conf.dist`, its blacklist and its pool keys after install; name the feature in the release notes |
| `pins.toml` | `[module.mercenary]` entry, in-repo module like `mod-survivor` |
| `scripts/thor-install.sh` | fetch pattern for `--from-source`, `conf_pair modules/mod-mercenaries`, `Mercenary.*` and the loot/`AddClassCommand`/pool-size keys, a stale `Mercenary.ObeliskEntry` drop on repair, and a log line |
| `README.md`, `docs/RUNBOOK.md`, `docs/SERVER-STACK.md` | feature, operation and module-table rows |
| `docs/MERCENARY-SYSTEM-DESIGN.md` | point its decisions at this plan; the patch route is superseded |

## Phases

| Phase | Work | Acceptance |
|---|---|---|
| P1 | Module skeleton: conf, loader, config struct, `MercenaryMgr` roster I/O and schema creation | module directory complete; loader symbol `Addmod_mercenariesScripts` present; the roster table is created on first boot |
| P2 | Archetypes: spec choice and pinning | the spec table resolves and every PvE spec the picker may offer has a name at startup |
| P3 | Picker: the innkeeper line, gossip pages, recruit and dismiss | the line appears on an innkeeper beside the bind option, pages, recruits and dismisses |
| P4 | Login queue, level window, party restore | a recruited mercenary returns after relog and after a restart |
| P5 | Installer and CI wiring, pins, docs | workflow copies and asserts the module; installer writes the keys |
| P6 | Offline checks | `scripts/mercenaries/check_mercenaries.py` passes: config keys used in code exist in the `.dist`, the installer's defaults and the conf agree both ways, the picker is an `AllCreatureScript` answering the two gossip hooks, gated on the innkeeper flag, with actions clear of the core's range and no `GetCreatureAI` left in it, no module class derives from a database-bound script type, the spec and temperament tables are built in `OnStartup` and only rebuilt under `if (reload)`, the temperament keys are enumerated rather than probed, the pool reads the playerbots account table, the roster schema matches the columns written, the loader symbol matches the directory name, and pins/workflow/installer mention the module |

## Verification

The compile gate is the arm64 workflow, as it is for every other module in this repository, and
`scripts/syntax-check.sh` gives the same answer locally from the recorded flags. What is verifiable
offline is checked by `scripts/mercenaries/check_mercenaries.py`, which the workflow runs before it
builds - the roster schema, the conf, the installer's defaults and the picker's own invariants are
cross-checked before the 40-minute build rather than after it. The in-game steps are:

1. Talk to an innkeeper: the line sits beside "make this inn your home" and the bind option still
   works. The page lists 10-15 mercenaries within ±2 levels of the player, each with class, spec and
   level; closing and reopening shows the same mercenaries, and the same face keeps its level and spec.
2. Recruit one: it appears in the party, at the level the menu showed, with the spec the menu named.
3. Recruit a second; a third recruit is refused and the message names the two exits.
4. `.playerbots bot addclass warrior` as a non-GM is refused.
5. Whisper the mercenary a `train`-level ability it should not have and check it is refused by the
   core's own level gate: the module no longer refuses anything, so its spellbook is exactly what its
   class and level give it.
6. Kill something with a drop: both mercenaries pass; the corpse is the player's under free-for-all.
7. Relog, then restart the world server: both mercenaries return without being re-recruited.
8. Level past the window with a mercenary in the party: it stays at its hired level, the inn marks it
   `fallen behind`, and hiring it again at the new offer level charges the fee once.
