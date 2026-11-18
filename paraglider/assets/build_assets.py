# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
"""Build the held paraglider actor's game files for the Ocarina of Time remake from TotK's paraglider.

Windows only (drives Switch Toolbox through Windows PowerShell). Writes models, textures, animation and packs into
the output romfs, assets_manifest.json beside it, and rsdb_extra.json beside this script.

  uv run --no-project --with zstandard python assets/build_assets.py \\
      --work <work folder> --game-romfs <full extracted remake romfs> \\
      --totk-romfs <extracted TotK romfs>
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import zstandard

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import actor_pack  # noqa: E402
import anim_curves  # noqa: E402
import textures  # noqa: E402

PREFIX = "Obj_Paraglider"
ROUGHNESS, OCCLUSION = 217, 255  # constant maps: fairly rough (0.85), no occlusion


def die(message: str):
    raise SystemExit(f"build_assets: {message}")


def run(command: list[str], what: str) -> str:
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.returncode:
        tail = "\n".join((result.stdout + result.stderr).strip().splitlines()[-25:])
        die(f"{what} failed:\n{tail}")
    return result.stdout


class Outputs:
    def __init__(self, root: Path):
        self.root, self.rows = root, []

    def add(self, relative: str, blob: bytes, raw: bytes, inner: dict[str, bytes] | None = None) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        self.rows.append({
            "path": relative, "compressed": len(blob), "uncompressed": len(raw),
            "zstd_dictionary": zstandard.get_frame_parameters(blob).dict_id, "sha256": hashlib.sha256(blob).hexdigest(),
            "inner": [{"path": name, "uncompressed": len(data)} for name, data in sorted((inner or {}).items())],
        })
        print(f"  {relative}  {len(raw)} bytes ({len(blob)} compressed)")


def stage_inputs(game: actor_pack.Game, totk: actor_pack.Game, stage: Path, decoder: Path) -> dict[str, Path]:
    """Decoded copies of the files the model library reads (Toolbox reads ../Shader's name table only as plain zstd)."""
    remake, source = stage / "remake-romfs", stage / "totk"
    for folder in (remake / "Model", remake / "Shader", source):
        folder.mkdir(parents=True, exist_ok=True)
    paths = {
        "donor": remake / "Model" / "Animal_Cucco.Animal_Cucco.bfres",
        "native_anim": remake / "Model" / "Animal_Octopus_Animation.anim.bfres",
        "model": source / "Item_Parasail.Item_Parasail.bfres",
        "anim": source / "Item_Parasail.anim.bfres",
    }
    paths["donor"].write_bytes(game.load("Model/Animal_Cucco.Animal_Cucco.bfres.zs")[0])
    paths["native_anim"].write_bytes(game.load("Model/Animal_Octopus_Animation.anim.bfres.zs")[0])
    names = game.load("Shader/ExternalBinaryString.bfres.zs")[0]
    (remake / "Shader" / "ExternalBinaryString.bfres.zs").write_bytes(zstandard.ZstdCompressor(level=3).compress(names))
    paths["anim"].write_bytes(totk.load("Model/Item_Parasail.anim.bfres.zs")[0])
    run([str(decoder), str(totk.path("Model/Item_Parasail.Item_Parasail.bfres.mc")), str(paths["model"])], "decoding TotK's model")
    for path in paths.values():
        if path.read_bytes()[:4] != b"FRES":
            die(f"{path.name} is not a decoded model file")
    return paths


def powershell(script: str, arguments: list[str], what: str) -> str:
    command = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / script), *arguments]
    return run(command, what)


def build_textures(totk: actor_pack.Game, fabric: str) -> list[textures.Texture]:
    def port(source: str, name: str, fmt: int, channels: bytes) -> textures.Texture:
        texture = textures.read_txtg(totk.path(f"TexToGo/{source}.txtg"))
        if texture.format != fmt or texture.size != 256:
            die(f"{source}: format {texture.format:#06x}, size {texture.size}; expected {fmt:#06x}, 256")
        return dataclasses.replace(texture, name=name, channels=channels)

    made = [
        port("Item_Parasail_Alb", f"{PREFIX}_Body_Alb", textures.BC1_SRGB, textures.CHANNELS_RGBA),
        # Vanilla's own BC1 normal maps use the plain red, green, blue, alpha mapping.
        port("Item_Parasail_Nrm", f"{PREFIX}_Body_Nrm", textures.BC1_UNORM, textures.CHANNELS_RGBA),
        port(f"Item_Parasail_Fabric_Alb.{fabric}", f"{PREFIX}_Fabric_Alb", textures.BC1_SRGB, textures.CHANNELS_RGBA),
        port("Item_Parasail_Fabric_Nrm", f"{PREFIX}_Fabric_Nrm", textures.BC5_UNORM, textures.CHANNELS_RG01),
        textures.constant_bc4(f"{PREFIX}_Rgh", ROUGHNESS),
        textures.constant_bc4(f"{PREFIX}_AO", OCCLUSION),
    ]
    for texture in made:
        if textures.parse_bntx(textures.build_bntx(texture)) != texture:
            die(f"{texture.name}: texture does not read back as written")
    return made


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--work", type=Path, required=True, help="the main checkout's work/switch2/oot-remake folder")
    parser.add_argument("--game-romfs", type=Path, action="append",
                        help="extracted remake romfs holding Model/, Tex/ and Pack/ (repeatable; default <work>/extract/romfs)")
    parser.add_argument("--totk-romfs", type=Path, required=True, help="extracted Tears of the Kingdom 1.2.1 romfs")
    parser.add_argument("--out", type=Path, help="output romfs folder (default <work>/build/paraglider/romfs)")
    parser.add_argument("--stage", type=Path, help="folder for intermediate files (default <out>/../assets-stage)")
    parser.add_argument("--animation", choices=("flutter", "cloth", "none"), default="flutter",
                        help="flutter: TotK's canopy motion plus a tail wave; cloth: canopy only; none: a static actor")
    parser.add_argument("--fabric", default="00", help="TotK fabric pattern number (Item_Parasail_Fabric_Alb.NN)")
    parser.add_argument("--toolbox", type=Path, help="Switch Toolbox 2026-11-05 folder (default vendor/external-tools/...)")
    parser.add_argument("--unsafe-dll", type=Path, help="System.Runtime.CompilerServices.Unsafe.dll for Toolbox")
    parser.add_argument("--mc-decoder", type=Path, help="codec-check.exe, which decodes TotK's .bfres.mc")
    args = parser.parse_args()

    repo = args.work.resolve().parents[2]
    out = (args.out or args.work / "build" / "paraglider" / "romfs").resolve()
    stage = (args.stage or out.parent / "assets-stage").resolve()
    toolbox = args.toolbox or repo / "vendor/external-tools/switch-toolbox-2026-11-05"
    unsafe = args.unsafe_dll or repo / "vendor/external-tools/switch-toolbox-compat/System.Runtime.CompilerServices.Unsafe.dll"
    decoder = args.mc_decoder or repo / ".history/champions-poc-deep-20261025/codec-check/build/Release/codec-check.exe"
    for path, what in ((toolbox / "Toolbox.exe", "Switch Toolbox"), (unsafe, "the Unsafe DLL"), (decoder, "the .mc decoder")):
        if not path.is_file():
            die(f"{what} is not at {path}")
    try:
        game = actor_pack.Game([root.resolve() for root in (args.game_romfs or [args.work / "extract" / "romfs"])])
        totk = actor_pack.Game([args.totk_romfs.resolve()])

        print("checks against vanilla files")
        tex_root = game.path(f"Tex/{textures.SELFTEST_NAMES[0]}.bntx.zs").parents[1]
        textures.reproduce(tex_root)
        fits = textures.check_block_order(tex_root)
        actor_pack.selftest(game)
        print(f"  texture writer, archive writer and BYML writer reproduce vanilla; block order fit {max(fits):.2f}")

        print("inputs")
        staged = stage_inputs(game, totk, stage, decoder)
        common = ["-ToolboxDir", str(toolbox), "-UnsafeCompatPath", str(unsafe), "-TotkRomfs", str(totk.roots[0])]
        outputs = Outputs(out)
        animated = args.animation != "none"
        model_dictionary = game.load("Model/Animal_Cucco.Animal_Cucco.bfres.zs")[1]
        anim_dictionary = game.load("Model/Animal_Octopus_Animation.anim.bfres.zs")[1]
        pack_dictionary = game.load("Pack/Actor/Animal_Cucco.pack.zs")[1]
        registration = {"ActorInfo": [], "GameActorInfo": []}

        for variant in actor_pack.VARIANTS:
            name, folder = variant.name, stage / variant.name
            print(name)
            size = ["-OutputDirectory", str(folder), "-Project", name, "-Scale", repr(variant.scale)]
            powershell("port_model.ps1", common + size + [
                "-SourceModel", str(staged["model"]), "-DonorModel", str(staged["donor"]), "-TexturePrefix", PREFIX], f"{name} model")
            receipt = json.loads((folder / "model-receipt.json").read_text(encoding="utf-8-sig"))
            raw = (folder / f"{name}.{name}.bfres").read_bytes()
            outputs.add(f"Model/{name}.{name}.bfres.zs", game.rezs(raw, model_dictionary), raw)

            if animated:
                flutter = ["-Flutter"] if args.animation == "flutter" else []
                powershell("port_anim.ps1", common + size + flutter + [
                    "-SourceAnim", str(staged["anim"]), "-NativeAnim", str(staged["native_anim"])], f"{name} animation")
                anim_path = folder / f"{name}_Animation.anim.bfres"
                anim_path.write_bytes(anim_curves.requantise(anim_path.read_bytes()))
                # The library reads the game's curve form, so this compares the file as the game stores it with the plan.
                powershell("port_anim.ps1", common + ["-OutputDirectory", str(folder), "-Project", name, "-Verify"],
                           f"{name} animation check")
                raw = anim_path.read_bytes()
                outputs.add(f"Model/{name}_Animation.anim.bfres.zs", game.rezs(raw, anim_dictionary), raw)

            files = actor_pack.build_pack(game, variant, animated)
            raw = actor_pack.sarc_write(files)
            if actor_pack.sarc_read(raw) != files:
                die(f"{name}: pack does not read back as written")
            outputs.add(f"Pack/Actor/{name}.pack.zs", game.rezs(raw, pack_dictionary), raw, files)

            # Culling box: the model's own bounds plus room for the tails' swing.
            margin = 0.4 * variant.scale
            bounds = (tuple(round(value - margin, 3) for value in receipt["bounds"]["min"]),
                      tuple(round(value + margin, 3) for value in receipt["bounds"]["max"]))
            for table, row in actor_pack.registration(variant, bounds, animated).items():
                registration[table].append(row)

        print("textures")
        for texture in build_textures(totk, args.fabric):
            raw = textures.build_bntx(texture)
            outputs.add(f"Tex/{texture.name}.bntx.zs", textures.compress(raw), raw)
    except (actor_pack.PackError, textures.TextureError, anim_curves.CurveError) as error:
        die(str(error))

    (HERE / "rsdb_extra.json").write_text(json.dumps(registration, indent=2) + "\n", encoding="utf-8", newline="\n")
    manifest = {"animation": args.animation, "fabric": args.fabric, "files": outputs.rows}
    (out.parent / "assets_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {len(outputs.rows)} files under {out}")
    print(f"table rows: {HERE / 'rsdb_extra.json'}; sizes: {out.parent / 'assets_manifest.json'}")


if __name__ == "__main__":
    main()
