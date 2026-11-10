// Draws the warp menu at the bottom left of the screen: a panel with the area pictures, painted on the CPU
// (ui/panel.cpp) into an image in the module's own memory, in the screen image's format, and copied onto the screen
// image each time the "UI 2D" or "UI_2D_Overlay" layer binds it as a drawing target, following the panel's rounded
// outline. Layers' command lists run in layer order, so the last copy lands on top of the HUD. Nothing is drawn while
// the menu is closed.
#pragma once

#include "game.hpp"

namespace warp {

// Installs the graphics table load and layer draw hooks (sites already checked). pictureFiles: the game's file
// routines matched, so the area pictures may be read from the SD card.
void installOverlayHooks(bool pictureFiles);

}  // namespace warp
