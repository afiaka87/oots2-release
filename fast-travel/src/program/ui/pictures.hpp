// The area pictures: the player's own JPEG files on the SD card, fast-travel/pictures/<GameBancParam name>.jpg
// (baseline JPEG, any size; 16:9 is shown whole, other shapes are cropped to 16:9 about the centre). Each is decoded
// and scaled when the menu first shows it: the thumbnails are kept, and the cursor's area keeps its large picture.
// Without any picture file the menu shows the list alone.
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

// True when at least one area's picture file opens; the first call looks. The answer does not change later.
bool available();

// Index of the picture named after a GameBancParam name, or -1.
int find(const char* name);
// The picture's thumbnail at this screen scale, decoded on first use. Empty when its file is missing or unreadable.
Picture thumbnail(int picture, int scale);
// The picture's large preview at this screen scale; one is kept. Empty when its file is missing or unreadable.
Picture preview(int picture, int scale);

}  // namespace pictures
}  // namespace warp::ui
