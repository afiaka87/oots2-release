# Fast Travel v1.0.0

Adds a menu to Ocarina of Time on Nintendo Switch 2 that warps Link to places all over Hyrule, as
young Link or as adult Link.

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

The lists hold 214 spots, all points where the game itself places Link in its map files. They are
in `src/program/destinations.inc`, generated from the game's map files by a script that is not
included here because it reads the game's files.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Each hook site is checked at startup; on any other
build the mod installs nothing and the game runs normally.

Emulator only. Tested in Lichii nx2-build-117 on Windows; not tested on Switch 2 hardware.

1. Close the game.
2. Extract the archive and copy its `Fast Travel` folder into the game's mod folder (in Lichii,
   right-click the game and choose Open Mod Data Location).
3. Confirm that the installed file is `Fast Travel/exefs/subsdk5`. No `main.npdm` is included or
   needed.
4. Enable Fast Travel in the game's Add-Ons and start the game from the game list.

Fast Travel uses `subsdk5`, so it can run with a mod that supplies `subsdk9` (tested with
Graphics-Menu). It cannot be combined with another mod that supplies `subsdk5`, or with Mirror
Mode, which hooks the same game routines.

## Known issues

- Some spots are in dungeons and boss rooms. A boss that is not yet beaten may start its fight when
  Link arrives.
- The game does not receive L3 while the mod runs, so any game action on L3 would not work.
- Changing age in a save that has never been adult has not been tested.
- The module writes short status lines to the emulator log: startup checks, menu actions and each
  warp's steps.

## Source and dependencies

The public source is `src/program/` under the MIT License, except `font8x8.hpp`, which is
font8x8_basic by Daniel Hepper (public domain). It is source only and is not a standalone build
tree. Native builds require devkitA64 and exlaunch configured for module name `ootfasttravel`,
fake heap enabled, the SVC debug-string logger, `HeapSize 0x10000`, `JitSize 0x1000`,
`InlinePoolSize 0x1000`, `LogBufferSize 512`, an empty relocation table set, one default game
version and subsdk slot 5. The released `subsdk5` has SHA-256
`0241ce3bc2799c2226224e7c444a59a54cb26de9db98f5c84c9ae18c95b5972a`.

exlaunch, SDK headers and game assets are not included. The compiled module includes exlaunch
(GPL-2.0); see `NOTICE.txt`.
