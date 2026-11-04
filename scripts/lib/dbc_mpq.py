#!/usr/bin/env python3
"""The client-data half of a custom id: read and write a 3.3.5a DBC, and ship it as a patch MPQ.

Two tools need the same thing and differ only in which records they add: the loot bags add
``Item.dbc`` rows, the skateboard's deck adds ``CreatureModelData``/``CreatureDisplayInfo`` rows.
The DBC container layout, the MPQ writer and the rule about which archive the client actually
loads are not specific to either, so they live here once.

The archive is written the way a patch has to be: version-1, stored single-unit blocks with no
sector table, both tables encrypted with the key of their own name. Everything about the load
order - ``patch-4`` to ``patch-9``, the last archive carrying a file wins - is in
:func:`patch_rank` and :func:`install_patch`.
"""

from __future__ import annotations

import struct
from pathlib import Path

DBC_MAGIC = b"WDBC"
MPQ_MAGIC = b"MPQ\x1a"
MPQ_FILE_EXISTS = 0x80000000
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_SINGLE_UNIT = 0x01000000
SECTOR_SIZE_SHIFT = 3                        # the client's sector size is 512 << shift
SECTOR_SIZE = 512 << SECTOR_SIZE_SHIFT       # 4096 bytes
MPQ_HASH_ENTRIES = 4
MPQ_HEADER_SIZE = 32
MPQ_EMPTY_HASH = struct.pack("<IIHHI", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)

# Patch archives are loaded after the stock patch.MPQ, patch-2 and patch-3, in
# name order, and the last archive that carries a file wins.  So the archive has
# to be named *after* every patch already in the directory: a patch-4 that sits
# under a launcher's patch-5 loses its rows without a word, which looks
# exactly like a patch that was never applied.  The highest free name is used.
# Only the numeric names are used: 4-9 is six archives, and the letter names are
# not loaded by every 3.3.5a build, so writing one would be a silent no-op on the
# clients that ignore it.
PATCH_NAMES = [f"patch-{n}.MPQ" for n in range(9, 3, -1)]


# ---------------------------------------------------------------------------
# DBC
# ---------------------------------------------------------------------------

def parse_dbc(data: bytes, label: str, fields: int) -> tuple[list[tuple[int, ...]], bytes]:
    """Return the records and the string block of a DBC held in memory.

    ``fields`` is the count the caller expects: a file with a different width is a different
    DBC (or another client version), and appending to it would shift every later record.
    """
    record_size = fields * 4
    if len(data) < 20:
        raise SystemExit(f"{label}: too short to be a DBC")
    magic, count, found_fields, found_size, string_size = struct.unpack_from("<4sIIII", data)
    if magic != DBC_MAGIC:
        raise SystemExit(f"{label}: not a DBC (magic {magic!r})")
    if found_fields != fields or found_size != record_size:
        raise SystemExit(
            f"{label}: {found_fields} fields of {found_size} bytes is not the expected layout "
            f"({fields} fields of {record_size})"
        )
    expected = 20 + count * record_size + string_size
    if len(data) != expected:
        raise SystemExit(f"{label}: {len(data)} bytes, header implies {expected}")
    records = [
        struct.unpack_from(f"<{fields}I", data, 20 + i * record_size)
        for i in range(count)
    ]
    strings = data[20 + count * record_size:]
    return records, strings


def read_dbc(path: Path, fields: int) -> tuple[list[tuple[int, ...]], bytes]:
    """Return the records and the string block of a DBC on disk."""
    return parse_dbc(path.read_bytes(), str(path), fields)


def write_dbc(records: list[tuple[int, ...]], strings: bytes, fields: int) -> bytes:
    out = bytearray(struct.pack(
        "<4sIIII", DBC_MAGIC, len(records), fields, fields * 4, len(strings)
    ))
    for record in records:
        # -1 is written as 0xFFFFFFFF: every field is four bytes, and the signed ones are read
        # back from the same bits.
        out += struct.pack(f"<{fields}I", *[value & 0xFFFFFFFF for value in record])
    out += strings
    return bytes(out)


def float_bits(value: float) -> int:
    """The four bytes of a DBC float field, as the uint32 the record stores."""
    return struct.unpack("<I", struct.pack("<f", value))[0]


def bits_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def add_string(strings: bytes, text: str) -> tuple[int, bytes]:
    """The offset of ``text`` in the string block, appending it when it is not there yet."""
    encoded = text.encode("ascii")
    if not encoded.endswith(b"\0"):
        encoded += b"\0"
    index = strings.find(encoded)
    # A match inside another string ("...\\Shield_A.mdxX") is not the same string.
    if index > 0 and strings[index - 1:index] == b"\0":
        return index, strings
    offset = len(strings)
    return offset, strings + encoded


def string_at(strings: bytes, offset: int) -> str:
    if offset == 0 or offset >= len(strings):
        return ""
    return strings[offset:strings.find(b"\0", offset)].decode("ascii", "replace")


def extend_records(
    records: list[tuple[int, ...]],
    ours: list[tuple[int, ...]],
    ids: range,
    label: str = "the file",
) -> list[tuple[int, ...]]:
    """Drop any previous copy of our records, then append them in id order."""
    kept = [record for record in records if record[0] not in ids]
    if kept and max(record[0] for record in kept) >= ids.start:
        raise SystemExit(
            f"{label} already has entries above {ids.start}; appending would break the file's id order"
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


def _file_block(data: bytes) -> bytes:
    r"""A stored file's bytes, exactly as they are.

    A stored file carries **no sector offset table**: the client only reads one
    when the block is compressed or carries sector CRCs, and treats an
    uncompressed block as one contiguous unit.  Writing the table anyway
    makes the client read the table as file content, so the first bytes of
    Item.dbc were `a0 06 00 00` instead of `WDBC` and 3.3.5a stopped at
    "ERROR #121 Version Mismatch: unable to open DBFilesClient\Item.dbc".
    MPQ_FILE_SINGLE_UNIT says so explicitly.
    """
    return data


def build_mpq(files: list[tuple[str, bytes]]) -> bytes:
    """A version-1 archive holding stored single-unit files: enough for a patch."""
    if not files:
        raise SystemExit("an archive needs at least one file")
    hash_size = MPQ_HASH_ENTRIES
    while hash_size < len(files) * 2:
        hash_size <<= 1

    blocks = []
    body = bytearray()
    for _, data in files:
        block = _file_block(data)
        blocks.append(struct.pack("<IIII", MPQ_HEADER_SIZE + len(body), len(block), len(data),
                                  MPQ_FILE_EXISTS | MPQ_FILE_SINGLE_UNIT))
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
        "<4sIIHHIIII", MPQ_MAGIC, MPQ_HEADER_SIZE, archive_size, 0, SECTOR_SIZE_SHIFT,
        hash_pos, block_pos, hash_size, len(blocks)
    )
    # The client always stores both tables encrypted with the key of their own
    # name; a plain table reads back as garbage.
    return (
        header
        + bytes(body)
        + mpq_encrypt(b"".join(hash_table), HASH_TABLE_KEY)
        + mpq_encrypt(b"".join(blocks), BLOCK_TABLE_KEY)
    )


def assert_readable(archive: bytes, name: str, expected: bytes) -> None:
    """Prove the archive answers the name the client asks for, both separators included."""
    for wanted in (name, name.replace("\\", "/")):
        if read_mpq_file(archive, wanted) != expected:
            raise SystemExit(f"the archive does not read back as {wanted}")


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
            block = archive[pos:pos + archived]
            if flags & MPQ_FILE_COMPRESS:
                raise SystemExit(f"{name}: compressed blocks are not written by this tool")
            if flags & MPQ_FILE_SINGLE_UNIT or archived == size:
                return block[:size]
            # An older run wrote a stored file with a sector offset table in front of it, which the
            # client reads as content. Recognise that layout so those archives can still be replaced.
            count = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
            offsets = struct.unpack_from(f"<{count + 1}I", block)
            if offsets[0] != (count + 1) * 4 or offsets[-1] != archived:
                raise SystemExit(f"{name}: block is neither single-unit nor a sector table")
            data = b"".join(block[offsets[i]:offsets[i + 1]] for i in range(count))
            if len(data) != size:
                raise SystemExit(f"{name}: sectors hold {len(data)} bytes, block says {size}")
            return data
        index = (index + step) & (hash_size - 1)
    raise SystemExit(f"{name}: not in the archive")


# ---------------------------------------------------------------------------
# Reading the *client's* archives
# ---------------------------------------------------------------------------

# What a block's leading compression byte says, for the ones that can be undone with the standard
# library. The client's own archives are written by Blizzard, not by this tool, so unlike the
# writer's stored-only blocks these are compressed - and the (listfile) member is the one worth
# reading, because it is the only place a doodad model's path is written down.
MPQ_COMPRESSION_MASK = 0x0000FF00
COMPRESSION_ZLIB_NAMES = {
    0x02: "zlib",
    0x10: "bzip2",
    0x08: "PKWARE implode",
    0x01: "huffman",
    0x12: "lzma",
}


def _decompress(block: bytes, label: str) -> bytes:
    """Undo one compressed block, or say precisely which codec stopped us."""
    if not block:
        return block
    kind = block[0]
    body = block[1:]
    if kind == 0x02:
        import zlib
        return zlib.decompress(body)
    name = COMPRESSION_ZLIB_NAMES.get(kind, f"codec 0x{kind:02x}")
    raise SystemExit(f"{label}: the block is compressed with {name}, which this reader does not undo")


def _archive_entries(index, label: str):
    """The hash and block tables of an open archive, as lists."""
    magic, header_size, archive_size, _version, _shift, hash_pos, block_pos, hash_size, block_count = \
        struct.unpack_from("<4sIIHHIIII", index)
    if magic != MPQ_MAGIC or header_size != MPQ_HEADER_SIZE:
        raise SystemExit(f"{label}: archive header is malformed")
    if archive_size > len(index):
        raise SystemExit(f"{label}: archive says {archive_size} bytes, file has {len(index)}")
    hash_table = mpq_decrypt(index[hash_pos:hash_pos + hash_size * 16], HASH_TABLE_KEY)
    block_table = mpq_decrypt(index[block_pos:block_pos + block_count * 16], BLOCK_TABLE_KEY)
    return hash_table, block_table, hash_size, block_count


def read_mpq_path(path: Path, name: str) -> bytes:
    """One member of an archive on disk, decompressing what the reader knows how to undo.

    Unlike read_mpq_file this mmaps the archive: a 3.3.5a Data directory holds one archive over a
    gigabyte, and reading all of it to pull one small file out is not worth the memory.
    """
    import mmap

    with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as index:
        hash_table, block_table, hash_size, block_count = _archive_entries(index, str(path))
        wanted_a, wanted_b = mpq_hash(name, 1), mpq_hash(name, 2)
        slot = mpq_hash(name, 0) & (hash_size - 1)
        step = 1 + (mpq_hash(name, 1) & (hash_size - 1))
        for _ in range(hash_size):
            entry_a, entry_b, _locale, _platform, block_index = struct.unpack_from("<IIHHI", hash_table, slot * 16)
            if entry_a == 0xFFFFFFFF and entry_b == 0xFFFFFFFF:
                break
            if entry_a == wanted_a and entry_b == wanted_b:
                if block_index >= block_count:
                    raise SystemExit(f"{path.name}: {name}: block index {block_index} out of range")
                pos, archived, size, flags = struct.unpack_from("<IIII", block_table, block_index * 16)
                if not flags & MPQ_FILE_EXISTS:
                    raise SystemExit(f"{path.name}: {name} is not present")
                raw = index[pos:pos + archived]
                # Encrypted members continue their key across sectors with FIX_KEY adjustments, and
                # getting that subtly wrong returns plausible garbage instead of an error - so this
                # reader refuses them instead of guessing. --listfile covers that case.
                if flags & 0x00010000:
                    raise SystemExit(f"{path.name}: {name} is encrypted; pass the same names "
                                     f"in a listfile.txt instead (--listfile)")
                if not flags & MPQ_FILE_COMPRESS:
                    return raw[:size]
                # Compressed blocks come in sectors, each with its own codec byte, behind a
                # sector offset table. A single-unit block is one sector with no table.
                if flags & MPQ_FILE_SINGLE_UNIT:
                    return _decompress(raw, f"{path.name}: {name}")[:size]
                count = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
                offsets = struct.unpack_from(f"<{count + 1}I", raw)
                if offsets[0] != (count + 1) * 4:
                    raise SystemExit(f"{path.name}: {name}: malformed sector table")
                return b"".join(_decompress(raw[offsets[i]:offsets[i + 1]], f"{path.name}: {name}")
                                for i in range(count))[:size]
            slot = (slot + step) & (hash_size - 1)
    raise SystemExit(f"{path.name}: {name} is not in the archive")


def list_mpq_files(path: Path) -> list[str]:
    """Every member name an archive's (listfile) holds, or SystemExit saying why it could not be read."""
    try:
        text = read_mpq_path(path, "(listfile)")
    except SystemExit as error:
        raise SystemExit(str(error)) from None
    return [line.strip() for line in text.decode("utf-8", "replace").splitlines() if line.strip()]


# ---------------------------------------------------------------------------
# Installing into a client's Data directory
# ---------------------------------------------------------------------------

def patch_rank(name: str) -> tuple[int, int] | None:
    """Where a Data-directory file sits in the client's archive order, or None when it is not a patch.

    ``patch.MPQ`` and ``patch-2``/``patch-3`` are the stock archives and ``patch-4`` to ``patch-9``
    follow them. Everything else in Data - ``common.MPQ``, ``lichking.MPQ``, the locale archives and
    the letter patches, which not every build loads - is ignored: it cannot shadow a numeric patch
    on the clients that do load it, and counting it could name a file the client never reads.
    """
    stem = Path(name).stem.lower()
    if stem == "patch":
        return (1, 0)
    if stem.startswith("patch-") and len(stem) == 7 and stem[6].isdigit():
        return (int(stem[6]), 0)
    return None


def client_archives(data_dir: Path) -> list[tuple[tuple[int, int], Path]]:
    """The Data directory's patch archives in load order, last first."""
    found = []
    for path in data_dir.iterdir():
        if not path.is_file() or path.suffix.lower() != ".mpq":
            continue
        rank = patch_rank(path.name)
        if rank is not None:
            found.append((rank, path))
    return sorted(found, reverse=True)


def our_archives(data_dir: Path, members: list[str]) -> list[Path]:
    """The archives an earlier run of a tool wrote, in load order.

    Re-running has to overwrite its own archive. Picking the highest free name every time would
    leave patch-7, patch-8 and patch-9 behind - a megabyte of the same DBC each, and only the
    last one read - so the newest of ours is reused and the rest are removed. An archive only
    counts as ours when it holds exactly the files the tool writes (plus the listfile):
    overwriting a launcher's patch would take that patch's own files with it.
    """
    found = []
    for _, path in client_archives(data_dir):
        try:
            archive = path.read_bytes()
            _, header_size, _, _, _, _, _, _, block_count = struct.unpack_from("<4sIIHHIIII", archive)
            if header_size != MPQ_HEADER_SIZE or block_count != len(members) + 1:
                continue
            for name in list(members) + ["(listfile)"]:
                read_mpq_file(archive, name)
        except (SystemExit, struct.error):
            continue
        found.append(path)
    return found


def first_free_patch_name(data_dir: Path) -> str | None:
    """The highest patch name the directory does not use, or None when all six are taken."""
    for name in PATCH_NAMES:
        if not (data_dir / name).exists():
            return name
    return None


def patch_target(data_dir: Path, members: list[str]) -> Path:
    """Where this run's archive goes, and the copies of it that are now dead.

    Two rules, in this order. Ours that are not the last one in load order are dead the moment a
    later one exists, so they are removed rather than left as a megabyte of shadowed records. Then
    the archive goes to the highest free name when that name loads after the copy we already have,
    and over the copy itself otherwise - so a re-run either replaces its own archive or moves it up
    the load order, and never piles up a new one per run.
    """
    ours = our_archives(data_dir, members)
    for stale in ours[1:]:
        stale.unlink()
        print(f"removed {stale.name}: an older copy of this archive, shadowed by {ours[0].name}")
    keep = ours[0] if ours else None
    free = first_free_patch_name(data_dir)
    if keep is not None and (free is None or patch_rank(free) <= patch_rank(keep.name)):
        return keep
    if free is None:
        raise SystemExit(
            f"{data_dir}: every patch name from {PATCH_NAMES[-1]} to {PATCH_NAMES[0]} is taken, and none "
            f"of them is this tool's; merge or delete one of them, then run this again"
        )
    if keep is not None:
        keep.unlink()
        print(f"removed {keep.name}: {free} loads after it, so that is the copy to keep")
    return data_dir / free


def install_patch(data_dir: Path, archive: bytes, members: list[str]) -> Path:
    """Write the archive into the client's Data directory and say what happened."""
    if not data_dir.is_dir():
        raise SystemExit(f"{data_dir} is not a directory; point --client-data at the client's Data folder")
    target = patch_target(data_dir, members)
    replaced = target.exists()
    target.write_bytes(archive)
    print(f"client patch: {target}" + (" (replaced)" if replaced else ""))
    print("restart the client so it loads the new archive")
    return target


def verify_client(data_dir: Path, member: str, present: "object") -> int:
    """Name the archive the client actually reads ``member`` from, and whether it carries it.

    The client reads a file from the last archive in load order that carries it, so a patched
    archive that sits *under* another patch's copy is the same as no patch at all. ``present``
    is a callable that takes the member's bytes and says whether the rows are in it.
    """
    archives = client_archives(data_dir)
    if not archives:
        print(f"{data_dir}: no patch-*.MPQ - the client reads the stock {member}")
        return 1

    winner: tuple[Path, bool] | None = None
    for _, path in archives:
        try:
            data = read_mpq_file(path.read_bytes(), f"DBFilesClient\\{member}")
        except (SystemExit, struct.error):
            # A stock archive is compressed or encrypted past what this reader handles, and it is
            # not the one being asked about.
            continue
        has_rows = bool(present(data))
        print(f"  {path.name}: carries {member}, our rows {'present' if has_rows else 'MISSING'}")
        if winner is None:
            winner = (path, has_rows)

    if winner is None:
        print(f"{data_dir}: no patch-*.MPQ carries {member}")
        return 1

    path, has_rows = winner
    if has_rows:
        print(f"{path}: the client reads {member} from here and it has our rows")
        return 0

    print(f"{path}: the client reads {member} from here and it has no rows of ours.")
    print("Either this Data directory is not the one the game runs from, or the patch landed under a")
    print("later archive - name it after every patch already here (patch-9 is the last).")
    return 1
