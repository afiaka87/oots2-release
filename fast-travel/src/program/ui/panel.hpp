// The Fast Travel panel: a header with the age tabs, the list (Autosave, then the areas, or an area's spots) and a
// footer with the status and the buttons. With picture files on the SD card, the list rows have thumbnails and the
// area's picture and name sit on the right; without them the panel is the list alone.
// Sizes are in layout units; one unit is 1.25 x `scale` pixels (scale 3 on screens of 1000+ rows, else 2), so the
// panel is 1200 x 525 pixels with pictures and 615 x 525 without at scale 3.
#pragma once

#include "../menu.hpp"
#include "canvas.hpp"

namespace warp::ui {

constexpr int kPanelUnitsW = 320;      // with pictures
constexpr int kListPanelUnitsW = 164;  // the list alone
constexpr int kPanelUnitsH = 140;
constexpr int kPanelCornerUnits = 6;

constexpr float unitPixels(int scale) { return 1.25f * static_cast<float>(scale); }
constexpr int pixels(float units, int scale) { return static_cast<int>(units * unitPixels(scale) + 0.5f); }

constexpr int panelWidth(int scale, bool pictures) { return pixels(pictures ? kPanelUnitsW : kListPanelUnitsW, scale); }
constexpr int panelHeight(int scale) { return pixels(kPanelUnitsH, scale); }
constexpr int panelCorner(int scale) { return pixels(kPanelCornerUnits, scale); }

// Paints the whole panel for this menu state. The canvas is panelWidth x panelHeight.
void paintPanel(Canvas& canvas, const MenuView& view, int scale, bool pictures);

}  // namespace warp::ui
