#!/usr/bin/env python3
"""Check whether custom ``spell_dbc`` rows are client-visible or intentionally server-only.

A custom spell the server can cast still needs a record in the *client's*
``Spell.dbc`` for the client to resolve anything about it: a name, an icon, a
visual, a duration.  The server DBC is loaded from the same file the installer
copied out of the client, so the usual failure is a row that exists in the
database only - the server casts it, the client draws nothing, and the realm log
fills with "Spell 90001 does not exist".

This reads the ``INSERT INTO spell_dbc`` statements the repository ships (the
custom spells in ``sql/custom`` and in the modules), then compares the ids and
the fields that decide visibility against a real 234-field ``Spell.dbc``:

  * present in the client DBC  -> the client can resolve the row;
  * absent, no visual or icon  -> a plain server-only aura, which is fine;
  * absent, but ``SpellVisualID_1`` or ``SpellIconID`` is nonzero -> a warning:
    the client cannot resolve that visual/icon without a client DBC row.  With
    ``--strict`` (or ``--require-visible`` for that id) it is an error.

``--allow-server-only`` names the ids that are *meant* to be absent, and reports
an id that is present anyway as a stale declaration rather than a problem.
``--require-visible`` names the ids the client must be able to see and fails
when they are missing.  ``--client-data`` additionally names the patch archive
the client actually reads ``DBFilesClient\\Spell.dbc`` from and whether that
archive carries the custom ids, which catches a patch that landed under a later
one.

Without ``--spell-dbc`` the tool skips and exits 0: CI never needs Blizzard data.

Usage::

    python3 scripts/dbc/check_spell_dbc.py \\
        --sql sql/custom/db_world/wotlkskate_10_fall_protection.sql

    python3 scripts/dbc/check_spell_dbc.py \\
        --sql sql/custom/db_world/wotlkskate_10_fall_protection.sql \\
        --sql modules/mod-creature-affixes/data/sql/world/base/creature_affixes_50_custom_effects.sql \\
        --spell-dbc /opt/acore/data/dbc/Spell.dbc \\
        --allow-server-only 100200-100203,100205,100207 \\
        --require-visible 90001,90002 \\
        --client-data "/sdcard/WOTLK 3.3.5a/Data"

Exit codes: 0 clean or skipped, 1 a contradiction under the selected mode,
2 a usage problem or an unreadable input file.
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
ID, NAME = 0, 136
SPELL_FIELDS = 234

# The SQL columns this checker reads, lowercased.  Everything else in a row is
# dropped: only these decide whether the client can resolve the custom spell.
FIELDS = (
    "id", "name_lang_enus", "spellvisualid_1", "spelliconid",
    "attributes", "equippeditemclass", "durationindex",
)

RANGE = re.compile(r"^(\d+)(?:\s*-\s*(\d+))?$")


# ---------------------------------------------------------------------------
# A very small SQL reader: only INSERT INTO spell_dbc (...) VALUES (...)
# ---------------------------------------------------------------------------

def tokenize(text: str) -> list[tuple[str, str]]:
    """Split SQL into (kind, value) tokens, keeping quoted strings intact."""
    tokens: list[tuple[str, str]] = []
    i, end = 0, len(text)
    while i < end:
        char = text[i]
        if char.isspace():
            i += 1
            continue
        if text.startswith("--", i):
            newline = text.find("\n", i)
            i = end if newline < 0 else newline + 1
            continue
        if text.startswith("/*", i):
            close = text.find("*/", i + 2)
            i = end if close < 0 else close + 2
            continue
        if char == "`":
            close = text.find("`", i + 1)
            if close < 0:
                raise ValueError("unterminated backtick identifier")
            tokens.append(("word", text[i + 1:close]))
            i = close + 1
            continue
        if char in "'\"":
            quote, j, chars = char, i + 1, []
            while j < end:
                if text[j] == "\\" and j + 1 < end:
                    chars.append(text[j + 1])
                    j += 2
                    continue
                if text[j] == quote:
                    if j + 1 < end and text[j + 1] == quote:   # SQL doubled quote
                        chars.append(quote)
                        j += 2
                        continue
                    break
                chars.append(text[j])
                j += 1
            if j >= end:
                raise ValueError("unterminated string")
            tokens.append(("string", "".join(chars)))
            i = j + 1
            continue
        if char.isdigit() or (char in "+-" and i + 1 < end and
                              (text[i + 1].isdigit() or text[i + 1] == ".")):
            j = i + 1
            while j < end and (text[j].isdigit() or text[j] in ".eE" or
                               (text[j] in "+-" and text[j - 1] in "eE")):
                j += 1
            tokens.append(("number", text[i:j]))
            i = j
            continue
        if char.isalnum() or char in "_$":
            j = i + 1
            while j < end and (text[j].isalnum() or text[j] in "_$"):
                j += 1
            tokens.append(("word", text[i:j]))
            i = j
            continue
        tokens.append(("punct", char))
        i += 1
    return tokens


def read_name(tokens: list[tuple[str, str]], i: int) -> tuple[str, int]:
    if i >= len(tokens) or tokens[i][0] not in ("word", "string"):
        raise ValueError("expected a name")
    return tokens[i][1], i + 1


def read_table(tokens: list[tuple[str, str]], i: int) -> tuple[str, int]:
    """A table name, with a ``db.`` qualifier dropped."""
    name, i = read_name(tokens, i)
    while i < len(tokens) and tokens[i] == ("punct", "."):
        name, i = read_name(tokens, i + 1)
    return name, i


def parse_columns(tokens: list[tuple[str, str]], i: int) -> tuple[list[str], int]:
    if tokens[i] != ("punct", "("):
        raise ValueError("expected a column list")
    i += 1
    columns: list[str] = []
    while True:
        name, i = read_name(tokens, i)
        columns.append(name.strip().lower())
        if i < len(tokens) and tokens[i] == ("punct", ","):
            i += 1
            continue
        if i < len(tokens) and tokens[i] == ("punct", ")"):
            return columns, i + 1
        raise ValueError("malformed column list")


def parse_value(tokens: list[tuple[str, str]], i: int) -> tuple[object, int]:
    kind, value = tokens[i]
    if kind == "string":
        return value, i + 1
    if kind == "number":
        return (float(value) if any(c in value for c in ".eE") else int(value)), i + 1
    if kind == "word":
        upper = value.upper()
        if upper == "NULL":
            return None, i + 1
        if upper == "TRUE":
            return 1, i + 1
        if upper == "FALSE":
            return 0, i + 1
        return value, i + 1
    if tokens[i] == ("punct", "("):          # a nested expression; keep it opaque
        depth, j = 1, i + 1
        while j < len(tokens) and depth:
            if tokens[j] == ("punct", "("):
                depth += 1
            elif tokens[j] == ("punct", ")"):
                depth -= 1
            j += 1
        return None, j
    raise ValueError(f"unexpected token {value!r} in VALUES")


def parse_values(tokens: list[tuple[str, str]], i: int) -> tuple[list[object], int]:
    if tokens[i] != ("punct", "("):
        raise ValueError("expected a VALUES row")
    i += 1
    values: list[object] = []
    while True:
        value, i = parse_value(tokens, i)
        values.append(value)
        if i < len(tokens) and tokens[i] == ("punct", ","):
            i += 1
            continue
        if i < len(tokens) and tokens[i] == ("punct", ")"):
            return values, i + 1
        raise ValueError("malformed VALUES row")


def parse_statements(text: str) -> list[dict[str, object]]:
    """Every row of every ``INSERT INTO spell_dbc`` statement in ``text``."""
    tokens = tokenize(text)
    rows: list[dict[str, object]] = []
    i = 0
    while i < len(tokens):
        kind, value = tokens[i]
        if kind != "word" or value.upper() != "INSERT":
            i += 1
            continue
        j = i + 1
        while j < len(tokens) and tokens[j][0] == "word" and tokens[j][1].upper() in (
                "IGNORE", "LOW_PRIORITY", "DELAYED", "HIGH_PRIORITY"):
            j += 1
        if not (j < len(tokens) and tokens[j][0] == "word" and tokens[j][1].upper() == "INTO"):
            i += 1
            continue
        table, j = read_table(tokens, j + 1)
        if table.lower() != "spell_dbc":
            i = j
            continue
        if not (j < len(tokens) and tokens[j] == ("punct", "(")):
            raise ValueError("INSERT INTO spell_dbc without a column list is not supported")
        columns, j = parse_columns(tokens, j)
        if not (j < len(tokens) and tokens[j][0] == "word" and tokens[j][1].upper() == "VALUES"):
            raise ValueError("INSERT INTO spell_dbc without VALUES is not supported")
        j += 1
        while j < len(tokens) and tokens[j] == ("punct", "("):
            values, j = parse_values(tokens, j)
            if len(values) != len(columns):
                raise ValueError(
                    f"a spell_dbc row has {len(values)} values for {len(columns)} columns")
            rows.append(dict(zip(columns, values)))
            if j < len(tokens) and tokens[j] == ("punct", ","):
                j += 1
                continue
            break
        i = j
    return rows


def load_rows(paths: list[Path]) -> dict[int, dict[str, object]]:
    """Merge the custom rows by id, in file order, the way ON DUPLICATE would."""
    merged: dict[int, dict[str, object]] = {}
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValueError(f"{path}: {error.strerror or error}") from None
        try:
            rows = parse_statements(text)
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from None
        for row in rows:
            raw = row.get("id")
            if raw is None:
                raise ValueError(f"{path}: an INSERT INTO spell_dbc row has no ID")
            try:
                spell_id = int(raw)          # type: ignore[arg-type]
            except (TypeError, ValueError):
                raise ValueError(f"{path}: spell row ID {raw!r} is not an integer") from None
            merged.setdefault(spell_id, {}).update(
                {key: row[key] for key in FIELDS if key in row})
    return merged


# ---------------------------------------------------------------------------
# Field helpers
# ---------------------------------------------------------------------------

def as_int(row: dict[str, object], key: str, default: int = 0) -> int:
    value = row.get(key)
    if value is None or isinstance(value, str):
        return default
    return int(value)


def as_text(row: dict[str, object], key: str) -> str:
    value = row.get(key)
    return value if isinstance(value, str) else ""


def parse_id_set(text: str) -> set[int]:
    """A comma-separated list of ids and ``a-b`` ranges."""
    out: set[int] = set()
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        match = RANGE.match(part)
        if not match:
            raise argparse.ArgumentTypeError(f"{part!r} is not an id or an id range")
        low = int(match.group(1))
        high = int(match.group(2)) if match.group(2) else low
        if high < low:
            raise argparse.ArgumentTypeError(f"{part!r} counts backwards")
        out.update(range(low, high + 1))
    return out


# ---------------------------------------------------------------------------
# The client's archive
# ---------------------------------------------------------------------------

MEMBER = "DBFilesClient\\Spell.dbc"


def inspect_client_data(data_dir: Path, custom_ids: set[int]) -> tuple[str, set[int]] | None:
    """The patch archive that wins ``Spell.dbc`` and the custom ids it carries.

    Returns None when no patch archive carries the file (the client reads the
    stock archive, which ``--spell-dbc`` should already describe) or when the
    winning archive cannot be read with this reader.
    """
    for _, path in dbc_mpq.client_archives(data_dir):
        try:
            data = dbc_mpq.read_mpq_path(path, MEMBER)
        except SystemExit as error:
            message = str(error)
            if "is not in the archive" in message:
                continue
            print(f"  {path.name}: cannot inspect {MEMBER}: {message}")
            continue
        print(f"  {path.name}: carries {MEMBER}")
        try:
            records, _ = dbc_mpq.parse_dbc(data, f"{path.name}:{MEMBER}", SPELL_FIELDS)
        except SystemExit as error:
            print(f"  {path.name}: cannot parse {MEMBER}: {error}")
            continue
        present = {record[ID] for record in records}
        found = present & custom_ids
        print(f"  winning archive {path.name} carries {len(found)} of "
              f"{len(custom_ids)} custom id(s)")
        return path.name, found

    print(f"  no inspected patch archive carries {MEMBER}; `--spell-dbc` is the comparison")
    return None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--sql", type=Path, action="append", required=True, metavar="PATH",
        help="an SQL file with INSERT INTO spell_dbc rows; repeat for several files",
    )
    parser.add_argument(
        "--spell-dbc", type=Path, default=None, metavar="PATH",
        help="the client's 234-field Spell.dbc (default: none, which skips and exits 0)",
    )
    parser.add_argument(
        "--client-data", type=Path, default=None, metavar="PATH",
        help="the client's Data directory, to name the patch archive that wins Spell.dbc",
    )
    parser.add_argument(
        "--allow-server-only", type=parse_id_set, default=set(), metavar="IDS",
        help="ids meant to be absent from the client DBC (default: none); "
             "comma-separated ids and a-b ranges",
    )
    parser.add_argument(
        "--require-visible", type=parse_id_set, default=set(), metavar="IDS",
        help="ids the client must be able to see (default: none); fail when absent",
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="treat a server-only spell with a visual or icon as an error "
             "(default: warning)",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    if args.spell_dbc is None:
        print("skip: --spell-dbc was not given, so there is no client Spell.dbc to compare "
              "against; custom spell_dbc rows were not checked")
        return 0

    overlap = args.allow_server_only & args.require_visible
    if overlap:
        print(f"error: {', '.join(str(i) for i in sorted(overlap))} listed as both "
              f"--allow-server-only and --require-visible", file=sys.stderr)
        return 2

    try:
        rows = load_rows(args.sql)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    try:
        records, strings = dbc_mpq.read_dbc(args.spell_dbc, SPELL_FIELDS)
    except (OSError, SystemExit) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    spells = {record[ID]: record for record in records}

    custom_ids = set(rows) | args.require_visible
    client_result = None
    if args.client_data is not None:
        if not args.client_data.is_dir():
            print(f"error: {args.client_data} is not a directory; point --client-data "
                  f"at the client's Data folder", file=sys.stderr)
            return 2
        print(f"client data: {args.client_data}")
        client_result = inspect_client_data(args.client_data, custom_ids)
        print()

    warnings: list[str] = []
    errors: list[str] = []

    print(f"custom spell_dbc rows: {len(rows)} id(s) from {len(args.sql)} SQL file(s)")
    if not rows:
        print("  no INSERT INTO spell_dbc rows were found")
    print(f"Spell.dbc: {args.spell_dbc} ({len(records)} records)")

    table = []
    for spell_id in sorted(custom_ids):
        row = rows.get(spell_id, {})
        record = spells.get(spell_id)
        present = record is not None
        sql_name = as_text(row, "name_lang_enus")
        if present:
            name = dbc_mpq.string_at(strings, record[NAME]) or sql_name
        else:
            name = sql_name or "(not in SQL)"
        visual = as_int(row, "spellvisualid_1")
        icon = as_int(row, "spelliconid")
        attrs = as_int(row, "attributes")
        attrs_text = f"0x{attrs:X}" if "attributes" in row else "-"
        duration = str(as_int(row, "durationindex")) if "durationindex" in row else "-"
        item_class = (str(as_int(row, "equippeditemclass"))
                      if "equippeditemclass" in row else "-")
        required = spell_id in args.require_visible
        allowed = spell_id in args.allow_server_only

        if present:
            verdict = "visible"
        elif not row:
            verdict = "absent (required, not in SQL)" if required else "absent (not in SQL)"
        elif allowed and not required:
            verdict = "server-only (allowed)"
        elif visual or icon:
            verdict = "server-only, cannot resolve visual/icon"
        else:
            verdict = "server-only"

        if not present:
            if visual or icon:
                message = (f"{spell_id}: absent from {args.spell_dbc.name} but has "
                           f"SpellVisualID_1={visual} and SpellIconID={icon}; the client "
                           f"cannot resolve a visual/icon without a client Spell.dbc row")
                if args.strict or required:
                    errors.append(message)
                else:
                    warnings.append(message)
            if required:
                errors.append(f"{spell_id}: listed in --require-visible but absent from "
                              f"{args.spell_dbc.name}")
        if present and allowed:
            warnings.append(f"{spell_id}: listed in --allow-server-only but present in "
                            f"{args.spell_dbc.name}; the server-only intent is stale")
        if required and client_result is not None and spell_id not in client_result[1]:
            errors.append(f"{spell_id}: listed in --require-visible but the winning archive "
                          f"{client_result[0]} does not carry it")

        table.append((str(spell_id), "present" if present else "absent",
                      str(visual), str(icon), duration, item_class,
                      attrs_text, name, verdict))

    header = ("ID", "client", "visual", "icon", "dur", "item", "attrs", "name", "verdict")
    widths = [max(len(row[i]) for row in table + [header]) for i in range(8)]
    print(f"{header[0]:<{widths[0]}}  {header[1]:<{widths[1]}}  {header[2]:>6}  "
          f"{header[3]:>5}  {header[4]:>5}  {header[5]:>6}  {header[6]:<{widths[6]}}  "
          f"{header[7]:<{widths[7]}}  {header[8]}")
    for row in table:
        print(f"{row[0]:<{widths[0]}}  {row[1]:<{widths[1]}}  {row[2]:>6}  "
              f"{row[3]:>5}  {row[4]:>5}  {row[5]:>6}  {row[6]:<{widths[6]}}  "
              f"{row[7]:<{widths[7]}}  {row[8]}")

    if warnings:
        print()
        for message in warnings:
            print(f"WARN  {message}")
    if errors:
        print()
        for message in errors:
            print(f"ERROR {message}")

    print()
    if errors:
        print(f"VERDICT: {len(errors)} error(s), {len(warnings)} warning(s) - "
              f"the custom spell rows contradict the selected mode")
        return 1
    if warnings:
        print(f"VERDICT: ok with {len(warnings)} warning(s)")
    else:
        print("VERDICT: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
