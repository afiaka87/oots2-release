#include "canvas.hpp"

namespace warp::ui {
namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float maxf(float a, float b) { return a > b ? a : b; }
float minf(float a, float b) { return a < b ? a : b; }
float absf(float v) { return v < 0.0f ? -v : v; }
int floorToInt(float v) { const int i = static_cast<int>(v); return (v < 0.0f && static_cast<float>(i) != v) ? i - 1 : i; }
int ceilToInt(float v) { const int i = static_cast<int>(v); return (v > 0.0f && static_cast<float>(i) != v) ? i + 1 : i; }

// Signed distance from a point to a rounded rectangle (negative inside).
float roundRectDistance(float px, float py, float x, float y, float w, float h, float r) {
    const float hw = w * 0.5f, hh = h * 0.5f;
    r = minf(r, minf(hw, hh));
    const float qx = absf(px - (x + hw)) - (hw - r);
    const float qy = absf(py - (y + hh)) - (hh - r);
    const float ox = maxf(qx, 0.0f), oy = maxf(qy, 0.0f);
    return __builtin_sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0.0f) - r;
}

uint32_t mix(uint32_t dst, Color c, int a) {
    if (a >= 255) { return pack(c); }
    const int r = dst & 0xFF, g = (dst >> 8) & 0xFF, b = (dst >> 16) & 0xFF;
    const int nr = r + ((c.r - r) * a + 127) / 255;
    const int ng = g + ((c.g - g) * a + 127) / 255;
    const int nb = b + ((c.b - b) * a + 127) / 255;
    return static_cast<uint32_t>(nr) | (static_cast<uint32_t>(ng) << 8) | (static_cast<uint32_t>(nb) << 16) |
           0xFF000000u;
}

}  // namespace

void Canvas::blend(int x, int y, Color c, int alpha) {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height || alpha <= 0) { return; }
    uint32_t& p = row(y)[x];
    p = mix(p, c, alpha);
}

void Canvas::fill(Color c) {
    const uint32_t v = pack(c);
    for (int y = 0; y < m_height; ++y) {
        uint32_t* r = row(y);
        for (int x = 0; x < m_width; ++x) { r[x] = v; }
    }
}

void Canvas::fillGradient(Color top, Color bottom) {
    for (int y = 0; y < m_height; ++y) {
        const int t = m_height > 1 ? (y * 255) / (m_height - 1) : 0;
        const Color c{static_cast<uint8_t>(top.r + ((bottom.r - top.r) * t) / 255),
                      static_cast<uint8_t>(top.g + ((bottom.g - top.g) * t) / 255),
                      static_cast<uint8_t>(top.b + ((bottom.b - top.b) * t) / 255)};
        const uint32_t v = pack(c);
        uint32_t* r = row(y);
        for (int x = 0; x < m_width; ++x) { r[x] = v; }
    }
}

void Canvas::fillRect(float x, float y, float w, float h, Color c) { fillRoundRect(x, y, w, h, 0.0f, c); }

void Canvas::fillRoundRect(float x, float y, float w, float h, float radius, Color c, int alpha) {
    const int x0 = floorToInt(x), y0 = floorToInt(y), x1 = ceilToInt(x + w), y1 = ceilToInt(y + h);
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= m_height) { continue; }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= m_width) { continue; }
            const float d = roundRectDistance(px + 0.5f, py + 0.5f, x, y, w, h, radius);
            const float cover = clamp01(0.5f - d);
            if (cover > 0.0f) { blend(px, py, c, static_cast<int>(cover * alpha + 0.5f)); }
        }
    }
}

void Canvas::strokeRoundRect(float x, float y, float w, float h, float radius, float t, Color c) {
    const int x0 = floorToInt(x), y0 = floorToInt(y), x1 = ceilToInt(x + w), y1 = ceilToInt(y + h);
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= m_height) { continue; }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= m_width) { continue; }
            const float outer = clamp01(0.5f - roundRectDistance(px + 0.5f, py + 0.5f, x, y, w, h, radius));
            const float inner = clamp01(
                0.5f - roundRectDistance(px + 0.5f, py + 0.5f, x + t, y + t, w - 2 * t, h - 2 * t, maxf(radius - t, 0)));
            const float cover = outer - inner;
            if (cover > 0.0f) { blend(px, py, c, static_cast<int>(cover * 255.0f + 0.5f)); }
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float r, Color c) { fillRoundRect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

void Canvas::drawPicture(const uint8_t* rgb, int w, int h, int x, int y, float radius) {
    for (int py = 0; py < h; ++py) {
        const int ty = y + py;
        if (ty < 0 || ty >= m_height) { continue; }
        uint32_t* r = row(ty);
        const uint8_t* s = rgb + static_cast<size_t>(py) * w * 3;
        for (int px = 0; px < w; ++px, s += 3) {
            const int tx = x + px;
            if (tx < 0 || tx >= m_width) { continue; }
            const Color c{s[0], s[1], s[2]};
            const float d = roundRectDistance(px + 0.5f, py + 0.5f, 0.0f, 0.0f, static_cast<float>(w),
                                              static_cast<float>(h), radius);
            const float cover = clamp01(0.5f - d);
            if (cover >= 1.0f) {
                r[tx] = pack(c);
            } else if (cover > 0.0f) {
                r[tx] = mix(r[tx], c, static_cast<int>(cover * 255.0f + 0.5f));
            }
        }
    }
}

void Canvas::blendMask(const uint8_t* mask, int w, int h, int x, int y, Color c) {
    for (int py = 0; py < h; ++py) {
        const int ty = y + py;
        if (ty < 0 || ty >= m_height) { continue; }
        uint32_t* r = row(ty);
        const uint8_t* m = mask + static_cast<size_t>(py) * w;
        for (int px = 0; px < w; ++px) {
            const int tx = x + px;
            if (tx < 0 || tx >= m_width || m[px] == 0) { continue; }
            r[tx] = mix(r[tx], c, m[px]);
        }
    }
}

}  // namespace warp::ui
