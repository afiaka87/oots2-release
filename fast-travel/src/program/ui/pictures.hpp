// The area pictures, read from the SD card's fast-travel/pictures.bin (made by tools/make_assets.py pictures): a raw
// RGB preview and thumbnail per area for each screen scale. The file is opened the first time the menu is
// painted; without it the menu draws empty frames where the pictures go.
#pragma once

#include <cstdint>

namespace warp::ui {

struct Picture {
    const uint8_t* rgb = nullptr;  // RGB888, rows top-down
    int width = 0, height = 0;
};

namespace pictures {

// Allows reading the SD card; called once at start-up when the game's file routines matched.
void enable();

// Index of the picture named after a GameBancParam name, or -1. The first call reads the file's picture table.
int find(const char* name);
Picture thumbnail(int picture, int scale);
// Reads (and caches) the picture's preview. Empty on failure; a failed picture is not retried.
Picture preview(int picture, int scale);

}  // namespace pictures
}  // namespace warp::ui
