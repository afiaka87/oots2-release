# Paraglider v0.1.1

Adds the paraglider from Tears of the Kingdom to the Ocarina of Time remake on Nintendo Switch 2. Link finds it in a
new treasure chest in Hyrule Field. With it, jump and press jump again in mid-air: Link holds the paraglider over his
head and floats down slowly, the way he does with a cucco.

## What changes

- A large treasure chest in Hyrule Field, near the river a short walk from the drawbridge to Castle Town, for young
  and adult Link. It plays the game's item-get scene and adds the Paraglider to the key items.
- With the Paraglider: jump, then press jump again while in the air. Link opens it and floats down.
- It closes when Link lands, or when jump is pressed again while it is open.
- It opens only with empty hands or with the sword out, not while Link holds something else.
- The item's name and text are in English in every language.

## Status

Tested in game on the emulator (Fritchii v5.8.4 on Windows) with young and adult Link: the chest in Hyrule Field,
opening the paraglider, its animation, the slow fall and putting it away. The item-get scene was tested with young
Link. Not tested: adult Link opening the chest (the test save had already opened it), use together with other mods,
and Switch 2 hardware. The v0.1.1 fix was checked in game with adult Link floating.

## Changes

- v0.1.1: adult Link's paraglider was turned about 20 degrees to his left; it now faces forward. Nothing else
  changed.
- v0.1.0: first release.

## Requirements and installation

Supports only the game's v131072 update (version 2.0.0; main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only.

The mod needs game files as well as the patch, so install it from the release ZIP on GameBanana. This folder holds
the patch and the source, not the game files.

1. Close the game.
2. Open the game's mod folder (in Lichii or Fritchii, right-click the game and choose Open Mod Data Location).
3. From the release ZIP, copy the `Paraglider` folder there.
4. Enable Paraglider in the game's Add-Ons and start the game from the game list. In Fritchii a game-list launch
   loads both the mod's game files and its patch.

## Compatibility

- The mod replaces whole game files: `GameData/GameDataList`, the 16 `Mals` text archives, the `ActorInfo`,
  `GameActorInfo` and `PouchActorInfo` tables, both resource size tables, the two Hyrule Field `_Dynamic` map files,
  and the `Player_Adult` and `Player_Child` packs. Any other mod that replaces one of these files is not compatible,
  for example the Entrance Randomizer.
- Its code patch uses the unused space at the end of the game's code, as Alpha's Air Strafing does, so the two
  cannot be enabled together. It shares no patched address with the other patch mods in this repository and uses no
  subsdk slot.

## Source and local build

The source is under the MIT License. It is source only: the scripts expect the folder layout of the author's working
tree and are not guaranteed to run as they are. No game files are included; every game file in the release is built
from the player's own dumps of both games.

| Path | What it is |
| --- | --- |
| `patches/paraglider.pchtxt` | The released code patch (SHA-256 `37eef9f765ed359b1a322f3704795864ab09375e36a9726ef764789cf8fe2274`) |
| `code/glider.c`, `code/hooks.S` | The added code: the second jump press, handing Link the paraglider, putting it away |
| `build_patch.py` | Compiles the code into the unused tail of the game's code and writes the pchtxt |
| `test_patch.py` | Runs the compiled code in a CPU emulator with the game routines it calls modelled in Python |
| `assets/` | Converts TotK's paraglider model, textures and animation and writes the two held actors (adult and child size) |
| `data/` | The item, the chest, its text and icons, and the saved flags |
| `build_glue.py` | Adds the paraglider to Link's packs and writes both resource size tables |
| `tools/` | Shared helpers: the patch builder (`patchkit.py`), BYML version 7, size tables, texture block order, and the flag-name hash |

Build order for the game files: `assets/build_assets.py`, then `data/build_data.py`, then `build_glue.py`. Each
script's first lines give its command. What they need:

- Python with `uv`, and the packages `zstandard`, `numpy`, `pillow`, `texture2ddecoder`, `capstone` and `unicorn`.
- The helper modules in `tools/` on the import path (for example `PYTHONPATH=tools`). `assets/textures.py` loads
  `swizzle.py` by file path from `scripts/totk_mod_tools/textures/`; copy `tools/swizzle.py` there or change
  `_load_swizzle`. `tools/oot_save.py` holds only the hash function the builders use.
- The code patch: devkitA64 GCC (`aarch64-none-elf`) and the flattened 2.0.0 `main` as `main.bin`; `build_patch.py`
  and `tools/patchkit.py` give the paths they read.
- The model and animation steps: Windows, Switch Toolbox (the 2026-11-05 build, driven through Windows PowerShell),
  and a decoder for TotK's `.bfres.mc` models. The item icon step: `astcenc`.
- Your own dumps: the remake's full 2.0.0 romfs and TotK 1.2.1's romfs.

## Credits

- HanSoloBlaster, afiaka87.
- The paraglider's model, textures, animation and icon art come from The Legend of Zelda: Tears of the Kingdom
  (Nintendo). The scripts convert them from the player's own dump; they are not in this repository.

License: MIT for the code and build scripts.
