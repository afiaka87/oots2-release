"""Builds the Fast Travel menu's two asset files.

  fonts <out.inc>              anti-aliased glyph sets, as a C array the patch build compiles into the module
  pictures <folder> <out.bin>  one picture per area (raw RGB previews and thumbnails for screen scales 3 and 2),
                               installed on the SD card as fast-travel/pictures.bin

The picture for each area is <GameBancParam name>.jpg or .png in the folder; a missing picture is an error.

Font file (little-endian; every offset is from the start of the file, and 0 means "none"):
  header:  'FTF1', u32 fontCount, u32 glyphFirst (32), u32 glyphCount (95)
  fonts:   fontCount x { u32 id; i32 ascent; i32 descent; u32 glyphsOff }   id = class * 10 + screen scale
  glyphs:  per font glyphCount x { i16 xoff; i16 yoff; u16 w; u16 h; u16 advance64; u16 0; u32 bitmapOff }
  data:    8-bit glyph coverage, w x h bytes per glyph

Picture file:
  header:  'FTI1', u32 imageCount, u32 0, u32 0
  images:  imageCount x { char name[40]; u32 preview3Off, preview2Off, thumb3Off, thumb2Off, 0, 0 }
  data:    RGB888 previews (450x252 and 300x168) and thumbnails (48x27 and 32x18), rows top-down

The previews are raw so the patch needs no picture decoder (one decoder did not fit in the game's free space).

The sizes and ids here must match src/program/ui/assets.cpp.
"""
import re
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
INC = HERE.parent / "src" / "program" / "destinations.inc"

PREVIEW = {3: (450, 252), 2: (300, 168)}
THUMB = {3: (48, 27), 2: (32, 18)}
GLYPH_FIRST, GLYPH_COUNT = 32, 95

FONTS = [
    # class, file, weight, px at scale 3, px at scale 2
    (0, "Cinzel.ttf", 700, 30, 20),  # menu title
    (1, "Cinzel.ttf", 600, 25, 17),  # area name under the picture
    (2, "Inter.ttf", 500, 20, 14),   # list rows
    (3, "Inter.ttf", 400, 16, 12),   # small text: counts, status, hints
    (4, "Inter.ttf", 700, 15, 11),   # letters inside the button marks
]


def area_names() -> list[str]:
    text = INC.read_text(encoding="utf-8")
    start = text.index("kAreas[]")
    names = []
    for name in re.findall(r'\{"([^"]+)"', text[start:text.index("};", start)]):
        if name not in names:
            names.append(name)
    return names


class Blob:
    """A file built back to front: fixed-size tables first, then 4-byte aligned data appended after them."""

    def __init__(self, table_bytes: int):
        self.data = bytearray(table_bytes)

    def put(self, payload: bytes) -> int:
        self.data.extend(bytes(-len(self.data) % 4))
        offset = len(self.data)
        self.data.extend(payload)
        return offset


def load_font(path: Path, weight: int, px: int):
    font = ImageFont.truetype(str(path), px)
    axes = font.get_variation_axes()
    values = []
    for axis in axes:
        name = axis.get("name", b"")
        name = name.decode() if isinstance(name, bytes) else name
        if name.lower().startswith("weight"):
            values.append(weight)
        elif name.lower().startswith("optical"):
            values.append(max(axis["minimum"], min(axis["maximum"], px)))
        else:
            values.append(axis["default"])
    if axes:
        font.set_variation_by_axes(values)
    return font


def build_fonts() -> bytes:
    sets = [(cls * 10 + scale, load_font(HERE / "fonts" / file, weight, px))
            for cls, file, weight, px3, px2 in FONTS for scale, px in ((3, px3), (2, px2))]
    table = 16 + 16 * len(sets)
    blob = Blob(table)
    struct.pack_into("<4sIII", blob.data, 0, b"FTF1", len(sets), GLYPH_FIRST, GLYPH_COUNT)
    for index, (font_id, font) in enumerate(sets):
        rows = bytearray()
        for code in range(GLYPH_FIRST, GLYPH_FIRST + GLYPH_COUNT):
            ch = chr(code)
            left, top, right, bottom = font.getbbox(ch, anchor="ls")
            w, h = max(0, right - left), max(0, bottom - top)
            offset = 0
            if w and h:
                im = Image.new("L", (w, h), 0)
                ImageDraw.Draw(im).text((-left, -top), ch, font=font, fill=255, anchor="ls")
                offset = blob.put(im.tobytes())
            rows += struct.pack("<hhHHHHI", left, top, w, h, round(font.getlength(ch) * 64), 0, offset)
        ascent, descent = font.getmetrics()
        struct.pack_into("<IiiI", blob.data, 16 + 16 * index, font_id, ascent, descent, blob.put(bytes(rows)))
    return bytes(blob.data)


def crop_16x9(im: Image.Image) -> Image.Image:
    w, h = im.size
    if w * 9 <= h * 16:
        top = (h - w * 9 / 16) / 2
        return im.crop((0, round(top), w, round(h - top)))
    left = (w - h * 16 / 9) / 2
    return im.crop((round(left), 0, round(w - left), h))


def build_pictures(folder: Path) -> bytes:
    names = area_names()
    blob = Blob(16 + 64 * len(names))
    struct.pack_into("<4sIII", blob.data, 0, b"FTI1", len(names), 0, 0)
    for index, name in enumerate(names):
        path = next((p for p in (folder / f"{name}.jpg", folder / f"{name}.png") if p.exists()), None)
        if path is None:
            raise SystemExit(f"no picture for {name} in {folder}")
        if len(name) >= 40:
            raise SystemExit(f"area name too long for the picture table: {name}")
        im = crop_16x9(Image.open(path).convert("RGB"))
        fields = [blob.put(im.resize(size[scale], Image.LANCZOS).tobytes()) for size in (PREVIEW, THUMB) for scale in (3, 2)]
        struct.pack_into("<40s6I", blob.data, 16 + 64 * index, name.encode(), *fields, 0, 0)
    return bytes(blob.data)


def as_c_array(blob: bytes, name: str) -> str:
    lines = [f"// Generated by tools/make_assets.py fonts; {len(blob)} bytes.",
             f"alignas(4) inline constexpr unsigned char {name}[] = {{"]
    for at in range(0, len(blob), 32):
        lines.append("    " + ",".join(str(b) for b in blob[at:at + 32]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "fonts":
        blob = build_fonts()
        Path(sys.argv[2]).write_text(as_c_array(blob, "kFontFile"), encoding="utf-8")
    elif len(sys.argv) == 4 and sys.argv[1] == "pictures":
        blob = build_pictures(Path(sys.argv[2]))
        Path(sys.argv[3]).write_bytes(blob)
    else:
        raise SystemExit(__doc__)
    print(f"{sys.argv[1]}: {len(blob)} bytes -> {sys.argv[-1]}")


if __name__ == "__main__":
    main()
