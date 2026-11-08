# Moon Jump

Link's jumps go much higher and he floats down slowly, as if on the moon. Everything else stays
normal.

## What changes

- The side hop, back hop, backflip, forward hop and regular ledge jump use one sixth of their normal
  gravity, and their hop heights are six times higher. Take-off speed is unchanged, so each one rises
  about six times higher and stays in the air longer.
- The jump attack rises six times higher, with normal gravity.
- The game still limits a ledge jump's peak to 5 m.

## Status

Tested in game on the emulator: the side, back and forward hops, the backflip, the ledge jump and
the jump attack all work.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii, right-click the game and choose Open Mod Data Location).
3. Create `Moon Jump/exefs/` there and copy `patches/moon-jump.pchtxt` into it.
4. Enable Moon Jump in the game's Add-Ons and start the game.

## Compatibility

- Code patch only: no game files are replaced and no executable module or subsdk slot is used.
- Added code sits in unused gaps between the game's functions. Each patch mod in this repository uses
  its own gaps, so Moon Jump can be enabled with the others.

## Patch source

`patches/moon-jump.pchtxt` is the version-locked patch source: 70 instruction words, with the added
code in 11 unused gaps. Every patched address is listed in the file. It contains no game assets.

License: MIT.
