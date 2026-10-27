#!/usr/bin/env python3
"""Offline checks for modules/mod-pawn.

This host has no Boost headers, so no AzerothCore translation unit can be compiled here; the
compile gate is the arm64 workflow, which runs this script first. What can be checked offline is
the agreement between the files, which is where a module of this shape actually breaks:

  * every Pawn.* key the C++ reads is defined, uncommented, in conf/mod-pawn.conf.dist;
  * the installer's PAWN_CONF_DEFAULTS and the shipped conf agree, both ways: the repair path
    restores exactly what the conf ships, and no key can be added to one without the other;
  * every column the roster code writes exists in the table it creates, the vanity columns
    survive a save, and the one text column is quoted by the statement and escaped by the
    caller, because Execute's variadic overload formats a std::string raw;
  * every personality pool parses to at least one profile and every dial names a strategy
    playerbots registers *as a strategy* - a trigger, an action or a value of the same name is
    not one, and the engine drops it - never drags in `chat` for speech, and puts a dial only
    where it does something: `save mana` on a healer,
    `close`/`ranged` off the classes whose strategy already inherits it, and a pool on every
    PvE premade spec;
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
# Sibling checkout, present on a developer host but not in CI's arm64 job, which builds only this
# repository. The vocabulary check degrades to a note when it is absent rather than failing.
PLAYERBOTS = ROOT.parent / "mod-playerbots"

PLAYABLE_CLASSES = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11}
MAX_SPECS = 8

# One strategy dial in a personality pool: "+aggressive", "-close", "save mana".
PROFILE_TOKEN = re.compile(r"^[+-][a-z][a-z ]*$")

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
        match = re.fullmatch(r"Pawn\.(?:Blacklist|Variant|Temperament)\.(\d+)\.(\d+)", key)
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
                 f"{sum(1 for k in keys if k.startswith('Pawn.Variant.'))} variant blocks, "
                 f"{sum(1 for k in keys if k.startswith('Pawn.Temperament.'))} temperament blocks")
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

    # The other direction: every key the repair path can restore is in the defaults. The
    # per-class pools are deliberately absent: they are long, they change with balance, and the
    # player is meant to edit them, so a repair leaves them alone.
    for key in sorted(keys):
        if key.startswith(("Pawn.Blacklist.", "Pawn.Variant.", "Pawn.Temperament.")):
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
    for needle in ("playerbots_account_type", "RefreshPool", "DrawOffers",
                   "sPawnPersonality.Temperament", "sPawnPersonality.Apply"):
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


# Every kind of playerbots AI object registers the same way, `creators["name"] = &Class::factory;`,
# and a class file declares its strategy, trigger, action and value contexts side by side. The
# name is a strategy only when the class around it derives from NamedObjectContext<Strategy>, so
# the scan tracks the enclosing class instead of taking every creator in the file. All three
# shapes here - base, class and the raid/dungeon strategy contexts - use that one form.
CONTEXT_CLASS = re.compile(r"^class\s+\w+[^\n{;]*\bNamedObjectContext<(\w+)>")
CREATOR = re.compile(r'creators\["([^"]+)"\]')


def strategy_vocabulary() -> set[str] | None:
    """Every strategy name playerbots registers, or None when the sibling tree is absent.

    Engine::addStrategy resolves the name through GetStrategy, which returns null for a trigger,
    an action or a value; the dial is then dropped without a word. That is why the vocabulary is
    the Strategy contexts only.
    """
    if not PLAYERBOTS.is_dir():
        return None

    sources = [PLAYERBOTS / "src" / "Ai" / "Base" / "StrategyContext.h"]
    sources += sorted(PLAYERBOTS.glob("src/Ai/Class/*/*AiObjectContext.cpp"))
    sources += [
        PLAYERBOTS / "src" / "Ai" / "Raid" / "RaidStrategyContext.h",
        PLAYERBOTS / "src" / "Ai" / "Dungeon" / "DungeonStrategyContext.h",
    ]
    names: set[str] = set()
    for path in sources:
        if not path.is_file():
            # A fork that renames or moves a context would otherwise shrink the vocabulary to
            # whatever is left, which is a weaker check that still passes.
            fail(f"vocabulary: {path.relative_to(PLAYERBOTS.parent)} is missing from the "
                 f"mod-playerbots checkout")
            continue
        kind: str | None = None
        for line in path.read_text(encoding="utf-8").splitlines():
            declared = CONTEXT_CLASS.match(line)
            if declared:
                kind = declared.group(1)
            elif line.startswith("};"):
                kind = None  # end of the class body; its context no longer applies
            elif kind == "Strategy":
                names.update(CREATOR.findall(line))
    return names or None


PLAYERBOTS_CONF = PLAYERBOTS / "conf" / "playerbots.conf.dist"
TEMPERAMENT_KEY = re.compile(r"^Pawn\.Temperament\.(\d+)\.(\d+)$")
# Playerbots names its premade specs by talent, and the healer ones are the only specs whose
# healing actions the mana-conservation strategy ever sees. These are whole words, as in the
# module: "unholy pve" contains "holy" as a substring, and a death knight is no healer.
HEALER_SPEC_WORDS = ("holy", "resto", "disc")

# These class strategies already inherit the base the dial would add, so the trigger is already
# there and the dial changes nothing: the token is dead on that class whatever the spec.
CLASS_BASE_DIALS = {
    (6, "close"): "GenericDKStrategy inherits MeleeCombatStrategy",
    (8, "ranged"): "GenericMageStrategy inherits RangedCombatStrategy",
    (5, "ranged"): "GenericPriestStrategy inherits RangedCombatStrategy",
}


def premade_specs() -> dict[tuple[int, int], str] | None:
    """Playerbots' premade spec names by (class, spec), or None when the sibling conf is absent."""
    if not PLAYERBOTS_CONF.is_file():
        return None
    text = PLAYERBOTS_CONF.read_text(encoding="utf-8")
    specs: dict[tuple[int, int], str] = {}
    for match in re.finditer(r"^Playerbots\.PremadeSpecName\.(\d+)\.(\d+)\s*=\s*(.+)$",
                             text, re.M):
        specs[(int(match.group(1)), int(match.group(2)))] = match.group(3).strip()
    return specs or None


def parse_pool(value: str, key: str) -> list[tuple[str, list[str]]]:
    """Split one pool value into (label, change tokens), reporting anything malformed."""
    profiles: list[tuple[str, list[str]]] = []
    for chunk in value.split("|"):
        profile = chunk.strip()
        if not profile:
            continue  # a trailing or doubled '|' is a formatting slip, as in the module
        if ":" not in profile:
            fail(f"conf: {key}: profile {profile!r} has no label")
            continue
        label, changes = profile.split(":", 1)
        label, changes = label.strip(), changes.strip()
        if not label:
            fail(f"conf: {key}: a profile has an empty label")
            continue
        if not changes:
            fail(f"conf: {key}: profile {label} has an empty change list")
            continue
        tokens: list[str] = []
        for token in changes.split(","):
            token = token.strip()
            if not token:
                fail(f"conf: {key}: profile {label} has an empty change token")
            elif not PROFILE_TOKEN.fullmatch(token):
                fail(f"conf: {key}: profile {label} has the malformed token {token!r}; each "
                     f"token is +name or -name, lower case, spaces allowed inside")
            else:
                tokens.append(token)
        profiles.append((label, tokens))
    return profiles


def has_word(name: str, word: str) -> bool:
    """True when `word` is a whole word of `name`, the rule PawnArchetype::RoleFor uses."""
    return re.search(rf"(?<![A-Za-z]){re.escape(word)}(?![A-Za-z])", name.lower()) is not None


def check_personality(keys: dict[str, str]) -> None:
    vocabulary = strategy_vocabulary()
    specs = premade_specs()

    pools: list[tuple[str, str]] = []
    if "Pawn.Voice" not in keys:
        fail("conf: no Pawn.Voice; every pawn would roll the same non-combat behaviour")
    else:
        pools.append(("Pawn.Voice", keys["Pawn.Voice"]))
    temperament_keys = sorted(k for k in keys if k.startswith("Pawn.Temperament."))
    if not temperament_keys:
        fail("conf: no Pawn.Temperament.* blocks; every pawn would roll an empty temperament")
    pools.extend((key, keys[key]) for key in temperament_keys)

    voice_profiles = 0
    dials: set[str] = set()
    dead: set[tuple[str, str]] = set()
    covered: set[tuple[int, int]] = set()
    for key, value in pools:
        profiles = parse_pool(value, key)
        if not profiles:
            # PawnPersonality::Load keeps a block only `if (!pool.empty())`, so an empty one does
            # not survive as an empty temperament: it is dropped and the spec falls back to
            # whatever the class default is, silently.
            fail(f"conf: {key} defines no profiles; the loader drops an empty pool, so this "
                 f"block adds nothing")
        if key == "Pawn.Voice":
            voice_profiles = len(profiles)
        spec_key = TEMPERAMENT_KEY.fullmatch(key)
        cls = int(spec_key.group(1)) if spec_key else None
        spec = int(spec_key.group(2)) if spec_key else None
        if profiles and spec_key:
            covered.add((cls, spec))
        spec_name = specs.get((cls, spec)) if specs is not None and cls is not None else None
        for label, tokens in profiles:
            for token in tokens:
                name = token[1:]
                dials.add(name)
                # The engine ignores an unknown name without a word, so the only place a typo
                # can be caught is here.
                if name == "chat":
                    fail(f"conf: {key}: profile {label} names {token!r}; chat is chat-command "
                         f"handling, not speech - use emote for a voice")
                elif vocabulary is not None and name not in vocabulary:
                    fail(f"conf: {key}: profile {label} names {token!r}, which playerbots does "
                         f"not register as a strategy; addStrategy looks the name up with "
                         f"GetStrategy and silently ignores a trigger, an action or a value")
                if (key, name) in dead:
                    continue  # one report per block and dial; the second profile adds nothing
                # HealerAutoSaveManaStrategy multiplies only CastHealingSpellAction, so on a
                # class that never casts a heal the dial is inert rather than frugal.
                if name == "save mana" and spec_name is not None:
                    if not any(has_word(spec_name, word) for word in HEALER_SPEC_WORDS):
                        dead.add((key, name))
                        fail(f"conf: {key}: profile {label} uses {token!r} on {spec_name}, "
                             f"which is not a healer spec; the mana-conservation strategy only "
                             f"scales healing spells, so the dial does nothing")
                reason = CLASS_BASE_DIALS.get((cls, name)) if cls is not None else None
                if reason:
                    dead.add((key, name))
                    fail(f"conf: {key}: profile {label} uses {token!r} on class {cls}, where "
                         f"{reason}; the base is already there and the dial does nothing")

    notes.append(f"personality: {voice_profiles} voice profiles, {len(temperament_keys)} "
                 f"temperament blocks, {len(dials)} dials")
    if vocabulary is None:
        notes.append("personality: strategy vocabulary check skipped (no mod-playerbots checkout)")
    else:
        notes.append(f"personality: every dial is one of {len(vocabulary)} playerbots strategies "
                     f"(the Strategy contexts only, not the triggers, actions and values that "
                     f"register the same way)")

    if specs is None:
        notes.append("personality: spec applicability and coverage checks skipped "
                     "(no mod-playerbots conf)")
        return

    # Every PvE spec the module offers must have a pool, because an offer with no temperament
    # rolls a pawn that behaves like every other pawn of its class. Blacklist and variant blocks
    # are not held to this: 6.3 and 8.3 never had one, and that gap is not this check's to close.
    # `covered` only holds blocks that parsed to at least one profile: a block that is present
    # but empty is dropped by the loader and would leave the spec with no temperament at all.
    pve = {pair: name for pair, name in specs.items() if name.endswith("pve")}
    for pair, name in sorted(pve.items()):
        if pair not in covered:
            fail(f"conf: no Pawn.Temperament.{pair[0]}.{pair[1]} for the PvE premade spec "
                 f"{name}; an offer of it would roll an empty temperament")
    notes.append(f"personality: {len(set(pve) & covered)}/{len(pve)} PvE specs have a "
                 f"temperament")


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
        for column in ("mount_spell", "pet_name"):
            if column not in written:
                fail(f"code: the REPLACE INTO column list does not name {column}; the "
                     f"vanity read back after the build would be dropped on the next save")
        # CharacterDatabase.Execute's variadic overload formats its arguments with fmt, and the
        # core has no formatter for std::string, so a text argument is written out raw: unquoted
        # and unescaped. An empty name then leaves a trailing comma and any name makes the next
        # word look like a column, so the statement has to supply the quotes itself. The literal
        # is a run of adjacent strings, so look at everything up to the end of the call.
        end = cpp.find(";", statement.end())
        bound = cpp[statement.end():] if end < 0 else cpp[statement.end():end]
        if "'{}'" not in bound:
            fail("code: the REPLACE INTO statement binds its text column without quotes; "
                 "Execute's variadic overload formats a std::string unquoted and unescaped, so "
                 "the statement must spell the placeholder '{}' and the value must be escaped "
                 "before it is bound")

    # The other half of that: the statement quotes, the caller escapes.
    if "EscapeString" not in cpp:
        fail("code: PawnMgr.cpp never calls EscapeString; a pet name bound through Execute's "
             "variadic overload is written raw, so a quote in it ends the literal early and an "
             "empty one leaves a trailing comma in the statement")

    # Vanity is read back after the build because the mount and pet are the outcome, not an
    # input: the row has to carry them or the party menu can only say "a mount".
    for column in ("mount_spell", "pet_name"):
        if column not in columns:
            fail(f"code: the roster table has no {column} column")

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
    check_personality(keys)
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
