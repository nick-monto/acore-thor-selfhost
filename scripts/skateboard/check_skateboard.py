#!/usr/bin/env python3
"""Offline checks for modules/mod-skateboard.

This script reads files, it does not compile them: `scripts/syntax-check.sh` is the compile gate,
and .github/workflows/module-checks.yml runs both. What can be checked offline here is the
agreement between the files, which is where a module of this shape actually breaks:

  * the four files the module needs are there, and the loader symbol is the one the build derives
    from the directory name - `Addmod_skateboardScripts()` - calling the one function the module
    defines, `AddSkateboardScripts()`;
  * every `Skateboard.*` key conf/mod-skateboard.conf.dist ships is read from a string literal in
    the module's sources, and the module reads no key the conf lacks, so a typo cannot look like a
    working setting;
  * the conf's defaults, the module's own `GetOption<T>("Key", default)` fallbacks, the
    `$SkateboardConfDefaults` copy in scripts/windows-install.ps1 and the `SKATEBOARD_CONF_DEFAULTS`
    list in scripts/write-confs.sh agree key for key and value for value - a key the module cannot
    use is silently replaced by its fallback, so the four must not drift;
  * the `.board` subcommands in the module's ChatCommandTable are the ones docs/SKATEBOARD-FIRST-PASS.md
    §2.8 documents, and every verb the add-on sends through the AC command channel and its bar is
    one of them. The add-on lives in the sibling launcher checkout; when that checkout is absent the
    comparison is printed as a `skip` line rather than failing, because CI does not clone it. The
    reverse direction is deliberately not asserted: `.board speed` is a player command the add-on
    has no use for;
  * the boarding cast's push agrees: the module sends `AzerothCore\tc` with `begin=`, `done` and
    `cancel=`, and the add-on handles exactly those three payloads under that opcode - the wire
    format is the part of this module that broke once already;
  * pins.toml carries `[module.skateboard]` and all three server workflows copy the module into the
    core, so a release is not built without it;
  * the deck aliases resolve to the display ids docs §3 allocates: `log` to the stock 26609,
    `engravedwall` to 90001 and `coffinlid` to 90002;
  * nothing calls `CharacterDatabase.DirectCommitTransaction`, which CI forbids.

Exit code 0 means every check passed. Run it from anywhere:

    python3 scripts/skateboard/check_skateboard.py
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "mod-skateboard"
SRC = MODULE / "src"
CONF = MODULE / "conf" / "mod-skateboard.conf.dist"
PINS = ROOT / "pins.toml"
DOC = ROOT / "docs" / "SKATEBOARD-FIRST-PASS.md"
WRITE_CONFS = ROOT / "scripts" / "write-confs.sh"
DECK_TOOL = ROOT / "scripts" / "skateboard" / "patch_deck_dbc.py"
# The Windows installer carries its own copy of the conf half - a different shell, so the same
# library cannot be sourced - which is why its array is compared to the .dist here.
WINDOWS_INSTALLER = ROOT / "scripts" / "windows-install.ps1"
WORKFLOWS = (
    ".github/workflows/server-aarch64.yml",
    ".github/workflows/server-win64.yml",
    ".github/workflows/server-x86_64.yml",
)
# The add-on is a launcher asset, in the sibling checkout on a development host and absent in CI,
# where module-checks.yml does not clone it. AC_LAUNCHER points it elsewhere, like AC_CORE does for
# the other checkers.
LAUNCHER = Path(os.environ.get("AC_LAUNCHER") or ROOT.parent / "acore-thor-launcher")
ADDON = LAUNCHER / "app" / "src" / "main" / "assets" / "skateboard" / "Skateboard.lua"

# The three sources the build expects, and the conf, by the names the contract fixes.
REQUIRED_SOURCES = ("skateboard_common.h", "Skateboard.cpp", "mod_skateboard_loader.cpp")
LOADER_SYMBOL = "Add" + MODULE.name.replace("-", "_") + "Scripts"   # Addmod_skateboardScripts
REGISTER_SYMBOL = "AddSkateboardScripts"

# The deck aliases and the display id each resolves to: 26609 is the stock object (no client patch),
# the other two are the rows docs/SKATEBOARD-FIRST-PASS.md §3 appends above the current maximum.
DECK_IDS = {"log": 26609, "engravedwall": 90001, "coffinlid": 90002}
DBC_DECKS = ("engravedwall", "coffinlid")

CONF_LINE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_.]*)\s*=\s*(.*?)\s*$")
GET_OPTION = re.compile(
    r'GetOption\s*<\s*([^<>\n]+?)\s*>\s*\(\s*"(Skateboard\.[A-Za-z0-9_.]+)"\s*,\s*([^;()\n]*?)\s*\)'
)
KEY_LITERAL = re.compile(r'"(Skateboard\.[A-Za-z0-9_.]+)"')
NUMBER = re.compile(r"^[-+]?(?:\d+(?:\.\d*)?|\.\d+)[fFuUlL]*$")
# A conf ships a switch as 0/1; the module reads it as GetOption<bool>, so its fallback is written
# `false`/`true`. Those are the same default and must compare equal.
BOOLEANS = {"true": 1.0, "false": 0.0}
# An array literal in either installer dialect: `$Name = @(` ... `)` or `NAME=(` ... `)`, at the
# start of a line so a `Keys = @(...)` inside a PowerShell hashtable is not mistaken for one. The
# body is taken by matching parentheses, so a list laid out on one line is read as well as a column.
ARRAY_START = re.compile(r"^[ \t]*(?:\$)?([A-Za-z_]\w*)\s*=\s*@?\(", re.M)
ARRAY_ENTRY = re.compile(r'"([^"\n]+)"|\'([^\'\n]+)\'')
# A ChatCommandTable initializer and the two things an entry can be: a literal and a handler, or a
# literal and a nested table's name.
TABLE = re.compile(r"ChatCommandTable\s+(\w+)\s*=\s*\{")
TABLE_ENTRY = re.compile(r'^\{\s*"([^"]+)"\s*,\s*([^,}]+)')
# The `.board <verb>` mentions in a document, and the ones inside a Lua string literal.
BOARD_MENTION = re.compile(r"\.board\s+([a-z][a-z0-9_-]*)")
LUA_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'')
BOARD_COMMAND = re.compile(r"(?<![A-Za-z])board\s+([a-z][a-z0-9_-]*)")

failures: list[str] = []
notes: list[str] = []
skips: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def read(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def normalise(value: str) -> tuple[str, object]:
    """A conf value, a C++ literal or a shell literal reduced to something comparable.

    A number compares as a number, so `21.0`, `21.0f` and `21` agree; anything else compares
    as its collapsed, lower-cased text with the quotes stripped, so `log` and `"log"` agree.
    """
    text = value.strip().strip('"').strip("'").strip()
    if text.lower() in BOOLEANS:
        return ("number", BOOLEANS[text.lower()])
    if NUMBER.match(text):
        try:
            return ("number", float(text.rstrip("fFuUlL")))
        except ValueError:
            pass
    return ("text", " ".join(text.split()).lower())


def source_files() -> list[Path]:
    """Every C++ source in the module, so a second file cannot hide a config read."""
    if not SRC.is_dir():
        return []
    found = sorted(path for path in SRC.glob("*.cpp")) + sorted(path for path in SRC.glob("*.h"))
    return found


def sources_text() -> str:
    return "\n".join(read(path) for path in source_files())


def conf_defaults() -> dict[str, str]:
    """The uncommented Skateboard.* keys of the shipped .dist, in file order."""
    defaults: dict[str, str] = {}
    for number, line in enumerate(read(CONF).splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", "[", ";")):
            continue
        match = CONF_LINE.match(line)
        if not match:
            fail(f"{rel(CONF)}:{number}: cannot parse '{stripped}'")
            continue
        key = match.group(1)
        value = re.sub(r"\s+#.*$", "", match.group(2)).strip()
        if not key.startswith("Skateboard."):
            fail(f"{rel(CONF)}:{number}: {key} is outside the Skateboard. prefix")
            continue
        if key in defaults:
            fail(f"{rel(CONF)}:{number}: {key} is defined twice")
            continue
        defaults[key] = value
    return defaults


def module_fallbacks() -> dict[str, tuple[str, str]]:
    """Key -> (the default as written, the file that reads it)."""
    fallbacks: dict[str, tuple[str, str]] = {}
    for path in source_files():
        for match in GET_OPTION.finditer(read(path)):
            key, default = match.group(2), match.group(3).strip()
            if key in fallbacks and normalise(fallbacks[key][0]) != normalise(default):
                fail(f"{rel(path)}: {key} is read twice with different fallbacks, "
                     f"{fallbacks[key][0]!r} and {default!r}")
                continue
            fallbacks.setdefault(key, (default, path.name))
    return fallbacks


def array_bodies(text: str) -> list[tuple[str, str]]:
    """(name, body) for every `Name=(` / `$Name = @(` in `text`, body paren-matched."""
    bodies: list[tuple[str, str]] = []
    for match in ARRAY_START.finditer(text):
        start = match.end() - 1
        depth = 0
        for index in range(start, len(text)):
            char = text[index]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    bodies.append((match.group(1), text[start + 1:index]))
                    break
    return bodies


def installer_defaults(path: Path) -> dict[str, str] | None:
    """KEY=VALUE pairs from the arrays in `path` that list Skateboard. keys.

    The array's name is not assumed: both installers have grown one per module and the name is
    comment-only convention, so the keys are what identifies the list.
    """
    if not path.is_file():
        fail(f"{rel(path)} is missing")
        return None
    named: dict[str, str] = {}
    lists = 0
    for name, body in array_bodies(read(path)):
        pairs: dict[str, str] = {}
        for entry in ARRAY_ENTRY.finditer(body):
            value = entry.group(1) if entry.group(1) is not None else entry.group(2)
            if "=" not in value:
                continue
            key, default = value.split("=", 1)
            pairs[key.strip()] = default.strip()
        skateboard = {key: value for key, value in pairs.items() if key.startswith("Skateboard.")}
        if not skateboard:
            if name.lower().startswith("skateboard"):
                fail(f"{rel(path)}: ${name} lists no Skateboard.* key")
            continue
        if len(skateboard) != len(pairs):
            stray = sorted(set(pairs) - set(skateboard))
            fail(f"{rel(path)}: ${name} mixes Skateboard.* keys with {', '.join(stray)}")
        for key, value in skateboard.items():
            if key in named:
                fail(f"{rel(path)}: {key} is listed twice")
                continue
            named[key] = value
        lists += 1
    if not lists:
        fail(f"{rel(path)} has no array listing the Skateboard.* defaults")
        return None
    return named


def compare_defaults(source: str, given: dict[str, str], defaults: dict[str, str]) -> None:
    for key, value in given.items():
        if key not in defaults:
            fail(f"{source} lists {key}, which {rel(CONF)} does not ship")
        elif normalise(value) != normalise(defaults[key]):
            fail(f"{source} lists {key}={value}, but {rel(CONF)} ships {defaults[key]}")
    for key, value in defaults.items():
        if key not in given:
            fail(f"{source} does not list {key} (the .dist ships {value})")


# --------------------------------------------------------------------------- checks


def check_files() -> tuple[str, None]:
    if not MODULE.is_dir():
        fail(f"{rel(MODULE)} is missing")
        return "", None
    present = []
    for name in REQUIRED_SOURCES:
        path = SRC / name
        if not path.is_file():
            fail(f"{rel(path)} is missing")
        elif not read(path).strip():
            fail(f"{rel(path)} is empty")
        else:
            present.append(f"src/{name}")
    if not CONF.is_file():
        fail(f"{rel(CONF)} is missing")
    elif not read(CONF).strip():
        fail(f"{rel(CONF)} is empty")
    else:
        present.append("conf/mod-skateboard.conf.dist")
    if len(present) != len(REQUIRED_SOURCES) + 1:
        return "", None
    return f"files: {', '.join(present)}", None


def check_config_keys() -> tuple[str, dict[str, str]]:
    defaults = conf_defaults()
    if not defaults:
        if CONF.is_file():
            fail(f"{rel(CONF)} defines no Skateboard.* key")
        return "", {}
    text = sources_text()
    if not text.strip():
        fail(f"{rel(SRC)} holds no readable source, so no config key can be matched")
        return "", defaults
    for key in defaults:
        if f'"{key}"' not in text:
            fail(f"{key} is in {rel(CONF)} but no module source reads it")
    for key in sorted(set(KEY_LITERAL.findall(text)) - set(defaults)):
        fail(f"a module source reads {key}, which {rel(CONF)} does not ship")
    return (f"conf: {len(defaults)} Skateboard.* keys, all read by the module and all defined "
            f"by the .dist"), defaults


def check_defaults(defaults: dict[str, str]) -> tuple[str, None]:
    if not defaults:
        return "", None
    fallbacks = module_fallbacks()
    for key, value in defaults.items():
        if key not in fallbacks:
            fail(f"{key}: {rel(CONF)} ships {value!r} but the module has no GetOption fallback for it")
            continue
        fallback, source = fallbacks[key]
        if normalise(value) != normalise(fallback):
            fail(f"{key}: {rel(CONF)} ships {value!r} but {source} falls back to {fallback!r}")
    for key in sorted(set(fallbacks) - set(defaults)):
        fail(f"{key}: {fallbacks[key][1]} falls back to {fallbacks[key][0]!r}, "
             f"which {rel(CONF)} does not ship")
    for path in (WRITE_CONFS, WINDOWS_INSTALLER):
        listed = installer_defaults(path)
        if listed is not None:
            compare_defaults(rel(path), listed, defaults)
    return (f"defaults: the .dist, the module's fallbacks, {rel(WRITE_CONFS)} and "
            f"{rel(WINDOWS_INSTALLER)} agree on {len(defaults)} keys"), None


def check_loader() -> tuple[str, None]:
    loader_path = SRC / "mod_skateboard_loader.cpp"
    module_path = SRC / "Skateboard.cpp"
    loader = read(loader_path)
    module = read(module_path)
    if not loader.strip():
        fail(f"{rel(loader_path)} is missing or empty, so the loader cannot be checked")
        return "", None
    if not re.search(rf"\b{LOADER_SYMBOL}\s*\(", loader):
        fail(f"{rel(loader_path)} does not define {LOADER_SYMBOL}(); the build derives that name "
             f"from the directory 'mod-skateboard'")
    calls = [
        line for line in loader.splitlines()
        if re.search(rf"\b{REGISTER_SYMBOL}\s*\(\s*\)\s*;", line)
        and not re.match(r"\s*(?:extern\s+)?void\b", line)
    ]
    if not calls:
        fail(f"{rel(loader_path)} does not call {REGISTER_SYMBOL}(); a script registered nowhere "
             f"never runs")
    if not module.strip():
        fail(f"{rel(module_path)} is missing or empty, so {REGISTER_SYMBOL}() cannot be checked")
    elif not re.search(rf"\b{REGISTER_SYMBOL}\s*\(\s*\)\s*\{{", module):
        fail(f"{rel(module_path)} does not define {REGISTER_SYMBOL}()")
    return (f"loader: {LOADER_SYMBOL}() calls {REGISTER_SYMBOL}(), defined in "
            f"src/Skateboard.cpp"), None


def brace_body(text: str, open_index: int) -> str:
    """The text between the brace at `open_index` and its matching close brace."""
    depth = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index]
    return text[open_index + 1:]


def top_level_entries(body: str) -> list[str]:
    """The `{ ... }` initializers directly inside a table body."""
    entries: list[str] = []
    depth = 0
    start = None
    for index, char in enumerate(body):
        if char == "{":
            if depth == 0:
                start = index
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0 and start is not None:
                entries.append(body[start:index + 1])
                start = None
    return entries


def command_tables(text: str) -> dict[str, list[tuple[str, str]]]:
    """Table name -> [(string literal, what follows it)] in source order."""
    tables: dict[str, list[tuple[str, str]]] = {}
    for match in TABLE.finditer(text):
        body = brace_body(text, match.end() - 1)
        entries: list[tuple[str, str]] = []
        for raw in top_level_entries(body):
            hit = TABLE_ENTRY.match(raw.strip())
            if hit:
                entries.append((hit.group(1), hit.group(2).strip()))
        tables[match.group(1)] = entries
    return tables


def board_subcommands(text: str) -> set[str] | None:
    """The `.board` verbs, read out of the table registered under the literal "board"."""
    tables = command_tables(text)
    for entries in tables.values():
        for literal, target in entries:
            if literal == "board" and target in tables:
                return {entry for entry, _ in tables[target]}
    # No `{ "board", ... }` line: fall back to the table that looks like the board's own, so the
    # failure names the subcommands rather than the shape of the registration.
    for name, entries in tables.items():
        literals = {entry for entry, _ in entries}
        if "board" in name.lower() and "status" in literals:
            return literals
    return None


def doc_section(pattern: str) -> str:
    match = re.search(pattern, read(DOC), re.S | re.M)
    return match.group(0) if match else ""


def check_commands() -> tuple[str, None]:
    text = sources_text()
    if not text.strip():
        fail(f"{rel(SRC)} holds no readable source, so the command surface cannot be checked")
        return "", None
    verbs = board_subcommands(text)
    if verbs is None:
        fail(f"no ChatCommandTable in {rel(SRC)} registers a 'board' command with subcommands")
        return "", None

    section = doc_section(r"^### 2\.8\b.*?(?=^#{2,3} )")
    if not section:
        fail(f"{rel(DOC)} has no §2.8, so the documented commands cannot be compared")
    else:
        documented = set(BOARD_MENTION.findall(section))
        for verb in sorted(documented - verbs):
            fail(f"{rel(DOC)} §2.8 documents '.board {verb}', which the module does not implement")
        for verb in sorted(verbs - documented):
            fail(f"the module implements '.board {verb}', which {rel(DOC)} §2.8 does not document")

    skipped = False
    if not ADDON.is_file():
        skips.append(f"commands: {rel(ADDON)} is not checked out, add-on verbs not compared")
        skipped = True
    else:
        literals = []
        for match in LUA_LITERAL.finditer(read(ADDON)):
            literal = match.group(1) if match.group(1) is not None else match.group(2)
            literals.append(literal)
        sent = set()
        for literal in literals:
            sent.update(BOARD_COMMAND.findall(literal))
        if not sent:
            # The add-on may build the payload as `"board " .. verb` and keep the verbs in a table
            # of their own. Two or more bare literals that name a module verb are that table.
            bare = {literal.strip() for literal in literals} & verbs
            if len(bare) >= 2:
                sent = bare
        if not sent:
            fail(f"{rel(ADDON)} carries no '.board <verb>' or 'board <verb>' string, so the "
                 f"add-on's commands cannot be checked")
        for verb in sorted(sent - verbs):
            fail(f"{rel(ADDON)} sends '.board {verb}', which the module does not implement")

    where = "the add-on (skipped)" if skipped else "the add-on"
    return f"commands: {len(verbs)} .board verbs agree between the module, docs §2.8 and {where}", None


def check_cast_push() -> tuple[str, None]:
    """The boarding cast's push: one opcode, three payloads, both sides."""
    module = sources_text()
    verbs = set(re.findall(r'PushCastState\(player, "([a-z]+)', module))
    expected = {"begin", "done", "cancel"}
    if verbs != expected:
        fail(f"the module pushes cast payloads {sorted(verbs)} instead of {sorted(expected)}")
    if '"AzerothCore\\tc"' not in module:
        fail('the module does not build the cast push on the "AzerothCore\\tc" prefix the add-on '
             'parses (opcode c)')

    where = "the add-on (skipped)"
    if ADDON.is_file():
        addon = read(ADDON)
        addon_verbs = set(re.findall(r'verb == "([a-z]+)"', addon))
        if addon_verbs != expected:
            fail(f"{rel(ADDON)} handles cast payloads {sorted(addon_verbs)} instead of {sorted(expected)}")
        if 'op == "c"' not in addon:
            fail(f'{rel(ADDON)} does not branch on the "c" opcode the module pushes')
        where = "the add-on"
    else:
        skips.append(f"cast push: {rel(ADDON)} is not checked out, payloads not compared")

    return f"cast: opcode c with {', '.join(sorted(verbs))} agree between the module and {where}", None


def check_wiring() -> tuple[str, None]:
    pins = read(PINS)
    if not pins:
        fail(f"{rel(PINS)} is missing")
    else:
        table = re.search(r"^\[module\.skateboard\][^\n]*$", pins, re.M)
        if not table:
            fail(f"{rel(PINS)} has no [module.skateboard] table")
        else:
            block = pins[table.end():].split("\n[", 1)[0]
            if "modules/mod-skateboard" not in block:
                fail(f"[module.skateboard] in {rel(PINS)} does not name modules/mod-skateboard")
    copy_keywords = ("cp -a", "cp -r", "cp ", "rsync", "Copy-Item", "robocopy")
    checked = []
    for name in WORKFLOWS:
        path = ROOT / name
        text = read(path)
        if not text:
            fail(f"{rel(path)} is missing")
            continue
        lines = [
            line for line in text.splitlines()
            if "mod-skateboard" in line and not line.lstrip().startswith("#")
        ]
        copies = [
            line for line in lines
            if re.search(r"modules[/\\]mod-skateboard", line)
            and any(keyword.lower() in line.lower() for keyword in copy_keywords)
        ]
        if not copies:
            fail(f"{rel(path)} does not copy modules/mod-skateboard into the core")
        else:
            checked.append(name.rsplit("/", 1)[-1])
    return (f"wiring: pins.toml and {', '.join(checked)} carry the module"), None


def check_decks() -> tuple[str, None]:
    text = sources_text()
    if not text.strip():
        fail(f"{rel(SRC)} holds no readable source, so the deck ids cannot be checked")
        return "", None
    # A named constant, `constexpr uint32 DeckEngravedWall = 90001;`, so an arm that assigns it by
    # name still counts as tying the alias to its id.
    constants = {
        hit.group(1): int(hit.group(2))
        for hit in re.finditer(r"\b([A-Za-z_]\w*)\s*=\s*(\d{4,6})\b", text)
    }
    for alias, display in DECK_IDS.items():
        if not re.search(rf'"{alias}"', text, re.I):
            fail(f"no module source names the deck alias {alias!r}")
        # The alias has to resolve to its id: a `{ "log", 26609 }` entry, an `if (deck == "log")
        # displayId = DeckLog;` arm, or anything else in which the id - or a constant holding it -
        # follows the alias before the next string literal starts another arm. The window also
        # stops at the statement boundary, so the header's own defaults - `DeckName = "log";`
        # followed by `DeckDisplayId = 26609;` - cannot satisfy the alias on their own.
        reached: set[int] = set()
        for occurrence in re.finditer(rf'"{alias}"', text, re.I):
            window = text[occurrence.end(): occurrence.end() + 240].split('"', 1)[0]
            window = window.split(';', 1)[0].split('}', 1)[0]
            reached.update(int(hit) for hit in re.findall(r"(?<!\d)(\d{4,6})(?!\d)", window))
            reached.update(
                constants[name] for name in re.findall(r"\b([A-Za-z_]\w*)\b", window)
                if name in constants
            )
        if display not in reached:
            fail(f"the module never resolves the deck alias {alias!r} to display id {display} "
                 f"(it resolves to {sorted(reached) or 'nothing'})")

    section = doc_section(r"^## 3\. The deck rows.*?(?=^#{2,3} )")
    if not section:
        fail(f"{rel(DOC)} has no §3, so the deck rows cannot be checked")
    else:
        # The table names the decks the way the tool will, "Engraved Wall" and "Iron Coffin Lid";
        # the conf names them `engravedwall` and `coffinlid`, so a row is matched by the alias it
        # contains.
        rows = []
        for line in section.splitlines():
            if not line.lstrip().startswith("|"):
                continue
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            if len(cells) < 2 or not any(".mdx" in cell.lower() for cell in cells[1:]):
                continue
            name = re.sub(r"[^a-z]", "", cells[0].lower())
            matches = [alias for alias in DECK_IDS if alias in name]
            rows.append((max(matches, key=len) if matches else name, cells))
        documented = {alias for alias, _ in rows}
        for alias in sorted(documented - set(DBC_DECKS)):
            fail(f"{rel(DOC)} §3 lists a deck '{alias}' the module does not patch")
        for alias in sorted(set(DBC_DECKS) - documented):
            fail(f"{rel(DOC)} §3 does not list a row for the '{alias}' deck")
        for alias, cells in rows:
            if alias not in DECK_IDS:
                continue
            ids = [
                int(cell.strip("`")) for cell in cells[1:]
                if re.fullmatch(r"\d{4,6}", cell.strip("`"))
            ]
            for display in ids:
                if display != DECK_IDS[alias]:
                    fail(f"{rel(DOC)} §3 gives the '{alias}' deck display id {display}, "
                         f"but the module uses {DECK_IDS[alias]}")
    # The DBC tool is the other half of the same contract: it is what puts the display ids in the
    # files the module and the client read, so its table of aliases, ids and model paths has to be
    # the one the module and §3 name.
    tool = read(DECK_TOOL)
    if not tool.strip():
        fail(f"{rel(DECK_TOOL)} is missing or empty, so the deck rows cannot be checked")
    else:
        ids_block = re.search(r"^DECK_IDS\s*=\s*\{(.*?)^\}", tool, re.S | re.M)
        models_block = re.search(r"^DECK_MODELS\s*=\s*\{(.*?)^\}", tool, re.S | re.M)
        if not ids_block or not models_block:
            fail(f"{rel(DECK_TOOL)} no longer holds top-level DECK_IDS and DECK_MODELS tables")
        else:
            tool_ids = {alias: int(number) for alias, number in
                        re.findall(r'"([a-z]+)"\s*:\s*(\d+)', ids_block.group(1))}
            tool_models = {alias: name for alias, name in
                           re.findall(r'"([a-z]+)"\s*:\s*"([^"]+\.mdx)"', models_block.group(1))}
            for alias, display in DECK_IDS.items():
                if alias not in DBC_DECKS:
                    continue                      # `log` is stock: the tool registers nothing for it
                if tool_ids.get(alias) != display:
                    fail(f"{rel(DECK_TOOL)} registers {alias!r} as {tool_ids.get(alias)}, "
                         f"but the module uses {display}")
            for alias in sorted(set(tool_ids) - set(DECK_IDS)):
                fail(f"{rel(DECK_TOOL)} registers a deck {alias!r} the module does not resolve")
            for alias, model in tool_models.items():
                if alias not in DECK_IDS:
                    fail(f"{rel(DECK_TOOL)} names a model for an unknown deck {alias!r}")
                    continue
                listed = [cells for name, cells in rows
                          if alias in re.sub(r"[^a-z]", "", name.lower())]
                if listed and not any(any(model.lower() in cell.lower() for cell in cells[1:])
                                      for cells in listed):
                    fail(f"{rel(DECK_TOOL)} registers {model} for {alias!r}, which "
                         f"{rel(DOC)} §3 does not list for that deck")
        for other in (WRITE_CONFS,):
            if "Skateboard.Deck=engravedwall" not in read(other):
                fail(f"{rel(other)} does not ship the engravedwall deck as its default")
        defaults = conf_defaults()
        if defaults.get("Skateboard.Deck") != "engravedwall":
            fail(f"{rel(CONF)} ships Skateboard.Deck={defaults.get('Skateboard.Deck')}, "
                 f"but the shield the tool registers is the shipped default")
    return (f"decks: {', '.join(f'{display} ({alias})' for alias, display in DECK_IDS.items())} "
            f"match {rel(DOC)} §3 and {rel(DECK_TOOL)}"), None


def check_no_direct_commit() -> tuple[str, None]:
    if not MODULE.is_dir():
        return "", None
    for path in sorted(MODULE.rglob("*")):
        if not path.is_file():
            continue
        if re.search(r"DirectCommitTransaction\s*\(", read(path)):
            fail(f"{rel(path)} calls DirectCommitTransaction; CI forbids it (its comment in "
                 f".github/workflows/module-checks.yml explains why)")
    return "safety: no DirectCommitTransaction in the module", None


# --------------------------------------------------------------------------- main


def run(check, *args):
    """Run one check; record its `ok` line only when it added no failure of its own."""
    before = len(failures)
    note, value = "", None
    try:
        note, value = check(*args)
    except Exception as exc:  # a check must never take the gate down with a traceback
        fail(f"{check.__name__}: the check itself crashed ({type(exc).__name__}: {exc})")
    if len(failures) == before and note:
        notes.append(note)
    return value


def main() -> int:
    run(check_files)
    defaults = run(check_config_keys)
    run(check_defaults, defaults)
    run(check_loader)
    run(check_commands)
    run(check_cast_push)
    run(check_wiring)
    run(check_decks)
    run(check_no_direct_commit)

    for note in notes:
        print(f"ok   {note}")
    for skip in skips:
        print(f"skip {skip}")
    for failure in failures:
        print(f"FAIL {failure}")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1

    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
