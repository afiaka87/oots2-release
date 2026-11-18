"""Raise the Ocarina of Time remake's resource size budgets for files a RomFS mod made bigger.

The game reserves a load buffer per file from System/Resource/ResourceSizeTable.Product.*.rsizetable.zs; the
remake's budget for most files is the decompressed size plus about 270 bytes. A replacement that decompresses
larger than its budget may fail to load. This reads every *.zs file under a mod's romfs/, decompresses it with
the game's own dictionaries (Pack/ZsDic.pack.zs), and writes copies of both size tables with each over-budget
file's entry raised to its aligned size + 0x1000 (the Entrance Randomizer's rule). Entries are never lowered,
nothing else in the tables changes, and the written tables are plain zstd, as the game's are.

  uv run --no-project --with zstandard python switch2/tools/restbl_raise.py --game-romfs EXTRACT/romfs \
      --mod-romfs MOD/romfs --out OUT

OUT receives romfs/System/Resource/<both tables>. Files whose names have no entry are reported and skipped.
The Entrance Randomizer ships its own copies of these tables, so only one of the two add-ons can be enabled.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import zstandard

TABLE_GLOB = "ResourceSizeTable.Product.*.rsizetable.zs"
HEADER = 22  # magic "RESTBL", version, string size, crc count, name count; the sorted crc table follows


def sarc_files(blob: bytes) -> dict[str, bytes]:
    if blob[:4] != b"SARC":
        raise SystemExit("ZsDic pack is not a SARC")
    data_start = struct.unpack_from("<I", blob, 0xC)[0]
    count = struct.unpack_from("<H", blob, 0x14 + 6)[0]
    names_start = 0x14 + 12 + 16 * count + 8
    files = {}
    for i in range(count):
        _, attributes, start, end = struct.unpack_from("<IIII", blob, 0x14 + 12 + 16 * i)
        name = blob[names_start + (attributes & 0xFFFF) * 4:].split(b"\0", 1)[0].decode()
        files[name] = blob[data_start + start:data_start + end]
    return files


def dictionaries(game_romfs: Path) -> dict[int, zstandard.ZstdCompressionDict]:
    pack = zstandard.ZstdDecompressor().decompress((game_romfs / "Pack" / "ZsDic.pack.zs").read_bytes(),
                                                   max_output_size=1 << 26)
    result = {}
    for name, data in sarc_files(pack).items():
        if name.endswith(".zsdic"):
            zdict = zstandard.ZstdCompressionDict(data)
            result[zdict.dict_id()] = zdict
    return result


def decompressed_size(data: bytes, dicts: dict[int, zstandard.ZstdCompressionDict]) -> int:
    dict_id = zstandard.get_frame_parameters(data).dict_id
    if dict_id and dict_id not in dicts:
        raise SystemExit(f"needs dictionary {dict_id}, which the game does not have")
    return len(zstandard.ZstdDecompressor(dict_data=dicts.get(dict_id)).decompress(data, max_output_size=1 << 30))


def budget_offset(table: bytes, key: str) -> int | None:
    count = struct.unpack_from("<I", table, 14)[0]
    crc = zlib.crc32(key.encode())
    lo, hi = 0, count
    while lo < hi:
        mid = (lo + hi) // 2
        if struct.unpack_from("<I", table, HEADER + mid * 8)[0] < crc:
            lo = mid + 1
        else:
            hi = mid
    if lo < count and struct.unpack_from("<I", table, HEADER + lo * 8)[0] == crc:
        return HEADER + lo * 8 + 4
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-romfs", type=Path, required=True, help="extracted game romfs (Pack/, System/Resource/)")
    ap.add_argument("--mod-romfs", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    dicts = dictionaries(args.game_romfs)
    sizes = {}
    for path in sorted(args.mod_romfs.rglob("*.zs")):
        key = path.relative_to(args.mod_romfs).as_posix()[:-3]
        sizes[key] = decompressed_size(path.read_bytes(), dicts)
    if not sizes:
        raise SystemExit(f"no .zs files under {args.mod_romfs}")

    tables = sorted((args.game_romfs / "System" / "Resource").glob(TABLE_GLOB))
    if not tables:
        raise SystemExit(f"no {TABLE_GLOB} under {args.game_romfs / 'System' / 'Resource'}")
    raised_any = False
    for table_path in tables:
        original = zstandard.ZstdDecompressor().decompress(table_path.read_bytes(), max_output_size=1 << 28)
        if original[:6] != b"RESTBL":
            raise SystemExit(f"{table_path.name}: not a RESTBL")
        table = bytearray(original)
        touched, raised = set(), 0
        for key, size in sizes.items():
            offset = budget_offset(original, key)
            if offset is None:
                print(f"{table_path.name}: {key}: no entry, skipped")
                continue
            old = struct.unpack_from("<I", original, offset)[0]
            new = max(old, (size + 0x1F & ~0x1F) + 0x1000)
            print(f"{table_path.name}: {key}: decompressed {size:,}, budget {old:,} -> {new:,}")
            if new != old:
                struct.pack_into("<I", table, offset, new)
                touched.update(range(offset, offset + 4))
                raised += 1
        changed = {i for i, (a, b) in enumerate(zip(original, table)) if a != b}
        if not changed <= touched or len(table) != len(original):
            raise SystemExit(f"{table_path.name}: edit touched bytes outside the raised budgets")
        if not raised:
            continue
        compressed = zstandard.ZstdCompressor(level=19).compress(bytes(table))
        if zstandard.get_frame_parameters(compressed).dict_id != 0:
            raise SystemExit("size table must be plain zstd")
        if zstandard.ZstdDecompressor().decompress(compressed, max_output_size=1 << 28) != table:
            raise SystemExit(f"{table_path.name}: compressed table does not round-trip")
        out = args.out / "romfs" / "System" / "Resource" / table_path.name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(compressed)
        raised_any = True
        print(f"wrote {out} ({raised} budget(s) raised)")
    if not raised_any:
        print("every file fits its budget; no size table needed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
