// The area pictures. Built in: BC1-compressed pictures compiled into the module (kPictureFile, made by
// tools/make_assets.py embedded), scaled to the preview and thumbnail sizes when shown. Optional override: the SD card's
// fast-travel/pictures.bin (made by tools/make_assets.py pictures), raw RGB previews and thumbnails per screen scale,
// opened the first time the menu is painted; when it is present it replaces the built-in pictures.
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
