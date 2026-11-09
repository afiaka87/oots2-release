# Mirror Mode v1.0.1

Shows the 3D world of Ocarina of Time on Nintendo Switch 2 flipped left to right. The HUD, dialog
and menus are not flipped.

## What is mirrored

- The world picture. It is flipped just before the HUD layer draws, so the HUD and text boxes
  draw on top of it normally.
- Both sticks during gameplay: Link's movement and the camera. Menus keep normal directions.
- Gyro aiming (slingshot, bow, hookshot and other motion aiming). Left and right turning is
  reversed; up and down is unchanged.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Each hook site is checked at startup; on any other
build the mod installs nothing and the game runs normally.

Emulator only. Tested in Lichii nx2-build-117 on Windows; not tested on Switch 2 hardware.

1. Close the game. If v1.0.0 is installed, delete its `Mirror Mode` folder.
2. Extract the archive and copy its `Mirror Mode` folder into the game's mod folder (in Lichii,
   right-click the game and choose Open Mod Data Location).
3. Confirm that the installed file is `Mirror Mode/exefs/subsdk4`. No `main.npdm` is included or
   needed.
4. Enable Mirror Mode in the game's Add-Ons and start the game from the game list.

Mirror Mode uses `subsdk4`, so it can run with a mod that supplies `subsdk9` (tested with
Graphics-Menu). It cannot be combined with another mod that supplies `subsdk4`.

## Known issues

- Switching between docked and handheld mode during play has not been tested.
- The module writes short status lines to the emulator log: startup checks and occasional
  counters.

## Source and dependencies

The public source is `src/program/main.cpp` under the MIT License. It is source only and is not a
standalone build tree. Native builds require devkitA64 and exlaunch configured for module name
`ootmirror`, fake heap enabled, the SVC debug-string logger, `HeapSize 0x10000`, `JitSize 0x1000`,
`InlinePoolSize 0x1000`, `LogBufferSize 512`, an empty relocation table set, one default game
version and subsdk slot 4. The released `subsdk4` has SHA-256
`c0020990fa20aa016ca3c8de2fabc2cb3565da08d60f35d775889be90c5b567f`.

exlaunch, SDK headers and game assets are not included. The compiled module includes exlaunch
(GPL-2.0); see `NOTICE.txt`.
