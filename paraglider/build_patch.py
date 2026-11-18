"""Build the Paraglider code patch for the Ocarina of Time remake (Switch 2, title 0400486024DEC000), main 2.0.0.

Compiles code/glider.c and code/hooks.S into the zero tail of the code segment and replaces the first
instruction of three game routines with a branch to a hook:

  0x2B41580  upper-body selector pass              -> hook_sub_pass   (glider_frame, for SelectorPlayerSubSchedulerMain)
  0x2B43870  SelectorPlayerSubSchedulerNone check   -> hook_sub_none   (switches the arms to carrying)
  0x499B70   SelectorPlayerSubSchedulerDrawn check  -> hook_sub_drawn  (same, with the sword out)

Each routine is reached only through the listed vtable slots.

  uv run --no-project --with capstone --with numpy python build_patch.py
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import patchkit  # noqa: E402
from patchkit import PatchKit  # noqa: E402

CODE, CODE_END = 0x3C46280, 0x3C46FF0  # zero tail of the code segment; Lichii's built-in patch ends at 0x3C46277
STATE, STATE_SIZE = 0x4E9FD00, 0x40   # unused zeroed memory
OWN_FLAG = "IsGet.Item_Paraglider"
CHEST_FLAG = "IsTreasureBoxOpen.7395792061164335289"  # must match data_manifest.json "chest_flag"

# slot +0x138 of the 13 upper-body selector vtables
SUB_PASS_SLOTS = (0x4B0BC88, 0x4B0BE10, 0x4B0BFB0, 0x4B0C138, 0x4B0C2C0, 0x4B0C448, 0x4B0C5D0, 0x4B0C758,
                  0x4B0C8E0, 0x4B0CA68, 0x4B0CBF0, 0x4B0CD78, 0x4B0CF00)

# routine, its first word (also in code/hooks.S), its only references, hook, trampoline
DETOURS = [
    (0x2B41580, 0xD503237F, SUB_PASS_SLOTS, "hook_sub_pass", "game_sub_pass"),
    (0x2B43870, 0xF9400408, (0x4B0CC18,), "hook_sub_none", "game_sub_none_check"),
    (0x499B70, 0xD503237F, (0x4B0C160,), "hook_sub_drawn", "game_sub_drawn_check"),
]

# name: (flat offset, expected first word or None)
GAME = {
    "game_sub_pass_body": (0x2B41584, 0xD10103FF),
    "game_sub_none_body": (0x2B43874, 0xF9400508),
    "game_sub_drawn_body": (0x499B74, 0xA9BE7BFD),
    "game_sub_main_vtable": (0x4B0C930, None),
    "game_select_child": (0x802000, None),
    "game_key_pressed": (0x935090, 0xF9402008),
    "game_holder_slot_by_name": (0x93F400, 0xD503237F),
    "game_lift_start": (0x2E50760, None),
    "game_lift_bind": (0x2E50B50, None),
    "game_lift_cancel": (0x2E50920, None),
    "game_drop": (0x301F850, 0xF9413809),
    "game_link_set": (0xBC960, None),
    "game_link_destroy": (0x20B6F0, None),
    "game_link_valid": (0x37A9E0, None),
    "game_gmd_get_bool": (0x20FD90, None),
    "game_link_vtable": (0x4974A90, None),
    "game_link_dead_vtable": (0x4974B20, None),
    "game_lift_param_slot": (0x4C7E618, None),
    "game_wake_vtable_slot": (0x4C7E0A0, None),
    "game_wake_vec_slot": (0x4C7E0B8, None),
    "game_gmd_slot": (0x4C7E180, None),
    "glider_state": (STATE, None),
}

# game call sites that use each routine the way the code does: (site, routine)
PRECEDENTS = [
    (0x27E2CCC, 0xBC960), (0x27E2CE0, 0x2E50760), (0x27E2CF4, 0x2E50B50), (0x27E2D10, 0x20B6F0),  # bomb pull-out
    (0x27E5690, 0x301F850), (0x27E5698, 0x2E50920),  # carry action's leave
    (0x27E5700, 0x37A9E0),  # carry action's "still holding" test
]

CFLAGS = ["-std=gnu17", "-Os", "-ffreestanding", "-fno-builtin", "-fno-pic", "-mcmodel=small",
          "-fno-jump-tables", "-fno-tree-switch-conversion", "-fno-tree-loop-distribute-patterns",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-stack-protector",
          "-mno-outline-atomics", "-march=armv8.2-a", "-Wall", "-Wextra", "-Werror"]

LINK_SCRIPT = """
SECTIONS {
  . = %#x;
  .text : { *(.text*) . = ALIGN(4); *(.rodata*) . = ALIGN(4); }
  .stray : { *(.data*) *(.bss*) *(COMMON) *(.got*) *(.igot*) *(.iplt*) *(.rela*) *(.init_array*) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) }
}
"""


def murmur3_32(data: bytes, seed: int = 0) -> int:
    h = seed
    for i in range(0, len(data) & ~3, 4):
        k = (struct.unpack_from("<I", data, i)[0] * 0xCC9E2D51) & 0xFFFFFFFF
        k = (((k << 15) | (k >> 17)) * 0x1B873593) & 0xFFFFFFFF
        h ^= k
        h = (((h << 13) | (h >> 19)) * 5 + 0xE6546B64) & 0xFFFFFFFF
    k = 0
    for j, byte in enumerate(data[len(data) & ~3:]):
        k |= byte << (8 * j)
    if k:
        k = (k * 0xCC9E2D51) & 0xFFFFFFFF
        k = (((k << 15) | (k >> 17)) * 0x1B873593) & 0xFFFFFFFF
        h ^= k
    h ^= len(data)
    h = ((h ^ (h >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def main_checkout() -> Path:
    if (patchkit.WORK / "main.bin").exists():
        return patchkit.REPO
    common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                     cwd=HERE, text=True).strip()
    return Path(common).parent


def only_reference(image: bytes, routine: int, slots: tuple[int, ...]) -> None:
    import numpy as np
    words = np.frombuffer(image[:len(image) // 8 * 8], dtype="<u8")
    stored = [int(i) * 8 for i in np.nonzero(words == routine)[0]]
    if stored != list(slots):
        raise SystemExit(f"0x{routine:X} is stored at {[hex(s) for s in stored]}, expected {[hex(s) for s in slots]}")
    insns = np.frombuffer(image[:patchkit.TEXT_SIZE // 4 * 4], dtype="<u4").astype(np.int64)
    sites = np.arange(len(insns), dtype=np.int64) * 4
    branch = (insns & 0x7C000000) == 0x14000000  # b and bl
    offset = ((insns & 0x3FFFFFF) ^ 0x2000000) - 0x2000000
    targets = sites + offset * 4
    hits = sites[branch & (targets >= routine) & (targets < routine + 8)]
    if len(hits):
        raise SystemExit(f"0x{routine:X}: branched to from {[hex(int(h)) for h in hits[:5]]}")


class Kit(PatchKit):
    def _records(self) -> list[tuple[int, bytes]]:
        out: list[tuple[int, bytes]] = []
        for addr, data in super()._records():
            out += [(addr + i, data[i:i + 64]) for i in range(0, len(data), 64)]
        return out


def run(tool: Path, *args: object) -> str:
    return subprocess.run([str(tool), *map(str, args)], check=True, capture_output=True, text=True).stdout


def compile_code(root: Path, obj: Path) -> tuple[bytes, dict[str, int]]:
    tools = root / "vendor/totk-dkp/root/opt/devkitpro/devkitA64/bin"
    gcc, ld = tools / "aarch64-none-elf-gcc.exe", tools / "aarch64-none-elf-ld.exe"
    objcopy, nm, readelf = (tools / f"aarch64-none-elf-{t}.exe" for t in ("objcopy", "nm", "readelf"))
    obj.mkdir(parents=True, exist_ok=True)
    defines = [f"-DGLIDER_OWN_HASH=0x{murmur3_32(OWN_FLAG.encode()):08X}u",
               f"-DGLIDER_CHEST_HASH=0x{murmur3_32(CHEST_FLAG.encode()):08X}u"]
    objects = []
    for source in ("glider.c", "hooks.S"):
        out = obj / (source + ".o")
        run(gcc, "-c", HERE / "code" / source, "-o", out, *CFLAGS, *defines)
        relocs = run(readelf, "-r", out)
        if "R_AARCH64_ABS" in relocs:
            raise SystemExit(f"{source}: absolute relocation (a stored pointer); the code must stay PC-relative")
        objects.append(out)
    script = obj / "link.ld"
    script.write_text(LINK_SCRIPT % CODE, encoding="ascii")
    elf = obj / "glider.elf"
    run(ld, "-T", script, "--no-warn-rwx-segments", *(f"--defsym={n}={a:#x}" for n, (a, _) in GAME.items()),
        "-o", elf, *objects)
    for line in run(readelf, "-S", "-W", elf).splitlines():
        parts = line.split()
        if ".stray" in parts and int(parts[parts.index(".stray") + 4], 16) != 0:
            raise SystemExit("the code has writable data, a GOT or stored pointers; it must be code and constants only")
    blob_path = obj / "glider.bin"
    run(objcopy, "-O", "binary", "-j", ".text", elf, blob_path)
    symbols = {}
    for line in run(nm, elf).splitlines():
        addr, _, name = line.split()
        symbols[name] = int(addr, 16)
    return blob_path.read_bytes(), symbols


def main() -> None:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()
    if murmur3_32(b"Kokko") != 1577663790:  # the cucco's LiftableType value in its actor pack
        raise SystemExit("murmur3 self-check failed")
    root = main_checkout()
    work = root / "work" / "switch2" / "oot-remake"
    kit = Kit("paraglider", "Paraglider", nso=work / "main.uncompressed.nso", image_path=work / "main.bin")

    manifest = work / "build" / "paraglider" / "data_manifest.json"
    if manifest.exists():
        import json
        names = json.loads(manifest.read_text(encoding="utf-8"))
        if (names["ownership_flag"]["name"], names["chest_flag"]["name"]) != (OWN_FLAG, CHEST_FLAG):
            raise SystemExit("data_manifest.json names other flags than OWN_FLAG and CHEST_FLAG")
        if names["ownership_flag"]["murmur3_32"] != murmur3_32(OWN_FLAG.encode()):
            raise SystemExit("flag hash differs from the data build's")
    blob, symbols = compile_code(root, work / "build" / "paraglider" / "obj")
    if len(blob) % 4 or CODE + len(blob) > CODE_END:
        raise SystemExit(f"code is {len(blob)} bytes; the tail holds {CODE_END - CODE}")
    if any(kit.image[CODE:CODE_END]):
        raise SystemExit("the code tail is not zero in this image")
    if len(kit.image) > STATE and any(kit.image[STATE:STATE + STATE_SIZE]):
        raise SystemExit("the state area is not zero in this image")

    for name, (addr, first) in GAME.items():
        if first is not None:
            kit.expect(addr, first, name)
        elif addr < patchkit.TEXT_SIZE:
            before = kit.word(addr - 4)
            if kit.word(addr) != patchkit.PACIBSP and before != 0 and not patchkit._is_function_end(before):
                raise SystemExit(f"{name} 0x{addr:X} does not look like the start of a routine")
    for site, routine in PRECEDENTS:
        kit.expect(site, patchkit.bl(site, routine).word, f"bl 0x{routine:X}")

    if kit.image[0x4B0C930 + 0x138:0x4B0C930 + 0x140] != (0x2B41580).to_bytes(8, "little"):
        raise SystemExit("SelectorPlayerSubSchedulerMain's vtable is not at 0x4B0C930")
    for routine, first, slots, hook, trampoline_name in DETOURS:
        only_reference(kit.image, routine, slots)
        trampoline = symbols[trampoline_name]
        if struct.unpack_from("<I", blob, trampoline - CODE)[0] != first:
            raise SystemExit(f"{hook}: the trampoline does not start with the replaced instruction")
        if struct.unpack_from("<I", blob, trampoline + 4 - CODE)[0] != patchkit.b(trampoline + 4, routine + 4).word:
            raise SystemExit(f"{hook}: the trampoline does not continue at 0x{routine + 4:X}")
        kit.put(routine, patchkit.b(routine, symbols[hook]), vanilla=first)
    for i in range(0, len(blob), 4):
        kit.put_data(CODE + i, struct.unpack_from("<I", blob, i)[0], vanilla=0)

    kit.write(out=work / "build" / "paraglider", verbose=False)
    print(f"code 0x{CODE:X}..0x{CODE + len(blob):X} ({len(blob)} of {CODE_END - CODE} bytes), state 0x{STATE:X}")
    print(f"flags {OWN_FLAG} = 0x{murmur3_32(OWN_FLAG.encode()):08X}, "
          f"{CHEST_FLAG} = 0x{murmur3_32(CHEST_FLAG.encode()):08X}")
    for routine, _, _, hook, _ in DETOURS:
        print(f"  0x{routine:X}: b {hook} 0x{symbols[hook]:X}")


if __name__ == "__main__":
    main()
