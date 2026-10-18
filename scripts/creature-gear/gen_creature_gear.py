#!/usr/bin/env python3
"""Generate the mod-creature-gear world SQL from the pinned core's base SQL dumps.

Reads  <core>/data/sql/base/db_world/{creature_template,creature,item_template}.sql
Rules  scripts/creature-gear/gear.toml
Writes (into --out):
  creature_gear_10_schema.sql           mod_creature_gear_outfit DDL
  creature_gear_20_weapon_pools.sql     creature_equip_template pools (phase A)
  creature_gear_30_outfits.sql          mod_creature_gear_outfit rows (phases B/C)
  creature_gear_35_outfit_variants.sql  per-spawn armour variants for five slots (phases B/C)
  creature_gear_40_spawn_equipment.sql  creature.equipment_id = -1 (phase A)

Every emitted statement is re-appliable: the module's SQL is replayed by the core
updater whenever the file content changes, so INSERTs use ON DUPLICATE KEY UPDATE
and the spawn UPDATE only touches rows that still have equipment_id = 0.

The spawn UPDATE names `creature` columns as they exist *after* the core's release
migrations (<core>/data/sql/updates/db_world), because the module's SQL runs in the
updater's MODULE pass, after the RELEASED ones: derive_creature_spawn_columns reads
that tree and raises on any creature-table ALTER it does not model, so a future
migration cannot silently invalidate the generated SQL.

Python 3.12, standard library only. Output is deterministic: same inputs, same bytes.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

# Armour/weapon item classes in item_template.
CLASS_WEAPON = 2
CLASS_ARMOR = 4

# item_template.bonding = 2 is "bind on equip". The module only drops an item the creature wears
# when it is BoE (CreatureGear.Drop.RequireBindOnEquip, shipped as 1), so every candidate list is
# ordered BoE-first — see ItemPool.candidates.
BOND_ON_EQUIP = 2

# Highest item entry present in the server's Item.dbc; items above it are unusable.
MAX_ITEM_ENTRY = 56806

# Humanoid creature type in creature_template.
CREATURE_TYPE_HUMANOID = 7


def armour_position(*inventory_types: int) -> tuple[tuple[int, int], ...]:
    """One outfit position: armour (class 4) items with any of the given InventoryTypes."""
    return tuple((CLASS_ARMOR, inventory_type) for inventory_type in inventory_types)


# Weapon pool roles, cycled by variant index: variant i uses role i % len(ROLES).
# Each role is (mainhand position, offhand position, ranged position); a position is the tuple of
# (item class, InventoryType) pairs it draws candidates from. Between them they cover exactly the
# nine InventoryTypes the core accepts in a hand slot (ObjectMgr::LoadCreatureEquipTemplates):
# 13, 14, 15, 17, 21, 22, 23, 25, 26.
# The off-hand position is the only one that spans classes: off-hand weapons (class 2, 13/22) plus
# shields and holdables (class 4, 14/23), which is how a shield ends up in ItemID2.
ROLES = (
    (((CLASS_WEAPON, 17), (CLASS_WEAPON, 21)),                    # 0 = two-hander
     (), ()),
    (((CLASS_WEAPON, 13), (CLASS_WEAPON, 21), (CLASS_WEAPON, 22)),  # 1 = one-hander
     ((CLASS_WEAPON, 13), (CLASS_WEAPON, 22),                     #     + shield/holdable
      (CLASS_ARMOR, 14), (CLASS_ARMOR, 23)),
     ()),
    ((), (),                                                     # 2 = ranged
     ((CLASS_WEAPON, 15), (CLASS_WEAPON, 25), (CLASS_WEAPON, 26))),
)

# Outfit slots in SMSG_MIRRORIMAGE_DATA order; the column name is ItemEntry<N>, N = 1..11.
# Each slot lists candidate positions, tried in order: the first position with candidates
# wins, which is how CHEST falls back to ROBE when no chest exists for the level band.
OUTFIT_SLOTS = (
    ("HEAD", (armour_position(1),)),
    ("SHOULDERS", (armour_position(3),)),
    ("BODY", (armour_position(4),)),
    ("CHEST", (armour_position(5), armour_position(20))),      # chest, falling back to robe
    ("WAIST", (armour_position(6),)),
    ("LEGS", (armour_position(7),)),
    ("FEET", (armour_position(8),)),
    ("WRISTS", (armour_position(9),)),
    ("HANDS", (armour_position(10),)),
    ("BACK", (armour_position(16),)),
    ("TABARD", (armour_position(19),)),
)

OUTFIT_APPEARANCE_COLUMNS = ("Race", "Gender", "Class", "Skin", "Face", "HairStyle", "HairColor", "FacialHair")
OUTFIT_ITEM_COLUMNS = tuple(f"ItemEntry{i}" for i in range(1, len(OUTFIT_SLOTS) + 1))
OUTFIT_COLUMNS = ("CreatureID",) + OUTFIT_APPEARANCE_COLUMNS + OUTFIT_ITEM_COLUMNS

# The five outfit slots that vary per spawn: helm, body armour, pants, boots, gloves. The module
# picks one variant per spawn point (spawn id % variant count) and uses that same pick for the
# creature's drop, so the armour a corpse yields is the armour its spawn point shows. The other six
# slots stay on `mod_creature_gear_outfit`, so an entry keeps its shoulders, shirt, belt, bracers,
# cloak and tabard across spawns.
VARIED_OUTFIT_SLOT_NAMES = ("HEAD", "CHEST", "LEGS", "FEET", "HANDS")
VARIED_OUTFIT_SLOTS = tuple(
    index for index, (name, _) in enumerate(OUTFIT_SLOTS) if name in VARIED_OUTFIT_SLOT_NAMES
)
OUTFIT_VARIANT_COLUMNS = ("CreatureID", "Variant") + tuple(OUTFIT_SLOTS[index][0].capitalize() for index in VARIED_OUTFIT_SLOTS)

EQUIP_TEMPLATE_COLUMNS = ("CreatureID", "ID", "ItemID1", "ItemID2", "ItemID3")

HEADER_MARKER = "-- Generated by scripts/creature-gear/gen_creature_gear.py — do not edit by hand.\n"

# Tuples per INSERT statement; keeps the generated file reviewable.
ROWS_PER_STATEMENT = 500

DEFAULT_GEAR_TOML = Path(__file__).resolve().parent / "gear.toml"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO_ROOT / "modules" / "mod-creature-gear" / "data" / "sql" / "world" / "base"


# --------------------------------------------------------------------------- parsing


def parse_columns(text: str) -> list[str]:
    """Column names in CREATE TABLE order, read from the dump's own DDL."""
    match = re.search(r"CREATE TABLE `[^`]+` \((.*?)\n\) ENGINE", text, re.S)
    if not match:
        raise SystemExit("FAIL: no CREATE TABLE block found in dump")
    columns = []
    for line in match.group(1).splitlines():
        column = re.match(r"\s*`([^`]+)`", line)
        if column:
            columns.append(column.group(1))
    return columns


def parse_dump(path: Path) -> tuple[list[str], list[list[str]]]:
    """Parse a mysqldump-style single-table dump into (columns, rows).

    One tuple per line; strings single-quoted with \\' \\\\ \\n escapes, NULL bare.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    columns = parse_columns(text)
    width = len(columns)
    rows: list[list[str]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("("):
            continue
        if line.endswith(",") or line.endswith(";"):
            line = line[:-1]
        fields = next(csv.reader([line[1:-1]], quotechar="'", escapechar="\\", doublequote=False))
        if len(fields) != width:
            raise SystemExit(f"FAIL: {path.name} line {len(rows) + 1}: {len(fields)} fields, expected {width}")
        rows.append(fields)
    if not rows:
        raise SystemExit(f"FAIL: no data rows found in {path}")
    return columns, rows


def column_index(columns: list[str], name: str) -> int:
    try:
        return columns.index(name)
    except ValueError:
        raise SystemExit(f"FAIL: column `{name}` not found in dump DDL") from None


# --------------------------------------------------------------------------- schema derivation

# The core updater applies <core>/data/sql/updates/db_world/*.sql in filename order on top of the
# base dumps, and the module's SQL runs after that pass, so a spawn statement may only name columns
# left by the *last* release migration — never the base dump's. The walker below derives that
# layout instead of assuming today's answer. 2026_06_16_00.sql is the release that rewrites
# `creature`:
#
#   ALTER TABLE `creature` CHANGE COLUMN `id1` `id` int unsigned NOT NULL DEFAULT 0 ...;
#   ALTER TABLE `creature` DROP COLUMN `id2`, DROP COLUMN `id3`;
#
# and copies those id2/id3 entries into `creature_multispawn`, so on a migrated database the phase-A
# statement must read `WHERE id = <entry> AND equipment_id = 0`, with no id2 filter; on an update
# tree without such a migration the base form (`id1 = <entry> AND id2 = 0`) still stands. Anything
# else touching the table raises, naming the file and statement, so a future migration cannot
# silently invalidate the generated SQL.

# The base-dump columns this walker models. The spawn UPDATE needs a row's creature entry, its
# optional second/third entries and its equipment slot — nothing else, so a migration that renames
# or drops any other `creature` column gets a human look rather than a guess.
BASE_SPAWN_COLUMNS = ("id1", "id2", "id3", "equipment_id")

# `ALTER TABLE `creature` <operations>;` (optionally schema-qualified, as `acore_world`.`creature`)
# — the only statements that can change that layout.
CREATURE_ALTER_RE = re.compile(
    r"ALTER\s+TABLE\s+(?:(?:`[^`]+`|\w+)\s*\.\s*)?`?creature`?\s+([^;]*);", re.IGNORECASE | re.DOTALL
)

# Operations that cannot change the modelled columns: index/key/constraint definitions never appear
# in the emitted statements (`DROP INDEX idx_id` / `ADD INDEX idx_id (id)` in 2026_06_16_00.sql).
ALTER_NOOP_OP_RE = re.compile(
    r"(?:ADD|DROP)\s+(?:(?:UNIQUE|FULLTEXT|SPATIAL)\s+)?(?:INDEX|KEY)\s"
    r"|(?:ADD|DROP)\s+PRIMARY\s+KEY"
    r"|ADD\s+CONSTRAINT\s"
    r"|DROP\s+(?:FOREIGN\s+KEY|CONSTRAINT|CHECK)\s",
    re.IGNORECASE,
)
# Renames (CHANGE COLUMN old new, RENAME COLUMN old TO new), name-preserving redefinitions
# (MODIFY COLUMN) and drops. Every other operation is unmodelled and raises.
ALTER_RENAME_OP_RE = re.compile(r"CHANGE(?:\s+COLUMN)?\s+`([^`]+)`\s+`([^`]+)`", re.IGNORECASE)
ALTER_RENAME_COLUMN_OP_RE = re.compile(r"RENAME\s+COLUMN\s+`([^`]+)`\s+TO\s+`([^`]+)`", re.IGNORECASE)
ALTER_MODIFY_OP_RE = re.compile(r"MODIFY(?:\s+COLUMN)?\s+`([^`]+)`", re.IGNORECASE)
ALTER_DROP_OP_RE = re.compile(r"DROP(?:\s+COLUMN)?\s+`([^`]+)`", re.IGNORECASE)


@dataclass(frozen=True)
class SpawnColumnForm:
    """The column names and row filter a phase-A spawn UPDATE must use on a migrated database.

    `entry`, `equipment` and `secondary` are the post-migration names of the base dumps' `id1`,
    `equipment_id` and `id2`. `secondary` is None once the update chain has dropped that column: the
    extra entries then live in `creature_multispawn` and the statement carries no secondary filter.
    """

    entry: str
    equipment: str
    secondary: str | None

    @property
    def columns(self) -> tuple[str, ...]:
        """Every `creature` column the emitted statement names."""
        return tuple(name for name in (self.entry, self.secondary, self.equipment) if name)

    @property
    def clause_tail(self) -> str:
        """The WHERE clause after the entry value: `equipment_id = 0`, plus a surviving filter."""
        return " AND ".join(f"`{name}` = 0" for name in (self.secondary, self.equipment) if name)

    @property
    def where_form(self) -> str:
        """The WHERE clause with a placeholder, for logs and generated-file headers."""
        return f"`{self.entry}` = <entry> AND {self.clause_tail}"

    def where(self, entry: int) -> str:
        return f"`{self.entry}` = {entry} AND {self.clause_tail}"


def spawn_update_statement(entry: int, form: SpawnColumnForm) -> str:
    """One idempotent phase-A statement: only equipment-less spawns of that entry are touched."""
    return f"UPDATE `creature` SET `{form.equipment}` = -1 WHERE {form.where(entry)};"


def split_alter_operations(operations: str) -> list[str]:
    """Split an ALTER TABLE clause list on top-level commas, ignoring commas in quotes/parens."""
    parts: list[str] = []
    start = 0
    depth = 0
    quote = ""
    for index, char in enumerate(operations):
        if quote:
            if char == quote:
                quote = ""
        elif char in "'\"`":
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(operations[start:index].strip())
            start = index + 1
    parts.append(operations[start:].strip())
    return [part for part in parts if part]


def tracked_column(tracked: dict[str, str | None], name: str) -> str | None:
    """The modelled column currently called `name`, or None when the walker does not model it."""
    return next((key for key, current in tracked.items() if current == name), None)


def apply_creature_alter_operations(tracked: dict[str, str | None], operations: str, source: str) -> None:
    """Apply one `ALTER TABLE `creature`` statement's operations to the tracked column names.

    `source` names the file and the statement for the failure messages: a migration this walker does
    not model must stop generation loudly, not silently emit statements against a changed layout.
    """
    for operation in split_alter_operations(operations):
        if ALTER_NOOP_OP_RE.match(operation):
            continue

        rename = ALTER_RENAME_OP_RE.match(operation) or ALTER_RENAME_COLUMN_OP_RE.match(operation)
        if rename:
            old, new = rename.group(1), rename.group(2)
            key = tracked_column(tracked, old)
            if key is None:
                raise SystemExit(f"FAIL: {source} renames `{old}`, which derive_creature_spawn_columns does not model")
            if new in tracked.values():
                raise SystemExit(f"FAIL: {source} renames `{old}` to `{new}`, which already exists")
            tracked[key] = new
            continue

        modify = ALTER_MODIFY_OP_RE.match(operation)
        if modify:
            # MODIFY COLUMN cannot rename, so a modelled column keeps its name.
            if tracked_column(tracked, modify.group(1)) is None:
                raise SystemExit(f"FAIL: {source} modifies `{modify.group(1)}`, which derive_creature_spawn_columns does not model")
            continue

        drop = ALTER_DROP_OP_RE.match(operation)
        if drop:
            key = tracked_column(tracked, drop.group(1))
            if key is None:
                raise SystemExit(f"FAIL: {source} drops `{drop.group(1)}`, which derive_creature_spawn_columns does not model")
            tracked[key] = None
            continue

        raise SystemExit(f"FAIL: {source} operation {operation!r} is not modelled by derive_creature_spawn_columns")


def derive_creature_spawn_columns(core: Path, base_columns: list[str]) -> SpawnColumnForm:
    """Derive the phase-A spawn UPDATE's column form from the base dump plus the release migrations.

    `base_columns` are the `creature` columns of the base dump, the schema the update chain starts
    from; see the section comment above for why the chain (read in the updater's own filename order
    from <core>/data/sql/updates/db_world) decides the emitted form rather than the dump does.
    """
    updates = core / "data" / "sql" / "updates" / "db_world"
    if not updates.is_dir():
        raise SystemExit(f"FAIL: {updates} is not a directory; cannot derive the creature spawn columns")

    missing = [name for name in BASE_SPAWN_COLUMNS if name not in base_columns]
    if missing:
        raise SystemExit(f"FAIL: base creature dump has no {', '.join(missing)} column")

    tracked: dict[str, str | None] = {name: name for name in BASE_SPAWN_COLUMNS}
    for path in sorted(updates.glob("*.sql")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in CREATURE_ALTER_RE.finditer(text):
            operations = match.group(1).strip()
            apply_creature_alter_operations(tracked, operations, f"{path}: ALTER TABLE `creature` {operations};")

    entry = tracked["id1"]
    equipment = tracked["equipment_id"]
    if entry is None or equipment is None:
        raise SystemExit(
            f"FAIL: the update chain drops `id1`/`equipment_id` (id1 -> {entry}, equipment_id -> {equipment});"
            " the phase-A spawn UPDATE cannot be emitted"
        )
    secondary = tracked["id2"]
    if secondary is None and tracked["id3"] is not None:
        raise SystemExit(
            "FAIL: the update chain drops `id2` while `id3` survives, so the base form's secondary"
            " filter cannot be carried over; extend derive_creature_spawn_columns"
        )
    return SpawnColumnForm(entry=entry, equipment=equipment, secondary=secondary)


# --------------------------------------------------------------------------- rules


def load_rules(path: Path) -> dict:
    with path.open("rb") as fh:
        rules = tomllib.load(fh)
    defaults = {
        "min_level": 1,
        "max_level": 80,
        "skip_nonzero_npcflag": True,
        "exclude_entries": [],
        "max_creatures": 0,
        "pool_size": 3,
        "armour_variants": 4,
        "item_level_window": 8,
        "required_level_margin": 3,
        "quality_max": 2,
        # Consumed by check_creature_gear.py, not by this generator; accepted here so the two
        # scripts can share one rules file.
        "min_boe_coverage": 0.70,
    }
    for key, value in defaults.items():
        rules.setdefault(key, value)
    unknown = set(rules) - set(defaults)
    if unknown:
        raise SystemExit(f"FAIL: unknown keys in {path}: {', '.join(sorted(unknown))}")
    if rules["max_creatures"] < 0:
        raise SystemExit("FAIL: max_creatures must be 0 or more (0 = every creature the rules admit)")
    if not 1 <= rules["pool_size"] <= 3:
        raise SystemExit("FAIL: pool_size must be 1..3 (the core's random picker indexes IDs 1..n)")
    if not 1 <= rules["armour_variants"] <= 255:
        raise SystemExit("FAIL: armour_variants must be 1..255 (the variant column is tinyint unsigned)")
    return rules


# --------------------------------------------------------------------------- selection


def index_creatures(columns: list[str], rows: list[list[str]]) -> dict[int, dict]:
    """entry -> {minlevel, maxlevel, npcflag, unit_class}."""
    i_entry = column_index(columns, "entry")
    i_npc = column_index(columns, "npcflag")
    i_type = column_index(columns, "type")
    i_min = column_index(columns, "minlevel")
    i_max = column_index(columns, "maxlevel")
    i_class = column_index(columns, "unit_class")
    out = {}
    for row in rows:
        out[int(row[i_entry])] = {
            "minlevel": int(row[i_min]),
            "maxlevel": int(row[i_max]),
            "npcflag": int(row[i_npc]),
            "type": int(row[i_type]),
            "unit_class": int(row[i_class]),
        }
    return out


def index_spawns(columns: list[str], rows: list[list[str]], form: SpawnColumnForm) -> dict[int, dict]:
    """entry -> {'spawn_rows': n, 'all_gearless': bool} over the rows the UPDATE will touch.

    The dump is the pre-migration schema, so its columns keep the base names; which of its rows the
    emitted statement reaches is the derived form's business. While the secondary-id column still
    exists only the `id2 = 0` rows of an entry are touched; once the chain has dropped it the
    statement reaches every row of the entry (the extra entries live in `creature_multispawn`).
    `spawn_rows` counts the equipment-less ones — exactly what the statement changes — and
    `all_gearless` says whether any touched row already carries hand-set equipment.
    """
    i_id1 = column_index(columns, "id1")
    i_id2 = column_index(columns, "id2")
    i_equip = column_index(columns, "equipment_id")
    out: dict[int, dict] = {}
    for row in rows:
        if form.secondary is not None and int(row[i_id2]) != 0:
            continue
        entry = int(row[i_id1])
        spawn = out.setdefault(entry, {"spawn_rows": 0, "all_gearless": True})
        if int(row[i_equip]) == 0:
            spawn["spawn_rows"] += 1
        else:
            spawn["all_gearless"] = False
    return out


def select_entries(templates: dict[int, dict], spawns: dict[int, dict], rules: dict) -> list[int]:
    excluded = {int(entry) for entry in rules["exclude_entries"]}
    selected = []
    for entry, template in templates.items():
        if template["type"] != CREATURE_TYPE_HUMANOID:
            continue
        if template["minlevel"] < rules["min_level"] or template["maxlevel"] > rules["max_level"]:
            continue
        if rules["skip_nonzero_npcflag"] and template["npcflag"] != 0:
            continue
        if entry in excluded:
            continue
        spawn = spawns.get(entry)
        if not spawn or spawn["spawn_rows"] == 0:
            continue
        # Never gear an entry whose spawns the UPDATE would find carrying hand-set equipment.
        if not spawn["all_gearless"]:
            continue
        selected.append(entry)
    selected.sort()
    cap = rules["max_creatures"]
    return selected[:cap] if cap else selected


def index_items(columns: list[str], rows: list[list[str]], classes: tuple[int, ...]) -> dict[tuple[int, int], list[tuple[int, int, int, int, int]]]:
    """(item class, InventoryType) -> list of (ItemLevel, entry, RequiredLevel, Quality, bonding).

    sorted by (ItemLevel, entry) so ItemPool._band can bisect the level band; the bind-on-equip
    ordering the drop half needs is applied later, in ItemPool.candidates.
    Items above MAX_ITEM_ENTRY are dropped: they have no Item.dbc row on the server.
    """
    i_entry = column_index(columns, "entry")
    i_class = column_index(columns, "class")
    i_inv = column_index(columns, "InventoryType")
    i_ilevel = column_index(columns, "ItemLevel")
    i_req = column_index(columns, "RequiredLevel")
    i_quality = column_index(columns, "Quality")
    i_bonding = column_index(columns, "bonding")
    out: dict[tuple[int, int], list[tuple[int, int, int, int, int]]] = {}
    for row in rows:
        item_class = int(row[i_class])
        if item_class not in classes:
            continue
        entry = int(row[i_entry])
        if entry > MAX_ITEM_ENTRY:
            continue
        key = (item_class, int(row[i_inv]))
        out.setdefault(key, []).append((
            int(row[i_ilevel]),
            entry,
            int(row[i_req]),
            int(row[i_quality]),
            int(row[i_bonding]),
        ))
    for items in out.values():
        items.sort(key=lambda item: (item[0], item[1]))
    return out


class ItemPool:
    """Candidate pools per position, cached per (position, creature level).

    A position is the tuple of (item class, InventoryType) pairs a hand or outfit slot may draw
    from, so one position can span classes (the off-hand one does).
    """

    def __init__(self, by_class_inventory_type: dict[tuple[int, int], list[tuple[int, int, int, int, int]]], rules: dict):
        self._by_key = by_class_inventory_type
        self._window = rules["item_level_window"]
        self._margin = rules["required_level_margin"]
        self._quality_max = rules["quality_max"]
        self._cache: dict[tuple[tuple[tuple[int, int], ...], int], list[tuple[int, int]]] = {}
        self._bands: dict[int, dict[tuple[int, int], list[tuple[int, int, int, int, int]]]] = {}

    def candidates(self, position: tuple[tuple[int, int], ...], level: int) -> list[tuple[int, int]]:
        key = (position, level)
        cached = self._cache.get(key)
        if cached is not None:
            return cached

        band = self._band(level)
        items: list[tuple[int, int, int, int, int]] = []
        for class_inventory_type in position:
            items.extend(band.get(class_inventory_type, ()))
        # Bind-on-equip first: the module only drops a worn item with item_template.bonding = 2
        # (CreatureGear.Drop.RequireBindOnEquip, shipped as 1). Ordering by (bonding != 2, ItemLevel,
        # entry) puts every BoE candidate ahead of the non-BoE ones while keeping both groups in the
        # usual item-level/entry order, so a non-BoE item of the band can never crowd out a BoE one.
        items.sort(key=lambda item: (item[4] != BOND_ON_EQUIP, item[0], item[1]))
        pool = [(item_level, entry) for item_level, entry, _, _, _ in items]

        self._cache[key] = pool
        return pool

    def _band(self, level: int) -> dict[tuple[int, int], list[tuple[int, int, int, int, int]]]:
        cached = self._bands.get(level)
        if cached is not None:
            return cached
        low = level
        high = level + self._window
        required_max = level + self._margin
        band: dict[tuple[int, int], list[tuple[int, int, int, int, int]]] = {}
        for key, items in self._by_key.items():
            start = bisect.bisect_left(items, (low, 0))
            end = bisect.bisect_left(items, (high + 1, 0))
            picked = [item for item in items[start:end] if item[2] <= required_max and item[3] <= self._quality_max]
            if picked:
                band[key] = picked
        self._bands[level] = band
        return band

    @staticmethod
    def pick(candidates: list[tuple[int, int]], entry: int, variant: int) -> int:
        """(entry + variant) % len(candidates) into the sorted candidate list, 0 when empty."""
        if not candidates:
            return 0
        return candidates[(entry + variant) % len(candidates)][1]


def pick_outfit_slot(
    pool: ItemPool,
    positions: tuple[tuple[int, int], ...],
    level: int,
    entry: int,
    index: int,
) -> int:
    """One outfit slot: the first position with candidates wins (CHEST falls back to ROBE), then the
    pick rule (entry + index) % len(candidates). Consecutive `index` values therefore pick distinct
    items for as long as the candidate list is at least that long, which is what makes an entry's
    armour variants differ from each other."""
    for position in positions:
        candidates = pool.candidates(position, level)
        if candidates:
            return ItemPool.pick(candidates, entry, index)
    return 0


# --------------------------------------------------------------------------- emit


def write_insert(fh, table: str, columns: tuple[str, ...], rows: list[tuple[int, ...]], update_columns: tuple[str, ...]) -> None:
    column_list = ", ".join(f"`{column}`" for column in columns)
    for chunk_start in range(0, len(rows), ROWS_PER_STATEMENT):
        chunk = rows[chunk_start : chunk_start + ROWS_PER_STATEMENT]
        fh.write(f"INSERT INTO `{table}` ({column_list}) VALUES\n")
        for index, row in enumerate(chunk):
            separator = "," if index + 1 < len(chunk) else ""
            fh.write("(" + ", ".join(str(value) for value in row) + ")" + separator + "\n")
        if update_columns:
            assignments = ",\n    ".join(f"`{column}` = VALUES(`{column}`)" for column in update_columns)
            fh.write(f"ON DUPLICATE KEY UPDATE\n    {assignments};\n\n")
        else:
            fh.write(";\n\n")


def header(lines: list[str]) -> str:
    return HEADER_MARKER + "".join(f"-- {line}\n" for line in lines) + "\n"


def build_schema(summary: list[str]) -> str:
    lines = summary + [
        "Sources: azerothcore-wotlk data/sql/base/db_world/{creature_template,creature,item_template}.sql",
        "Stores the eleven-slot outfit (SMSG_MIRRORIMAGE_DATA slot order) used by phases B and C.",
        "The appearance columns stay 0: the module derives Race from the creature's baked display",
        "and Class from creature_template.unit_class at runtime.",
    ]
    out = header(lines)
    out += "CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit` (\n"
    out += "  `CreatureID` int unsigned NOT NULL,\n"
    for column in OUTFIT_APPEARANCE_COLUMNS:
        out += f"  `{column}` tinyint unsigned NOT NULL DEFAULT '0',\n"
    for column in OUTFIT_ITEM_COLUMNS:
        out += f"  `{column}` int unsigned NOT NULL DEFAULT '0',\n"
    out += "  PRIMARY KEY (`CreatureID`)\n"
    out += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n"
    return out


class _Buffer:
    """Minimal file-like wrapper so write_insert can target a string buffer."""

    def __init__(self) -> None:
        self._parts: list[str] = []

    def write(self, text: str) -> None:
        self._parts.append(text)

    def getvalue(self) -> str:
        return "".join(self._parts)


def build_pools(pool_rows: list[tuple[int, ...]], summary: list[str]) -> str:
    out = header(summary + ["Phase A: three-variant weapon pool per creature, resolved by creature.equipment_id = -1."])
    buffer = _Buffer()
    write_insert(buffer, "creature_equip_template", EQUIP_TEMPLATE_COLUMNS, pool_rows,
                 ("ItemID1", "ItemID2", "ItemID3"))
    return out + buffer.getvalue()


def build_outfits(outfit_rows: list[tuple[int, ...]], summary: list[str]) -> str:
    out = header(summary + [
        "Phases B and C: eleven armour slots per creature in mirror-image packet order.",
        "CHEST falls back to ROBE when no chest candidate exists for the level band.",
    ])
    buffer = _Buffer()
    write_insert(buffer, "mod_creature_gear_outfit", OUTFIT_COLUMNS, outfit_rows,
                 tuple(column for column in OUTFIT_COLUMNS if column != "CreatureID"))
    return out + buffer.getvalue()


def build_variants(variant_rows: list[tuple[int, ...]], summary: list[str]) -> str:
    out = header(summary + [
        "Per-spawn armour variants. The module picks one variant per spawn point (spawn id % variant",
        "count) and uses the same pick when the creature drops its gear, so a corpse yields the armour",
        "its spawn point showed. The six slots not listed here stay on `mod_creature_gear_outfit`,",
        "which is also the fallback for a creature without variant rows.",
    ])
    out += "CREATE TABLE IF NOT EXISTS `mod_creature_gear_outfit_variant` (\n"
    out += "  `CreatureID` int unsigned NOT NULL,\n"
    out += "  `Variant` tinyint unsigned NOT NULL,\n"
    for column in OUTFIT_VARIANT_COLUMNS[2:]:
        out += f"  `{column}` int unsigned NOT NULL DEFAULT '0',\n"
    out += "  PRIMARY KEY (`CreatureID`, `Variant`)\n"
    out += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;\n\n"
    buffer = _Buffer()
    write_insert(buffer, "mod_creature_gear_outfit_variant", OUTFIT_VARIANT_COLUMNS, variant_rows,
                 OUTFIT_VARIANT_COLUMNS[2:])
    return out + buffer.getvalue()


def build_spawns(entries: list[int], summary: list[str], form: SpawnColumnForm) -> str:
    if form.secondary is None:
        columns_note = [
            "The module's SQL runs in the updater's MODULE pass, after the core's own updates:",
            "db_world update 2026_06_16_00 renames the base dumps' `id1` to `id` and moves `id2`/`id3`",
            "into `creature_multispawn`, so the statement has no secondary-id filter. Both the column",
            "names and that filter are derived from data/sql/updates/db_world in filename order.",
        ]
    else:
        columns_note = [
            "The module's SQL runs in the updater's MODULE pass, after the core's own updates, but no",
            "update in data/sql/updates/db_world rewrites `creature`, so the base dumps' column form",
            f"still stands: {form.where_form}.",
        ]
    out = header(summary + ["Phase A: point equipment-less spawns at the generated pool; never overwrite gear."] + columns_note)
    for entry in entries:
        out += spawn_update_statement(entry, form) + "\n"
    return out


# --------------------------------------------------------------------------- main


def generate(core: Path, gear_toml: Path, out_dir: Path) -> dict:
    rules = load_rules(gear_toml)

    template_columns, template_rows = parse_dump(core / "data" / "sql" / "base" / "db_world" / "creature_template.sql")
    creature_columns, creature_rows = parse_dump(core / "data" / "sql" / "base" / "db_world" / "creature.sql")
    item_columns, item_rows = parse_dump(core / "data" / "sql" / "base" / "db_world" / "item_template.sql")

    form = derive_creature_spawn_columns(core, creature_columns)

    templates = index_creatures(template_columns, template_rows)
    spawns = index_spawns(creature_columns, creature_rows, form)
    selected = select_entries(templates, spawns, rules)

    pool = ItemPool(index_items(item_columns, item_rows, (CLASS_WEAPON, CLASS_ARMOR)), rules)

    pool_rows: list[tuple[int, ...]] = []
    outfit_rows: list[tuple[int, ...]] = []
    variant_rows: list[tuple[int, ...]] = []
    spawn_row_count = 0

    for entry in selected:
        level = templates[entry]["maxlevel"]
        spawn_row_count += spawns[entry]["spawn_rows"]

        for variant in range(rules["pool_size"]):
            mainhand, offhand, ranged = ROLES[variant % len(ROLES)]
            pool_rows.append((
                entry,
                variant + 1,
                ItemPool.pick(pool.candidates(mainhand, level), entry, variant) if mainhand else 0,
                ItemPool.pick(pool.candidates(offhand, level), entry, variant) if offhand else 0,
                ItemPool.pick(pool.candidates(ranged, level), entry, variant) if ranged else 0,
            ))

        # The five varying slots are picked once per variant; the outfit row carries variant 1, so a
        # creature whose variant rows are missing still shows the set the module would pick first.
        picks: dict[int, list[int]] = {}
        for slot in VARIED_OUTFIT_SLOTS:
            positions = OUTFIT_SLOTS[slot][1]
            picks[slot] = [
                pick_outfit_slot(pool, positions, level, entry, slot + variant)
                for variant in range(1, rules["armour_variants"] + 1)
            ]

        outfit = [entry] + [0] * len(OUTFIT_APPEARANCE_COLUMNS)
        for slot, (_, positions) in enumerate(OUTFIT_SLOTS):
            if slot in picks:
                outfit.append(picks[slot][0])
            else:
                outfit.append(pick_outfit_slot(pool, positions, level, entry, slot))
        outfit_rows.append(tuple(outfit))

        for variant in range(1, rules["armour_variants"] + 1):
            variant_rows.append((entry, variant) + tuple(picks[slot][variant - 1] for slot in VARIED_OUTFIT_SLOTS))

    summary = [
        f"Counts: {len(selected)} creatures, {len(pool_rows)} creature_equip_template pool rows,"
        f" {len(outfit_rows)} outfit rows, {len(variant_rows)} outfit variant rows,"
        f" {spawn_row_count} spawn rows updated.",
        "Rule: creature_level = creature_template.maxlevel;"
        f" ItemLevel in [creature_level, creature_level + {rules['item_level_window']}],"
        f" RequiredLevel <= creature_level + {rules['required_level_margin']},"
        f" Quality <= {rules['quality_max']};"
        " pick index (entry + variant) % len(candidates) for weapon pool variants,"
        " (entry + slot index) % len(candidates) for the six fixed outfit slots and"
        " (entry + slot index + variant) % len(candidates) for the five varying ones.",
        "Rule: candidate positions are two-hander {17, 21}, main hand {13, 21, 22} and ranged"
        " {15, 25, 26} from item class 2; the off-hand position mixes classes — class 2 with {13, 22}"
        " plus class 4 with {14 shield, 23 holdable} — so shields and off-hand frills can fill ItemID2.",
        "Rule: every candidate list is ordered bind-on-equip first — (bonding != 2, ItemLevel, entry) —"
        " so the drop half (CreatureGear.Drop.RequireBindOnEquip = 1) always has a droppable item.",
    ]

    files = {
        "creature_gear_10_schema.sql": build_schema(summary),
        "creature_gear_20_weapon_pools.sql": build_pools(pool_rows, summary),
        "creature_gear_30_outfits.sql": build_outfits(outfit_rows, summary),
        "creature_gear_35_outfit_variants.sql": build_variants(variant_rows, summary),
        "creature_gear_40_spawn_equipment.sql": build_spawns(selected, summary, form),
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    sizes = {}
    for name, content in files.items():
        path = out_dir / name
        path.write_text(content, encoding="utf-8", newline="\n")
        sizes[name] = len(content.encode("utf-8"))

    return {
        "creatures": len(selected),
        "pool_rows": len(pool_rows),
        "outfit_rows": len(outfit_rows),
        "variant_rows": len(variant_rows),
        "spawn_rows": spawn_row_count,
        "spawn_where_form": form.where_form,
        "sizes": sizes,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--core", type=Path, required=True, help="path to the pinned azerothcore-wotlk checkout")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"module SQL output directory (default {DEFAULT_OUT})")
    parser.add_argument("--rules", type=Path, default=DEFAULT_GEAR_TOML, help=f"rules file (default {DEFAULT_GEAR_TOML})")
    args = parser.parse_args(argv)

    if not (args.core / "data" / "sql" / "base" / "db_world").is_dir():
        raise SystemExit(f"FAIL: {args.core} does not look like an azerothcore-wotlk checkout")

    result = generate(args.core, args.rules, args.out)

    print(f"creatures selected        {result['creatures']}")
    print(f"weapon pool rows          {result['pool_rows']}")
    print(f"outfit rows               {result['outfit_rows']}")
    print(f"outfit variant rows       {result['variant_rows']}")
    print(f"spawn rows updated        {result['spawn_rows']}")
    print(f"spawn column form         {result['spawn_where_form']}")
    for name, size in result["sizes"].items():
        print(f"{name:<38} {size:>8} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
