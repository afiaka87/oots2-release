# Auto Jump

Link jumps by himself when he runs off a ledge, as in the older versions of Ocarina of Time. The
remake's jump button still works.

## What changes

- Running off a ledge starts a jump in the direction Link is moving, instead of a fall.
- Walking slowly off a ledge still drops him, with no jump.
- Steps and drops lower than about half a metre are walked down as before.
- The jump is the remake's normal jump, so it rises a little before it falls.
- It applies only while Link is walking or running normally. Moving while locked on is unchanged.
- Where the game would refuse the jump button (deep water, carrying an object), Link falls as before.

## Status

Tested in game on the emulator (Fritchii v5.7): running off ledges, walking slowly off them, running
over slopes, stairs and low steps, and the jump button. Not tested: locked-on movement, carrying
objects, ledges over water, or use together with other mods.

## Requirements and installation

Supports only the game's v131072 update (version 2.0.0; main executable build ID
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). Emulator only; not tested on Switch 2 hardware.

1. Close the game.
2. Open the game's mod folder (in Lichii or Fritchii, right-click the game and choose Open Mod Data
   Location).
3. From the release ZIP, copy the `Auto Jump` folder there. From this repository, create
   `Auto Jump/exefs/` there and copy `patches/auto-jump.pchtxt` into it.
4. Enable Auto Jump in the game's Add-Ons and start the game from the game list.

## Compatibility

- Code patch only: no game files are replaced and no executable module or subsdk slot is used.
- Added code sits in two unused gaps between the game's functions. Each patch mod in this repository
  uses its own gaps, and Auto Jump shares no patched address with them.
- Moon Jump changes the height of every jump, so it should change this one as well. The two have not
  been tested together.

## Patch source

`patches/auto-jump.pchtxt` is the version-locked patch source: 13 instruction words, with the added
code in 2 unused gaps. Every patched address is listed in the file. It contains no game assets.

License: MIT.
