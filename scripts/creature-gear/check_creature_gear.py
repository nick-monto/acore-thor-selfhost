#!/usr/bin/env python3
"""Validate the generated mod-creature-gear world SQL against the pinned core's base dumps.

Re-parses <core>/data/sql/base/db_world/{creature_template,creature,item_template}.sql and the
five generated files in --sql-dir, then asserts every cross-file invariant the generator relies on.
Also re-derives the rules it needs from scripts/creature-gear/gear.toml (--rules) to gate the
bind-on-equip coverage of the worn gear and the off-hand fill rate.
Exits non-zero with a precise message on the first failure; prints a summary table on success.

The spawn file is checked against the column form gen_creature_gear.derive_creature_spawn_columns
derives from <core>/data/sql/updates/db_world, not against the base dumps: the module's SQL runs in
the updater's MODULE pass, so on a migrated database the statement keys on `creature`.`id` alone.

Python 3.12, standard library only.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
import tomllib
from pathlib import Path

# The generator is a sibling script, not an installed package; importing it here keeps the derived
# spawn column form in one place instead of two copies that can drift apart.
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True      # keep the check out of the source tree

from gen_creature_gear import (  # noqa: E402
    BASE_SPAWN_COLUMNS,
    SpawnColumnForm,
    derive_creature_spawn_columns,
)

# The generator's slot table, its five varying slots and the variant table's columns; imported so
# the shape has one source of truth instead of two copies that can drift apart.
from gen_creature_gear import OUTFIT_SLOTS as GENERATOR_OUTFIT_SLOTS  # noqa: E402
from gen_creature_gear import OUTFIT_VARIANT_COLUMNS, VARIED_OUTFIT_SLOTS  # noqa: E402

# InventoryType values the core accepts in one of the three virtual item slots.
HAND_INVENTORY_TYPES = (13, 14, 15, 17, 21, 22, 23, 25, 26)

# Highest item entry present in the server's Item.dbc.
MAX_ITEM_ENTRY = 56806

# item_template.class values.
CLASS_WEAPON = 2
CLASS_ARMOR = 4

# item_template.bonding = 2 is "bind on equip": BIND_WHEN_EQUIPPED, the only bonding the module
# may drop while CreatureGear.Drop.RequireBindOnEquip = 1 (its shipped default).
BOND_ON_EQUIP = 2

# The off-hand position's (item class, InventoryType) pairs, mirroring ROLES in gen_creature_gear.py:
# off-hand weapons (class 2, 13 WEAPON / 22 WEAPONOFFHAND) plus shields and holdables (class 4,
# 14 SHIELD / 23 HOLDABLE). An empty candidate pool here means ItemID2 is legitimately all zero.
OFFHAND_CLASS_INVENTORY_TYPES = (
    (CLASS_WEAPON, 13),
    (CLASS_WEAPON, 22),
    (CLASS_ARMOR, 14),
    (CLASS_ARMOR, 23),
)

# Outfit slots in SMSG_MIRRORIMAGE_DATA order; ItemEntry<N> must map to one of these types.
OUTFIT_SLOTS = (
    ("HEAD", (1,)),
    ("SHOULDERS", (3,)),
    ("BODY", (4,)),
    ("CHEST", (5, 20)),            # chest, falling back to robe
    ("WAIST", (6,)),
    ("LEGS", (7,)),
    ("FEET", (8,)),
    ("WRISTS", (9,)),
    ("HANDS", (10,)),
    ("BACK", (16,)),
    ("TABARD", (19,)),
)

GENERATED_FILES = (
    "creature_gear_10_schema.sql",
    "creature_gear_20_weapon_pools.sql",
    "creature_gear_30_outfits.sql",
    "creature_gear_35_outfit_variants.sql",
    "creature_gear_40_spawn_equipment.sql",
)

OUTFIT_COLUMNS = ("CreatureID", "Race", "Gender", "Class", "Skin", "Face", "HairStyle", "HairColor", "FacialHair") + tuple(
    f"ItemEntry{i}" for i in range(1, len(OUTFIT_SLOTS) + 1)
)

EQUIP_TEMPLATE_COLUMNS = ("CreatureID", "ID", "ItemID1", "ItemID2", "ItemID3")

# The phase-A spawn UPDATE, parsed generically and then asserted against the derived column form
# (see parse_spawn_updates): group 1 is the SET column, group 2 the WHERE clause.
SPAWN_UPDATE_RE = re.compile(r"^UPDATE `creature` SET `([^`]+)` = -1 WHERE (.+);$")

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SQL_DIR = REPO_ROOT / "modules" / "mod-creature-gear" / "data" / "sql" / "world" / "base"
DEFAULT_RULES_TOML = Path(__file__).resolve().parent / "gear.toml"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_rules(path: Path) -> dict:
    """The rule values this validator needs; defaults mirror gen_creature_gear.py."""
    if not path.is_file():
        fail(f"missing rules file {path}")
    with path.open("rb") as fh:
        rules = tomllib.load(fh)
    defaults = {
        "min_boe_coverage": 0.70,
        "item_level_window": 8,
        "required_level_margin": 3,
        "quality_max": 2,
    }
    for key, value in defaults.items():
        rules.setdefault(key, value)
    return rules


# --------------------------------------------------------------------------- parsing


def parse_columns(text: str, path: Path) -> list[str]:
    match = re.search(r"CREATE TABLE `[^`]+` \((.*?)\n\) ENGINE", text, re.S)
    if not match:
        fail(f"{path}: no CREATE TABLE block found")
    return [m.group(1) for m in (re.match(r"\s*`([^`]+)`", line) for line in match.group(1).splitlines()) if m]


def parse_dump(path: Path) -> tuple[list[str], list[list[str]]]:
    """Parse a mysqldump-style single-table dump: one tuple per line, '(' ... ')' per line."""
    text = path.read_text(encoding="utf-8", errors="replace")
    columns = parse_columns(text, path)
    width = len(columns)
    rows: list[list[str]] = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line.startswith("("):
            continue
        body = line[:-1] if line.endswith(",") or line.endswith(";") else line
        if not body.endswith(")"):
            fail(f"{path}:{number}: tuple does not end with ')'")
        fields = next(csv.reader([body[1:-1]], quotechar="'", escapechar="\\", doublequote=False))
        if len(fields) != width:
            fail(f"{path}:{number}: {len(fields)} fields, expected {width}")
        rows.append(fields)
    if not rows:
        fail(f"{path}: no data rows found")
    return columns, rows


def parse_generated_insert(path: Path, table: str, columns: tuple[str, ...]) -> list[list[str]]:
    """Rows of the INSERT INTO `table` statements in a generated file (ON DUPLICATE KEY UPDATE ignored)."""
    if not path.is_file():
        fail(f"missing generated file {path}")
    rows: list[list[str]] = []
    in_statement = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("INSERT INTO"):
            match = re.match(r"INSERT INTO `([^`]+)` \(([^)]*)\) VALUES$", stripped)
            if not match:
                fail(f"{path}:{number}: unexpected INSERT statement shape")
            if match.group(1) != table:
                fail(f"{path}:{number}: INSERT into `{match.group(1)}`, expected `{table}`")
            declared = tuple(part.strip().strip("`") for part in match.group(2).split(","))
            if declared != columns:
                fail(f"{path}:{number}: columns {declared} do not match the expected {columns}")
            in_statement = True
            continue
        if not in_statement or not stripped.startswith("("):
            continue
        more = stripped.endswith(",")
        body = stripped[:-1] if more else stripped
        if not body.endswith(")"):
            fail(f"{path}:{number}: tuple does not end with ')'")
        fields = [value.strip() for value in next(csv.reader([body[1:-1]], quotechar="'", escapechar="\\", doublequote=False))]
        if len(fields) != len(columns):
            fail(f"{path}:{number}: {len(fields)} values, expected {len(columns)} for `{table}`")
        for value in fields:
            if not value.isdigit():
                fail(f"{path}:{number}: non-numeric value {value!r} in `{table}` row")
        rows.append(fields)
        if not more:
            in_statement = False
    if not rows:
        fail(f"{path}: no data rows found")
    return rows


def parse_spawn_updates(path: Path, form: SpawnColumnForm) -> list[int]:
    """Entry values of the spawn UPDATEs, each asserted against the derived column form.

    Every statement must match `spawn_update_statement(entry, form)` byte for byte: same SET column,
    same WHERE columns, same order. On a schema where the update chain has dropped the base dumps'
    secondary-id column, naming it (`id2 = 0`) or the pre-rename primary (`id1`) is a hard failure.
    """
    if not path.is_file():
        fail(f"missing generated file {path}")
    absent = [column for column in BASE_SPAWN_COLUMNS if column not in form.columns]
    entries = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("--"):
            continue
        match = SPAWN_UPDATE_RE.match(stripped)
        if not match:
            fail(f"{path}:{number}: unexpected statement {stripped!r}")
        for column in absent:
            if f"`{column}`" in stripped:
                fail(
                    f"{path}:{number}: statement names `{column}`, which the core's update chain no longer"
                    f" has on `creature`; the derived form is {form.where_form}"
                )
        prefix = f"`{form.entry}` = "
        tail = f" AND {form.clause_tail}"
        clause = match.group(2)
        if match.group(1) != form.equipment or not clause.startswith(prefix) or not clause.endswith(tail):
            fail(
                f"{path}:{number}: {stripped!r} does not match the derived spawn form"
                f" `UPDATE `creature` SET `{form.equipment}` = -1 WHERE {form.where_form};`"
            )
        value = clause[len(prefix) : -len(tail)]
        if not value.isdigit():
            fail(f"{path}:{number}: entry {value!r} is not a positive integer")
        entries.append(int(value))
    if not entries:
        fail(f"{path}: no UPDATE statements found")
    return entries


def parse_schema(path: Path) -> None:
    if not path.is_file():
        fail(f"missing generated file {path}")
    text = path.read_text(encoding="utf-8")
    if not re.search(r"CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit`", text):
        fail(f"{path}: no CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit`")
    for column in OUTFIT_COLUMNS:
        if not re.search(rf"^\s*`{column}` ", text, re.M):
            fail(f"{path}: column `{column}` missing from `mod_creature_gear_outfit`")
    if not re.search(r"PRIMARY KEY \(`CreatureID`\)", text):
        fail(f"{path}: PRIMARY KEY (`CreatureID`) missing")


def parse_variant_schema(path: Path) -> None:
    if not path.is_file():
        fail(f"missing generated file {path}")
    text = path.read_text(encoding="utf-8")
    if not re.search(r"CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit_variant`", text):
        fail(f"{path}: no CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit_variant`")
    for column in OUTFIT_VARIANT_COLUMNS:
        if not re.search(rf"^\s*`{column}` ", text, re.M):
            fail(f"{path}: column `{column}` missing from `mod_creature_gear_outfit_variant`")
    if not re.search(r"PRIMARY KEY \(`CreatureID`, `Variant`\)", text):
        fail(f"{path}: PRIMARY KEY (`CreatureID`, `Variant`) missing")


def check_slot_tables() -> None:
    """This module's slot table and the generator's must describe the same slots.

    The validator keeps its own InventoryType mapping (it asserts against it in the failure
    messages); the generator owns the packet order the module writes. A drift between them would
    make the variant columns land in the wrong slots, so it is checked rather than assumed.
    """
    if len(OUTFIT_SLOTS) != len(GENERATOR_OUTFIT_SLOTS):
        fail("OUTFIT_SLOTS and the generator's slot table have different lengths")
    for index, ((name, inventory_types), (generator_name, positions)) in enumerate(
        zip(OUTFIT_SLOTS, GENERATOR_OUTFIT_SLOTS)
    ):
        # A generator position is a tuple of (class, InventoryType) pairs; a slot may list several
        # positions (CHEST falls back to ROBE), each with its own candidate list.
        generator_types = tuple(
            inventory_type for position in positions for _, inventory_type in position
        )
        if name != generator_name or inventory_types != generator_types:
            fail(
                f"slot {index}: this validator has {name} {inventory_types},"
                f" the generator has {generator_name} {generator_types}"
            )
    expected_variant_columns = ("CreatureID", "Variant") + tuple(
        GENERATOR_OUTFIT_SLOTS[index][0].capitalize() for index in VARIED_OUTFIT_SLOTS
    )
    if OUTFIT_VARIANT_COLUMNS != expected_variant_columns:
        fail(f"variant columns {OUTFIT_VARIANT_COLUMNS} do not match the varying slots {VARIED_OUTFIT_SLOTS}")


def check_slot_item(where: str, slot: int, item_id: int, core: dict) -> None:
    """Shared per-item slot check: entry in range, in item_template, and fitting the slot's types."""
    if item_id == 0:
        return
    name, inventory_types = OUTFIT_SLOTS[slot]
    if item_id > MAX_ITEM_ENTRY:
        fail(f"{where}: {name} item {item_id} exceeds {MAX_ITEM_ENTRY}")
    item = core["items"].get(item_id)
    if item is None:
        fail(f"{where}: {name} item {item_id} is not in item_template")
    if item["inventory_type"] not in inventory_types:
        fail(f"{where}: {name} item {item_id} has InventoryType {item['inventory_type']}, expected one of {inventory_types}")


# --------------------------------------------------------------------------- checks


def build_core_maps(core: Path) -> dict:
    world = core / "data" / "sql" / "base" / "db_world"

    template_columns, template_rows = parse_dump(world / "creature_template.sql")
    at = {name: position for position, name in enumerate(template_columns)}
    creature_entries = {int(row[at["entry"]]) for row in template_rows}
    creature_levels = {int(row[at["entry"]]): int(row[at["maxlevel"]]) for row in template_rows}

    creature_columns, creature_rows = parse_dump(world / "creature.sql")
    ac = {name: position for position, name in enumerate(creature_columns)}
    form = derive_creature_spawn_columns(core, creature_columns)
    # The dump is pre-migration, so its columns keep their base names (the dump's `id1`, `id2`,
    # `equipment_id`); `form` decides which of its rows the post-migration UPDATE reaches, and
    # `spawns` is keyed by the dump's `id1`, i.e. the entry the migrated `id` column carries.
    spawns: dict[int, list[int]] = {}
    for row in creature_rows:
        if form.secondary is not None and int(row[ac["id2"]]) != 0:
            continue
        spawns.setdefault(int(row[ac["id1"]]), []).append(int(row[ac["equipment_id"]]))

    item_columns, item_rows = parse_dump(world / "item_template.sql")
    ai = {name: position for position, name in enumerate(item_columns)}
    items: dict[int, dict] = {}
    for row in item_rows:
        items[int(row[ai["entry"]])] = {
            "class": int(row[ai["class"]]),
            "inventory_type": int(row[ai["InventoryType"]]),
            "item_level": int(row[ai["ItemLevel"]]),
            "required_level": int(row[ai["RequiredLevel"]]),
            "quality": int(row[ai["Quality"]]),
            "bonding": int(row[ai["bonding"]]),
        }
    return {
        "creature_entries": creature_entries,
        "creature_levels": creature_levels,
        "spawns": spawns,
        "items": items,
        "form": form,
    }


def check_equip_templates(rows: list[list[str]], core: dict) -> list[int]:
    """Returns the CreatureIDs, ascending, after asserting every invariant."""
    entries = core["creature_entries"]
    items = core["items"]
    by_creature: dict[int, list[list[int]]] = {}
    for row in rows:
        creature_id, id_, *item_ids = (int(value) for value in row)
        if creature_id not in entries:
            fail(f"creature_equip_template: CreatureID {creature_id} is not in creature_template")
        if id_ not in (1, 2, 3):
            fail(f"creature_equip_template: CreatureID {creature_id} has ID {id_}, expected 1..3")
        if sum(1 for item_id in item_ids if item_id != 0) > 3:
            fail(f"creature_equip_template: CreatureID {creature_id} ID {id_} has more than 3 non-zero items")
        for item_id in item_ids:
            if item_id == 0:
                continue
            if item_id > MAX_ITEM_ENTRY:
                fail(f"creature_equip_template: item {item_id} exceeds the Item.dbc limit {MAX_ITEM_ENTRY}")
            item = items.get(item_id)
            if item is None:
                fail(f"creature_equip_template: item {item_id} is not in item_template")
            if item["inventory_type"] not in HAND_INVENTORY_TYPES:
                fail(f"creature_equip_template: item {item_id} has InventoryType {item['inventory_type']}, not hand-equippable")
        by_creature.setdefault(creature_id, []).append([id_, *item_ids])

    for creature_id, pool in by_creature.items():
        ids = sorted(entry[0] for entry in pool)
        if ids != list(range(1, len(ids) + 1)):
            fail(f"creature_equip_template: CreatureID {creature_id} has non-contiguous or duplicate IDs {ids}")
    return sorted(by_creature)


def check_outfits(rows: list[list[str]], core: dict) -> tuple[list[int], list[int]]:
    """Returns (CreatureIDs ascending, per-slot non-zero counts)."""
    entries = core["creature_entries"]
    seen: set[int] = set()
    slot_counts = [0] * len(OUTFIT_SLOTS)
    creature_ids = []
    item_offset = OUTFIT_COLUMNS.index("ItemEntry1")
    for row in rows:
        creature_id = int(row[0])
        if creature_id not in entries:
            fail(f"mod_creature_gear_outfit: CreatureID {creature_id} is not in creature_template")
        if creature_id in seen:
            fail(f"mod_creature_gear_outfit: duplicate CreatureID {creature_id} (primary key)")
        seen.add(creature_id)
        creature_ids.append(creature_id)
        for slot in range(len(OUTFIT_SLOTS)):
            item_id = int(row[item_offset + slot])
            check_slot_item(
                f"mod_creature_gear_outfit: CreatureID {creature_id} ItemEntry{slot + 1}", slot, item_id, core
            )
            if item_id:
                slot_counts[slot] += 1
    return sorted(creature_ids), slot_counts


def build_band_index(core: dict) -> dict[tuple[int, int], list[tuple[int, int, int]]]:
    """(class, InventoryType) -> sorted [(item_level, required_level, quality)] from item_template."""
    index: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
    for item in core["items"].values():
        index.setdefault((item["class"], item["inventory_type"]), []).append(
            (item["item_level"], item["required_level"], item["quality"])
        )
    for bucket in index.values():
        bucket.sort()
    return index


def band_candidate_count(
    index: dict[tuple[int, int], list[tuple[int, int, int]]], level: int, slot: int, rules: dict
) -> int:
    """Size of the generator's candidate list for one slot, re-derived from the item dump.

    The generator takes the first slot position with candidates (so CHEST falls back to ROBE) and
    that position's list holds every item of its class/InventoryType with ItemLevel in
    [level, level + window], RequiredLevel <= level + margin and Quality <= quality_max.
    """
    low = level
    high = level + rules["item_level_window"]
    required_max = level + rules["required_level_margin"]
    quality_max = rules["quality_max"]
    for position in GENERATOR_OUTFIT_SLOTS[slot][1]:
        count = 0
        for item_class, inventory_type in position:
            bucket = index.get((item_class, inventory_type), [])
            start = bisect.bisect_left(bucket, (low,))
            end = bisect.bisect_left(bucket, (high + 1,))
            count += sum(
                1
                for _, required_level, quality in bucket[start:end]
                if required_level <= required_max and quality <= quality_max
            )
        if count:
            return count
    return 0


def check_outfit_variants(
    rows: list[list[str]], outfit_rows: list[list[str]], core: dict, rules: dict
) -> tuple[list[int], dict[int, list[int]]]:
    """Returns (CreatureIDs ascending, per varying slot the list of distinct-item counts).

    Every creature must carry exactly `armour_variants` contiguous variants, each pick must fit its
    slot, the outfit row's five varying slots must be variant 1 (so a creature whose variant rows are
    missing shows the set the module would pick first), and each slot's picks must be as distinct as
    its candidate list allows: the generator's index advances by one per variant, so a slot with at
    least `armour_variants` candidates must yield that many different items, and a smaller pool must
    still be used to its full width.
    """
    variants_per_creature: dict[int, list[list[int]]] = {}
    for row in rows:
        creature_id = int(row[0])
        if creature_id not in core["creature_entries"]:
            fail(f"mod_creature_gear_outfit_variant: CreatureID {creature_id} is not in creature_template")
        variant = int(row[1])
        if not 1 <= variant <= rules["armour_variants"]:
            fail(
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} variant {variant}"
                f" is outside 1..{rules['armour_variants']}"
            )
        for column, slot in enumerate(VARIED_OUTFIT_SLOTS, start=2):
            check_slot_item(
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} variant {variant}"
                f" {OUTFIT_VARIANT_COLUMNS[column]}",
                slot,
                int(row[column]),
                core,
            )
        variants = variants_per_creature.setdefault(creature_id, [])
        if len(variants) != variant - 1:
            fail(
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} variants are not contiguous"
                f" from 1 (row {len(variants) + 1} carries variant {variant})"
            )
        variants.append([int(value) for value in row[2:]])

    item_offset = OUTFIT_COLUMNS.index("ItemEntry1")
    outfit_items = {int(row[0]): [int(value) for value in row[item_offset:]] for row in outfit_rows}
    index = build_band_index(core)
    distinct_counts: dict[int, list[int]] = {slot: [] for slot in VARIED_OUTFIT_SLOTS}

    for creature_id, variants in variants_per_creature.items():
        if len(variants) != rules["armour_variants"]:
            fail(
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} carries {len(variants)} variants,"
                f" expected {rules['armour_variants']}"
            )
        base = outfit_items.get(creature_id)
        if base is None:
            fail(f"mod_creature_gear_outfit_variant: CreatureID {creature_id} has no mod_creature_gear_outfit row")
        level = core["creature_levels"][creature_id]
        for position, slot in enumerate(VARIED_OUTFIT_SLOTS):
            picks = [variant[position] for variant in variants]
            name = OUTFIT_SLOTS[slot][0]
            if picks[0] != base[slot]:
                fail(
                    f"mod_creature_gear_outfit_variant: CreatureID {creature_id} {name} variant 1 item"
                    f" {picks[0]} does not match the outfit row's {base[slot]}"
                )
            candidates = band_candidate_count(index, level, slot, rules)
            distinct = len(set(picks) - {0})
            expected = min(rules["armour_variants"], candidates)
            if distinct != expected:
                fail(
                    f"mod_creature_gear_outfit_variant: CreatureID {creature_id} {name} offers {distinct}"
                    f" different item(s) across {len(variants)} variants, expected {expected}"
                    f" from a candidate list of {candidates}"
                )
            distinct_counts[slot].append(distinct)

    return sorted(variants_per_creature), distinct_counts


def check_spawn_updates(entries: list[int], core: dict, form: SpawnColumnForm) -> int:
    """Returns how many spawn rows the UPDATE statements will actually touch.

    The touched set is the derived form's: every dump row whose `id1` is the entry once the update
    chain has dropped the secondary-id column, and only that entry's `id2 = 0` rows while the column
    still exists. No touched row may carry hand-set equipment — `equipment_id = 0` in the statement
    would leave it alone, but the generator must not point a creature's spawns at a pool while one
    of them does. An entry the statement cannot match at all is a dead no-op and fails too.
    """
    spawns = core["spawns"]
    touched = 0
    for entry in entries:
        equipment_ids = spawns.get(entry)
        if not equipment_ids:
            fail(f"creature_gear_40_spawn_equipment.sql: entry {entry} matches no creature spawn row ({form.where_form})")
        for equipment_id in equipment_ids:
            if equipment_id != 0:
                fail(
                    f"creature_gear_40_spawn_equipment.sql: entry {entry} has a spawn row the statement"
                    f" matches ({form.where_form}) with equipment_id = {equipment_id};"
                    " the UPDATE would gear a spawn that already carries equipment"
                )
        touched += len(equipment_ids)
    return touched


def check_drop_coverage(
    equip_rows: list[list[str]],
    outfit_rows: list[list[str]],
    variant_rows: list[list[str]],
    core: dict,
    rules: dict,
) -> dict:
    """BoE coverage of the worn gear, plus the off-hand fill rate.

    A creature wears one of its three pool variants: the core resolves `creature.equipment_id = -1`
    to one of the IDs 1..n, and the module re-rolls that pick per spawn. Its armour is one of the
    outfit variants, picked per spawn point out of the five varying slots. The creature therefore
    counts as covered when at least one of its three `creature_equip_template` rows carries a
    bind-on-equip item (`item_template.bonding = 2`) among ItemID1..3 or one of its outfit entries -
    the row's eleven, or any of its variants' five - is BoE.
    Only BoE items may drop while CreatureGear.Drop.RequireBindOnEquip = 1, the shipped default.

    The candidate rules (level band, RequiredLevel margin, Quality cap) are frozen in gear.toml, so
    this number is a regression guard around them, not a target: the base item_template simply has
    no BoE armour below ItemLevel 10 and none inside the quality <= 2 band at levels 70-72.

    Returns the counts main() prints and gates on.
    """
    items = core["items"]
    outfit_item_offset = OUTFIT_COLUMNS.index("ItemEntry1")
    outfit_items = {int(row[0]): [int(value) for value in row[outfit_item_offset:]] for row in outfit_rows}
    for row in variant_rows:
        outfit_items[int(row[0])].extend(int(value) for value in row[2:])

    pools: dict[int, list[list[int]]] = {}
    for row in equip_rows:
        creature_id, _, *item_ids = (int(value) for value in row)
        pools.setdefault(creature_id, []).append(item_ids)

    def is_boe(item_id: int) -> bool:
        return item_id != 0 and items[item_id]["bonding"] == BOND_ON_EQUIP

    boe_covered = 0
    offhand_filled = 0
    for creature_id, creature_pools in pools.items():
        if any(is_boe(item_id) for pool in creature_pools for item_id in pool) or any(
            is_boe(item_id) for item_id in outfit_items[creature_id]
        ):
            boe_covered += 1
        if any(pool[1] != 0 for pool in creature_pools):
            offhand_filled += 1

    by_position: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
    for item in items.values():
        by_position.setdefault((item["class"], item["inventory_type"]), []).append(
            (item["item_level"], item["required_level"], item["quality"])
        )
    for bucket in by_position.values():
        bucket.sort()

    levels = {core["creature_levels"][creature_id] for creature_id in pools}
    return {
        "creatures": len(pools),
        "boe_covered": boe_covered,
        "offhand_filled": offhand_filled,
        "offhand_pool_available": offhand_pool_available(by_position, levels, rules),
    }


def offhand_pool_available(by_position: dict[tuple[int, int], list[tuple[int, int, int]]], levels: set[int], rules: dict) -> bool:
    """True when the off-hand position has candidates for at least one selected level band.

    Mirrors gen_creature_gear.ItemPool: ItemLevel in [level, level + item_level_window],
    RequiredLevel <= level + required_level_margin, Quality <= quality_max.
    """
    window = rules["item_level_window"]
    margin = rules["required_level_margin"]
    quality_max = rules["quality_max"]
    for level in levels:
        for position in OFFHAND_CLASS_INVENTORY_TYPES:
            bucket = by_position.get(position, ())
            start = bisect.bisect_left(bucket, (level, 0, 0))
            end = bisect.bisect_left(bucket, (level + window + 1, 0, 0))
            for _, required_level, quality in bucket[start:end]:
                if required_level <= level + margin and quality <= quality_max:
                    return True
    return False


def check_filename_collisions(sql_dir: Path, core: Path, playerbots: Path | None) -> None:
    trees = [("core", core / "data" / "sql")]
    if playerbots is not None and playerbots.is_dir():
        trees.append(("mod-playerbots", playerbots / "data" / "sql"))
    elif playerbots is not None:
        print(f"note: {playerbots}/data/sql does not exist, skipping that collision scan")
    for name in GENERATED_FILES:
        for label, tree in trees:
            if not tree.is_dir():
                continue
            collision = next((path for path in tree.rglob(name) if path.name == name), None)
            if collision is not None:
                fail(f"filename {name} collides with {label} SQL {collision}")


# --------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--core", type=Path, required=True, help="path to the pinned azerothcore-wotlk checkout")
    parser.add_argument("--sql-dir", type=Path, default=DEFAULT_SQL_DIR, help=f"generated SQL directory (default {DEFAULT_SQL_DIR})")
    parser.add_argument("--rules", type=Path, default=DEFAULT_RULES_TOML, help=f"rules file (default {DEFAULT_RULES_TOML})")
    parser.add_argument(
        "--playerbots",
        type=Path,
        default=None,
        help="path to the mod-playerbots checkout for the collision scan (default <core>/../mod-playerbots)",
    )
    args = parser.parse_args(argv)

    core_dir = args.core / "data" / "sql" / "base" / "db_world"
    if not core_dir.is_dir():
        fail(f"{args.core} does not look like an azerothcore-wotlk checkout")
    playerbots = args.playerbots if args.playerbots is not None else args.core.resolve().parent / "mod-playerbots"

    core = build_core_maps(args.core)
    form = core["form"]
    rules = load_rules(args.rules)

    parse_schema(args.sql_dir / GENERATED_FILES[0])
    parse_variant_schema(args.sql_dir / GENERATED_FILES[3])
    check_slot_tables()
    check_filename_collisions(args.sql_dir, args.core, playerbots)

    equip_rows = parse_generated_insert(args.sql_dir / GENERATED_FILES[1], "creature_equip_template", EQUIP_TEMPLATE_COLUMNS)
    outfit_rows = parse_generated_insert(args.sql_dir / GENERATED_FILES[2], "mod_creature_gear_outfit", OUTFIT_COLUMNS)
    variant_rows = parse_generated_insert(
        args.sql_dir / GENERATED_FILES[3], "mod_creature_gear_outfit_variant", OUTFIT_VARIANT_COLUMNS
    )
    spawn_entries = parse_spawn_updates(args.sql_dir / GENERATED_FILES[4], form)

    pool_creatures = check_equip_templates(equip_rows, core)
    outfit_creatures, slot_counts = check_outfits(outfit_rows, core)
    variant_creatures, variant_distinct = check_outfit_variants(variant_rows, outfit_rows, core, rules)
    spawn_rows = check_spawn_updates(spawn_entries, core, form)
    coverage = check_drop_coverage(equip_rows, outfit_rows, variant_rows, core, rules)

    if pool_creatures != outfit_creatures:
        fail("creature_equip_template and mod_creature_gear_outfit cover different creature sets")
    if variant_creatures != outfit_creatures:
        fail("mod_creature_gear_outfit_variant and mod_creature_gear_outfit cover different creature sets")
    if spawn_entries != sorted(set(spawn_entries)):
        fail("creature_gear_40_spawn_equipment.sql has duplicate or unsorted entry values")

    min_boe = rules["min_boe_coverage"]
    boe_coverage = coverage["boe_covered"] / coverage["creatures"]
    offhand_rate = coverage["offhand_filled"] / coverage["creatures"]
    if boe_coverage < min_boe:
        fail(
            f"bind-on-equip coverage {boe_coverage:.2%} ({coverage['boe_covered']}/{coverage['creatures']} creatures)"
            f" is below min_boe_coverage {min_boe:.2%} from {args.rules}"
        )
    if coverage["offhand_filled"] == 0 and coverage["offhand_pool_available"]:
        fail(
            "no creature got an off-hand item although the off-hand candidate pool"
            " (class 2 with 13/22 plus class 4 with 14/23) is non-empty for the selected level bands"
        )

    print("mod-creature-gear generated SQL: OK")
    print(f"  core dumps        {args.core}/data/sql/base/db_world")
    print(f"  update chain      {args.core}/data/sql/updates/db_world")
    print(f"  generated SQL     {args.sql_dir}")
    print(f"  rules             {args.rules}")
    print()
    print(f"  {'spawn WHERE form (derived)':<38}{form.where_form}")
    print(f"  {'selected creatures':<38}{len(pool_creatures):>8}")
    print(f"  {'creature_equip_template pool rows':<38}{len(equip_rows):>8}")
    print(f"  {'outfit rows':<38}{len(outfit_rows):>8}")
    print(f"  {'outfit variant rows':<38}{len(variant_rows):>8}")
    print(f"  {'spawn rows updated (equipment_id = -1)':<38}{spawn_rows:>8}")
    print()
    print(f"  {'BoE coverage (worn weapons or outfit)':<38}{coverage['boe_covered']:>8}  {boe_coverage:>7.2%}  (min_boe_coverage {min_boe:.2%})")
    print(f"  {'  of which off-hand filled':<38}{coverage['offhand_filled']:>8}  {offhand_rate:>7.2%}")
    if not coverage["offhand_pool_available"]:
        print("  note: the off-hand candidate pool is empty for every selected level band")
    print()
    print("  outfit rows filled per slot:")
    for slot, (name, _) in enumerate(OUTFIT_SLOTS):
        label = f"    {slot + 1:>2} {name}"
        print(f"  {label:<38}{slot_counts[slot]:>8}")
    print("  outfit variants: distinct items per creature, by varying slot:")
    for slot in VARIED_OUTFIT_SLOTS:
        counts = variant_distinct[slot]
        full = sum(1 for count in counts if count == rules["armour_variants"])
        mean = sum(counts) / len(counts) if counts else 0.0
        label = f"    {OUTFIT_SLOTS[slot][0].lower():<12}"
        print(f"  {label:<38}{mean:>8.2f}  full width {full:>4}/{len(counts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
