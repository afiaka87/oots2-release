# Hookshot Everything

The hookshot and longshot latch onto any surface they hit and pull Link there, not only the marked
wooden targets. Nothing else changes.

## What changes

- When the hookshot's tip touches a surface, the game's "can it stick here?" check always answers yes.
- While aiming, the check that decides whether the surface under the reticle is a target is removed.

## Status

Not tested in game yet.

## Requirements and installation

Supports only the game's v131072 update (main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii, right-click the game and choose Open Mod Data Location).
3. Create `Hookshot Everything/exefs/` there and copy `patches/hookshot-everything.pchtxt` into it.
4. Enable Hookshot Everything in the game's Add-Ons and start the game.

## Compatibility

- Code patch only: no game files are replaced and no executable module or subsdk slot is used.
- No added code. It does not overlap the other patch mods in this repository.

## Patch source

`patches/hookshot-everything.pchtxt` is the version-locked patch source: 3 instruction words. Every
patched address is listed in the file. It contains no game assets.

License: MIT.
