# Fast Travel v1.2.0

Fast Travel adds an L3 menu to Ocarina of Time on Nintendo Switch 2. Pick an area and an arrival spot to warp there as young or adult Link.

| Download | Destinations | Install file |
| --- | --- | --- |
| Fast Travel (Visited Patch) v1.2.0 | Spots Link has reached in the current save | `Fast Travel (Visited Patch)/exefs/fast-travel-patch.pchtxt` |
| Fast Travel (Visited) v1.1.0 | Spots Link has reached in the current save | `Fast Travel (Visited)/exefs/subsdk5` |
| Fast Travel v1.0.0 | All 214 spots from the start | `Fast Travel/exefs/subsdk5` |

The v1.2.0 patch uses no subsdk slot. It reads the existing Visited version's `fasttravel.sav` records. See the [v1.1.0 release](https://github.com/afiaka87/oots2-release/releases/tag/fast-travel/v1.1.0) for the older module.

## Requirements and install

Emulator only. The visited patch was tested on Windows in Lichii nx2-build-130 from the game list. It targets game version 2.0.0 (update v131072), main build ID `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`.

1. Close the game. In Lichii, right-click it and choose **Open Mod Data Location**.
2. Extract the ZIP and copy the `Fast Travel (Visited Patch)` folder to that location.
3. Enable **Fast Travel (Visited Patch)** and disable the older Fast Travel add-ons, Mirror Mode, and Graphics-Menu for this setup. The patch and those mods can use the same game hooks.
4. Start the game from the game list. Press L3 (click the left stick) after loading a save.

## Using the menu

- Use D-pad up/down to move and left/right to page. Press A to open an area or warp to the selected spot. Press B to go back or L3 to close the menu.
- Press X to switch between young and adult destinations. The mod goes through the Temple of Time when a warp needs an age change. Changing age in a save that has never been adult still needs testing.
- Select **Autosave** and press A to toggle automatic saves for the session. The mod also holds automatic saves partway through an age change.
- A warp can wait while the game loads or a scene plays. The menu says why. A warp into a boss room can start the fight if the boss is still alive.

## Visited destinations

Each save has its own `fasttravel.sav` alongside `progress.sav`. The list fills as Link reaches arrival points or goes through loading zones. For spots without a named arrival point, such as the Lost Woods clearings, the mod checks whether Link comes within 5 m. A Fast Travel warp adds no destinations.

It works best with a new save or one that already has a record from the older Visited version. A save without a visit record starts with an empty list, and the adult list opens after Link first grows up. Arrivals by warp song or owl and copying a save slot still need testing. Removing the mod leaves its record files beside the saves.

## Source and local build

The feature source is in `src/program/`. The patch builder, startup code, and hook adapters are in `patch/`. The game calls are resolved against your own dump. The tested visited patch has SHA-256 `b22ca4636fe3a951c963179d5e96cb842bbf992264df2221692de935f67673ac`.

The build used devkitA64 GCC 16.1.0. Prepare a local input directory containing flattened `main.bin` and `sdk.bin` at their NSO memory offsets, `main.uncompressed.nso` with its valid NSO header, and the original SDK NSO at `extract/exefs/sdk`. Keep these inputs outside the source checkout. From the Fast Travel folder, run the following with your paths.

```powershell
uv run --no-project --with capstone --with pyelftools --with lz4 --with numpy python patch/build_patch.py --game-dir <local-input-directory> --tool-bin <devkitA64-bin> --output-dir <local-output-directory>
```

The output directory contains `fast-travel-patch/exefs/fast-travel-patch.pchtxt` and a separate IPS32 fallback. Use the pchtxt for installation. `--all-places` builds an experimental patch for the all-places version with offline verification. `--compile-only` stops after creating the relocatable object and listing its imports.

The source is MIT licensed. The font is font8x8_basic by Daniel Hepper, in the public domain. See `NOTICE.txt` for credits and the legacy module dependency.
