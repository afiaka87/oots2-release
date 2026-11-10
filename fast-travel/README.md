# Fast Travel v1.2.0

Fast Travel adds an L3 menu to Ocarina of Time on Nintendo Switch 2. Pick an area and an arrival spot to warp there as young or adult Link.

| Download | Destinations | Install file |
| --- | --- | --- |
| Fast Travel (Visited Patch) v1.2.0 | Spots Link has reached in the current save | `Fast Travel (Visited Patch)/exefs/fast-travel-patch.pchtxt` |
| Fast Travel (Visited) v1.1.0 | Spots Link has reached in the current save | `Fast Travel (Visited)/exefs/subsdk5` |
| Fast Travel v1.0.0 | All 214 spots from the start | `Fast Travel/exefs/subsdk5` |

The v1.2.0 patch uses no subsdk slot. It reads the existing Visited version's `fasttravel.sav` records. See the [v1.1.0 release](https://github.com/afiaka87/oots2-release/releases/tag/fast-travel/v1.1.0) for the older module.

## Requirements and install

- Press L3 (click the left stick) to open or close the menu at the bottom left of the screen.
- Each age has its own list: an Autosave row, then the areas. The menu opens on Link's own age's
  list with the cursor on the first area.
- D-pad up and down move the cursor; left and right move a page. On the areas page, A opens the
  area's spots and B closes the menu. On the spots page, A warps there and B goes back to the areas.
- X switches between the young and adult lists. A place from the other age's list takes Link to the
  Temple of Time to change age first, then on to the place.
- A on the Autosave row turns the game's automatic saves off until the game is closed. Automatic
  saves are also held mid-way through an age change.
- While the menu is open, A, B, X, Y and the D-pad go only to the menu. L3 never reaches the game
  while the mod runs.
- A warp waits while a scene plays or the game is still loading; the status line says why.
- The list shows a small picture beside each area, and a large picture of the area under the cursor
  sits on the right with its name and number of spots.

![The menu](assets/menu-preview.png)

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

## The menu's pictures and fonts

The area pictures are illustrations made with an image generator (Magnific, Z-Image model) from a
short description of each place, written without the game's name; `assets/prompts.json` holds the
descriptions. They suggest what each place looks like and are not the game's art: no image from the
game was used to make them. Each picture is named after its area's `GameBancParam` name, and an area
without a picture shows an empty frame.

The text uses Inter and Cinzel (SIL Open Font License 1.1), drawn ahead of time into anti-aliased
glyphs; `tools/fonts/` holds the fonts and their licenses.

## Known issues

The build used devkitA64 GCC 16.1.0. Prepare a local input directory containing flattened `main.bin` and `sdk.bin` at their NSO memory offsets, `main.uncompressed.nso` with its valid NSO header, and the original SDK NSO at `extract/exefs/sdk`. Keep these inputs outside the source checkout. From the Fast Travel folder, run the following with your paths.

```powershell
uv run --no-project --with capstone --with pyelftools --with lz4 --with numpy python patch/build_patch.py --game-dir <local-input-directory> --tool-bin <devkitA64-bin> --output-dir <local-output-directory>
```

The output directory contains `fast-travel-patch/exefs/fast-travel-patch.pchtxt` and a separate IPS32 fallback. Use the pchtxt for installation. `--all-places` builds an experimental patch for the all-places version with offline verification. `--compile-only` stops after creating the relocatable object and listing its imports.

The public source is `src/program/` under the MIT License, except `ext/picojpeg.c` and
`ext/picojpeg.h`, which are picojpeg by Rich Geldreich (public domain). It is source only and is not a
standalone build tree. Native builds require devkitA64 and exlaunch configured for module name `ootfasttravel`,
fake heap enabled, the SVC debug-string logger, `HeapSize 0x10000`, `JitSize 0x1000`,
`InlinePoolSize 0x1000`, `LogBufferSize 512`, an empty relocation table set, one default game
version and subsdk slot 5. The Visited version is the same build with `FAST_TRAVEL_VISITED_ONLY=1`
defined, which enables `visits.cpp`.

The menu's pictures and glyphs are built into the module. `python tools/make_assets.py assets/areas
<exlaunch>/data/assets.bin` writes them into exlaunch's `data/` folder, which its build links in with
bin2o; the module reads the blob between `assets_bin` and `assets_bin_end` (current devkitPro bin2s
puts the size only in the generated header). The module is then about 3.3 MB, and the panel image,
its CPU copy and the decoded pictures take about 6.5 MB of the module's own memory. `tools/preview.cpp`
draws the panel on a PC from the same code, to check the layout without the game.

Released modules (`subsdk5`, SHA-256):

- Fast Travel v1.0.0: `0241ce3bc2799c2226224e7c444a59a54cb26de9db98f5c84c9ae18c95b5972a`. Built
  from this source, its code and data are unchanged; only the build ID differs.
- Fast Travel (Visited) v1.1.0:
  `f0a727d88861b950c4fda075506af65515b494592d61dc44fd89c2c0f7959184`.

exlaunch, SDK headers and game assets are not included. The compiled module includes exlaunch
(GPL-2.0); see `NOTICE.txt`.

The source is MIT licensed. The font is font8x8_basic by Daniel Hepper, in the public domain. See `NOTICE.txt` for credits and the legacy module dependency.
