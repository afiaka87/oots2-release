"""Shared builder for the Switch 2 Ocarina of Time remake (title 0400486024DEC000) code patches.

Every mod in this folder is a set of instruction-word replacements in `main`, written as a pchtxt file for
the emulator's exefs patch loader. A builder script creates a `PatchKit`, declares each edit with the
vanilla word it expects, and calls `write()`. The kit refuses to write when:

  - the NSO header's build ID is not the one these offsets were mapped against;
  - a patched word does not hold its expected vanilla value;
  - a cave (added code) is not a reserved run of zero words between two functions;
  - two edits overlap, or a cave belongs to another mod in CAVES;
  - an encoded instruction does not disassemble to the intended text (needs `--with capstone`).

It then parses its own pchtxt the way the yuzu/Eden IPSwitch loader does, applies it to a copy of the NSO
header plus the program image, and checks that exactly the intended bytes changed. An IPS32 file with the
same records is written next to it as a fallback (IPS32 is the format already proven in Lichii); it lives
outside `exefs/`, so the emulator never loads both.

Offsets in this kit are flat-image offsets (memory offsets of `main`). pchtxt lines use the same offsets
with `@flag offset_shift 0x100`, because the loader patches the 0x100-byte NSO header plus the image.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
WORK = REPO / "work" / "switch2" / "oot-remake"

BUILD_ID = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
HEADER = 0x100
TEXT_SIZE = 0x3C461D0

PACIBSP = 0xD503237F
RET = 0xD65F03C0
NOP = 0xD503201F

# Reserved caves: 7-word zero runs that sit between a function's last instruction and the next function's
# pacibsp, with no branch anywhere in the image targeting them (work/switch2/oot-remake/re/caves.py lists
# 1023 such runs). Each mod owns its own caves so the whole pack can be enabled together.
CAVES: dict[str, list[int]] = {
    "link-speed": [0x934384],  # three words; built by link-speed/build_patch.py
    "moon-jump": [0x1E07404, 0x1E0AA24, 0x1E15C24, 0x1E18704, 0x1E19EC4, 0x1E1C264,
                  0x1E90D04, 0x1E91444, 0x1E969E4, 0x1E9C6C4, 0x1EA3E44, 0x1EA7E64, 0x1EAF084,
                  0x1EB25A4],  # last eight added 2026-11-06 (inlined reads, jump attack)
    "faster-movement": [0x1E1E384, 0x1E23E44, 0x1E284C4, 0x1E2C9A4, 0x1E304C4, 0x1E32184, 0x1E34BC4,
                        0x1E81B04, 0x1E8F384, 0x1E901C4],  # last three added 2026-11-06 (Link's animation sites)
    "faster-movement-extra": [0x1E36264, 0x1E6A8C4, 0x1E73F04],
    "giant-link": [0x1E37A84, 0x1E47824],
    "tiny-link": [0x1E528C4, 0x1E543C4],
    "climb-anything": [0x1E57864, 0x1E5BFA4, 0x1E5F9A4],
}
CAVE_WORDS = 7


def _is_function_end(w: int) -> bool:
    return w in (RET, 0xD65F0BFF, 0xD65F0FFF) or (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) == 0xD61F0000


# ---------------------------------------------------------------- instruction encoders
# Each returns (word, text); `text` is what capstone prints for the word and is checked at build time.

@dataclass(frozen=True)
class Insn:
    word: int
    text: str


def ldr_s(t: int, n: int, imm: int = 0) -> Insn:
    assert imm % 4 == 0 and 0 <= imm < 0x4000
    text = f"ldr s{t}, [x{n}]" if imm == 0 else f"ldr s{t}, [x{n}, #0x{imm:x}]"
    return Insn(0xBD400000 | (imm // 4) << 10 | n << 5 | t, text)


def ldr_x(t: int, n: int, imm: int = 0) -> Insn:
    assert imm % 8 == 0 and 0 <= imm < 0x8000
    text = f"ldr x{t}, [x{n}]" if imm == 0 else f"ldr x{t}, [x{n}, #0x{imm:x}]"
    return Insn(0xF9400000 | (imm // 8) << 10 | n << 5 | t, text)


def fmov_imm(d: int, value: float) -> Insn:
    """FMOV (scalar, immediate); only values of the form n/16 * 2^k, 16 <= n < 32, -3 <= k <= 4."""
    for imm8 in range(256):
        sign = imm8 >> 7
        b6 = (imm8 >> 6) & 1
        exp = ((1 - b6) << 7) | ((0b11111 if b6 else 0) << 2) | ((imm8 >> 4) & 3)
        bits = (sign << 31) | (exp << 23) | ((imm8 & 0xF) << 19)
        if struct.unpack("<f", struct.pack("<I", bits))[0] == value:
            return Insn(0x1E201000 | imm8 << 13 | d, f"fmov s{d}, #{value:.8f}")
    raise ValueError(f"{value} is not an FMOV immediate")


_FOPS = {"fmul": 0x1E200800, "fdiv": 0x1E201800, "fadd": 0x1E202800, "fsub": 0x1E203800}


def fop(op: str, d: int, n: int, m: int) -> Insn:
    return Insn(_FOPS[op] | m << 16 | n << 5 | d, f"{op} s{d}, s{n}, s{m}")


def ret() -> Insn:
    return Insn(RET, "ret")


def nop() -> Insn:
    return Insn(NOP, "nop")


def mov_w(d: int, imm: int) -> Insn:
    assert 0 <= imm < 0x10000
    return Insn(0x52800000 | imm << 5 | d, f"mov w{d}, #{imm}" if imm < 10 else f"mov w{d}, #0x{imm:x}")


def _branch_imm(site: int, target: int, bits: int) -> int:
    delta = target - site
    if delta % 4 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f"branch from 0x{site:X} to 0x{target:X} out of range")
    return (delta >> 2) & ((1 << bits) - 1)


def bl(site: int, target: int) -> Insn:
    return Insn(0x94000000 | _branch_imm(site, target, 26), f"bl #0x{target:x}")


def b(site: int, target: int) -> Insn:
    return Insn(0x14000000 | _branch_imm(site, target, 26), f"b #0x{target:x}")


def cbz_x(t: int, site: int, target: int) -> Insn:
    return Insn(0xB4000000 | _branch_imm(site, target, 19) << 5 | t, f"cbz x{t}, #0x{target:x}")


def cbnz_x(t: int, site: int, target: int) -> Insn:
    return Insn(0xB5000000 | _branch_imm(site, target, 19) << 5 | t, f"cbnz x{t}, #0x{target:x}")


def ldp_s(t: int, t2: int, n: int, imm: int) -> Insn:
    assert imm % 4 == 0 and -256 <= imm < 256
    return Insn(0x2D400000 | ((imm // 4) & 0x7F) << 15 | t2 << 10 | n << 5 | t,
                f"ldp s{t}, s{t2}, [x{n}, #0x{imm:x}]")


def str_x(t: int, n: int, imm: int = 0) -> Insn:
    assert imm % 8 == 0 and 0 <= imm < 0x8000
    text = f"str x{t}, [x{n}]" if imm == 0 else f"str x{t}, [x{n}, #0x{imm:x}]"
    return Insn(0xF9000000 | (imm // 8) << 10 | n << 5 | t, text)


def mov_x(d: int, m: int) -> Insn:
    return Insn(0xAA0003E0 | m << 16 | d, f"mov x{d}, x{m}")


def lsr_x(d: int, n: int, shift: int) -> Insn:
    """LSR (immediate), the UBFM alias."""
    assert 0 < shift < 64
    return Insn(0xD340FC00 | shift << 16 | n << 5 | d, f"lsr x{d}, x{n}, #0x{shift:x}")


def _logical_imm64(value: int) -> int:
    """N:immr:imms field (13 bits, placed at bit 10) for a 64-bit logical immediate."""
    for n in (1, 0):
        for imms in range(64):
            for immr in range(64):
                length = ((n << 6) | (~imms & 0x3F)).bit_length() - 1
                if length < 1:
                    continue
                esize = 1 << length
                s, r = imms & (esize - 1), immr & (esize - 1)
                if s == esize - 1:
                    continue
                elem = (1 << (s + 1)) - 1
                elem = ((elem >> r) | (elem << (esize - r))) & ((1 << esize) - 1)
                full = 0
                for k in range(0, 64, esize):
                    full |= elem << k
                if full == value:
                    return (n << 12 | immr << 6 | imms) << 10
    raise ValueError(f"0x{value:x} is not a 64-bit logical immediate")


def tst_x(n: int, imm: int) -> Insn:
    return Insn(0xF2000000 | _logical_imm64(imm) | n << 5 | 31, f"tst x{n}, #0x{imm:x}")


def orr_x(d: int, n: int, imm: int) -> Insn:
    return Insn(0xB2000000 | _logical_imm64(imm) | n << 5 | d, f"orr x{d}, x{n}, #0x{imm:x}")


_CONDS = {"eq": 0, "ne": 1, "hs": 2, "lo": 3, "mi": 4, "pl": 5, "hi": 8, "ls": 9, "ge": 10, "lt": 11, "gt": 12, "le": 13}


def b_cond(cond: str, site: int, target: int) -> Insn:
    return Insn(0x54000000 | _branch_imm(site, target, 19) << 5 | _CONDS[cond], f"b.{cond} #0x{target:x}")


def tbnz(t: int, bit: int, site: int, target: int) -> Insn:
    """TBNZ on bit `bit` of x`t` (capstone names the register w`t` for bits below 32)."""
    reg = f"x{t}" if bit >= 32 else f"w{t}"
    return Insn(0x37000000 | (bit >> 5) << 31 | (bit & 0x1F) << 19 | _branch_imm(site, target, 14) << 5 | t,
                f"tbnz {reg}, #0x{bit:x}, #0x{target:x}" if bit >= 10 else f"tbnz {reg}, #{bit}, #0x{target:x}")


# ---------------------------------------------------------------- patch kit

@dataclass
class PatchKit:
    mod: str
    title: str
    nso: Path = WORK / "main.uncompressed.nso"
    image_path: Path = WORK / "main.bin"
    edits: dict[int, tuple[int, int, str | None]] = field(default_factory=dict)  # addr -> (old, new, disassembly)
    notes: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        header = self.nso.read_bytes()[:HEADER]
        if header[:4] != b"NSO0":
            raise SystemExit(f"{self.nso}: not an NSO0 header")
        build_id = header[0x40:0x60].hex().upper().rstrip("0")
        if build_id != BUILD_ID:
            raise SystemExit(f"build ID {build_id}, expected {BUILD_ID}: the offsets belong to another build")
        self.header = header
        self.image = self.image_path.read_bytes()
        self._caves_free = list(CAVES.get(self.mod, []))
        self._caves_used: dict[object, int] = {}

    # -- reading
    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.image, addr)[0]

    def expect(self, addr: int, value: int, what: str) -> None:
        if self.word(addr) != value:
            raise SystemExit(f"vanilla check failed at 0x{addr:X} ({what}): 0x{self.word(addr):08X} != 0x{value:08X}")

    # -- writing
    def put(self, addr: int, insn: Insn, vanilla: int | None = None) -> None:
        self._put_word(addr, insn.word, insn.text, vanilla)

    def put_data(self, addr: int, word: int, vanilla: int) -> None:
        self._put_word(addr, word, None, vanilla)

    def _put_word(self, addr: int, word: int, text: str | None, vanilla: int | None) -> None:
        if addr in self.edits:
            raise SystemExit(f"two edits at 0x{addr:X}")
        old = self.word(addr)
        if vanilla is not None and old != vanilla:
            raise SystemExit(f"vanilla check failed at 0x{addr:X}: 0x{old:08X} != 0x{vanilla:08X}")
        self.edits[addr] = (old, word, text)

    def cave(self, key: object, build) -> int:
        """Place added code in the next free reserved cave. `build(addr)` returns the instruction list.
        Calls with the same key share one cave."""
        if key in self._caves_used:
            return self._caves_used[key]
        if not self._caves_free:
            raise SystemExit(f"{self.mod}: out of reserved caves (add more to CAVES)")
        addr = self._caves_free.pop(0)
        for other, caves in CAVES.items():
            if other != self.mod and addr in caves:
                raise SystemExit(f"cave 0x{addr:X} also belongs to {other}")
        words = build(addr)
        size = 3 if self.mod == "link-speed" else CAVE_WORDS
        if len(words) > size:
            raise SystemExit(f"cave code at 0x{addr:X} is {len(words)} words, cave holds {size}")
        if self.image[addr:addr + 4 * size] != bytes(4 * size):
            raise SystemExit(f"cave 0x{addr:X} is not {size} zero words")
        if not _is_function_end(self.word(addr - 4)) or self.word(addr + 4 * size) != PACIBSP:
            raise SystemExit(f"cave 0x{addr:X} is not padding between two functions")
        for i, insn in enumerate(words):
            self.put(addr + 4 * i, insn, vanilla=0)
        self._caves_used[key] = addr
        return addr

    def detour_param_load(self, site: int, getter: int, op: str, const: float) -> None:
        """Scale one parameter read. `site` holds `ldr sD, [x0]` right after `bl getter` (optionally with one
        unrelated `adrp`/`ldr x` between them). The load becomes `bl cave`; the cave reloads the value, applies `op const`
        through s16 and returns. After the getter call x30, x9-x17 and v16-v31 hold nothing the caller uses."""
        w = self.word(site)
        if (w & 0xFFFFFFE0) != 0xBD400000:
            raise SystemExit(f"0x{site:X}: expected `ldr sD, [x0]`, found 0x{w:08X}")
        d = w & 0x1F
        call = site - 4
        between = self.word(call)
        # One unrelated integer load may sit between the call and the read: `adrp xN` or `ldr xN, [xM, #imm]`.
        # It must not overwrite x0 (the parameter pointer) or x30; the cave touches neither, nor any x register.
        if (between & 0x9F000000) == 0x90000000 or (between & 0xFFC00000) == 0xF9400000:
            if (between & 0x1F) in (0, 30):
                raise SystemExit(f"0x{call:X}: the instruction between the call and the read writes x0 or x30")
            call -= 4
        if self.word(call) != bl(call, getter).word:
            raise SystemExit(f"0x{site:X}: the instruction before is not `bl 0x{getter:X}`")
        self.check_scratch_dead(site, 16)
        cave = self.cave((op, const, d), lambda a: [ldr_s(d, 0), fmov_imm(16, const), fop(op, d, d, 16), ret()])
        self.put(site, bl(site, cave), vanilla=w)

    def detour_inline_load(self, site: int, op: str, const: float) -> None:
        """Scale an inlined parameter read `ldr sD, [xB, #imm]`: the getter's "value is set locally" path that
        the compiler copied into the caller (added 2026-11-06 for Moon Jump, after boot 1 showed that patching
        only the getter call misses it). The load becomes `bl cave`; the cave repeats the load, applies
        `op const` through s16 and returns. Refuses unless the enclosing function saves x30 in its prologue."""
        w = self.word(site)
        if (w & 0xFFC00000) != 0xBD400000:
            raise SystemExit(f"0x{site:X}: expected `ldr sD, [xB, #imm]`, found 0x{w:08X}")
        d, n, imm = w & 0x1F, (w >> 5) & 0x1F, ((w >> 10) & 0xFFF) * 4
        start = site
        while self.word(start) != PACIBSP:
            start -= 4
            if site - start > 0x4000:
                raise SystemExit(f"0x{site:X}: no function start within 0x4000 bytes")
        if not any((self.word(start + 4 * i) & 0xFFC07FFF) in (0xA9807BFD, 0xA9007BFD) for i in range(1, 12)):
            raise SystemExit(f"0x{site:X}: function 0x{start:X} does not save x29/x30 in its prologue")
        self.check_scratch_dead(site, 16)
        cave = self.cave(("inline", op, const, d, n, imm),
                         lambda a: [ldr_s(d, n, imm), fmov_imm(16, const), fop(op, d, d, 16), ret()])
        self.put(site, bl(site, cave), vanilla=w)

    def detour_call(self, call: int, getter: int, op: str, const: float) -> None:
        """Same as detour_param_load, given the address of the `bl getter` instead of the read."""
        if self.word(call) != bl(call, getter).word:
            raise SystemExit(f"0x{call:X}: expected `bl 0x{getter:X}`")
        for site in (call + 4, call + 8):
            if (self.word(site) & 0xFFFFFFE0) == 0xBD400000:
                return self.detour_param_load(site, getter, op, const)
        raise SystemExit(f"0x{call:X}: no `ldr sD, [x0]` within two instructions of the call")

    def check_scratch_dead(self, site: int, vreg: int, limit: int = 64) -> None:
        """Refuse if the code after `site` mentions SIMD register `vreg` before the next call or return (a
        conservative stand-in for "the cave may overwrite it")."""
        import re

        import capstone  # type: ignore

        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        pat = re.compile(rf"\b[bhsdqv]{vreg}\b")
        for insn in md.disasm(self.image[site + 4:site + 4 + 4 * limit], site + 4):
            if pat.search(insn.op_str):
                raise SystemExit(f"0x{site:X}: v{vreg} is used at 0x{insn.address:X} before the next call")
            if insn.mnemonic in ("bl", "blr", "ret", "retab", "b", "br"):
                return

    # -- output
    def _records(self) -> list[tuple[int, bytes]]:
        out: list[tuple[int, bytes]] = []
        for addr in sorted(self.edits):
            data = struct.pack("<I", self.edits[addr][1])
            if out and out[-1][0] + len(out[-1][1]) == addr:
                out[-1] = (out[-1][0], out[-1][1] + data)
            else:
                out.append((addr, data))
        return out

    def pchtxt(self) -> str:
        lines = [f"@nsobid-{BUILD_ID}", "", "@flag offset_shift 0x100", "", f"// {self.title}", "@enabled"]
        lines += [f"{addr:08X} {data.hex().upper()}" for addr, data in self._records()]
        lines += ["@stop", ""]
        return "\n".join(lines)

    def ips32(self) -> bytes:
        out = bytearray(b"IPS32")
        for addr, data in self._records():
            out += struct.pack(">IH", HEADER + addr, len(data)) + data
        return bytes(out + b"EEOF")

    def write(self, out: Path | None = None, pchtxt_name: str | None = None, *, verbose: bool = True) -> Path:
        self._check_encodings()
        out = out or WORK / "build" / self.mod
        text = self.pchtxt()
        ips = self.ips32()
        base = bytearray(self.header + self.image)
        expected = bytearray(base)
        for addr, (_, new, _) in self.edits.items():
            struct.pack_into("<I", expected, HEADER + addr, new)
        for name, patched in (("pchtxt", _apply_pchtxt(base, text)), ("ips32", _apply_ips32(base, ips))):
            if patched != expected:
                raise SystemExit(f"{name}: re-applied image differs from the intended image")
        changed = sum(1 for a, c in zip(expected, base) if a != c)
        exefs = out / "exefs"
        exefs.mkdir(parents=True, exist_ok=True)
        path = exefs / f"{pchtxt_name or self.mod}.pchtxt"
        path.write_text(text, encoding="ascii", newline="\n")
        fallback = out.parent / f"{out.name}.fallback-ips" / "exefs"
        fallback.mkdir(parents=True, exist_ok=True)
        (fallback / f"{BUILD_ID}.ips").write_bytes(ips)
        print(f"{path} ({len(self.edits)} words, {changed} bytes changed)")
        if verbose:
            for addr in sorted(self.edits):
                old, new, txt = self.edits[addr]
                print(f"  0x{addr:07X}  {old:08X} -> {new:08X}  {txt or 'data'}")
        return path

    def _check_encodings(self) -> None:
        try:
            import capstone  # type: ignore
        except ImportError:
            raise SystemExit("run with `uv run --no-project --with capstone python ...` to check encodings")
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for addr, (_, new, txt) in self.edits.items():
            if txt is None:
                continue
            got = [f"{i.mnemonic} {i.op_str}".strip() for i in md.disasm(struct.pack("<I", new), addr)]
            if got != [txt]:
                raise SystemExit(f"encoding check failed at 0x{addr:X}: {got} != {txt!r}")


def _apply_ips32(base: bytes, ips: bytes) -> bytearray:
    target = bytearray(base)
    assert ips[:5] == b"IPS32"
    pos = 5
    while ips[pos:pos + 4] != b"EEOF":
        off, size = struct.unpack_from(">IH", ips, pos)
        pos += 6
        target[off:off + size] = ips[pos:pos + size]
        pos += size
    return target


def _apply_pchtxt(base: bytes, text: str) -> bytearray:
    """Apply a pchtxt the way the yuzu/Eden IPSwitch compiler reads it (the subset this kit writes)."""
    target = bytearray(base)
    lines = text.replace("\r", "").split("\n")
    shift = 0
    little = False
    build_ok = False
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("@stop"):
            break
        if line.startswith("@nsobid-"):
            build_ok = line[8:].upper().ljust(64, "0") == BUILD_ID.ljust(64, "0")
        elif line.startswith("@little"):
            little = True
        elif line.startswith("@big"):
            little = False
        elif line.startswith("@flag"):
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "offset_shift":
                shift = int(parts[2], 0)
        elif line.startswith("@enabled") or line.startswith("@disabled"):
            if i == 0:
                raise ValueError("@enabled on the first line: the loader rejects the file")
            enabled = line.startswith("@enabled")
            while i + 1 < len(lines):
                pl = lines[i + 1]
                if pl.startswith("@enabled") or pl.startswith("@disabled"):
                    break
                i += 1
                if pl.startswith("@flag"):
                    continue
                if len(pl) < 11:
                    break
                off = int(pl[:8], 16) + shift
                data = bytes.fromhex(pl[9:])
                if little:
                    data = data[::-1]
                if enabled:
                    target[off:off + len(data)] = data
        i += 1
    if not build_ok:
        raise ValueError("pchtxt build ID does not match")
    return target
