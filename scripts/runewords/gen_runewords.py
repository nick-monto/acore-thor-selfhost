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
import dbc_mpq  # noqa: E402

# SpellEntry and SpellDuration.dbc field indices, as DBCStructure.h numbers them for this
# client - the same ones check_spells.py reads.
SPELL_FIELDS = 234
DURATION_FIELDS = 4
EFFECT, BASEPOINTS, PROC_CHANCE = 71, 80, 35
AURA, MISC, TRIGGER_SPELL = 95, 110, 116
SCHOOL = 225
PROC_AURA = 42
SCHOOL_DAMAGE = 2


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
    #
    # Two things keep a word's *power* in step with its price. The rune count decides
    # `scale` (see WORD_SCALE_BY_RUNES): a six-rune word lends the stock auras at twice
    # their printed value, a two-rune word at face value, so the bag space a word costs
    # is what it is worth. The proc auras carry their damage in the DBC and cannot be
    # scaled at all, so those are *banded*: a word gated at 20 borrows a ~100 damage
    # proc, one gated at 48 a ~220, one gated at 60 a ~220 at 25%, and the 70+ words
    # borrow the several-hundred-to-a-few-thousand ones. A word therefore reads as
    # impactful at the level it unlocks and above, never as a rounding error.
    # Bulwark is D2's 2.6 helm word, so it carries D2's runes (Shael Io Sol) rather than the
    # Tal/Eth pair it launched with: Stealth is the Tal/Eth word, and two words that answer to
    # the same runes are one word with two names.
    {"id": 1, "name": "Bulwark", "runes": ["Shael", "Io", "Sol"],
     "spells": [9764, 13677, 9119], "required_level": 36},   # Armor 50, Block 20, +12 Sta
    {"id": 2, "name": "Malice", "runes": ["Ith", "El", "Eth"],
     "spells": [4133, 14049], "required_level": 15},         # Bruise proc, AP 40
    {"id": 3, "name": "Strength", "runes": ["Amn", "Tir"],
     "spells": [14507, 9119], "required_level": 25},         # +35 Str, +12 Sta
    {"id": 4, "name": "Zephyr", "runes": ["Ort", "Eth"],
     "spells": [29150, 34935], "required_level": 20},                      # Electric Discharge 99 nature, Backlash when struck
    # Insight is Ral Tir Tal Sol in D2; the shipped shape was a three-rune simplification that
    # collided with Ancients' Pledge (Ral Ort Tal), so it is the real sequence now.
    {"id": 5, "name": "Insight", "runes": ["Ral", "Tir", "Tal", "Sol"],
     "spells": [18378, 14415], "required_level": 28},        # Mana regen 10, +25 Int
    {"id": 6, "name": "Vigor", "runes": ["Thul", "Sol", "Amn"],
     "spells": [14482, 20885, 14049], "required_level": 35}, # +35 Sta, Vitality 8, AP 40
    {"id": 7, "name": "Harmony", "runes": ["Ko", "Dol", "Tir", "Eth"],
     "spells": [14939, 14951, 20959], "required_level": 45}, # +45 Agi, +45 Spi, mana regen 12
    {"id": 8, "name": "Frenzy", "runes": ["Um", "Shael", "Gul", "Fal"],
     "spells": [14964, 14052, 15573], "required_level": 60},              # +46 Str, AP 60, Fire Blast 219 fire
    {"id": 9, "name": "Nadir", "runes": ["Nef", "Tir"],
     "spells": [9762, 13387], "required_level": 8},          # Armor 30, Defense 12
    {"id": 10, "name": "Radiance", "runes": ["Nef", "Sol", "Ith"],
     "spells": [15666, 21363, 14415], "required_level": 28}, # Armor 100, mana regen 6, +25 Int
    {"id": 11, "name": "Smoke", "runes": ["Nef", "Lum"],
     "spells": [18686, 15693], "required_level": 41},        # All Resist 15, Armor 150
    {"id": 12, "name": "Treachery", "runes": ["Shael", "Thul", "Lem"],
     "spells": [18681, 35184, 14049], "required_level": 48},              # All Resist 10, Unstable Affliction 508 shadow, AP 40
    {"id": 13, "name": "Lawbringer", "runes": ["Amn", "Lem", "Ko"],
     "spells": [21526, 14049, 4133], "required_level": 48},  # Holy Dam 40, AP 40, Bruise
    {"id": 14, "name": "Voice of Reason", "runes": ["Lem", "Ko", "El", "Eld"],
     "spells": [17900, 35188, 14415], "required_level": 48},# Frost Dam 40, Chilling Touch, +25 Int
    {"id": 15, "name": "Oath", "runes": ["Shael", "Pul", "Mal", "Lum"],
     "spells": [14963, 14052, 13670], "required_level": 55}, # +45 Str, AP 60, Dodge 24
    {"id": 16, "name": "Delirium", "runes": ["Lem", "Ist", "Io"],
     "spells": [25820, 14945, 18378], "required_level": 58}, # Aura of Fear proc, +45 Int, mana regen 10
    {"id": 17, "name": "Heart of the Oak", "runes": ["Ko", "Vex", "Pul", "Thul"],
     "spells": [14945, 14951, 20959], "required_level": 63}, # +45 Int, +45 Spi, mana regen 12
    {"id": 18, "name": "Call to Arms", "runes": ["Amn", "Ral", "Mal", "Ist", "Ohm"],
     "spells": [14958, 14964, 18378], "required_level": 65}, # +46 Sta, +46 Str, mana regen 10
    {"id": 19, "name": "Bramble", "runes": ["Ral", "Ohm", "Sur", "Eth"],
     "spells": [4436, 18686, 17997], "required_level": 70},  # Immolation thorns 10, All Resist 15, Nature Dam 40
    {"id": 20, "name": "Chains of Honor", "runes": ["Dol", "Um", "Ber", "Ist"],
     "spells": [18691, 15957, 14963], "required_level": 73}, # All Resist 20, Armor 300, +45 Str
    {"id": 21, "name": "Dream", "runes": ["Io", "Jah", "Pul"],
     "spells": [17690, 21526, 14946], "required_level": 75},              # Disease Burst 749 nature, Holy Dam 40, +46 Int
    {"id": 22, "name": "Phoenix", "runes": ["Vex", "Vex", "Lo", "Jah"],
     "spells": [17875, 32339, 14803], "required_level": 75},              # Fire Dam 40, Fiery Broom 2374 fire, Armor 200
    {"id": 23, "name": "Doom", "runes": ["Hel", "Ohm", "Um", "Lo", "Cham"],
     "spells": [17900, 35188, 14964], "required_level": 78}, # Frost Dam 40, Chilling Touch, +46 Str
    {"id": 24, "name": "Breath of the Dying", "runes": ["Vex", "Hel", "El", "Eld", "Zod", "Eth"],
     "spells": [14964, 14940, 14958], "required_level": 80}, # +46 Str, +46 Agi, +46 Sta

    # ---------------------------------------------------------------------------------
    # The second tranche: D2's words from Nadir's tier up to Memory. Every word is declared by its
    # rune sequence and the axis follows the rule in line_of; the level is the drop level of its
    # rarest rune, so a word is never formable before its last rune exists in the world.
    # ---------------------------------------------------------------------------------
    {"id": 25, "name": "Steel", "runes": ["Tir", "El"],
     "spells": [14049, 9764]},                                   # AP 40, Armor 50
    {"id": 26, "name": "Stealth", "runes": ["Tal", "Eth"],
     "spells": [18378, 13670]},                                  # Mana regen 10, Dodge 24
    {"id": 27, "name": "Leaf", "runes": ["Tir", "Ral"],
     "spells": [17875, 15573]},                                  # Fire Dam 40, Fire Blast proc
    {"id": 28, "name": "Ancients' Pledge", "runes": ["Ral", "Ort", "Tal"],
     "spells": [18686, 15666]},                                  # All Resist 15, Armor 100
    {"id": 29, "name": "Holy Thunder", "runes": ["Eth", "Ral", "Ort", "Tal"],
     "spells": [21526, 29150]},                                  # Holy Dam 40, Electric Discharge
    {"id": 30, "name": "Pattern", "runes": ["Tal", "Ort", "Thul"],
     "spells": [15573, 18681, 14939]},                           # Fire Blast, All Resist 10, +45 Agi
    {"id": 31, "name": "Edge", "runes": ["Tir", "Tal", "Amn"],
     "spells": [4436, 14049]},                                   # Immolation thorns, AP 40
    {"id": 32, "name": "King's Grace", "runes": ["Amn", "Ral", "Thul"],
     "spells": [21526, 14507, 14049]},                           # Holy Dam 40, +35 Str, AP 40
    {"id": 33, "name": "Myth", "runes": ["Hel", "Amn", "Nef"],
     "spells": [4436, 14482]},                                   # Immolation thorns, +35 Sta
    {"id": 34, "name": "Spirit", "runes": ["Tal", "Thul", "Ort", "Amn"],
     "spells": [14415, 20959, 14049]},                           # +25 Int, mana regen 13, AP 40
    {"id": 35, "name": "Honor", "runes": ["Amn", "El", "Ith", "Tir", "Sol"],
     "spells": [14963, 14052, 13670]},                           # +45 Str, AP 60, Dodge 24
    {"id": 36, "name": "Lore", "runes": ["Ort", "Sol"],
     "spells": [14415, 18681]},                                  # +25 Int, All Resist 10
    {"id": 37, "name": "Authority", "runes": ["Hel", "Shael", "Ral"],
     "spells": [18681, 14958]},                                  # All Resist 10, +46 Sta
    {"id": 38, "name": "Peace", "runes": ["Shael", "Thul", "Amn"],
     "spells": [13670, 14482]},                                  # Dodge 24, +35 Sta
    {"id": 39, "name": "Rhyme", "runes": ["Shael", "Eth"],
     "spells": [13677, 18378]},                                  # Block 20, mana regen 10
    {"id": 40, "name": "Black", "runes": ["Thul", "Io", "Nef"],
     "spells": [35184, 14507]},                                  # Unstable Affliction, +35 Str
    {"id": 41, "name": "White", "runes": ["Dol", "Io"],
     "spells": [14945, 21363]},                                  # +45 Int, mana regen 6
    {"id": 42, "name": "Ground", "runes": ["Shael", "Io", "Ort"],
     "spells": [15693, 14958]},                                  # Armor 150, +46 Sta
    {"id": 43, "name": "Temper", "runes": ["Shael", "Io", "Ral"],
     "spells": [17875, 15693]},                                  # Fire Dam 40, Armor 150
    {"id": 44, "name": "Hearth", "runes": ["Shael", "Io", "Thul"],
     "spells": [17900, 14958]},                                  # Frost Dam 40, +46 Sta
    {"id": 45, "name": "Cure", "runes": ["Shael", "Io", "Tal"],
     "spells": [18686, 20885]},                                  # All Resist 15, Vitality 8
    {"id": 46, "name": "Memory", "runes": ["Lum", "Io", "Sol", "Eth"],
     "spells": [14945, 20959, 15666]},                           # +45 Int, mana regen 13, Armor 100
]

WORDS_DDL = """CREATE TABLE `runeword_words` (
  `id` int unsigned NOT NULL,
  `name` varchar(48) NOT NULL,
  `pattern` varchar(255) NOT NULL COMMENT 'ordered row:col:entry triples, the word''s shape',
  `spell_1` int unsigned NOT NULL DEFAULT '0',
  `spell_2` int unsigned NOT NULL DEFAULT '0',
  `spell_3` int unsigned NOT NULL DEFAULT '0',
  `scale` smallint unsigned NOT NULL DEFAULT '100' COMMENT 'percent of the stock spell''s basepoints',
  `required_level` tinyint unsigned NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;"""

WORDS_COLUMNS = [
    "id", "name", "pattern", "spell_1", "spell_2", "spell_3",
    "scale", "required_level",
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


# ---------------------------------------------------------------------------
# Shapes
# ---------------------------------------------------------------------------
#
# A word is a straight line: it reads across, or down, the way a line of runes does. Nothing else
# is legible in a bag without being told - two runes on a diagonal are a guess - and the straight
# line is what lets two words share a rune: one rune can stand in a line across and in a line down
# at the same time, so a player who crosses two words has spent the shared rune once. The module
# matches *maximal* runs only, so a line that carries a rune past the word's own ends forms
# nothing; every word therefore has to be placeable with clean ends, which it always is.
#
# Across is four cells at most (a bag is four columns wide); down is as many rows as the bag
# draws. A word longer than a row has no choice but to run down.
LINE_ACROSS = "H"
LINE_DOWN = "V"


def line_of(word: dict) -> str:
    """The axis a word is written on.

    A word alternates axis with its id. Ids are the ladder order, so consecutive ids are the
    neighbouring tiers, and alternating them gives every tier both an across word and a down word
    to try. A word longer than a row has no choice: a bag is four columns wide.
    """
    if len(word["runes"]) > GRID_COLUMNS:
        return LINE_DOWN
    return LINE_ACROSS if word["id"] % 2 else LINE_DOWN


def line_cells(word: dict, taken: set) -> list[tuple[str, str]]:
    """``[(row:col, rune)]`` for a word, in reading order.

    A straight line's cells follow from its length alone, so two words can only be the same word
    when they are on the same axis with the same runes; what ``taken`` keeps is exactly that pair.
    The same runes may still be two words if they are written on different axes, which is a
    distinction the player can see.
    """
    runes = tuple(word["runes"])
    axis = line_of(word)
    if (axis, runes) in taken:
        raise SystemExit(f"word {word['name']}: {'across' if axis == LINE_ACROSS else 'down'} "
                         f"{' '.join(runes)} is already another word's line")
    taken.add((axis, runes))
    if axis == LINE_ACROSS:
        return [(f"0:{col}", rune) for col, rune in enumerate(runes)]
    return [(f"{row}:0", rune) for row, rune in enumerate(runes)]


# A word's `scale` is what the module multiplies the stock auras' basepoints by: the
# bag it occupies is its budget, so two runes lend the printed value and every extra
# rune adds a quarter of it. Without this a six-rune word grants the same +46 Strength
# as a two-rune one, which is the opposite of a reason to fill the bag.
WORD_SCALE_BY_RUNES = {2: 100, 3: 125, 4: 150, 5: 175, 6: 200}


def resolved_words() -> list[dict]:
    """The WORDS table with its derived fields filled in.

    A word is declared by its ``runes`` and gets the line they are written on, and a word
    without an explicit ``required_level`` unlocks at the level its rarest rune starts
    dropping - which is the level the player can first hold it at all. Resolved once and read
    by both writers, so the SQL rows and the add-on's table are the same table.
    """
    taken: set = set()

    out = []
    for declaration in WORDS:
        word = dict(declaration)
        word["cells"] = line_cells(word, taken)
        if "required_level" not in word:
            word["required_level"] = available_at(max(RUNES.index(rune) for _, rune in word["cells"]))
        out.append(word)
    return out


def word_scale(rune_count: int) -> int:
    if rune_count not in WORD_SCALE_BY_RUNES:
        raise SystemExit(f"a word of {rune_count} rune(s) has no scale; add it to WORD_SCALE_BY_RUNES")
    return WORD_SCALE_BY_RUNES[rune_count]


def build_words():
    rows = []
    for word in resolved_words():
        cells = word["cells"]
        if len(cells) < 2:
            raise SystemExit(f"word {word['name']}: a word needs at least two runes")
        for cell, rune in cells:
            row_text, col_text = cell.split(":")
            row, col = int(row_text), int(col_text)
            if not (0 <= row < GRID_ROWS and 0 <= col < GRID_COLUMNS):
                raise SystemExit(f"word {word['name']}: cell {cell} is outside the 4x18 grid")
            if rune not in RUNES:
                raise SystemExit(f"word {word['name']}: unknown rune {rune}")
        if len({cell for cell, _ in cells}) != len(cells):
            raise SystemExit(f"word {word['name']}: repeated cell")

        # The line, not just its cells: across has to fit the bag's four columns, and down has
        # to fit the tallest grid a container can address.
        rows_used = {int(cell.split(":")[0]) for cell, _ in cells}
        cols_used = {int(cell.split(":")[1]) for cell, _ in cells}
        if len(rows_used) != 1 and len(cols_used) != 1:
            raise SystemExit(f"word {word['name']}: the cells are neither one row nor one column")
        if len(rows_used) == 1 and len(cells) > GRID_COLUMNS:
            raise SystemExit(f"word {word['name']}: {len(cells)} runes do not lie across a "
                             f"{GRID_COLUMNS}-wide bag")
        if len(cols_used) == 1 and len(cells) > GRID_ROWS:
            raise SystemExit(f"word {word['name']}: {len(cells)} runes do not lie down the "
                             f"{GRID_ROWS}-row grid")

        spells = list(word["spells"]) + [0] * (3 - len(word["spells"]))
        rows.append([
            word["id"],
            word["name"],
            pattern(cells),
            spells[0], spells[1], spells[2],
            word_scale(len(cells)),
            word["required_level"],
        ])
    return rows


# ---------------------------------------------------------------------------
# Effect labels
# ---------------------------------------------------------------------------
#
# The add-on has to say what a word does in numbers, and a word lends stock spells whose
# printed amounts mean nothing to the player: "Increased Armor 50" at a 71% roll is 35
# armour, and a proc carries its damage in its *triggered* spell, not in its own. So the
# generator reads the same Spell.dbc the realm loads and writes each word's effects out
# with the amount the player will actually see at a 100% roll; the add-on only scales
# that by the rune roll.
#
# (aura type, misc value) -> what the player calls it. Anything not named here falls back
# to the stock spell's own name, which is still the truth, just not a friendly one.
STAT_NAMES = {0: "Strength", 1: "Agility", 2: "Stamina", 3: "Intellect", 4: "Spirit"}
SCHOOL_NAMES = {1: "physical", 2: "holy", 4: "fire", 8: "nature", 16: "frost", 32: "shadow", 64: "arcane"}
# The panel is a few hundred pixels wide and prints three of these side by side, so the labels
# are the short form a player reads on a rating cell, not the spell's own wording.
RATING_NAMES = {1: "Dodge", 2: "Defense", 3: "Parry", 4: "Dodge", 16: "Block"}
RESIST_NAMES = {1: "Armor", 2: "Holy res", 4: "Fire res", 8: "Nature res",
                16: "Frost res", 32: "Shadow res", 64: "Arcane res"}


def spell_label(aura: int, misc: int) -> str | None:
    if aura == 22:
        if misc > 64:
            return "All res"
        return RESIST_NAMES.get(misc)
    if aura == 29:
        return STAT_NAMES.get(misc)
    if aura == 189:
        return RATING_NAMES.get(misc)
    if aura == 13:
        return f"{SCHOOL_NAMES.get(misc, 'Spell').title()} damage"
    if aura == 15:
        return "Damage shield"
    if aura == 85:
        return "Mana regen"
    if aura == 99:
        return "Attack power"
    if aura == 161:
        return "Health regen"
    return None


def effect_descriptors(spells, spell_dbc, duration_dbc) -> list[dict]:
    """One descriptor per borrowed spell: what it does and how much of it."""
    spell_records, spell_strings = dbc_mpq.read_dbc(spell_dbc, SPELL_FIELDS)
    duration_records, _ = dbc_mpq.read_dbc(duration_dbc, DURATION_FIELDS)
    durations = {record[0]: record[1] for record in duration_records}
    spells_by_id = {record[0]: record for record in spell_records}

    def text(record, field) -> str:
        offset = record[field]
        if not offset:
            return ""
        end = spell_strings.find(b"\0", offset)
        return spell_strings[offset:end].decode("utf-8", "replace")

    out = []
    for spell_id in spells:
        if not spell_id:
            continue
        record = spells_by_id[spell_id]
        name = text(record, 136).strip()

        if record[AURA] == PROC_AURA:
            # A proc carries its payload in the spell it triggers: a flat damage spell has the
            # number, a weapon-damage one has a negative basepoint and scales with the hit, and
            # anything else is a debuff the panel can only name.
            trigger = spells_by_id.get(record[TRIGGER_SPELL])
            row = {"label": name, "proc": True, "amount": 0, "kind": "effect",
                   "chance": record[PROC_CHANCE]}
            if trigger:
                row["label"] = text(trigger, 136).strip() or name
                base = trigger[BASEPOINTS] - (1 << 32) if trigger[BASEPOINTS] >= (1 << 31) else trigger[BASEPOINTS]
                row["school"] = "/".join(SCHOOL_NAMES[m] for m in sorted(SCHOOL_NAMES)
                                         if trigger[SCHOOL] & m) or "damage"
                if trigger[EFFECT] == SCHOOL_DAMAGE and base > 0:
                    row["amount"], row["kind"] = base, "damage"
                elif base < 0:
                    row["kind"] = "weapon"
            out.append(row)
            continue

        # The amount the aura actually applies is the effect's basepoints: it is the stock spell
        # name that is rounded ("Increased Stamina 12" grants 11), and the module scales those
        # basepoints, so this is the number the player sees.
        label = spell_label(record[AURA], record[MISC])
        out.append({"label": label or name, "amount": record[BASEPOINTS], "proc": False})
    return out


def lua_table(effects_for=None) -> str:
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
    for word in resolved_words():
        cells = []
        for cell, rune in word["cells"]:
            row, col = (int(part) for part in cell.split(":"))
            cells.append(f"{{ {row}, {col}, {rune_entry(rune_names[rune])} }}")
        spells = ", ".join(str(s) for s in word["spells"] + [0] * (3 - len(word["spells"])))
        lines.append(
            f'\t\t{{ id = {word["id"]}, name = "{word["name"]}", '
            f'cells = {{ {", ".join(cells)} }}, spells = {{ {spells} }}, '
            f'scale = {word_scale(len(word["cells"]))}, '
            f'level = {word["required_level"]}' +
            (f', effects = {{ {effects_for(word)} }}' if effects_for else '') + ' },'
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
    # A rune sequence names one word. Two words that answer to the same runes are one word
    # with two names: the bags form both, and the panel cannot tell the player which is which.
    sequences = {}
    for row in word_rows:
        key = tuple(int(triple.split(":")[2]) for triple in row[2].split(","))
        if key in sequences:
            raise SystemExit(f"words {sequences[key]} and {row[1]} are drawn from the same runes")
        sequences[key] = row[1]

    patterns = [row[2] for row in word_rows]
    if len(set(patterns)) != len(patterns):
        seen, offender = set(), None
        for row in word_rows:
            if row[2] in seen:
                offender = row[1]
                break
            seen.add(row[2])
        raise SystemExit(f"word {offender} draws the same runes in the same cells as another word")
    known = set(entries)
    for row in word_rows:
        for triple in row[2].split(","):
            _row, _col, entry = triple.split(":")
            if int(entry) not in known:
                raise SystemExit(f"word {row[1]} names rune {entry}, which is not in the ladder")
        if not any(row[3:6]):
            raise SystemExit(f"word {row[1]} has no spells")
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
        "--spell-dbc", type=Path,
        help="the client's Spell.dbc; with it, --lua also carries each word's effect labels "
             "and printed amounts",
    )
    parser.add_argument(
        "--duration-dbc", type=Path,
        help="the client's SpellDuration.dbc (needed by --spell-dbc, not used by the labels)",
    )
    parser.add_argument(
        "--lua",
        type=Path,
        help="also write the add-on's data table here (acore-thor-launcher: "
             "app/src/main/assets/runewords/Runewords_Data.lua); needs --spell-dbc, because the "
             "panel prints each word's effects and their amounts",
    )
    args = parser.parse_args()

    world = args.core / "data" / "sql" / "base" / "db_world"
    if not world.is_dir():
        raise SystemExit(f"{world} is not a checkout of the pinned core's base SQL dumps")

    if args.lua is not None and args.spell_dbc is None:
        raise SystemExit(
            "gen_runewords.py: --lua needs --spell-dbc: the add-on's panel prints each word's "
            "effects, and those are read out of the client's own Spell.dbc"
        )

    outputs = build_outputs(world)

    targets = {str(args.out / name): text for name, text in outputs.items()}
    if args.lua is not None:
        effects_for = None
        if args.spell_dbc is not None:
            duration = args.duration_dbc or args.spell_dbc.with_name("SpellDuration.dbc")

            def effects_for(word, _dbc=args.spell_dbc, _duration=duration):
                rows = effect_descriptors(word["spells"], _dbc, _duration)
                parts = []
                for row in rows:
                    if row["proc"]:
                        parts.append(f'{{ label = "{row["label"]}", proc = true, kind = "{row["kind"]}", '
                                     f'amount = {row["amount"]}, school = "{row.get("school", "damage")}", '
                                     f'chance = {row["chance"]} }}')
                    else:
                        parts.append(f'{{ label = "{row["label"]}", amount = {row["amount"]} }}')
                return ", ".join(parts)

        targets[str(args.lua)] = lua_table(effects_for)

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
