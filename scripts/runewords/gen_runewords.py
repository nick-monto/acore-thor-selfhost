#!/usr/bin/env python3
"""Generate the runeword SQL: rune items, the word table, and their world drops.

The patch is pure data, in four files under ``sql/custom/db_world``:

``runewords_10_items.sql``
    One ``item_template`` row per rune (33 of them, El through Zod). Class 15,
    not equippable, and one rune per slot: the 1..100 roll belongs to the item
    instance, so a stack would carry one roll for several runes. Each row
    carries the quality colour its ladder position earns and the vendor price
    that rarity is worth; the roll pays up to that price again on sale.

``runewords_20_words.sql``
    The ``runeword_words`` table and its rows. A word is a *pattern*: an ordered
    list of ``row:col:entry`` cells. The module reads the pattern, looks for it in
    the player's backpack or bags, and grants the word's spells while it holds.
    The table is created here rather than in module SQL because a failing module
    SQL file aborts the whole worldserver boot on this fork; a plain
    ``CREATE TABLE IF NOT EXISTS`` in a custom SQL file is applied by the
    database updater and cannot do that.

``runewords_30_drop_tables.sql`` / ``runewords_40_creature_drops.sql``
    One ``reference_loot_template`` per level bracket a creature actually rolls,
    and one ``creature_loot_template`` row per loot id pointing at it. A rune
    becomes available at a level in the ladder and stays in every later bracket,
    with a weight that falls as it ages, so the newest rune in a bracket is the
    rarest thing in it.

Every icon is borrowed: the runes take ``displayid``s from the base database's
own gems, so nothing is drawn and no ``ItemDisplayInfo`` row is added. The 33
``Item.dbc`` records the client needs are appended by
``scripts/item-dbc/patch_item_dbc.py`` from the same tables this file writes, so
the SQL and the DBC cannot drift.

Run against a checkout of the pinned core (see ``pins.toml``)::

    python3 scripts/runewords/gen_runewords.py --core ../azerothcore-wotlk

``--check`` regenerates in memory and fails if the committed SQL differs.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "lib"))


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The loot-bag generator is the sibling this one is modelled on: the same SQL
# emission, the same bracketing and the same read of the pinned base dumps. Its
# helpers are reused rather than copied so the two files cannot drift apart.
loot = _load("gen_loot_bags", REPO / "scripts" / "loot-bags" / "gen_loot_bags.py")

BRACKETS = loot.BRACKETS
MAX_BRACKET_LEVEL = loot.MAX_BRACKET_LEVEL
ITEM_INSERT_COLUMNS = loot.ITEM_INSERT_COLUMNS
LOOT_INSERT_COLUMNS = loot.LOOT_INSERT_COLUMNS
insert_statement = loot.insert_statement
read_table = loot.read_table


def file_header(title: str, counts: str):
    """The sibling's header with this generator named as the author."""
    return [
        line.replace("scripts/loot-bags/gen_loot_bags.py", "scripts/runewords/gen_runewords.py")
        for line in loot.file_header(title, counts)
    ]

# ---------------------------------------------------------------------------
# Id ranges
# ---------------------------------------------------------------------------
#
# Above the loot bags (91000-91063) and far above the base item entries (max
# 54591 in the pinned dumps). The delete ranges are wider than the data so a
# regenerated, smaller ladder cannot leave stale rows behind.

RUNE_ENTRY_BASE = 93000
RUNE_ENTRY_DELETE_FROM = 93000
RUNE_ENTRY_DELETE_TO = 93099

# Reference ids sit above the loot bags' own (9800100-9800199) and above the
# base reference entries (max 1619100).
RUNE_REF_BASE = 9800200
RUNE_REF_DELETE_FROM = 9800200
RUNE_REF_DELETE_TO = 9800299

# creature/reference group used by every rune row. The base data uses 1..7 and
# the loot bags use 101; the generator fails if the base outgrows this one.
RUNE_GROUP = 102

# ---------------------------------------------------------------------------
# The ladder
# ---------------------------------------------------------------------------

RUNES = [
    "El", "Eld", "Tir", "Nef", "Eth", "Ith", "Tal", "Ral", "Ort", "Thul",
    "Amn", "Sol", "Shael", "Dol", "Hel", "Io", "Lum", "Ko", "Fal", "Lem",
    "Pul", "Um", "Mal", "Ist", "Gul", "Vex", "Ohm", "Lo", "Sur", "Ber",
    "Jah", "Cham", "Zod",
]

assert len(RUNES) == 33


def rune_entry(index: int) -> int:
    """item_template entry of the rune at 0-based ``index``."""
    return RUNE_ENTRY_BASE + index


def available_at(index: int) -> int:
    """Player level at which the rune starts dropping (El 1 .. Zod 80)."""
    return 1 + round(index * (MAX_BRACKET_LEVEL - 1) / (len(RUNES) - 1))


# A rune's rarity *is* its ladder position: the rune that has just become
# available is the rarest row in its bracket's table and one that has been
# dropping for levels is the commonest, so the colour ladder and the five bands
# below say the same thing twice. (first step, last step, quality), one-based.
QUALITY_BANDS = (
    (1, 8, 1),                        # white
    (9, 16, 2),                       # green
    (17, 24, 3),                      # blue
    (25, 30, 4),                      # epic
    (31, 33, 5),                      # legendary
)

# What a vendor pays for a rune at a zero roll, by quality, in copper: the band
# floor is what the rarity is worth. Inside a band the price still climbs, a
# quarter of the floor per step, because a later rune of the same colour is the
# rarer one; the roll pays up to the same amount again on top (mod-runewords,
# on sale), so a perfect Zod is worth two zero-roll Zods.
QUALITY_SELL = {1: 25, 2: 500, 3: 5000, 4: 25000, 5: 120000}
BAND_STEP_PERCENT = 25


def quality(index: int) -> int:
    """Item quality colour of the rune at 0-based ``index``."""
    step = index + 1
    for first, last, colour in QUALITY_BANDS:
        if first <= step <= last:
            return colour
    raise SystemExit(f"rune {step} is outside the quality bands")


def band_first(index: int) -> int:
    """0-based index of the first rune in ``index``'s quality band."""
    step = index + 1
    for first, last, _ in QUALITY_BANDS:
        if first <= step <= last:
            return first - 1
    raise SystemExit(f"rune {step} is outside the quality bands")


def sell_price(index: int) -> int:
    """Vendor price of the rune at 0-based ``index``, in copper, before its roll."""
    return QUALITY_SELL[quality(index)] * (100 + BAND_STEP_PERCENT * (index - band_first(index))) // 100


# Chance a creature drops its bracket's rune table at all, indexed by the base
# data's creature rank (0 normal, 1 elite, 2 rare elite, 3 boss, 4 rare).
RUNE_CHANCE = [2, 3, 5, 15, 8]


def weight(index: int, bracket_hi: int) -> int:
    """Relative weight of a rune inside one bracket's table.

    A rune that has been dropping for many levels is common in that bracket; the
    rune that has just become available is the rarest row in the table, which is
    what makes the top of the ladder feel like a chase.
    """
    age = bracket_hi - available_at(index)
    return max(1, min(40, 1 + age // 2))


# ---------------------------------------------------------------------------
# Words
# ---------------------------------------------------------------------------
#
# A word is staged from D2's list but its *effects* are borrowed stock spells, so
# nothing new is authored and the client patch stays at Item.dbc only. The palette
# is every permanent aura a bag can hold: the "Increased <stat> NN" ladder,
# "Attack Power NN", the per-school "Increase <school> Dam NN" ladder, "Increased
# Armor", "Increased [All] Resist", the defense/dodge/block ratings, the mana and
# health regeneration rungs, a damage shield, the weapon elemental procs
# ("Add Fire/Lightning Dam - Weap NN") and the stock proc auras. Which word takes
# which is the comment beside it; the rule is that a word should read like the D2
# word it is named for, not that it should be a stat stick.
#
# Every spell here was read out of the client's own Spell.dbc and passes four
# conditions: the first effect is APPLY_AURA, the duration is -1 (permanent),
# EffectDieSides is 0 or 1 and EffectRealPointsPerLevel 0 so the value is
# deterministic, and ImplicitTargetA is 1 so it lands on the holder.
# scripts/runewords/check_spells.py re-checks all four against a real Spell.dbc.
#
# Duration is the one the module cannot forgive: a word's auras are granted once and
# re-derived only when the pattern changes, so a spell with a finite duration would
# simply lapse and never come back. ``scale`` is a percent of the stock spell's own
# basepoints, applied after the rune-roll variance, so the numbers rise with the
# roll without a spell per tier.

WORDS = [
    # Staged from D2's list, and its *effects* are the game's own permanent auras: no
    # spell is authored, so the client patch stays at the runes' Item.dbc records. Each
    # word carries up to three of them, and they are drawn from the families a bag
    # resident aura can actually use - stats, attack power, per-school spell damage,
    # armour, resistances, combat ratings, mana and health regeneration, a damage
    # shield, weapon-damage procs and the stock proc auras - so a word reads like the
    # D2 word it is named for: Bramble reflects damage, Doom chills, Phoenix burns,
    # Delirium frightens, Smoke resists.
    #
    # Every id below was read out of the client's own Spell.dbc and satisfies four
    # conditions: the first effect is APPLY_AURA, the duration is permanent
    # (SpellDuration.dbc -1), the spell targets its caster (ImplicitTargetA 1), and its
    # value is deterministic (EffectDieSides 1, EffectRealPointsPerLevel 0).
    # scripts/runewords/check_spells.py re-checks all of that against a Spell.dbc.
    # `scale` is 100 throughout: a word grows by breadth (more auras, higher rungs),
    # never by a multiplier, so the roll's 50..100% is the only variance.
    {"id": 1, "name": "Bulwark", "cells": [("0:0", "Tal"), ("0:1", "Eth")],
     "spells": [9764, 13677, 9119], "scale": 100, "min_pct": 50, "required_level": 12},   # Armor 50, Block 20, +12 Sta
    {"id": 2, "name": "Malice", "cells": [("0:0", "Ith"), ("0:1", "El"), ("1:1", "Eth")],
     "spells": [4133, 14049], "scale": 100, "min_pct": 50, "required_level": 15},         # Bruise proc, AP 40
    {"id": 3, "name": "Strength", "cells": [("0:0", "Amn"), ("1:0", "Tir")],
     "spells": [14507, 9119], "scale": 100, "min_pct": 60, "required_level": 25},         # +35 Str, +12 Sta
    {"id": 4, "name": "Zephyr", "cells": [("0:0", "Ort"), ("0:1", "Eth")],
     "spells": [16615, 34935], "scale": 100, "min_pct": 50, "required_level": 20},        # Lightning Strike proc, Backlash when struck
    {"id": 5, "name": "Insight", "cells": [("0:0", "Ral"), ("1:0", "Ort"), ("2:0", "Tal")],
     "spells": [18378, 14415], "scale": 100, "min_pct": 50, "required_level": 25},        # Mana regen 10, +25 Int
    {"id": 6, "name": "Vigor", "cells": [("1:0", "Thul"), ("1:1", "Sol"), ("1:2", "Amn")],
     "spells": [14482, 20885, 14049], "scale": 100, "min_pct": 50, "required_level": 35}, # +35 Sta, Vitality 8, AP 40
    {"id": 7, "name": "Harmony", "cells": [("0:2", "Ko"), ("1:2", "Dol"), ("1:1", "Tir"), ("0:1", "Eth")],
     "spells": [14939, 14951, 20959], "scale": 100, "min_pct": 50, "required_level": 45}, # +45 Agi, +45 Spi, mana regen 12
    {"id": 8, "name": "Frenzy", "cells": [("0:0", "Um"), ("1:0", "Shael"), ("2:0", "Gul"), ("2:1", "Fal")],
     "spells": [14964, 14052, 7726], "scale": 100, "min_pct": 60, "required_level": 60},  # +46 Str, AP 60, Fire Strike 14
    {"id": 9, "name": "Nadir", "cells": [("0:0", "Nef"), ("1:0", "Tir")],
     "spells": [9762, 13387], "scale": 100, "min_pct": 50, "required_level": 8},          # Armor 30, Defense 12
    {"id": 10, "name": "Radiance", "cells": [("0:0", "Nef"), ("0:1", "Sol"), ("1:1", "Ith")],
     "spells": [15666, 21363, 14415], "scale": 100, "min_pct": 50, "required_level": 28}, # Armor 100, mana regen 6, +25 Int
    {"id": 11, "name": "Smoke", "cells": [("0:0", "Nef"), ("0:1", "Lum")],
     "spells": [18686, 15693], "scale": 100, "min_pct": 50, "required_level": 41},        # All Resist 15, Armor 150
    {"id": 12, "name": "Treachery", "cells": [("0:0", "Shael"), ("1:0", "Thul"), ("1:1", "Lem")],
     "spells": [18681, 14111, 14049], "scale": 100, "min_pct": 50, "required_level": 48}, # All Resist 10, Bloodpetal Poison, AP 40
    {"id": 13, "name": "Lawbringer", "cells": [("0:0", "Amn"), ("0:1", "Lem"), ("0:2", "Ko")],
     "spells": [21526, 14049, 4133], "scale": 100, "min_pct": 50, "required_level": 48},  # Holy Dam 40, AP 40, Bruise
    {"id": 14, "name": "Voice of Reason", "cells": [("0:0", "Lem"), ("0:1", "Ko"), ("1:1", "El"), ("1:2", "Eld")],
     "spells": [17900, 35188, 14415], "scale": 100, "min_pct": 50, "required_level": 48},# Frost Dam 40, Chilling Touch, +25 Int
    {"id": 15, "name": "Oath", "cells": [("0:0", "Shael"), ("0:1", "Pul"), ("1:1", "Mal"), ("1:2", "Lum")],
     "spells": [14963, 14052, 13670], "scale": 100, "min_pct": 50, "required_level": 55}, # +45 Str, AP 60, Dodge 24
    {"id": 16, "name": "Delirium", "cells": [("0:0", "Lem"), ("1:0", "Ist"), ("1:1", "Io")],
     "spells": [25820, 14945, 18378], "scale": 100, "min_pct": 50, "required_level": 58}, # Aura of Fear proc, +45 Int, mana regen 10
    {"id": 17, "name": "Heart of the Oak", "cells": [("0:0", "Ko"), ("0:1", "Vex"), ("0:2", "Pul"), ("0:3", "Thul")],
     "spells": [14945, 14951, 20959], "scale": 100, "min_pct": 50, "required_level": 63}, # +45 Int, +45 Spi, mana regen 12
    {"id": 18, "name": "Call to Arms", "cells": [("0:0", "Amn"), ("0:1", "Ral"), ("1:1", "Mal"), ("1:2", "Ist"), ("2:2", "Ohm")],
     "spells": [14958, 14964, 18378], "scale": 100, "min_pct": 50, "required_level": 65}, # +46 Sta, +46 Str, mana regen 10
    {"id": 19, "name": "Bramble", "cells": [("0:0", "Ral"), ("0:1", "Ohm"), ("1:1", "Sur"), ("1:2", "Eth")],
     "spells": [4436, 18686, 17997], "scale": 100, "min_pct": 50, "required_level": 70},  # Immolation thorns 10, All Resist 15, Nature Dam 40
    {"id": 20, "name": "Chains of Honor", "cells": [("0:0", "Dol"), ("0:1", "Um"), ("1:1", "Ber"), ("1:2", "Ist")],
     "spells": [18691, 15957, 14963], "scale": 100, "min_pct": 50, "required_level": 73}, # All Resist 20, Armor 300, +45 Str
    {"id": 21, "name": "Dream", "cells": [("0:0", "Io"), ("1:0", "Jah"), ("1:1", "Pul")],
     "spells": [16615, 21526, 14946], "scale": 100, "min_pct": 50, "required_level": 75}, # Lightning Strike, Holy Dam 40, +46 Int
    {"id": 22, "name": "Phoenix", "cells": [("0:0", "Vex"), ("0:1", "Vex"), ("1:1", "Lo"), ("1:2", "Jah")],
     "spells": [17875, 7726, 14803], "scale": 100, "min_pct": 50, "required_level": 75},  # Fire Dam 40, Fire Strike 14, Armor 200
    {"id": 23, "name": "Doom", "cells": [("0:0", "Hel"), ("0:1", "Ohm"), ("1:1", "Um"), ("1:2", "Lo"), ("2:2", "Cham")],
     "spells": [17900, 35188, 14964], "scale": 100, "min_pct": 50, "required_level": 78}, # Frost Dam 40, Chilling Touch, +46 Str
    {"id": 24, "name": "Breath of the Dying", "cells": [("0:0", "Vex"), ("0:1", "Hel"), ("0:2", "El"), ("0:3", "Eld"), ("1:3", "Zod"), ("1:2", "Eth")],
     "spells": [14964, 14940, 14958], "scale": 100, "min_pct": 50, "required_level": 80}, # +46 Str, +46 Agi, +46 Sta
]

WORDS_DDL = """CREATE TABLE `runeword_words` (
  `id` int unsigned NOT NULL,
  `name` varchar(48) NOT NULL,
  `pattern` varchar(255) NOT NULL COMMENT 'ordered row:col:entry triples, the word''s shape',
  `spell_1` int unsigned NOT NULL DEFAULT '0',
  `spell_2` int unsigned NOT NULL DEFAULT '0',
  `spell_3` int unsigned NOT NULL DEFAULT '0',
  `scale` smallint unsigned NOT NULL DEFAULT '100' COMMENT 'percent of the stock spell''s basepoints',
  `min_pct` tinyint unsigned NOT NULL DEFAULT '50' COMMENT 'floor on the rune-roll variance',
  `required_level` tinyint unsigned NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;"""

WORDS_COLUMNS = [
    "id", "name", "pattern", "spell_1", "spell_2", "spell_3",
    "scale", "min_pct", "required_level",
]


def pattern(cells) -> str:
    """Encode a word's cells as ``row:col:entry`` triples, in order."""
    return ",".join(f"{cell}:{rune_entry(RUNES.index(rune))}" for cell, rune in cells)


# The client lays every bag out four columns wide (FrameXML's
# NUM_CONTAINER_COLUMNS), and a container holds at most 72 slots, so eighteen
# rows is the tallest grid a pattern may address.
GRID_COLUMNS = 4
GRID_ROWS = 18


# ---------------------------------------------------------------------------
# Icons
# ---------------------------------------------------------------------------

def gem_display_ids(world: Path) -> list[int]:
    """The distinct display ids the base database's own gems use, in tier order.

    Runes borrow these: no ItemDisplayInfo row is added and no art is drawn, the
    client just needs the Item.dbc record to point at a display it already has.
    Sorted by the quality of the cheapest gem wearing it and then by entry, so
    the choice is stable across runs.
    """
    index, rows = read_table(world / "item_template.sql", "item_template")
    best: dict[int, tuple[int, int]] = {}
    for row in rows:
        if row[index["class"]] != 3:                          # ITEM_CLASS_GEM
            continue
        display = row[index["displayid"]]
        if display <= 0:
            continue
        rank = (row[index["Quality"]], row[index["entry"]])
        if display not in best or rank < best[display]:
            best[display] = rank
    ordered = [display for display, _ in sorted(best.items(), key=lambda kv: kv[1])]
    if len(ordered) < len(RUNES):
        raise SystemExit(
            f"{world / 'item_template.sql'} has only {len(ordered)} distinct gem displays; "
            f"the ladder needs {len(RUNES)}"
        )
    return ordered[: len(RUNES)]


# ---------------------------------------------------------------------------
# Rows
# ---------------------------------------------------------------------------

def build_items(world: Path):
    displays = gem_display_ids(world)
    rows = []
    for index, name in enumerate(RUNES):
        level = available_at(index)
        rows.append([
            rune_entry(index),
            15,                                   # ITEM_CLASS_MISC: not a gem, not socketable
            0,
            f"{name} Rune",
            displays[index],
            quality(index),
            0,                                    # Flags: not unique, not conjured
            1,                                    # BuyCount
            0,                                    # BuyPrice
            sell_price(index),                    # SellPrice: rarity floor, roll pays on top
            0,                                    # InventoryType: carried, never equipped
            -1,                                   # AllowableClass
            -1,                                   # AllowableRace
            level,                                # ItemLevel
            0,                                    # RequiredLevel (see the word's own gate)
            0,                                    # maxcount
            1,                                    # stackable: one rune per slot, the roll is per instance
            0,                                    # ContainerSlots
            0,                                    # bonding: runes stay tradable
            "A rune. Hold a pattern of them in your bags to form a runeword.",
            0,                                    # minMoneyLoot
            0,                                    # maxMoneyLoot
            0,                                    # flagsCustom
            12340,                                # VerifiedBuild
        ])
    return rows, displays


def build_words():
    rows = []
    for word in WORDS:
        cells = word["cells"]
        for cell, rune in cells:
            row_text, col_text = cell.split(":")
            row, col = int(row_text), int(col_text)
            if not (0 <= row < GRID_ROWS and 0 <= col < GRID_COLUMNS):
                raise SystemExit(f"word {word['name']}: cell {cell} is outside the 4x18 grid")
            if rune not in RUNES:
                raise SystemExit(f"word {word['name']}: unknown rune {rune}")
        if len({cell for cell, _ in cells}) != len(cells):
            raise SystemExit(f"word {word['name']}: repeated cell")

        # The declared order is a path the player can draw: a step right, left, up
        # or down each time, never a diagonal jump over a cell the word does not use.
        points = [tuple(int(part) for part in cell.split(":")) for cell, _ in cells]
        for (row_a, col_a), (row_b, col_b) in zip(points, points[1:]):
            if abs(row_a - row_b) + abs(col_a - col_b) != 1:
                raise SystemExit(
                    f"word {word['name']}: {row_a}:{col_a} and {row_b}:{col_b} are not "
                    f"adjacent; a word is a connected path"
                )
        spells = list(word["spells"]) + [0] * (3 - len(word["spells"]))
        rows.append([
            word["id"],
            word["name"],
            pattern(cells),
            spells[0], spells[1], spells[2],
            word["scale"],
            word["min_pct"],
            word["required_level"],
        ])
    return rows


def lua_table() -> str:
    """The client's copy of the tables, for the add-on shipped by the launcher.

    The add-on needs the same three facts the module loads - which entry is which
    rune, which pattern is which word, and what each word is worth - because it
    draws the bag highlight and the word panel before the server says anything.
    Generating it here is what keeps those two copies identical.
    """
    rune_names = {name: i for i, name in enumerate(RUNES)}
    lines = [
        "-- Generated by scripts/runewords/gen_runewords.py --lua; do not edit.",
        "--",
        "-- The add-on's copy of the rune ladder and the word table. It is emitted from the",
        "-- same declarations runewords_10_items.sql and runewords_20_words.sql are built",
        "-- from, so the cells the client highlights are the cells the server matches.",
        "RunewordsData = {",
        "\trunes = {",
    ]
    for index, name in enumerate(RUNES):
        level = available_at(index)
        lines.append(
            f'\t\t[{rune_entry(index)}] = {{ name = "{name}", step = {index + 1}, '
            f'level = {level}, quality = {quality(index)}, sell = {sell_price(index)} }},'
        )
    lines += ["\t},", "\twords = {"]
    for word in WORDS:
        cells = []
        for cell, rune in word["cells"]:
            row, col = (int(part) for part in cell.split(":"))
            cells.append(f"{{ {row}, {col}, {rune_entry(rune_names[rune])} }}")
        spells = ", ".join(str(s) for s in word["spells"] + [0] * (3 - len(word["spells"])))
        lines.append(
            f'\t\t{{ id = {word["id"]}, name = "{word["name"]}", '
            f'cells = {{ {", ".join(cells)} }}, spells = {{ {spells} }}, '
            f'scale = {word["scale"]}, minPct = {word["min_pct"]}, '
            f'level = {word["required_level"]} }},'
        )
    lines += ["\t},", "}", ""]
    return "\n".join(lines)


def rune_ref(bracket_index: int) -> int:
    return RUNE_REF_BASE + bracket_index


def build_reference_tables(used_brackets):
    """One table per bracket a creature actually rolls.

    A table nothing points at is logged by the core at startup as "isn't
    reference id and not referenced from loot", so only the referenced brackets
    are written.
    """
    rows = []
    for bracket_index in sorted(used_brackets):
        lo, hi = BRACKETS[bracket_index - 1]
        ref_entry = rune_ref(bracket_index)
        for index, name in enumerate(RUNES):
            if available_at(index) > hi:
                continue
            rows.append([
                ref_entry,
                rune_entry(index),
                0,
                weight(index, hi),
                0,
                1,
                RUNE_GROUP,
                1,
                1,
                f"{lo}-{hi} {name} Rune",
            ])
    return rows


def build_creature_drops(world: Path):
    creature_index, creature_rows = read_table(world / "creature_template.sql", "creature_template")

    by_loot_id = {}
    base_loot_ids = set()
    for row in creature_rows:
        loot_id = row[creature_index["lootid"]]
        min_level = row[creature_index["minlevel"]]
        if not loot_id or min_level <= 0:
            continue
        base_loot_ids.add(loot_id)
        if row[creature_index["type"]] == 8:                  # critters drop nothing
            continue
        # A loot id the base dumps still carry but the assembled world database
        # does not reference would be logged at startup as "isn't creature entry
        # and not referenced from loot". The list is the loot-bag generator's,
        # and it fails there if an entry stops existing, so a refreshed pin
        # forces both generators to drop it.
        if loot_id in loot.RETIRED_LOOT_IDS:
            continue
        rank = min(len(RUNE_CHANCE) - 1, max(0, row[creature_index["rank"]]))
        # A loot id shared by several templates takes its bracket from the median
        # level and its rank from the highest rank wearing it.
        by_loot_id.setdefault(loot_id, []).append((min_level, rank))

    stale = sorted(loot.RETIRED_LOOT_IDS - base_loot_ids)
    if stale:
        raise SystemExit(
            f"RETIRED_LOOT_IDS {stale} no longer exist in {world / 'creature_template.sql'}; "
            "the loot-bag generator's list needs them dropped"
        )

    loot_index, loot_rows = read_table(world / "creature_loot_template.sql", "creature_loot_template")
    max_group = max(row[loot_index["GroupId"]] for row in loot_rows)
    if max_group >= RUNE_GROUP:
        raise SystemExit(
            f"creature_loot_template already uses GroupId {max_group}; "
            f"the rune group is {RUNE_GROUP}"
        )

    rows = []
    used_brackets = set()
    for loot_id in sorted(by_loot_id):
        records = by_loot_id[loot_id]
        levels = sorted(level for level, _ in records)
        bracket_index = min(
            len(BRACKETS),
            max(1, (levels[len(levels) // 2] - 1) // loot.BRACKET_WIDTH + 1),
        )
        rank = max(rank for _, rank in records)
        ref = rune_ref(bracket_index)
        lo, hi = BRACKETS[bracket_index - 1]
        rows.append([
            loot_id, ref, ref, RUNE_CHANCE[rank], 0, 1, RUNE_GROUP, 1, 1,
            f"runeword rune (level {lo}-{hi})",
        ])
        used_brackets.add(bracket_index)
    return rows, used_brackets


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def validate_outputs(world: Path, item_rows, word_rows, ref_rows, creature_rows, displays):
    if len(item_rows) != len(RUNES):
        raise SystemExit(f"{len(item_rows)} rune rows for {len(RUNES)} runes")
    entries = [row[0] for row in item_rows]
    if entries != list(range(RUNE_ENTRY_BASE, RUNE_ENTRY_BASE + len(RUNES))):
        raise SystemExit("rune entries are not the contiguous ladder")
    if len(set(displays)) != len(displays):
        raise SystemExit("two runes borrowed the same display id")
    if len({row[0] for row in word_rows}) != len(word_rows):
        raise SystemExit("duplicate runeword word id")
    known = set(entries)
    for row in word_rows:
        for triple in row[2].split(","):
            _row, _col, entry = triple.split(":")
            if int(entry) not in known:
                raise SystemExit(f"word {row[1]} names rune {entry}, which is not in the ladder")
        if not any(row[3:6]):
            raise SystemExit(f"word {row[1]} has no spells")
        if not 1 <= row[7] <= 100:
            raise SystemExit(f"word {row[1]} has min_pct {row[7]}")
    refs = {row[0] for row in ref_rows}
    for row in creature_rows:
        if row[1] not in refs:
            raise SystemExit(f"loot id {row[0]} points at reference {row[1]}, which is not written")

    # Every rune must be *used*: a rune no word names is an item whose only purpose is
    # its price, and the add-on says exactly that on its tooltip. The ladder is the
    # design's vocabulary, so a rune with no word is a hole in it.
    named = set()
    for row in word_rows:
        for triple in row[2].split(","):
            named.add(int(triple.split(":")[2]))
    unused = [name for index, name in enumerate(RUNES) if rune_entry(index) not in named]
    if unused:
        raise SystemExit(f"runes in no word: {unused}")

    # A rune must be reachable: every rune appears in the table of the bracket
    # that matches its availability, and no table holds a rune that does not
    # exist yet.
    seen = {row[1] for row in ref_rows}
    missing = [name for index, name in enumerate(RUNES) if rune_entry(index) not in seen]
    if missing:
        raise SystemExit(f"runes in no drop table: {missing}")


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def build_outputs(world: Path):
    item_rows, displays = build_items(world)
    word_rows = build_words()
    creature_rows, used_brackets = build_creature_drops(world)
    ref_rows = build_reference_tables(used_brackets)
    validate_outputs(world, item_rows, word_rows, ref_rows, creature_rows, displays)

    items_sql = file_header(
        "Runewords: the rune ladder as item templates.",
        f"{len(item_rows)} runes, ids {RUNE_ENTRY_BASE}-{RUNE_ENTRY_BASE + len(RUNES) - 1}.",
    )
    items_sql += [
        f"DELETE FROM `item_template` WHERE `entry` BETWEEN {RUNE_ENTRY_DELETE_FROM} AND {RUNE_ENTRY_DELETE_TO};",
        "",
        insert_statement("item_template", ITEM_INSERT_COLUMNS, item_rows),
        "",
    ]

    words_sql = file_header(
        "Runewords: the words, as ordered cell patterns over the 4-wide bag grid.",
        f"{len(word_rows)} words.",
    )
    words_sql += [
        "DROP TABLE IF EXISTS `runeword_words`;",
        WORDS_DDL,
        "",
        insert_statement("runeword_words", WORDS_COLUMNS, word_rows),
        "",
    ]

    tables_sql = file_header(
        "Runewords: one reference table per level bracket a creature actually rolls.",
        f"{len({row[0] for row in ref_rows})} tables, {len(ref_rows)} rows.",
    )
    tables_sql += [
        f"DELETE FROM `reference_loot_template` WHERE `Entry` BETWEEN {RUNE_REF_DELETE_FROM} AND {RUNE_REF_DELETE_TO};",
        "",
        insert_statement("reference_loot_template", LOOT_INSERT_COLUMNS, ref_rows),
        "",
    ]

    drops_sql = file_header(
        "Runewords: one independent drop row per lootable creature template loot id.",
        f"{len(creature_rows)} creature loot ids.",
    )
    drops_sql += [
        f"DELETE FROM `creature_loot_template` WHERE `Reference` BETWEEN {RUNE_REF_DELETE_FROM} AND {RUNE_REF_DELETE_TO};",
        "",
        insert_statement("creature_loot_template", LOOT_INSERT_COLUMNS, creature_rows),
        "",
    ]

    return {
        "runewords_10_items.sql": "\n".join(items_sql),
        "runewords_20_words.sql": "\n".join(words_sql),
        "runewords_30_drop_tables.sql": "\n".join(tables_sql),
        "runewords_40_creature_drops.sql": "\n".join(drops_sql),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--core", required=True, type=Path, help="checkout of the pinned AzerothCore tree")
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO / "sql" / "custom" / "db_world",
        help="output directory (default: sql/custom/db_world)",
    )
    parser.add_argument("--check", action="store_true", help="fail if the committed SQL differs from a fresh run")
    parser.add_argument(
        "--lua",
        type=Path,
        help="also write the add-on's data table here (acore-thor-launcher: "
             "app/src/main/assets/runewords/Runewords_Data.lua)",
    )
    args = parser.parse_args()

    world = args.core / "data" / "sql" / "base" / "db_world"
    if not world.is_dir():
        raise SystemExit(f"{world} is not a checkout of the pinned core's base SQL dumps")

    outputs = build_outputs(world)

    targets = {str(args.out / name): text for name, text in outputs.items()}
    if args.lua is not None:
        targets[str(args.lua)] = lua_table()

    if args.check:
        stale = []
        for path, text in targets.items():
            committed = Path(path).read_text(encoding="utf-8") if Path(path).exists() else ""
            if committed != text:
                stale.append((path, committed, text))
        if stale:
            for name, committed, text in stale:
                sys.stderr.writelines(
                    difflib.unified_diff(
                        committed.splitlines(keepends=True),
                        text.splitlines(keepends=True),
                        fromfile=f"{name} (committed)",
                        tofile=f"{name} (regenerated)",
                    )
                )
            return 1
        print(f"runewords: {len(targets)} files up to date")
        return 0

    for path, text in targets.items():
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(text, encoding="utf-8")
        print(f"wrote {path} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
