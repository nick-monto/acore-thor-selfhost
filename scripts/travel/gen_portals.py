#!/usr/bin/env python3
"""Generate the custom two-way city portal network for the solo realm.

The patch is pure data: sixteen ``gameobject`` spawns of stock
``gameobject_template`` rows with ``type = 22`` (GAMEOBJECT_TYPE_SPELLCASTER).
The gameobject casts its ``Data0`` spell on the player who uses it, and that
spell's destination is the ``spell_target_position`` row in the same core.
Nothing is added to the client or to the template tables: the eight portal
templates already exist, and seven of them are also spawned in the base
``gameobject`` dump (the Ironforge template, 176497, is not, but its spell
44089 has a valid target row, which is the only thing the mechanism needs).

Each capital hosts two portals, always within one network faction (Ironforge's
template is the one neutral exception) and always to the other continent:

    Alliance, Eastern Kingdoms : Stormwind, Ironforge   -> Darnassus, Exodar
    Alliance, Kalimdor         : Darnassus, Exodar      -> Stormwind, Ironforge
    Horde, Kalimdor            : Orgrimmar, Thunder Bluff -> Undercity, Silvermoon
    Horde, Eastern Kingdoms    : Undercity, Silvermoon  -> Orgrimmar, Thunder Bluff

A portal is placed in its host city at that city's own teleport arrival point
(the host spell's ``spell_target_position`` row).  The first of the two (the
lower destination template entry) sits five yards along the row's orientation;
the second sits another five yards to the side, at ``orientation + pi/2``, so
the two objects do not overlap.  The portal's template and spell are always
the destination city's, so the two objects in a capital are the ways out of
it.

Run against a checkout of the pinned core (see ``pins.toml``)::

    python3 scripts/travel/gen_portals.py --core ../azerothcore-wotlk

``--check`` regenerates in memory and fails if the committed SQL differs.
"""

from __future__ import annotations

import argparse
import difflib
import math
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Tuning
# ---------------------------------------------------------------------------

# Spawn guids: a private range above the base gameobject dump (max guid in the
# pinned dump is below 6,000,000).  The file deletes exactly the guids it
# inserts: after it applies, MAX(guid) + 1 is the server's allocator seed, so a
# range delete would cover guids the server later hands out to other objects.
GUID_BASE = 9800000
GUID_MAX = 9800099

# Five yards from the host arrival point: enough separation from the landing
# point and from the stock portal object already on the pad.
PORTAL_OFFSET = 5.0

# Non-position columns copied from the stock Shattrath portal spawns.
SPAWN_MASK = 1
PHASE_MASK = 1
SPAWNTIMESECS = 181
ANIMPROGRESS = 100
STATE = 1
SCRIPT_NAME = ""
VERIFIED_BUILD = 0

# gameobject_template_addon.faction values.
ALLIANCE_FACTION = 1732
HORDE_FACTION = 1735

# Ironforge's template is the one portal template with no faction (0/neutral):
# it is allowed as a destination but nowhere else.
NEUTRAL_DESTINATION = "Ironforge"
NEUTRAL_TEMPLATE = 176497

# Stock "Portal to Blasted Lands" objects sit on the same capital pads; they
# are independent proof that the pad exists and is reachable.  Stormwind's
# stock object is 100 yd above the pad (the pad is under the city), so it is
# checked for x/y only, with a wider z bound.
STOCK_PORTAL_ENTRIES = (195141, 195142)
STOCK_PORTAL_RADIUS = 30.0
STOCK_PORTAL_DZ = 5.0
STOCK_PORTAL_DZ_EXCEPTIONS = {"Stormwind": 150.0}

CITIES = {
    "Stormwind": {
        "template": 183325,
        "spell": 17334,
        "zone": 1519,
        "map": 0,
    },
    "Ironforge": {
        "template": 176497,
        "spell": 44089,
        "zone": 1537,
        "map": 0,
    },
    "Darnassus": {
        "template": 183317,
        "spell": 17608,
        "zone": 1657,
        "map": 1,
    },
    "Exodar": {
        "template": 183321,
        "spell": 32268,
        "zone": 3557,
        "map": 530,
    },
    "Orgrimmar": {
        "template": 183323,
        "spell": 17609,
        "zone": 1637,
        "map": 1,
    },
    "Undercity": {
        "template": 183327,
        "spell": 17611,
        "zone": 1497,
        "map": 0,
    },
    "Thunder Bluff": {
        "template": 183326,
        "spell": 17610,
        "zone": 1638,
        "map": 1,
    },
    "Silvermoon": {
        "template": 183324,
        "spell": 32270,
        "zone": 3487,
        "map": 530,
    },
}

# host -> the two destinations on the other continent.
NETWORK = {
    "Stormwind": ("Darnassus", "Exodar"),
    "Ironforge": ("Darnassus", "Exodar"),
    "Darnassus": ("Stormwind", "Ironforge"),
    "Exodar": ("Stormwind", "Ironforge"),
    "Orgrimmar": ("Undercity", "Silvermoon"),
    "Thunder Bluff": ("Undercity", "Silvermoon"),
    "Undercity": ("Orgrimmar", "Thunder Bluff"),
    "Silvermoon": ("Orgrimmar", "Thunder Bluff"),
}

INSERT_COLUMNS = [
    "guid",
    "id",
    "map",
    "zoneId",
    "areaId",
    "spawnMask",
    "phaseMask",
    "position_x",
    "position_y",
    "position_z",
    "orientation",
    "rotation0",
    "rotation1",
    "rotation2",
    "rotation3",
    "spawntimesecs",
    "animprogress",
    "state",
    "ScriptName",
    "VerifiedBuild",
    "Comment",
]

OUTPUT_NAME = "travel_portals_10_spawns.sql"


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


def read_dump(path: Path, table: str):
    """Return (column -> index, list of value tuples) for every INSERT in a dump.

    The mysqldump files carry several ``INSERT INTO ... VALUES`` statements, one
    per chunk of rows.  Rows are all-columns, in schema order.
    """
    columns = read_columns(path, table)
    index = {name: i for i, name in enumerate(columns)}
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = f"INSERT INTO `{table}` VALUES"
    rows = []
    pos = 0
    statements = 0
    while True:
        start = text.find(marker, pos)
        if start < 0:
            break
        statements += 1
        i = start + len(marker)
        n = len(text)
        depth = 0
        in_string = False
        escaped = False
        row_start = None
        while i < n:
            ch = text[i]
            if row_start is None:
                if ch == "(":
                    row_start = i
                    depth = 1
                elif ch == ";":
                    break
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
                                f"{path}: row has {len(values)} values, schema has {len(columns)} columns"
                            )
                        rows.append(values)
                        row_start = None
            i += 1
        pos = i
    if not statements:
        raise SystemExit(f"{path}: no INSERT INTO `{table}` VALUES")
    return index, rows


# ---------------------------------------------------------------------------
# Data construction
# ---------------------------------------------------------------------------

def arrival_point(index, rows, city: str):
    """Return the city's teleport arrival row from spell_target_position."""
    spell = CITIES[city]["spell"]
    matches = [
        row
        for row in rows
        if row[index["ID"]] == spell and row[index["EffectIndex"]] == 0
    ]
    if len(matches) != 1:
        raise SystemExit(
            f"{city}: expected exactly one spell_target_position row for spell {spell} "
            f"with EffectIndex = 0, found {len(matches)}"
        )
    row = matches[0]
    if row[index["MapID"]] != CITIES[city]["map"]:
        raise SystemExit(
            f"{city}: spell {spell} arrives on map {row[index['MapID']]}, "
            f"expected {CITIES[city]['map']}"
        )
    for column in ("PositionX", "PositionY", "PositionZ", "Orientation"):
        value = row[index[column]]
        if not isinstance(value, (int, float)) or not math.isfinite(value):
            raise SystemExit(f"{city}: spell {spell} has non-finite {column}")
    return row


def validate_templates(template_index, template_rows):
    by_entry = {}
    for row in template_rows:
        by_entry.setdefault(row[template_index["entry"]], []).append(row)
    for city, info in CITIES.items():
        rows = by_entry.get(info["template"])
        if not rows:
            raise SystemExit(f"{city}: gameobject_template entry {info['template']} is missing")
        if len(rows) != 1:
            raise SystemExit(f"{city}: gameobject_template entry {info['template']} is not unique")
        row = rows[0]
        if row[template_index["type"]] != 22:
            raise SystemExit(
                f"{city}: template {info['template']} has type "
                f"{row[template_index['type']]}, expected 22 (SPELLCASTER)"
            )
        if row[template_index["Data0"]] != info["spell"]:
            raise SystemExit(
                f"{city}: template {info['template']} casts spell "
                f"{row[template_index['Data0']]}, expected {info['spell']}"
            )


def validate_template_factions(addon_index, addon_rows):
    """Return city -> gameobject_template_addon.faction, from the dump."""
    by_entry = {}
    for row in addon_rows:
        by_entry.setdefault(row[addon_index["entry"]], []).append(row[addon_index["faction"]])
    factions = {}
    for city, info in CITIES.items():
        values = by_entry.get(info["template"])
        if not values:
            raise SystemExit(f"{city}: template {info['template']} has no gameobject_template_addon row")
        if len(values) != 1:
            raise SystemExit(f"{city}: template {info['template']} has several addon rows")
        faction = values[0]
        if info["template"] == NEUTRAL_TEMPLATE:
            # The one named exception: Ironforge's template is neutral.
            if faction != 0:
                raise SystemExit(
                    f"{city}: template {info['template']} is expected to be neutral (0), got {faction}"
                )
        elif faction not in (ALLIANCE_FACTION, HORDE_FACTION):
            raise SystemExit(
                f"{city}: template {info['template']} has faction {faction}, "
                f"expected {ALLIANCE_FACTION} (Alliance) or {HORDE_FACTION} (Horde)"
            )
        factions[city] = faction
    return factions


def validate_stock_portal_pad(host, host_info, px, py, pz, gameobject_index, gameobject_rows):
    """Prove the host arrival pad is a real, walkable spot from stock data.

    The stock "Portal to Blasted Lands" objects (195141/195142) sit on the same
    capital pads.  Stormwind's stock object is about 100 yd above the pad, so
    its x/y is checked with a wider z bound; every other city is within a few
    yards in z.
    """
    dz_limit = STOCK_PORTAL_DZ_EXCEPTIONS.get(host, STOCK_PORTAL_DZ)
    for row in gameobject_rows:
        if row[gameobject_index["id"]] not in STOCK_PORTAL_ENTRIES:
            continue
        if row[gameobject_index["map"]] != host_info["map"]:
            continue
        dx = row[gameobject_index["position_x"]] - px
        dy = row[gameobject_index["position_y"]] - py
        dz = row[gameobject_index["position_z"]] - pz
        if math.hypot(dx, dy) <= STOCK_PORTAL_RADIUS and abs(dz) <= dz_limit:
            return
    raise SystemExit(
        f"{host}: no stock Blasted Lands portal within {STOCK_PORTAL_RADIUS} yd of the arrival pad "
        f"(dz <= {dz_limit})"
    )


def build_spawns(
    template_index,
    template_rows,
    addon_index,
    addon_rows,
    target_index,
    target_rows,
    graveyard_index,
    graveyard_rows,
    gameobject_index,
    gameobject_rows,
):
    validate_templates(template_index, template_rows)
    factions = validate_template_factions(addon_index, addon_rows)

    ghost_zone_column = graveyard_index["GhostZone"]
    ghost_zones = {row[ghost_zone_column] for row in graveyard_rows}
    for city, info in CITIES.items():
        if info["zone"] not in ghost_zones:
            raise SystemExit(f"{city}: zone {info['zone']} is not a GhostZone in graveyard_zone")

    spawns = []
    guid = GUID_BASE
    for host, destinations in NETWORK.items():
        host_info = CITIES[host]
        if len(destinations) != 2:
            raise SystemExit(f"{host}: expected exactly two destinations")
        # Host faction is derived from the data: the host's non-neutral
        # destinations must agree.  Ironforge's own template is the one neutral
        # exception, so it cannot supply its own faction when it is the host.
        dest_factions = {
            factions[city]
            for city in destinations
            if CITIES[city]["template"] != NEUTRAL_TEMPLATE
        }
        if len(dest_factions) != 1:
            raise SystemExit(f"{host}: destinations disagree on faction: {sorted(dest_factions)}")
        host_faction = dest_factions.pop()
        if host_faction not in (ALLIANCE_FACTION, HORDE_FACTION):
            raise SystemExit(f"{host}: destination faction {host_faction} is neither Alliance nor Horde")
        if host_info["template"] == NEUTRAL_TEMPLATE:
            if host != NEUTRAL_DESTINATION or factions[host] != 0:
                raise SystemExit(f"{host}: neutral template exception does not match the data")
        elif factions[host] != host_faction:
            raise SystemExit(
                f"{host}: host template faction {factions[host]} does not match "
                f"its destinations' faction {host_faction}"
            )
        # Stable A/B order: the lower destination template entry goes on the
        # arrival orientation, the higher one to its side.
        destinations = sorted(destinations, key=lambda city: CITIES[city]["template"])
        host_row = arrival_point(target_index, target_rows, host)
        px = host_row[target_index["PositionX"]]
        py = host_row[target_index["PositionY"]]
        pz = host_row[target_index["PositionZ"]]
        orientation = host_row[target_index["Orientation"]]
        validate_stock_portal_pad(
            host, host_info, px, py, pz, gameobject_index, gameobject_rows
        )
        host_positions = []
        for slot, destination in enumerate(destinations):
            dest_info = CITIES[destination]
            dest_faction = factions[destination]
            if dest_info["template"] == NEUTRAL_TEMPLATE:
                if destination != NEUTRAL_DESTINATION:
                    raise SystemExit(
                        f"{destination}: neutral template {NEUTRAL_TEMPLATE} is only allowed "
                        f"for {NEUTRAL_DESTINATION}"
                    )
                if dest_faction != 0:
                    raise SystemExit(f"{destination}: neutral exception has faction {dest_faction}")
            else:
                if dest_faction == 0:
                    raise SystemExit(f"{host} -> {destination}: destination is neutral")
                if dest_faction != host_faction:
                    raise SystemExit(
                        f"{host} -> {destination}: destination faction {dest_faction} "
                        f"does not match host faction {host_faction}"
                    )
            # Resolving the destination asserts its spell arrives on the
            # destination's own map; its template and spell were validated above.
            arrival_point(target_index, target_rows, destination)
            if host_info["map"] == dest_info["map"]:
                raise SystemExit(f"{host} -> {destination}: same map {host_info['map']}")
            # Slot A sits five yards along the arrival orientation; slot B is
            # another five yards to the side (same along component plus the
            # perpendicular), so the two objects never overlap.
            along_x = PORTAL_OFFSET * math.cos(orientation)
            along_y = PORTAL_OFFSET * math.sin(orientation)
            if slot == 0:
                offset_x, offset_y = along_x, along_y
            else:
                offset_x = along_x + PORTAL_OFFSET * math.cos(orientation + math.pi / 2.0)
                offset_y = along_y + PORTAL_OFFSET * math.sin(orientation + math.pi / 2.0)
            x = round(px + offset_x, 6)
            y = round(py + offset_y, 6)
            z = round(pz, 6)
            distance = math.hypot(x - px, y - py)
            if not (1.0 <= distance <= 8.0):
                raise SystemExit(f"{host} -> {destination}: offset lands {distance} yards from arrival")
            quat2 = round(math.sin(orientation / 2.0), 6)
            quat3 = round(math.cos(orientation / 2.0), 6)
            spawn_orientation = round(orientation, 6)
            for axis, value in (("x", x), ("y", y), ("z", z)):
                if not math.isfinite(value) or abs(value) > 20000:
                    raise SystemExit(f"{host} -> {destination}: implausible {axis}={value}")
            host_positions.append((x, y))
            spawns.append(
                {
                    "guid": guid,
                    "id": dest_info["template"],
                    "map": host_info["map"],
                    "zoneId": host_info["zone"],
                    "areaId": host_info["zone"],
                    "position_x": x,
                    "position_y": y,
                    "position_z": z,
                    "orientation": spawn_orientation,
                    "rotation0": 0.0,
                    "rotation1": 0.0,
                    "rotation2": quat2,
                    "rotation3": quat3,
                    "Comment": f"portal {host} -> {destination}",
                }
            )
            guid += 1
        (ax, ay), (bx, by) = host_positions
        separation = math.hypot(bx - ax, by - ay)
        if separation < 5.0 - 1e-6:
            raise SystemExit(f"{host}: portals are only {separation} yards apart")

    if len(spawns) != 16:
        raise SystemExit(f"expected 16 spawns, built {len(spawns)}")
    if guid - 1 > GUID_MAX:
        raise SystemExit(f"spawn guids overflow the private range at {guid - 1}")

    spawns.sort(key=lambda spawn: spawn["guid"])
    return spawns


def validate_free_guids(gameobject_index, gameobject_rows, spawns):
    used = {row[gameobject_index["guid"]] for row in gameobject_rows}
    for spawn in spawns:
        guid = spawn["guid"]
        if not GUID_BASE <= guid <= GUID_MAX:
            raise SystemExit(f"guid {guid} outside {GUID_BASE}-{GUID_MAX}")
        if guid in used:
            raise SystemExit(f"guid {guid} is already used in the base gameobject dump")


# ---------------------------------------------------------------------------
# SQL emission
# ---------------------------------------------------------------------------

def sql_number(value) -> str:
    if isinstance(value, float):
        text = f"{value:.6f}".rstrip("0").rstrip(".")
        return text if text else "0"
    return str(value)


def sql_literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return sql_number(value)


def insert_statement(table: str, columns: list[str], rows: list[list]) -> str:
    head = f"INSERT INTO `{table}` (" + ", ".join(f"`{column}`" for column in columns) + ") VALUES\n"
    body = ",\n".join(
        "(" + ", ".join(sql_literal(value) for value in row) + ")" for row in rows
    )
    return head + body + ";"


def file_header(count: int) -> list[str]:
    return [
        "-- Two-way city portal network for the solo-hosted realm.",
        "-- Generated by scripts/travel/gen_portals.py from the pinned core's base world dumps.",
        "-- Do not edit by hand; change the generator and regenerate.",
        "--",
        "-- Mechanism: each row is a `gameobject` spawn of a stock `gameobject_template`",
        "-- entry with type 22 (GAMEOBJECT_TYPE_SPELLCASTER).  On use the player is the",
        "-- caster: `GameObject::Use` passes the user as `spellCaster`, and the type-22",
        "-- branch has him cast the template's Data0 spell on himself.  That spell's",
        "-- destination is the core's `spell_target_position` row.  There is no DBC",
        "-- change, no new template and no module; the eight portal templates already",
        "-- exist in the base data and seven of them are spawned by the stock",
        "-- Shattrath portals.",
        "-- Each portal stands in its host city at that city's own teleport arrival",
        "-- point: one five yards along the arrival orientation, the other five",
        "-- yards to its side.  The template and spell it casts belong to the",
        "-- destination city.",
        f"-- {count} spawns: two same-faction, cross-continent portals per capital.",
        "--",
        "-- The DELETE names only the guids this file inserts.  The server seeds its",
        "-- gameobject guid allocator from MAX(guid) + 1, so after this file applies",
        "-- new objects are allocated above these guids; a range delete could delete",
        "-- them when the updater re-applies this file.",
        "",
    ]


def build_outputs(world: Path) -> dict[str, str]:
    template_index, template_rows = read_dump(world / "gameobject_template.sql", "gameobject_template")
    addon_index, addon_rows = read_dump(
        world / "gameobject_template_addon.sql", "gameobject_template_addon"
    )
    target_index, target_rows = read_dump(world / "spell_target_position.sql", "spell_target_position")
    graveyard_index, graveyard_rows = read_dump(world / "graveyard_zone.sql", "graveyard_zone")
    gameobject_index, gameobject_rows = read_dump(world / "gameobject.sql", "gameobject")

    spawns = build_spawns(
        template_index,
        template_rows,
        addon_index,
        addon_rows,
        target_index,
        target_rows,
        graveyard_index,
        graveyard_rows,
        gameobject_index,
        gameobject_rows,
    )
    validate_free_guids(gameobject_index, gameobject_rows, spawns)

    values = [
        [
            spawn["guid"],
            spawn["id"],
            spawn["map"],
            spawn["zoneId"],
            spawn["areaId"],
            SPAWN_MASK,
            PHASE_MASK,
            spawn["position_x"],
            spawn["position_y"],
            spawn["position_z"],
            spawn["orientation"],
            spawn["rotation0"],
            spawn["rotation1"],
            spawn["rotation2"],
            spawn["rotation3"],
            SPAWNTIMESECS,
            ANIMPROGRESS,
            STATE,
            SCRIPT_NAME,
            VERIFIED_BUILD,
            spawn["Comment"],
        ]
        for spawn in spawns
    ]

    sql = file_header(len(spawns))
    guids = ", ".join(str(spawn["guid"]) for spawn in spawns)
    sql.append(f"DELETE FROM `gameobject` WHERE `guid` IN ({guids});")
    sql.append("")
    sql.append(insert_statement("gameobject", INSERT_COLUMNS, values))
    sql.append("")
    return {OUTPUT_NAME: "\n".join(sql)}


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

    outputs = build_outputs(world)
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
