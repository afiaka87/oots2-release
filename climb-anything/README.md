# Climb Anything

Link can climb any wall, not only the vine and ladder surfaces the game marks as climbable.

## What changes

- Surfaces marked for climbing can be climbed whatever their material, not only grass, snow or stone.
- Unmarked walls, scenery included, become climbable.
- Ladders keep their normal ladder climbing. Slippery slopes, pits and characters stay unclimbable.
- Once a climb starts, the climbing check accepts the surface, so the climb continues.

Walking into any wall may now start a climb, including low ledges and furniture Link would normally
clamber onto or bump against. Floors and gentle slopes stay walkable.

## Status

This build has not been tested in game yet. An earlier build changed only the check that runs once a
climb is under way, and it did not start a climb on the walls tried. This build also changes the
check that decides whether to start climbing.

The changed check has no Link-only guard. If another character's wall detection shares it, that
character could also start climbing walls.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii, right-click the game and choose Open Mod Data Location).
3. Create `Climb Anything/exefs/` there and copy `patches/climb-anything.pchtxt` into it.
4. Enable Climb Anything in the game's Add-Ons and start the game.

## Compatibility

- Code patch only: no game files are replaced and no executable module or subsdk slot is used.
- Added code sits in an unused gap between the game's functions. Each patch mod in this repository
  uses its own gaps, so Climb Anything can be enabled with the others.

## Patch source

`patches/climb-anything.pchtxt` is the version-locked patch source: 18 instruction words, with the
added code in 1 unused gap. Every patched address is listed in the file. It contains no game assets.

License: MIT.
