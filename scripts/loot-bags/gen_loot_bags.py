#!/usr/bin/env python3
"""Generate the custom loot-bag SQL from the pinned core's base world dumps.

The patch is pure data: a set of openable ``item_template`` rows (class 15,
``ITEM_FLAG_HAS_LOOT``), their ``item_loot_template`` contents, one
``reference_loot_template`` table per level bracket and creature rank that rolls
the bag quality, and one ``creature_loot_template`` row per loot id.

Money is not a loot-template row: the core reads ``item_template.minMoneyLoot`` /
``maxMoneyLoot`` when an item is opened (``Player.cpp`` item-loot branch), so the
bags get their coin from the item template.

Contents reference the base database's own world-loot quality pools (the
``Vanilla/TBC/WotLK {Greys,Whites,Greens,Blues,Purples} N-M Level Range``
references), chosen by the items' actual ``RequiredLevel`` so the gear is usable
at the bracket.  No item icons or client files are added: every bag reuses the
``displayid`` of an existing bag item, and the item query response carries that
display id to the client.

Run against a checkout of the pinned core (see ``pins.toml``)::

    python3 scripts/loot-bags/gen_loot_bags.py --core ../azerothcore-wotlk

``--check`` regenerates in memory and fails if the committed SQL differs.
"""

from __future__ import annotations

import argparse
import difflib
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# ---------------------------------------------------------------------------
# Tuning
# ---------------------------------------------------------------------------

BRACKET_WIDTH = 5
MAX_BRACKET_LEVEL = 80
BRACKETS = [
    (lo, min(lo + BRACKET_WIDTH - 1, MAX_BRACKET_LEVEL))
    for lo in range(1, MAX_BRACKET_LEVEL + 1, BRACKET_WIDTH)
]

# Item ids are a private range above the base item entries (max 54591 in the
# pinned dumps); reference ids sit above the base reference entries (max
# 1619100).  Delete ranges are deliberately wider than the current data so a
# regenerated, smaller dataset cannot leave stale rows behind.
BAG_ENTRY_BASE = 91000
BAG_ENTRY_DELETE_FROM = 91000
BAG_ENTRY_DELETE_TO = 91099
DROP_REF_BASE = 9800100
DROP_REF_DELETE_FROM = 9800100
DROP_REF_DELETE_TO = 9800199

# Bag tiers.  ``displayid`` reuses an existing bag: 8271 Small Brown Pouch,
# 3337 Small Silk Pack, 8861 Black Silk Pack, 20342 Bottomless Bag.
TIERS = [
    {
        "name": "Worn",
        "quality": 1,
        "displayid": 8271,
        "cloth": (2, 4),
        "heal": (60, 1, 2),
        "mana": (40, 1, 1),
    },
    {
        "name": "Sturdy",
        "quality": 2,
        "displayid": 3337,
        "cloth": (3, 5),
        "heal": (70, 1, 2),
        "mana": (50, 1, 2),
    },
    {
        "name": "Fine",
        "quality": 3,
        "displayid": 8861,
        "cloth": (4, 6),
        "heal": (100, 2, 2),
        "mana": (70, 1, 2),
    },
    {
        "name": "Exquisite",
        "quality": 4,
        "displayid": 20342,
        "cloth": (5, 8),
        "heal": (100, 2, 3),
        "mana": (100, 2, 3),
    },
]

# (min, max) copper for tiers 1..4, per bracket.  10000 copper = 1 gold.
MONEY = [
    [(100, 300), (500, 1200), (2000, 4000), (6000, 10000)],
    [(200, 500), (800, 1800), (3000, 6000), (9000, 15000)],
    [(400, 900), (1500, 3000), (5000, 9000), (15000, 25000)],
    [(800, 1800), (3000, 6000), (10000, 18000), (30000, 50000)],
    [(1500, 3000), (5000, 10000), (18000, 30000), (50000, 80000)],
    [(2500, 5000), (8000, 16000), (28000, 45000), (75000, 120000)],
    [(4000, 8000), (12000, 24000), (40000, 65000), (110000, 170000)],
    [(6000, 12000), (18000, 35000), (60000, 95000), (160000, 250000)],
    [(9000, 18000), (25000, 50000), (85000, 130000), (220000, 350000)],
    [(12000, 24000), (35000, 70000), (120000, 180000), (300000, 480000)],
    [(18000, 35000), (50000, 100000), (180000, 280000), (450000, 700000)],
    [(25000, 50000), (70000, 140000), (250000, 380000), (600000, 950000)],
    [(35000, 70000), (100000, 200000), (350000, 520000), (850000, 1300000)],
    [(50000, 100000), (150000, 300000), (500000, 750000), (1200000, 1900000)],
    [(70000, 140000), (200000, 400000), (700000, 1050000), (1700000, 2600000)],
    [(100000, 200000), (300000, 600000), (1000000, 1500000), (2400000, 3800000)],
]

# Cloth, healing potion and mana potion per bracket.
BRACKET_ITEMS = [
    (2589, 118, 2455),      # Linen Cloth, Minor Healing/Mana Potion
    (2589, 118, 2455),
    (2592, 858, 3385),      # Wool Cloth, Lesser Healing/Mana Potion
    (2592, 858, 3385),
    (4306, 929, 3827),      # Silk Cloth, Healing/Mana Potion
    (4306, 929, 3827),
    (4338, 1710, 6149),     # Mageweave Cloth, Greater Healing/Mana Potion
    (4338, 1710, 6149),
    (14047, 3928, 13443),   # Runecloth, Superior Healing/Mana Potion
    (14047, 3928, 13443),
    (14047, 13446, 13444),  # Runecloth, Major Healing/Mana Potion
    (14047, 13446, 13444),
    (21877, 22829, 22832),  # Netherweave Cloth, Super Healing/Mana Potion
    (21877, 22829, 22832),
    (33470, 33447, 33448),  # Frostweave Cloth, Runic Healing/Mana Potion
    (33470, 33447, 33448),
]

# Creature rank (creature_template.rank) -> drop chance and bag-tier weights.
# Groups 101..105 in the reference tables match rank 0..4.
RANKS = [
    {"name": "normal", "chance": 10, "weights": [70.0, 24.0, 5.5, 0.5]},
    {"name": "elite", "chance": 20, "weights": [45.0, 40.0, 13.0, 2.0]},
    {"name": "rare elite", "chance": 35, "weights": [20.0, 45.0, 28.0, 7.0]},
    {"name": "boss", "chance": 100, "weights": [0.0, 30.0, 50.0, 20.0]},
    {"name": "rare", "chance": 50, "weights": [20.0, 45.0, 28.0, 7.0]},
]

BAG_GROUP = 101  # creature/reference group used by every bag row (base data uses 1..7)

QUALITY_ID = {"Greys": 0, "Whites": 1, "Greens": 2, "Blues": 3, "Purples": 4}
POOL_LABEL = re.compile(
    r"^(Vanilla|TBC|WotLK) (Greys|Whites|Greens|Blues|Purples) (\d+)-(\d+) Level Range$"
)

ITEM_INSERT_COLUMNS = [
    "entry", "class", "subclass", "name", "displayid", "Quality", "Flags",
    "BuyCount", "BuyPrice", "SellPrice", "InventoryType", "AllowableClass",
    "AllowableRace", "ItemLevel", "RequiredLevel", "maxcount", "stackable",
    "ContainerSlots", "bonding", "description", "minMoneyLoot", "maxMoneyLoot",
    "flagsCustom", "VerifiedBuild",
]
LOOT_INSERT_COLUMNS = [
    "Entry", "Item", "Reference", "Chance", "QuestRequired", "LootMode",
    "GroupId", "MinCount", "MaxCount", "Comment",
]


# ---------------------------------------------------------------------------
# Base dump reader
# ---------------------------------------------------------------------------

def read_columns(path: Path, table: str) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"CREATE TABLE `{re.escape(table)}` \((.*?)\n\) ENGINE", text, re.S)
    if not match:
        raise SystemExit(f"{path}: no CREATE TABLE for {table}")
    columns = [
        m.group(1)
        for line in match.group(1).splitlines()
        if (m := re.match(r"\s*`(\w+)`", line))
    ]
    if not columns:
        raise SystemExit(f"{path}: no columns found for {table}")
    return columns


def split_values(row: str) -> list:
    """Split one MySQL VALUES tuple into Python values (NULL, numbers, strings)."""
    if not (row.startswith("(") and row.endswith(")")):
        raise ValueError(f"not a tuple: {row[:80]!r}")
    text = row[1:-1]
    values = []
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i] in " \t\r\n,":
            i += 1
        if i >= n:
            break
        if text[i] == "'":
            i += 1
            out = []
            escaped = False
            while i < n:
                ch = text[i]
                if escaped:
                    out.append({"0": "\0", "n": "\n", "r": "\r", "t": "\t", "Z": "\x1a"}.get(ch, ch))
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == "'":
                    if i + 1 < n and text[i + 1] == "'":
                        out.append("'")
                        i += 1
                    else:
                        break
                else:
                    out.append(ch)
                i += 1
            i += 1
            values.append("".join(out))
        else:
            j = i
            while j < n and text[j] != ",":
                j += 1
            token = text[i:j].strip()
            if token.upper() == "NULL":
                values.append(None)
            else:
                try:
                    values.append(int(token))
                except ValueError:
                    try:
                        values.append(float(token))
                    except ValueError:
                        raise ValueError(f"unparsed token {token!r} in {row[:120]!r}")
            i = j
        while i < n and text[i] in " \t\r\n":
            i += 1
    return values


def read_table(path: Path, table: str):
    """Return (column -> index, list of value tuples) for one base dump."""
    columns = read_columns(path, table)
    index = {name: i for i, name in enumerate(columns)}
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = f"INSERT INTO `{table}` VALUES"
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"{path}: no INSERT INTO `{table}` VALUES")
    i = start + len(marker)
    n = len(text)
    depth = 0
    in_string = False
    escaped = False
    row_start = None
    rows = []
    while i < n:
        ch = text[i]
        if row_start is None:
            if ch == "(":
                row_start = i
                depth = 1
            i += 1
            continue
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == "'":
                in_string = False
        else:
            if ch == "'":
                in_string = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    values = split_values(text[row_start:i + 1])
                    if len(values) != len(columns):
                        raise SystemExit(
                            f"{path}: {table} row {len(rows) + 1} has {len(values)} values, "
                            f"expected {len(columns)}"
                        )
                    rows.append(values)
                    row_start = None
            elif ch == ";" and depth == 0:
                break
        i += 1
    if not rows:
        raise SystemExit(f"{path}: no rows in `{table}`")
    return index, rows


# ---------------------------------------------------------------------------
# Pool catalogue
# ---------------------------------------------------------------------------

class Pool:
    __slots__ = ("ref", "realm", "quality", "label_lo", "label_hi", "items", "req_min", "req_max", "req_median")

    def __init__(self, ref, realm, quality, label_lo, label_hi, items, reqs):
        self.ref = ref
        self.realm = realm
        self.quality = quality
        self.label_lo = label_lo
        self.label_hi = label_hi
        self.items = items
        self.req_min = min(reqs)
        self.req_max = max(reqs)
        self.req_median = statistics.median(reqs)


def build_pools(world: Path):
    item_index, item_rows = read_table(world / "item_template.sql", "item_template")
    ref_index, ref_rows = read_table(world / "reference_loot_template.sql", "reference_loot_template")

    items = {}
    for row in item_rows:
        items[row[item_index["entry"]]] = {
            "quality": row[item_index["Quality"]],
            "required": row[item_index["RequiredLevel"]],
            "class": row[item_index["class"]],
            "flags": row[item_index["Flags"]],
            "name": row[item_index["name"]],
        }

    labelled = defaultdict(set)
    for row in ref_rows:
        match = POOL_LABEL.match(row[ref_index["Comment"]])
        reference = row[ref_index["Reference"]]
        if match and reference > 0:
            labelled[reference].add(
                (match.group(1), match.group(2), int(match.group(3)), int(match.group(4)))
            )

    direct = defaultdict(list)
    for row in ref_rows:
        if row[ref_index["Reference"]] == 0:
            direct[row[ref_index["Entry"]]].append(row)

    pools = []
    for reference, labels in labelled.items():
        identities = {(realm, quality) for realm, quality, _lo, _hi in labels}
        if len(identities) != 1:
            raise SystemExit(f"reference {reference} has conflicting labels: {sorted(labels)}")
        realm, quality = next(iter(identities))
        label_lo = min(label[2] for label in labels)
        label_hi = max(label[3] for label in labels)
        rows = direct.get(reference, [])
        if not rows:
            continue
        if any(row[ref_index["GroupId"]] != 1 or row[ref_index["Chance"]] != 0 for row in rows):
            # A few base pools mix explicit chances with equal-chanced rows.  They
            # still roll one item, but not uniformly, so leave them out of the
            # bag contents rather than silently changing their distribution.
            print(
                f"note: skipping non-uniform pool {reference} ({realm} {quality})",
                file=sys.stderr,
            )
            continue
        item_ids = [row[ref_index["Item"]] for row in rows]
        if len(set(item_ids)) != len(item_ids):
            raise SystemExit(f"reference {reference}: duplicate item rows")
        reqs = []
        for item_id in item_ids:
            item = items.get(item_id)
            if item is None:
                raise SystemExit(f"reference {reference}: item {item_id} missing from item_template")
            if item["quality"] != QUALITY_ID[quality]:
                raise SystemExit(
                    f"reference {reference} ({realm} {quality}): item {item_id} "
                    f"{item['name']!r} has quality {item['quality']}"
                )
            if item["class"] not in (2, 4):
                raise SystemExit(
                    f"reference {reference} ({realm} {quality}): item {item_id} "
                    f"{item['name']!r} is class {item['class']}, not gear"
                )
            if item["flags"] & 0x00000001:  # ITEM_FLAG_NO_PICKUP
                raise SystemExit(f"reference {reference}: item {item_id} {item['name']!r} cannot be picked up")
            reqs.append(item["required"])
        pools.append(Pool(reference, realm, quality, label_lo, label_hi, item_ids, reqs))
    return items, pools


def select_pool(pools: list[Pool], quality: str, lo: int, hi: int):
    """Pick the pool of *quality* whose required levels best cover [lo, hi]."""
    candidates = []
    for pool in pools:
        if pool.quality != quality:
            continue
        if pool.req_max < lo or pool.req_min > hi:
            continue
        distance = abs(pool.req_median - (lo + hi) / 2)
        candidates.append((distance, pool.ref, pool))
    if not candidates:
        return None
    return min(candidates, key=lambda entry: (entry[0], entry[1]))[2]


def pools_for_bracket(pools: list[Pool], lo: int, hi: int):
    available = {quality: select_pool(pools, quality, lo, hi) for quality in QUALITY_ID}
    common = available["Whites"] or available["Greys"]
    # A grey pool can span several brackets (25-35, 37-45).  If its top end is
    # more than four levels above the bracket, a green pool is the more usable
    # common-quality pick; the tier is defined by its roll count and money too.
    if common is None or common.req_max > hi + 4:
        common = available["Greens"]
    uncommon = available["Greens"]
    rare = available["Blues"] or uncommon
    epic = available["Purples"] or available["Blues"] or uncommon
    if not common or not uncommon or not rare or not epic:
        raise SystemExit(f"bracket {lo}-{hi}: no usable pools ({available})")
    return {"common": common, "uncommon": uncommon, "rare": rare, "epic": epic}


# ---------------------------------------------------------------------------
# Data construction
# ---------------------------------------------------------------------------

def gear_plan(tier_index: int, pools: dict):
    if tier_index == 0:
        return [(pools["common"], 1, 100)]
    if tier_index == 1:
        return [(pools["uncommon"], 1, 100), (pools["uncommon"], 1, 35)]
    if tier_index == 2:
        return [(pools["rare"], 2, 100), (pools["uncommon"], 1, 100)]
    return [(pools["epic"], 2, 100), (pools["rare"], 2, 100)]


def build_bags(items, pools):
    item_rows = []
    loot_rows = []
    bracket_pools = []
    for bracket_index, (lo, hi) in enumerate(BRACKETS, start=1):
        chosen = pools_for_bracket(pools, lo, hi)
        bracket_pools.append((lo, hi, chosen))
        cloth, heal, mana = BRACKET_ITEMS[bracket_index - 1]
        for tier_index, tier in enumerate(TIERS):
            entry = BAG_ENTRY_BASE + (bracket_index - 1) * len(TIERS) + tier_index
            if entry in items:
                raise SystemExit(f"bag entry {entry} already exists in item_template")
            money_min, money_max = MONEY[bracket_index - 1][tier_index]
            item_rows.append([
                entry,
                15,                      # ITEM_CLASS_MISC, like the lockboxes
                0,
                f"{tier['name']} Satchel ({lo}-{hi})",
                tier["displayid"],
                tier["quality"],
                0x00000004,              # ITEM_FLAG_HAS_LOOT: right-click to open
                1,
                0,
                0,
                0,                       # no InventoryType: not equippable
                -1,
                -1,
                hi,
                0,
                0,
                1,
                0,
                0,                       # not bound
                f"A level {lo}-{hi} cache: money, cloth, potions and gear.",
                money_min,
                money_max,
                0,
                12340,
            ])

            item_key = 1
            for pool, rolls, chance in gear_plan(tier_index, chosen):
                # A reference row has no count range: the core reads MaxCount as the reference
                # multiplier and ignores MinCount, and warns about every reference row whose
                # MinCount differs from its MaxCount.  Emit the multiplier in both columns.
                loot_rows.append([
                    entry, item_key, pool.ref, chance, 0, 1, 0, rolls, rolls,
                    f"{tier['name']} gear ({pool.realm} {pool.quality})",
                ])
                item_key += 1

            cloth_min, cloth_max = tier["cloth"]
            loot_rows.append([entry, cloth, 0, 100, 0, 1, 0, cloth_min, cloth_max, "Cloth"])
            heal_chance, heal_min, heal_max = tier["heal"]
            loot_rows.append([entry, heal, 0, heal_chance, 0, 1, 0, heal_min, heal_max, "Healing potion"])
            mana_chance, mana_min, mana_max = tier["mana"]
            loot_rows.append([entry, mana, 0, mana_chance, 0, 1, 0, mana_min, mana_max, "Mana potion"])
    return item_rows, loot_rows, bracket_pools


def drop_ref(bracket_index: int, rank_index: int) -> int:
    return DROP_REF_BASE + (bracket_index - 1) * len(RANKS) + rank_index + 1


def build_drop_tables():
    rows = []
    for bracket_index, (lo, hi) in enumerate(BRACKETS, start=1):
        for rank_index, rank in enumerate(RANKS):
            # One table per bracket and rank: reference_loot_template is keyed by
            # (Entry, Item), so the same bag cannot sit in several groups of one
            # table.  The table has a single group, matched by the creature row.
            ref_entry = drop_ref(bracket_index, rank_index)
            for tier_index, weight in enumerate(rank["weights"]):
                if weight <= 0:
                    continue
                bag_entry = BAG_ENTRY_BASE + (bracket_index - 1) * len(TIERS) + tier_index
                rows.append([
                    ref_entry, bag_entry, 0, weight, 0, 1, BAG_GROUP, 1, 1,
                    f"{lo}-{hi} {rank['name']} tier {tier_index + 1}",
                ])
    return rows


def build_creature_drops(world: Path):
    creature_index, creature_rows = read_table(world / "creature_template.sql", "creature_template")
    by_loot_id = defaultdict(list)
    for row in creature_rows:
        loot_id = row[creature_index["lootid"]]
        min_level = row[creature_index["minlevel"]]
        creature_type = row[creature_index["type"]]
        if not loot_id or min_level <= 0 or creature_type == 8:  # 8 = critter
            continue
        by_loot_id[loot_id].append(
            (min_level, row[creature_index["rank"]], row[creature_index["name"]])
        )

    # Guard against group-id collisions in the existing template rows.
    loot_index, loot_rows = read_table(world / "creature_loot_template.sql", "creature_loot_template")
    max_group = max(row[loot_index["GroupId"]] for row in loot_rows)
    if max_group >= BAG_GROUP:
        raise SystemExit(
            f"creature_loot_template already uses GroupId {max_group}; "
            f"the bag group is {BAG_GROUP}"
        )
    existing_entries = {row[loot_index["Entry"]] for row in loot_rows}
    missing = sorted(set(by_loot_id) - existing_entries)
    if missing:
        print(
            f"note: {len(missing)} loot ids have no creature_loot_template rows; "
            f"the bag row will create them (first: {missing[:5]})",
            file=sys.stderr,
        )

    rows = []
    tiers_by_loot = {}
    for loot_id in sorted(by_loot_id):
        records = by_loot_id[loot_id]
        levels = sorted(record[0] for record in records)
        median_level = levels[len(levels) // 2]
        bracket_index = min(
            len(BRACKETS), max(1, (median_level - 1) // BRACKET_WIDTH + 1)
        )
        rank_index = min(len(RANKS) - 1, max(record[1] for record in records))
        drop_ref_id = drop_ref(bracket_index, rank_index)
        chance = RANKS[rank_index]["chance"]
        lo, hi = BRACKETS[bracket_index - 1]
        rows.append([
            loot_id, drop_ref_id, drop_ref_id, chance, 0, 1, BAG_GROUP, 1, 1,
            f"loot bag (level {lo}-{hi}, {RANKS[rank_index]['name']})",
        ])
        tiers_by_loot[loot_id] = (bracket_index, rank_index)
    return rows, tiers_by_loot


# ---------------------------------------------------------------------------
# SQL emission
# ---------------------------------------------------------------------------

def sql_literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    if isinstance(value, float):
        text = f"{value:.4f}".rstrip("0").rstrip(".")
        return text if text else "0"
    return str(value)


def insert_statement(table: str, columns: list[str], rows: list[list]) -> str:
    head = f"INSERT INTO `{table}` (" + ", ".join(f"`{column}`" for column in columns) + ") VALUES\n"
    body = ",\n".join(
        "(" + ", ".join(sql_literal(value) for value in row) + ")" for row in rows
    )
    return head + body + ";"


def file_header(title: str, counts: str) -> list[str]:
    return [
        f"-- {title}",
        "-- Generated by scripts/loot-bags/gen_loot_bags.py from the pinned core's base world dumps.",
        "-- Do not edit by hand; change the generator and regenerate.",
        f"-- {counts}",
        "",
    ]


def validate_outputs(items, pools, item_rows, loot_rows, drop_rows, creature_rows):
    pool_refs = {pool.ref for pool in pools}
    drop_refs = {
        drop_ref(bracket_index, rank_index)
        for bracket_index in range(1, len(BRACKETS) + 1)
        for rank_index in range(len(RANKS))
    }
    bag_entries = {row[0] for row in item_rows}
    expected_bags = len(BRACKETS) * len(TIERS)
    if len(item_rows) != expected_bags or len(bag_entries) != expected_bags:
        raise SystemExit(f"expected {expected_bags} bag rows, got {len(item_rows)}")

    item_col = {name: i for i, name in enumerate(ITEM_INSERT_COLUMNS)}
    for row in item_rows:
        if row[item_col["class"]] != 15 or row[item_col["subclass"]] != 0:
            raise SystemExit(f"bag {row[0]}: class/subclass must be 15/0")
        if not row[item_col["Flags"]] & 0x00000004:
            raise SystemExit(f"bag {row[0]}: missing ITEM_FLAG_HAS_LOOT")
        if row[item_col["Quality"]] not in (1, 2, 3, 4):
            raise SystemExit(f"bag {row[0]}: bad quality")
        if not (0 < row[item_col["minMoneyLoot"]] <= row[item_col["maxMoneyLoot"]]):
            raise SystemExit(f"bag {row[0]}: bad money range")
        if row[item_col["RequiredLevel"]] != 0 or row[item_col["ContainerSlots"]] != 0:
            raise SystemExit(f"bag {row[0]}: must be usable at any level and hold no slots")

    loot_col = {name: i for i, name in enumerate(LOOT_INSERT_COLUMNS)}
    loot_by_bag = defaultdict(list)
    loot_keys = set()
    for row in loot_rows:
        bag = row[loot_col["Entry"]]
        key = (bag, row[loot_col["Item"]])
        if key in loot_keys:
            raise SystemExit(f"duplicate item_loot_template key {key}")
        loot_keys.add(key)
        if bag not in bag_entries:
            raise SystemExit(f"item_loot_template entry {bag} is not a bag")
        if row[loot_col["Reference"]]:
            if row[loot_col["Reference"]] not in pool_refs:
                raise SystemExit(f"bag {bag}: unknown reference {row[loot_col['Reference']]}")
            if not 0 < row[loot_col["Chance"]] <= 100:
                raise SystemExit(f"bag {bag}: bad reference chance")
            # The core reads MaxCount as the reference multiplier, ignores MinCount, and logs
            # "minCount != maxCount which is not supported for this loot type" for reference
            # rows that disagree.  Rows from here must therefore carry one multiplier twice.
            if row[loot_col["MinCount"]] != row[loot_col["MaxCount"]]:
                raise SystemExit(
                    f"bag {bag}: reference row {row[loot_col['Item']]} has "
                    f"MinCount {row[loot_col['MinCount']]} != MaxCount {row[loot_col['MaxCount']]}"
                )
            if row[loot_col["MaxCount"]] < 1:
                raise SystemExit(f"bag {bag}: reference multiplier must be at least 1")
        else:
            if row[loot_col["Item"]] not in items:
                raise SystemExit(f"bag {bag}: direct item {row[loot_col['Item']]} does not exist")
            if not 1 <= row[loot_col["MinCount"]] <= row[loot_col["MaxCount"]] <= 255:
                raise SystemExit(f"bag {bag}: bad direct-item count")
        loot_by_bag[bag].append(row)
    for bag in bag_entries:
        if not any(row[loot_col["Reference"]] for row in loot_by_bag[bag]):
            raise SystemExit(f"bag {bag}: no gear reference")

    groups = defaultdict(list)
    drop_keys = set()
    for row in drop_rows:
        # reference_loot_template is keyed by (Entry, Item) in the pinned schema.
        key = (row[0], row[loot_col["Item"]])
        if key in drop_keys:
            raise SystemExit(f"duplicate reference_loot_template key {key}")
        drop_keys.add(key)
        if row[0] not in drop_refs:
            raise SystemExit(f"drop table {row[0]} is outside the generated range")
        if row[loot_col["Item"]] not in bag_entries:
            raise SystemExit(f"drop table {row[0]}: item {row[loot_col['Item']]} is not a bag")
        group = row[loot_col["GroupId"]]
        if group != BAG_GROUP:
            raise SystemExit(f"drop table {row[0]}: group {group} is not {BAG_GROUP}")
        groups[(row[0], group)].append(row[loot_col["Chance"]])
    for (entry, group), chances in groups.items():
        if abs(sum(chances) - 100.0) > 0.01:
            raise SystemExit(f"drop table {entry} group {group}: weights sum to {sum(chances)}")
    if len(groups) != len(drop_refs):
        raise SystemExit(f"expected {len(drop_refs)} rank tables, found {len(groups)}")

    creature_keys = set()
    used_drop_refs = set()
    for row in creature_rows:
        key = (row[0], row[loot_col["Item"]], row[loot_col["Reference"]], row[loot_col["GroupId"]])
        if key in creature_keys:
            raise SystemExit(f"duplicate creature_loot_template key {key}")
        creature_keys.add(key)
        reference = row[loot_col["Reference"]]
        if reference not in drop_refs:
            raise SystemExit(f"creature {row[0]}: unknown drop table {reference}")
        if row[loot_col["Item"]] != reference:
            raise SystemExit(f"creature {row[0]}: item key must mirror the reference")
        group = row[loot_col["GroupId"]]
        if group != BAG_GROUP:
            raise SystemExit(f"creature {row[0]}: group {group} is not {BAG_GROUP}")
        rank_index = (reference - DROP_REF_BASE - 1) % len(RANKS)
        if row[loot_col["Chance"]] != RANKS[rank_index]["chance"]:
            raise SystemExit(f"creature {row[0]}: chance does not match rank {RANKS[rank_index]['name']}")
        used_drop_refs.add(reference)
    if used_drop_refs != drop_refs:
        unused = sorted(drop_refs - used_drop_refs)
        print(f"note: drop tables without creatures: {unused}", file=sys.stderr)


def build_outputs(world: Path):
    items, pools = build_pools(world)
    item_rows, loot_rows, bracket_pools = build_bags(items, pools)
    drop_rows = build_drop_tables()
    creature_rows, _ = build_creature_drops(world)
    validate_outputs(items, pools, item_rows, loot_rows, drop_rows, creature_rows)

    items_sql = file_header(
        "Loot bags: openable item templates and their contents.",
        f"{len(item_rows)} bags, {len(loot_rows)} item-loot rows.",
    )
    items_sql += [
        f"DELETE FROM `item_template` WHERE `entry` BETWEEN {BAG_ENTRY_DELETE_FROM} AND {BAG_ENTRY_DELETE_TO};",
        f"DELETE FROM `item_loot_template` WHERE `Entry` BETWEEN {BAG_ENTRY_DELETE_FROM} AND {BAG_ENTRY_DELETE_TO};",
        "",
        insert_statement("item_template", ITEM_INSERT_COLUMNS, item_rows),
        "",
        insert_statement("item_loot_template", LOOT_INSERT_COLUMNS, loot_rows),
        "",
    ]

    tables_sql = file_header(
        "Loot bags: one reference table per level bracket and creature rank.",
        f"{len(BRACKETS) * len(RANKS)} tables, {len(drop_rows)} rows.",
    )
    tables_sql += [
        f"DELETE FROM `reference_loot_template` WHERE `Entry` BETWEEN {DROP_REF_DELETE_FROM} AND {DROP_REF_DELETE_TO};",
        "",
        insert_statement("reference_loot_template", LOOT_INSERT_COLUMNS, drop_rows),
        "",
    ]

    drops_sql = file_header(
        "Loot bags: one independent drop row per lootable creature template loot id.",
        f"{len(creature_rows)} creature loot ids.",
    )
    drops_sql += [
        f"DELETE FROM `creature_loot_template` WHERE `Reference` BETWEEN {DROP_REF_DELETE_FROM} AND {DROP_REF_DELETE_TO};",
        "",
        insert_statement("creature_loot_template", LOOT_INSERT_COLUMNS, creature_rows),
        "",
    ]

    return {
        "loot_bags_10_items.sql": "\n".join(items_sql),
        "loot_bags_20_drop_tables.sql": "\n".join(tables_sql),
        "loot_bags_30_creature_drops.sql": "\n".join(drops_sql),
    }, bracket_pools


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--core", required=True, type=Path, help="checkout of the pinned AzerothCore tree")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "sql" / "custom" / "db_world",
        help="output directory (default: sql/custom/db_world)",
    )
    parser.add_argument("--check", action="store_true", help="fail if the committed SQL differs from a fresh run")
    args = parser.parse_args()

    world = args.core / "data" / "sql" / "base" / "db_world"
    if not world.is_dir():
        raise SystemExit(f"{world} does not exist; --core must point at a core checkout")

    outputs, bracket_pools = build_outputs(world)
    if args.check:
        failures = 0
        for name, content in outputs.items():
            path = args.out / name
            if not path.exists():
                print(f"missing: {path}", file=sys.stderr)
                failures += 1
                continue
            current = path.read_text(encoding="utf-8")
            if current != content:
                print(f"out of date: {path}", file=sys.stderr)
                diff = list(
                    difflib.unified_diff(
                        current.splitlines(),
                        content.splitlines(),
                        fromfile=str(path),
                        tofile="freshly generated",
                        lineterm="",
                        n=1,
                    )
                )
                print("\n".join(diff[:40]), file=sys.stderr)
                failures += 1
        if failures:
            return 1
        print(f"up to date: {len(outputs)} files in {args.out}")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    for name, content in outputs.items():
        (args.out / name).write_text(content, encoding="utf-8")
        print(f"wrote {args.out / name}")

    for lo, hi, chosen in bracket_pools:
        line = "  ".join(
            f"{key}={pool.ref}({pool.realm} {pool.quality} req {pool.req_min}-{pool.req_max})"
            for key, pool in chosen.items()
        )
        print(f"bracket {lo:>2}-{hi:<2}: {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
