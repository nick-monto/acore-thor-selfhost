#!/usr/bin/env python3
"""Offline checks for modules/mod-mercenaries.

This host has no Boost headers, so no AzerothCore translation unit can be compiled here; the
compile gate is the arm64 workflow, which runs this script first. What can be checked offline is
the agreement between the files, which is where a module of this shape actually breaks:

  * every Mercenary.* key the C++ reads is defined, uncommented, in conf/mod-mercenaries.conf.dist;
  * the installer's MERCENARY_CONF_DEFAULTS and the shipped conf agree, both ways: the repair path
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

    python3 scripts/mercenaries/check_mercenaries.py
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "mod-mercenaries"
CONF = MODULE / "conf" / "mod-mercenaries.conf.dist"
SRC = MODULE / "src"
PINS = ROOT / "pins.toml"
WORKFLOW = ROOT / ".github" / "workflows" / "server-aarch64.yml"
INSTALLER = ROOT / "scripts" / "thor-install.sh"
# Sibling checkouts on a developer host, present in CI as the workflow clones them. Both can be
# pointed anywhere with AC_PLAYERBOTS and AC_CORE, which is what .github/workflows/module-checks.yml
# does. The checks that need them degrade to a note when they are absent rather than failing.
PLAYERBOTS = Path(os.environ.get("AC_PLAYERBOTS") or ROOT.parent / "mod-playerbots")
# The core is the other checkout; its script headers are what an override has to match.
CORE = Path(os.environ.get("AC_CORE") or ROOT.parent / "azerothcore-wotlk")
SCRIPT_HOOKS = CORE / "src" / "server" / "game" / "Scripting" / "ScriptDefines"
# An AI override is checked the same way as a script override, and its hooks are declared here:
# sGossipHello and sGossipSelect are UnitAI members (UnitAI.h:421-423), not ScriptDefines ones.
AI_HOOKS = [CORE / "src" / "server" / "game" / "AI" / "CoreAI" / "UnitAI.h",
            CORE / "src" / "server" / "game" / "AI" / "CreatureAI.h"]
# A hook declaration in the core, and a hook the module claims to override.
DECLARED_HOOK = re.compile(r"\bvirtual\s+[\w:<>&*\s]+?\b(\w+)\s*\(")
OVERRIDE = re.compile(r"\b(\w+)\s*\([^()]*\)\s*(?:const\s*)?override\b")
# A script type derived from ScriptObject whose header makes IsDatabaseBound return true: the
# manager matches its name against script_name and registers nothing without a row.
DATABASE_BOUND = re.compile(r"IsDatabaseBound\(\)\s*const\s*override\s*\{\s*return\s+true")
SCRIPT_OBJECT = re.compile(r"\bclass\s+(\w+)\s*:\s*public\s+ScriptObject\b")
# A class in the module and its base list: `class X : public Y`, `class X : Y("name") {`.
MODULE_CLASS = re.compile(r"\bclass\s+(\w+)\s*(?:final\s*)?:\s*([^{;]*)\{")

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
    for path in (CONF, PINS, WORKFLOW, INSTALLER, SRC / "MercenaryInnkeeper.cpp"):
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
        for match in re.finditer(r'GetOption<[^>]+>\(\s*"(Mercenary\.[^"]+)"', path.read_text(encoding="utf-8")):
            used.add(match.group(1))

    for key in sorted(used):
        if key not in keys:
            fail(f"conf: {key} is read by the code but not defined in mod-mercenaries.conf.dist")

    # Blacklist keys: class must be playable, spec inside the premade spec table.
    for key in sorted(keys):
        match = re.fullmatch(r"Mercenary\.(?:Blacklist|Variant|Temperament)\.(\d+)\.(\d+)", key)
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
                 f"{sum(1 for k in keys if k.startswith('Mercenary.Blacklist.'))} blacklist blocks, "
                 f"{sum(1 for k in keys if k.startswith('Mercenary.Variant.'))} variant blocks, "
                 f"{sum(1 for k in keys if k.startswith('Mercenary.Temperament.'))} temperament blocks")
    return keys


def check_installer_defaults(keys: dict[str, str]) -> None:
    installer = read(INSTALLER)
    if not installer:
        return

    block = re.search(r"^MERCENARY_CONF_DEFAULTS=\((.*?)^\)", installer, re.S | re.M)
    if not block:
        fail("installer: no MERCENARY_CONF_DEFAULTS block")
        return

    defaults: dict[str, str] = {}
    for match in re.finditer(r'"([^"=]+)=([^"]*)"', block.group(1)):
        defaults[match.group(1)] = match.group(2)

    for key, value in sorted(defaults.items()):
        if key not in keys:
            fail(f"installer: MERCENARY_CONF_DEFAULTS has {key}, which the conf does not define")
        elif keys[key] != value:
            fail(f"installer: {key}={value} but the conf ships {keys[key]}; a repair would "
                 f"rewrite a value the conf does not carry")

    # The other direction: every key the repair path can restore is in the defaults. The
    # per-class pools and the per-voice lines are deliberately absent: they are long, they change
    # with balance, and the player is meant to edit them, so a repair leaves them alone.
    for key in sorted(keys):
        if key.startswith(("Mercenary.Blacklist.", "Mercenary.Variant.", "Mercenary.Temperament.",
                           "Mercenary.VoiceLine.")):
            continue
        if key not in defaults:
            fail(f"installer: {key} is in the conf but not in MERCENARY_CONF_DEFAULTS; a repair "
                 f"would leave a bad value in place")

    notes.append(f"installer: {len(defaults)} conf defaults agree with the shipped conf")


def check_picker(keys: dict[str, str]) -> None:
    picker = read(SRC / "MercenaryInnkeeper.cpp")
    if not picker:
        return

    # The picker is one line on every innkeeper, not a gameobject: the AllCreatureScript answers the
    # two gossip hooks, and the flag check decides which creatures get the line. A GameObjectScript
    # here would need a template row and a spawn, and the module ships no SQL at all. A named
    # CreatureScript cannot serve it either: CreatureScript is database-bound, and gossip is
    # dispatched by the creature's own script id, which for an innkeeper is the core's
    # npc_innkeeper.
    if not re.search(r"\bclass\s+\w+\s*:\s*public\s+AllCreatureScript\b", picker):
        fail("picker: MercenaryInnkeeper.cpp declares no AllCreatureScript; the picker has no way "
             "to reach a creature without a template row or a ScriptName")
    for hook in ("CanCreatureGossipHello", "CanCreatureGossipSelect"):
        if not re.search(rf"\b{hook}\s*\(\s*Player\s*\*\s*\w+\s*,\s*Creature\s*\*\s*\w+", picker):
            fail(f"picker: the AllCreatureScript does not answer {hook}; the line never reaches "
                 f"a player")
    # The hooks must not be traded back for an AI. GetCreatureAI has to leave a creature's own AI
    # alone, so the only way to hand the picker out from there is to skip every innkeeper whose
    # creature_template.AIName is set - and 65 of the 125 innkeeper-flagged templates are SmartAI,
    # Razor Hill's Innkeeper Grosk (6928) among them. That is the bug this hook pair replaced.
    if re.search(r"\bGetCreatureAI\s*\(", picker):
        fail("picker: MercenaryInnkeeper.cpp hands out a CreatureAI again; that path skips every "
             "innkeeper whose creature_template.AIName is set (65 of 125 are SmartAI)")
    if re.search(r"GetCreatureTemplate\(\)\s*->\s*AIName", picker):
        fail("picker: the picker still refuses creatures by AIName")
    if "UNIT_NPC_FLAG_INNKEEPER" not in picker:
        fail("picker: the hello hook does not gate on UNIT_NPC_FLAG_INNKEEPER")
    if "GameObjectScript" in picker:
        fail("picker: still a GameObjectScript")

    # Action codes below GOSSIP_OPTION_MAX (21) would swallow the innkeeper's own bind option,
    # because a database gossip row's action is its OptionType.
    open_action = re.search(r"MERCENARY_ACTION_OPEN\s*=\s*(\d+)", picker)
    if not open_action:
        fail("picker: no MERCENARY_ACTION_OPEN")
    elif int(open_action.group(1)) <= 21:
        fail(f"picker: MERCENARY_ACTION_OPEN is {open_action.group(1)}, inside the range the core "
             f"uses for its own gossip options")

    # The picker draws from the module's own pool query. Falling back to
    # sRandomPlayerbotMgr.addclassCache would silently show nothing, because that cache is only
    # filled when Playerbots.AddClassCommand is on and the installer turns it off.
    mgr = read(SRC / "MercenaryMgr.cpp")
    if "addclassCache" in mgr:
        fail("pool: MercenaryMgr.cpp reads addclassCache, which is empty with AddClassCommand = 0")
    for needle in ("playerbots_account_type", "RefreshPool", "SelectionFor",
                   "sMercenaryPersonality.Temperament", "sMercenaryPersonality.Apply"):
        if needle not in mgr:
            fail(f"pool: MercenaryMgr.cpp has no {needle}")

    # The selection is the pool cycle's, not the menu's: RefreshPool has to clear it, or walking out
    # of the inn and back in would reroll the crowd. And nothing may drop it on logout for the same
    # reason - the only things that end a selection are a refresh and a restart.
    if not re.search(r"_decks\.clear\(\)\s*;\s*\n\s*_selections\.clear\(\)", mgr):
        fail("pool: RefreshPool does not clear the selections with the decks")
    if re.search(r"_selections\.erase", mgr):
        fail("pool: MercenaryMgr.cpp erases a selection outside RefreshPool; only a pool refresh or a "
             "restart may change a player's crowd")

    for key in ("Mercenary.FeaturedMin", "Mercenary.FeaturedMax",
                "Mercenary.PoolRefreshMinMinutes", "Mercenary.PoolRefreshMaxMinutes"):
        if key not in keys:
            fail(f"pool: {key} is missing from the conf")

    # The obelisk is gone; a leftover key or script would look like it still selects something.
    for path in (CONF, INSTALLER, *sorted(SRC.glob("*.cpp"))):
        if "ObeliskEntry" in path.read_text(encoding="utf-8") and path != INSTALLER:
            fail(f"{path.relative_to(ROOT)}: still refers to Mercenary.ObeliskEntry")
    if "Mercenary\\.ObeliskEntry" not in read(INSTALLER):
        fail("installer: does not drop a stale Mercenary.ObeliskEntry from an existing conf")

    notes.append("picker: the two AllCreatureScript gossip hooks on every innkeeper, "
                 "actions clear of the core's option range")


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
TEMPERAMENT_KEY = re.compile(r"^Mercenary\.Temperament\.(\d+)\.(\d+)$")
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
    """True when `word` is a whole word of `name`, the rule MercenaryArchetype::RoleFor uses."""
    return re.search(rf"(?<![A-Za-z]){re.escape(word)}(?![A-Za-z])", name.lower()) is not None


def check_personality(keys: dict[str, str]) -> None:
    vocabulary = strategy_vocabulary()
    specs = premade_specs()

    pools: list[tuple[str, str]] = []
    if "Mercenary.Voice" not in keys:
        fail("conf: no Mercenary.Voice; every mercenary would roll the same non-combat behaviour")
    else:
        pools.append(("Mercenary.Voice", keys["Mercenary.Voice"]))
    temperament_keys = sorted(k for k in keys if k.startswith("Mercenary.Temperament."))
    if not temperament_keys:
        fail("conf: no Mercenary.Temperament.* blocks; every mercenary would roll an empty temperament")
    pools.extend((key, keys[key]) for key in temperament_keys)

    voice_profiles = 0
    dials: set[str] = set()
    dead: set[tuple[str, str]] = set()
    covered: set[tuple[int, int]] = set()
    for key, value in pools:
        profiles = parse_pool(value, key)
        if not profiles:
            # MercenaryPersonality::Load keeps a block only `if (!pool.empty())`, so an empty one does
            # not survive as an empty temperament: it is dropped and the spec falls back to
            # whatever the class default is, silently.
            fail(f"conf: {key} defines no profiles; the loader drops an empty pool, so this "
                 f"block adds nothing")
        if key == "Mercenary.Voice":
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
    # rolls a mercenary that behaves like every other mercenary of its class. Blacklist and variant blocks
    # are not held to this: 6.3 and 8.3 never had one, and that gap is not this check's to close.
    # `covered` only holds blocks that parsed to at least one profile: a block that is present
    # but empty is dropped by the loader and would leave the spec with no temperament at all.
    pve = {pair: name for pair, name in specs.items() if name.endswith("pve")}
    for pair, name in sorted(pve.items()):
        if pair not in covered:
            fail(f"conf: no Mercenary.Temperament.{pair[0]}.{pair[1]} for the PvE premade spec "
                 f"{name}; an offer of it would roll an empty temperament")
    notes.append(f"personality: {len(set(pve) & covered)}/{len(pve)} PvE specs have a "
                 f"temperament")

    # The traits must land after the last ResetStrategies in the build path: that call rebuilds
    # the role defaults, so a trait applied before it is wiped on the next login.
    mgr = read(SRC / "MercenaryMgr.cpp")
    reset = mgr.rfind("ResetStrategies")
    applied = mgr.rfind("sMercenaryPersonality.Apply")
    if reset == -1 or applied == -1:
        fail("personality: MercenaryMgr.cpp has no ResetStrategies or no sMercenaryPersonality.Apply; the "
             "traits cannot be landing on a rebuilt engine")
    elif applied < reset:
        fail("personality: sMercenaryPersonality.Apply comes before the last ResetStrategies in "
             "MercenaryMgr.cpp; that call rebuilds the role defaults, so the trait is wiped")


VOICE_EVENTS = ("kill", "death", "dismiss")


def check_voice_lines(keys: dict[str, str]) -> None:
    """Every Mercenary.VoiceLine block names a voice the pool defines, and events the module speaks.

    A block keyed by a label the pool does not have is dead configuration: a mercenary's voice is one of
    Mercenary.Voice's labels, so nothing can ever draw it. The loader drops such a key with a warning at
    boot; this is the same check before the build, where it is cheap to fix.
    """
    if "Mercenary.Voice" not in keys:
        return  # check_personality already reports a missing pool

    labels: set[str] = set()
    for chunk in keys["Mercenary.Voice"].split("|"):
        label = chunk.strip().partition(":")[0].strip()
        if label:
            labels.add(label)

    line_keys = sorted(key for key in keys if key.startswith("Mercenary.VoiceLine."))
    if not line_keys:
        fail("conf: no Mercenary.VoiceLine.* blocks; no mercenary would ever say anything")
        return

    for key in line_keys:
        label = key[len("Mercenary.VoiceLine."):]
        if label not in labels:
            fail(f"conf: {key} names the voice {label!r}, which Mercenary.Voice does not define; "
                 f"a mercenary's voice is one of that pool's labels, so the line can never be drawn")

        spoken: set[str] = set()
        for chunk in keys[key].split("|"):
            entry = chunk.strip()
            if not entry:
                continue  # a trailing or doubled '|' is a formatting slip, as in the module
            event, separator, text = entry.partition(":")
            event, text = event.strip(), text.strip()
            if not separator or not event:
                fail(f"conf: {key}: {entry!r} is not `event: text`")
                continue
            if event not in VOICE_EVENTS:
                fail(f"conf: {key}: {entry!r} names the event {event!r}; the module speaks "
                     f"{', '.join(VOICE_EVENTS)}")
                continue
            if not text:
                fail(f"conf: {key}: the {event} line has no text")
            spoken.add(event)

        notes.append(f"voice lines: {label} speaks {', '.join(sorted(spoken)) or 'nothing'}")


def check_roster() -> None:
    cpp = read(SRC / "MercenaryMgr.cpp")
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
        fail("code: MercenaryMgr.cpp never calls EscapeString; a pet name bound through Execute's "
             "variadic overload is written raw, so a quote in it ends the literal early and an "
             "empty one leaves a trailing comma in the statement")

    # Vanity is read back after the build because the mount and pet are the outcome, not an
    # input: the row has to carry them or the party menu can only say "a mount".
    for column in ("mount_spell", "pet_name"):
        if column not in columns:
            fail(f"code: the roster table has no {column} column")

    if "state" not in columns or "target_level" not in columns:
        fail("code: the roster table is missing state or target_level")

    # The module was mod-pawn before it was mod-mercenaries and the roster table went with it. The
    # old name may only appear in the two statements that migrate it: the existence probe and the
    # RENAME. Anywhere else it would be a query reading a table nothing creates.
    old = [line.strip() for line in cpp.splitlines() if "pawn_roster" in line]
    allowed = [line for line in old if "RENAME TABLE" in line or "TableExists(" in line]
    if len(old) != len(allowed) or not any("RENAME TABLE" in line for line in old):
        stray = [line for line in old if line not in allowed]
        fail(f"code: pawn_roster survives outside the migration: {stray or old}")

    notes.append(f"schema: {table_name} has {len(columns)} named columns, migrated from pawn_roster")


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
    if "[module.mercenary]" not in pins:
        fail("pins.toml: no [module.mercenary] entry")
    if 'path = "modules/mod-mercenaries"' not in pins:
        fail('pins.toml: [module.mercenary] does not point at modules/mod-mercenaries')

    workflow = read(WORKFLOW)
    for needle in ("modules/mod-mercenaries", "mod-mercenaries.conf.dist", "Mercenary.FeaturedMin"):
        if needle not in workflow:
            fail(f"workflow: {needle} is not mentioned; the build would not ship the module")
    if "modules/mod-mercenaries/data/sql" in workflow:
        fail("workflow: still lists modules/mod-mercenaries/data/sql, which no longer exists")

    installer = read(INSTALLER)
    for needle in ("mod-mercenaries", "MERCENARY_CONF_DEFAULTS", "mercenary_value_ok"):
        if needle not in installer:
            fail(f"installer: {needle} is not mentioned; an install would not configure the module")
    if "modules/mod-mercenaries/data/sql" in installer:
        fail("installer: still lists modules/mod-mercenaries/data/sql, which no longer exists")

    # The fast gate is only a gate if it is wired: a workflow that runs the compile check on push
    # is what keeps a misspelt core symbol from waiting for the arm64 build.
    checks_workflow = ROOT / ".github" / "workflows" / "module-checks.yml"
    if not checks_workflow.is_file():
        fail("wiring: no .github/workflows/module-checks.yml; the compile check would only run "
             "when someone remembers to run it")
    else:
        text = read(checks_workflow)
        for needle in ("push:", "scripts/syntax-check.sh", "scripts/mercenaries/check_mercenaries.py",
                       "scripts/module-flags.txt"):
            if needle not in text:
                fail(f"wiring: module-checks.yml does not mention {needle}")

    notes.append("wiring: pins, the arm64 workflow, the module-checks workflow and the installer "
                 "mention the module")


def check_flags_file() -> None:
    """The checked-in compile flags must belong to the core the build pins.

    scripts/module-flags.txt is what CI compiles the modules against when it has no configured
    build, so flags taken from a different core commit would check the wrong headers - or, worse,
    compile cleanly against an include set the real build no longer has.
    """
    flags = ROOT / "scripts" / "module-flags.txt"
    if not flags.is_file():
        fail("flags: no scripts/module-flags.txt; the module-checks workflow has nothing to "
             "compile against, so it would skip every translation unit")
        return

    text = read(flags)
    recorded = re.search(r"^# core: ([0-9a-f]{40})$", text, re.M)
    if not recorded:
        fail("flags: scripts/module-flags.txt has no `# core: <commit>` line; regenerate it with "
             "scripts/syntax-check.sh --dump-flags")
        return

    pins = read(PINS)
    section = re.search(r"^\[core\]$(.*?)(?=^\[)", pins, re.M | re.S)
    pinned = re.search(r'^commit = "([0-9a-f]{40})"', section.group(1), re.M) if section else None
    if not pinned:
        fail("pins: [core] has no 40-character commit to compare the flags against")
        return
    if pinned.group(1) != recorded.group(1):
        fail(f"flags: scripts/module-flags.txt was taken from core {recorded.group(1)[:7]}, but "
             f"pins.toml builds {pinned.group(1)[:7]}; regenerate it with "
             f"scripts/syntax-check.sh --dump-flags")

    for needle in ("-std=gnu++20", "@CORE@", "@PLAYERBOTS@"):
        if needle not in text:
            fail(f"flags: scripts/module-flags.txt has no {needle}; it is not a module compile "
                 f"line any more")
    if "/home/" in text:
        fail("flags: scripts/module-flags.txt carries an absolute home path; the checkout roots "
             "must stay as placeholders so another machine can use the file")

    notes.append(f"flags: the compile flags belong to the pinned core {pinned.group(1)[:7]} and "
                 f"carry placeholders, not paths")


def check_core_hooks() -> None:
    """Every hook the module overrides must be one the core declares.

    A misspelt hook is not an override: the compiler says so, and the arm64 job is the only
    compiler this repository has, so the names are checked here instead. `OnLogin` for
    `OnPlayerLogin` compiled nowhere and failed the build a day after it was written.

    The AI hooks count as much as the script hooks: the picker used to override `sGossipHello`
    and `sGossipSelect`, which are UnitAI members (UnitAI.h:421-423) that no ScriptDefines header
    declares, and it now overrides `CanCreatureGossipHello` and `CanCreatureGossipSelect`, which
    AllCreatureScript.h does declare (AllCreatureScript.h:67,79).
    """
    if not SCRIPT_HOOKS.is_dir():
        notes.append("hooks: no core checkout beside this repository, so the override names, "
                     "the database-bound script types and the config-pass order are unchecked")
        return

    declared: set[str] = set()
    headers = list(SCRIPT_HOOKS.glob("*.h")) + [path for path in AI_HOOKS if path.is_file()]
    for header in headers:
        declared.update(DECLARED_HOOK.findall(header.read_text()))

    overridden: set[str] = set()
    for source in sorted(SRC.glob("*.cpp")):
        overridden.update(OVERRIDE.findall(source.read_text()))

    for name in sorted(overridden - declared):
        fail(f"hooks: {name} is marked override, but the core declares no such hook; the "
             f"script would compile nowhere and the arm64 build would stop")

    notes.append(f"hooks: {len(overridden)} overrides, all declared by the core's script and "
                 f"AI headers")


def database_bound_types() -> set[str] | None:
    """The script types the core matches against script_name, or None without a core checkout.

    A header makes its type database-bound by overriding IsDatabaseBound to return true; the
    concrete type is the one deriving from ScriptObject in that same header.
    """
    if not SCRIPT_HOOKS.is_dir():
        return None

    types: set[str] = set()
    for header in SCRIPT_HOOKS.glob("*.h"):
        text = header.read_text()
        if DATABASE_BOUND.search(text):
            types.update(SCRIPT_OBJECT.findall(text))
    return types or None


def check_database_bound_scripts() -> None:
    """No module class may derive from a database-bound script type.

    Such a script is looked up in script_name and dropped with `Script named ... is not assigned
    in the database` when there is no row (ScriptMgr.h:817-885), and this module ships no SQL to
    add one. That was the boot failure of the picker's first shape.
    """
    types = database_bound_types()
    if types is None:
        notes.append("scripts: no core checkout, so the database-bound script types are "
                     "unchecked")
        return

    unbound = []
    for path in sorted([*SRC.glob("*.cpp"), *SRC.glob("*.h")]):
        text = path.read_text(encoding="utf-8")
        for match in MODULE_CLASS.finditer(text):
            derived, bases = match.group(1), match.group(2)
            for base in re.findall(r"\b[A-Za-z_]\w*\b", bases):
                if base in types:
                    unbound.append(f"{path.relative_to(ROOT)}: {derived} : {base}")

    for derived in unbound:
        fail(f"scripts: {derived} derives from a database-bound script type; the manager looks "
             f"its name up in script_name and registers nothing when there is no row, so it is "
             f"dropped at boot unless the database has a row for its name")

    notes.append(f"scripts: no class derives from the {len(types)} database-bound script types "
                 f"({', '.join(sorted(types))})")


def method_body(text: str, name: str) -> str:
    """The body of the first `name(...)` method in `text`, with its braces matched."""
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?\{{", text)
    if not match:
        return ""

    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.end():index]
    return ""


def check_startup_tables() -> None:
    """The spell-derived tables are built in OnStartup, not at the first config load.

    World::SetInitialWorldSettings calls OnAfterConfigLoad at World.cpp:302 and
    sSpellMgr->LoadSpellInfoStore at World.cpp:407, so the spell store is empty during the first
    config pass: every blacklist and variant spell name fails to resolve there. A reload happens
    on a running server, where the store is loaded, so that rebuild stays guarded.
    """
    cpp = read(SRC / "MercenaryScripts.cpp")
    if not cpp:
        return

    if "sMercenaryArchetype.Load()" not in method_body(cpp, "OnStartup"):
        fail("startup: sMercenaryArchetype.Load() is not in the OnStartup body; the first config "
             "pass runs before sSpellMgr->LoadSpellInfoStore (World.cpp:302 and World.cpp:407), "
             "so the archetype and variant tables build against an empty spell store and every "
             "blacklist and variant spell name fails to resolve")

    config = method_body(cpp, "OnAfterConfigLoad")
    guarded = re.search(r"if\s*\(\s*reload\s*\)\s*\{[^}]*sMercenaryArchetype\.Load\(\)", config)
    if not guarded:
        fail("startup: the rebuild in the OnAfterConfigLoad body is not guarded by "
             "`if (reload)`; the first pass runs before the spell store is loaded, so an "
             "unguarded build leaves every blacklist and variant spell name unresolved")
    elif config.count("sMercenaryArchetype.Load()") != 1:
        fail("startup: the OnAfterConfigLoad body builds the archetype table more than once, "
             "so a call sits outside the `if (reload)` guard; the first pass runs before the "
             "spell store is loaded and that call resolves nothing")

    # The same trap catches anything else that reads a store the core fills after the config:
    # a manager reached from this body reads a half-built world. Only sConfigMgr is safe here.
    # Comments are stripped first: this body explains why it defers, and it names the managers
    # it must not touch.
    code = re.sub(r"/\*.*?\*/", "", config, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    premature = sorted(set(re.findall(r"\b(sSpellMgr|sObjectMgr|sWorld|sMapMgr|sPlayerbotAIConfig)\b",
                                      code)))
    if premature:
        fail(f"startup: the OnAfterConfigLoad body reaches {', '.join(premature)}, which the core "
             f"fills after the config pass (sSpellMgr at World.cpp:407, and a module's own "
             f"config in its own hook, in no guaranteed order); read it from OnStartup instead")

    notes.append("startup: the archetype and personality tables are built in OnStartup, after "
                 "the spell store, and a reload-only rebuild is guarded by `if (reload)`")


def check_temperament_keys() -> None:
    """The temperament keys are enumerated, never probed.

    A probe of `Mercenary.Temperament.<class>.<spec>` is a missing-option warning for every spec the
    conf does not define, and the class x spec grid is mostly undefined by design: the old
    probe wrote 47 `Config: Missing property` lines at boot.
    """
    cpp = read(SRC / "MercenaryPersonality.cpp")
    if not cpp:
        return

    if re.search(r'GetOption<[^>]+>\(\s*"Mercenary\.Temperament\.', cpp):
        fail("temperament: MercenaryPersonality.cpp asks for a literal Mercenary.Temperament.<class>."
             "<spec> key; every spec the conf does not define logs `Config: Missing property` "
             "at boot, so the keys must be enumerated instead")

    prefix = re.search(r'(\w+)\s*=\s*"Mercenary\.Temperament\."', cpp)
    # Every pool is enumerated the same way now (voice lines joined temperament), so the check
    # looks for a call that takes the temperament prefix rather than for the first call in the
    # file: which pool is read first is an implementation detail, that this one is enumerated is
    # the invariant.
    wanted = {'"Mercenary.Temperament."'} | ({prefix.group(1)} if prefix else set())
    enumerated = re.findall(r"GetKeysByString\(\s*([^)]*?)\s*\)", cpp)
    if not prefix or not any(argument in wanted for argument in enumerated):
        fail("temperament: MercenaryPersonality.cpp does not enumerate the keys with "
             "GetKeysByString over the Mercenary.Temperament. prefix; a spec the conf leaves out "
             "would then be probed and log `Config: Missing property`")

    notes.append("temperament: the keys present are enumerated with GetKeysByString, so an "
                 "absent <class>.<spec> is silent and that spec has no temperament")


def main() -> int:
    check_files()
    keys = check_config_keys()
    check_installer_defaults(keys)
    check_picker(keys)
    check_personality(keys)
    check_voice_lines(keys)
    check_roster()
    check_loader()
    check_wiring()
    check_core_hooks()
    check_flags_file()
    check_database_bound_scripts()
    check_startup_tables()
    check_temperament_keys()

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
