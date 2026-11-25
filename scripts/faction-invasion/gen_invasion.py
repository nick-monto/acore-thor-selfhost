#!/usr/bin/env python3
"""Generate mod-faction-invasion's data SQL and the add-on ring assets.

The generator reads the pinned core's base world dumps, selects the ten themed
creature rosters by faction/name, clones every selected rank-0 entry into a
rank-1 elite at ``entry + 700000`` (with its model/spell/equip/addon/movement/
resistance rows), reuses scripts/loot-bags/gen_loot_bags.py for the level
bracket item pools and money table, and writes the committed module SQL.

Run from the repository root::

    python3 scripts/faction-invasion/gen_invasion.py --core ../azerothcore-wotlk

``--check`` regenerates in memory and fails if any committed file differs.
``--assets DIR`` also writes the add-on ring/disc TGA files (normally into
acore-thor-launcher/app/src/main/assets/invasion).
"""

from __future__ import annotations

import argparse
import difflib
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "modules" / "mod-faction-invasion" / "data" / "sql" / "world" / "base"
ELITE_BASE = 700000
LIMIT_PER_BAND = 6

sys.path.insert(0, str(REPO / "scripts" / "creature-gear"))
sys.path.insert(0, str(REPO / "scripts" / "loot-bags"))
from gen_creature_gear import column_index, parse_dump  # noqa: E402
from gen_loot_bags import BRACKETS, BRACKET_ITEMS, MONEY, build_outputs  # noqa: E402

# The ten themes. ``faction`` is displayed; ``faction_match`` matches the DBC
# faction name and ``name_match`` matches the creature's name/subname. Every
# candidate is already restricted to FactionGroup 8 (hostile to both player
# factions) and rank 0, so the name fallback cannot leak a friendly NPC.
THEMES = [
    dict(key="scourge", name="Scourge", faction="Scourge", marker=71432,
         stat="health", amount=0.20, weight=1,
         faction_match=r"scourge|undead, scourge",
         name_match=r"scourge|plague|ghoul|abomination|geist|banshee|skeleton|bone|necro|flesh|blight"),
    dict(key="legion", name="Burning Legion", faction="Burning Legion", marker=69737,
         stat="spell", amount=0.20, weight=1,
         faction_match=r"demon|burning legion|servant of illidan",
         name_match=r"demon|fel|infernal|imp|succubus|doomguard|eredar|legion|voidwalker|warlock"),
    dict(key="elemental", name="Elemental", faction="Elemental", marker=67828,
         stat="damage", amount=0.15, weight=1,
         faction_match=r"elemental",
         name_match=r"elemental|inferno|tide|flame|frost|storm|earth|water|air spirit"),
    dict(key="dragonkin", name="Dragonkin", faction="Dragonkin", marker=69737,
         stat="health", amount=0.25, weight=1,
         faction_match=r"dragonflight|dragonkin|dragonflight, black",
         name_match=r"dragon|drake|wyrm|whelp|drakonid|brood"),
    dict(key="scarlet", name="Scarlet Crusade", faction="Scarlet Crusade", marker=67828,
         stat="damage", amount=0.15, weight=1,
         faction_match=r"scarlet crusade",
         name_match=r"scarlet|crusader|zealot|abbot|chaplain"),
    dict(key="troll", name="Troll", faction="Troll", marker=71432,
         stat="damage", amount=0.20, weight=1,
         faction_match=r"troll",
         name_match=r"troll|headhunter|witch doctor|berserker|shadowhunter"),
    dict(key="ogre", name="Ogre", faction="Ogre", marker=69737,
         stat="health", amount=0.25, weight=1,
         faction_match=r"ogre",
         name_match=r"ogre|brute|gorger"),
    dict(key="naga", name="Naga", faction="Naga", marker=67828,
         stat="spell", amount=0.15, weight=1,
         faction_match=r"naga",
         name_match=r"naga|siren|tidecaller|slavedriver|wrangler"),
    dict(key="darkiron", name="Dark Iron", faction="Dark Iron", marker=71432,
         stat="armor", amount=0.30, weight=1,
         faction_match=r"dark iron",
         name_match=r"dark iron|molten|forge"),
    dict(key="murloc", name="Murloc", faction="Murloc", marker=67828,
         stat="health", amount=0.10, weight=1,
         faction_match=r"murloc",
         name_match=r"murloc|mrgl|tidemaster|coastrunner"),
]

# Curated anchors from the zone scan (top outdoor zones). One anchor per zone;
# the module resolves the terrain Z at spawn. Level range is deliberately 1-80:
# the wave is scaled to the triggering player, not to the zone's level.
AREAS = [
    (1, 495, 571, "Howling Fjord", 885.0, -4850.0),
    (2, 210, 571, "Icecrown", 6172.0, 2241.0),
    (3, 3537, 571, "Borean Tundra", 4504.0, 5339.0),
    (4, 65, 571, "Dragonblight", 3529.0, -1150.0),
    (5, 85, 0, "Tirisfal Glades", 3039.0, 668.0),
    (6, 3520, 530, "Shadowmoon Valley", -4984.0, 293.0),
    (7, 17, 1, "The Barrens", 1027.0, -3066.0),
    (8, 12, 0, "Elwynn Forest", -9793.0, 160.0),
    (9, 3483, 530, "Hellfire Peninsula", 904.0, 1729.0),
    (10, 3519, 530, "Terokkar Forest", -3664.0, 3766.0),
    (11, 3521, 530, "Zangarmarsh", 188.0, 8729.0),
    (12, 3523, 530, "Netherstorm", 2819.0, 4022.0),
    (13, 394, 571, "Grizzly Hills", 4533.0, -2126.0),
    (14, 3518, 530, "Nagrand", -2934.0, 6431.0),
    (15, 66, 571, "Zul'Drak", 6164.0, -1979.0),
    (16, 3522, 530, "Blade's Edge Mountains", 1736.0, 4626.0),
    (17, 3524, 530, "Azuremyst Isle", -4505.0, -11590.0),
    (18, 440, 1, "Tanaris", -8070.0, -5334.0),
    (19, 3711, 571, "Sholazar Basin", 6208.0, 3898.0),
    (20, 1, 0, "Dun Morogh", -5581.0, -1734.0),
]

GROUND = [
    ("scourge", 59285, "single", 1, 10.0),
    ("legion", 35895, "single", 1, 5.0),
    ("elemental", 26607, "cluster", 3, 8.0),
    ("dragonkin", 62910, "single", 1, 9.0),
    ("scarlet", 20116, "single", 1, 8.0),
    ("troll", 23861, "single", 1, 8.0),
    ("ogre", 61882, "cluster", 2, 15.0),
    ("naga", 55868, "single", 1, 10.0),
    ("darkiron", 2120, "cluster", 2, 5.0),
    ("murloc", 26607, "cluster", 3, 8.0),
]

BANDS = [(1, 19), (20, 39), (40, 59), (60, 69), (70, 80)]

# The pinned base dump ships factiontemplate rows but no Faction.dbc name rows.
# These are the names the zone scan read from the live client DBC for the
# factions these themes can match; an unknown faction contributes no name and
# the creature-name regex still covers the generic Monster/Creature buckets.
FACTION_NAMES = {
    1: "PLAYER, Human",
    7: "Creature",
    14: "Monster",
    16: "Monster",
    18: "Murloc",
    21: "Undead, Scourge",
    35: "Friendly",
    37: "Troll, Frostmane",
    38: "Beast - Wolf",
    40: "Orc, Blackrock",
    45: "Ogre",
    54: "Dark Iron Dwarves",
    57: "Ironforge",
    67: "Scarlet Crusade",
    74: "Naga",
    83: "Horde Generic",
    90: "Demon",
    91: "Elemental",
    103: "Dragonflight, Black",
    190: "Ambient",
    974: "Undead, Scourge",
    1214: "Frostwolf Clan",
    1216: "Stormpike Guard",
    1720: "Hyjal Invaders",
    1813: "Servant of Illidan",
    1885: "Vrykul",
}


def as_int(value: str) -> int:
    return int(value)


def sql_literal(value: str) -> str:
    if value == "NULL":
        return "NULL"
    try:
        float(value)
        return value
    except ValueError:
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def insert_statement(table: str, columns: list[str], rows: list[list[str]]) -> str:
    head = f"INSERT INTO `{table}` (" + ", ".join(f"`{c}`" for c in columns) + ") VALUES\n"
    return head + ",\n".join("(" + ", ".join(sql_literal(v) for v in row) + ")" for row in rows) + ";\n"


def read(world: Path, name: str):
    return parse_dump(world / f"{name}.sql")


def build_roster(world: Path):
    creature_columns, creature_rows = read(world, "creature_template")
    template_columns, template_rows = read(world, "factiontemplate_dbc")
    loot_columns, loot_rows = read(world, "creature_loot_template")
    model_columns, model_rows = read(world, "creature_template_model")

    creature = {c: column_index(creature_columns, c) for c in
                ("entry", "name", "subname", "minlevel", "maxlevel", "faction", "npcflag",
                 "rank", "type", "lootid", "flags_extra")}
    ftemplate = {c: column_index(template_columns, c) for c in ("ID", "Faction", "FactionGroup")}
    loot_entry = column_index(loot_columns, "Entry")
    model_id = column_index(model_columns, "CreatureID")

    template_group = {row[ftemplate["ID"]]: as_int(row[ftemplate["FactionGroup"]]) for row in template_rows}
    template_faction = {row[ftemplate["ID"]]: row[ftemplate["Faction"]] for row in template_rows}
    loot_ids = {row[loot_entry] for row in loot_rows}
    model_ids = {row[model_id] for row in model_rows}

    candidates = []
    for row in creature_rows:
        if as_int(row[creature["type"]]) in (8, 10, 11, 12):
            continue
        if as_int(row[creature["npcflag"]]) != 0:
            continue
        flags = as_int(row[creature["flags_extra"]])
        if flags & 0x80 or flags & 0x2:
            continue
        if as_int(row[creature["rank"]]) != 0:
            continue
        if row[creature["entry"]] not in model_ids:
            continue
        loot = row[creature["lootid"]]
        if not loot or loot not in loot_ids:
            continue
        minlevel = as_int(row[creature["minlevel"]])
        maxlevel = as_int(row[creature["maxlevel"]])
        if maxlevel < 1 or minlevel > 80:
            continue
        name = row[creature["name"]]
        if not name or any(bad in name for bad in ("UNUSED", "Invisible", "Trigger", "Test", "[PH]")):
            continue
        template = row[creature["faction"]]
        if template_group.get(template) != 8:
            continue
        fname = FACTION_NAMES.get(as_int(template_faction.get(template, "0")), "")
        candidates.append({
            "entry": row[creature["entry"]],
            "name": name,
            "subname": row[creature["subname"]],
            "minlevel": minlevel,
            "maxlevel": maxlevel,
            "faction": fname,
        })

    import re
    roster = {}
    for theme in THEMES:
        faction_re = re.compile(theme["faction_match"], re.I)
        name_re = re.compile(theme["name_match"], re.I)
        roster[theme["key"]] = {}
        for band_index, (lo, hi) in enumerate(BANDS):
            picked = []
            for row in candidates:
                if not (lo <= row["maxlevel"] <= hi):
                    continue
                haystack = row["name"] + " " + row["subname"]
                if faction_re.search(row["faction"]) or name_re.search(haystack):
                    picked.append(row)
            picked.sort(key=lambda r: (as_int(r["entry"]), r["name"]))
            seen = set()
            unique = []
            for row in picked:
                if row["entry"] in seen:
                    continue
                seen.add(row["entry"])
                unique.append(row)
            roster[theme["key"]][band_index] = unique[:LIMIT_PER_BAND]

    return roster


def clone_entries(world: Path, bases: set[str]):
    """Clone every selected base entry and its companion rows."""
    creature_columns, creature_rows = read(world, "creature_template")
    by_entry = {row[column_index(creature_columns, "entry")]: row for row in creature_rows}
    idx = {name: column_index(creature_columns, name) for name in
           ("entry", "rank", "difficulty_entry_1", "difficulty_entry_2", "difficulty_entry_3",
            "KillCredit1", "KillCredit2", "npcflag", "VehicleId", "AIName", "ScriptName")}

    clone_rows = []
    for base in sorted(bases, key=lambda value: as_int(value)):
        row = list(by_entry[base])
        row[idx["entry"]] = str(as_int(base) + ELITE_BASE)
        row[idx["rank"]] = "1"
        row[idx["difficulty_entry_1"]] = "0"
        row[idx["difficulty_entry_2"]] = "0"
        row[idx["difficulty_entry_3"]] = "0"
        row[idx["KillCredit1"]] = "0"
        row[idx["KillCredit2"]] = "0"
        row[idx["npcflag"]] = "0"
        row[idx["VehicleId"]] = "0"
        row[idx["AIName"]] = ""
        row[idx["ScriptName"]] = ""
        clone_rows.append(row)

    companion = []
    for table, key in (
        ("creature_template_model", "CreatureID"),
        ("creature_template_spell", "CreatureID"),
        ("creature_equip_template", "CreatureID"),
        ("creature_template_addon", "entry"),
        ("creature_template_movement", "CreatureId"),
        ("creature_template_resistance", "CreatureID"),
    ):
        path = world / f"{table}.sql"
        if not path.exists():
            continue
        columns, rows = parse_dump(path)
        key_index = column_index(columns, key)
        copied = []
        for row in rows:
            value = row[key_index]
            if value not in bases:
                continue
            copied_row = list(row)
            copied_row[key_index] = str(as_int(value) + ELITE_BASE)
            copied.append(copied_row)
        if copied:
            companion.append((table, columns, copied, key))

    return creature_columns, clone_rows, companion


def build_loot(world: Path):
    files, bracket_pools = build_outputs(world)
    del files

    pool_rows = []
    for bracket_index, (_lo, _hi, chosen) in enumerate(bracket_pools, start=1):
        for quality in ("common", "uncommon", "rare", "epic"):
            pool = chosen[quality]
            for item in pool.items:
                pool_rows.append([str(bracket_index), quality, str(item)])

    loot_rows = []
    for bracket_index, (lo, hi) in enumerate(BRACKETS, start=1):
        cloth, heal, mana = BRACKET_ITEMS[bracket_index - 1]

        def add(difficulty, kind, order, quality, item_id="0", rolls="1", chance="100"):
            loot_rows.append([str(bracket_index), str(difficulty), kind, str(order),
                              quality, str(item_id), rolls, chance])

        add(1, "mob", 0, "common", rolls="1", chance="65")
        add(1, "mob", 1, "common", rolls="1", chance="30")
        add(1, "mob", 2, "fixed", item_id=heal, rolls="1", chance="25")
        add(2, "mob", 0, "uncommon", rolls="1", chance="75")
        add(2, "mob", 1, "common", rolls="1", chance="45")
        add(2, "mob", 2, "fixed", item_id=heal, rolls="1", chance="35")
        add(3, "mob", 0, "rare", rolls="1", chance="80")
        add(3, "mob", 1, "uncommon", rolls="1", chance="50")
        add(3, "mob", 2, "fixed", item_id=mana, rolls="1", chance="35")

        add(1, "chest", 0, "uncommon", rolls="2", chance="100")
        add(1, "chest", 1, "common", rolls="1", chance="60")
        add(1, "chest", 2, "fixed", item_id=heal, rolls="2", chance="100")
        add(2, "chest", 0, "rare", rolls="2", chance="100")
        add(2, "chest", 1, "uncommon", rolls="1", chance="70")
        add(2, "chest", 2, "fixed", item_id=heal, rolls="2", chance="100")
        add(2, "chest", 3, "fixed", item_id=mana, rolls="2", chance="100")
        add(3, "chest", 0, "epic", rolls="1", chance="100")
        add(3, "chest", 1, "rare", rolls="2", chance="100")
        add(3, "chest", 2, "fixed", item_id=heal, rolls="2", chance="100")
        add(3, "chest", 3, "fixed", item_id=mana, rolls="2", chance="100")

    money_rows = []
    for bracket_index, (lo, hi) in enumerate(BRACKETS, start=1):
        for difficulty in (1, 2, 3):
            tier = min(difficulty - 1, 3)
            chest_tier = min(difficulty, 3)
            mob = max(1, MONEY[bracket_index - 1][tier][0] // 50)
            chest = max(1, MONEY[bracket_index - 1][chest_tier][0])
            money_rows.append([str(bracket_index), str(difficulty), str(mob), str(chest)])

    return pool_rows, loot_rows, money_rows


def header(title: str, counts: str) -> str:
    return (
        f"-- {title}\n"
        "-- Generated by scripts/faction-invasion/gen_invasion.py; do not edit by hand.\n"
        f"-- {counts}\n"
        "-- Re-run: python3 scripts/faction-invasion/gen_invasion.py --core <pinned core>\n\n"
    )


def generate(world: Path):
    roster = build_roster(world)
    bases = set()
    for themed in roster.values():
        for rows in themed.values():
            bases.update(row["entry"] for row in rows)

    creature_sql = header("mod-faction-invasion: themed level-band creature roster.",
                          f"{sum(len(rows) for themed in roster.values() for rows in themed.values())} rows.")
    creature_sql += "DELETE FROM `mod_faction_invasion_creature`;\n\n"
    creature_rows_out = []
    for theme in THEMES:
        for band_index, (lo, hi) in enumerate(BANDS):
            for row in roster[theme["key"]][band_index]:
                elite = str(as_int(row["entry"]) + ELITE_BASE)
                creature_rows_out.append([theme["key"], row["entry"], elite, str(lo), str(hi), "1"])
    creature_sql += insert_statement(
        "mod_faction_invasion_creature",
        ["theme_key", "base_entry", "elite_entry", "min_level", "max_level", "weight"],
        creature_rows_out)

    theme_sql = header("mod-faction-invasion: the ten themed factions.",
                       f"{len(THEMES)} rows.")
    theme_sql += "DELETE FROM `mod_faction_invasion_theme`;\n\n"
    theme_sql += insert_statement(
        "mod_faction_invasion_theme",
        ["theme_key", "name", "faction", "marker_spell", "modifier_stat", "modifier_amount", "weight"],
        [[theme["key"], theme["name"], theme["faction"], str(theme["marker"]), theme["stat"],
          f"{theme['amount']:.2f}", str(theme["weight"])] for theme in THEMES])

    area_sql = header("mod-faction-invasion: curated outdoor anchors.",
                      f"{len(AREAS)} rows.")
    area_sql += "DELETE FROM `mod_faction_invasion_area`;\n\n"
    area_sql += insert_statement(
        "mod_faction_invasion_area",
        ["id", "zone_id", "map_id", "name", "x", "y", "z", "radius", "min_level", "max_level", "weight"],
        [[str(i), str(zone), str(map_id), name, f"{x:.1f}", f"{y:.1f}", "0", "120", "1", "80", "1"]
         for i, zone, map_id, name, x, y in AREAS])

    ground_sql = header("mod-faction-invasion: the faction ground effect spells.",
                        f"{len(GROUND)} rows.")
    ground_sql += "DELETE FROM `mod_faction_invasion_ground`;\n\n"
    ground_sql += insert_statement(
        "mod_faction_invasion_ground",
        ["theme_key", "spell_id", "pattern", "patch_count", "radius", "weight"],
        [[theme, str(spell), pattern, str(count), f"{radius:.1f}", "1"]
         for theme, spell, pattern, count, radius in GROUND])

    pool_rows, loot_rows, money_rows = build_loot(world)

    loot_sql = header("mod-faction-invasion: level-bracket item pools and roll plans.",
                      f"{len(pool_rows)} pool items, {len(loot_rows)} roll rows, {len(money_rows)} money rows.")
    loot_sql += "DELETE FROM `mod_faction_invasion_pool`;\n\n"
    loot_sql += insert_statement("mod_faction_invasion_pool",
                                 ["bracket", "quality", "item_id"], pool_rows)
    loot_sql += "\nDELETE FROM `mod_faction_invasion_loot`;\n\n"
    loot_sql += insert_statement("mod_faction_invasion_loot",
                                 ["bracket", "difficulty", "kind", "roll_order", "quality",
                                  "item_id", "rolls", "chance"], loot_rows)
    loot_sql += "\nDELETE FROM `mod_faction_invasion_money`;\n\n"
    loot_sql += insert_statement("mod_faction_invasion_money",
                                 ["bracket", "difficulty", "mob_copper", "chest_copper"], money_rows)

    clone_columns, clone_rows, companion = clone_entries(world, bases)
    clone_sql = header("mod-faction-invasion: rank-1 elite clones (base + 700000).",
                       f"{len(clone_rows)} creature rows, {len(companion)} companion tables.")
    clone_sql += (
        "-- Rank 1 is the client's elite frame. On this realm Rate.Creature.* is 1, so rank 1\n"
        "-- adds no raw stats; the module's CreatureStatScale supplies the elite multipliers.\n"
        "-- Difficulty entries, kill credit, vehicle id, npcflag and scripts are zeroed so a\n"
        "-- clone cannot start an instance, a quest or an AI of its own.\n\n"
    )
    ids = ", ".join(row[column_index(clone_columns, "entry")] for row in clone_rows)
    clone_sql += f"DELETE FROM `creature_template` WHERE `entry` IN ({ids});\n"
    for table, columns, _rows, key in companion:
        clone_sql += f"DELETE FROM `{table}` WHERE `{key}` IN ({ids});\n"
    clone_sql += "\n"
    clone_sql += insert_statement("creature_template", clone_columns, clone_rows)
    for table, columns, rows, _key in companion:
        clone_sql += "\n"
        clone_sql += insert_statement(table, columns, rows)

    return {
        "faction_invasion_20_themes.sql": theme_sql,
        "faction_invasion_30_creatures.sql": creature_sql,
        "faction_invasion_40_areas.sql": area_sql,
        "faction_invasion_50_ground.sql": ground_sql,
        "faction_invasion_60_loot.sql": loot_sql,
        "faction_invasion_70_elite_clones.sql": clone_sql,
    }


def write_assets(directory: Path):
    directory.mkdir(parents=True, exist_ok=True)

    def tga(path: Path, inner_alpha: int, ring: bool):
        size = 256
        center = (size - 1) / 2.0
        pixels = bytearray()
        for y in range(size - 1, -1, -1):
            for x in range(size):
                distance = math.hypot(x - center, y - center)
                alpha = 0
                if ring:
                    if 116 <= distance <= 126:
                        alpha = 255
                elif distance <= 126:
                    alpha = inner_alpha
                pixels += bytes((255, 255, 255, alpha))  # BGRA, white, tinted by the add-on
        header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, size, size, 32, 0x28)
        path.write_bytes(header + bytes(pixels))

    tga(directory / "ring.tga", 0, True)
    tga(directory / "disc.tga", 64, False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--core", type=Path, required=True, help="checkout of the pinned AzerothCore tree")
    parser.add_argument("--check", action="store_true", help="fail if the committed SQL differs from a fresh run")
    parser.add_argument("--assets", type=Path, help="also write ring.tga/disc.tga into this directory")
    args = parser.parse_args()

    world = args.core / "data" / "sql" / "base" / "db_world"
    generated = generate(world)

    if args.assets:
        write_assets(args.assets)

    failed = False
    for name, content in generated.items():
        path = OUT / name
        if args.check:
            if not path.exists():
                print(f"missing {path}", file=sys.stderr)
                failed = True
                continue
            current = path.read_text(encoding="utf-8")
            if current != content:
                print(f"FAIL {name} differs from the generator", file=sys.stderr)
                for line in list(difflib.unified_diff(current.splitlines(), content.splitlines(),
                                                      fromfile=str(path), tofile="generated", lineterm=""))[:60]:
                    print(line, file=sys.stderr)
                failed = True
        else:
            path.write_text(content, encoding="utf-8")
            print(f"wrote {path.relative_to(REPO)} ({len(content.splitlines())} lines)")

    if args.check and failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
