#!/usr/bin/env python3
"""Check AzerothCore module SQL for the invariants this repository relies on.

Only ``modules/<name>/data/sql`` trees are scanned; ``sql/custom`` and the core
tree are ignored.  Violations are printed as ``file:line: message`` and make
the checker exit 1.  Usage problems exit 2.

Checks:
  1. SQL basenames are unique across the scanned module trees.
  2. CREATE TABLE statements use IF NOT EXISTS.
  3. spell_dbc INSERTs list columns explicitly, have an ON DUPLICATE KEY UPDATE
     clause, include EquippedItemClass, and set every EquippedItemClass value
     to -1.
  4. spell_script_names INSERTs do not use INSERT IGNORE, update ScriptName on
     duplicate, and name a script registered with RegisterSpellScript(...) in
     the same module's src/*.cpp.
  5. UPDATEs of custom spell_dbc rows (ID >= 100000) target IDs created by an
     earlier-sorting INSERT INTO spell_dbc (a DELETE does not create a row).
  6. EffectTriggerSpell_1/2/3 values >= 100000 resolve to a custom spell
     inserted by the same module.

Warnings mark SQL the checker cannot verify.  They are printed but only make
the exit code non-zero with --strict.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CUSTOM_SPELL_MIN = 100000

QUOTES = "'\"`"

INSERT_RE = re.compile(r"\s*INSERT\s+(IGNORE\s+)?INTO\s+`?([A-Za-z0-9_]+)`?", re.I)
VALUES_RE = re.compile(r"\s*VALUES\b", re.I)
UPDATE_RE = re.compile(r"\s*UPDATE\s+`?([A-Za-z0-9_]+)`?", re.I)
DELETE_RE = re.compile(r"\s*DELETE\s+FROM\s+`?([A-Za-z0-9_]+)`?", re.I)
CREATE_TABLE_RE = re.compile(r"\s*CREATE\s+(?:TEMPORARY\s+)?TABLE\b", re.I)
IF_NOT_EXISTS_RE = re.compile(r"\bIF\s+NOT\s+EXISTS\b", re.I)
ON_DUP_RE = re.compile(r"\bON\s+DUPLICATE\s+KEY\s+UPDATE\b", re.I)
WHERE_RE = re.compile(r"\bWHERE\b", re.I)
ID_COLUMN = r"(?<![A-Za-z0-9_])(?:`ID`|ID)(?![A-Za-z0-9_])"
ID_EQUALS_RE = re.compile(ID_COLUMN + r"\s*=\s*(\d+)", re.I)
ID_BETWEEN_RE = re.compile(ID_COLUMN + r"\s+BETWEEN\s+(\d+)\s+AND\s+(\d+)", re.I)
INTEGER_RE = re.compile(r"^[+-]?\d+$")
REGISTER_RE = re.compile(
    r"RegisterSpellScript\s*(?:<[^<>]*>)?\s*\(\s*&?\s*([A-Za-z_][A-Za-z0-9_:]*)"
)


def strip_comments(text):
    """Blank out SQL comments while preserving offsets and newlines."""
    out = list(text)
    n = len(text)
    i = 0
    quote = None
    while i < n:
        c = text[i]
        if quote is not None:
            if quote != "`" and c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in QUOTES:
            quote = c
            i += 1
            continue
        if c == "-" and i + 1 < n and text[i + 1] == "-":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "#":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = " "
            out[i + 1] = " "
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    out[i] = " "
                    out[i + 1] = " "
                    i += 2
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def matching_paren(text, start):
    depth = 0
    quote = None
    i = start
    while i < len(text):
        c = text[i]
        if quote is not None:
            if quote != "`" and c == "\\" and i + 1 < len(text):
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in QUOTES:
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top_level(text, delimiter=","):
    parts = []
    start = 0
    depth = 0
    quote = None
    i = 0
    while i < len(text):
        c = text[i]
        if quote is not None:
            if quote != "`" and c == "\\" and i + 1 < len(text):
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in QUOTES:
            quote = c
        elif c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == delimiter and depth == 0:
            parts.append(text[start:i])
            start = i + 1
        i += 1
    parts.append(text[start:])
    return parts


def split_statements(text):
    """Return (statement, absolute offset) pairs, split on top-level ';'."""
    statements = []
    start = 0
    depth = 0
    quote = None
    i = 0
    while i < len(text):
        c = text[i]
        if quote is not None:
            if quote != "`" and c == "\\" and i + 1 < len(text):
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in QUOTES:
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth = max(0, depth - 1)
        elif c == ";" and depth == 0:
            statements.append((text[start:i], start))
            start = i + 1
        i += 1
    statements.append((text[start:], start))
    return [(stmt, offset) for stmt, offset in statements if stmt.strip()]


def find_top_level_keyword(text, pattern, start=0, end=None):
    if end is None:
        end = len(text)
    depth = 0
    quote = None
    i = start
    while i < end:
        c = text[i]
        if quote is not None:
            if quote != "`" and c == "\\" and i + 1 < end:
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in QUOTES:
            quote = c
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth = max(0, depth - 1)
        elif depth == 0:
            match = pattern.match(text, i, end)
            if match:
                return i
        i += 1
    return None


def unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in QUOTES:
        body = value[1:-1]
        if value[0] != "`":
            body = body.replace(value[0] * 2, value[0])
        return body
    return value


def value_int(value):
    value = value.strip()
    return int(value) if INTEGER_RE.match(value) else None


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def statement_line(text, offset, statement):
    lead = len(statement) - len(statement.lstrip())
    return line_of(text, offset + lead)


def column_index(columns, name):
    target = name.lower()
    for index, column in enumerate(columns):
        if unquote(column).lower() == target:
            return index
    return None


def parse_value_rows(text, start, end):
    rows = []
    i = start
    while i < end:
        while i < end and text[i] in " \t\r\n,":
            i += 1
        if i >= end or text[i] != "(":
            break
        close = matching_paren(text, i)
        if close < 0 or close > end:
            break
        values = [value.strip() for value in split_top_level(text[i + 1 : close])]
        rows.append((values, i))
        i = close + 1
    return rows


def parse_insert(statement, base_offset=0):
    match = INSERT_RE.match(statement)
    if not match:
        return None
    pos = match.end()
    while pos < len(statement) and statement[pos].isspace():
        pos += 1
    columns = None
    if pos < len(statement) and statement[pos] == "(":
        close = matching_paren(statement, pos)
        if close < 0:
            return {
                "table": match.group(2),
                "ignore": bool(match.group(1)),
                "columns": None,
                "rows": [],
                "on_dup": None,
                "parse_error": True,
            }
        columns = [unquote(part) for part in split_top_level(statement[pos + 1 : close])]
        pos = close + 1
    values_match = VALUES_RE.match(statement, pos)
    if not values_match:
        return {
            "table": match.group(2),
            "ignore": bool(match.group(1)),
            "columns": columns,
            "rows": [],
            "on_dup": None,
            "parse_error": True,
        }
    values_start = values_match.end()
    on_dup_at = find_top_level_keyword(statement, ON_DUP_RE, values_start)
    values_end = on_dup_at if on_dup_at is not None else len(statement)
    rows = parse_value_rows(statement, values_start, values_end)
    return {
        "table": match.group(2),
        "ignore": bool(match.group(1)),
        "columns": columns,
        "rows": [(values, base_offset + row_offset) for values, row_offset in rows],
        "on_dup": statement[on_dup_at:] if on_dup_at is not None else None,
        "parse_error": False,
    }


def assigned_columns(on_dup):
    body = ON_DUP_RE.sub("", on_dup, count=1)
    columns = []
    for part in split_top_level(body):
        if "=" in part:
            columns.append(unquote(part.split("=", 1)[0]))
    return [column for column in columns if column]


def registered_scripts(module_dir, cache):
    key = str(module_dir)
    if key in cache:
        return cache[key]
    names = set()
    src = module_dir / "src"
    if src.is_dir():
        for path in sorted(src.rglob("*.cpp")):
            names.update(REGISTER_RE.findall(path.read_text(encoding="utf-8", errors="replace")))
    cache[key] = names
    return names


def check_spell_dbc_insert(info, text, rel, line, created, errors, warnings):
    rows = []
    columns = info["columns"]
    if columns is None:
        errors.append((rel, line, "spell_dbc INSERT must list its columns explicitly"))
        if info["parse_error"]:
            warnings.append((rel, line, "spell_dbc INSERT could not be parsed"))
        return rows
    if info["on_dup"] is None:
        errors.append((rel, line, "spell_dbc INSERT must use ON DUPLICATE KEY UPDATE"))
    if info["parse_error"]:
        warnings.append((rel, line, "spell_dbc INSERT could not be parsed"))
        return rows

    equipped_index = column_index(columns, "EquippedItemClass")
    if equipped_index is None:
        errors.append((rel, line, "spell_dbc INSERT must include EquippedItemClass"))
    id_index = column_index(columns, "ID")
    if id_index is None:
        warnings.append((rel, line, "spell_dbc INSERT has no ID column; its rows are unchecked"))
    trigger_columns = []
    for name in ("EffectTriggerSpell_1", "EffectTriggerSpell_2", "EffectTriggerSpell_3"):
        index = column_index(columns, name)
        if index is not None:
            trigger_columns.append((name, index))

    for values, row_offset in info["rows"]:
        row_line = line_of(text, row_offset)
        if len(values) != len(columns):
            warnings.append(
                (rel, row_line, f"spell_dbc row has {len(values)} values for {len(columns)} columns")
            )
        if equipped_index is not None:
            if equipped_index >= len(values):
                warnings.append((rel, row_line, "spell_dbc row has no EquippedItemClass value"))
            else:
                value = values[equipped_index].strip()
                if value != "-1":
                    errors.append(
                        (rel, row_line, f"spell_dbc EquippedItemClass must be -1, got {value}")
                    )
        if id_index is None or id_index >= len(values):
            continue
        spell_id = value_int(values[id_index])
        if spell_id is None:
            warnings.append(
                (rel, row_line, f"spell_dbc ID is not an integer: {values[id_index].strip()}")
            )
            continue
        created.add(spell_id)
        triggers = []
        for name, index in trigger_columns:
            if index >= len(values):
                continue
            trigger = value_int(values[index])
            if trigger is not None and trigger >= CUSTOM_SPELL_MIN:
                triggers.append((name, trigger))
        rows.append({"id": spell_id, "line": row_line, "triggers": triggers, "path": rel})
    return rows


def check_script_names_insert(info, text, rel, line, module_dir, errors, warnings, cache):
    if info["ignore"]:
        errors.append((rel, line, "spell_script_names INSERT must not use INSERT IGNORE"))
    if info["on_dup"] is None:
        errors.append((rel, line, "spell_script_names INSERT must use ON DUPLICATE KEY UPDATE"))
    elif not any(column.lower() == "scriptname" for column in assigned_columns(info["on_dup"])):
        errors.append(
            (rel, line, "spell_script_names ON DUPLICATE KEY UPDATE must assign ScriptName")
        )
    columns = info["columns"]
    if columns is None:
        warnings.append((rel, line, "spell_script_names INSERT has no column list"))
        return
    if info["parse_error"]:
        warnings.append((rel, line, "spell_script_names INSERT could not be parsed"))
        return
    index = column_index(columns, "ScriptName")
    if index is None:
        warnings.append((rel, line, "spell_script_names INSERT does not list ScriptName"))
        return
    names = registered_scripts(module_dir, cache)
    for values, row_offset in info["rows"]:
        row_line = line_of(text, row_offset)
        if index >= len(values):
            warnings.append((rel, row_line, "spell_script_names row has no ScriptName value"))
            continue
        script = unquote(values[index])
        if script not in names:
            errors.append(
                (
                    rel,
                    row_line,
                    f"ScriptName '{script}' is not registered with RegisterSpellScript "
                    f"in {module_dir.name}/src",
                )
            )


def check_spell_dbc_update(statement, rel, line, module_name, created, errors, warnings):
    update = UPDATE_RE.match(statement)
    if not update or update.group(1).lower() != "spell_dbc":
        return
    where_at = find_top_level_keyword(statement, WHERE_RE, update.end())
    if where_at is None:
        return
    where = statement[where_at:]
    between = ID_BETWEEN_RE.search(where)
    equal = ID_EQUALS_RE.search(where)
    if between:
        low, high = int(between.group(1)), int(between.group(2))
        if high < CUSTOM_SPELL_MIN:
            return
        low = max(low, CUSTOM_SPELL_MIN)
        if not any(low <= spell_id <= high for spell_id in created):
            errors.append(
                (
                    rel,
                    line,
                    f"UPDATE spell_dbc targets ID BETWEEN {between.group(1)} AND "
                    f"{between.group(2)}, but no earlier INSERT INTO spell_dbc in "
                    f"{module_name} creates a custom spell in that range",
                )
            )
    elif equal:
        spell_id = int(equal.group(1))
        if spell_id >= CUSTOM_SPELL_MIN and spell_id not in created:
            errors.append(
                (
                    rel,
                    line,
                    f"UPDATE spell_dbc targets custom ID {spell_id}, but no earlier "
                    f"INSERT INTO spell_dbc in {module_name} creates it",
                )
            )
    elif re.search(r"(?<![0-9])1[0-9]{5}(?![0-9])", where):
        warnings.append(
            (rel, line, "cannot verify spell_dbc UPDATE; no ID = N or ID BETWEEN N AND M")
        )


def check_spell_dbc_delete(statement, created):
    delete = DELETE_RE.match(statement)
    if not delete or delete.group(1).lower() != "spell_dbc":
        return
    where_at = find_top_level_keyword(statement, WHERE_RE, delete.end())
    if where_at is None:
        return
    where = statement[where_at:]
    between = ID_BETWEEN_RE.search(where)
    equal = ID_EQUALS_RE.search(where)
    if between:
        low, high = int(between.group(1)), int(between.group(2))
        created.difference_update(range(low, high + 1))
    elif equal:
        created.discard(int(equal.group(1)))


def check_module(module_name, module_dir, files, repo, errors, warnings, cache):
    created = set()
    inserted = []
    for path in files:
        rel = display_path(path, repo)
        try:
            raw = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            warnings.append((rel, 1, f"cannot read SQL: {exc}"))
            continue
        text = strip_comments(raw)
        for statement, offset in split_statements(text):
            line = statement_line(text, offset, statement)
            if CREATE_TABLE_RE.match(statement):
                if not IF_NOT_EXISTS_RE.search(statement):
                    errors.append((rel, line, "CREATE TABLE must use IF NOT EXISTS"))
            elif INSERT_RE.match(statement):
                info = parse_insert(statement, offset)
                if info is None:
                    continue
                table = info["table"].lower()
                if table == "spell_dbc":
                    inserted.extend(
                        check_spell_dbc_insert(
                            info, text, rel, line, created, errors, warnings
                        )
                    )
                elif table == "spell_script_names":
                    check_script_names_insert(
                        info, text, rel, line, module_dir, errors, warnings, cache
                    )
            elif UPDATE_RE.match(statement):
                check_spell_dbc_update(
                    statement, rel, line, module_name, created, errors, warnings
                )
            elif DELETE_RE.match(statement):
                check_spell_dbc_delete(statement, created)

    inserted_ids = {
        row["id"] for row in inserted if row["id"] is not None and row["id"] >= CUSTOM_SPELL_MIN
    }
    for row in inserted:
        for column, trigger in row["triggers"]:
            if trigger not in inserted_ids:
                errors.append(
                    (
                        row["path"],
                        row["line"],
                        f"{column} = {trigger} does not resolve to a spell_dbc row "
                        f"inserted in {module_name}",
                    )
                )


def display_path(path, repo):
    try:
        return path.relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix()


def discover_modules(repo, module_name):
    modules_root = repo / "modules"
    if module_name:
        module_dir = modules_root / module_name
        sql_root = module_dir / "data" / "sql"
        if not sql_root.is_dir():
            return None
        files = sorted(
            sql_root.rglob("*.sql"),
            key=lambda path: path.relative_to(sql_root).as_posix(),
        )
        return [(module_name, module_dir, files)]
    if not modules_root.is_dir():
        return None
    found = []
    for module_dir in sorted(path for path in modules_root.iterdir() if path.is_dir()):
        sql_root = module_dir / "data" / "sql"
        if not sql_root.is_dir():
            continue
        files = sorted(
            sql_root.rglob("*.sql"),
            key=lambda path: path.relative_to(sql_root).as_posix(),
        )
        found.append((module_dir.name, module_dir, files))
    return found


def check_duplicate_basenames(module_files, repo, errors):
    seen = {}
    for _module_name, _module_dir, files in module_files:
        for path in files:
            previous = seen.get(path.name)
            if previous is not None and previous != path:
                errors.append(
                    (
                        display_path(path, repo),
                        1,
                        f"duplicate SQL basename '{path.name}' "
                        f"(also {display_path(previous, repo)})",
                    )
                )
            else:
                seen[path.name] = path


def default_repo():
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "modules").is_dir():
            return parent
    return here.parents[2]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check module SQL invariants (no database access).",
        epilog=(
            "Warnings identify SQL this checker cannot verify. They are always "
            "printed but only affect the exit code with --strict."
        ),
    )
    parser.add_argument(
        "--repo",
        metavar="PATH",
        help="repository root (default: the repository containing this script)",
    )
    parser.add_argument(
        "--module",
        metavar="NAME",
        help="only check modules/NAME/data/sql (default: every module with SQL)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 for unverifiable SQL warnings as well as violations",
    )
    args = parser.parse_args(argv)

    repo = Path(args.repo).expanduser().resolve() if args.repo else default_repo()
    if not (repo / "modules").is_dir():
        print(f"error: {repo} has no modules/ directory", file=sys.stderr)
        return 2
    module_files = discover_modules(repo, args.module)
    if module_files is None:
        print(f"error: no module SQL found under {repo / 'modules'}", file=sys.stderr)
        return 2

    errors = []
    warnings = []
    cache = {}
    check_duplicate_basenames(module_files, repo, errors)
    for module_name, module_dir, files in module_files:
        check_module(module_name, module_dir, files, repo, errors, warnings, cache)

    for rel, line, message in sorted(set(errors)):
        print(f"{rel}:{line}: {message}")
    for rel, line, message in sorted(set(warnings)):
        print(f"{rel}:{line}: warning: {message}")

    if errors or (args.strict and warnings):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
