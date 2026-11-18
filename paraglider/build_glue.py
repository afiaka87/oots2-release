# /// script
# requires-python = ">=3.11"
# dependencies = ["zstandard", "texture2ddecoder", "pillow", "numpy"]
# ///
"""Last game-file step of the Paraglider mod (Ocarina of Time remake, Switch 2, 2.0.0), after assets/ and data/.

Adds the paraglider to each age's holder list (a copy of the bomb's entry: adult Obj_Paraglider, child
Obj_Paraglider_Child) and writes both resource size tables with a budget for every added or enlarged file.
Other add-ons that ship the size tables cannot be enabled with this one.

  uv run build_glue.py --game-romfs <full 2.0.0 romfs> [--romfs <mod romfs>]
"""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import zstandard

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "data"))
sys.path.insert(0, str(HERE.parents[2] / "tools"))
import gbyml  # noqa: E402
import restbl_raise  # noqa: E402
from build_data import Game, byml_bytes, sarc_read, sarc_write  # noqa: E402
from byml7 import U32  # noqa: E402

BASE_HOLDER = "Component/ActorHolderParam/Player.game__component__ActorHolderParam.bgyml"
AGE_HOLDER = "Component/ActorHolderParam/{}.game__component__ActorHolderParam.bgyml"
AGE_PACKS = ("Player_Adult", "Player_Child")
HOLD_NAME = "Obj_Paraglider"  # entry name; code/glider.c asks for this slot
HOLD_ACTOR = "Work/Actor/{}.engine__actor__ActorParam.gyml"
CHILD_ACTOR = "Obj_Paraglider_Child"
TABLE_DIR = "System/Resource"


def die(msg: str):
    raise SystemExit(msg)


def main_work() -> Path:
    common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                     cwd=HERE, text=True).strip()
    return Path(common).parent / "work" / "switch2" / "oot-remake"


def holder_packs(game: Game, romfs: Path, child_actor: str) -> None:
    """Write the two age packs with the paraglider in that age's holder list."""
    for pack, actor in zip(AGE_PACKS, (HOLD_NAME, child_actor)):
        rel = f"Pack/Actor/{pack}.pack.zs"
        raw, dict_id = game.load(rel)
        members = sarc_read(raw)
        if sarc_write(dict(members)) != raw:
            die(f"{rel}: the archive writer does not reproduce the game's file")
        files = dict(members)
        name = AGE_HOLDER.format(pack)
        if name not in files or BASE_HOLDER not in files:
            die(f"{rel}: holder list files missing")
        version, big, tree = gbyml.load(files[name])
        if gbyml.dump(tree, version, big) != files[name]:
            die(f"{rel}: the BYML writer does not reproduce {name}")
        base = gbyml.load(files[BASE_HOLDER])[2]["ActorHoldSettings"]
        bomb = base["Item_Bomb"]
        settings = tree["ActorHoldSettings"]
        if tree.get("$parent") != "Work/" + BASE_HOLDER[:-len(".bgyml")] + ".gyml" or HOLD_NAME in settings                 or HOLD_NAME in base or set(bomb) != {"Actor", "DependencyWithHolder", "IsAutoRecreate",
                                                      "IsEnableSleepDeleteWithHolder", "IsEnableSyncModelCtrlParent",
                                                      "MaxActors", "ResidentTypeIfAlsoHolder"}:
            die(f"{rel}: the holder list is not the one this edit was written for")
        if not isinstance(bomb["DependencyWithHolder"], U32):
            die("bomb entry types changed")
        entry = dict(bomb)
        entry["Actor"] = HOLD_ACTOR.format(actor)
        entry["MaxActors"] = 1
        settings[HOLD_NAME] = entry
        tree["ActorHoldSettings"] = dict(sorted(settings.items()))
        new = byml_bytes(tree, version, big)
        if gbyml.load(new)[2] != tree:
            die(f"{rel}: the edited list does not read back")
        files[name] = new
        out = romfs / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(game.rezs(sarc_write(files), dict_id))
        print(f"{rel}: {name} {len(dict(members)[name]):,} -> {len(new):,} bytes, holds {actor}")


def lookup(table: bytes, key: str) -> int | None:
    at = restbl_raise.budget_offset(table, key)
    return None if at is None else struct.unpack_from("<I", table, at)[0]


def set_budget(table: bytearray, key: str, budget: int) -> None:
    at = restbl_raise.budget_offset(bytes(table), key)
    if at is None:
        count = struct.unpack_from("<I", table, 14)[0]
        crc = zlib.crc32(key.encode())
        lo, hi = 0, count
        while lo < hi:
            mid = (lo + hi) // 2
            if struct.unpack_from("<I", table, restbl_raise.HEADER + mid * 8)[0] < crc:
                lo = mid + 1
            else:
                hi = mid
        table[restbl_raise.HEADER + lo * 8:restbl_raise.HEADER + lo * 8] = struct.pack("<II", crc, budget)
        struct.pack_into("<I", table, 14, count + 1)
    else:
        struct.pack_into("<I", table, at, budget)


def size_tables(game: Game, game_romfs: Path, romfs: Path) -> list[dict]:
    """Budgets for every file under romfs that is new or differs from the game's copy."""
    wanted: list[tuple[str, int, int | None, bool]] = []  # key, size, the game's size, inside a pack
    for path in sorted(romfs.rglob("*.zs")):
        rel = path.relative_to(romfs).as_posix()
        if rel.startswith(TABLE_DIR + "/"):
            continue
        raw, _ = game.unzs(path.read_bytes())
        vanilla = game.load(rel)[0] if game.has(rel) else None
        if vanilla == raw:
            die(f"{rel}: identical to the game's file; it should not be in the mod")
        wanted.append((rel[:-3], len(raw), None if vanilla is None else len(vanilla), False))
        if rel.endswith(".pack.zs"):
            old = dict(sarc_read(vanilla)) if vanilla is not None else {}
            for name, data in sarc_read(raw):
                if old.get(name) != data:
                    wanted.append((name, len(data), len(old[name]) if name in old else None, True))
    tables = sorted((game_romfs / TABLE_DIR).glob(restbl_raise.TABLE_GLOB))
    if len(tables) != 2:
        die(f"expected two size tables under {game_romfs / TABLE_DIR}")
    report = []
    for table_path in tables:
        original = zstandard.ZstdDecompressor().decompress(table_path.read_bytes(), max_output_size=1 << 28)
        if original[:6] != b"RESTBL":
            die(f"{table_path.name}: not a RESTBL")
        table = bytearray(original)
        added = raised = 0
        for key, size, vanilla_size, inner in wanted:
            old = lookup(original, key)
            aligned = size + 0x1F & ~0x1F
            if old is None:
                new = int(aligned * 2.5) + 0x10000 & ~0x1F
                added += 1
            elif inner and vanilla_size is not None:
                new = max(old, old + 8 * max(0, size - vanilla_size) + 0x400)
            else:
                new = max(old, aligned + 0x1000)
            if old is not None and new != old:
                raised += 1
            if new != old:
                set_budget(table, key, new)
            if lookup(bytes(table), key) != new:
                die(f"{table_path.name}: {key}: budget did not take")
            report.append({"table": table_path.name, "name": key, "size": size, "game_size": vanilla_size,
                           "old_budget": old, "budget": new})
        if len(table) != len(original) + 8 * added:
            die(f"{table_path.name}: unexpected table length")
        crcs = [struct.unpack_from("<I", table, restbl_raise.HEADER + 8 * i)[0]
                for i in range(struct.unpack_from("<I", table, 14)[0])]
        if crcs != sorted(set(crcs)):
            die(f"{table_path.name}: the name-hash column is no longer sorted and unique")
        blob = zstandard.ZstdCompressor(level=19).compress(bytes(table))
        if zstandard.get_frame_parameters(blob).dict_id != 0 or \
                zstandard.ZstdDecompressor().decompress(blob, max_output_size=1 << 28) != table:
            die(f"{table_path.name}: compressed table does not round-trip as plain zstd")
        out = romfs / TABLE_DIR / table_path.name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(blob)
        print(f"{table_path.name}: {added} added, {raised} raised, {len(wanted) - added - raised} already large enough")
    return report


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-romfs", type=Path, required=True, help="the full extracted 2.0.0 romfs")
    ap.add_argument("--work", type=Path, help="work/switch2/oot-remake (default: the main checkout's)")
    ap.add_argument("--romfs", type=Path, help="the mod's romfs folder (default <work>/build/paraglider/romfs)")
    ap.add_argument("--child-actor", help=f"actor child Link holds (default {CHILD_ACTOR} when the mod has "
                                          f"its pack, else {HOLD_NAME})")
    args = ap.parse_args()
    romfs = args.romfs or (args.work or main_work()) / "build" / "paraglider" / "romfs"
    if not romfs.is_dir():
        die(f"{romfs} does not exist; run data/build_data.py and the assets build first")
    game = Game([args.game_romfs], "game")
    child = args.child_actor or (CHILD_ACTOR if (romfs / f"Pack/Actor/{CHILD_ACTOR}.pack.zs").exists() else HOLD_NAME)
    for actor in {HOLD_NAME, child}:
        if not (romfs / f"Pack/Actor/{actor}.pack.zs").exists():
            die(f"{romfs}: no Pack/Actor/{actor}.pack.zs; build the held actor first")
    holder_packs(game, romfs, child)
    report = size_tables(game, args.game_romfs, romfs)
    (romfs.parent / "size_manifest.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    for row in report:
        if row["table"].endswith("111.Nin_Ounce_NVN2.rsizetable.zs"):
            print(f"  {row['name']}: {row['size']:,} bytes, budget {row['old_budget']} -> {row['budget']:,}")


if __name__ == "__main__":
    main()
