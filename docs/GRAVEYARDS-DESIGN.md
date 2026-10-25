# Graveyard coverage for unlinked questing zones (not shipped)

A database patch that added one custom graveyard per area the stock `graveyard_zone`
table leaves unlinked was generated, reviewed and then removed before it ever reached a
realm. The generator and its SQL file are gone from this repository. This page is the
record of what was attempted and why the data says it was wrong.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported,
**[I]** inference.

## Proposal

A solo player who dies in a zone with no graveyard link is released at
`GetDefaultGraveyard` — Westfall (id 4) for the Alliance, the Crossroads (id 10) for the
Horde (`GameGraveyard.cpp:86-95`) [V] — which is a corpse run from the far side of the
world. The plan was to remove that fallback entirely: for every area that has quests, no
stock graveyard link and at least three quest-starter spawns to place a point on, add one
`game_graveyard` row and one neutral (`Faction 0`) `graveyard_zone` link, so the release
point would always be inside the area the player died in.

Positions were derived from the base dumps, not authored: the patch took the area's
quest ids from `quest_template.QuestSortID`, resolved them to creature templates through
`creature_queststarter`, collected those templates' spawns on maps 0 and 1 from
`creature`, and placed the graveyard at the spawn nearest the group's median `(x, y)`.
Ids lived in the private range 9900000-9900999. The work shipped as
`sql/custom/db_world/graveyards_10_zones.sql`, produced by
`scripts/graveyards/gen_graveyards.py`; both have been deleted.

## What the data actually says

A positive `quest_template.QuestSortID` is an AreaTable id, and most of those ids are
**sub-areas**, not zones. The core flattens the distinction before any graveyard lookup:
`Map::GetZoneAndAreaId` overwrites the zone id with the area's parent
(`Map.cpp:1328-1333`) [V], and `GetClosestGraveyard` consults the area id first, then
falls back to the zone id (`GameGraveyard.cpp:146-152`) [V]. A parent zone that is
already linked therefore covers all of its sub-areas.

Counting the 182 distinct AreaTable ids that appear in `quest_template.QuestSortID`
against the stock links [V]:

| Measure | Count |
|---|---|
| Quest AreaTable ids in the base dump | 182 |
| Of those, unlinked in stock `graveyard_zone` | 48 |
| Unlinked ids that are sub-areas with an already-linked parent zone | 43 |
| Unlinked top-level ids | 5 |

The five top-level unlinked ids are Designer Island (151), Deeprun Tram (2257), Eye of
the Storm (3820), Strand of the Ancients (4384) and Isle of Conquest (4710) — a
developer island, a tram, and three battlegrounds. None is an open-world questing zone on
map 0 or map 1, and battlegrounds do not use this release path.

**Stock graveyard coverage on the two continents is complete.** The fallback this patch
set out to remove does not fire for any open-world questing zone.

## Why it was reverted rather than fixed

Because the area lookup runs before the zone lookup, a row keyed to a sub-area is found
first and the working parent-zone link is never consulted. The patch did not fill gaps:
it **shadowed** stock release points for 15 sub-areas, every one of which already had a
working link through its parent zone. All 15 selected ids were sub-areas whose parent zone is linked
in stock data [V]:

| Area id | Area | Parent zone (already linked) |
|---|---|---|
| 9 | Northshire | 12 Elwynn Forest |
| 19 | Zul'Gurub | 33 Stranglethorn Vale |
| 132 | Coldridge Valley | 1 Dun Morogh |
| 133 | Gnomeregan | 1 Dun Morogh |
| 154 | Deathknell | 85 Tirisfal Glades |
| 188 | Shadowglen | 141 Teldrassil |
| 220 | Red Cloud Mesa | 215 Mulgore |
| 363 | Valley of Trials | 14 Durotar |
| 702 | Rut'theran Village | 141 Teldrassil |
| 978 | Zul'Farrak | 440 Tanaris |
| 1116 | Feathermoon Stronghold | 357 Feralas |
| 1517 | Uldaman | 3 Badlands |
| 1717 | Razorfen Kraul | 17 The Barrens |
| 1769 | Timbermaw Hold | 361 Felwood |
| 2562 | Karazhan | 41 Deadwind Pass |

Five of the new release points were **farther from the corpse** than the stock point they
hid [V]:

| Area | Stock release | New release | Ratio | Where the new point lands |
|---|---|---|---|---|
| 1717 Razorfen Kraul | 602 yd | 3673 yd | 6.1x | Thunder Bluff |
| 1517 Uldaman | 582 yd | 2180 yd | 3.7x | inside Ironforge's Hall of Explorers |
| 19 Zul'Gurub | 891 yd | 2461 yd | 2.8x | — |
| 133 Gnomeregan | 917 yd | 2206 yd | 2.4x | inside Ironforge's Tinker Town |
| 978 Zul'Farrak | 960 yd | 1134 yd | 1.2x | — |

`Faction` was 0 (neutral) on every row, so the Uldaman and Gnomeregan entries released a
Horde player inside the Alliance capital [V].

The other ten rows were smaller changes in the same wrong direction: each sat 24-211
yards from an already-linked stock graveyard whose spirit healer was 3-17 yards away,
while the new point was 25-781 yards from the nearest spirit healer [V]. They traded
"release beside a healer" for "release at a quest NPC". A patch that makes death worse is
not worth keeping for a marginal position change.

## The two generator defects

- **The median was computed across both continents.** A single `(x, y)` median over a
  group whose spawns exist on maps 0 and 1 lets a minority-map cluster decide the pick.
  The Uldaman row is the clear case: the Orgrimmar quest starters for that instance dragged
  the result into Ironforge, on the wrong continent and the wrong faction's capital [V].
- **"The quest starter stands in the quest's zone" was assumed, never checked.**
  `creature.zoneId` is zero for those NPCs in the pinned dumps, so a starter could be
  anywhere — a capital, another continent — and the generator would still treat its
  position as representative of the area [V].

## What would be worth doing instead

The honest options, none of which was implemented:

1. **Nothing.** Stock data is complete for open-world questing zones; there is no gap
   for a database patch to fill.
2. **Spirit-healer density.** If release *distances* are the complaint, the lever is
   extra spirit-healer creature spawns, not `graveyard_zone` rows. That is a `creature`
   change with its own risks (patrol paths, faction, and duplicate-healer handling).
3. **Instance-entrance release points** for the five dungeon areas in the table above
   (Zul'Gurub is a raid). This would anchor on the entrance coordinate from
   `areatrigger_teleport` and need a faction-correct link per instance, since a neutral
   row is what released Horde players into Ironforge.

## Verification

What was checked, and how [V] unless noted:

- **Counts.** `quest_template.QuestSortID` from the pinned base dump re-parsed against
  the stock `graveyard_zone` links: 182 ids, 48 unlinked, 43 sub-areas with a linked
  parent, 5 top-level. The unlinked status of each of the 15 selected ids was checked
  directly against the stock rows.
- **Parent-zone mapping.** Taken from the stock `graveyard_zone` comments and links
  (which name each graveyard's zone) plus an external AreaTable dump. Limitation:
  `areatable_dbc.sql` in the pinned checkout carries the table definition but **no rows**,
  so the parent ids are the one input that is not in this repository.
- **Distances.** Straight-line 2-D yards (`sqrt(dx^2 + dy^2)`) between `game_tele` area
  anchors for out-of-instance areas, `areatrigger_teleport` entrance coordinates for
  instances, and the graveyard coordinates in `game_graveyard`. Spirit-healer distances
  use the nearest spirit-healer `creature` spawn in the base dump. These are plan-view
  distances; they say nothing about the path a ghost has to walk.
- **Core behaviour.** Read from the pinned source at the line references above
  (`Map.cpp:1328-1333`, `GameGraveyard.cpp:86-95`, `GameGraveyard.cpp:146-161`).
- **Hardcore module.** Irrelevant to this patch: `mod-hardcore` overrides no
  graveyard-selection hook — it has no `OnPlayerCanRepopAtGraveyard` override by design
  (`Hardcore.cpp:505`), and its local change only keeps release allowed for a fallen
  ghost so a spirit healer stays reachable.
- **Not verified: anything in a game client or on the Thor.** No release point was
  visited, no corpse run measured in game, and the geometry above rests on the dumps and
  plan-view distance only.
