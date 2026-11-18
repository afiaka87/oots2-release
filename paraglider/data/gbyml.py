"""BYML version 7 reader and writer for both byte orders (GameDataList is big-endian "BY").

Values keep their stored type through switch2/tools/byml7.py's wrapper classes; `roundtrip` checks files byte for byte.

  uv run --no-project python switch2/mods/oot-remake/paraglider/data/gbyml.py roundtrip FILE.byml [FILE...]
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools"))
from byml7 import F32, F64, S64, U32, U64, _kind, typed  # noqa: E402


def load(data: bytes):
    """Decode a BYML file of either byte order. Returns (version, big_endian, tree)."""
    if data[:2] == b"YB":
        big = False
    elif data[:2] == b"BY":
        big = True
    else:
        raise ValueError("not a BYML file")
    e = ">" if big else "<"
    order = "big" if big else "little"
    version, keys_off, strings_off, root_off = struct.unpack_from(e + "HIII", data, 2)

    def table(off: int) -> list[str]:
        if not off:
            return []
        if data[off] != 0xC2:
            raise ValueError(f"string table at 0x{off:X} has type 0x{data[off]:02X}")
        count = int.from_bytes(data[off + 1:off + 4], order)
        offs = struct.unpack_from(f"{e}{count + 1}I", data, off + 4)
        return [data[off + offs[i]:data.index(b"\0", off + offs[i])].decode("utf-8") for i in range(count)]

    keys, strings = table(keys_off), table(strings_off)

    def value(kind: int, raw: int):
        if kind in (0xC0, 0xC1):
            return node(raw)
        if kind == 0xA0:
            return strings[raw]
        if kind == 0xD0:
            return bool(raw)
        if kind == 0xD1:
            return raw - (1 << 32) if raw & 0x80000000 else raw
        if kind == 0xD2:
            return F32(raw)
        if kind == 0xD3:
            return U32(raw)
        if kind == 0xD4:
            return S64(struct.unpack_from(e + "q", data, raw)[0])
        if kind == 0xD5:
            return U64(struct.unpack_from(e + "Q", data, raw)[0])
        if kind == 0xD6:
            return F64(struct.unpack_from(e + "Q", data, raw)[0])
        if kind == 0xFF:
            return None
        raise ValueError(f"unsupported value type 0x{kind:02X}")

    def node(off: int):
        kind = data[off]
        count = int.from_bytes(data[off + 1:off + 4], order)
        if kind == 0xC1:
            out = {}
            for i in range(count):
                at = off + 4 + 8 * i
                if big:
                    key, vkind = int.from_bytes(data[at:at + 3], "big"), data[at + 3]
                else:
                    key, vkind = int.from_bytes(data[at:at + 3], "little"), data[at + 3]
                out[keys[key]] = value(vkind, struct.unpack_from(e + "I", data, at + 4)[0])
            return out
        if kind == 0xC0:
            base = off + 4 + ((count + 3) & ~3)
            return [value(data[off + 4 + i], struct.unpack_from(e + "I", data, base + 4 * i)[0]) for i in range(count)]
        raise ValueError(f"unsupported node type 0x{kind:02X} at 0x{off:X}")

    return version, big, (node(root_off) if root_off else None)


def dump(root, version: int = 7, big: bool = False) -> bytes:
    """Encode a tree from load() (or built from the same types)."""
    e = ">" if big else "<"
    order = "big" if big else "little"
    keys, strings, wide = set(), set(), []

    def collect(v):
        k = _kind(v)
        if k == 0xC1:
            for key, child in v.items():
                keys.add(key)
                collect(child)
        elif k == 0xC0:
            for child in v:
                collect(child)
        elif k == 0xA0:
            strings.add(v)
        elif k in (0xD4, 0xD5, 0xD6):
            wide.append(v)

    collect(root)
    key_list = sorted(keys, key=lambda s: s.encode("utf-8"))
    str_list = sorted(strings, key=lambda s: s.encode("utf-8"))
    key_ix = {s: i for i, s in enumerate(key_list)}
    str_ix = {s: i for i, s in enumerate(str_list)}

    def string_table(items: list[str]) -> bytes:
        encoded = [s.encode("utf-8") + b"\0" for s in items]
        offs, pos = [], 4 + 4 * (len(encoded) + 1)
        for item in encoded:
            offs.append(pos)
            pos += len(item)
        offs.append(pos)
        out = bytes([0xC2]) + len(encoded).to_bytes(3, order) + struct.pack(f"{e}{len(offs)}I", *offs) + b"".join(encoded)
        return out + b"\0" * (-len(out) % 4)

    out = bytearray(16)
    keys_off = len(out) if key_list else 0
    if key_list:
        out += string_table(key_list)
    strings_off = len(out) if str_list else 0
    if str_list:
        out += string_table(str_list)
    wide_at = {}
    for v in wide:
        if v not in wide_at:
            wide_at[v] = len(out)
            out += struct.pack(e + "Q", v.bits if isinstance(v, F64) else v.value & 0xFFFFFFFFFFFFFFFF)

    def scalar(v) -> int:
        k = _kind(v)
        if k == 0xA0:
            return str_ix[v]
        if k == 0xD0:
            return int(v)
        if k == 0xD1:
            if not -(1 << 31) <= v < (1 << 31):
                raise ValueError(f"int {v} does not fit 32 bits")
            return v & 0xFFFFFFFF
        if k == 0xD2:
            return v.bits
        if k == 0xD3:
            return v.value
        if k in (0xD4, 0xD5, 0xD6):
            return wide_at[v]
        if k == 0xFF:
            return 0
        raise AssertionError(k)

    written: dict = {}

    def write(v) -> int:
        ident = typed(v)
        if ident in written:
            return written[ident]
        at = written[ident] = len(out)
        if isinstance(v, dict):
            items = sorted(v.items(), key=lambda kv: key_ix[kv[0]])
            out.extend(bytes([0xC1]) + len(items).to_bytes(3, order) + b"\0" * (8 * len(items)))
            for i, (key, child) in enumerate(items):
                k = _kind(child)
                raw = write(child) if k in (0xC0, 0xC1) else scalar(child)
                out[at + 4 + 8 * i:at + 7 + 8 * i] = key_ix[key].to_bytes(3, order)
                out[at + 7 + 8 * i] = k
                struct.pack_into(e + "I", out, at + 8 + 8 * i, raw)
        else:
            n = len(v)
            types = bytes(_kind(c) for c in v)
            base = at + 4 + ((n + 3) & ~3)
            out.extend(bytes([0xC0]) + n.to_bytes(3, order) + types + b"\0" * (-n % 4) + b"\0" * (4 * n))
            for i, child in enumerate(v):
                k = types[i]
                struct.pack_into(e + "I", out, base + 4 * i, write(child) if k in (0xC0, 0xC1) else scalar(child))
        return at

    root_off = write(root) if root is not None else 0
    struct.pack_into(e + "2sHIII", out, 0, b"BY" if big else b"YB", version, keys_off, strings_off, root_off)
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] != "roundtrip":
        print(__doc__)
        return 2
    bad = 0
    for name in sys.argv[2:]:
        data = Path(name).read_bytes()
        version, big, tree = load(data)
        again = dump(tree, version, big)
        same_tree = typed(load(again)[2]) == typed(tree)
        exact = again == data
        bad += not same_tree
        print(f"{'exact' if exact else 'same tree' if same_tree else 'BAD'} {name}: {len(data)} -> {len(again)} bytes")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
