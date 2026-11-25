# mod-faction-invasion

A random open-world event: a themed faction invades the zone a real player is already in. The wave
is 15-18 mobs at that player's level +/-2, with at most three elites. Killing the whole wave summons
one owner-only reward chest; the chest is one tier above the mob loot and its coin scales with the
elite count. A short-lived faction ground effect marks the camp, and the bundled `Invasion` add-on
draws the area on the zone's own world map and an edge arrow on the minimap.

## How it runs

1. `WorldScript::OnUpdate` checks the odds every `CheckIntervalSec`, once the cooldown has expired.
   It chooses a real (non-headless) player whose zone has an anchor and queues a `PendingStart`.
   It does **not** touch a `Map` from the world thread.
2. `AllMapScript::OnMapUpdate` for that player's map consumes the pending start. It picks one of
   the ten themes that has a creature for the target level, then spawns the wave in a ring around
   the anchor. The timeout and the GM stop use the same map-thread hand-off.
3. Each mob is a stock rank-0 entry (normal slot) or a generated rank-1 clone at `base + 700000`
   (elite slot). `CreatureStatScale` forces the target level and applies the elite/theme
   multipliers **inside** `Creature::SelectLevel`, so hit tables, avoidance, armour DR, spell
   damage and healing all agree with the displayed level. The entry never changes, so the client's
   dragon frame is correct and loot/spell rows keep resolving.
4. The ground carrier is the stock Invisible Stalker (entry 15214), set hostile, rooted and
   non-attackable, casting a stock persistent-area spell (Blight, Consecrated, Blizzard, ...).
5. `OnAfterLootTemplateProcess` replaces the stock mob loot with the generated bracket/difficulty
   pool. `OnUnitDeath` writes the mob's coin after the core's `generateMoneyLoot` and records the
   killing blow. The first real player to empty the wave wins; ties go to the earlier last-kill
   time, then the lower GUID.
6. The chest uses `gameobject_template` entry 5000100: type 3, `lootId=0` (the hand-filled loot is
   preserved), `consumable=1`, zero add-on gold. `OnAllowedToLootContainerCheck` is the inverted
   gate: the winner may open it, everyone else is denied. If the winner is offline past
   `WinnerGraceMinutes`, the chest opens to everyone. The chest row persists in
   `faction_invasion_chest` (characters DB); a later zone update re-summons it.
7. The `INVS` add-on line carries the event's `WorldMapArea` bounds, so the client needs no DBC
   table: the circle uses `u=(left-y)/(left-right)`, `v=(top-x)/(top-bottom)`, and the minimap
   arrow inverts the same transform.

## The core patch

`patches/invasion-stat-scale.patch` applies to the pinned core. It adds:

- `CreatureStatScale` (`health`, `mana`, `damage`, `armor`, `spellDamage`, `healing`, `level`) and
  `Creature::GetStatScale()` in `Creature.h`.
- `OnCreatureCalculateStatScale` (`AllCreatureScript`) dispatched from
  `Creature::SelectLevel`, consumed at health, mana, base damage, attack power, armour,
  `GetSpellDamageMod` and `SpellHealingBonusDone`.
- `GetZoneMapBounds(zoneId, left, right, top, bottom)` so the add-on gets the four world bounds
  without shipping `WorldMapArea.dbc`.

The module will not compile against the bare core pin. The three server workflows and
`module-checks.yml` apply the patch before configure/compile; `thor-install.sh --from-source`
applies it with `apply_core_patch`.

## Data

| File | Content |
|---|---|
| `faction_invasion_10_schema.sql` | the seven world tables, `CREATE TABLE IF NOT EXISTS` |
| `faction_invasion_20_themes.sql` | the ten themes and their stat modifier + marker aura |
| `faction_invasion_30_creatures.sql` | theme x level band x base/elite entry |
| `faction_invasion_40_areas.sql` | 20 curated outdoor anchors |
| `faction_invasion_50_ground.sql` | the stock ground spell per theme |
| `faction_invasion_60_loot.sql` | item pools, roll plans and money |
| `faction_invasion_70_elite_clones.sql` | 267 rank-1 clones (`base + 700000`) and their model/spell/equip/addon/movement/resistance rows |
| `faction_invasion_80_objects.sql` | the reward chest template |

The generated files are written by:

```sh
python3 scripts/faction-invasion/gen_invasion.py --core <pinned core>
python3 scripts/faction-invasion/gen_invasion.py --core <pinned core> --check
```

The generator reads the pinned `creature_template`, `factiontemplate_dbc`,
`creature_loot_template`, `creature_template_model` and the loot-bag item pools. It selects
hostile-to-both (`FactionGroup = 8`), rank-0, lootable entries by faction name and creature-name
regex, six per band, and clones each one. `--check` is part of `module-checks.yml`.

## Configuration

`conf/mod-faction-invasion.conf.dist` — the live file is `etc/modules/mod-faction-invasion.conf`:

| Key | Default | Meaning |
|---|---:|---|
| `FactionInvasion.Enable` | 1 | master switch |
| `FactionInvasion.CheckIntervalSec` | 60 | how often the odds are checked |
| `FactionInvasion.MinPlayers` | 1 | minimum real players in a supported zone for a random start |
| `FactionInvasion.ChancePercent` | 5 | chance per check; 0 keeps the GM command only |
| `FactionInvasion.CooldownMinutes` | 15 | gap between events |
| `FactionInvasion.DurationMinutes` | 10 | wave lifetime before it times out |
| `FactionInvasion.ChestEntry` | 5000100 | reward chest template |
| `FactionInvasion.GroundCarrierEntry` | 15214 | Invisible Stalker carrier |
| `FactionInvasion.ChestLifetimeMinutes` | 30 | unopened chest lifetime |
| `FactionInvasion.WinnerGraceMinutes` | 10 | offline-winner open-to-all delay |
| `FactionInvasion.Announce` | 1 | zone chat announcements |
| `FactionInvasion.Debug` | 0 | module debug logging |

## Commands and add-on

- `.invasion start` — GM: start a wave in the player's zone immediately (queued to the map thread).
- `.invasion stop` — GM: end the active wave without a chest.
- `.invasion status` — show the active event.
- `.invasion` has `SEC_PLAYER` on `status`; `start`/`stop` require `SEC_GAMEMASTER`.
- `/inv` — open the current invasion's zone map; `/inv test` asks the server for the current state.

The add-on is installed by `acore-thor-launcher` as `Interface/AddOns/Invasion` (assets under
`app/src/main/assets/invasion`: `Invasion.toc`, `Invasion.lua`, `ring.tga`, `disc.tga`). It listens
on the `INVS` add-on prefix; no `RegisterAddonMessagePrefix` is required (the call is guarded for
clients that have it).

## Verification

Passed on 2026-11-25 against the pinned core `8a4e76e`:

- `git apply --check` of the core patch on the pin.
- `scripts/syntax-check.sh` — all 21 module translation units, including this module, compile with
  the core's own flags (48 s).
- `scripts/sql/check_module_sql.py --repo .` — re-runnable module SQL.
- `scripts/faction-invasion/gen_invasion.py --check` — committed SQL matches the generator.
- `scripts/check-module-wiring.py` — pins, three workflows and `thor-install.sh` name the module.
- Lua syntax of `Invasion.lua` via LuaJIT (`lupa`).

Not yet exercised: a real 3.3.5a client (the world-map circle and minimap arrow), a running realm
for the spawn/loot/chest path, and the `Model`/`creaturecache` behaviour of a clone in a live
client. Those are the remaining live-acceptance checks; the module's data and code paths are
otherwise gated by the offline checks above.

## Files

```
conf/mod-faction-invasion.conf.dist
data/sql/world/base/*.sql
src/FactionInvasion.cpp
src/faction_invasion.h
src/faction_invasion_loader.cpp
```
