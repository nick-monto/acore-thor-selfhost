#!/usr/bin/env python3
"""Give the client the two shield decks: register item models as creature displays, ship the DBCs.

Why this is needed
------------------
A mount display is a ``CreatureDisplayInfo`` row pointing at a ``CreatureModelData`` row
(``DBCStructure.h``), and the server only sends the id (``UNIT_FIELD_MOUNTDISPLAYID``). The stock
client has no shield model registered as a creature model - checked: 0 of the ``Item\\ObjectComponents
\\Shield`` models appear in ``CreatureModelData.dbc`` - so the shield decks the design allocates
(`engravedwall` 90001, `coffinlid` 90002) do not render until two rows exist per deck.

The rows are *cloned* from a working mount rather than invented: the ``log`` deck (display 26609 →
model 2920) is a stock flat model the client already draws a rider on, so its flags, footprints,
sound and collision are copied verbatim and only the id, the model path and the display scale
differ. Everything a DBC cannot express - the model's own orientation - comes from the model, which
is why the shields are the flattest art in the data (``Shield_Rectangle_B_01.mdx``, "Engraved Wall").

This script reads the two DBCs the installer already put in the server's data directory (they are
copies of the client's), appends the rows, and writes both patched DBCs (for the server, so
``sCreatureDisplayInfoStore`` agrees and the module accepts the deck) and a patch archive (for the
client's ``Data`` directory, under the highest free patch name so it loads after every archive
already there). It is the same route as ``scripts/item-dbc/patch_item_dbc.py``, and shares its
container and MPQ writer through ``scripts/lib/dbc_mpq.py``.

Usage, on the realm host (Termux; `pkg install python` once):

    RF=$PREFIX/var/lib/proot-distro/installed-rootfs/ubuntu
    python3 scripts/skateboard/patch_deck_dbc.py --dbcs "$RF/opt/acore/data/dbc" \\
        --client-data "/sdcard/WOTLK 3.3.5a/Data" --server-dbc "$RF/opt/acore/data/dbc"

The DBCs live inside the proot rootfs, so they are addressed through the rootfs path from Termux; the
client's `Data` directory is on shared storage and reachable from both. Both copies are optional;
without them the patched files and the archive land in ``--out`` for the operator to place by hand.
``--check`` re-derives everything in memory and fails if the files in ``--out`` differ, and
``--verify --client-data`` answers which archive the client actually reads ``CreatureDisplayInfo.dbc``
from and whether it carries the decks.

The scale is the one number to tune on the device: the shields are authored as held items, so how
large they read under a rider is a judgement the DBC row makes for you (``--scale``, default 2.0, the
stock log's own display scale). ``--mount-height`` shifts the rider up off the deck.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "lib"))
sys.dont_write_bytecode = True      # keep the run out of the source tree
import dbc_mpq  # noqa: E402  (the shared DBC container and MPQ writer)

# The two DBCs, at the width this client ships. Both counts were read off the files the installer
# copies in: 28 fields of 4 bytes for CreatureModelData, 16 for CreatureDisplayInfo.
MODEL_DBC = "CreatureModelData.dbc"
DISPLAY_DBC = "CreatureDisplayInfo.dbc"
MODEL_DBC_FIELDS = 28
DISPLAY_DBC_FIELDS = 16

# The rows our decks are cloned from: the stock `Log Ride`, display 26609 → model 2920, which the
# client already draws a rider standing on. Anything this script does not set comes from these.
REFERENCE_DISPLAY_ID = 26609
REFERENCE_MODEL_ID = 2920

# The ids the module resolves the aliases to (`Skateboard.cpp`: DeckEngravedWall / DeckCoffinLid) and
# docs/SKATEBOARDING-DESIGN.md §4.2 allocates. Above every id either file ships (32754 / 3440).
DECK_IDS = {"plank": 90001, "coffinlid": 90002}

# A doodad model, not a shield. Item models are *skinned at draw time*: their M2 carries no texture
# path, and the client supplies one from ItemDisplayInfo (which is how the same shield model can be
# painted several ways). Nothing supplies that on a creature, so a shield registered as a creature
# model draws white - and its authored frame is "held", so it stands on edge instead of lying flat.
# A world doodad is self-skinned (its M2 names its own .blp) and authored on the ground plane, which
# is the shape a deck wants. Both of these are from docs/SKATEBOARDING-DESIGN.md §4.2.
DECK_MODELS = {
    "plank": "World\\Azeroth\\RedRidge\\PassiveDoodads\\DockPieces\\RedRidgeDocksBoardwalk01.mdx",
    "coffinlid": "World\\Azeroth\\Duskwood\\PassiveDoodads\\CoffinLid\\CoffinLid.mdx",
}

DEFAULT_SCALE = 2.0        # display 26609's own scale: the log the decks replace
FIRST_ID = min(DECK_IDS.values())
ID_COUNT = len(DECK_IDS)

STAGED_ARCHIVE = "deck-dbc.MPQ"
MEMBERS = [f"DBFilesClient\\{MODEL_DBC}", f"DBFilesClient\\{DISPLAY_DBC}"]


# ---------------------------------------------------------------------------
# Rows
# ---------------------------------------------------------------------------

def reference_rows(models, displays):
    """The stock log's two rows, which every deck row is a copy of."""
    model = next((row for row in models if row[0] == REFERENCE_MODEL_ID), None)
    display = next((row for row in displays if row[0] == REFERENCE_DISPLAY_ID), None)
    if model is None:
        raise SystemExit(f"{MODEL_DBC} has no row {REFERENCE_MODEL_ID}, the deck rows are cloned from it")
    if display is None:
        raise SystemExit(f"{DISPLAY_DBC} has no row {REFERENCE_DISPLAY_ID}, the deck rows are cloned from it")
    return model, display


def resolve_model(alias: str, override: str | None) -> str:
    """The model path for one deck: an operator's --model, else the shipped plank."""
    if not override:
        return DECK_MODELS[alias]
    path = override.replace("/", "\\").upper()
    # A path is what CreatureModelData stores: relative to the client's Data directory, backslashes.
    # A .mdx and a .m2 are the same model to the client, and the DBC stores whatever the file is
    # called, so either spelling is accepted as long as it is not an absolute one.
    if ":" in path or path.startswith("\\"):
        raise SystemExit(f"--model {override!r}: give a client-relative path, e.g. WORLD\\...\\plank.m2")
    return path


def deck_rows(models, displays, strings: bytes, scale: float, mount_height: float,
              overrides: dict[str, str] | None = None):
    """The model and display records to append, and the string block they need."""
    model_ref, display_ref = reference_rows(models, displays)
    new_models, new_displays = [], []
    for alias, display_id in DECK_IDS.items():
        model_id = display_id                      # one model per deck, same id in both files
        path, strings = dbc_mpq.add_string(strings, resolve_model(alias, (overrides or {}).get(alias)))

        model = list(model_ref)
        model[0] = model_id
        model[2] = path                            # ModelPath
        # MountHeight is where the rider's feet sit. Zero, like the log, puts them at the model's
        # origin; the deck's own geometry is what the rider stands on.
        model[16] = dbc_mpq.float_bits(mount_height)
        new_models.append(tuple(model))

        display = list(display_ref)
        display[0] = display_id                    # Displayid
        display[1] = model_id                      # ModelId
        display[4] = dbc_mpq.float_bits(scale)     # CreatureModelScale
        new_displays.append(tuple(display))
    return new_models, new_displays, strings


# Creature model paths the client skins *itself*: the M2 carries the material's texture path, so
# registering one as a creature model draws it textured. Everything under ITEM or CHARACTER is
# skinned at draw time from ItemDisplayInfo/CharSections, and nothing supplies that on a creature -
# which is why a shield registered as a creature model draws white.
SELF_SKINNED_PREFIXES = ("WORLD\\", "ENVIRONMENTS\\", "DUNGEONS\\", "CREATURE\\")
SKIN_SUPPLIED_PREFIXES = ("ITEM\\", "CHARACTER\\")


def _model_table(dbc_dir: Path) -> dict[str, tuple[int, list[tuple[int, float]]]]:
    """Model path -> (model id, [(display id, scale)]) for everything the client already registers."""
    models, model_strings = dbc_mpq.read_dbc(dbc_dir / MODEL_DBC, MODEL_DBC_FIELDS)
    displays, _ = dbc_mpq.read_dbc(dbc_dir / DISPLAY_DBC, DISPLAY_DBC_FIELDS)

    rows: dict[str, tuple[int, list[tuple[int, float]]]] = {}
    for model in models:
        path = dbc_mpq.string_at(model_strings, model[2]).upper()
        rows[path] = (model[0], [])
    for display in displays:
        for path, (model_id, used) in rows.items():
            if model_id == display[1]:
                used.append((display[0], dbc_mpq.bits_float(display[4])))
    return rows


def _model_names(listfile: Path) -> list[str]:
    names = []
    for line in listfile.read_text(encoding="utf-8", errors="replace").splitlines():
        name = line.strip().replace("/", "\\")
        if name and name.lower().endswith((".mdx", ".m2")):
            names.append(name)
    return names


def _archive_names(data_dir: Path) -> tuple[list[str], list[str]]:
    """Every model name the client's archives advertise, and the archives that could not be read."""
    names, unreadable = [], []
    for _, path in sorted(dbc_mpq.client_archives(data_dir)):
        try:
            names.extend(name for name in dbc_mpq.list_mpq_files(path)
                         if name.lower().endswith((".mdx", ".m2")))
        except SystemExit as error:
            unreadable.append(str(error))
    return names, unreadable


def survey(dbc_dir: Path, hint: str, listfile: Path | None = None,
           data_dir: Path | None = None) -> int:
    """Find a board: every model whose path contains HINT, and what it would take to draw it.

    Three sources, because a model has to be in one of them to be used: the model table a creature
    display already points at, a 3.3.5a listfile (the only place a *doodad's* path is written down),
    and the client's own archives when they can be read.
    """
    for path in (dbc_dir / MODEL_DBC, dbc_dir / DISPLAY_DBC):
        if not path.is_file():
            raise SystemExit(f"{path} does not exist; --dbcs is the directory the two DBCs are in")

    table = _model_table(dbc_dir)

    candidates: dict[str, str] = {}          # upper path -> where it came from
    for path in table:
        candidates.setdefault(path, "registered as a creature model")
    if listfile:
        if not listfile.is_file():
            raise SystemExit(f"{listfile} is not a file; --listfile takes a 3.3.5a listfile.txt")
        for name in _model_names(listfile):
            candidates.setdefault(name.upper(), f"in {listfile.name}")
    if data_dir:
        names, unreadable = _archive_names(data_dir)
        for name in names:
            candidates.setdefault(name.upper(), "in the client's archives")
        for note in unreadable:
            print(f"note: {note}")

    wanted = hint.upper().replace("/", "\\")
    hits = sorted(path for path in candidates if wanted in path)

    if not hits:
        print(f"no model path containing '{hint}' in "
              f"{len(candidates)} known path(s)")
        return 1

    for path in hits:
        print(path)
        print(f"  {candidates[path]}")
        if path.startswith(SELF_SKINNED_PREFIXES):
            print("  self-skinned: the M2 names its own texture, so a creature display draws it textured")
        elif path.startswith(SKIN_SUPPLIED_PREFIXES):
            print("  skin-supplied: an item model, drawn white when a creature display points at it")
        registered = table.get(path)
        if registered and registered[1]:
            display_id, scale = registered[1][0]
            print(f"  model {registered[0]}, display {display_id}, scale {scale:g}   -> Skateboard.Deck = {display_id}")
        else:
            print(f"  not registered as a creature model   -> --model plank={path.lower()}")
        print()

    print(f"{len(hits)} of {len(candidates)} known model paths contain '{hint}'. A registered one is a "
          f"display id for Skateboard.Deck; the others need the patch row this tool writes (--model).")
    return 0


# ---------------------------------------------------------------------------
# Archive
# ---------------------------------------------------------------------------

def patch_archive(model_dbc: bytes, display_dbc: bytes) -> bytes:
    listfile = "".join(f"{name}\r\n" for name in MEMBERS).encode("ascii")
    return dbc_mpq.build_mpq([
        (MEMBERS[0], model_dbc),
        (MEMBERS[1], display_dbc),
        ("(listfile)", listfile),
    ])


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build(dbc_dir: Path, scale: float, mount_height: float,
          overrides: dict[str, str] | None = None) -> tuple[bytes, bytes, bytes]:
    model_path = dbc_dir / MODEL_DBC
    display_path = dbc_dir / DISPLAY_DBC
    for path in (model_path, display_path):
        if not path.is_file():
            raise SystemExit(f"{path} does not exist; --dbcs is the directory the two DBCs are in")

    models, model_strings = dbc_mpq.read_dbc(model_path, MODEL_DBC_FIELDS)
    displays, display_strings = dbc_mpq.read_dbc(display_path, DISPLAY_DBC_FIELDS)

    new_models, new_displays, model_strings = deck_rows(
        models, displays, model_strings, scale, mount_height, overrides
    )
    ids = range(FIRST_ID, FIRST_ID + ID_COUNT)
    if len(new_models) != ID_COUNT or len(new_displays) != ID_COUNT:
        raise SystemExit(f"{len(new_models)} model and {len(new_displays)} display rows for {ID_COUNT} decks")

    models = dbc_mpq.extend_records(models, new_models, ids, str(model_path))
    displays = dbc_mpq.extend_records(displays, new_displays, ids, str(display_path))

    patched_models = dbc_mpq.write_dbc(models, model_strings, MODEL_DBC_FIELDS)
    patched_displays = dbc_mpq.write_dbc(displays, display_strings, DISPLAY_DBC_FIELDS)
    archive = patch_archive(patched_models, patched_displays)

    dbc_mpq.assert_readable(archive, MEMBERS[0], patched_models)
    dbc_mpq.assert_readable(archive, MEMBERS[1], patched_displays)
    return patched_models, patched_displays, archive


def carries_decks(data: bytes) -> bool:
    """Whether these bytes are a CreatureDisplayInfo.dbc holding every deck display."""
    try:
        records, _ = dbc_mpq.parse_dbc(data, "an archive's " + DISPLAY_DBC, DISPLAY_DBC_FIELDS)
    except SystemExit:
        return False
    return set(DECK_IDS.values()) <= {record[0] for record in records}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dbcs", type=Path, help=f"the directory holding {MODEL_DBC} and {DISPLAY_DBC}")
    parser.add_argument("--out", type=Path, default=HERE.parents[1] / "build" / "deck-dbc",
                        help="where to write the patched DBCs and the patch archive")
    parser.add_argument("--client-data", type=Path, help="the client's Data directory; the patch archive is copied in under the first free name")
    parser.add_argument("--server-dbc", type=Path, help="the server's data/dbc directory; the patched DBCs are copied in")
    parser.add_argument("--scale", type=float, default=DEFAULT_SCALE,
                        help=f"the display scale of the deck (default {DEFAULT_SCALE}, the stock log's)")
    parser.add_argument("--mount-height", type=float, default=0.0,
                        help="how far above the deck's origin the rider stands (default 0)")
    parser.add_argument("--model", action="append", metavar="ALIAS=PATH", default=[],
                        help="draw a deck with another client model instead of its shipped shield, e.g. "
                             "--model engravedwall=WORLD\\\\GENERIC\\\\SOMEDOODAD\\\\plank.m2; repeatable")
    parser.add_argument("--survey", metavar="HINT", help="list every model whose path contains HINT - registered ones with their display ids - and exit")
    parser.add_argument("--listfile", type=Path,
                        help="with --survey: a 3.3.5a listfile.txt, the only listing of doodad model paths")
    parser.add_argument("--data-dir", type=Path,
                        help="with --survey: the client's Data directory; its archives' listfiles are read too")
    parser.add_argument("--check", action="store_true", help="fail if the files in --out differ from a fresh run")
    parser.add_argument("--verify", action="store_true",
                        help="report whether the client in --client-data reads a patched CreatureDisplayInfo.dbc, and exit")
    args = parser.parse_args()

    if args.verify:
        if not args.client_data:
            raise SystemExit("--verify needs --client-data: the Data directory of the client the game runs from")
        if not args.client_data.is_dir():
            raise SystemExit(f"{args.client_data} is not a directory; point --client-data at the client's Data folder")
        return dbc_mpq.verify_client(args.client_data, DISPLAY_DBC, carries_decks)

    if args.dbcs is None:
        parser.error("--dbcs is required unless --verify is given")
    if not args.dbcs.is_dir():
        raise SystemExit(f"{args.dbcs} is not a directory; it holds {MODEL_DBC} and {DISPLAY_DBC}")

    if args.survey:
        if args.data_dir and not args.data_dir.is_dir():
            raise SystemExit(f"{args.data_dir} is not a directory; --data-dir takes the client's Data folder")
        return survey(args.dbcs, args.survey, args.listfile, args.data_dir)

    overrides = {}
    for item in args.model:
        alias, _, path = item.partition("=")
        alias = alias.strip().lower()
        if alias not in DECK_IDS or not path.strip():
            parser.error(f"--model {item!r}: ALIAS must be one of {', '.join(sorted(DECK_IDS))} and PATH must not be empty")
        overrides[alias] = path.strip()

    patched_models, patched_displays, archive = build(args.dbcs, args.scale, args.mount_height, overrides)
    for alias, display_id in sorted(DECK_IDS.items(), key=lambda item: item[1]):
        print(f"{display_id} ({alias}): {resolve_model(alias, overrides.get(alias))}, display scale {args.scale}")
    print(f"{MODEL_DBC}: {len(patched_models)} bytes, {DISPLAY_DBC}: {len(patched_displays)} bytes")
    print(f"archive: {len(archive)} bytes")

    if args.check:
        stale = []
        for name, content in ((MODEL_DBC, patched_models), (DISPLAY_DBC, patched_displays),
                              (STAGED_ARCHIVE, archive)):
            path = args.out / name
            if not path.is_file() or path.read_bytes() != content:
                stale.append(str(path))
        if stale:
            raise SystemExit("out of date: " + ", ".join(stale))
        print(f"up to date: {args.out}")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / MODEL_DBC).write_bytes(patched_models)
    (args.out / DISPLAY_DBC).write_bytes(patched_displays)
    (args.out / STAGED_ARCHIVE).write_bytes(archive)
    print(f"wrote {args.out / MODEL_DBC}, {args.out / DISPLAY_DBC} and {args.out / STAGED_ARCHIVE}")

    if args.server_dbc:
        args.server_dbc.mkdir(parents=True, exist_ok=True)
        (args.server_dbc / MODEL_DBC).write_bytes(patched_models)
        (args.server_dbc / DISPLAY_DBC).write_bytes(patched_displays)
        print(f"server DBCs: {args.server_dbc / MODEL_DBC}, {args.server_dbc / DISPLAY_DBC}")

    if args.client_data:
        dbc_mpq.install_patch(args.client_data, archive, MEMBERS)
    else:
        print(f"place {STAGED_ARCHIVE} in the client's Data directory, named after every patch already "
              f"there, to finish the fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
