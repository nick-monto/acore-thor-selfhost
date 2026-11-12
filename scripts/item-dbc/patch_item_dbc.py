#!/usr/bin/env python3
"""Give the client the custom items: extend ``Item.dbc`` and ship it as a patch MPQ.

The records are the loot-bag items (from ``scripts/loot-bags/gen_loot_bags.py``,
mirrored here) and the rune items (from the SQL that
``scripts/runewords/gen_runewords.py`` writes, which is also what the database is
loaded from).

Why this is needed
------------------
A 3.3.5a client draws an item from its own ``Item.dbc`` record, not from the
``SMSG_ITEM_QUERY_SINGLE_RESPONSE`` the server sends.  The loot window is the one
place that does not: ``SMSG_LOOT_RESPONSE`` carries ``ItemTemplate::DisplayInfoID``
per slot (``LootMgr.cpp``), so a custom bag shows its icon there while the same
bag in the player's bags draws the red question mark, because entry 91000+ has no
``Item.dbc`` record.  The server notices the same gap and says so at debug level
(``ObjectMgr.cpp``: "does not exist in item.dbc! (not correct id?)"), then skips
the DBC attribute checks for that entry.

The fix is the standard one for custom items: add the entries to the client's
``Item.dbc`` and ship the file as a patch MPQ.  This script reads the copy of
``Item.dbc`` the installer already put in the server's data directory (it is
copied from the client), appends one record per bag, and writes both the patched
``Item.dbc`` (for the server, so ``sItemStore`` agrees and the attribute checks
run) and a patch archive (for the client's ``Data`` directory), under the highest
free patch name so it loads after every archive already there.

The records mirror the ``item_template`` rows the loot-bag generator writes, so
``enforceDBCAttributes`` has nothing to correct: class 15, subclass 0, material
0, inventory type 0, sheath 0, and the tier's borrowed bag ``displayid``.

Usage, on the realm host (Termux; `pkg install python` once):

    RF=$PREFIX/var/lib/proot-distro/installed-rootfs/ubuntu
    python3 scripts/item-dbc/patch_item_dbc.py --dbc "$RF/opt/acore/data/dbc/Item.dbc" \\
        --client-data "/sdcard/WOTLK 3.3.5a/Data" --server-dbc "$RF/opt/acore/data/dbc"

The DBC lives inside the proot rootfs, so it is addressed through the rootfs path
from Termux; the client's `Data` directory is on shared storage and reachable
from both.  Both copies are optional; without them the patched file and the
archive land in ``--out`` for the operator to place by hand.  ``--check``
re-derives everything in memory and fails if the files in ``--out`` differ, so
the outputs can be kept in step with the generator.  ``--verify --client-data``
answers the question a red question mark asks - which archive the client actually
reads ``Item.dbc`` from, and whether it carries the bags - without changing
anything.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "lib"))
import dbc_mpq  # noqa: E402  (the shared DBC container and MPQ writer)

GENERATOR = HERE.parent / "loot-bags" / "gen_loot_bags.py"

# Item.dbc is eight uint32 fields in this client: ID, ClassID, SubclassID,
# SoundOverrideSubclassID, Material, DisplayInfoID, InventoryType, SheatheType
# (DBCStructure.h, ItemEntry).
ITEM_DBC_FIELDS = 8

# The bag rows the SQL writes, mirrored here so the two cannot drift.
BAG_CLASS = 15            # ITEM_CLASS_MISC, like the lockboxes
BAG_SUBCLASS = 0
# -1, because that is what item_template's column defaults to and what the core compares this
# field against: a 0 here made every bag log "does not have a correct SoundOverrideSubclass (-1),
# must be 0" at startup, once per item. The client's own misc items carry -1 as well.
BAG_SOUND_OVERRIDE = -1
BAG_MATERIAL = 0          # item_template leaves this at 0
BAG_INVENTORY_TYPE = 0    # not equippable, as in item_template
BAG_SHEATHE = 0

# ---------------------------------------------------------------------------
# Bag records
# ---------------------------------------------------------------------------

def load_generator():
    spec = importlib.util.spec_from_file_location("gen_loot_bags", GENERATOR)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {GENERATOR}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bag_records(gen) -> list[tuple[int, ...]]:
    """One Item.dbc record per bag item, in the generator's id order."""
    records = []
    for bracket_index in range(1, len(gen.BRACKETS) + 1):
        for tier_index, tier in enumerate(gen.TIERS):
            entry = gen.BAG_ENTRY_BASE + (bracket_index - 1) * len(gen.TIERS) + tier_index
            records.append(
                (
                    entry,
                    BAG_CLASS,
                    BAG_SUBCLASS,
                    BAG_SOUND_OVERRIDE,
                    BAG_MATERIAL,
                    tier["displayid"],
                    BAG_INVENTORY_TYPE,
                    BAG_SHEATHE,
                )
            )
    return records


def bag_id_range(gen) -> range:
    count = len(gen.BRACKETS) * len(gen.TIERS)
    return range(gen.BAG_ENTRY_BASE, gen.BAG_ENTRY_BASE + count)


# ---------------------------------------------------------------------------
# Rune records
# ---------------------------------------------------------------------------
#
# Read from the SQL the runeword generator writes, not from the generator itself:
# the runes' display ids are borrowed from the base database's gems, which would
# mean needing a core checkout on the realm host. The committed SQL is the same
# data, is what the database is loaded from, and scripts/runewords/gen_runewords.py
# --check keeps it in step with the pinned dumps, so there is still one source.

RUNEWORD_SQL_NAME = "runewords_10_items.sql"

# The generator's file is not in the same place in every layout this script runs
# in: this repository keeps it under sql/, while the packaged server artifact and
# the container image carry the same tree under src/data/. The search is ordered
# and the failure names every path it tried, so a moved file is a one-line error
# rather than a silently smaller patch.
RUNEWORD_SQL_HINTS = (
    Path("sql") / "custom" / "db_world",
    Path("src") / "data" / "sql" / "custom" / "db_world",
    Path("data") / "sql" / "custom" / "db_world",
)


# Where the tree above this script can start. In this repository the script sits at
# scripts/item-dbc/, so the repository root is the grandparent; both installers fetch it to
# <state>/item-dbc/ next to loot-bags/ and the SQL, which makes the parent the root; the
# container image keeps it at /opt/acore/dbc-patch/item-dbc/ with the artifact's src/ beside
# again one level up. Searching the grandparent first keeps the repository case unambiguous,
# and the second root is what makes a fetched copy find its own sql/.
RUNEWORD_SQL_ROOTS = (lambda: (HERE.parents[1], HERE.parent))()


def default_runeword_sql() -> Path:
    tried = []
    for root in RUNEWORD_SQL_ROOTS:
        for hint in RUNEWORD_SQL_HINTS:
            candidate = root / hint / RUNEWORD_SQL_NAME
            tried.append(str(candidate))
            if candidate.is_file():
                return candidate
    raise SystemExit(
        f"cannot find {RUNEWORD_SQL_NAME}; tried:\n  " + "\n  ".join(tried) + "\n"
        "run scripts/runewords/gen_runewords.py first, or pass --runeword-sql"
    )

RUNE_ROW = re.compile(r"^\((\d+), (\d+), (\d+), '(?:[^'\\]|\\.)*', (\d+), (\d+),")


def rune_rows(path: Path | None = None) -> list[tuple[int, int, int, int]]:
    """(entry, class, subclass, displayid) for every rune the SQL writes."""
    path = path or default_runeword_sql()
    if not path.is_file():
        raise SystemExit(f"{path} is missing; run scripts/runewords/gen_runewords.py first")
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = RUNE_ROW.match(line)
        if match:
            entry, cls, subclass, display, _quality = (int(match.group(i)) for i in range(1, 6))
            rows.append((entry, cls, subclass, display))
    if not rows:
        raise SystemExit(f"{path} carries no rune rows")
    return rows


def rune_records(path: Path | None = None) -> list[tuple[int, ...]]:
    """One Item.dbc record per rune, in id order."""
    return [
        (entry, cls, subclass, BAG_SOUND_OVERRIDE, BAG_MATERIAL, display, BAG_INVENTORY_TYPE, BAG_SHEATHE)
        for entry, cls, subclass, display in rune_rows(path)
    ]


def rune_id_range(path: Path | None = None) -> range:
    ids = [entry for entry, _, _, _ in rune_rows(path)]
    if ids != list(range(ids[0], ids[0] + len(ids))):
        raise SystemExit(f"{path}: rune entries are not a contiguous range")
    return range(ids[0], ids[0] + len(ids))


# ---------------------------------------------------------------------------
# DBC
# ---------------------------------------------------------------------------

def read_dbc(path: Path) -> tuple[list[tuple[int, ...]], bytes]:
    """Return the records and the string block of an Item.dbc on disk."""
    return dbc_mpq.read_dbc(path, ITEM_DBC_FIELDS)


def parse_dbc(data: bytes, label: str) -> tuple[list[tuple[int, ...]], bytes]:
    """Return the records and the string block of an Item.dbc held in memory."""
    return dbc_mpq.parse_dbc(data, label, ITEM_DBC_FIELDS)


def write_dbc(records: list[tuple[int, ...]], strings: bytes) -> bytes:
    return dbc_mpq.write_dbc(records, strings, ITEM_DBC_FIELDS)


def extend_records(records: list[tuple[int, ...]], ours: list[tuple[int, ...]], ids: range) -> list[tuple[int, ...]]:
    """Drop any previous copy of our records, then append them in id order."""
    return dbc_mpq.extend_records(records, ours, ids, "the client's Item.dbc")


# ---------------------------------------------------------------------------
# MPQ
# ---------------------------------------------------------------------------

build_mpq = dbc_mpq.build_mpq
read_mpq_file = dbc_mpq.read_mpq_file
MPQ_HEADER_SIZE = dbc_mpq.MPQ_HEADER_SIZE

STAGED_ARCHIVE = "item-dbc.MPQ"
MEMBERS = ["DBFilesClient\\Item.dbc"]


def patch_archive(item_dbc: bytes) -> bytes:
    return build_mpq([
        ("DBFilesClient\\Item.dbc", item_dbc),
        ("(listfile)", b"DBFilesClient\\Item.dbc\r\n"),
    ])


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

patch_rank = dbc_mpq.patch_rank
client_archives = dbc_mpq.client_archives
first_free_patch_name = dbc_mpq.first_free_patch_name


def verify_client(data_dir: Path, ids: range) -> int:
    """Report which archive the client reads Item.dbc from, and whether it carries the bags."""
    def carries(data: bytes) -> bool:
        records, _ = parse_dbc(data, "an archive's Item.dbc")
        return set(ids) <= {record[0] for record in records}

    return dbc_mpq.verify_client(data_dir, "Item.dbc", carries)


def our_archives(data_dir: Path) -> list[Path]:
    return dbc_mpq.our_archives(data_dir, MEMBERS)


def patch_target(data_dir: Path) -> Path:
    return dbc_mpq.patch_target(data_dir, MEMBERS)


def build(dbc_path: Path, runeword_sql: Path | None = None) -> tuple[bytes, bytes, int]:
    gen = load_generator()
    records, strings = read_dbc(dbc_path)
    ours = bag_records(gen)
    ids = bag_id_range(gen)
    if len(ours) != len(ids):
        raise SystemExit(f"{len(ours)} bag records for a range of {len(ids)} ids")
    # The runes are a second, higher range. Both are dropped in one pass and then
    # appended in id order: dropping and appending range by range breaks the
    # file's order on a re-run, because the bags would be appended below runes
    # that are already there.
    rune_ids = rune_id_range(runeword_sql)
    rune_ours = rune_records(runeword_sql)
    kept = [record for record in records if record[0] not in ids and record[0] not in rune_ids]
    if kept and max(record[0] for record in kept) >= ids.start:
        raise SystemExit(
            f"the client's Item.dbc already has entries above {ids.start}; "
            f"appending would break the file's id order"
        )
    extended = kept + ours + rune_ours
    patched = write_dbc(extended, strings)
    archive = patch_archive(patched)
    if read_mpq_file(archive, "DBFilesClient\\Item.dbc") != patched:
        raise SystemExit("the archive does not read back as the patched Item.dbc")
    if read_mpq_file(archive, "DBFilesClient/Item.dbc") != patched:
        raise SystemExit("the archive does not find the file through a forward-slash name")
    return patched, archive, len(ours) + len(rune_ids)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dbc", type=Path, help="the client's Item.dbc, as copied into the server's data/dbc")
    parser.add_argument("--out", type=Path, default=HERE.parents[1] / "build" / "item-dbc", help="where to write Item.dbc and the patch archive")
    parser.add_argument("--client-data", type=Path, help="the client's Data directory; the patch archive is copied in under the first free name")
    parser.add_argument("--server-dbc", type=Path, help="the server's data/dbc directory; the patched Item.dbc is copied in")
    parser.add_argument("--runeword-sql", type=Path,
                        help="the generated runewords_10_items.sql (default: search the repository and artifact layouts)")
    parser.add_argument("--check", action="store_true", help="fail if the files in --out differ from a fresh run")
    parser.add_argument("--verify", action="store_true",
                        help="report whether the client in --client-data reads a patched Item.dbc, and exit")
    args = parser.parse_args()

    if args.verify:
        if not args.client_data:
            raise SystemExit("--verify needs --client-data: the Data directory of the client the game runs from")
        if not args.client_data.is_dir():
            raise SystemExit(f"{args.client_data} is not a directory; point --client-data at the client's Data folder")
        gen = load_generator()
        ids = set(bag_id_range(gen)) | set(rune_id_range(args.runeword_sql))
        return verify_client(args.client_data, ids)

    if args.dbc is None:
        parser.error("--dbc is required unless --verify is given")
    if not args.dbc.is_file():
        raise SystemExit(f"{args.dbc} does not exist; it is the client's Item.dbc")

    patched, archive, count = build(args.dbc, args.runeword_sql)
    print(f"{args.dbc}: {count} custom item records appended (loot bags and runes), {len(patched)} bytes")
    print(f"archive: {len(archive)} bytes")

    if args.check:
        stale = []
        for name, content in (("Item.dbc", patched), (STAGED_ARCHIVE, archive)):
            path = args.out / name
            if not path.is_file() or path.read_bytes() != content:
                stale.append(str(path))
        if stale:
            raise SystemExit("out of date: " + ", ".join(stale))
        print(f"up to date: {args.out}")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "Item.dbc").write_bytes(patched)
    (args.out / STAGED_ARCHIVE).write_bytes(archive)
    print(f"wrote {args.out / 'Item.dbc'} and {args.out / STAGED_ARCHIVE}")

    if args.server_dbc:
        args.server_dbc.mkdir(parents=True, exist_ok=True)
        (args.server_dbc / "Item.dbc").write_bytes(patched)
        print(f"server Item.dbc: {args.server_dbc / 'Item.dbc'}")

    if args.client_data:
        if not args.client_data.is_dir():
            raise SystemExit(f"{args.client_data} is not a directory; point --client-data at the client's Data folder")
        target = patch_target(args.client_data)
        replaced = target.exists()
        target.write_bytes(archive)
        print(f"client patch: {target}" + (" (replaced)" if replaced else ""))
        print("restart the client so it loads the new archive")

    if not args.client_data:
        print(f"place {STAGED_ARCHIVE} in the client's Data directory, named after every patch already "
              f"there, to finish the fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
