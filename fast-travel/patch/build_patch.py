"""Compile Fast Travel and write a pchtxt and a separate IPS32 from a local OoT remake dump."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from lz4.block import compress, decompress

HERE = Path(__file__).resolve().parent
MOD = HERE.parent
SRC = MOD / "src" / "program"
sys.path.insert(0, str(MOD.parent))
import patchkit  # noqa: E402

REPO = patchkit.REPO
WORK = patchkit.WORK
BIN = REPO / "vendor/totk-dkp/root/opt/devkitpro/devkitA64/bin"
CS = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

CODE, DATA = 0x5000000, 0x6000000  # virtual placement bases for compiled code and state
CHAIN = 0x4F00000  # virtual address of the stored-state chain, resolved after code placement
TEXT_END = patchkit.TEXT_SIZE
BSS_RECORDED_END = 0x4E9A4C8  # main's module header: end of zeroed state
MAPPED_END = 0x4EA0000  # main's mapping (data + zeroed state) ends here
ANCHOR = 0x4E9FFF0  # 16 bytes in the unused tail: {state address, open-gate bits}
NNMAIN, NNMAIN_BODY = 0x1202CD0, 0x1202CE0  # nnMain is one branch to its body
# The emulator's always-on built-in patches (lichii.exe record table: main offset, bytes).
BUILTIN = [(0x805BD0, 4), (0x6E6CC4, 4), (0x3C46200, 0x78)]
SDK_BUILD_ID = "1643A838814F9C10C696F930ED681E7FA3FB9AF8"
MAGIC = 0x31505446  # "FTP1"
MAX_GAP_WORDS = 6

# Hook name in the source, Site constant in game.hpp.
HOOKS = [("ControllerCalc", "kControllerCalc"), ("AutoSaveRequest", "kAutoSaveRequest"),
         ("LoadGraphicsProcs", "kLoadGraphicsProcs"), ("DrawLayer", "kDrawLayer")]
HOOKS += [(f"NpadStates{i}", f"kNpadStates[{i}]") for i in range(5)]
VISITED_HOOKS = [("OpenFileHook", "kOpenFile")]

IMPORTS = {
    "svcGetSystemTick": "_ZN2nn2os13GetSystemTickEv",
    "patch_current_thread": "_ZN2nn2os16GetCurrentThreadEv",
    "patch_allocate_region": "_ZN2nn2os21AllocateAddressRegionEPmm",
    "patch_allocate_pages": "_ZN2nn2os19AllocateMemoryPagesEmm",
    "patch_free_region": "_ZN2nn2os17FreeAddressRegionEm",
    "patch_free_pages": "_ZN2nn2os15FreeMemoryPagesEmm",
}
LIBC = {"memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "vsnprintf", "sqrtf"}

FLAGS = ["-std=gnu++26", "-Oz", "-fno-exceptions", "-fno-rtti", "-fno-unwind-tables",
         "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections", "-fno-jump-tables",
         "-fno-tree-switch-conversion", "-fno-strict-aliasing", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
         "-mno-outline-atomics", "-march=armv8.2-a+crc+crypto", "-fno-pic", "-fno-stack-protector",
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-DFAST_TRAVEL_PATCH=1"]
CXX_ONLY = {"-std=gnu++26", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit"}
C_FLAGS = ["-std=gnu17", "-Wno-shift-negative-value"] + [flag for flag in FLAGS if flag not in CXX_ONLY]


def u32(data: bytes, at: int) -> int:
    return struct.unpack_from("<I", data, at)[0]


def decode(word: int, pc: int):
    result = list(CS.disasm(struct.pack("<I", word), pc))
    if len(result) != 1:
        raise ValueError(f"undecodable word {word:08x} at {pc:x}")
    return result[0]


def target(insn) -> int:
    return int(insn.op_str.rsplit("#", 1)[1], 0)


BRANCHES = ("b", "bl", "cbz", "cbnz", "tbz", "tbnz")


def branch_kind(insn) -> str | None:
    if insn.mnemonic in BRANCHES or insn.mnemonic.startswith("b."):
        return insn.mnemonic
    return None


# ---------------------------------------------------------------- encoders (each result is checked with capstone)

def _rel(pc: int, dest: int, bits: int, what: str) -> int:
    delta = dest - pc
    if delta & 3 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f"{what} at {pc:x} cannot reach {dest:x}")
    return (delta >> 2) & ((1 << bits) - 1)


def enc_b(pc: int, dest: int, link: bool = False) -> int:
    return (0x94000000 if link else 0x14000000) | _rel(pc, dest, 26, "branch")


def enc_retarget(word: int, insn, pc: int, dest: int) -> int:
    """The same branch instruction (register, condition, bit) aimed at dest from pc."""
    m = insn.mnemonic
    if m in ("b", "bl"):
        return enc_b(pc, dest, m == "bl")
    if m in ("tbz", "tbnz"):
        return (word & 0xFFF8001F) | (_rel(pc, dest, 14, m) << 5)
    return (word & 0xFF00001F) | (_rel(pc, dest, 19, m) << 5)  # b.cond, cbz, cbnz


def enc_adrp(pc: int, rd: int, dest: int) -> int:
    pages = ((dest & ~0xFFF) - (pc & ~0xFFF)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError(f"adrp at {pc:x} cannot reach {dest:x}")
    pages &= (1 << 21) - 1
    return 0x90000000 | ((pages & 3) << 29) | ((pages >> 2) << 5) | rd


def enc_adr(pc: int, rd: int, dest: int) -> int:
    delta = dest - pc
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError(f"adr at {pc:x} cannot reach {dest:x}")
    delta &= (1 << 21) - 1
    return 0x10000000 | ((delta & 3) << 29) | ((delta >> 2) << 5) | rd


def enc_add_page(rd: int, pages: int) -> int:  # add xd, xd, #pages, lsl #12
    if not 0 < pages < 0x1000:
        raise ValueError("state page offset")
    return 0x91400000 | (pages << 10) | (rd << 5) | rd


def set_lo12(word: int, value: int, shift: int = 0) -> int:
    if value & ((1 << shift) - 1):
        raise ValueError("misaligned low-12 relocation")
    return (word & ~(0xFFF << 10)) | (((value & 0xFFF) >> shift) << 10)


# ---------------------------------------------------------------- game inputs

def load_sites() -> dict[str, tuple[int, list[int]]]:
    text = (SRC / "game.hpp").read_text(encoding="utf-8")
    sites = {}
    for name, offset, words in re.findall(r"constexpr Site (k\w+)\{\"[^\"]+\",\s*(0x[0-9A-F]+),\s*\{([^}]+)\}\}", text):
        sites[name] = (int(offset, 16), [int(w, 16) for w in words.split(",")])
    block = re.search(r"kNpadStates\[5\] = \{(.*?)\n\};", text, re.S)[1]
    for i, (offset, words) in enumerate(re.findall(r"\{\"[^\"]+\",\s*(0x[0-9A-F]+),\s*\{([^}]+)\}\}", block)):
        sites[f"kNpadStates[{i}]"] = (int(offset, 16), [int(w, 16) for w in words.split(",")])
    return sites


def dynamic_tags(main: bytes) -> dict[int, int]:
    mod0 = u32(main, 4)
    dyn = mod0 + struct.unpack_from("<i", main, mod0 + 4)[0]
    tags, at = {}, dyn
    while True:
        tag, value = struct.unpack_from("<qQ", main, at)
        at += 16
        if tag == 0:
            break
        tags.setdefault(tag, value)
    return tags


def load_imports(main: bytes) -> tuple[dict[str, int], dict[str, int]]:
    """Resolve import entries and table slots, checking that each entry reads the right slot."""
    tags = dynamic_tags(main)
    plt0 = struct.unpack_from("<Q", main, struct.unpack_from("<Q", main, tags[23])[0])[0]  # unbound slots: lazy entry
    first_slot = tags[3] + 24
    stubs, slots = {}, {}
    for i in range(tags[2] // 24):
        slot, info, _ = struct.unpack_from("<QQq", main, tags[23] + 24 * i)
        name_at = tags[5] + u32(main, tags[6] + (info >> 32) * 24)
        name = main[name_at:main.index(b"\0", name_at)].decode()
        stub = plt0 + 32 + 16 * ((slot - first_slot) // 8)
        adrp, ldr = decode(u32(main, stub), stub), decode(u32(main, stub + 4), stub + 4)
        offset = re.search(r"#(0x[0-9a-f]+|\d+)\]", ldr.op_str)
        if adrp.mnemonic != "adrp" or ldr.mnemonic != "ldr" or \
                target(adrp) + (int(offset[1], 0) if offset else 0) != slot:
            raise ValueError(f"import entry for {name} does not read its slot")
        stubs[name], slots[name] = stub, slot
    return stubs, slots


def sdk_deltas(game_dir: Path) -> tuple[int, int]:
    """Distances from the SDK's tick export to its debug output routine and to nn::fs::MountSdCard."""
    header = (game_dir / "extract/exefs/sdk").read_bytes()[:0x100]
    if header[:4] != b"NSO0" or header[0x40:0x54].hex().upper() != SDK_BUILD_ID:
        raise ValueError("sdk build differs from the one the boots used")
    sdk = (game_dir / "sdk.bin").read_bytes()
    tick, debug, mount = 0x166370, 0x1A5568, 0x10F0C0
    if sdk[tick:tick + 8] != bytes.fromhex("20e03bd5c0035fd6"):  # mrs x0, cntpct_el0; ret
        raise ValueError("sdk tick export changed")
    if sdk.count(bytes.fromhex("e10400d4c0035fd6")) != 1 or sdk.find(bytes.fromhex("e10400d4c0035fd6")) != debug:
        raise ValueError("sdk debug output routine (svc 0x27; ret) is not unique")
    # _ZN2nn2fs11MountSdCardEPKc's export value; src/program/ui/pictures.cpp checks the same first words.
    if sdk[mount:mount + 16] != bytes.fromhex("7f2303d5ff0301d1fd7b01a9f51300f9"):
        raise ValueError("sdk SD card mount routine changed")
    return debug - tick, tick - mount


def sign_extend(values, bits: int):
    sign = 1 << (bits - 1)
    return (values ^ sign) - sign


def find_gaps(kit: patchkit.PatchKit) -> list[tuple[int, int]]:
    """Find zero padding after terminal instructions, excluding referenced and reserved space."""
    main = kit.image
    w = np.frombuffer(main, dtype="<u4", count=TEXT_END // 4)
    referenced = []
    for mask, opcode, shift, bits in (
        (0x7C000000, 0x14000000, 0, 26),  # b, bl
        (0xFF000000, 0x54000000, 5, 19),  # b.cond
        (0x7E000000, 0x34000000, 5, 19),  # cbz, cbnz
        (0x7E000000, 0x36000000, 5, 14),  # tbz, tbnz
        (0x3B000000, 0x18000000, 5, 19),  # ldr (literal), all widths
    ):
        indices = np.flatnonzero((w & mask) == opcode)
        offsets = (w[indices].astype(np.int64) >> shift) & ((1 << bits) - 1)
        referenced.append(4 * (indices + sign_extend(offsets, bits)))
    for op in (0x10000000, 0x90000000):  # adr and adrp
        indices = np.flatnonzero((w & 0x9F000000) == op)
        words = w[indices].astype(np.int64)
        offsets = sign_extend(((words >> 3) & 0x1FFFFC) | ((words >> 29) & 3), 21)
        if op == 0x10000000:
            referenced.append(4 * indices + offsets)
        else:
            pages = set(((4 * indices & ~0xFFF) + (offsets << 12)).tolist())
    pointers = np.frombuffer(main[TEXT_END:len(main) // 8 * 8], dtype="<u8")
    referenced.append(pointers[(pointers > 0) & (pointers < MAPPED_END)].astype(np.int64))
    refs = np.unique(np.concatenate(referenced))
    audit_anchor(kit, refs, pages)
    zero = (w == 0).astype(np.int8)
    edges = np.diff(np.concatenate(([0], zero, [0])))
    starts, ends = np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]
    reserved = [(a, a + 4 * patchkit.CAVE_WORDS) for caves in patchkit.CAVES.values() for a in caves]
    reserved += [(a, a + n) for a, n in BUILTIN]
    gaps = []
    for s, e in zip(starts, ends):
        n = int(e - s)
        if s == 0 or e >= len(w) or n > MAX_GAP_WORDS or not patchkit._is_function_end(int(w[s - 1])):
            continue
        if int(w[e]) != patchkit.PACIBSP:
            continue
        lo, hi = int(s) * 4, int(e) * 4
        i = np.searchsorted(refs, lo)
        if (i < len(refs) and refs[i] < hi) or (lo & ~0xFFF) in pages or ((hi - 1) & ~0xFFF) in pages:
            continue
        if any(a < hi and lo < b for a, b in reserved):
            continue
        gaps.append((lo, n))
    return gaps


def audit_anchor(kit: patchkit.PatchKit, refs, pages: set[int]) -> None:
    main, header = kit.image, kit.header
    mod0 = u32(main, 4)
    recorded_end = mod0 + struct.unpack_from("<i", main, mod0 + 12)[0]
    mapped_end = u32(header, 0x34) + u32(header, 0x38) + u32(header, 0x3C)
    if (recorded_end, mapped_end) != (BSS_RECORDED_END, MAPPED_END) or not recorded_end <= ANCHOR <= mapped_end - 16:
        raise ValueError("main's writable padding differs from the anchor layout")
    i = np.searchsorted(refs, ANCHOR)
    if (ANCHOR & ~0xFFF) in pages or (i < len(refs) and refs[i] < ANCHOR + 16):
        raise ValueError("vanilla code or data references the anchor")
    tags = dynamic_tags(main)
    for start, size in ((tags.get(7, 0), tags.get(8, 0)), (tags[23], tags[2])):
        for offset in range(start, start + size, 24):
            site, _, addend = struct.unpack_from("<QQq", main, offset)
            if ANCHOR - 7 <= site < ANCHOR + 16 or ANCHOR <= addend < ANCHOR + 16:
                raise ValueError("vanilla dynamic relocation references the anchor")


# ---------------------------------------------------------------- compile

def run(args: list[str], log: Path) -> str:
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    with log.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(args) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        errors = [line for line in (result.stderr + result.stdout).splitlines() if "error" in line]
        raise SystemExit("\n".join(errors[:12]) or f"command failed; see {log}")
    return result.stdout + result.stderr


def tool(tool_bin: Path, name: str) -> str:
    suffix = ".exe" if os.name == "nt" else ""
    return str(tool_bin / f"aarch64-none-elf-{name}{suffix}")


def compile_module(out: Path, tool_bin: Path, visited: bool, hooks: list[tuple[str, int, int]],
                   replay: dict[str, tuple[int, bool]], deltas: tuple[int, int]) -> Path:
    gen = out / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(MOD / "tools"))
    import make_assets  # noqa: E402  (needs Pillow)
    fonts = make_assets.build_fonts()
    (gen / "fonts.inc").write_text(make_assets.as_c_array(fonts, "kFontFile"), encoding="utf-8")
    pictures = make_assets.build_embedded_pictures(MOD / "assets" / "areas")
    (gen / "pictures.inc").write_text(make_assets.as_c_array(pictures, "kPictureFile"), encoding="utf-8")
    (gen / "patch_hooks.inc").write_text(
        "".join(f"    {{{site:#x}, {bit}}},  // {name}\n" for name, site, bit in hooks), encoding="utf-8")
    (gen / "patch_adapters.inc").write_text(
        "".join(f"hook {name}, {bit}, {replay[name][0]:#x}, {int(replay[name][1])}\n"
                for name, _, bit in hooks), encoding="utf-8")
    log = out / "commands.log"
    log.write_text("", encoding="utf-8")
    includes = [HERE / "include", SRC, gen]
    sources = sorted(SRC.rglob("*.cpp")) + [HERE / "runtime.cpp"] + sorted(SRC.rglob("*.c"))
    if len({source.stem for source in sources}) != len(sources):
        raise ValueError("two sources share a file name")
    objects, warnings = [], []
    for source in sources:
        obj = out / f"{source.stem}.o"
        compiler, language = ("gcc", C_FLAGS) if source.suffix == ".c" else ("g++", FLAGS)
        text = run([tool(tool_bin, compiler), *language, f"-DFAST_TRAVEL_VISITED_ONLY={int(visited)}",
                    *(f"-I{p}" for p in includes), "-c", str(source), "-o", str(obj)], log)
        warnings += [line for line in text.splitlines() if "warning:" in line]
        objects.append(obj)
    print(f"fonts: {len(fonts)} bytes built in")
    debug_delta, mount_back = deltas
    adapter = out / "adapters.o"
    run([tool(tool_bin, "gcc"), f"-I{gen}", f"-DDEBUG_DELTA_PAGES={debug_delta >> 12}",
         f"-DDEBUG_DELTA_LOW={debug_delta & 0xfff}", f"-DMOUNT_BACK_PAGES={mount_back >> 12}",
         f"-DMOUNT_BACK_LOW={mount_back & 0xfff}", "-c", str(HERE / "adapters.S"), "-o", str(adapter)], log)
    objects.append(adapter)
    combined = out / "fast-travel-patch.o"
    run([tool(tool_bin, "ld"), "-r", *map(str, objects), "-o", str(combined)], log)
    (out / "compiled.asm").write_text(run([tool(tool_bin, "objdump"), "-dr", "-C", str(combined)], log),
                                      encoding="utf-8")
    if warnings:
        print(f"{len(warnings)} compiler warnings:\n  " + "\n  ".join(warnings[:20]))
    return combined


# ---------------------------------------------------------------- gap pool

class Gaps:
    def __init__(self, gaps: list[tuple[int, int]]):
        self.free: dict[int, list[int]] = {n: [] for n in range(1, MAX_GAP_WORDS + 1)}
        for a, n in gaps:
            self.free[n].append(a)
        for values in self.free.values():
            values.reverse()

    def take(self, words: int = 2, *, largest: bool = True) -> tuple[int, int]:
        """Take a gap with at least this many free words."""
        sizes = range(MAX_GAP_WORDS, words - 1, -1) if largest else range(words, MAX_GAP_WORDS + 1)
        for n in sizes:
            if self.free[n]:
                return self.free[n].pop(), n
        raise ValueError(f"no free gap of at least {words} words left")

    def recycle(self, start: int, end: int) -> None:
        if start < end:
            n = (end - start) // 4
            self.free[n].append(start)


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & -boundary


@dataclass
class Module:
    text: list[tuple[int, bytes]]
    symbols: dict[str, int]
    code_relocs: dict[int, tuple[int, int]]
    data_relocs: dict[int, int]
    data: bytearray
    region_bytes: int


def read_object(path: Path, externals: dict[str, int], hooks: list[tuple[str, int, int]]) -> Module:
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        if elf["e_machine"] != "EM_AARCH64" or elf["e_type"] != "ET_REL" or not elf.little_endian:
            raise ValueError("expected a little-endian AArch64 relocatable object")
        sections = list(elf.iter_sections())
        symtab = elf.get_section_by_name(".symtab")
        bases, text, data = {}, [], bytearray()
        code_end = CODE
        for index, section in enumerate(sections):
            size, flags = section["sh_size"], section["sh_flags"]
            if not size or not flags & 2 or section["sh_type"] == "SHT_NOBITS":
                continue
            if section["sh_type"] != "SHT_PROGBITS" or flags & 0x400:
                raise ValueError(f"unsupported allocated section: {section.name}")
            alignment = max(section["sh_addralign"], 1)
            if alignment > 0x10000:
                raise ValueError(f"unsupported section alignment: {section.name}")
            if flags & 4:
                code_end = align(code_end, max(4, alignment))
                if size % 4:
                    raise ValueError(f"partial instruction in {section.name}")
                bases[index] = code_end
                text.append((code_end, section.data()))
                code_end += size
            else:
                start = align(len(data), alignment)
                bases[index] = DATA + start
                data.extend(bytes(start - len(data)))
                data.extend(section.data())

        relocations = [sec for sec in sections if isinstance(sec, RelocationSection)]
        fixups = sum(sec.num_relocations() for sec in relocations if bases.get(sec["sh_info"], 0) >= DATA)
        region_bytes = align(len(data), 4) + fixups * 4
        for index, section in enumerate(sections):
            if section["sh_type"] == "SHT_NOBITS" and section["sh_flags"] & 2 and section["sh_size"]:
                if section["sh_flags"] & 0x400 or section["sh_addralign"] > 0x10000:
                    raise ValueError(f"unsupported zeroed section: {section.name}")
                region_bytes = align(region_bytes, max(section["sh_addralign"], 1))
                bases[index] = DATA + region_bytes
                region_bytes += section["sh_size"]
        if code_end >= DATA or align(region_bytes, 0x1000) > 0x400000:
            raise ValueError("compiled code or state exceeds its virtual range")

        symbols = {sym.name: bases[sym["st_shndx"]] + sym["st_value"] for sym in symtab.iter_symbols()
                   if sym.name and sym["st_shndx"] in bases}
        for name, _, _ in hooks:
            matches = [addr for symbol, addr in symbols.items() if f"{len(name)}{name}8CallbackE" in symbol]
            if len(matches) != 1:
                raise ValueError(f"ambiguous callback symbol for {name}: {len(matches)} matches")
            externals[f"patch_callback_{name}"] = matches[0]

        code_relocs, data_relocs = {}, {}
        for sec in relocations:
            owner = sec["sh_info"]
            if owner not in bases:
                continue
            for reloc in sec.iter_relocations():
                kind, offset = reloc["r_info_type"], reloc["r_offset"]
                symbol = symtab.get_symbol(reloc["r_info_sym"])
                shndx = symbol["st_shndx"]
                if shndx == "SHN_UNDEF":
                    if symbol.name not in externals:
                        raise ValueError(f"unresolved service: {symbol.name}")
                    destination = externals[symbol.name]
                else:
                    destination = bases[shndx] + symbol["st_value"]
                destination += reloc["r_addend"]
                at = bases[owner] + offset
                if at >= DATA:
                    if kind != 257 or offset + 8 > sections[owner]["sh_size"] or at & 7 or at in data_relocs:
                        raise ValueError(f"unsupported state relocation {kind} at {at:x}")
                    data_relocs[at] = destination
                else:
                    if offset + 4 > sections[owner]["sh_size"] or at & 3 or at in code_relocs:
                        raise ValueError(f"invalid instruction relocation at {at:x}")
                    code_relocs[at] = kind, destination
        if len(data_relocs) != fixups:
            raise ValueError("relocation table overlaps the uninitialized state")
        return Module(text, symbols, code_relocs, data_relocs, data, region_bytes)


def place_code(module: Module, pool: Gaps):
    """Assign physical words, preserving fallthrough without using scratch registers or changing flags."""
    mapped, connectors, placed = {}, {}, []
    for base, data in module.text:
        cursor = end = 0
        for offset in range(0, len(data), 4):
            pc = base + offset
            word = u32(data, offset)
            insn = decode(word, pc)
            branch = branch_kind(insn)
            conditional = branch is not None and branch not in ("b", "bl")
            terminal = branch == "b" or insn.mnemonic in ("br", "ret", "retab", "retaa")
            kind, dest = module.code_relocs.get(pc, (0, 0))
            heap_page = dest & ~0xFFF if kind == 275 and dest >= DATA else 0
            if conditional:
                count = 3
            elif heap_page:
                count = 3 if heap_page > DATA else 2
            else:
                count = 1
            needed = count + int(not (terminal or conditional))
            if end - cursor < 4 * needed:
                old_cursor, old_end = cursor, end
                cursor, capacity = pool.take(needed, largest=not (terminal or conditional))
                end = cursor + 4 * capacity
                if old_cursor:
                    connectors[old_cursor] = cursor
                    pool.recycle(old_cursor + 4, old_end)
            mapped[pc] = cursor
            placed.append((cursor, insn, word, conditional, heap_page))
            cursor += 4 * count
            if terminal or conditional:
                pool.recycle(cursor, end)
                cursor = end = 0
        if cursor:
            raise ValueError(f"compiled section at {base:x} falls through its end")
    return mapped, connectors, placed


def emit_state(kit: patchkit.PatchKit, module: Module, mapped: dict[int, int], pool: Gaps) -> tuple[int, int]:
    blob = module.data
    fixup_offset = align(len(blob), 4)
    blob.extend(bytes(fixup_offset - len(blob)))
    data_fixups, main_fixups = [], []
    for at, dest in sorted(module.data_relocs.items()):
        offset = at - DATA
        if DATA <= dest < DATA + module.region_bytes:
            data_fixups.append(offset)
            value = dest - DATA
        else:
            main_fixups.append(offset)
            value = mapped[dest] if dest >= CODE else dest
            if not 0 <= value < MAPPED_END:
                raise ValueError(f"state pointer target outside main: {dest:x}")
        struct.pack_into("<Q", blob, offset, value)
    for offset in data_fixups + main_fixups:
        blob.extend(struct.pack("<I", offset))
    checksum = 2166136261
    for byte in blob:
        checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
    packed = compress(bytes(blob), mode="high_compression", compression=12, store_size=False)
    header = struct.pack("<8I", MAGIC, len(packed), len(blob), module.region_bytes, checksum,
                         fixup_offset, len(data_fixups), len(main_fixups))
    stream = header + packed
    chunks = []
    cursor = 0
    while cursor < len(stream):
        address, capacity = pool.take()
        data = stream[cursor:cursor + 4 * (capacity - 1)]
        chunks.append((address, data))
        cursor += len(data)
    for index, (address, data) in enumerate(chunks):
        next_address = chunks[index + 1][0] if index + 1 < len(chunks) else 0
        kit.put_data(address, (next_address >> 2) << 8 | len(data), 0)
        padded = data.ljust(align(len(data), 4), b"\0")
        for offset in range(0, len(padded), 4):
            kit.put_data(address + 4 + offset, u32(padded, offset), 0)

    # Read the emitted words with the runtime's chain protocol before accepting the layout.
    chain, visited = bytearray(), set()
    address = head = chunks[0][0]
    while address:
        if address in visited:
            raise ValueError("loop in stored-state chain")
        visited.add(address)
        word = kit.edits[address][1]
        count = word & 255
        payload = b"".join(struct.pack("<I", kit.edits[address + 4 + i][1]) for i in range(0, count, 4))
        chain.extend(payload[:count])
        address = (word >> 8) << 2
    if chain[:len(header)] != header or decompress(bytes(chain[len(header):]), uncompressed_size=len(blob)) != blob:
        raise ValueError("stored state failed round trip")
    print(f"state: {len(blob)} initialized bytes, {module.region_bytes} bytes mapped before page rounding, "
          f"{len(packed)} bytes compressed, {len(data_fixups)} state + {len(main_fixups)} main pointers")
    return head, len(chunks)


def put_code(kit: patchkit.PatchKit, at: int, word: int, old: int = 0, dest: int | None = None) -> None:
    insn = decode(word, at)
    if dest is not None and target(insn) != dest:
        raise ValueError(f"emitted {insn.mnemonic} at {at:x} misses {dest:x}")
    kit.put(at, patchkit.Insn(word, f"{insn.mnemonic} {insn.op_str}".strip()), old)


def put_branch(kit: patchkit.PatchKit, at: int, dest: int, old: int = 0) -> None:
    put_code(kit, at, enc_b(at, dest), old, dest)


def emit_code(kit: patchkit.PatchKit, module: Module, mapped, connectors, placed, chain_head: int) -> None:
    def physical(address: int) -> int:
        if address >= CODE:
            return mapped[address]
        return chain_head if address == CHAIN else address

    for at, dest in connectors.items():
        put_branch(kit, at, dest)
    for at, insn, word, conditional, heap_page in placed:
        pc = insn.address
        kind, dest = module.code_relocs.get(pc, (0, 0))
        branch = branch_kind(insn)
        if branch:
            if kind not in (0, 282 if branch == "b" else 283 if branch == "bl" else -1):
                raise ValueError(f"unsupported branch relocation {kind} at {pc:x}")
            dest = physical(dest if kind else target(insn))
            if conditional:
                put_code(kit, at, enc_retarget(word, insn, at, at + 8), dest=at + 8)
                put_branch(kit, at + 4, mapped[pc + 4])
                put_branch(kit, at + 8, dest)
            else:
                put_code(kit, at, enc_retarget(word, insn, at, dest), dest=dest)
        elif heap_page:
            rd = word & 31
            anchor_page = ANCHOR & ~0xFFF
            put_code(kit, at, enc_adrp(at, rd, anchor_page), dest=anchor_page)
            put_code(kit, at + 4, patchkit.ldr_x(rd, rd, ANCHOR & 0xFFF).word)
            if heap_page > DATA:
                put_code(kit, at + 8, enc_add_page(rd, (heap_page - DATA) >> 12))
        elif kind == 275 and insn.mnemonic == "adrp":
            page = physical(dest) & ~0xFFF
            put_code(kit, at, enc_adrp(at, word & 31, page), dest=page)
        elif kind == 274 and insn.mnemonic == "adr" and dest < DATA:
            address = physical(dest)
            put_code(kit, at, enc_adr(at, word & 31, address), dest=address)
        elif kind in (277, 278, 284, 285, 286, 299):
            shift = {277: 0, 278: 0, 284: 1, 285: 2, 286: 3, 299: 4}[kind]
            put_code(kit, at, set_lo12(word, dest if dest >= DATA else physical(dest), shift))
        else:
            if kind or insn.mnemonic in ("adr", "adrp", "svc", "hvc", "smc", "brk", "hlt", "udf") or \
                    insn.mnemonic.startswith("bc.") or (insn.mnemonic.startswith(("ldr", "prfm")) and "[" not in insn.op_str):
                raise ValueError(f"unsupported instruction or relocation {kind} at {pc:x}: {insn.mnemonic} {insn.op_str}")
            put_code(kit, at, word)


def layout(kit: patchkit.PatchKit, out: Path, obj: Path, hooks, externals: dict[str, int]) -> None:
    module = read_object(obj, externals, hooks)
    gaps = find_gaps(kit)
    pool = Gaps(gaps)
    mapped, connectors, placed = place_code(module, pool)
    head, chunks = emit_state(kit, module, mapped, pool)
    left = sum(n * len(at) for n, at in pool.free.items())
    print(f"gaps left: {sum(len(at) for at in pool.free.values())} gaps, {left} words; room for about "
          f"{4 * (left - sum(len(at) for at in pool.free.values()))} more compressed state bytes")
    emit_code(kit, module, mapped, connectors, placed, head)
    put_branch(kit, NNMAIN, mapped[module.symbols["patch_start"]], enc_b(NNMAIN, NNMAIN_BODY))
    for name, site, _ in hooks:
        put_branch(kit, site, mapped[module.symbols[f"patch_gate_{name}"]], kit.word(site))
    if any(addr < start + size and start < addr + 4 for addr in kit.edits for start, size in BUILTIN):
        raise ValueError("patch overlaps an emulator built-in patch")
    (out / "layout.json").write_text(json.dumps({"symbols": {name: mapped[addr] for name, addr in module.symbols.items()
                                                              if addr in mapped},
                                                "state_head": head, "state_chunks": chunks,
                                                "gap_counts": dict(Counter(n for _, n in gaps))}, indent=2) + "\n",
                                     encoding="utf-8")
    print(f"layout: {len(mapped)} compiled instructions, {chunks} state chunks, {len(kit._records())} patch records")
    kit.write(out.parent / kit.mod, verbose=False)


def check_main_image(kit: patchkit.PatchKit) -> None:
    for index in range(3):
        start, size = struct.unpack_from("<II", kit.header, 0x14 + index * 0x10)
        actual = hashlib.sha256(kit.image[start:start + size]).digest()
        if actual != kit.header[0xA0 + index * 32:0xC0 + index * 32]:
            raise ValueError(f"flat image segment {index} does not match the NSO header")
    if kit.word(NNMAIN) != enc_b(NNMAIN, NNMAIN_BODY):
        raise ValueError("nnMain entry changed")


def prepare_hooks(main_bin: bytes, visited: bool):
    sites = load_sites()
    hook_list = HOOKS + (VISITED_HOOKS if visited else [])
    hooks = [(name, sites[const][0], bit) for bit, (name, const) in enumerate(hook_list)]
    stubs, slots = load_imports(main_bin)
    externals = {alias: stubs[name] for alias, name in IMPORTS.items()}
    externals.update({name: stubs[name] for name in LIBC if name in stubs})
    externals.update(patch_image_base=0, patch_anchor=ANCHOR, patch_hook_mask=ANCHOR + 8,
                     patch_chain_head=CHAIN, patch_tick_slot=slots[IMPORTS["svcGetSystemTick"]],
                     patch_nnmain_body=NNMAIN_BODY)
    replay = {}
    for name, const in hook_list:
        offset, words = sites[const]
        actual = [u32(main_bin, offset + 4 * i) for i in range(4)]
        if actual != words:
            raise ValueError(f"{name}: game words differ from game.hpp")
        first = decode(words[0], offset)
        plt = first.mnemonic == "adrp" and first.op_str.startswith("x16, ") and offset in stubs.values()
        if plt:
            externals[f"patch_plt_page_{name}"] = target(first)
        elif first.mnemonic not in ("pacibsp", "ldrb"):
            raise ValueError(f"cannot replay {name}'s first instruction: {first.mnemonic} {first.op_str}")
        externals[f"patch_resume_{name}"] = offset + 4
        replay[name] = words[0], plt
    return hooks, externals, replay


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all-places", action="store_true", help="build the all-places version (default: visited)")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--game-dir", type=Path, default=WORK, help="directory containing the flattened game inputs")
    parser.add_argument("--tool-bin", type=Path, default=BIN, help="devkitA64 bin directory")
    parser.add_argument("--output-dir", type=Path, help="output root (default: <game-dir>/build)")
    args = parser.parse_args()

    visited = not args.all_places
    slug = "fast-travel-patch" if visited else "fast-travel-patch-all"
    title = "Fast Travel (Visited Patch)" if visited else "Fast Travel (All Places Patch)"
    kit = patchkit.PatchKit(slug, title, nso=args.game_dir / "main.uncompressed.nso",
                            image_path=args.game_dir / "main.bin")
    check_main_image(kit)
    hooks, externals, replay = prepare_hooks(kit.image, visited)
    deltas = sdk_deltas(args.game_dir)

    out = (args.output_dir or args.game_dir / "build") / f"{slug}-work"
    combined = compile_module(out, args.tool_bin, visited, hooks, replay, deltas)
    if args.compile_only:
        print(run([tool(args.tool_bin, "size"), "-A", str(combined)], out / "commands.log"))
        print(run([tool(args.tool_bin, "nm"), "-u", str(combined)], out / "commands.log"))
        return
    layout(kit, out, combined, hooks, externals)


if __name__ == "__main__":
    main()
