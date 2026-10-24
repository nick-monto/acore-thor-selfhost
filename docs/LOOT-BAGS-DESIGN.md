# Loot bags on every lootable creature

A pure database patch that gives each lootable creature a chance to drop a
level-banded satchel containing money, cloth, potions and gear. It is meant to
support solo play: the player kills level-appropriate creatures and the bags
keep gear, consumables and coin flowing without touching client files or C++.

Implemented in this repository as generated SQL under `sql/custom/db_world/`
and delivered through the core's already-registered **CUSTOM** SQL slot.

## What the player sees

- **Four bag qualities per level bracket**: Worn (white), Sturdy (green), Fine
  (blue), Exquisite (purple). The bag's quality colours its name; the icon is
  the existing icon of a classic bag (`Small Brown Pouch`, `Small Silk Pack`,
  `Black Silk Pack`, `Bottomless Bag`), so no client patch is needed.
- **Sixteen 5-level brackets**: 1-5, 6-10, ... 76-80. A creature's bracket is
  the median `creature_template.minlevel` of the creatures sharing its
  `lootid`; levels 81-83 fold into 76-80.
- **Rank scales both the chance and the quality**: normal 10%, elite 20%, rare
  elite 35%, rare 50%, boss 100%. Tier weights by rank:

| Rank | Worn | Sturdy | Fine | Exquisite |
|---|---:|---:|---:|---:|
| Normal | 70% | 24% | 5.5% | 0.5% |
| Elite | 45% | 40% | 13% | 2% |
| Rare elite / rare | 20% | 45% | 28% | 7% |
| Boss | — | 30% | 50% | 20% |

- **Contents** (all bags also contain money; ranges scale with bracket):

| Tier | Gear rolls | Cloth | Healing potion | Mana potion |
|---|---|---|---|---|
| Worn | 1 from the common pool | 2-4 | 60% for 1-2 | 40% for 1 |
| Sturdy | 2 from the uncommon pool (second at 35%) | 3-5 | 70% for 1-2 | 50% for 1-2 |
| Fine | 2 from the rare pool + 1 uncommon | 4-6 | 2 | 70% for 1-2 |
| Exquisite | 2 epic + 2 rare | 5-8 | 2-3 | 2-3 |

The gear pools are the base database's own world-loot pools (`Vanilla/TBC/WotLK
{Greys,Whites,Greens,Blues,Purples} N-M Level Range`). They are selected by the
items' actual `RequiredLevel`, not the pool's label, so gear is usable in the
bracket. When a bracket has no pool of the requested quality (e.g. no blues
below level 19), or the only common pool's required level tops out more than
four levels above the bracket, the tier falls back to the next-lower quality;
every bracket therefore has usable contents.

## Why it is database-only

Two core features carry the whole feature:

1. **Money inside an item** is `item_template.minMoneyLoot` /
   `maxMoneyLoot`. `Player::SendLoot()` calls `generateMoneyLoot()` on the item
   template before `FillLoot()` for the item branch, and
   `HandleLootMoneyOpcode()` handles the `HighGuid::Item` case. This is the same
   mechanism as the stock "A Sack of Coins" (item 5335).
2. **Item contents** are `item_loot_template` rows keyed by the bag item entry.
   A row with a non-zero `Reference` processes an existing
   `reference_loot_template` pool; `MaxCount` is the reference multiplier, so a
   row with `MaxCount=2` rolls two items from the pool. This is the mechanism
   the stock lockboxes already use.

   A reference row has no count range: `MinCount` is ignored, and the core's
   startup check logs `Table 'reference_loot_template' Entry ... has minCount
   ( 1 ) != maxCount ( 2 ) which is not supported for this loot type` for every
   reference row whose two counts disagree (`LootTemplate::CheckLootRefs` ->
   `LootStore::ReportInvalidCount`). The generator therefore writes the
   multiplier into **both** columns, and refuses to emit a reference row whose
   counts differ. The base world dumps hold no reference row with a range, so a
   clean install boots with no such warning.

The bag item itself is class 15 (miscellaneous), `Flags=4`
(`ITEM_FLAG_HAS_LOOT`), `InventoryType=0`, `ContainerSlots=0` — exactly the
lockbox/Sack-of-Coins shape. The client opens it with `CMSG_OPEN_ITEM`; the
core destroys the item once its loot window is empty.

No client DBC or MPQ edit is needed: custom item entries are served from
`item_template`, and `SMSG_ITEM_QUERY_SINGLE_RESPONSE` carries the borrowed
`displayid` to the client.

## Drop wiring

For every eligible `creature_template.lootid` (non-zero, positive level, not a
critter) the patch adds **one** `creature_loot_template` row:

```
Entry = lootid
Item = Reference = <drop table>
Chance = rank drop chance
GroupId = 101
MinCount = MaxCount = 1
```

The row sits in its own loot group, 101. The base data uses groups 0-7, so the
bag neither competes with nor replaces the creature's existing group loot. The
generator refuses to run if the base data ever grows a group at or above 101.

`<drop table>` is a generated `reference_loot_template` entry. There is **one
table per (bracket, rank)** — 16 brackets x 5 ranks = 80 tables, entries
9800101-9800180 — with a single group 101 holding the bag tiers as explicit
chances summing to 100. The table-per-rank shape is required by the schema:
`reference_loot_template`'s primary key is `(Entry, Item)` in this core, so the
same bag item cannot appear in several groups of one table. Unused rank tables
(for ranks a bracket has no creatures of) are harmless and stay for custom
creatures.

Reference ranges:

| Range | Used by |
|---|---|
| 91000-91063 | bag `item_template` entries |
| 91000-91099 | bag delete range in the item file |
| 9800101-9800180 | (bracket, rank) drop tables |
| 9800100-9800199 | drop-table and creature-row delete ranges |

## Delivery

- `sql/custom/db_world/loot_bags_10_items.sql` — bag items and their contents.
- `sql/custom/db_world/loot_bags_20_drop_tables.sql` — 80 rank tables.
- `sql/custom/db_world/loot_bags_30_creature_drops.sql` — one row per loot id.
- The base world database already has
  `('$/data/sql/custom/db_world','CUSTOM')` in `updates_include`, so the
  worldserver's DB updater applies these files on first boot and re-applies them
  when their content hash changes. Every file starts with a `DELETE` of its own
  ranges, so it is re-applicable.
- `.github/workflows/server-aarch64.yml` copies `sql/custom/db_world` into the
  artifact's `src/data/sql/custom/db_world` and checks that it contains SQL.
- `scripts/thor-install.sh --from-source` extracts the same directory from this
  repository's tarball; `--status` reports whether the patch is present.
- Prebuilt installs need no extra step: the SQL is inside the artifact.

## Regenerating

```sh
python3 scripts/loot-bags/gen_loot_bags.py --core <core>
python3 scripts/loot-bags/gen_loot_bags.py --core <core> --check
```

The generator reads the pinned core's base dumps
(`data/sql/base/db_world/{item_template,reference_loot_template,
creature_template,creature_loot_template}.sql`), validates the chosen pools and
every emitted row, and fails on a non-uniform pool, a missing item, a quality
mismatch, a group whose weights do not sum to 100, a duplicate key, or an id
collision. Tuning lives in the constants at the top of the script: bracket
width, tier weights, drop chances, money ranges, cloth/potion ids and roll
counts.

`Rate.Drop.Money`, `Rate.Drop.Item.*` and `Rate.Drop.Item.Referenced` still
apply on top of the patch, so server-side rate configuration changes the
economy without editing the SQL.

## Verification

| Check | Result |
|---|---|
| SQL syntax | all three files parse with `sqlglot` (MySQL dialect) |
| Real MySQL 8.0.46 | imported the five base tables, applied the three files, then applied them a second time: 64 bags, 304 item-loot rows, 80 drop tables, 7801 creature rows before and after |
| Relational integrity in MySQL | zero rows for: bad bag flags/money, bag without a gear reference, item-loot reference to a missing pool, direct item missing from `item_template`, bad counts, reference row whose `MinCount` differs from its `MaxCount`, drop row pointing to a missing bag, drop row outside group 101, creature row pointing to a missing table, creature row outside group 101, creature group collision |
| Weight/rank checks in MySQL | every rank table's group sums to 100; every creature row's chance matches the rank encoded in its table id; every loot id exists in `creature_template.lootid` |
| Roll simulation | 50k rolls per representative bracket/rank reproduced the configured drop chances (10/20/35/50/100%) and tier mixes; bag opens always produced cloth, potions and 1-4 gear items whose required level was inside the bracket or at most four levels above it |
| Generator `--check` | committed SQL matches a fresh run |
| Worldserver / client | **not run**: no worldserver boot or in-game open was performed here |

## Limits

- Bag contents scale with the **creature's** bracket, not the opener's level; a
  level 80 can farm level 1-5 bags and mail them to an alt.
- The bags are unbound, so they can be traded, mailed and stockpiled.
- Some base quality pools are small (purple pools can have 2-21 items), so epic
  contents repeat more than green contents.
- The 18 unused rank tables exist for completeness; the base creature set has no
  creatures of those bracket/rank combinations.
- No quest, keyed, or class-restricted bags are generated; every bag opens for
  every class and race.
