// Draws the warp menu at the bottom left of the screen. The text is drawn on the CPU (8x8 font, scaled) into a small
// row-by-row image in the module's own memory, in the screen image's format, and copied onto the screen image each
// time the "UI 2D" or "UI_2D_Overlay" layer binds it as a drawing target. Layers' command lists run in layer order,
// so the last copy lands on top of the HUD. Nothing is drawn while the menu is closed.
#pragma once

#include "game.hpp"

namespace warp {

// Installs the graphics table load and layer draw hooks (sites already checked).
void installOverlayHooks();

}  // namespace warp
