# /// script
# requires-python = ">=3.11"
# dependencies = ["zstandard", "texture2ddecoder", "pillow", "numpy"]
# ///
"""Build the game-data half of the Paraglider mod for the Ocarina of Time remake (Switch 2, version 2.0.0).

A large chest in Hyrule Field (child and adult) gives a new key item, "Paraglider", with the game's item-get
sequence and a saved flag the code half reads. Reads the user's own game dumps;
writes only under --out. Every step re-reads what it wrote and stops on any difference from the game's own layout.

  uv run data/build_data.py --work <work folder> \
      --game-romfs <full 2.0.0 romfs> --totk-romfs <TotK romfs> [--out <mod romfs>]
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import texture2ddecoder
import zstandard
from PIL import Image, ImageFilter

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[3] / "tools"))
import gbyml  # noqa: E402
from byml7 import F32, U32, U64, typed  # noqa: E402
from oot_save import murmur3_32  # noqa: E402

ITEM = "Item_Paraglider"
TEMPLATE = "Item_Token_Gerudo"  # key item: no use action, no equipment slot, large item-get sequence
RSDB_VERSION, DATA_VERSION = "111", "110"
# map files, age variants, a game chest present in each (placement template), our placement-id labels
CHEST_MAPS = {
    "hyrule-field": ("Banc/HyruleField/HyruleField_{}_Dynamic.bcett.byml.zs", ("Child", "Adult"),
                     17445672702269453589, "oot-paraglider:chest:hyrule-field:v1", "oot-paraglider:keep-away:hyrule-field:{}:v1"),
}
VANILLA_CHEST = 12902291359202819102  # Kakariko's small chest; template for the new chest's saved flag
SPOT_POS, SPOT_YAW = [-55.886, 148.050, 9.089], -0.6429  # HanSoloBlaster's spot; the lid opens along the yaw

# English in every language; line lengths follow the game's own entries (Caption ~42 x 3, GetCaption ~48 x 3)
TEXT = {
    "Name": "Paraglider",
    "Caption": "A sturdy glider of cloth and wood, found\nin Hyrule Field. Jump, then press jump\n"
               "again in midair to drift down gently.",
    "GetMessage": "You found the Paraglider!",
    "GetCaption": "Jump, then press jump again in midair to open\nit and drift gently to the ground.",
}


def die(msg: str):
    raise SystemExit(f"build_data: {msg}")


def m3(name: str) -> int:
    return murmur3_32(name.encode("utf-8"))


def f32(x: float) -> F32:
    return F32(struct.unpack("<I", struct.pack("<f", x))[0])


def stable_u64(label: str) -> int:
    return int.from_bytes(hashlib.sha256(label.encode()).digest()[:8], "little")


def sarc_read(data: bytes) -> list[tuple[str, bytes]]:
    if data[:4] != b"SARC" or data[6:8] != b"\xff\xfe":
        die("not a little-endian SARC archive")
    hdr, _, data_off = struct.unpack_from("<HxxII", data, 4)
    if data[hdr:hdr + 4] != b"SFAT":
        die("SARC without SFAT")
    count, mult = struct.unpack_from("<HI", data, hdr + 6)
    if mult != 101:
        die(f"SARC hash multiplier {mult}")
    nodes = hdr + 0x0C
    names = nodes + 0x10 * count + 8
    out = []
    for i in range(count):
        _, attr, start, end = struct.unpack_from("<IIII", data, nodes + 0x10 * i)
        at = names + (attr & 0xFFFF) * 4
        out.append((data[at:data.index(b"\0", at)].decode(), data[data_off + start:data_off + end]))
    return out


def sarc_hash(name: str) -> int:
    h = 0
    for c in name.encode():
        h = (h * 101 + c) & 0xFFFFFFFF
    return h


def sarc_write(files: dict[str, bytes]) -> bytes:
    """Layout of the game's own archives: nodes by name hash, names padded to 4, data and every file aligned to 8."""
    order = sorted(files, key=sarc_hash)
    if len({sarc_hash(n) for n in order}) != len(order):
        die("SARC name hash collision")
    names, nodes, data = bytearray(), bytearray(), bytearray()
    for name in order:
        data += b"\0" * (-len(data) % 8)
        nodes += struct.pack("<IIII", sarc_hash(name), 0x01000000 | (len(names) // 4), len(data), len(data) + len(files[name]))
        data += files[name]
        names += name.encode() + b"\0"
        names += b"\0" * (-len(names) % 4)
    head = 0x14 + 0x0C + len(nodes) + 8 + len(names)
    data_off = head + (-head % 8)
    out = struct.pack("<4sHHIIHH", b"SARC", 0x14, 0xFEFF, data_off + len(data), data_off, 0x0100, 0)
    out += struct.pack("<4sHHI", b"SFAT", 0x0C, len(order), 101) + bytes(nodes)
    out += struct.pack("<4sHH", b"SFNT", 8, 0) + bytes(names)
    return out + b"\0" * (data_off - head) + bytes(data)


class Game:
    """Read-only access to an extracted romfs (one or more folders holding the same game version)."""

    def __init__(self, roots: list[Path], what: str):
        self.roots, self.what = roots, what
        pack = zstandard.ZstdDecompressor().decompress(self.read("Pack/ZsDic.pack.zs"), max_output_size=1 << 26)
        self.dicts = {}
        for name, blob in sarc_read(pack):
            if name.endswith(".zsdic"):
                d = zstandard.ZstdCompressionDict(blob)
                self.dicts[d.dict_id()] = d

    def has(self, rel: str) -> bool:
        return any((r / rel).is_file() for r in self.roots)

    def read(self, rel: str) -> bytes:
        found = [(r / rel).read_bytes() for r in self.roots if (r / rel).is_file()]
        if not found:
            die(f"{self.what}: {rel} is in none of {[str(r) for r in self.roots]}")
        if any(b != found[0] for b in found[1:]):
            die(f"{self.what}: {rel} differs between the romfs folders given; they are not the same game version")
        return found[0]

    def unzs(self, blob: bytes) -> tuple[bytes, int]:
        dict_id = zstandard.get_frame_parameters(blob).dict_id
        if dict_id and dict_id not in self.dicts:
            die(f"{self.what}: frame needs dictionary {dict_id}, which Pack/ZsDic.pack.zs does not hold")
        dec = zstandard.ZstdDecompressor(dict_data=self.dicts[dict_id]) if dict_id else zstandard.ZstdDecompressor()
        return dec.decompress(blob, max_output_size=1 << 30), dict_id

    def load(self, rel: str) -> tuple[bytes, int]:
        return self.unzs(self.read(rel))

    def rezs(self, raw: bytes, dict_id: int) -> bytes:
        """Compress the way the game's copy of the file is compressed (same dictionary, or none)."""
        kw = {"dict_data": self.dicts[dict_id]} if dict_id else {}
        blob = zstandard.ZstdCompressor(level=19, write_content_size=True, **kw).compress(raw)
        back, got = self.unzs(blob)
        if back != raw or got != dict_id:
            die("compression round trip failed")
        return blob


class Out:
    """Collects the files to write and the size facts the resource-size step needs."""

    def __init__(self):
        self.files: dict[str, bytes] = {}
        self.rows: list[dict] = []

    def add(self, rel: str, blob: bytes, raw: bytes, vanilla_raw: bytes | None, dict_id: int, inner: list | None = None,
            template: str | None = None):
        self.files[rel] = blob
        self.rows.append({
            "path": rel, "new_file": vanilla_raw is None, "template": template, "compressed": len(blob), "uncompressed": len(raw),
            "vanilla_uncompressed": None if vanilla_raw is None else len(vanilla_raw),
            "zstd_dictionary": dict_id, "sha256": hashlib.sha256(blob).hexdigest(), "inner": inner or [],
        })


def retarget(v, old: str, new: str):
    if isinstance(v, dict):
        return {k: retarget(x, old, new) for k, x in v.items()}
    if isinstance(v, list):
        return [retarget(x, old, new) for x in v]
    return v.replace(old, new) if isinstance(v, str) else v


def coerce(template, value):
    """Give a plain JSON value the stored type of the matching value in a row of the game's table."""
    if isinstance(value, dict):
        return {k: coerce(template.get(k) if isinstance(template, dict) else None, x) for k, x in value.items()}
    if isinstance(value, list):
        first = template[0] if isinstance(template, list) and template else None
        return [coerce(first, x) for x in value]
    if isinstance(value, (bool, str)) or value is None:
        return value
    if isinstance(template, F32) or (template is None and isinstance(value, float)):
        return f32(float(value))
    if isinstance(template, U32):
        return U32(int(value))
    if isinstance(template, U64):
        return U64(int(value))
    if isinstance(template, bool) or isinstance(template, str):
        die(f"value {value!r} does not fit a {type(template).__name__} column")
    return int(value)


def byml_bytes(tree, version: int, big: bool) -> bytes:
    out = gbyml.dump(tree, version, big)
    if typed(gbyml.load(out)[2]) != typed(tree):
        die("BYML re-read differs from the tree written")
    return out


def build_item_pack(game: Game, out: Out, args) -> None:
    rel = f"Pack/Actor/{TEMPLATE}.pack.zs"
    raw, dict_id = game.load(rel)
    members = sarc_read(raw)
    if sarc_write(dict(members)) != raw:
        die(f"{rel}: this archive writer does not reproduce the game's file")
    # the game's ModelInfo files carry DebugModelScale {X, Y, Z} (always 1.0)
    probe = sarc_read(game.load("Pack/Actor/TBox_Shield_Deku.pack.zs")[0])
    shapes = [gbyml.load(b)[2].get("DebugModelScale") for n, b in probe if "/ModelInfo/" in n]
    if not any(isinstance(s, dict) and set(s) == {"X", "Y", "Z"} and isinstance(s["X"], F32) for s in shapes):
        die("DebugModelScale no longer has the shape the game's own files use")
    files, renamed = {}, []
    for name, blob in members:
        version, big, tree = gbyml.load(blob)
        new_name = name.replace(TEMPLATE, ITEM)
        new_tree = retarget(tree, TEMPLATE, ITEM)
        if "/ModelInfo/" in name and TEMPLATE in name:
            new_tree["FmdbName"], new_tree["ModelProjectName"] = args.model_fmdb, args.model_project
            if args.demo_scale != 1.0:
                new_tree["DebugModelScale"] = {k: f32(args.demo_scale) for k in "XYZ"}
        if "/AnimationParam/" in name and TEMPLATE in name:
            new_tree["AnimationResources"] = [{"ModelProjectName": n} for n in args.item_anim]
        if new_name == name and typed(new_tree) == typed(tree):
            files[name] = blob  # shared member, byte for byte the game's
        else:
            files[new_name] = byml_bytes(new_tree, version, big)
            renamed.append(new_name)
    expect = {f"Actor/{ITEM}.engine__actor__ActorParam.bgyml", f"Component/ModelInfo/{ITEM}.engine__component__ModelInfo.bgyml",
              f"Component/AnimationParam/{ITEM}.engine__component__AnimationParam.bgyml",
              f"Component/CaptureParam/{ITEM}.game__component__CaptureParam.bgyml",
              f"GameParameter/GameParameterTable/{ITEM}.engine__actor__GameParameterTable.bgyml"}
    if set(renamed) != expect:
        die(f"item pack: changed members {sorted(renamed)} are not the five expected")
    actor = gbyml.load(files[f"Actor/{ITEM}.engine__actor__ActorParam.bgyml"])[2]
    for key, ref in actor["Components"].items():
        target = ref[1:]
        if ref.startswith("?") and target not in files and not target.startswith(("AS/", "ActorSystem/ActorName/")):
            die(f"item pack: {key} points at {target}, which the pack does not hold")
    new = sarc_write(files)
    if dict(sarc_read(new)) != files:
        die("item pack: re-read differs")
    out.add(f"Pack/Actor/{ITEM}.pack.zs", game.rezs(new, dict_id), new, None, dict_id,
            [{"path": n, "uncompressed": len(b), "new_name": n in expect, "template": n.replace(ITEM, TEMPLATE) if n in expect else None,
              "template_uncompressed": len(dict(members)[n.replace(ITEM, TEMPLATE)]) if n in expect else None}
             for n, b in sarc_read(new)], template=rel)
    model = f"Model/{args.model_project}.{args.model_fmdb}.bfres.zs"
    if not (args.out / model).is_file() and not game.has(model):
        msg = f"the item's model file {model} is neither in the output folder nor in the game"
        if not args.allow_missing_model:
            die(msg + " (build the model first, or pass --allow-missing-model)")
        print(f"WARNING: {msg}; the chest would give an item with no model")


def rsdb_rows(game: Game, out: Out, table: str, additions: list[dict]) -> None:
    rel = f"RSDB/{table}.Product.{RSDB_VERSION}.rstbl.byml.zs"
    raw, dict_id = game.load(rel)
    version, big, tree = gbyml.load(raw)
    ids = [row["__RowId"] for row in tree]
    if ids != sorted(ids, key=str.encode) or len(set(ids)) != len(ids):
        die(f"{rel}: rows are not sorted by __RowId")
    by_id = dict(zip(ids, tree))
    for spec in additions:
        if spec["clone"] not in by_id:
            die(f"{table}: no row {spec['clone']} to clone")
        source = by_id[spec["clone"]]
        row = copy.deepcopy(source)
        for key, value in spec["set"].items():
            row[key] = coerce(source.get(key), value)
        if row["__RowId"] in by_id:
            die(f"{table}: row {row['__RowId']} already exists")
        by_id[row["__RowId"]] = row
        print(f"  {table}: + {row['__RowId']} (from {spec['clone']})")
    rows = [by_id[k] for k in sorted(by_id, key=str.encode)]
    new = byml_bytes(rows, version, big)
    out.add(rel, game.rezs(new, dict_id), new, raw, dict_id)


def build_rsdb(game: Game, out: Out, args) -> None:
    lo, hi = args.model_aabb[:3], args.model_aabb[3:]
    spec = {
        "ActorInfo": [{"clone": TEMPLATE, "set": {
            "__RowId": ITEM, "ActorName": ITEM, "FmdbName": args.model_fmdb, "ModelProjectName": args.model_project,
            "AnimationResources": [{"ModelProjectName": n} for n in args.item_anim],
            "ModelAabbMin": dict(zip("XYZ", lo)), "ModelAabbMax": dict(zip("XYZ", hi)),
            "InstanceHeapSize": args.instance_heap, "PreActorRenderInfoHeapSize": args.render_heap}}],
        "GameActorInfo": [{"clone": TEMPLATE, "set": {"__RowId": ITEM}}],
        "PouchActorInfo": [{"clone": TEMPLATE, "set": {"__RowId": ITEM}}],
    }
    if args.rsdb_extra and args.rsdb_extra.is_file():
        extra = json.loads(args.rsdb_extra.read_text(encoding="utf-8"))
        for table, rows in extra.items():
            if table not in spec:
                die(f"{args.rsdb_extra}: table {table} is not one this builder writes")
            spec[table] += rows
    for table, rows in spec.items():
        rsdb_rows(game, out, table, rows)


def msbt_read(raw: bytes):
    if raw[:8] != b"MsgStdBn" or raw[8:10] != b"\xff\xfe" or raw[12] != 0 or raw[13] != 3:
        die("message file is not UTF-8 MSBT version 3")
    if struct.unpack_from("<I", raw, 18)[0] != len(raw):
        die("message file size field is wrong")
    pos, sections = 32, {}
    while pos < len(raw):
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        sections[raw[pos:pos + 4]] = raw[pos + 16:pos + 16 + size]
        pos += 16 + size
        pos += -pos % 16
    if list(sections) != [b"LBL1", b"TXT2"]:
        die(f"message file sections {list(sections)}")
    lbl, txt = sections[b"LBL1"], sections[b"TXT2"]
    groups = []
    for g in range(struct.unpack_from("<I", lbl, 0)[0]):
        count, off = struct.unpack_from("<II", lbl, 4 + 8 * g)
        items = []
        for _ in range(count):
            n = lbl[off]
            items.append((lbl[off + 1:off + 1 + n].decode(), struct.unpack_from("<I", lbl, off + 1 + n)[0]))
            off += 5 + n
        groups.append(items)
    n = struct.unpack_from("<I", txt, 0)[0]
    offs = list(struct.unpack_from(f"<{n}I", txt, 4)) + [len(txt)]
    return groups, [txt[offs[i]:offs[i + 1]] for i in range(n)]


def msbt_write(header: bytes, groups, strings) -> bytes:
    heads, labels = [], bytearray()
    for group in groups:
        heads.append(struct.pack("<II", len(group), 4 + 8 * len(groups) + len(labels)))
        for name, index in group:
            labels += bytes([len(name.encode())]) + name.encode() + struct.pack("<I", index)
    lbl = struct.pack("<I", len(groups)) + b"".join(heads) + bytes(labels)
    offs, cur = [], 4 + 4 * len(strings)
    for s in strings:
        offs.append(cur)
        cur += len(s)
    txt = struct.pack(f"<I{len(strings)}I", len(strings), *offs) + b"".join(strings)
    body = b"".join(tag + struct.pack("<I", len(d)) + b"\0" * 8 + d + b"\xab" * (-len(d) % 16)
                    for tag, d in ((b"LBL1", lbl), (b"TXT2", txt)))
    head = bytearray(header[:32])
    struct.pack_into("<I", head, 18, 32 + len(body))
    return bytes(head) + body


def msbt_add(raw: bytes, additions: dict[str, str]) -> bytes:
    groups, strings = msbt_read(raw)
    if msbt_write(raw, groups, strings) != raw:
        die("this message writer does not reproduce the game's file")
    have = {name for g in groups for name, _ in g}
    for label, text in sorted(additions.items()):
        if label in have:
            die(f"label {label} already exists")
        if "\x0e" in text or "\0" in text:
            die("control tags are not supported in the new text")
        h = 0
        for c in label.encode():
            h = (h * 0x492 + c) & 0xFFFFFFFF
        group = groups[h % len(groups)]
        in_order = group == sorted(group, key=lambda item: item[0].encode())
        group.append((label, len(strings)))
        if in_order:  # the game's own groups are alphabetical; keep them so
            group.sort(key=lambda item: item[0].encode())
        strings.append(text.encode("utf-8") + b"\0")
    new = msbt_write(raw, groups, strings)
    again_groups, again_strings = msbt_read(new)
    index = {name: i for g in again_groups for name, i in g}
    for label, text in additions.items():
        if again_strings[index[label]] != text.encode("utf-8") + b"\0":
            die(f"label {label} does not read back")
    return new


def build_text(game: Game, out: Out) -> None:
    member = "ActorMsg/PouchContent.msbt"
    langs = sorted({p.name for r in game.roots for p in (r / "Mals").glob(f"*.Product.{DATA_VERSION}.sarc.zs")})
    if len(langs) != 16:
        die(f"expected 16 language archives, found {len(langs)}")
    additions = {f"{ITEM}_{key}": value for key, value in TEXT.items()}
    for lang in langs:
        rel = f"Mals/{lang}"
        raw, dict_id = game.load(rel)
        members = sarc_read(raw)
        if sarc_write(dict(members)) != raw:
            die(f"{rel}: this archive writer does not reproduce the game's file")
        files = dict(members)
        old = files[member]
        files[member] = msbt_add(old, additions)
        new = sarc_write(files)
        out.add(rel, game.rezs(new, dict_id), new, raw, dict_id,
                [{"path": member, "uncompressed": len(files[member]), "vanilla_uncompressed": len(old)}])
    print(f"  text: {len(additions)} labels in {member} of {len(langs)} languages (English in all)")


def unit_index(bx: int, by: int, switch2: bool) -> int:
    """Block position in a 512-byte group; the remake swaps x0 and y1 against Switch 1 order."""
    x, y = bx % 4, by % 8
    if switch2:
        return (y & 1) | ((y >> 1) & 1) << 1 | (x & 1) << 2 | ((y >> 2) & 1) << 3 | ((x >> 1) & 1) << 4
    return (y & 1) | (x & 1) << 1 | ((y >> 1) & 1) << 2 | ((y >> 2) & 1) << 3 | ((x >> 1) & 1) << 4


def block_address(bx: int, by: int, cols: int, bh: int, switch2: bool) -> int:
    pitch = -(-cols * 16 // 64)
    group = (by // (8 * bh)) * bh * pitch + (bx // 4) * bh + (by % (8 * bh)) // 8
    return group * 512 + unit_index(bx, by, switch2) * 16


class Icon:
    """A single-texture, single-mip ASTC 4x4 BNTX file, as the game's item icons are."""

    def __init__(self, raw: bytes, what: str):
        if raw[:4] != b"BNTX" or struct.unpack_from("<I", raw, 0x24)[0] != 1:
            die(f"{what}: not a single-texture BNTX file")
        self.raw, self.what = raw, what
        b = self.brti = struct.unpack_from("<Q", raw, struct.unpack_from("<Q", raw, 0x28)[0])[0]
        if raw[b:b + 4] != b"BRTI":
            die(f"{what}: no texture info block")
        mips = struct.unpack_from("<H", raw, b + 0x16)[0]
        fmt = struct.unpack_from("<I", raw, b + 0x1C)[0]
        self.width, self.height, depth, layers, layout = struct.unpack_from("<IIIII", raw, b + 0x24)
        self.size = struct.unpack_from("<I", raw, b + 0x50)[0]
        self.name_at = struct.unpack_from("<Q", raw, b + 0x60)[0]
        self.data_at = struct.unpack_from("<Q", raw, struct.unpack_from("<Q", raw, b + 0x70)[0])[0]
        if (mips, fmt, depth, layers) != (1, 0x2D06, 1, 1):
            die(f"{what}: expected one mip of sRGB ASTC 4x4, found mips {mips}, format {fmt:#x}")
        self.bh = 1 << (layout & 7)
        self.cols, self.rows = -(-self.width // 4), -(-self.height // 4)
        pitch = -(-self.cols * 16 // 64)
        if self.size != pitch * 512 * self.bh * -(-self.rows // (8 * self.bh)):
            die(f"{what}: image size {self.size} does not match its layout")
        n = struct.unpack_from("<H", raw, self.name_at)[0]
        self.name = raw[self.name_at + 2:self.name_at + 2 + n].decode()
        self.dic_at = raw.index(b"_DIC", self.name_at)

    def blocks(self, switch2: bool) -> bytes:
        data = self.raw[self.data_at:self.data_at + self.size]
        out = bytearray()
        for by in range(self.rows):
            for bx in range(self.cols):
                a = block_address(bx, by, self.cols, self.bh, switch2)
                out += data[a:a + 16]
        return bytes(out)

    def pixels(self, switch2: bool) -> np.ndarray:
        """RGBA, padded to whole blocks."""
        w, h = self.cols * 4, self.rows * 4
        bgra = texture2ddecoder.decode_astc(self.blocks(switch2), w, h, 4, 4)
        return np.frombuffer(bgra, np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0, 3]].copy()

    def image(self, switch2: bool) -> Image.Image:
        return Image.fromarray(self.pixels(switch2)[:self.height, :self.width], "RGBA")


def seam_score(px: np.ndarray) -> float:
    """Mean jump across 4-pixel block edges; low when the blocks sit where they belong."""
    p = px.astype(np.int32)
    return float(np.abs(p[4::4] - p[3:-1:4]).mean() + np.abs(p[:, 4::4] - p[:, 3:-1:4]).mean())


def name_ref_bit(name: str) -> int:
    """Lowest set bit of the name counted from its last byte: the BNTX name dictionary's key for a lone entry."""
    data = name.encode()
    for i in range(8 * len(data)):
        if (data[len(data) - 1 - (i >> 3)] >> (i & 7)) & 1:
            return i
    die("empty texture name")


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean())
    return math.inf if mse == 0 else 10 * math.log10(255 * 255 / mse)


def icon_write(template: Icon, art: Image.Image, name: str, astcenc: Path, work: Path) -> bytes:
    if art.size != (template.width, template.height):
        die("icon art size differs from the template")
    padded = Image.new("RGBA", (template.cols * 4, template.rows * 4))
    padded.paste(art, (0, 0))
    src, dst = work / f"{name}.png", work / f"{name}.astc"
    padded.save(src)
    subprocess.run([str(astcenc), "-cs", str(src), str(dst), "4x4", "-thorough", "-silent"], check=True)
    astc = dst.read_bytes()
    dims = (int.from_bytes(astc[7:10], "little"), int.from_bytes(astc[10:13], "little"))
    if astc[:4] != bytes.fromhex("13aba15c") or tuple(astc[4:6]) != (4, 4) or dims != padded.size:
        die("astcenc wrote an unexpected file")
    blocks = astc[16:]
    if len(blocks) != template.cols * template.rows * 16:
        die("astcenc wrote the wrong number of blocks")
    out = bytearray(template.raw)
    for by in range(template.rows):
        for bx in range(template.cols):
            a = template.data_at + block_address(bx, by, template.cols, template.bh, True)
            s = (by * template.cols + bx) * 16
            out[a:a + 16] = blocks[s:s + 16]
    encoded = name.encode()
    end = template.name_at + 2 + len(encoded) + 1
    if end > template.dic_at:
        die(f"texture name {name} does not fit the template's name slot")
    out[template.name_at:template.dic_at] = b"\0" * (template.dic_at - template.name_at)
    struct.pack_into("<H", out, template.name_at, len(encoded))
    out[template.name_at + 2:template.name_at + 2 + len(encoded)] = encoded
    struct.pack_into("<I", out, template.dic_at + 8 + 16, name_ref_bit(name))
    new = Icon(bytes(out), name)
    score = psnr(new.pixels(True)[:template.height, :template.width], np.asarray(art))
    if new.name != name or len(new.raw) != len(template.raw) or score < 32:
        die(f"icon {name} does not read back as the art ({score:.1f} dB)")
    print(f"  icon {name}: {template.width}x{template.height}, reads back at {score:.1f} dB")
    return bytes(out)


def build_icons(game: Game, totk: Game, out: Out, args) -> None:
    pairs = [("UI/Tex/Icon/{}.bntx.zs", ""), ("UI/Tex/IconSh/{}_Sh.bntx.zs", "_Sh")]
    templates = {}
    for pattern, suffix in pairs:
        for actor in (TEMPLATE, "Armor_Adult_HoverBoot", "Item_PocketCucco"):
            raw, dict_id = game.load(pattern.format(actor))
            icon = Icon(raw, pattern.format(actor))
            ref = struct.unpack_from("<I", raw, icon.dic_at + 8 + 16)[0]
            if icon.name != actor + suffix or ref != name_ref_bit(icon.name) or dict_id != 0:
                die(f"{icon.what}: name {icon.name}, dictionary key {ref}, zstd dictionary {dict_id}; not as expected")
            s2, s1 = seam_score(icon.pixels(True)), seam_score(icon.pixels(False))
            if not suffix and s2 > 0.8 * s1:
                die(f"{icon.what}: the Switch 2 block order does not decode it more cleanly ({s2:.2f} vs {s1:.2f})")
            if actor == TEMPLATE:
                templates[suffix] = (icon, raw)
    src_raw, _ = totk.load("UI/Tex/Icon/Parasail.bntx.zs")
    src = Icon(src_raw, "TotK Parasail icon").image(False)
    box = src.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    main, main_raw = templates[""]
    side = round(args.icon_fill * main.width)
    crop = src.crop(box).convert("RGBa")
    scale = side / max(crop.size)
    crop = crop.resize((round(crop.width * scale), round(crop.height * scale)), Image.LANCZOS).convert("RGBA")
    art = Image.new("RGBA", (main.width, main.height))
    art.paste(crop, ((main.width - crop.width) // 2, (main.height - crop.height) // 2))
    shadow, shadow_raw = templates["_Sh"]
    if main.width != main.height or shadow.width != shadow.height:
        die("the template icons are not square")
    # the game's shadow icons: black outline at ~0.55 of the item's size, centred and blurred (five pairs measured)
    small = round(main.width * args.shadow_scale)
    alpha = Image.new("L", (shadow.width, shadow.height))
    alpha.paste(art.getchannel("A").resize((small, small), Image.LANCZOS), ((shadow.width - small) // 2,) * 2)
    alpha = alpha.filter(ImageFilter.GaussianBlur(args.shadow_blur))
    sh_art = Image.new("RGBA", (shadow.width, shadow.height))
    sh_art.putalpha(alpha)
    args.preview.mkdir(parents=True, exist_ok=True)
    art.save(args.preview / f"{ITEM}.png")
    sh_art.save(args.preview / f"{ITEM}_Sh.png")
    with tempfile.TemporaryDirectory() as tmp:
        for (pattern, suffix), template, vanilla, image in ((pairs[0], main, main_raw, art), (pairs[1], shadow, shadow_raw, sh_art)):
            new = icon_write(template, image, ITEM + suffix, args.astcenc, Path(tmp))
            out.add(pattern.format(ITEM), game.rezs(new, 0), new, None, 0, template=pattern.format(TEMPLATE))
            if len(new) != len(vanilla):
                die("icon size differs from the template")


def scan_maps(game: Game) -> tuple[set[int], set[int]]:
    """Every placement id in the game's map files, and the ids of treasure chests."""
    every, chests = set(), set()
    seen = set()
    for root in game.roots:
        for path in sorted((root / "Banc").rglob("*.bcett.byml.zs")):
            rel = path.relative_to(root).as_posix()
            if rel in seen:
                continue
            seen.add(rel)
            tree = gbyml.load(game.load(rel)[0])[2]
            for actor in (tree or {}).get("Actors", []):
                h = actor.get("Hash")
                if isinstance(h, U64):
                    every.add(h.value)
                    if str(actor.get("Gyaml", "")).startswith("Obj_TBox"):
                        chests.add(h.value)
    return every, chests


def hash_of(entry) -> int:
    return entry["Hash"].value


def insert_named(entries: list, entry: dict, name: str, known: dict[int, str], what: str) -> None:
    """Insert where the game's alphabetical order puts `name`, judged from the neighbours whose names are known."""
    named = [(i, known[hash_of(e)]) for i, e in enumerate(entries) if hash_of(e) in known]
    if len(named) < 3:
        die(f"{what}: too few neighbours with known names to place {name}")
    # the game folds letters to upper case ("EyeballFrog" before "EyeDrop"); other orders are tried so a change is reported
    for fold in (lambda s: s.upper().encode(), lambda s: s.encode(), lambda s: s.lower().encode()):
        seq = [fold(n) for _, n in named]
        if seq == sorted(seq):
            at = next((i for i, n in named if fold(n) > fold(name)), named[-1][0] + 1)
            entries.insert(at, entry)
            return
    die(f"{what}: the known names are not in alphabetical order; the list layout is not what this builder expects")


def build_gamedata(game: Game, out: Out, chest_hash: int, chest_ids: set[int], actor_names: list[str]) -> dict:
    rel = f"GameData/GameDataList.Product.{DATA_VERSION}.byml.zs"
    raw, dict_id = game.load(rel)
    version, big, tree = gbyml.load(raw)
    if not big or gbyml.dump(tree, version, big) != raw:
        die(f"{rel}: this BYML writer does not reproduce the game's file")
    data, meta = tree["Data"], tree["MetaData"]
    table_types = [t for t in data if t not in ("Bool64bitKey", "Struct", "BoolExp")]

    def counts():
        per_file = [sum(1 for t in table_types for e in data[t] if e.get("SaveFileIndex") == i) for i in range(4)]
        return per_file, sum(len(data[t]) for t in table_types)

    per_file, total = counts()
    if [0x28 + 8 * (n + 33) for n in per_file] != meta["SaveDataOffsetPos"] or 0x28 + 8 * (total + 33) != meta["AllDataSaveOffset"]:
        die(f"{rel}: the save layout numbers do not follow the formula this builder was checked against")
    data_region = [meta["SaveDataSize"][i] - meta["SaveDataOffsetPos"][i] for i in range(4)]
    all_region = meta["AllDataSaveSize"] - meta["AllDataSaveOffset"]

    def find(kind: str, name: str) -> dict:
        hits = [e for e in data[kind] if hash_of(e) == m3(name)]
        if len(hits) != 1:
            die(f"{rel}: {kind} {name} found {len(hits)} times")
        return hits[0]

    def member(struct_name: str, name: str) -> dict:
        hits = [e for e in find("Struct", struct_name)["DefaultValue"] if hash_of(e) == m3(name)]
        if len(hits) != 1:
            die(f"{rel}: member {name} of {struct_name} found {len(hits)} times")
        return hits[0]

    def clone(entry: dict, **fields) -> dict:
        new = copy.deepcopy(entry)
        for key, value in fields.items():
            new[key] = type(entry[key])(value) if isinstance(entry[key], (U32, U64)) else value
        return new

    existing = {kind: {hash_of(e) for e in data[kind]} for kind in ("Bool", "Int", "Struct", "Bool64bitKey")}
    chest_text = str(chest_hash)
    added = {"Bool": [], "Int": [], "Struct": [], "Bool64bitKey": [], "members": []}

    def add(kind: str, template_name: str, new_name: str, known_prefix: str, known_suffix: str, keys: list[str], **fields):
        if m3(new_name) in existing[kind]:
            die(f"{rel}: {kind} {new_name} already exists (or its hash collides)")
        entry = clone(find(kind, template_name), Hash=m3(new_name), **fields)
        known = {m3(f"{known_prefix}{k}{known_suffix}"): f"{known_prefix}{k}{known_suffix}" for k in keys}
        insert_named(data[kind], entry, new_name, known, f"{kind} list")
        added[kind].append({"name": new_name, "hash": m3(new_name)})

    def add_member(struct_name: str, template_key: str, new_key: str, keys: list[str]):
        members = find("Struct", struct_name)["DefaultValue"]
        if m3(new_key) in {hash_of(e) for e in members}:
            die(f"{rel}: {struct_name} already has member {new_key}")
        entry = clone(member(struct_name, template_key), Hash=m3(new_key), Value=m3(f"{struct_name}.{new_key}"))
        insert_named(members, entry, new_key, {m3(k): k for k in keys}, f"{struct_name} members")
        added["members"].append({"struct": struct_name, "member": new_key, "hash": m3(new_key)})

    chest_keys = [str(h) for h in chest_ids]
    for prefix in ("IsGet", "HavePlayedGetEvent"):
        add("Bool", f"{prefix}.{TEMPLATE}", f"{prefix}.{ITEM}", f"{prefix}.", "", actor_names)
        add_member(prefix, TEMPLATE, ITEM, actor_names)
    add("Int", f"PouchData.{TEMPLATE}.StockNum", f"PouchData.{ITEM}.StockNum", "PouchData.", ".StockNum", actor_names)
    stock = member(f"PouchData.{TEMPLATE}", "StockNum")
    add("Struct", f"PouchData.{TEMPLATE}", f"PouchData.{ITEM}", "PouchData.", "", actor_names,
        DefaultValue=[clone(stock, Value=m3(f"PouchData.{ITEM}.StockNum"))])
    add_member("PouchData", TEMPLATE, ITEM, actor_names)
    add("Bool", f"IsTreasureBoxOpen.{VANILLA_CHEST}", f"IsTreasureBoxOpen.{chest_text}", "IsTreasureBoxOpen.", "", chest_keys)
    add_member("IsTreasureBoxOpen", str(VANILLA_CHEST), chest_text, chest_keys)
    keys64 = data["Bool64bitKey"]
    if [hash_of(e) for e in keys64] != sorted(hash_of(e) for e in keys64) or chest_hash in existing["Bool64bitKey"]:
        die(f"{rel}: the 64-bit key list is not sorted, or already holds the new chest id")
    template64 = [e for e in keys64 if hash_of(e) == VANILLA_CHEST]
    if len(template64) != 1:
        die(f"{rel}: the village's own chest has no 64-bit key entry")
    at = next((i for i, e in enumerate(keys64) if hash_of(e) > chest_hash), len(keys64))
    keys64.insert(at, clone(template64[0], Hash=chest_hash))
    added["Bool64bitKey"].append({"name": chest_text, "hash": chest_hash})

    new_per_file, new_total = counts()
    if [b - a for a, b in zip(per_file, new_per_file)] != [4, 0, 0, 0] or new_total - total != 4:
        die("gamedata: the additions did not land in save file 0 as planned")
    meta["SaveDataOffsetPos"] = [0x28 + 8 * (n + 33) for n in new_per_file]
    meta["SaveDataSize"] = [meta["SaveDataOffsetPos"][i] + data_region[i] + (8 if i == 0 else 0) for i in range(4)]
    meta["AllDataSaveOffset"] = 0x28 + 8 * (new_total + 33)
    meta["AllDataSaveSize"] = meta["AllDataSaveOffset"] + all_region + 8
    new = byml_bytes(tree, version, big)
    out.add(rel, game.rezs(new, dict_id), new, raw, dict_id)
    print(f"  gamedata: +{len(added['Bool'])} Bool, +{len(added['Int'])} Int, +{len(added['Struct'])} Struct, "
          f"+{len(added['members'])} struct members, +1 64-bit key; save file 0 grows 40 bytes")
    added["save_layout"] = {k: meta[k] for k in ("SaveDataOffsetPos", "SaveDataSize", "AllDataSaveOffset", "AllDataSaveSize")}
    return added


def build_chest(game: Game, out: Out, args, chest_hash: int, all_ids: set[int]) -> dict:
    scale = (1.0, 1.0, 1.0)
    rotate = (0.0, args.chest_yaw, 0.0)
    srt_hash = murmur3_32(struct.pack("<9f", *scale, *rotate, *args.chest_pos))
    for need in (args.chest_actor, f"DropKeepAwayArea_{args.chest_actor.removeprefix('Obj_').removesuffix('_NoBake')}"):
        if not game.has(f"Pack/Actor/{need}.pack.zs"):
            die(f"the game has no actor {need}")
    keep_away = f"DropKeepAwayArea_{args.chest_actor.removeprefix('Obj_').removesuffix('_NoBake')}"
    placed = {"chest_hash": chest_hash, "srt_hash": srt_hash, "keep_away": {}, "map": args.chest_map}
    pattern, scenes, template_chest, _, area_label = CHEST_MAPS[args.chest_map]
    for scene in scenes:
        rel = pattern.format(scene)
        raw, dict_id = game.load(rel)
        version, big, tree = gbyml.load(raw)
        actors = tree["Actors"]
        chest = [a for a in actors if a["Gyaml"] == "Obj_TBox_A_01" and a["Hash"].value == template_chest]
        if len(chest) != 1 or set(chest[0]) != {"Dynamic", "Gyaml", "Hash", "Phive", "Rotate", "SRTHash", "Translate"}:
            die(f"{rel}: the template chest is not there in the expected form")
        area = [a for a in actors if a["Gyaml"] == "DropKeepAwayArea_TBox_A_01" and a["Translate"] == chest[0]["Translate"]]
        if len(area) != 1 or set(area[0]) != {"Gyaml", "Hash", "Phive", "Rotate", "SRTHash", "Translate"}:
            die(f"{rel}: the chest's keep-away area is not there in the expected form")
        area_hash = stable_u64(area_label.format(scene))
        for h in (chest_hash, area_hash):
            if h in all_ids:
                die(f"placement id {h} is already used by the game")
        common = {"Rotate": [f32(x) for x in rotate], "Translate": [f32(x) for x in args.chest_pos], "SRTHash": U32(srt_hash)}
        new_chest = copy.deepcopy(chest[0]) | common | {
            "Gyaml": args.chest_actor, "Hash": U64(chest_hash), "Phive": {"Placement": {"ID": U64(chest_hash)}},
            "Dynamic": {"Drop__DropActor": ITEM}}
        new_area = copy.deepcopy(area[0]) | common | {
            "Gyaml": keep_away, "Hash": U64(area_hash), "Phive": {"Placement": {"ID": U64(area_hash)}}}
        if typed(chest[0]["Phive"]) != typed({"Placement": {"ID": U64(template_chest)}}):
            die(f"{rel}: the chest's physics placement record is not just its id")
        actors += [new_chest] if args.no_keep_away else [new_chest, new_area]
        new = byml_bytes(tree, version, big)
        back = gbyml.load(new)[2]["Actors"]
        if len(back) != len(actors) or typed(back[:len(actors) - (1 if args.no_keep_away else 2)]) != typed(gbyml.load(raw)[2]["Actors"]):
            die(f"{rel}: the game's own placements changed")
        out.add(rel, game.rezs(new, dict_id), new, raw, dict_id)
        placed["keep_away"][scene] = area_hash
    print(f"  chest: {args.chest_actor} at {tuple(args.chest_pos)}, yaw {args.chest_yaw} rad, id {chest_hash}, in {len(scenes)} {args.chest_map} scene files")
    return placed


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", type=Path, required=True, help="the main checkout's work/switch2/oot-remake folder")
    ap.add_argument("--game-romfs", type=Path, action="append", help="extracted 2.0.0 romfs (repeatable; default <work>/extract/romfs)")
    ap.add_argument("--totk-romfs", type=Path, required=True, help="extracted Tears of the Kingdom romfs (icon source)")
    ap.add_argument("--out", type=Path, help="output romfs folder (default <work>/build/paraglider/romfs)")
    ap.add_argument("--astcenc", type=Path, help="astcenc executable (default vendor/external-tools/astcenc/astcenc-avx2.exe)")
    ap.add_argument("--model-project", default="Obj_Paraglider", help="model file project name shown in the item-get sequence")
    ap.add_argument("--model-fmdb", default="Obj_Paraglider", help="model name inside that file")
    ap.add_argument("--item-anim", action="append", help="animation resource of the item actor (repeatable; default Item_Common_Animation)")
    ap.add_argument("--demo-scale", type=float, default=0.25, help="DebugModelScale of the item actor; 1 leaves the key out")
    ap.add_argument("--model-aabb", type=float, nargs=6, default=[-1.5, -1.0, -1.5, 1.5, 1.5, 1.5], metavar="F")
    ap.add_argument("--instance-heap", type=int, default=1000000, help="ActorInfo InstanceHeapSize of the item actor")
    ap.add_argument("--render-heap", type=int, default=60000, help="ActorInfo PreActorRenderInfoHeapSize of the item actor")
    ap.add_argument("--rsdb-extra", type=Path, default=HERE / "rsdb_extra.json", help="more RSDB rows to add (see ITEM-CHEST.md)")
    ap.add_argument("--chest-actor", default="Obj_TBox_A_02", help="large chest actor")
    ap.add_argument("--chest-map", choices=sorted(CHEST_MAPS), default="hyrule-field", help="map files the chest goes in")
    ap.add_argument("--chest-pos", type=float, nargs=3, default=SPOT_POS, metavar=("X", "Y", "Z"))
    ap.add_argument("--chest-yaw", type=float, default=SPOT_YAW, help="radians; the lid opens toward (sin yaw, cos yaw) in X, Z")
    ap.add_argument("--no-keep-away", action="store_true", help="do not place the chest's drop keep-away area")
    ap.add_argument("--icon-fill", type=float, default=0.8, help="longest side of the art as a fraction of the icon")
    ap.add_argument("--shadow-scale", type=float, default=0.55, help="item size in the shadow icon, as a fraction of its pixel size in the main icon")
    ap.add_argument("--shadow-blur", type=float, default=5.0, help="blur radius of the icon shadow, pixels")
    ap.add_argument("--allow-missing-model", action="store_true")
    args = ap.parse_args()

    args.out = args.out or args.work / "build" / "paraglider" / "romfs"
    args.preview = args.out.parent / "preview"
    args.item_anim = args.item_anim or ["Item_Common_Animation"]
    roots = args.game_romfs or [args.work / "extract" / "romfs"]
    if not args.astcenc:
        tail = Path("vendor/external-tools/astcenc/astcenc-avx2.exe")
        args.astcenc = next((p for p in (HERE.parents[4] / tail, args.work.parents[2] / tail) if p.is_file()), None)
    if not args.astcenc or not args.astcenc.is_file():
        die("astcenc not found; pass --astcenc")
    game, totk = Game(roots, "game"), Game([args.totk_romfs], "TotK")
    for anim in args.item_anim:
        if not game.has(f"Model/{anim}.anim.bfres.zs") and not (args.out / "Model" / f"{anim}.anim.bfres.zs").is_file():
            print(f"WARNING: animation resource {anim} has no Model/{anim}.anim.bfres.zs in the game or the output folder")

    out = Out()
    print("item actor")
    build_item_pack(game, out, args)
    print("tables")
    build_rsdb(game, out, args)
    print("text")
    build_text(game, out)
    print("icons")
    build_icons(game, totk, out, args)
    print("saved flags and chest")
    all_ids, chest_ids = scan_maps(game)
    chest_hash = stable_u64(CHEST_MAPS[args.chest_map][3])
    actor_names = [row["__RowId"] for row in gbyml.load(game.load(f"RSDB/ActorInfo.Product.{RSDB_VERSION}.rstbl.byml.zs")[0])[2]]
    flags = build_gamedata(game, out, chest_hash, chest_ids, actor_names)
    placed = build_chest(game, out, args, chest_hash, all_ids)

    for rel, blob in out.files.items():
        path = args.out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    manifest = {
        "item": ITEM, "ownership_flag": {"type": "Bool", "name": f"IsGet.{ITEM}", "murmur3_32": m3(f"IsGet.{ITEM}")},
        "stock_flag": {"type": "Int", "name": f"PouchData.{ITEM}.StockNum", "murmur3_32": m3(f"PouchData.{ITEM}.StockNum")},
        "chest_flag": {"type": "Bool", "name": f"IsTreasureBoxOpen.{chest_hash}", "murmur3_32": m3(f"IsTreasureBoxOpen.{chest_hash}")},
        "chest": placed | {"actor": args.chest_actor, "translate": args.chest_pos, "yaw": args.chest_yaw},
        "gamedata_added": flags, "files": out.rows,
    }
    (args.out.parent / "data_manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")

    print(f"\nwrote {len(out.files)} files under {args.out}")
    print(f"{'uncompressed':>12} {'vanilla':>10} {'grows':>7}  file")
    for row in out.rows:
        old = row["vanilla_uncompressed"]
        print(f"{row['uncompressed']:>12} {'new' if old is None else old:>10} {'' if old is None else row['uncompressed'] - old:>7}  {row['path']}")
        for inner in row["inner"]:
            if inner.get("new_name") or "vanilla_uncompressed" in inner:
                was = f" (was {inner['vanilla_uncompressed']})" if "vanilla_uncompressed" in inner else " (new name)"
                print(f"{inner['uncompressed']:>12} {'':>10} {'':>7}    inside: {inner['path']}{was}")
    for key in ("ownership_flag", "stock_flag", "chest_flag"):
        flag = manifest[key]
        print(f"{key}: {flag['type']} {flag['name']}  murmur3_32 = {flag['murmur3_32']} (0x{flag['murmur3_32']:08X})")


if __name__ == "__main__":
    main()
