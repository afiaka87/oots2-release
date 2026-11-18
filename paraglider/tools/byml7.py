"""Read and write BYML version 7 (little-endian "YB"), as used by the Switch 2 Ocarina of Time remake's map
placement files (Banc/**/*.bcett.byml).

byml_dump.py prints a file; this module keeps every value's stored type so a file can be edited and written back.
oead 1.2.9 rejects version 7.

Supported node types are the ones the remake's map files use: hash (0xC1), array (0xC0), string (0xA0),
bool (0xD0), int (0xD1), float (0xD2), uint (0xD3), int64 (0xD4), uint64 (0xD5), double (0xD6) and null (0xFF).
Anything else raises ValueError rather than being dropped.

Layout written: header, key table, string table, a pool of the 8-byte values, then the containers depth-first.
Hash entries are sorted by key, and both string tables are sorted bytewise, as the game's own files are.

  uv run --no-project python switch2/tools/byml7.py roundtrip FILE.byml [FILE...]   (uncompressed files)
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class F32:
    bits: int

    @property
    def value(self) -> float:
        return struct.unpack("<f", struct.pack("<I", self.bits))[0]


@dataclass(frozen=True)
class F64:
    bits: int


@dataclass(frozen=True)
class U32:
    value: int


@dataclass(frozen=True)
class S64:
    value: int


@dataclass(frozen=True)
class U64:
    value: int


def load(data: bytes):
    """Decode a little-endian BYML file into dict/list/str/bool/int/None plus the wrapper types above."""
    if data[:2] != b"YB":
        raise ValueError("not a little-endian BYML file")
    version, keys_off, strings_off, root_off = struct.unpack_from("<HIII", data, 2)

    def table(off: int) -> list[str]:
        if not off:
            return []
        if data[off] != 0xC2:
            raise ValueError(f"string table at 0x{off:X} has type 0x{data[off]:02X}")
        count = int.from_bytes(data[off + 1:off + 4], "little")
        offs = struct.unpack_from(f"<{count + 1}I", data, off + 4)
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
            return struct.unpack("<i", struct.pack("<I", raw))[0]
        if kind == 0xD2:
            return F32(raw)
        if kind == 0xD3:
            return U32(raw)
        if kind == 0xD4:
            return S64(struct.unpack_from("<q", data, raw)[0])
        if kind == 0xD5:
            return U64(struct.unpack_from("<Q", data, raw)[0])
        if kind == 0xD6:
            return F64(struct.unpack_from("<Q", data, raw)[0])
        if kind == 0xFF:
            return None
        raise ValueError(f"unsupported value type 0x{kind:02X}")

    def node(off: int):
        kind = data[off]
        count = int.from_bytes(data[off + 1:off + 4], "little")
        if kind == 0xC1:
            out = {}
            for i in range(count):
                e = off + 4 + 8 * i
                out[keys[int.from_bytes(data[e:e + 3], "little")]] = value(data[e + 3], struct.unpack_from("<I", data, e + 4)[0])
            return out
        if kind == 0xC0:
            base = off + 4 + ((count + 3) & ~3)
            return [value(data[off + 4 + i], struct.unpack_from("<I", data, base + 4 * i)[0]) for i in range(count)]
        raise ValueError(f"unsupported node type 0x{kind:02X} at 0x{off:X}")

    return version, (node(root_off) if root_off else None)


def _kind(v) -> int:
    if isinstance(v, dict):
        return 0xC1
    if isinstance(v, list):
        return 0xC0
    if isinstance(v, str):
        return 0xA0
    if isinstance(v, bool):
        return 0xD0
    if isinstance(v, int):
        return 0xD1
    if v is None:
        return 0xFF
    kinds = {F32: 0xD2, U32: 0xD3, S64: 0xD4, U64: 0xD5, F64: 0xD6}
    if type(v) in kinds:
        return kinds[type(v)]
    raise ValueError(f"cannot write {type(v).__name__}")


def _string_table(strings: list[str]) -> bytes:
    encoded = [s.encode("utf-8") + b"\0" for s in strings]
    head = 4 + 4 * (len(encoded) + 1)
    offs, pos = [], head
    for e in encoded:
        offs.append(pos)
        pos += len(e)
    offs.append(pos)
    out = bytes([0xC2]) + len(encoded).to_bytes(3, "little") + struct.pack(f"<{len(offs)}I", *offs) + b"".join(encoded)
    return out + b"\0" * (-len(out) % 4)


def dump(root, version: int = 7) -> bytes:
    """Encode a tree from load() (or built from the same types) as little-endian BYML."""
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

    out = bytearray(16)
    keys_off = len(out) if key_list else 0
    if key_list:
        out += _string_table(key_list)
    strings_off = len(out) if str_list else 0
    if str_list:
        out += _string_table(str_list)
    wide_at = {}
    for v in wide:  # one 8-byte slot per distinct value, in first-use order
        if v not in wide_at:
            wide_at[v] = len(out)
            out += struct.pack("<Q", v.bits if isinstance(v, F64) else v.value & 0xFFFFFFFFFFFFFFFF)

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

    written: dict = {}  # identical containers are stored once, as in the game's files

    def write(v) -> int:
        """Write container v, then its child containers; return v's offset."""
        ident = typed(v)
        if ident in written:
            return written[ident]
        at = written[ident] = len(out)
        if isinstance(v, dict):
            items = sorted(v.items(), key=lambda kv: key_ix[kv[0]])
            out.extend(bytes([0xC1]) + len(items).to_bytes(3, "little") + b"\0" * (8 * len(items)))
            for i, (key, child) in enumerate(items):
                k = _kind(child)
                raw = write(child) if k in (0xC0, 0xC1) else scalar(child)
                struct.pack_into("<I", out, at + 4 + 8 * i, key_ix[key] | (k << 24))
                struct.pack_into("<I", out, at + 8 + 8 * i, raw)
        else:
            n = len(v)
            types = bytes(_kind(c) for c in v)
            base = at + 4 + ((n + 3) & ~3)
            out.extend(bytes([0xC0]) + n.to_bytes(3, "little") + types + b"\0" * (-n % 4) + b"\0" * (4 * n))
            for i, child in enumerate(v):
                k = types[i]
                struct.pack_into("<I", out, base + 4 * i, write(child) if k in (0xC0, 0xC1) else scalar(child))
        return at

    root_off = write(root) if root is not None else 0
    struct.pack_into("<2sHIII", out, 0, b"YB", version, keys_off, strings_off, root_off)
    return bytes(out)


def typed(v):
    """Tree with every value tagged by its stored type, for comparisons where True must not equal 1."""
    if isinstance(v, dict):
        return ("hash", tuple(sorted((k, typed(c)) for k, c in v.items())))
    if isinstance(v, list):
        return ("array", tuple(typed(c) for c in v))
    return (_kind(v), v)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 3 or sys.argv[1] != "roundtrip":
        print(__doc__)
        return 2
    bad = 0
    for name in sys.argv[2:]:
        data = Path(name).read_bytes()
        version, tree = load(data)
        again = dump(tree, version)
        v2, tree2 = load(again)
        ok = v2 == version and typed(tree2) == typed(tree)
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} {name}: {len(data)} -> {len(again)} bytes")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
