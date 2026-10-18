#!/usr/bin/env python3
"""Validate the generated mod-creature-gear world SQL against the pinned core's base dumps.

Re-parses <core>/data/sql/base/db_world/{creature_template,creature,item_template}.sql and the
five generated files in --sql-dir, then asserts every cross-file invariant the generator relies on.
Also re-derives the rules it needs from scripts/creature-gear/gear.toml (--rules) to gate the
bind-on-equip coverage of the worn gear and the off-hand fill rate.

The class rules are re-derived too, not trusted: every weapon-pool item and every outfit/variant
item is checked against the creature's own creature_template.unit_class — item_template.subclass
against the class's weapon set or armour chain (with the chain's fallbacks), item_template.
AllowableClass against the class bit, and the item's stat types against the class's focus — and the
weapon roles present must be exactly the class's non-empty ones, in order with contiguous IDs. The
weapon-pool prunes are checked with them: a creature whose class emits fewer roles than pool_size
must carry exactly one DELETE scoped to IDs n+1..pool_size, because the core resolves
creature.equipment_id = -1 by picking among the rows *present* for the entry, and a replayed older
dataset would otherwise still offer a role this one does not emit.

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
    ClassRules,
    SpawnColumnForm,
    choose_armour_tier,
    derive_creature_spawn_columns,
    emitted_roles,
    load_class_rules,
    offhand_position,
    outfit_positions,
)

# The generator's slot table, its five varying slots and the variant table's columns; imported so
# the shape has one source of truth instead of two copies that can drift apart.
from gen_creature_gear import OUTFIT_SLOTS as GENERATOR_OUTFIT_SLOTS  # noqa: E402
from gen_creature_gear import OUTFIT_VARIANT_COLUMNS, VARIED_OUTFIT_SLOTS, WARRIOR_CLASS  # noqa: E402

# InventoryType values the core accepts in one of the three virtual item slots.
HAND_INVENTORY_TYPES = (13, 14, 15, 17, 21, 22, 23, 25, 26)

# Highest item entry present in the server's Item.dbc.
MAX_ITEM_ENTRY = 56806

# item_template.bonding = 2 is "bind on equip": BIND_WHEN_EQUIPPED, the only bonding the module
# may drop while CreatureGear.Drop.RequireBindOnEquip = 1 (its shipped default).
BOND_ON_EQUIP = 2

# Outfit slots in SMSG_MIRRORIMAGE_DATA order; ItemEntry<N> must map to one of these types. This
# stays a class-independent description of the packet's slots — the per-class armour-tier positions
# come from the generator's outfit_positions() — and is compared against the generator's table by
# check_slot_tables.
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

# A weapon-pool prune in creature_gear_20_weapon_pools.sql: the creature's stale pool IDs above the
# roles this dataset emits. Groups: CreatureID, the role count n (exclusive lower bound) and the
# inclusive upper bound (pool_size).
POOL_DELETE_RE = re.compile(
    r"^DELETE FROM `creature_equip_template` WHERE `CreatureID` = (\d+)"
    r" AND `ID` > (\d+) AND `ID` <= (\d+);$"
)

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
        "pool_size": 3,
        "armour_variants": 4,
        "classes": {},
        "stat_groups": {},
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


def parse_pool_deletes(path: Path) -> list[tuple[int, int, int]]:
    """The (CreatureID, roles, upper ID) of every pool prune statement in the weapon-pool file.

    The statement shape is asserted here; whether the bounds are the right ones is
    check_pool_deletes' business.
    """
    if not path.is_file():
        fail(f"missing generated file {path}")
    deletes: list[tuple[int, int, int]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped.startswith("DELETE"):
            continue
        match = POOL_DELETE_RE.match(stripped)
        if not match:
            fail(f"{path}:{number}: unexpected DELETE statement {stripped!r}")
        deletes.append(tuple(int(value) for value in match.groups()))
    return deletes


def check_slot_tables() -> None:
    """This module's slot table and the generator's must describe the same slots.

    The validator keeps its own InventoryType mapping (it asserts against it in the failure
    messages); the generator owns the packet order the module writes. A drift between them would
    make the variant columns land in the wrong slots, so it is checked rather than assumed. Both
    tables are class-independent: which armour subclass a tier slot may draw from comes from the
    class's armour chain, applied by the generator's outfit_positions().
    """
    if len(OUTFIT_SLOTS) != len(GENERATOR_OUTFIT_SLOTS):
        fail("OUTFIT_SLOTS and the generator's slot table have different lengths")
    for index, ((name, inventory_types), (generator_name, generator_types)) in enumerate(
        zip(OUTFIT_SLOTS, GENERATOR_OUTFIT_SLOTS)
    ):
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
    # The creature's ChrClasses id; the rules' [classes.N] tables key on it.
    creature_classes = {int(row[at["entry"]]): int(row[at["unit_class"]]) for row in template_rows}

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
    stat_types = [ai[f"stat_type{index}"] for index in range(1, 11)]
    stat_values = [ai[f"stat_value{index}"] for index in range(1, 11)]
    items: dict[int, dict] = {}
    for row in item_rows:
        items[int(row[ai["entry"]])] = {
            "class": int(row[ai["class"]]),
            "subclass": int(row[ai["subclass"]]),
            "inventory_type": int(row[ai["InventoryType"]]),
            "allowable_class": int(row[ai["AllowableClass"]]),
            "item_level": int(row[ai["ItemLevel"]]),
            "required_level": int(row[ai["RequiredLevel"]]),
            "quality": int(row[ai["Quality"]]),
            "bonding": int(row[ai["bonding"]]),
            "stats": frozenset(
                int(row[stat_type])
                for stat_type, stat_value in zip(stat_types, stat_values)
                if int(row[stat_value]) != 0 and int(row[stat_type]) != 0
            ),
        }
    return {
        "creature_entries": creature_entries,
        "creature_levels": creature_levels,
        "creature_classes": creature_classes,
        "spawns": spawns,
        "items": items,
        "form": form,
    }


def build_band_index(core: dict) -> dict[tuple[int, int], list[tuple]]:
    """(class, InventoryType) -> rows sorted by (ItemLevel, entry).

    A row is (ItemLevel, RequiredLevel, Quality, entry, subclass, AllowableClass, stats, bonding) —
    the fields the class filters need. Items above MAX_ITEM_ENTRY are dropped exactly as the
    generator's index_items does: they have no Item.dbc row, so counting them would overstate a
    candidate list.
    """
    index: dict[tuple[int, int], list[tuple]] = {}
    for entry, item in core["items"].items():
        if entry > MAX_ITEM_ENTRY:
            continue
        index.setdefault((item["class"], item["inventory_type"]), []).append((
            item["item_level"],
            item["required_level"],
            item["quality"],
            entry,
            item["subclass"],
            item["allowable_class"],
            item["stats"],
            item["bonding"],
        ))
    for bucket in index.values():
        bucket.sort(key=lambda row: (row[0], row[3]))
    return index


class PoolMirror:
    """The generator's candidate enumeration, re-derived from the item dump.

    Mirrors gen_creature_gear.ItemPool: the level band is
    ItemLevel in [level, level + item_level_window], RequiredLevel <= level + required_level_margin
    and Quality <= quality_max, then the class filters — AllowableClass bit, no contrary stat, the
    class's weapon subclasses for weapons and the position's armour subclasses for armour. The
    generator's ordering (bind-on-equip first, then stat focus, then ItemLevel/entry) changes which
    item is picked, not how many candidates there are, so it is not modelled here; the picks that do
    land in the SQL are checked against these pools below.
    """

    def __init__(self, index: dict[tuple[int, int], list[tuple]], rules: dict) -> None:
        self._index = index
        self._rules = rules
        self._cache: dict[tuple, list[tuple]] = {}

    def candidates(self, position: tuple, level: int, class_rules: ClassRules) -> list[tuple]:
        key = (position, level, class_rules.id)
        cached = self._cache.get(key)
        if cached is not None:
            return cached
        low = level
        high = level + self._rules["item_level_window"]
        required_max = level + self._rules["required_level_margin"]
        quality_max = self._rules["quality_max"]
        rows: list[tuple] = []
        for item_class, inventory_type, armour_subclasses in position:
            bucket = self._index.get((item_class, inventory_type), ())
            start = bisect.bisect_left(bucket, (low,))
            end = bisect.bisect_left(bucket, (high + 1,))
            for row in bucket[start:end]:
                if row[1] > required_max or row[2] > quality_max:
                    continue
                if not class_rules.allows_item(item_class, row[4], row[5], row[6], armour_subclasses):
                    continue
                rows.append(row)
        self._cache[key] = rows
        return rows


def pool_entries(rows: list[tuple]) -> set[int]:
    """The item entries of one candidate list."""
    return {row[3] for row in rows}


def creature_class(core: dict, classes: dict[int, ClassRules], creature_id: int) -> ClassRules:
    """The creature's class rules; a unit_class with no [classes.N] table falls back to warrior."""
    return classes.get(core["creature_classes"][creature_id], classes[WARRIOR_CLASS])


def describe_item(core: dict, item_id: int) -> str:
    item = core["items"][item_id]
    return (
        f"subclass {item['subclass']}, class {item['class']}, InventoryType {item['inventory_type']},"
        f" AllowableClass {item['allowable_class']}, stats {sorted(item['stats'])}"
    )


def check_role_item(
    creature_id: int,
    class_rules: ClassRules,
    level: int,
    role: int,
    column: str,
    item_id: int,
    candidates: list[tuple],
    core: dict,
    description: str,
) -> None:
    """One hand slot against the role's re-derived candidate pool.

    The generator fills a non-empty pool (ItemPool.pick never returns 0 for one) and writes 0 for an
    empty one, so both directions are violations.
    """
    where = f"creature_equip_template: CreatureID {creature_id} ID {role + 1} {column}"
    if item_id == 0:
        if candidates:
            fail(
                f"{where} is 0 although the {class_rules.name} {description} pool holds"
                f" {len(candidates)} class-appropriate candidate(s) at level {level}"
            )
        return
    if item_id not in pool_entries(candidates):
        fail(
            f"{where} item {item_id} ({describe_item(core, item_id)}) is not a class-appropriate"
            f" {description} candidate for a {class_rules.name} at level {level};"
            f" the pool holds {len(candidates)} item(s)"
        )


def check_equip_templates(rows: list[list[str]], core: dict, pool: PoolMirror, rules: dict, classes: dict[int, ClassRules]) -> list[int]:
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

    for creature_id, creature_rows in by_creature.items():
        ids = sorted(entry[0] for entry in creature_rows)
        if ids != list(range(1, len(ids) + 1)):
            fail(f"creature_equip_template: CreatureID {creature_id} has non-contiguous or duplicate IDs {ids}")
        class_rules = creature_class(core, classes, creature_id)
        level = core["creature_levels"][creature_id]
        roles = emitted_roles(pool, class_rules, level, rules)
        if len(ids) != len(roles):
            fail(
                f"creature_equip_template: CreatureID {creature_id} ({class_rules.name}, level {level})"
                f" has pool IDs {ids}, but its class has {len(roles)} non-empty weapon role(s)"
            )
        for role, (mainhand, offhand, ranged) in enumerate(roles):
            _, i1, i2, i3 = next(entry for entry in creature_rows if entry[0] == role + 1)
            check_role_item(creature_id, class_rules, level, role, "ItemID1", i1, mainhand, core, "main-hand")
            check_role_item(creature_id, class_rules, level, role, "ItemID2", i2, offhand, core, "off-hand")
            check_role_item(creature_id, class_rules, level, role, "ItemID3", i3, ranged, core, "ranged")
    return sorted(by_creature)


def check_pool_deletes(
    deletes: list[tuple[int, int, int]],
    pool_creatures: list[int],
    core: dict,
    pool: PoolMirror,
    rules: dict,
    classes: dict[int, ClassRules],
    path: Path,
) -> int:
    """Assert the weapon-pool prunes, returning how many there are.

    The core resolves `creature.equipment_id = -1` by picking at random among the rows *present* for
    the entry, and the generated INSERTs only update the rows they name — so a creature whose class
    emits fewer roles than pool_size must carry exactly one DELETE covering the IDs it no longer
    writes. That DELETE has to stay inside this dataset's own ID range: `ID > n` (n = the creature's
    role count, so the rows the generator just wrote are never touched) and `ID <= pool_size` (never
    above what this dataset has ever written, so a base-dump or hand-set row above the pool survives).
    A prune for a creature outside the selected set, a duplicate, or a prune for a creature that
    emits a full set of roles are all failures.
    """
    pool_size = rules["pool_size"]

    def role_count(entry: int) -> int:
        return len(emitted_roles(pool, creature_class(core, classes, entry), core["creature_levels"][entry], rules))

    selected = set(pool_creatures)
    seen: set[int] = set()
    for entry, roles, upper in deletes:
        if entry not in selected:
            fail(f"{path}: DELETE names CreatureID {entry}, which is not one of the selected creatures")
        if entry in seen:
            fail(f"{path}: duplicate DELETE for CreatureID {entry}")
        seen.add(entry)
        expected = role_count(entry)
        if expected >= pool_size:
            fail(
                f"{path}: DELETE names CreatureID {entry}, which emits all {expected} role(s);"
                " only a creature with a shorter role set may have its pool pruned"
            )
        if roles != expected:
            fail(
                f"{path}: CreatureID {entry} DELETE says `ID` > {roles}, but the creature emits"
                f" {expected} role(s); `ID` > {expected} is the bound that spares the current rows"
            )
        if upper != pool_size:
            fail(
                f"{path}: CreatureID {entry} DELETE says `ID` <= {upper}, but pool_size is {pool_size};"
                " a prune must not reach outside the dataset's own ID range"
            )

    for entry in pool_creatures:
        expected = role_count(entry)
        if expected < pool_size and entry not in seen:
            fail(
                f"{path}: CreatureID {entry} emits {expected} of {pool_size} role(s) but has no DELETE;"
                f" its stale IDs {expected + 1}..{pool_size} would survive a replay and the core could"
                " still pick one of them"
            )

    entries = [entry for entry, _, _ in deletes]
    if entries != sorted(set(entries)):
        fail(f"{path}: DELETE statements are not strictly ascending by CreatureID")
    return len(deletes)


def check_slot_class(
    where: str,
    slot: int,
    item_id: int,
    core: dict,
    class_rules: ClassRules,
    tier: int,
    pool: PoolMirror,
    level: int,
) -> None:
    """The item must be one the class may wear in this slot.

    For the eight armour tier slots that means an item of the worn chain entry or of a fallback the
    rest of the chain offers (so a level-10 leather creature's HEAD may be cloth); for BODY, BACK
    and TABARD it means the class's AllowableClass and stat focus admit it, with no chain constraint
    (a shirt or tabard is armour subclass 0, a cloak cloth). The level band, RequiredLevel margin and
    Quality cap are part of the candidate pool, so membership covers them too.
    """
    if item_id == 0:
        return
    allowed: set[int] = set()
    for position in outfit_positions(class_rules, tier, slot):
        allowed |= pool_entries(pool.candidates(position, level, class_rules))
    if item_id not in allowed:
        fail(
            f"{where} item {item_id} ({describe_item(core, item_id)}) is not class-appropriate for a"
            f" {class_rules.name} at level {level} (armour chain {class_rules.armour_chain})"
        )


def check_outfits(rows: list[list[str]], core: dict, pool: PoolMirror, rules: dict, classes: dict[int, ClassRules]) -> tuple[list[int], list[int]]:
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
        class_rules = creature_class(core, classes, creature_id)
        level = core["creature_levels"][creature_id]
        tier = choose_armour_tier(pool, class_rules, level)
        for slot in range(len(OUTFIT_SLOTS)):
            item_id = int(row[item_offset + slot])
            where = f"mod_creature_gear_outfit: CreatureID {creature_id} ItemEntry{slot + 1}"
            check_slot_item(where, slot, item_id, core)
            check_slot_class(where, slot, item_id, core, class_rules, tier, pool, level)
            if item_id:
                slot_counts[slot] += 1
    return sorted(creature_ids), slot_counts


def slot_candidate_count(pool: PoolMirror, class_rules: ClassRules, tier: int, level: int, slot: int) -> int:
    """Size of the candidate list the generator picks an outfit slot from.

    The generator takes the first position of the slot's fallback chain that holds anything — the
    worn armour chain entry first, then the rest of the chain, and within one entry the slot's
    InventoryTypes in order (CHEST falls back to ROBE) — so this is that position's size.
    """
    for position in outfit_positions(class_rules, tier, slot):
        rows = pool.candidates(position, level, class_rules)
        if rows:
            return len(rows)
    return 0


def check_outfit_variants(
    rows: list[list[str]], outfit_rows: list[list[str]], core: dict, rules: dict, pool: PoolMirror, classes: dict[int, ClassRules]
) -> tuple[list[int], dict[int, list[int]]]:
    """Returns (CreatureIDs ascending, per varying slot the list of distinct-item counts).

    Every creature must carry exactly `armour_variants` contiguous variants, each pick must fit its
    slot and be class-appropriate, the outfit row's five varying slots must be variant 1 (so a
    creature whose variant rows are missing shows the set the module would pick first), and each
    slot's picks must be as distinct as its candidate list allows: the generator's index advances by
    one per variant, so a slot with at least `armour_variants` candidates must yield that many
    different items, and a smaller pool must still be used to its full width.
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
        class_rules = creature_class(core, classes, creature_id)
        level = core["creature_levels"][creature_id]
        tier = choose_armour_tier(pool, class_rules, level)
        for column, slot in enumerate(VARIED_OUTFIT_SLOTS, start=2):
            where = (
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} variant {variant}"
                f" {OUTFIT_VARIANT_COLUMNS[column]}"
            )
            item_id = int(row[column])
            check_slot_item(where, slot, item_id, core)
            check_slot_class(where, slot, item_id, core, class_rules, tier, pool, level)
        variants = variants_per_creature.setdefault(creature_id, [])
        if len(variants) != variant - 1:
            fail(
                f"mod_creature_gear_outfit_variant: CreatureID {creature_id} variants are not contiguous"
                f" from 1 (row {len(variants) + 1} carries variant {variant})"
            )
        variants.append([int(value) for value in row[2:]])

    item_offset = OUTFIT_COLUMNS.index("ItemEntry1")
    outfit_items = {int(row[0]): [int(value) for value in row[item_offset:]] for row in outfit_rows}
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
        class_rules = creature_class(core, classes, creature_id)
        level = core["creature_levels"][creature_id]
        tier = choose_armour_tier(pool, class_rules, level)
        for position, slot in enumerate(VARIED_OUTFIT_SLOTS):
            picks = [variant[position] for variant in variants]
            name = OUTFIT_SLOTS[slot][0]
            if picks[0] != base[slot]:
                fail(
                    f"mod_creature_gear_outfit_variant: CreatureID {creature_id} {name} variant 1 item"
                    f" {picks[0]} does not match the outfit row's {base[slot]}"
                )
            candidates = slot_candidate_count(pool, class_rules, tier, level, slot)
            distinct = len(set(picks) - {0})
            expected = min(rules["armour_variants"], candidates)
            if distinct != expected:
                fail(
                    f"mod_creature_gear_outfit_variant: CreatureID {creature_id} {name} offers {distinct}"
                    f" different item(s) across {len(variants)} variants, expected {expected}"
                    f" from a candidate list of {candidates}"
                    f" (class {class_rules.name}, level {level})"
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
    pool: PoolMirror,
    classes: dict[int, ClassRules],
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
    class_levels: set[tuple[ClassRules, int]] = set()
    for creature_id, creature_pools in pools.items():
        if any(is_boe(item_id) for pool in creature_pools for item_id in pool) or any(
            is_boe(item_id) for item_id in outfit_items[creature_id]
        ):
            boe_covered += 1
        if any(pool[1] != 0 for pool in creature_pools):
            offhand_filled += 1
        class_levels.add((creature_class(core, classes, creature_id), core["creature_levels"][creature_id]))

    return {
        "creatures": len(pools),
        "boe_covered": boe_covered,
        "offhand_filled": offhand_filled,
        "offhand_pool_available": offhand_pool_available(pool, class_levels),
    }


def offhand_pool_available(pool: PoolMirror, class_levels: set[tuple[ClassRules, int]]) -> bool:
    """True when at least one selected creature's class has an off-hand candidate in its level band.

    Mirrors gen_creature_gear's off-hand position: off-hand weapons from the class's weapon
    subclasses, plus shields for a shield class, plus holdables for every class. A class-level pair
    is asked on its own, so a pool that only exists for a class nobody selected cannot mask an empty
    one.
    """
    for class_rules, level in sorted(class_levels, key=lambda pair: (pair[0].id, pair[1])):
        if pool.candidates(offhand_position(class_rules), level, class_rules):
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
    classes = load_class_rules(rules)
    pool = PoolMirror(build_band_index(core), rules)

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
    pool_deletes = parse_pool_deletes(args.sql_dir / GENERATED_FILES[1])

    pool_creatures = check_equip_templates(equip_rows, core, pool, rules, classes)
    outfit_creatures, slot_counts = check_outfits(outfit_rows, core, pool, rules, classes)
    variant_creatures, variant_distinct = check_outfit_variants(variant_rows, outfit_rows, core, rules, pool, classes)
    spawn_rows = check_spawn_updates(spawn_entries, core, form)
    prunes = check_pool_deletes(
        pool_deletes, pool_creatures, core, pool, rules, classes, args.sql_dir / GENERATED_FILES[1]
    )
    coverage = check_drop_coverage(equip_rows, outfit_rows, variant_rows, core, rules, pool, classes)

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
            "no creature got an off-hand item although a selected class's off-hand candidate pool"
            " (class 2 off-hand weapons plus class 4 holdables, and shields for the shield classes)"
            " is non-empty for its level band"
        )

    class_counts: dict[int, int] = {}
    fallback_creatures = 0
    for creature_id in pool_creatures:
        unit_class = core["creature_classes"][creature_id]
        class_rules = creature_class(core, classes, creature_id)
        if unit_class not in classes:
            fallback_creatures += 1
        class_counts[class_rules.id] = class_counts.get(class_rules.id, 0) + 1

    print("mod-creature-gear generated SQL: OK")
    print(f"  core dumps        {args.core}/data/sql/base/db_world")
    print(f"  update chain      {args.core}/data/sql/updates/db_world")
    print(f"  generated SQL     {args.sql_dir}")
    print(f"  rules             {args.rules}")
    print()
    print(f"  {'spawn WHERE form (derived)':<38}{form.where_form}")
    print(f"  {'selected creatures':<38}{len(pool_creatures):>8}")
    print(f"  {'creature_equip_template pool rows':<38}{len(equip_rows):>8}")
    print(f"  {'  of which pool prune DELETE':<38}{prunes:>8}")
    print(f"  {'outfit rows':<38}{len(outfit_rows):>8}")
    print(f"  {'outfit variant rows':<38}{len(variant_rows):>8}")
    print(f"  {'spawn rows updated (equipment_id = -1)':<38}{spawn_rows:>8}")
    print()
    print("  class mix (creature_template.unit_class) and the gear checked for it:")
    for class_id in sorted(class_counts):
        class_rules = classes[class_id]
        chain = " ".join(f"{subclass}@{min_level}" for subclass, min_level in class_rules.armour_chain)
        print(
            f"    {class_rules.name:<14}{class_counts[class_id]:>6}"
            f"  weapons {sorted(class_rules.weapons)}"
            f"  armour {chain}"
            f"  shield {'yes' if class_rules.shield else 'no'}"
            f"  focus {class_rules.focus}"
        )
    print(f"  {'warrior fallback (no [classes.N] row)':<38}{fallback_creatures:>8}")
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
