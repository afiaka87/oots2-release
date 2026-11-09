# oot-release

Source for my Ocarina of Time mods for Nintendo Switch 2 (title `0400486024DEC000`). Each mod
lives in its own folder with its own README. Binaries are published on GameBanana; this repository
holds source only.

| Project | Folder | Version | Slot | What it does |
|---|---|---|---|---|
| Mirror Mode | `mirror-mode/` | v1.0.1 | subsdk4 | Shows the world flipped left to right, with both sticks and gyro aiming mirrored to match. The HUD, dialog and menus stay normal. |
| Fast Travel | `fast-travel/` | v1.1.0 | subsdk5 | An L3 menu that warps Link to 214 spots all over Hyrule, as young or adult Link, changing age at the Temple of Time when needed. A second version, Fast Travel (Visited), lists only the spots Link has reached in that save. |
| Moon Jump | `moon-jump/` | unreleased | none (patch file only) | Much higher hops and jumps with slow, floaty falls. |
| Hookshot Everything | `hookshot-everything/` | unreleased | none (patch file only) | The hookshot and longshot latch onto any surface, not only the marked targets. |
| Faster Movement | `faster-movement/` | unreleased | none (patch files only) | Link's animations and swim speeds doubled, with an optional add-on for walking, rolling and climbing. |
| Climb Anything | `climb-anything/` | unreleased | none (patch file only) | Climb any wall, not only vines and marked surfaces. |

All code here is MIT-licensed (see `LICENSE`). The code mods use exlaunch. It is not included. See `NOTICE`.

The patch-file mods are pchtxt text patches for the emulator; they need no build step.
