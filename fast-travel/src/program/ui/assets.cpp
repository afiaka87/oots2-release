#include "assets.hpp"

#include <cstring>

extern "C" {
#include "../ext/picojpeg.h"
}

namespace warp::ui::assets {
namespace {

struct ImageEntry {
    char name[40];
    uint32_t jpeg3, jpeg3Size, jpeg2, jpeg2Size, thumb3, thumb2;
};
static_assert(sizeof(ImageEntry) == 64);

struct FontEntry {
    uint32_t id;
    int32_t ascent, descent;
    uint32_t glyphs;
};

struct Glyph {
    int16_t xoff, yoff;
    uint16_t w, h, advance64, pad;
    uint32_t bitmap;
};
static_assert(sizeof(Glyph) == 16);

constexpr int kGlyphFirst = 32, kGlyphCount = 95;
constexpr int kThumbW[4] = {0, 0, 32, 48}, kThumbH[4] = {0, 0, 18, 27};
constexpr int kPreviewW[4] = {0, 0, 300, 450}, kPreviewH[4] = {0, 0, 168, 252};

const uint8_t* s_blob = nullptr;
size_t s_size = 0;
uint32_t s_imageCount = 0, s_fontCount = 0;
bool s_ready = false;

template <typename T>
T read(size_t offset) {
    T value;
    std::memcpy(&value, s_blob + offset, sizeof(T));
    return value;
}

ImageEntry image(int i) { return read<ImageEntry>(16 + static_cast<size_t>(i) * sizeof(ImageEntry)); }

bool findFont(Font font, int scale, FontEntry& out) {
    const uint32_t id = static_cast<uint32_t>(font) * 10 + static_cast<uint32_t>(scale);
    const size_t base = 16 + static_cast<size_t>(s_imageCount) * sizeof(ImageEntry);
    for (uint32_t i = 0; i < s_fontCount; ++i) {
        const FontEntry f = read<FontEntry>(base + i * sizeof(FontEntry));
        if (f.id == id) {
            out = f;
            return true;
        }
    }
    return false;
}

Glyph glyph(const FontEntry& font, char c) {
    int index = static_cast<unsigned char>(c) - kGlyphFirst;
    if (index < 0 || index >= kGlyphCount) { index = '?' - kGlyphFirst; }
    return read<Glyph>(font.glyphs + static_cast<size_t>(index) * sizeof(Glyph));
}

// ---- Preview decoding (picojpeg), with a small cache --------------------------------------------------------------

constexpr int kCacheSlots = 8;
constexpr size_t kSlotBytes = 450 * 252 * 3;
alignas(16) uint8_t s_cache[kCacheSlots][kSlotBytes];
int s_cacheKey[kCacheSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};
unsigned s_cacheUse[kCacheSlots] = {};
unsigned s_useClock = 0;

struct JpegSource {
    const uint8_t* data;
    uint32_t size, pos;
};

unsigned char needBytes(unsigned char* buf, unsigned char bufSize, unsigned char* read, void* user) {
    auto* src = static_cast<JpegSource*>(user);
    uint32_t n = src->size - src->pos;
    if (n > bufSize) { n = bufSize; }
    std::memcpy(buf, src->data + src->pos, n);
    src->pos += n;
    *read = static_cast<unsigned char>(n);
    return 0;
}

bool decodeJpeg(const uint8_t* data, uint32_t size, uint8_t* out, int width, int height) {
    JpegSource src{data, size, 0};
    pjpeg_image_info_t info;
    if (pjpeg_decode_init(&info, needBytes, &src, 0) != 0) { return false; }
    if (info.m_width != width || info.m_height != height || info.m_comps != 3) { return false; }
    const int pitch = width * 3;
    int mcuX = 0, mcuY = 0;
    for (;;) {
        const unsigned char status = pjpeg_decode_mcu();
        if (status != 0) { return status == PJPG_NO_MORE_BLOCKS; }
        if (mcuY >= info.m_MCUSPerCol) { return false; }
        for (int by = 0; by < info.m_MCUHeight; by += 8) {
            for (int bx = 0; bx < info.m_MCUWidth; bx += 8) {
                const int srcOffset = bx * 8 + by * 16;
                const uint8_t* r = info.m_pMCUBufR + srcOffset;
                const uint8_t* g = info.m_pMCUBufG + srcOffset;
                const uint8_t* b = info.m_pMCUBufB + srcOffset;
                const int x0 = mcuX * info.m_MCUWidth + bx, y0 = mcuY * info.m_MCUHeight + by;
                for (int y = 0; y < 8; ++y) {
                    if (y0 + y >= height) { break; }
                    uint8_t* dst = out + (y0 + y) * pitch + x0 * 3;
                    for (int x = 0; x < 8; ++x) {
                        if (x0 + x >= width) { break; }
                        dst[x * 3 + 0] = r[y * 8 + x];
                        dst[x * 3 + 1] = g[y * 8 + x];
                        dst[x * 3 + 2] = b[y * 8 + x];
                    }
                }
            }
        }
        if (++mcuX == info.m_MCUSPerRow) {
            mcuX = 0;
            ++mcuY;
        }
    }
}

}  // namespace

bool init(const uint8_t* blob, size_t size) {
    s_ready = false;
    if (blob == nullptr || size < 16 || std::memcmp(blob, "FTA1", 4) != 0) { return false; }
    s_blob = blob;
    s_size = size;
    s_imageCount = read<uint32_t>(4);
    s_fontCount = read<uint32_t>(8);
    const size_t header = 16 + static_cast<size_t>(s_imageCount) * sizeof(ImageEntry) + s_fontCount * sizeof(FontEntry);
    if (header > size) { return false; }
    s_ready = true;
    return true;
}

bool ready() { return s_ready; }

int findPicture(const char* name) {
    if (!s_ready || name == nullptr) { return -1; }
    for (uint32_t i = 0; i < s_imageCount; ++i) {
        const ImageEntry e = image(static_cast<int>(i));
        if (std::strncmp(e.name, name, sizeof(e.name)) == 0) { return static_cast<int>(i); }
    }
    return -1;
}

Picture thumbnail(int picture, int scale) {
    if (!s_ready || picture < 0 || picture >= static_cast<int>(s_imageCount) || (scale != 2 && scale != 3)) { return {}; }
    const ImageEntry e = image(picture);
    return {s_blob + (scale == 3 ? e.thumb3 : e.thumb2), kThumbW[scale], kThumbH[scale]};
}

Picture preview(int picture, int scale) {
    if (!s_ready || picture < 0 || picture >= static_cast<int>(s_imageCount) || (scale != 2 && scale != 3)) { return {}; }
    const int key = picture * 4 + scale;
    ++s_useClock;
    int slot = -1;
    for (int i = 0; i < kCacheSlots; ++i) {
        if (s_cacheKey[i] == key) {
            s_cacheUse[i] = s_useClock;
            return {s_cache[i], kPreviewW[scale], kPreviewH[scale]};
        }
        if (slot < 0 || s_cacheUse[i] < s_cacheUse[slot]) { slot = i; }
    }
    const ImageEntry e = image(picture);
    const uint32_t offset = scale == 3 ? e.jpeg3 : e.jpeg2, size = scale == 3 ? e.jpeg3Size : e.jpeg2Size;
    s_cacheKey[slot] = -1;
    if (offset + size > s_size || !decodeJpeg(s_blob + offset, size, s_cache[slot], kPreviewW[scale], kPreviewH[scale])) {
        return {};
    }
    s_cacheKey[slot] = key;
    s_cacheUse[slot] = s_useClock;
    return {s_cache[slot], kPreviewW[scale], kPreviewH[scale]};
}

int ascent(Font font, int scale) {
    FontEntry f;
    return s_ready && findFont(font, scale, f) ? f.ascent : 0;
}

int lineHeight(Font font, int scale) {
    FontEntry f;
    return s_ready && findFont(font, scale, f) ? f.ascent + f.descent : 0;
}

int capHeight(Font font, int scale) {
    FontEntry f;
    return s_ready && findFont(font, scale, f) ? -glyph(f, 'H').yoff : 0;
}

int textWidth(Font font, int scale, const char* text) {
    FontEntry f;
    if (!s_ready || text == nullptr || !findFont(font, scale, f)) { return 0; }
    int pen64 = 0;
    for (const char* c = text; *c != '\0'; ++c) { pen64 += glyph(f, *c).advance64; }
    return (pen64 + 32) / 64;
}

int drawText(Canvas& canvas, Font font, int scale, int x, int y, const char* text, Color color, int maxWidth) {
    FontEntry f;
    if (!s_ready || text == nullptr || !findFont(font, scale, f)) { return x; }
    const int dotsWidth = textWidth(font, scale, "...");
    const bool clip = maxWidth > 0 && textWidth(font, scale, text) > maxWidth;
    int pen64 = x * 64;
    for (const char* c = text; *c != '\0'; ++c) {
        const Glyph g = glyph(f, *c);
        if (clip && (pen64 + g.advance64) / 64 - x > maxWidth - dotsWidth) {
            for (int i = 0; i < 3; ++i) {
                const Glyph d = glyph(f, '.');
                if (d.bitmap != 0) {
                    canvas.blendMask(s_blob + d.bitmap, d.w, d.h, (pen64 + 32) / 64 + d.xoff, y + d.yoff, color);
                }
                pen64 += d.advance64;
            }
            break;
        }
        if (g.bitmap != 0 && g.w > 0 && g.h > 0) {
            canvas.blendMask(s_blob + g.bitmap, g.w, g.h, (pen64 + 32) / 64 + g.xoff, y + g.yoff, color);
        }
        pen64 += g.advance64;
    }
    return (pen64 + 32) / 64;
}

}  // namespace warp::ui::assets
