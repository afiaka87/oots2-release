# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
"""TotK TexToGo (.txtg) -> the remake's single-texture BNTX ("Ounc", 0x40101); BC1/BC4/BC5, square, power of two.

Blocks are copied unchanged, only moved to the remake's block order (_places). selftest rebuilds vanilla files byte
for byte and checks the block order on vanilla pictures.

  uv run --no-project --with zstandard python textures.py selftest GAME_ROMFS [NAME ...]
"""

from __future__ import annotations

import importlib.util
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import zstandard

ROOT = Path(__file__).resolve().parents[5]
BYTES_PER_BLOCK = {0x1A: 8, 0x1D: 8, 0x1E: 16}  # BC1, BC4, BC5
BC1_SRGB, BC1_UNORM, BC4_UNORM, BC5_UNORM = 0x1A06, 0x1A01, 0x1D01, 0x1E01
CHANNELS_RGBA = bytes([2, 3, 4, 5])
CHANNELS_RG01 = bytes([2, 3, 0, 1])  # two-channel normal map: red, green, zero, one
CHANNELS_RRRR = bytes([2, 2, 2, 2])
SELFTEST_NAMES = (  # BC5, BC4 256 and 128, BC1 sRGB, BC1
    "Cloth_Web_Basic_00_Nrm",
    "2dVolume_Texture_00_Density_Fi",
    "2dVolume_Texture_00_Height_Fi",
    "Cloud_Shape_EnvDome_01_FilmGrain_Fc",
    "CmnTex_Map_CityPartsRoofMossA_RAH",
)
ORDER_NAMES = ("Horse_Link_Saddle_06_Alb", "Horse_Link_Saddle_Cloth_06_Alb", "Mt_Alp_Enemy_Biri_Alb")  # BC1 pictures


def _load_swizzle():
    path = ROOT / "scripts" / "totk_mod_tools" / "textures" / "swizzle.py"
    spec = importlib.util.spec_from_file_location("paraglider_swizzle", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


swizzle = _load_swizzle()


class TextureError(ValueError):
    pass


@dataclass(frozen=True)
class Texture:
    name: str
    format: int
    size: int  # width and height in pixels
    channels: bytes
    mips: tuple[bytes, ...]  # row-major 4x4 blocks per mip level, largest first


def block_height(rows: int) -> int:
    """GOBs stacked per block for a surface `rows` elements high (1, 2, 4, 8 or 16)."""
    gobs = (rows + 7) // 8
    height = 1
    while height < gobs and height < 16:
        height *= 2
    return height


def _blocks(size: int, level: int) -> int:
    return (max(1, size >> level) + 3) // 4


def _check_class(fmt: int, size: int, mips: int) -> int:
    if fmt >> 8 not in BYTES_PER_BLOCK:
        raise TextureError(f"format {fmt:#06x} is not BC1, BC4 or BC5")
    if size < 128 or size & (size - 1):
        raise TextureError(f"size {size} is not a power of two of at least 128")
    if mips != size.bit_length():
        raise TextureError(f"{mips} mip levels; a full chain for {size} is {size.bit_length()}")
    return BYTES_PER_BLOCK[fmt >> 8]


def _places(blocks: int, per: int, remake: bool) -> list[int]:
    """Byte offset of every block in row order; remake GOBs go x%16, y%4, x/16, y/4 (Switch 1: y%2, y/2)."""
    height = block_height(blocks)
    per_row = swizzle.gobs_per_row(blocks, per)
    out = []
    for y in range(blocks):
        block_y, gob_in_block = divmod(y // 8, height)
        for x in range(blocks):
            xb = x * per
            base = (block_y * per_row * height + (xb // 64) * height + gob_in_block) * swizzle.GOB_BYTES
            if remake:
                inside = (xb % 64 // 32) * 256 + (y % 8 // 4) * 128 + (xb % 32 // 16) * 64 + (y % 4) * 16 + xb % 16
            else:
                inside = swizzle.gob_offset(xb, y)
            out.append(base + inside)
    return out


def _swizzled(linear: bytes, fmt: int, size: int, level: int, remake: bool = True) -> bytes:
    blocks, per = _blocks(size, level), BYTES_PER_BLOCK[fmt >> 8]
    if len(linear) != blocks * blocks * per:
        raise TextureError(f"mip {level} has {len(linear)} bytes, expected {blocks * blocks * per}")
    out = bytearray(swizzle.swizzled_size(blocks, blocks, per, block_height(blocks)))
    for index, place in enumerate(_places(blocks, per, remake)):
        out[place:place + per] = linear[index * per:(index + 1) * per]
    return bytes(out)


def _deswizzled(data: bytes, fmt: int, size: int, level: int, remake: bool = True) -> bytes:
    blocks, per = _blocks(size, level), BYTES_PER_BLOCK[fmt >> 8]
    expected = swizzle.swizzled_size(blocks, blocks, per, block_height(blocks))
    if len(data) > expected:
        raise TextureError(f"mip {level} surface is {len(data)} bytes, expected at most {expected}")
    data = data + bytes(expected - len(data))  # TexToGo drops the unused tail of small mips
    return b"".join(data[place:place + per] for place in _places(blocks, per, remake))


def read_txtg(path: Path) -> Texture:
    """TotK TexToGo file -> blocks. Format codes: 0x?02 BC1, 0x606/0x607 BC4, 0x702 BC5."""
    data = path.read_bytes()
    header, version, magic = struct.unpack_from("<HH4s", data, 0)
    if magic != b"6PK0" or header != 0x50:
        raise TextureError(f"{path.name}: not a TexToGo file")
    width, height, depth, mips = struct.unpack_from("<HHHB", data, 8)
    code = struct.unpack_from("<H", data, 0x3C)[0]
    srgb = bool(data[0x4D] & 2)
    if code in (0x606, 0x607):
        kind = 0x1D
    elif code == 0x702:
        kind = 0x1E
    elif code & 0xFF == 0x02 and code >> 8 in (2, 3):
        kind = 0x1A
    else:
        raise TextureError(f"{path.name}: unhandled TexToGo format {code:#x}")
    if width != height or depth != 1:
        raise TextureError(f"{path.name}: {width}x{height}x{depth} is not a square 2D texture")
    fmt = kind << 8 | (6 if kind == 0x1A and srgb else 1)
    _check_class(fmt, width, mips)
    cursor = 0x50 + 4 * mips
    sizes = []
    for level in range(mips):
        if data[0x50 + 4 * level + 2] != level:
            raise TextureError(f"{path.name}: mip table is not in level order")
        size, codec = struct.unpack_from("<II", data, cursor + 8 * level)
        if codec != 6:
            raise TextureError(f"{path.name}: mip {level} uses codec {codec}, expected zstd (6)")
        sizes.append(size)
    cursor += 8 * mips
    levels = []
    for level, size in enumerate(sizes):
        surface = zstandard.ZstdDecompressor().decompress(data[cursor:cursor + size], max_output_size=1 << 26)
        levels.append(_deswizzled(surface, fmt, width, level, remake=False))
        cursor += size
    if cursor != len(data):
        raise TextureError(f"{path.name}: {len(data) - cursor} bytes left after the last mip")
    return Texture(path.name.removesuffix(".txtg"), fmt, width, bytes(data[0x18:0x1C]), tuple(levels))


def constant_bc4(name: str, value: int, size: int = 128) -> Texture:
    """A BC4 texture with one value everywhere (both endpoints equal, all selectors zero)."""
    block = bytes([value, value, 0, 0, 0, 0, 0, 0])
    mips = tuple(block * _blocks(size, level) ** 2 for level in range(size.bit_length()))
    return Texture(name, BC4_UNORM, size, CHANNELS_RRRR, mips)


def _dict_reference(name: bytes) -> int:
    """Bit a one-entry name dictionary tests: the lowest set bit, counted from the end of the name."""
    for index in range(len(name) * 8):
        if name[len(name) - 1 - (index >> 3)] >> (index & 7) & 1:
            return index
    raise TextureError("empty texture name")


def build_bntx(texture: Texture) -> bytes:
    """One texture -> the remake's BNTX layout."""
    _check_class(texture.format, texture.size, len(texture.mips))
    name = texture.name.encode("ascii")
    surfaces = [_swizzled(mip, texture.format, texture.size, level) for level, mip in enumerate(texture.mips)]
    image_size = sum(map(len, surfaces))

    info_array, strings = 0x198, 0x1A0
    empty_key, name_entry = strings + 0x14, strings + 0x18
    dictionary = (name_entry + 2 + len(name) + 1 + 7) & ~7
    info = dictionary + 0x28
    runtime, view = info + 0xA0, info + 0x1A0
    mip_table = info + 0x2A0
    data_block = ((mip_table + 8 * len(surfaces) + 0x10 + 0xFFF) & ~0xFFF) - 0x10
    first_mip = data_block + 0x10
    relocations = (first_mip + image_size + 0xFFF) & ~0xFFF

    out = bytearray(relocations)
    struct.pack_into("<4sIIHBBIHHII", out, 0, b"BNTX", 0, 0x00040101, 0xFEFF, 0x0C, 0x40, name_entry + 2, 0,
                     strings, relocations, 0)
    struct.pack_into("<4sIQQQQ", out, 0x20, b"Ounc", 1, info_array, data_block, dictionary, 0x58)
    struct.pack_into("<Q", out, info_array, info)
    struct.pack_into("<4sIQI", out, strings, b"_STR", info - strings, info - strings, 1)
    struct.pack_into("<H", out, name_entry, len(name))
    out[name_entry + 2:name_entry + 2 + len(name)] = name
    reference = _dict_reference(name)
    struct.pack_into("<4sI", out, dictionary, b"_DIC", 1)
    struct.pack_into("<IHHQ", out, dictionary + 8, 0xFFFFFFFF, 1, 0, empty_key)
    struct.pack_into("<IHHQ", out, dictionary + 0x18, reference, 0, 1, name_entry)
    struct.pack_into("<4sIQ", out, info, b"BRTI", data_block - info, data_block - info)
    struct.pack_into("<BBHHHHH", out, info + 0x10, 9, 2, 0, 0, len(surfaces), 1, 0)
    layout = block_height(_blocks(texture.size, 0)).bit_length() - 1
    struct.pack_into("<IIIIIIII", out, info + 0x1C, texture.format, 0x21, texture.size, texture.size, 1, 1,
                     layout, 0x000100C9)
    struct.pack_into("<II4sI", out, info + 0x50, image_size, 0x200, texture.channels, 1)
    struct.pack_into("<QQQQQQ", out, info + 0x60, name_entry, 0x20, mip_table, 0, runtime, view)
    cursor = first_mip
    for level, surface in enumerate(surfaces):
        struct.pack_into("<Q", out, mip_table + 8 * level, cursor)
        out[cursor:cursor + len(surface)] = surface
        cursor += len(surface)
    struct.pack_into("<4sIQ", out, data_block, b"BRTD", 0, relocations - data_block)

    table = bytearray(struct.pack("<4sII4x", b"_RLT", relocations, 2))
    table += struct.pack("<QIIII", 0, 0, mip_table + 8 * len(surfaces), 0, 4)
    table += struct.pack("<QIIII", 0, data_block, relocations - data_block, 4, 2)
    for position, structures, offsets, padding in (
        (0x28, 2, 1, (info_array - 0x28) // 8 - 1),
        (0x38, 2, 2, (info + 0x80 - 0x38) // 8 - 2),
        (dictionary + 0x10, 2, 1, 1),
        (info + 0x60, 1, 3, 0),
        (0x30, 1, 1, 0),
        (mip_table, 1, len(surfaces), 0),
    ):
        if padding > 0xFF:
            raise TextureError("name too long for the relocation table")
        table += struct.pack("<IHBB", position, structures, offsets, padding)
    out += table
    struct.pack_into("<I", out, 0x1C, len(out))
    return bytes(out)


def parse_bntx(data: bytes) -> Texture:
    """The inverse of build_bntx for the same class of file."""
    if data[:4] != b"BNTX" or data[0x20:0x24] != b"Ounc" or struct.unpack_from("<I", data, 0x24)[0] != 1:
        raise TextureError("not a single-texture remake BNTX")
    info = struct.unpack_from("<Q", data, struct.unpack_from("<Q", data, 0x28)[0])[0]
    mips = struct.unpack_from("<H", data, info + 0x16)[0]
    fmt, _, width, height, depth, layers = struct.unpack_from("<IIIIII", data, info + 0x1C)
    if width != height or depth != 1 or layers != 1:
        raise TextureError(f"{width}x{height}, depth {depth}, {layers} layers is outside the handled class")
    _check_class(fmt, width, mips)
    name_entry, _, mip_table = struct.unpack_from("<QQQ", data, info + 0x60)
    name = data[name_entry + 2:name_entry + 2 + struct.unpack_from("<H", data, name_entry)[0]].decode("ascii")
    levels, per = [], BYTES_PER_BLOCK[fmt >> 8]
    for level in range(mips):
        start = struct.unpack_from("<Q", data, mip_table + 8 * level)[0]
        blocks = _blocks(width, level)
        size = swizzle.swizzled_size(blocks, blocks, per, block_height(blocks))
        levels.append(_deswizzled(data[start:start + size], fmt, width, level))
    return Texture(name, fmt, width, bytes(data[info + 0x58:info + 0x5C]), tuple(levels))


def compress(data: bytes) -> bytes:
    """Plain zstd (no dictionary), as every vanilla Tex/*.bntx.zs is."""
    packed = zstandard.ZstdCompressor(level=19).compress(data)
    if zstandard.get_frame_parameters(packed).dict_id:
        raise TextureError("texture must be plain zstd")
    return packed


def order_fit(texture: Texture) -> float:
    """Colour-step ratio, remake order over Switch 1 order: vanilla pictures 0.46-0.78, noise ~1, wrong order > 1."""
    if texture.format >> 8 != 0x1A:
        raise TextureError("the block order check needs a BC1 texture")
    side = _blocks(texture.size, 0)

    def steps(blocks: bytes) -> int:
        green = [struct.unpack_from("<H", blocks, 8 * index)[0] >> 5 & 63 for index in range(side * side)]
        across = sum(abs(green[i] - green[i + 1]) for i in range(side * side - 1) if (i + 1) % side)
        return across + sum(abs(green[i] - green[i + side]) for i in range(side * side - side))

    stored = _swizzled(texture.mips[0], texture.format, texture.size, 0)
    return steps(texture.mips[0]) / max(1, steps(_deswizzled(stored, texture.format, texture.size, 0, remake=False)))


def check_block_order(game_romfs: Path, names=ORDER_NAMES) -> list[float]:
    """Vanilla pictures must come out smoother in the remake's block order than in the Switch 1 order."""
    fits = []
    for name in names:
        path = game_romfs / "Tex" / f"{name}.bntx.zs"
        texture = parse_bntx(zstandard.ZstdDecompressor().decompress(path.read_bytes(), max_output_size=1 << 28))
        fits.append(order_fit(texture))
        if fits[-1] > 0.9:
            raise TextureError(f"{name}: block order fit {fits[-1]:.2f}; the remake's order no longer matches")
    return fits


def reproduce(game_romfs: Path, names=SELFTEST_NAMES) -> None:
    """Rebuild vanilla textures from their parsed fields; any byte difference is an error."""
    for name in names:
        path = game_romfs / "Tex" / f"{name}.bntx.zs"
        original = zstandard.ZstdDecompressor().decompress(path.read_bytes(), max_output_size=1 << 28)
        rebuilt = build_bntx(parse_bntx(original))
        if rebuilt != original:
            first = next((i for i, (a, b) in enumerate(zip(original, rebuilt)) if a != b), min(len(original), len(rebuilt)))
            raise TextureError(f"{name}: rebuilt file differs from vanilla at {first:#x} "
                               f"(sizes {len(original)} and {len(rebuilt)})")


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] != "selftest":
        print(__doc__)
        return 2
    names = tuple(sys.argv[3:]) or SELFTEST_NAMES
    reproduce(Path(sys.argv[2]), names)
    print(f"rebuilt {len(names)} vanilla texture file(s) byte for byte")
    fits = check_block_order(Path(sys.argv[2]))
    print("block order fit (remake order / Switch 1 order): " + ", ".join(f"{fit:.2f}" for fit in fits))
    return 0


if __name__ == "__main__":
    sys.exit(main())
