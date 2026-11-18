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

// On row py of a rounded rectangle, the columns whose distance depends only on the row: there the corner arcs do
// not reach, and roundRectDistance is qy - r for every column. `lo` > `hi` when there are none.
struct FlatRun {
    float d;
    int lo, hi;
};

FlatRun flatRun(float py, float x, float y, float w, float h, float r) {
    const float hw = w * 0.5f, hh = h * 0.5f;
    r = minf(r, minf(hw, hh));
    const float qy = absf(py - (y + hh)) - (hh - r);
    const float span = (hw - r) + minf(qy, 0.0f);
    if (span < 0.0f) { return {0.0f, 1, 0}; }
    const float cx = x + hw;
    return {qy - r, ceilToInt(cx - span - 0.5f), floorToInt(cx + span - 0.5f)};
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
    if (x < 0 || y < m_top || x >= m_width || y >= m_bottom || alpha <= 0) { return; }
    uint32_t& p = row(y)[x];
    p = mix(p, c, alpha);
}

void Canvas::fill(Color c) {
    const uint32_t v = pack(c);
    for (int y = m_top; y < m_bottom; ++y) {
        uint32_t* r = row(y);
        for (int x = 0; x < m_width; ++x) { r[x] = v; }
    }
}

void Canvas::fillGradient(Color top, Color bottom) {
    for (int y = m_top; y < m_bottom; ++y) {
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
    for (int py = y0 > m_top ? y0 : m_top; py < y1 && py < m_bottom; ++py) {
        const FlatRun run = flatRun(py + 0.5f, x, y, w, h, radius);
        const int flatAlpha = static_cast<int>(clamp01(0.5f - run.d) * alpha + 0.5f);
        uint32_t* r = row(py);
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < m_width; ++px) {
            if (px >= run.lo && px <= run.hi) {
                // Same coverage across the run: blend it without the distance.
                const int end = run.hi + 1 < x1 ? (run.hi + 1 < m_width ? run.hi + 1 : m_width) : (x1 < m_width ? x1 : m_width);
                if (flatAlpha > 0) {
                    if (flatAlpha >= 255) {
                        const uint32_t v = pack(c);
                        for (int i = px; i < end; ++i) { r[i] = v; }
                    } else {
                        for (int i = px; i < end; ++i) { r[i] = mix(r[i], c, flatAlpha); }
                    }
                }
                px = end - 1;
                continue;
            }
            const float d = roundRectDistance(px + 0.5f, py + 0.5f, x, y, w, h, radius);
            const float cover = clamp01(0.5f - d);
            if (cover > 0.0f) { blend(px, py, c, static_cast<int>(cover * alpha + 0.5f)); }
        }
    }
}

void Canvas::strokeRoundRect(float x, float y, float w, float h, float radius, float t, Color c) {
    const int x0 = floorToInt(x), y0 = floorToInt(y), x1 = ceilToInt(x + w), y1 = ceilToInt(y + h);
    for (int py = y0 > m_top ? y0 : m_top; py < y1 && py < m_bottom; ++py) {
        const FlatRun outerRun = flatRun(py + 0.5f, x, y, w, h, radius);
        const FlatRun innerRun = flatRun(py + 0.5f, x + t, y + t, w - 2 * t, h - 2 * t, maxf(radius - t, 0));
        const int lo = outerRun.lo > innerRun.lo ? outerRun.lo : innerRun.lo;
        const int hi = outerRun.hi < innerRun.hi ? outerRun.hi : innerRun.hi;
        const int flatAlpha =
            static_cast<int>((clamp01(0.5f - outerRun.d) - clamp01(0.5f - innerRun.d)) * 255.0f + 0.5f);
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < m_width; ++px) {
            if (px >= lo && px <= hi) {
                // Both outlines are straight here, so the stroke's coverage is the same across the run.
                const int end = hi + 1 < m_width ? hi + 1 : m_width;
                if (flatAlpha > 0) {
                    for (int i = px; i < end; ++i) { blend(i, py, c, flatAlpha); }
                }
                px = end - 1;
                continue;
            }
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
        if (ty < m_top || ty >= m_bottom) { continue; }
        uint32_t* r = row(ty);
        const uint8_t* s = rgb + static_cast<size_t>(py) * w * 3;
        const FlatRun run = flatRun(py + 0.5f, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), radius);
        const bool flatSolid = run.d <= -0.5f;
        for (int px = 0; px < w; ++px, s += 3) {
            const int tx = x + px;
            if (tx < 0 || tx >= m_width) { continue; }
            const Color c{s[0], s[1], s[2]};
            if (flatSolid && px >= run.lo && px <= run.hi) {
                r[tx] = pack(c);
                continue;
            }
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
        if (ty < m_top || ty >= m_bottom) { continue; }
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
