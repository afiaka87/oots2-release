# Camera Follow

The camera goes back to following Link when you let go of the right stick, as in the older versions of
Ocarina of Time. The right stick still turns the camera as in the remake.

## What changes

- In the remake, once the right stick turns the camera, it stays there until ZL is pressed. With this
  mod, letting go of the stick returns the camera to its normal follow: as Link moves, it swings round
  behind his direction of travel.
- The same applies after Z-targeting, which also leaves the remake's camera where it was. The patch
  covers pushing a block, the fishing zoom and riding Epona the same way; those were not tested in game.
- The right stick, ZL, first-person aiming, gyro aiming, conversations and cutscenes work as before.

## Status

Tested in game on the emulator (Fritchii v5.8.4 on Windows):

- Version 2.0.0: after the right stick turns the camera, letting go and running brings it back behind
  Link, and after Z-targeting it follows again without ZL.
- Version 1.0.0: the patch loads, and the minimap's camera marker showed the same behavior. In this
  emulator the 1.0.0 game picture was black, also with no mods enabled, so it was not checked on screen.

Not tested: riding Epona, or Switch 2 hardware.

## Requirements and installation

Supports two game versions, one patch file each: version 1.0.0 (no update; main executable build ID
`33FA6C2C23848282A77E8B88A0A93F4887C55920`) and the v131072 update, version 2.0.0 (build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). The emulator applies only the file whose build ID matches the
game it starts, so both can stay in the folder. Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii or Fritchii, right-click the game and choose Open Mod Data
   Location).
3. From the release ZIP, copy the `Camera Follow` folder there. From this repository, create
   `Camera Follow/exefs/` there and copy both files from `patches/` into it.
4. Enable Camera Follow in the game's Add-Ons and start the game.

## Compatibility

- Code patch only: no game files are replaced, no code is added and no executable module or subsdk
  slot is used.
- It shares no patched address with the other patch mods in this repository.

## Patch source

`patches/camera-follow-1.0.0.pchtxt` and `patches/camera-follow-2.0.0.pchtxt` are the version-locked patch
source: the same 5 instruction words changed in place, at each version's addresses. Every patched address is
listed in the files. They contain no game assets.

License: MIT.
