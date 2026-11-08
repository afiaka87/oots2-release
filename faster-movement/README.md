# Faster Movement

Link moves faster, with his animations sped up to match. An optional add-on speeds up walking,
rolling and climbing directly.

## What changes

- **Faster Movement:** Link's animations play at twice the normal rate, and his swim speeds (surface,
  dash, dive, up, down and with or against the current) are doubled.
- **Faster Movement - extra speed (optional add-on):** walking and running, rolling and climbing are
  each twice as fast, applied after the animation. Use it if the main patch leaves one of them at
  normal speed.

Everything Link animates runs twice as fast with the main patch, including sword swings, item use,
ledge pull-ups and his animation in cutscenes. Timed inputs may feel different. The add-on's climbing
speed may also apply to ladder and scripted climbs.

## Status

This build of the main patch has not been tested in game yet. In an earlier build, swimming was
faster (both travel and animation) but movement on land was unchanged. This build also applies the
faster animation rate to Link's own animation update.

The extra-speed add-on has not been clearly tested.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii, right-click the game and choose Open Mod Data Location).
3. Create `Faster Movement/exefs/` there and copy `patches/faster-movement.pchtxt` into it.
4. Optional: create `Faster Movement - extra speed/exefs/` and copy
   `patches/faster-movement-extra.pchtxt` into it.
5. Enable the mods in the game's Add-Ons and start the game.

## Compatibility

- Code patches only: no game files are replaced and no executable module or subsdk slot is used.
- The extra-speed add-on changes the same instruction as the Link Speed x4 mod. Enable at most one of
  the two.
- Added code sits in unused gaps between the game's functions. Each patch mod in this repository uses
  its own gaps.

## Patch source

`patches/faster-movement.pchtxt` (75 instruction words, added code in 9 unused gaps) and
`patches/faster-movement-extra.pchtxt` (11 words, 2 gaps) are the version-locked patch source. Every
patched address is listed in the files. They contain no game assets.

License: MIT.
