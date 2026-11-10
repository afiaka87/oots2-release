# oot-release

Source for my Ocarina of Time mods for Nintendo Switch 2 (title `0400486024DEC000`). Each mod
lives in its own folder with its own README. Downloads are available on GitHub and GameBanana. The folders here hold source and patch build tools.

| Project | Folder | Version | Slot | What it does |
|---|---|---|---|---|
| Mirror Mode | `mirror-mode/` | v1.0.1 | subsdk4 | Shows the world flipped left to right, with both sticks and gyro aiming mirrored to match. The HUD, dialog and menus stay normal. |
| Fast Travel | `fast-travel/` | v1.4.0 | none (patch files) | An L3 + R3 menu for warping as young or adult Link, with optional pictures of each area from the player's own JPEG files. The all-places patch lists every spot from the start; the visited patch lists spots Link has reached in that save. |
| Moon Jump | `moon-jump/` | unreleased | none (patch file only) | Much higher hops and jumps with slow, floaty falls. |
| Hookshot Everything | `hookshot-everything/` | unreleased | none (patch file only) | The hookshot and longshot latch onto any surface, not only the marked targets. |
| Faster Movement | `faster-movement/` | unreleased | none (patch files only) | Link's animations and swim speeds doubled, with an optional add-on for walking, rolling and climbing. |
| Climb Anything | `climb-anything/` | unreleased | none (patch file only) | Climb any wall, not only vines and marked surfaces. |

All code here is MIT-licensed (see `LICENSE`). The code mods use exlaunch. It is not included. See `NOTICE`.

The patch-file mods are pchtxt text patches for the emulator; they need no build step.
