# Fast Travel v1.1.0

Adds a menu to Ocarina of Time on Nintendo Switch 2 that warps Link to places all over Hyrule, as
young Link or as adult Link.

It comes in two versions. Install one of them, not both:

- **Fast Travel** (v1.0.0, unchanged) lists all 214 spots from the start.
- **Fast Travel (Visited)** (new in v1.1.0) lists only the spots Link has already reached in that
  save. Its lists start empty and fill in as Link travels.

## Using it

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

The lists hold 214 spots, all points where the game itself places Link in its map files. They are
in `src/program/destinations.inc`, generated from the game's map files by a script that is not
included here because it reads the game's files.

## What the Visited version lists

- A spot is listed once Link has reached it:
  - by arriving there the game's own way: a loading zone, a warp song, an owl or a story scene;
  - by going through the loading zone beside it, from either side. Walking from Kokiri Forest into
    the Lost Woods lists the Lost Woods' "From Kokiri Forest" and Kokiri Forest's "From Lost Woods";
  - or, for the 24 spots with no arrival point of their own (such as the Lost Woods clearings), by
    coming within 5 m of it.
- An area appears once the game has recorded it as visited and one of its spots is listed. The
  adult list stays empty until Link first grows up. Autosave is always there.
- A Fast Travel warp adds nothing new.
- The list belongs to the save. It is kept in a 1 KB file, `fasttravel.sav`, beside each save
  slot's `progress.sav`, written when the game saves that slot and read when it loads it. A new
  game starts with an empty list.
- It works best on a new save. A save made before this version was installed starts with an empty
  list, so places visited earlier have to be visited again before they are listed.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Each hook site is checked at startup; on any other
build the mod installs nothing and the game runs normally.

Emulator only. Fast Travel was tested in Lichii nx2-build-117 and Fast Travel (Visited) in
Fritchii v4.1, both on Windows; neither has been tested on Switch 2 hardware.

1. Close the game.
2. Extract the archive and copy its `Fast Travel` or `Fast Travel (Visited)` folder into the
   game's mod folder (in Lichii, right-click the game and choose Open Mod Data Location).
3. Confirm that the installed file is `Fast Travel/exefs/subsdk5` or
   `Fast Travel (Visited)/exefs/subsdk5`. No `main.npdm` is included or needed.
4. Enable that folder in the game's Add-Ons, keep the other version disabled, and start the game
   from the game list.

Both versions use `subsdk5`, so either can run with a mod that supplies `subsdk9` (tested with
Graphics-Menu). Neither can be combined with the other version or with another mod that supplies
`subsdk5`.

## The menu's pictures and fonts

The area pictures are illustrations made with an image generator (Magnific, Z-Image model) from a
short description of each place, written without the game's name; `assets/prompts.json` holds the
descriptions. They suggest what each place looks like and are not the game's art: no image from the
game was used to make them. Each picture is named after its area's `GameBancParam` name, and an area
without a picture shows an empty frame.

The text uses Inter and Cinzel (SIL Open Font License 1.1), drawn ahead of time into anti-aliased
glyphs; `tools/fonts/` holds the fonts and their licenses.

## Known issues

- Some spots are in dungeons and boss rooms. A boss that is not yet beaten may start its fight when
  Link arrives.
- The game does not receive L3 while the mod runs, so any game action on L3 would not work.
- Changing age in a save that has never been adult has not been tested.
- Visited version: arriving by warp song or owl, the Lost Woods clearings and copying a save slot
  have not been tested.
- Removing the Visited version leaves a `fasttravel.sav` in each save slot folder it wrote.
- The module writes short status lines to the emulator log: startup checks, menu actions, each
  warp's steps and, in the Visited version, each spot reached and each save file opened.

## Source and dependencies

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
