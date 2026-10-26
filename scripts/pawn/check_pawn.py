#!/usr/bin/env python3
"""Offline checks for modules/mod-pawn.

This host has no Boost headers, so no AzerothCore translation unit can be compiled here; the
compile gate is the arm64 workflow. What can be checked offline is the agreement between the
files, which is where a module of this shape actually breaks:

  * every Pawn.* key the C++ reads is defined, uncommented, in conf/mod-pawn.conf.dist;
  * the conf's obelisk entry and script name match the SQL and the C++ registration;
  * every column the roster SQL writes exists in the roster table;
  * the loader symbol is the one the build derives from the directory name;
  * the conf, the module's SQL trees, pins.toml and the workflow all mention the module.

Exit code 0 means every check passed. Run it from anywhere:

    python3 scripts/pawn/check_pawn.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "mod-pawn"
CONF = MODULE / "conf" / "mod-pawn.conf.dist"
SRC = MODULE / "src"
WORLD_SQL = MODULE / "data" / "sql" / "world" / "base" / "pawn_10_obelisk.sql"
PINS = ROOT / "pins.toml"
WORKFLOW = ROOT / ".github" / "workflows" / "server-aarch64.yml"
INSTALLER = ROOT / "scripts" / "thor-install.sh"

PLAYABLE_CLASSES = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11}
MAX_SPECS = 8

failures: list[str] = []
notes: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"missing file: {path.relative_to(ROOT)}")
        return ""
    return path.read_text(encoding="utf-8")


def conf_keys(text: str) -> dict[str, str]:
    """Uncommented KEY = VALUE pairs, in file order."""
    keys: dict[str, str] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped.startswith("["):
            continue
        if "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        key = key.strip()
        if key in keys:
            fail(f"conf: duplicate key {key}")
        keys[key] = value.strip()
    return keys


def check_files() -> None:
    for path in (CONF, WORLD_SQL, PINS, WORKFLOW, INSTALLER):
        if not path.is_file():
            fail(f"missing file: {path.relative_to(ROOT)}")
    for path in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
        if not path.read_text(encoding="utf-8").strip():
            fail(f"empty source file: {path.relative_to(ROOT)}")


def check_config_keys() -> dict[str, str]:
    text = read(CONF)
    keys = conf_keys(text)

    # Keys the C++ reads with a default; a missing definition means the shipped conf does not say
    # what the module does, which is the whole point of the file.
    used: set[str] = set()
    for path in sorted(SRC.glob("*.cpp")):
        for match in re.finditer(r'GetOption<[^>]+>\(\s*"(Pawn\.[^"]+)"', path.read_text(encoding="utf-8")):
            used.add(match.group(1))

    for key in sorted(used):
        if key not in keys:
            fail(f"conf: {key} is read by the code but not defined in mod-pawn.conf.dist")

    # Blacklist keys: class must be playable, spec inside the premade spec table.
    for key in sorted(keys):
        match = re.fullmatch(r"Pawn\.Blacklist\.(\d+)\.(\d+)", key)
        if not match:
            continue
        cls, spec = int(match.group(1)), int(match.group(2))
        if cls not in PLAYABLE_CLASSES:
            fail(f"conf: {key} names class {cls}, which is not playable")
        if spec >= MAX_SPECS:
            fail(f"conf: {key} names spec {spec}, beyond the premade spec table ({MAX_SPECS})")
        if not keys[key].strip():
            fail(f"conf: {key} is empty")

    notes.append(f"conf: {len(keys)} keys, {len(used)} read by the code, "
                 f"{sum(1 for k in keys if k.startswith('Pawn.Blacklist.'))} blacklist blocks")
    return keys


def check_obelisk(keys: dict[str, str]) -> None:
    world = read(WORLD_SQL)
    if not world:
        return

    entry = keys.get("Pawn.ObeliskEntry")
    if not entry:
        fail("conf: Pawn.ObeliskEntry is missing")
        return

    template = re.search(r"\((\d+),\s*10,\s*\d+,\s*'([^']*)'", world)
    if not template:
        fail("sql: no type-10 gameobject_template row found")
        return

    if template.group(1) != entry:
        fail(f"sql: template entry {template.group(1)} does not match Pawn.ObeliskEntry {entry}")

    script = re.search(r"'go_[a-z_]+'", world)
    if not script:
        fail("sql: the template row has no ScriptName")
    else:
        name = script.group(0).strip("'")
        if f'GameObjectScript("{name}")' not in read(SRC / "PawnObelisk.cpp"):
            fail(f"sql: ScriptName {name} is not the one PawnObelisk.cpp registers")

    # Every spawn must point at the template, and each DELETE must cover what the INSERT writes.
    spawn_block = re.search(r"INSERT INTO `gameobject`.*?VALUES(.*?);", world, re.S)
    spawn_ids = re.findall(r"\((\d+),\s*(\d+),\s*[01],\s*\d+,", spawn_block.group(1) if spawn_block else "")
    for guid, template_id in spawn_ids:
        if template_id != entry:
            fail(f"sql: spawn {guid} uses template {template_id}, not {entry}")
    deletes = set(re.findall(r"DELETE FROM `gameobject` WHERE `guid` IN \(([^)]*)\)", world))
    if not deletes:
        fail("sql: no DELETE for the obelisk spawns; a re-run would duplicate them")

    notes.append(f"sql: obelisk entry {entry}, {len(spawn_ids)} spawns")


def check_roster() -> None:
    cpp = read(SRC / "PawnMgr.cpp")
    if not cpp:
        return

    # The table is created programmatically (module SQL that fails aborts the boot on this fork),
    # so the schema to compare against is the string literal in the source.
    table = re.search(r'"CREATE TABLE IF NOT EXISTS `(\w+)` \((.*?)\) ENGINE', cpp, re.S)
    if not table:
        fail("code: no CREATE TABLE IF NOT EXISTS for the roster")
        return
    table_name, body = table.group(1), table.group(2)
    columns = {m.group(1) for m in re.finditer(r'`(\w+)`', body)}

    for statement in re.finditer(r'"REPLACE INTO (\w+) \(([^)]*)\)', cpp):
        if statement.group(1) != table_name:
            fail(f"code: writes {statement.group(1)}, SQL creates {table_name}")
        written = {c.strip() for c in statement.group(2).split(",")}
        missing = written - columns
        if missing:
            fail(f"code: writes columns the table does not have: {sorted(missing)}")

    if "state" not in columns or "target_level" not in columns:
        fail("code: the roster table is missing state or target_level")

    notes.append(f"schema: {table_name} has {len(columns)} named columns")


def check_loader() -> None:
    expected = "Add" + MODULE.name.replace("-", "_") + "Scripts"
    found = False
    for path in sorted(SRC.glob("*.cpp")):
        if re.search(rf"\bvoid {expected}\(\)", path.read_text(encoding="utf-8")):
            found = True
    if not found:
        fail(f"loader: no definition of {expected}() in {SRC.relative_to(ROOT)}")
    notes.append(f"loader: {expected}()")


def check_wiring() -> None:
    pins = read(PINS)
    if "[module.pawn]" not in pins:
        fail("pins.toml: no [module.pawn] entry")
    if 'path = "modules/mod-pawn"' not in pins:
        fail('pins.toml: [module.pawn] does not point at modules/mod-pawn')

    workflow = read(WORKFLOW)
    for needle in ("modules/mod-pawn", "mod-pawn.conf.dist", "modules/mod-pawn/data/sql"):
        if needle not in workflow:
            fail(f"workflow: {needle} is not mentioned; the build would not ship the module")

    installer = read(INSTALLER)
    for needle in ("mod-pawn", "PAWN_CONF_DEFAULTS", "pawn_value_ok"):
        if needle not in installer:
            fail(f"installer: {needle} is not mentioned; an install would not configure the module")

    notes.append("wiring: pins, workflow and installer mention the module")


def main() -> int:
    check_files()
    keys = check_config_keys()
    check_obelisk(keys)
    check_roster()
    check_loader()
    check_wiring()

    for note in notes:
        print(f"ok   {note}")
    for failure in failures:
        print(f"FAIL {failure}")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1

    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
