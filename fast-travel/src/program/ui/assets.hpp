// The menu's built-in assets (data/assets.bin, made by tools/make_assets.py): one picture per area, as a JPEG
// preview and a raw thumbnail for each screen scale (3 at 1000+ rows, else 2), and anti-aliased glyph sets.
#pragma once

#include "canvas.hpp"

namespace warp::ui {

enum class Font : int { Title = 0, AreaName = 1, Body = 2, Small = 3, Button = 4 };

struct Picture {
    const uint8_t* rgb = nullptr;  // RGB888, rows top-down
    int width = 0, height = 0;
};

namespace assets {

// Parses the blob. Returns false if it is missing or malformed (the menu then draws without pictures or text).
bool init(const uint8_t* blob, size_t size);
bool ready();

// Index of the area picture named after a GameBancParam name, or -1.
int findPicture(const char* name);
Picture thumbnail(int picture, int scale);
// Decodes (and caches) the area's preview picture. Empty on failure.
Picture preview(int picture, int scale);

// Pixels from the top of a line to its baseline, and the line's full height.
int ascent(Font font, int scale);
int lineHeight(Font font, int scale);
// Height of a capital letter above the baseline, for centring text vertically.
int capHeight(Font font, int scale);
// Width of the text in pixels.
int textWidth(Font font, int scale, const char* text);
// Draws text with its baseline at y; returns the pen's end x. Text past maxWidth (if > 0) ends with "...".
int drawText(Canvas& canvas, Font font, int scale, int x, int y, const char* text, Color color, int maxWidth = 0);

}  // namespace assets
}  // namespace warp::ui
