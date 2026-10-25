# Travel portals

A pure-database two-way portal network between the capital cities, delivered as
custom world SQL for a solo-hosted realm.

## Problem

A solo player on a handheld pays the full retail travel tax for every
cross-continent trip: a boat or zeppelin across the ocean, then two taxi
flights (or a long ride) to the destination city. With no group to summon and
no second account to park a mage, that is several minutes of doing nothing
each time a quest or trainer sends the player to the other continent.

## What the patch adds

Sixteen `gameobject` spawns, two per capital, always within one network faction
and always to a city on the other continent (Ironforge's template is the one
neutral exception; see the template table below):

| Capital | Faction | Continent | Portals to |
|---|---|---|---|
| Stormwind | Alliance | Eastern Kingdoms | Darnassus, Exodar |
| Ironforge | Alliance | Eastern Kingdoms | Darnassus, Exodar |
| Darnassus | Alliance | Kalimdor | Stormwind, Ironforge |
| Exodar | Alliance | Kalimdor | Stormwind, Ironforge |
| Orgrimmar | Horde | Kalimdor | Undercity, Silvermoon |
| Thunder Bluff | Horde | Kalimdor | Undercity, Silvermoon |
| Undercity | Horde | Eastern Kingdoms | Orgrimmar, Thunder Bluff |
| Silvermoon | Horde | Eastern Kingdoms | Orgrimmar, Thunder Bluff |

Each spawn is a stock `gameobject_template` row with `type = 22`
(`GAMEOBJECT_TYPE_SPELLCASTER`); its `Data0` is the portal spell. On use the
player is the caster, not the object: `GameObject::Use` initialises
`Unit* spellCaster = user` and the type-22 branch has that unit cast the
template's `Data0` on itself with `TRIGGERED_NONE`, so it is a normal validated
cast and the spell's own attributes (combat restriction, power cost) apply to
the player. The destination is the core's own `spell_target_position` row, so
no new template is needed:

| City | Template entry | Spell | Template faction (addon) | Zone |
|---|---|---|---|---|
| Stormwind | 183325 | 17334 | 1732 (Alliance) | 1519 |
| Ironforge | 176497 | 44089 | 0 (neutral) | 1537 |
| Darnassus | 183317 | 17608 | 1732 (Alliance) | 1657 |
| Exodar | 183321 | 32268 | 1732 (Alliance) | 3557 |
| Orgrimmar | 183323 | 17609 | 1735 (Horde) | 1637 |
| Undercity | 183327 | 17611 | 1735 (Horde) | 1497 |
| Thunder Bluff | 183326 | 17610 | 1735 (Horde) | 1638 |
| Silvermoon | 183324 | 32270 | 1735 (Horde) | 3487 |

Ironforge's template is the one neutral template (faction 0) and is used only
as an Alliance destination; the other seven carry their network faction in
`gameobject_template_addon`.

Seven of the eight templates are also spawned in the stock data (the Shattrath
portals in map 530), which is direct evidence that spawning the template works.
Ironforge's template 176497 is the exception: it exists as a template but has
no stock spawn. Its spell 44089 has a valid `spell_target_position` row (map 0,
Ironforge), which is the same requirement the other seven meet, so the
generator keeps it rather than dropping the Ironforge destination.

### Placement

Each portal stands in its **host** city, next to where that city's own portal
spell lands a player. The generator reads the host city's arrival row from
`spell_target_position` (`ID, EffectIndex, MapID, PositionX, PositionY,
PositionZ, Orientation`) and puts the host's two portals on two different
points around it:

```
slot A (lower destination template entry):
x = PositionX + 5 * cos(Orientation)
y = PositionY + 5 * sin(Orientation)

slot B (higher destination template entry):
x = PositionX + 5 * cos(Orientation) + 5 * cos(Orientation + pi/2)
y = PositionY + 5 * sin(Orientation) + 5 * sin(Orientation + pi/2)

z = PositionZ, orientation = Orientation   (both slots)
```

The two destinations per host are sorted by their template entry id before
assignment, so which portal is on the arrival axis and which is to the side is
deterministic and does not depend on the order of the network table. Slot A is
five yards from the landing point; slot B keeps that five-yard forward offset
and adds five more yards to the side (about 7.07 yards from the landing
point), and the two spawns are about five yards apart, so two stacked,
indistinguishable portal objects cannot occur.

The `map` is the **host** row's `MapID`; `zoneId` and `areaId` are the host
city's zone from the table above (each asserted to appear as a `GhostZone` in
the base `graveyard_zone.sql`, which is how the ids were checked). The
template id and the spell it casts remain the **destination** city's, so
clicking the object takes the player to that destination. The generator also
asserts that the destination spell arrives on the destination's own map and
that host and destination maps differ.

Orientation is written as both the Euler `orientation` and the matching
quaternion `rotation0..3 = 0, 0, sin(o/2), cos(o/2)`, rounded to six decimals.
Both portals in a host share the arrival orientation.

The offsets are separation, not loop prevention: type 22 has no proximity
trigger and casts only on `CMSG_GAMEOBJ_USE`, so standing inside the model
cannot re-trigger it. Five yards keeps both portals off the exact landing point
(so a player does not land inside a portal model) and off the stock "Portal to
Blasted Lands" object that already occupies that pad. The first offset is along
the arrival orientation because that is the direction the teleport faces the
player; the second is perpendicular so the host's second portal is visible and
separately clickable instead of occupying the same coordinates.

The portals stand at the arrival pad rather than at each capital's inn because
that is where the stock type-22 Blasted Lands portals already are: the core
data proves those pads are reachable and used for portals. The generator
checks that proof on every run (see Verification) instead of assuming it.

The non-position columns are copied from the stock Shattrath portal spawns:
`spawnMask 1`, `phaseMask 1`, `spawntimesecs 181`, `animprogress 100`,
`state 1`, `ScriptName ''`, `VerifiedBuild 0`. Guids come from the private
range 9800000-9800099. The file deletes exactly the guids it inserts, never a
range: the server seeds its gameobject guid allocator from `MAX(guid) + 1`, so
after this file applies, objects the server creates are allocated above these
guids and a range delete could remove them on re-apply.

### Generated file

`sql/custom/db_world/travel_portals_10_spawns.sql` is generated and must not
be edited by hand. It is exactly one `DELETE` naming the sixteen guids and one
`INSERT` with sixteen rows, sorted by guid. Regenerate with:

```bash
python3 scripts/travel/gen_portals.py --core ../azerothcore-wotlk
python3 scripts/travel/gen_portals.py --core ../azerothcore-wotlk --check
```

## How it is applied

The installer fetches the CUSTOM tree from this repository on every install or
re-run (`--from-source` extracts it with the module sources; the prebuilt path
fetches only `sql/custom`) and places it at the core tree's
`data/sql/custom/db_world/`. That directory is already registered as the
`CUSTOM` entry in `updates_include`, so the core's DB updater picks the files
up on the next worldserver boot. Before that installer change, the files only
arrived inside a rebuilt release artifact.

With the shipped `Updates.Redundancy = 1`, the updater tracks each CUSTOM file
by filename and SHA-1 hash in the `acore_world.updates` table and **re-applies**
a file whose content changed, logging `>> Reapplying update "…" (it changed)`.
So editing `travel_portals_10_spawns.sql` and re-running the installer is
enough: the new hash is applied on the next boot. The exact-guid `DELETE` at
the top of the file is what makes that safe and idempotent. Renaming the file
or deleting its `updates` row is only needed if redundancy checking is turned
off.

## What is not touched

- No DBC or client data.
- No new `gameobject_template` row.
- No module, script or core patch.
- No `spell_target_position` edit and no spell edit.
- No taxi, boat, zeppelin or graveyard change.

## Risks

- **Mechanism.** The player casts the template's spell on himself
  (`GameObject::Use` passes the user as the caster, `TRIGGERED_NONE`), so the
  spell is a normal validated cast: combat state, power cost and any other
  spell attributes apply to the player. Seven templates are proven by stock
  Shattrath spawns; Ironforge's 176497 is not spawned in stock data, only
  present as a template, so it is the one row with no in-data precedent for the
  spawn side. Its spell target row is valid.
- **Placement.** Positions are derived from each host city's teleport arrival
  point plus fixed five-yard offsets (forward for slot A, perpendicular for
  slot B), not hand-placed. The pad itself is checked against the stock type-22
  Blasted Lands portals on every run, but the resulting portal positions have
  not been seen in a running client; a pad near a wall or slope could still put
  a portal slightly inside geometry.
- **Hub location.** The portals sit at the arrival pad rather than at the
  capital inn, so they are not literally the "portal room" a retail player
  expects; they are wherever that city's teleport drops a player.
- **Faction.** `gameobject_template_addon.faction` is 1732 (Alliance) or 1735
  (Horde) for seven templates; Ironforge's 176497 is 0 (neutral) and is the
  only neutral one, used only as an Alliance destination. The server-side use
  path does not test that faction, so it does not gate who can click the
  portal; it is template metadata, not an access check.

## Verification

The generator fails loudly (non-zero exit, named entry) on any of these, so a
refreshed core pin cannot silently produce a stale or broken file:

- every template entry exists in `gameobject_template`, exactly once, with
  `type = 22`;
- each template's `Data0` equals the spell in the table above;
- every template has exactly one `gameobject_template_addon` row; all but
  176497 carry faction 1732 (Alliance) or 1735 (Horde), and 176497 must be 0
  (neutral) and may only be used as the Ironforge destination;
- a host's non-neutral destinations agree on one faction, the host's own
  template carries that faction (Ironforge's neutral template is the named
  exception), and the destination templates carry it too;
- each spell has exactly one `spell_target_position` row with
  `EffectIndex = 0`;
- each arrival row's `MapID` is that city's real map (0 for Stormwind,
  Ironforge, Undercity; 1 for Darnassus, Orgrimmar, Thunder Bluff; 530 for
  Exodar, Silvermoon), for both the host and the destination;
- host and destination maps differ;
- every host zone appears as a `GhostZone` in the base `graveyard_zone.sql`;
- each host's arrival pad has a stock type-22 "Portal to Blasted Lands" spawn
  (195141/195142) within 30 yards horizontally and |dz| < 5 (Stormwind's stock
  object is ~100 yd above the pad, so it is checked at the pad's x/y with a
  wider dz bound — named in the code);
- the spawn point is 1-8 yards from the host arrival point and never zero;
- the host's two spawns are at least 5 yards apart;
- every generated guid is inside 9800000-9800099 and free in the base
  `gameobject.sql` dump;
- every generated position is finite and within +/-20000 on each axis;
- exactly sixteen spawns are produced.

The dump parser reads column order from each file's own `CREATE TABLE` block
and asserts every row's value count matches it.

Honest gap: the SQL has not been imported into a live realm and the portals
have not been clicked in a client. The mechanism and the data are verified
against the pinned dumps only.
