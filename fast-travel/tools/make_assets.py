"""Builds data/assets.bin for the Fast Travel menu: area pictures (JPEG previews and raw RGB thumbnails, for screen
scale 3 and 2) and anti-aliased glyph sets for the menu's fonts.

Usage: python tools/make_assets.py assets/areas data/assets.bin
(the picture for each area is <GameBancParam name>.jpg or .png in the first folder; the output goes into exlaunch's
data/ folder, which the build links in with bin2o)

Blob layout (little-endian):
  header:  'FTA1', u32 imageCount, u32 fontCount, u32 0
  images:  imageCount x { char name[40]; u32 jpeg3Off, jpeg3Size, jpeg2Off, jpeg2Size, thumb3Off, thumb2Off }
  fonts:   fontCount x { u32 id; i32 ascent; i32 descent; u32 glyphsOff }
  glyphs:  per font 95 x { i16 xoff; i16 yoff; u16 w; u16 h; u16 advance64; u16 0; u32 bitmapOff } (chars 32-126)
  data:    JPEGs, RGB888 thumbnails, 8-bit glyph coverage
"""
import io
import os
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
INC = next(p for p in (os.path.join(HERE, "..", d, "program", "destinations.inc") for d in ("src", "source"))
           if os.path.exists(p))

PREVIEW = {3: (450, 252), 2: (300, 168)}
THUMB = {3: (48, 27), 2: (32, 18)}

# Font ids: class * 10 + screen scale. Classes must match overlay.cpp.
FONTS = [
    # class, file, weight, px at scale 3, px at scale 2
    (0, "Cinzel.ttf", 700, 30, 20),  # menu title
    (1, "Cinzel.ttf", 600, 25, 17),  # area name under the picture
    (2, "Inter.ttf", 500, 20, 14),   # list rows
    (3, "Inter.ttf", 400, 16, 12),   # small text: counts, status, hints
    (4, "Inter.ttf", 700, 15, 11),   # letters inside the button marks
]


def area_names():
    text = open(INC, encoding="utf-8").read()
    block = text[text.index("kAreas[]"):text.index("};", text.index("kAreas[]"))]
    names = []
    for name in re.findall(r'\{"([^"]+)"', block):
        if name not in names:
            names.append(name)
    return names


def crop_16x9(im):
    w, h = im.size
    target = w * 9 / 16
    if target <= h:
        top = (h - target) / 2
        return im.crop((0, round(top), w, round(top + target)))
    width = h * 16 / 9
    left = (w - width) / 2
    return im.crop((round(left), 0, round(left + width), h))


def jpeg(im, size):
    buf = io.BytesIO()
    im.resize(size, Image.LANCZOS).save(buf, "JPEG", quality=88, subsampling=0, progressive=False)
    return buf.getvalue()


def thumb(im, size):
    return im.resize(size, Image.LANCZOS).convert("RGB").tobytes()


def load_font(path, weight, px):
    font = ImageFont.truetype(path, px)
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


def glyphs(font):
    out = []
    for code in range(32, 127):
        ch = chr(code)
        advance64 = round(font.getlength(ch) * 64)
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        w, h = max(0, right - left), max(0, bottom - top)
        bitmap = b""
        if w > 0 and h > 0:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-left, -top), ch, font=font, fill=255, anchor="ls")
            bitmap = im.tobytes()
        out.append((left, top, w, h, advance64, bitmap))
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    names = area_names()
    data = bytearray()

    def put(blob):
        while len(data) % 4:
            data.append(0)
        off = len(data)
        data.extend(blob)
        return off

    image_rows = []
    for name in names:
        path = next(p for p in (os.path.join(src, name + ext) for ext in (".jpg", ".png")) if os.path.exists(p))
        im = crop_16x9(Image.open(path).convert("RGB"))
        j3, j2 = jpeg(im, PREVIEW[3]), jpeg(im, PREVIEW[2])
        row = (name, put(j3), len(j3), put(j2), len(j2), put(thumb(im, THUMB[3])), put(thumb(im, THUMB[2])))
        image_rows.append(row)

    font_rows = []
    for cls, file, weight, px3, px2 in FONTS:
        for scale, px in ((3, px3), (2, px2)):
            font = load_font(os.path.join(HERE, "fonts", file), weight, px)
            ascent, descent = font.getmetrics()
            table = bytearray()
            for left, top, w, h, adv, bitmap in glyphs(font):
                off = put(bitmap) if bitmap else 0
                table += struct.pack("<hhHHHHI", left, top, w, h, adv, 0, off)
            font_rows.append((cls * 10 + scale, ascent, descent, put(bytes(table))))

    header_size = 16 + len(image_rows) * (40 + 24) + len(font_rows) * 16
    blob = bytearray(b"FTA1" + struct.pack("<III", len(image_rows), len(font_rows), 0))
    for name, *offs in image_rows:
        assert len(name) < 40
        offs = [o + header_size if i % 2 == 0 or i >= 4 else o for i, o in enumerate(offs)]
        blob += name.encode().ljust(40, b"\0") + struct.pack("<6I", *offs)
    for fid, ascent, descent, goff in font_rows:
        blob += struct.pack("<IiiI", fid, ascent, descent, goff + header_size)
    assert len(blob) == header_size
    # Glyph tables hold offsets relative to the data area; fix them up to blob offsets.
    for fid, ascent, descent, goff in font_rows:
        for g in range(95):
            pos = goff + g * 16 + 12
            (rel,) = struct.unpack_from("<I", data, pos)
            if rel:
                struct.pack_into("<I", data, pos, rel + header_size)
    blob += data
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(blob)
    print(f"{len(image_rows)} images, {len(font_rows)} fonts, {len(blob) / 1024:.0f} KiB -> {dst}")


if __name__ == "__main__":
    main()
