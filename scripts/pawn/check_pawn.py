#!/usr/bin/env python3
"""Offline checks for modules/mod-pawn.

This host has no Boost headers, so no AzerothCore translation unit can be compiled here; the
compile gate is the arm64 workflow, which runs this script first. What can be checked offline is
the agreement between the files, which is where a module of this shape actually breaks:

  * every Pawn.* key the C++ reads is defined, uncommented, in conf/mod-pawn.conf.dist;
  * the installer's PAWN_CONF_DEFAULTS and the shipped conf agree, both ways: the repair path
    restores exactly what the conf ships, and no key can be added to one without the other;
  * every column the roster code writes exists in the table it creates;
  * the loader symbol is the one the build derives from the directory name;
  * the picker is the creature hook it is meant to be, and nothing still refers to the obelisk
    gameobject it replaced;
  * pins.toml, the workflow and the installer all mention the module.

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
    for path in (CONF, PINS, WORKFLOW, INSTALLER, SRC / "PawnInnkeeper.cpp"):
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
        match = re.fullmatch(r"Pawn\.(?:Blacklist|Variant)\.(\d+)\.(\d+)", key)
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
                 f"{sum(1 for k in keys if k.startswith('Pawn.Blacklist.'))} blacklist blocks, "
                 f"{sum(1 for k in keys if k.startswith('Pawn.Variant.'))} variant blocks")
    return keys


def check_installer_defaults(keys: dict[str, str]) -> None:
    installer = read(INSTALLER)
    if not installer:
        return

    block = re.search(r"^PAWN_CONF_DEFAULTS=\((.*?)^\)", installer, re.S | re.M)
    if not block:
        fail("installer: no PAWN_CONF_DEFAULTS block")
        return

    defaults: dict[str, str] = {}
    for match in re.finditer(r'"([^"=]+)=([^"]*)"', block.group(1)):
        defaults[match.group(1)] = match.group(2)

    for key, value in sorted(defaults.items()):
        if key not in keys:
            fail(f"installer: PAWN_CONF_DEFAULTS has {key}, which the conf does not define")
        elif keys[key] != value:
            fail(f"installer: {key}={value} but the conf ships {keys[key]}; a repair would "
                 f"rewrite a value the conf does not carry")

    # The other direction: every non-blacklist key is one the repair path can restore.
    for key in sorted(keys):
        if key.startswith(("Pawn.Blacklist.", "Pawn.Variant.")):
            continue
        if key not in defaults:
            fail(f"installer: {key} is in the conf but not in PAWN_CONF_DEFAULTS; a repair "
                 f"would leave a bad value in place")

    notes.append(f"installer: {len(defaults)} conf defaults agree with the shipped conf")


def check_picker(keys: dict[str, str]) -> None:
    picker = read(SRC / "PawnInnkeeper.cpp")
    if not picker:
        return

    # The picker is one line on every innkeeper, not a gameobject: a CreatureScript hook runs for
    # every creature and the flag check decides which ones show it. A GameObjectScript here would
    # need a template row and a spawn, and the module ships no SQL at all.
    if 'CreatureScript("npc_pawn_innkeeper")' not in picker:
        fail("picker: PawnInnkeeper.cpp does not register a CreatureScript")
    if "UNIT_NPC_FLAG_INNKEEPER" not in picker:
        fail("picker: the hello hook does not gate on UNIT_NPC_FLAG_INNKEEPER")
    if "GameObjectScript" in picker:
        fail("picker: still a GameObjectScript")

    # Action codes below GOSSIP_OPTION_MAX (21) would swallow the innkeeper's own bind option,
    # because a database gossip row's action is its OptionType.
    open_action = re.search(r"PAWN_ACTION_OPEN\s*=\s*(\d+)", picker)
    if not open_action:
        fail("picker: no PAWN_ACTION_OPEN")
    elif int(open_action.group(1)) <= 21:
        fail(f"picker: PAWN_ACTION_OPEN is {open_action.group(1)}, inside the range the core "
             f"uses for its own gossip options")

    # The picker draws from the module's own pool query. Falling back to
    # sRandomPlayerbotMgr.addclassCache would silently show nothing, because that cache is only
    # filled when Playerbots.AddClassCommand is on and the installer turns it off.
    mgr = read(SRC / "PawnMgr.cpp")
    if "addclassCache" in mgr:
        fail("pool: PawnMgr.cpp reads addclassCache, which is empty with AddClassCommand = 0")
    for needle in ("playerbots_account_type", "RefreshPool", "DrawOffers"):
        if needle not in mgr:
            fail(f"pool: PawnMgr.cpp has no {needle}")

    for key in ("Pawn.FeaturedMin", "Pawn.FeaturedMax",
                "Pawn.PoolRefreshMinMinutes", "Pawn.PoolRefreshMaxMinutes"):
        if key not in keys:
            fail(f"pool: {key} is missing from the conf")

    # The obelisk is gone; a leftover key or script would look like it still selects something.
    for path in (CONF, INSTALLER, *sorted(SRC.glob("*.cpp"))):
        if "ObeliskEntry" in path.read_text(encoding="utf-8") and path != INSTALLER:
            fail(f"{path.relative_to(ROOT)}: still refers to Pawn.ObeliskEntry")
    if "Pawn\\.ObeliskEntry" not in read(INSTALLER):
        fail("installer: does not drop a stale Pawn.ObeliskEntry from an existing conf")

    notes.append("picker: creature hook on innkeepers, actions clear of the core's option range")


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
    for needle in ("modules/mod-pawn", "mod-pawn.conf.dist", "Pawn.FeaturedMin"):
        if needle not in workflow:
            fail(f"workflow: {needle} is not mentioned; the build would not ship the module")
    if "modules/mod-pawn/data/sql" in workflow:
        fail("workflow: still lists modules/mod-pawn/data/sql, which no longer exists")

    installer = read(INSTALLER)
    for needle in ("mod-pawn", "PAWN_CONF_DEFAULTS", "pawn_value_ok"):
        if needle not in installer:
            fail(f"installer: {needle} is not mentioned; an install would not configure the module")
    if "modules/mod-pawn/data/sql" in installer:
        fail("installer: still lists modules/mod-pawn/data/sql, which no longer exists")

    notes.append("wiring: pins, workflow and installer mention the module")


def main() -> int:
    check_files()
    keys = check_config_keys()
    check_installer_defaults(keys)
    check_picker(keys)
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
