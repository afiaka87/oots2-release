// A CPU drawing surface for the menu panel: 32-bit pixels laid out as R, G, B, A bytes (the RGBA8 texture layout),
// with anti-aliased rounded rectangles, circles, text and pictures. Colours are display (sRGB) values; the overlay
// converts the finished panel to the screen image's format.
#pragma once

#include <cstddef>
#include <cstdint>

namespace warp::ui {

struct Color {
    uint8_t r, g, b;
};

constexpr uint32_t pack(Color c) { return c.r | (uint32_t{c.g} << 8) | (uint32_t{c.b} << 16) | 0xFF000000u; }

class Canvas {
public:
    // `stride` is the distance between rows in pixels (at least `width`).
    Canvas(uint32_t* pixels, int width, int height, int stride)
        : m_pixels(pixels), m_width(width), m_height(height), m_stride(stride), m_top(0), m_bottom(height) {}
    // A band of a width x height surface: only rows top..top+rows-1 are stored (from `pixels`), and drawing outside
    // them is skipped. Painting the whole surface band by band gives the same pixels as painting it at once.
    Canvas(uint32_t* pixels, int width, int height, int stride, int top, int rows)
        : m_pixels(pixels), m_width(width), m_height(height), m_stride(stride), m_top(top), m_bottom(top + rows) {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    // Row y of the surface; only valid for stored rows (top() <= y < bottom()).
    uint32_t* row(int y) const { return m_pixels + static_cast<size_t>(y - m_top) * m_stride; }
    int top() const { return m_top; }
    int bottom() const { return m_bottom; }

    void fill(Color c);
    // Vertical gradient over the whole surface.
    void fillGradient(Color top, Color bottom);
    void fillRect(float x, float y, float w, float h, Color c);
    // Anti-aliased rounded rectangle (radius in pixels); alpha 0-255 for a translucent fill.
    void fillRoundRect(float x, float y, float w, float h, float radius, Color c, int alpha = 255);
    // Anti-aliased outline of a rounded rectangle, `thickness` pixels wide, inside the given bounds.
    void strokeRoundRect(float x, float y, float w, float h, float radius, float thickness, Color c);
    void fillCircle(float cx, float cy, float r, Color c);
    // An RGB888 picture scaled 1:1 at (x, y), clipped to a rounded rectangle of the picture's size.
    void drawPicture(const uint8_t* rgb, int w, int h, int x, int y, float radius);
    // Blends a coverage (alpha) bitmap in colour c with its top-left at (x, y).
    void blendMask(const uint8_t* mask, int w, int h, int x, int y, Color c);

private:
    void blend(int x, int y, Color c, int alpha);
    uint32_t* m_pixels;
    int m_width, m_height, m_stride;
    int m_top, m_bottom;
};

}  // namespace warp::ui
