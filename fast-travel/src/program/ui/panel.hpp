// The Fast Travel panel: a header with the age tabs, the list (Autosave, then the areas, or an area's spots) with
// thumbnails on the left, the area's picture and name on the right, and a footer with the status and the buttons.
// Sizes are in layout units; one unit is `scale` pixels (3 on screens of 1000+ rows, else 2).
#pragma once

#include "../menu.hpp"
#include "canvas.hpp"

namespace warp::ui {

constexpr int kPanelUnitsW = 320;
constexpr int kPanelUnitsH = 140;
constexpr int kPanelCornerUnits = 6;

constexpr int panelWidth(int scale) { return kPanelUnitsW * scale; }
constexpr int panelHeight(int scale) { return kPanelUnitsH * scale; }
constexpr int panelCorner(int scale) { return kPanelCornerUnits * scale; }

// Paints the whole panel for this menu state. The canvas is panelWidth x panelHeight.
void paintPanel(Canvas& canvas, const MenuView& view, int scale);

}  // namespace warp::ui
