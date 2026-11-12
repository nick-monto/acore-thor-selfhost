#!/usr/bin/env python3
"""Check every spell the runeword table lends to the player against a real Spell.dbc.

mod-runewords casts a word's spells once and leaves the auras up until the pattern
breaks, so only a stock spell that is already permanent, deterministic and
self-targeted can be borrowed. This re-derives that from the client's own DBC - the
file the server loads at start - instead of trusting the comment beside each id in
the generator:

  * the spell exists in Spell.dbc;
  * ``Effect_1`` is APPLY_AURA (6);
  * ``Duration.dbc[DurationIndex]`` is -1 (permanent): a finite aura would lapse and
    never come back, because the module only re-derives on a shape change;
  * ``ImplicitTargetA_1`` is 1 (the caster): a word buffs its holder, not a target;
  * ``EffectDieSides_1`` is 0 or 1 (one outcome) and ``EffectRealPointsPerLevel_1`` is 0, so the
    amount is the basepoints and the roll scaling means what it says.

Usage, against the client data the realm has unpacked::

    python3 scripts/runewords/check_spells.py \\
        --spell-dbc /opt/acore/data/dbc/Spell.dbc \\
        --duration-dbc /opt/acore/data/dbc/SpellDuration.dbc

Exits non-zero and names every offending id when a word borrows something the
module cannot honour.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "lib"))
import dbc_mpq  # noqa: E402

# SpellEntry field indices, as DBCStructure.h numbers them for this client.
ID, DURATION, EFFECT, DIE_SIDES, REAL_PPL, BASEPOINTS, TARGET_A, AURA, MISC, NAME = (
    0, 40, 71, 74, 77, 80, 86, 95, 110, 136)
SPELL_FIELDS = 234
DURATION_FIELDS = 4
APPLY_AURA = 6
TARGET_CASTER = 1
PERMANENT = 0xFFFFFFFF          # Duration.dbc stores -1 as an unsigned 32-bit

AURAS = {
    13: "MOD_DAMAGE_DONE", 15: "DAMAGE_SHIELD", 22: "MOD_RESISTANCE", 29: "MOD_STAT",
    42: "PROC_TRIGGER_SPELL", 85: "MOD_POWER_REGEN", 99: "MOD_ATTACK_POWER",
    161: "MOD_HEALTH_REGEN_IN_COMBAT", 189: "MOD_RATING",
}


def string_at(strings: bytes, offset: int) -> str:
    if not offset:
        return ""
    end = strings.find(b"\0", offset)
    return strings[offset:end].decode("utf-8", "replace")


ROW = re.compile(r"^\((\d+), '([^']*)', '([^']*)', (\d+), (\d+), (\d+),")


def words(sql: Path) -> list[tuple[int, str, list[int]]]:
    """(id, name, spell ids) for every row the generator wrote.

    A regex, not a comma split: the pattern column is itself comma separated
    ("row:col:entry,row:col:entry").
    """
    out = []
    for line in sql.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line.strip())
        if not match:
            continue
        out.append((int(match.group(1)), match.group(2),
                    [int(match.group(i)) for i in (4, 5, 6) if int(match.group(i))]))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--spell-dbc", required=True, type=Path, help="the client's Spell.dbc")
    parser.add_argument("--duration-dbc", required=True, type=Path, help="the client's SpellDuration.dbc")
    parser.add_argument("--sql", type=Path, default=REPO / "sql" / "custom" / "db_world" / "runewords_20_words.sql",
                        help="the generated word table (default: sql/custom/db_world/runewords_20_words.sql)")
    args = parser.parse_args()

    spell_records, spell_strings = dbc_mpq.read_dbc(args.spell_dbc, SPELL_FIELDS)
    duration_records, _ = dbc_mpq.read_dbc(args.duration_dbc, DURATION_FIELDS)
    durations = {record[0]: record[1] for record in duration_records}
    spells = {record[ID]: record for record in spell_records}

    errors, used = [], {}
    for word_id, name, ids in words(args.sql):
        parts = []
        for spell_id in ids:
            used[spell_id] = used.get(spell_id, 0) + 1
            record = spells.get(spell_id)
            if record is None:
                errors.append(f"#{word_id} {name}: spell {spell_id} is not in {args.spell_dbc.name}")
                continue
            spell_name = string_at(spell_strings, record[NAME])
            problems = []
            if record[EFFECT] != APPLY_AURA:
                problems.append(f"effect {record[EFFECT]} is not APPLY_AURA")
            if durations.get(record[DURATION]) != PERMANENT:
                problems.append(f"duration {durations.get(record[DURATION])} is not permanent")
            if record[TARGET_A] != TARGET_CASTER:
                problems.append(f"target A {record[TARGET_A]} is not the caster")
            if record[DIE_SIDES] not in (0, 1):
                problems.append(f"die sides {record[DIE_SIDES]}")
            if record[REAL_PPL] != 0.0:
                problems.append(f"real points per level {record[REAL_PPL]}")
            aura = AURAS.get(record[AURA], f"aura {record[AURA]}")
            amount = record[BASEPOINTS] + 1 if record[AURA] != 42 else 0
            if problems:
                errors.append(f"#{word_id} {name}: {spell_id} {spell_name!r}: " + "; ".join(problems))
            else:
                parts.append(f"{spell_id} {spell_name!r} [{aura} {amount}, misc {record[MISC]}]")
        print(f"#{word_id:<2} {name:<18} " + " | ".join(parts))

    print(f"\n{len(used)} distinct spells across {len(words(args.sql))} words "
          f"({sum(1 for n in used.values() if n > 1)} shared by more than one word)")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(f"{len(errors)} problem(s)", file=sys.stderr)
        return 1
    print("every borrowed spell is a permanent, deterministic, caster-targeted aura")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
