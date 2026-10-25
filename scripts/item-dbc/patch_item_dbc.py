#!/usr/bin/env python3
"""Give the client the loot-bag items: extend ``Item.dbc`` and ship it as a patch MPQ.

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
run) and ``patch-4.MPQ`` (for the client's ``Data`` directory).

The records mirror the ``item_template`` rows the loot-bag generator writes, so
``enforceDBCAttributes`` has nothing to correct: class 15, subclass 0, material
0, inventory type 0, sheath 0, and the tier's borrowed bag ``displayid``.

Usage, on the realm host (Termux; `pkg install python` once):

    RF=$PREFIX/var/lib/proot-distro/installed-rootfs/ubuntu
    python3 scripts/item-dbc/patch_item_dbc.py --dbc "$RF/opt/acore/data/dbc/Item.dbc" \\
        --client-data "/sdcard/AzerothCore 3.3.5a/Data" --server-dbc "$RF/opt/acore/data/dbc"

The DBC lives inside the proot rootfs, so it is addressed through the rootfs path
from Termux; the client's `Data` directory is on shared storage and reachable
from both.  Both copies are optional; without them the patched file and the
archive land in ``--out`` for the operator to place by hand.  ``--check``
re-derives everything in memory and fails if the files in ``--out`` differ, so
the outputs can be kept in step with the generator.
"""

from __future__ import annotations

import argparse
import importlib.util
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GENERATOR = HERE.parent / "loot-bags" / "gen_loot_bags.py"

# Item.dbc is eight uint32 fields in this client: ID, ClassID, SubclassID,
# SoundOverrideSubclassID, Material, DisplayInfoID, InventoryType, SheatheType
# (DBCStructure.h, ItemEntry).
ITEM_DBC_FIELDS = 8
ITEM_DBC_RECORD_SIZE = ITEM_DBC_FIELDS * 4
ITEM_DBC_MAGIC = b"WDBC"

# The bag rows the SQL writes, mirrored here so the two cannot drift.
BAG_CLASS = 15            # ITEM_CLASS_MISC, like the lockboxes
BAG_SUBCLASS = 0
BAG_SOUND_OVERRIDE = 0    # item_template leaves this at 0
BAG_MATERIAL = 0          # item_template leaves this at 0
BAG_INVENTORY_TYPE = 0    # not equippable, as in item_template
BAG_SHEATHE = 0

# Patch archives are loaded after the stock patch.MPQ, patch-2 and patch-3, and
# the last one that carries a file wins.  Any free name works; the first free one
# from this list is used so an existing custom patch is never overwritten.
PATCH_NAMES = [f"patch-{n}.MPQ" for n in range(4, 10)] + [f"patch-{c}.MPQ" for c in "ABCDEFGHIJ"]

MPQ_MAGIC = b"MPQ\x1a"
MPQ_FILE_EXISTS = 0x80000000
SECTOR_SIZE_SHIFT = 3                        # the client's sector size is 512 << shift
SECTOR_SIZE = 512 << SECTOR_SIZE_SHIFT       # 4096 bytes
MPQ_HASH_ENTRIES = 4
MPQ_HEADER_SIZE = 32
MPQ_EMPTY_HASH = struct.pack("<IIHHI", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)


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
# DBC
# ---------------------------------------------------------------------------

def read_dbc(path: Path) -> tuple[list[tuple[int, ...]], bytes]:
    """Return the records and the string block of an Item.dbc."""
    data = path.read_bytes()
    if len(data) < 20:
        raise SystemExit(f"{path}: too short to be a DBC")
    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", data)
    if magic != ITEM_DBC_MAGIC:
        raise SystemExit(f"{path}: not a DBC (magic {magic!r})")
    if fields != ITEM_DBC_FIELDS or record_size != ITEM_DBC_RECORD_SIZE:
        raise SystemExit(
            f"{path}: {fields} fields of {record_size} bytes is not the Item.dbc layout "
            f"({ITEM_DBC_FIELDS} fields of {ITEM_DBC_RECORD_SIZE})"
        )
    expected = 20 + count * record_size + string_size
    if len(data) != expected:
        raise SystemExit(f"{path}: {len(data)} bytes, header implies {expected}")
    records = [
        struct.unpack_from(f"<{ITEM_DBC_FIELDS}I", data, 20 + i * record_size)
        for i in range(count)
    ]
    strings = data[20 + count * record_size:]
    return records, strings


def write_dbc(records: list[tuple[int, ...]], strings: bytes) -> bytes:
    out = bytearray(struct.pack("<4sIIII", ITEM_DBC_MAGIC, len(records), ITEM_DBC_FIELDS, ITEM_DBC_RECORD_SIZE, len(strings)))
    for record in records:
        out += struct.pack(f"<{ITEM_DBC_FIELDS}I", *record)
    out += strings
    return bytes(out)


def extend_records(records: list[tuple[int, ...]], ours: list[tuple[int, ...]], ids: range) -> list[tuple[int, ...]]:
    """Drop any previous copy of our records, then append them in id order."""
    kept = [record for record in records if record[0] not in ids]
    if kept and max(record[0] for record in kept) >= ids.start:
        raise SystemExit(
            f"the client's Item.dbc already has entries above {ids.start}; "
            "appending would break the file's id order"
        )
    return kept + ours


# ---------------------------------------------------------------------------
# MPQ
# ---------------------------------------------------------------------------

def _crypt_table() -> list[int]:
    table = [0] * 0x500
    seed = 0x00100001
    for index1 in range(0x100):
        index2 = index1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp1 = (seed & 0xFFFF) << 0x10
            seed = (seed * 125 + 3) % 0x2AAAAB
            temp2 = seed & 0xFFFF
            table[index2] = (temp1 | temp2) & 0xFFFFFFFF
            index2 += 0x100
    return table


CRYPT_TABLE = _crypt_table()


def mpq_hash(name: str, kind: int) -> int:
    """The three name hashes MPQ uses: kind 0 table offset, 1 name A, 2 name B."""
    seed1 = 0x7FED7FED
    seed2 = 0xEEEEEEEE
    for ch in name.upper().replace("/", "\\").encode("ascii"):
        seed1 = (CRYPT_TABLE[(kind << 8) + ch] ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed2 = (ch + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def _mpq_crypt(data: bytes, key: int, decrypt: bool) -> bytes:
    """The cipher the hash and block tables are stored under.

    Both directions feed the key schedule from the *plaintext*: the input word when
    encrypting, the output word when decrypting.  That is what makes the two
    directions inverses of each other.
    """
    seed1 = key
    seed2 = 0xEEEEEEEE
    out = bytearray()
    for (word,) in struct.iter_unpack("<I", data):
        seed2 = (seed2 + CRYPT_TABLE[0x400 + (seed1 & 0xFF)]) & 0xFFFFFFFF
        result = (word ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed1 = ((((~seed1) & 0xFFFFFFFF) << 0x15) + 0x11111111 | (seed1 >> 0x0B)) & 0xFFFFFFFF
        plain = result if decrypt else word
        seed2 = (plain + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
        out += struct.pack("<I", result)
    return bytes(out)


def mpq_encrypt(data: bytes, key: int) -> bytes:
    return _mpq_crypt(data, key, decrypt=False)


def mpq_decrypt(data: bytes, key: int) -> bytes:
    return _mpq_crypt(data, key, decrypt=True)


HASH_TABLE_KEY = mpq_hash("(hash table)", 3)
BLOCK_TABLE_KEY = mpq_hash("(block table)", 3)


def _sector_block(data: bytes) -> bytes:
    """A stored file's sector offset table followed by its bytes.

    The offsets are absolute from the start of the block, so the first sector
    begins after the table itself.  Stored rather than compressed: the client
    handles uncompressed blocks in any MPQ, and a patch this small gains nothing
    from zlib.
    """
    count = (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    table_size = (count + 1) * 4
    offsets = [table_size + index * SECTOR_SIZE for index in range(count)] + [table_size + len(data)]
    return struct.pack(f"<{count + 1}I", *offsets) + data


def build_mpq(files: list[tuple[str, bytes]]) -> bytes:
    """A version-1 archive holding stored, sectored files: enough for a patch."""
    if not files:
        raise SystemExit("an archive needs at least one file")
    hash_size = MPQ_HASH_ENTRIES
    while hash_size < len(files) * 2:
        hash_size <<= 1

    blocks = []
    body = bytearray()
    for _, data in files:
        block = _sector_block(data)
        blocks.append(struct.pack("<IIII", MPQ_HEADER_SIZE + len(body), len(block), len(data), MPQ_FILE_EXISTS))
        body += block

    hash_table = [MPQ_EMPTY_HASH] * hash_size
    occupied = [False] * hash_size
    for block_index, (name, _) in enumerate(files):
        index = mpq_hash(name, 0) & (hash_size - 1)
        step = 1 + (mpq_hash(name, 1) & (hash_size - 1))
        while occupied[index]:
            index = (index + step) & (hash_size - 1)
        occupied[index] = True
        hash_table[index] = struct.pack(
            "<IIHHI", mpq_hash(name, 1), mpq_hash(name, 2), 0, 0, block_index
        )

    hash_pos = MPQ_HEADER_SIZE + len(body)
    block_pos = hash_pos + hash_size * 16
    archive_size = block_pos + len(blocks) * 16
    header = struct.pack(
        "<4sIIHHIIII", MPQ_MAGIC, MPQ_HEADER_SIZE, archive_size, 0, SECTOR_SIZE_SHIFT, hash_pos, block_pos, hash_size, len(blocks)
    )
    # The client always stores both tables encrypted with the key of their own
    # name; a plain table reads back as garbage.
    return (
        header
        + bytes(body)
        + mpq_encrypt(b"".join(hash_table), HASH_TABLE_KEY)
        + mpq_encrypt(b"".join(blocks), BLOCK_TABLE_KEY)
    )


def read_mpq_file(archive: bytes, name: str) -> bytes:
    """Look one file up the way the client does, to prove the archive is readable."""
    magic, header_size, archive_size, _version, _shift, hash_pos, block_pos, hash_size, block_count = struct.unpack_from(
        "<4sIIHHIIII", archive
    )
    if magic != MPQ_MAGIC or header_size != MPQ_HEADER_SIZE:
        raise SystemExit("archive header is malformed")
    if archive_size != len(archive):
        raise SystemExit(f"archive says {archive_size} bytes, file has {len(archive)}")
    wanted_a, wanted_b = mpq_hash(name, 1), mpq_hash(name, 2)
    hash_table = mpq_decrypt(archive[hash_pos:hash_pos + hash_size * 16], HASH_TABLE_KEY)
    block_table = mpq_decrypt(archive[block_pos:block_pos + block_count * 16], BLOCK_TABLE_KEY)
    index = mpq_hash(name, 0) & (hash_size - 1)
    step = 1 + (mpq_hash(name, 1) & (hash_size - 1))
    for _ in range(hash_size):
        entry_a, entry_b, _locale, _platform, block_index = struct.unpack_from("<IIHHI", hash_table, index * 16)
        if entry_a == 0xFFFFFFFF and entry_b == 0xFFFFFFFF:
            break
        if entry_a == wanted_a and entry_b == wanted_b:
            if block_index >= block_count:
                raise SystemExit(f"{name}: block index {block_index} out of range")
            pos, archived, size, flags = struct.unpack_from("<IIII", block_table, block_index * 16)
            if not flags & MPQ_FILE_EXISTS:
                raise SystemExit(f"{name}: block is not present")
            if archived != size + (size + SECTOR_SIZE - 1) // SECTOR_SIZE * 4 + 4:
                raise SystemExit(f"{name}: block size does not match a stored sector table")
            block = archive[pos:pos + archived]
            count = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
            offsets = struct.unpack_from(f"<{count + 1}I", block)
            data = b"".join(block[offsets[i]:offsets[i + 1]] for i in range(count))
            if len(data) != size:
                raise SystemExit(f"{name}: sectors hold {len(data)} bytes, block says {size}")
            return data
        index = (index + step) & (hash_size - 1)
    raise SystemExit(f"{name}: not in the archive")


def patch_archive(item_dbc: bytes) -> bytes:
    return build_mpq([
        ("DBFilesClient\\Item.dbc", item_dbc),
        ("(listfile)", b"DBFilesClient\\Item.dbc\r\n"),
    ])


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def first_free_patch(data_dir: Path) -> Path:
    for name in PATCH_NAMES:
        candidate = data_dir / name
        if not candidate.exists():
            return candidate
    raise SystemExit(f"{data_dir}: every patch name from {PATCH_NAMES[0]} to {PATCH_NAMES[-1]} is taken")


def build(dbc_path: Path) -> tuple[bytes, bytes, int]:
    gen = load_generator()
    records, strings = read_dbc(dbc_path)
    ours = bag_records(gen)
    ids = bag_id_range(gen)
    if len(ours) != len(ids):
        raise SystemExit(f"{len(ours)} bag records for a range of {len(ids)} ids")
    extended = extend_records(records, ours, ids)
    patched = write_dbc(extended, strings)
    archive = patch_archive(patched)
    if read_mpq_file(archive, "DBFilesClient\\Item.dbc") != patched:
        raise SystemExit("the archive does not read back as the patched Item.dbc")
    if read_mpq_file(archive, "DBFilesClient/Item.dbc") != patched:
        raise SystemExit("the archive does not find the file through a forward-slash name")
    return patched, archive, len(ours)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dbc", required=True, type=Path, help="the client's Item.dbc, as copied into the server's data/dbc")
    parser.add_argument("--out", type=Path, default=HERE.parents[1] / "build" / "item-dbc", help="where to write Item.dbc and the patch archive")
    parser.add_argument("--client-data", type=Path, help="the client's Data directory; the patch archive is copied in under the first free name")
    parser.add_argument("--server-dbc", type=Path, help="the server's data/dbc directory; the patched Item.dbc is copied in")
    parser.add_argument("--check", action="store_true", help="fail if the files in --out differ from a fresh run")
    args = parser.parse_args()

    if not args.dbc.is_file():
        raise SystemExit(f"{args.dbc} does not exist; it is the client's Item.dbc")

    patched, archive, count = build(args.dbc)
    print(f"{args.dbc}: {count} bag records appended, {len(patched)} bytes")
    print(f"archive: {len(archive)} bytes")

    if args.check:
        stale = []
        for name, content in (("Item.dbc", patched), ("patch-4.MPQ", archive)):
            path = args.out / name
            if not path.is_file() or path.read_bytes() != content:
                stale.append(str(path))
        if stale:
            raise SystemExit("out of date: " + ", ".join(stale))
        print(f"up to date: {args.out}")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "Item.dbc").write_bytes(patched)
    (args.out / "patch-4.MPQ").write_bytes(archive)
    print(f"wrote {args.out / 'Item.dbc'} and {args.out / 'patch-4.MPQ'}")

    if args.server_dbc:
        args.server_dbc.mkdir(parents=True, exist_ok=True)
        (args.server_dbc / "Item.dbc").write_bytes(patched)
        print(f"server Item.dbc: {args.server_dbc / 'Item.dbc'}")

    if args.client_data:
        if not args.client_data.is_dir():
            raise SystemExit(f"{args.client_data} is not a directory; point --client-data at the client's Data folder")
        target = first_free_patch(args.client_data)
        target.write_bytes(archive)
        print(f"client patch: {target}")
        print("restart the client so it loads the new archive")

    if not args.client_data:
        print("place patch-4.MPQ in the client's Data directory to finish the fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
