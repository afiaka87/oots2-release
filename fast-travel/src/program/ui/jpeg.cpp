#include "jpeg.hpp"

namespace warp::ui::jpeg {
namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;
using s64 = int64_t;

constexpr size_t kInputBytes = 16384;
constexpr int kMaxPlaneWidth = kMaxWidth + 16;  // a row of blocks is padded to whole blocks

u8 s_input[kInputBytes];
u8 s_luma[kMaxPlaneWidth * 16];      // one row of blocks: luma (up to 16 picture rows)
u8 s_chroma[2][kMaxPlaneWidth * 8];  // and the two colour planes (8 rows)
u8 s_rgb[2][kMaxWidth * 3];          // the previous and current picture rows, for the bilinear scaling
u32 s_sum[kMaxSmallWidth * 3];
u32 s_count[kMaxSmallWidth];

constexpr u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// cos((2x + 1) u pi / 16), times 1/sqrt(2) for u = 0, times 1024; row x, column u.
constexpr s32 kCos[64] = {
    724, 1004, 946,  851,  724,  569,   392,  200,   724, 851,   392,  -200, -724, -1004, -946, -569,
    724, 569,  -392, -1004, -724, 200,  946,  851,   724, 200,   -946, -569, 724,  851,   -392, -1004,
    724, -200, -946, 569,  724,  -851,  -392, 1004,  724, -569,  -392, 1004, -724, -200,  946,  -851,
    724, -851, 392,  200,  -724, 1004,  -946, 569,   724, -1004, 946,  -851, 724,  -569,  392,  -200};

u8 clamp(s32 v) { return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// The file's bytes in order, through a buffer.
struct Stream {
    ReadFn read;
    void* context;
    int64_t offset = 0;
    size_t at = 0, end = 0;

    bool byte(u8& out) {
        if (at == end) {
            end = read(context, offset, s_input, kInputBytes);
            offset += static_cast<int64_t>(end);
            at = 0;
            if (end == 0) { return false; }
        }
        out = s_input[at++];
        return true;
    }
    bool be16(u32& out) {
        u8 a, b;
        if (!byte(a) || !byte(b)) { return false; }
        out = static_cast<u32>(a) << 8 | b;
        return true;
    }
    void skip(u32 n) {
        const size_t here = end - at;
        if (n <= here) {
            at += n;
            return;
        }
        offset += n - here;
        at = end;
    }
};

// The compressed picture data, most significant bit first, without the stuffed zero after each 0xFF.
struct Bits {
    Stream& s;
    u32 acc = 0;     // valid bits at the top
    int count = 0;
    int marker = 0;  // marker met in the data: after it (or the file's end, -1) the data reads as zeros

    void fill() {
        while (count <= 24) {
            u32 b = 0;
            if (marker == 0) {
                u8 c;
                if (!s.byte(c)) {
                    marker = -1;
                } else if (c != 0xFF) {
                    b = c;
                } else {
                    do {
                        if (!s.byte(c)) { marker = -1; }
                    } while (marker == 0 && c == 0xFF);
                    if (marker == 0) {
                        if (c == 0) { b = 0xFF; } else { marker = c; }
                    }
                }
            }
            acc |= b << (24 - count);
            count += 8;
        }
    }
    u32 get(int n) {
        if (n == 0) { return 0; }
        fill();
        const u32 v = acc >> (32 - n);
        acc <<= n;
        count -= n;
        return v;
    }
    // Moves past the next restart marker.
    bool restart() {
        acc = 0;
        count = 0;
        if (marker >= 0xD0 && marker <= 0xD7) {
            marker = 0;
            return true;
        }
        if (marker != 0) { return false; }
        for (u8 c;;) {
            if (!s.byte(c)) { return false; }
            if (c != 0xFF) { continue; }
            do {
                if (!s.byte(c)) { return false; }
            } while (c == 0xFF);
            if (c >= 0xD0 && c <= 0xD7) { return true; }
            if (c != 0) { return false; }
        }
    }
};

struct Huffman {
    bool ready;
    u8 values[256];
    s32 maxcode[17];  // largest code of each length, -1 when none
    s32 valptr[17];   // index in values of a length's first code, minus that code
};
Huffman s_huffman[2][4];  // [DC, AC][table id]
u16 s_quant[4][64];       // in the file's zigzag order
bool s_quantReady[4];

int decodeSymbol(Bits& bits, const Huffman& h) {
    s32 code = static_cast<s32>(bits.get(1));
    for (int length = 1; length <= 16; ++length) {
        if (code <= h.maxcode[length]) { return h.values[(h.valptr[length] + code) & 255]; }
        code = code << 1 | static_cast<s32>(bits.get(1));
    }
    return -1;
}

s32 extend(u32 v, int size) {
    return size == 0 ? 0 : (v < (1u << (size - 1)) ? static_cast<s32>(v) - (1 << size) + 1 : static_cast<s32>(v));
}

// Inverse DCT of dequantized coefficients (natural order) into 8x8 samples.
void idct(const s32* in, u8* out, int stride) {
    s32 tmp[64];
    for (int v = 0; v < 8; ++v) {
        const s32* f = in + v * 8;
        s32* t = tmp + v * 8;
        if ((f[1] | f[2] | f[3] | f[4] | f[5] | f[6] | f[7]) == 0) {
            const s32 d = f[0] * kCos[0] >> 7;
            for (int x = 0; x < 8; ++x) { t[x] = d; }
            continue;
        }
        for (int x = 0; x < 8; ++x) {
            s32 sum = 0;
            for (int u = 0; u < 8; ++u) { sum += f[u] * kCos[x * 8 + u]; }
            t[x] = sum >> 7;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            s64 sum = 1 << 14;
            for (int v = 0; v < 8; ++v) { sum += static_cast<s64>(tmp[v * 8 + x]) * kCos[y * 8 + v]; }
            out[y * stride + x] = clamp(static_cast<s32>(sum >> 15) + 128);
        }
    }
}

// Crops the picture to 16:9 and scales its rows into the targets as they arrive.
struct Scaler {
    int cx, cy, cw, ch;
    Target large, small;
    int largeRow = 0, smallRow = -1;
    const u8* last = nullptr;

    void emitLarge(const u8* a, const u8* b, int ay) {
        u8* out = large.rgb + static_cast<size_t>(largeRow) * large.width * 3;
        for (int ox = 0; ox < large.width; ++ox) {
            const int fx = ((2 * ox + 1) * cw * 128) / large.width - 128;
            const int x0 = fx < 0 ? 0 : fx >> 8, ax = fx < 0 ? 0 : fx & 255;
            const int x1 = x0 + 1 < cw ? x0 + 1 : x0;
            const u8 *a0 = a + (cx + x0) * 3, *a1 = a + (cx + x1) * 3, *b0 = b + (cx + x0) * 3, *b1 = b + (cx + x1) * 3;
            for (int i = 0; i < 3; ++i) {
                const s32 top = a0[i] * (256 - ax) + a1[i] * ax, bottom = b0[i] * (256 - ax) + b1[i] * ax;
                out[ox * 3 + i] = static_cast<u8>((top * (256 - ay) + bottom * ay + 32768) >> 16);
            }
        }
        ++largeRow;
    }
    void flushSmall() {
        if (smallRow < 0) { return; }
        u8* out = small.rgb + static_cast<size_t>(smallRow) * small.width * 3;
        for (int i = 0; i < small.width * 3; ++i) {
            const u32 n = s_count[i / 3];
            out[i] = static_cast<u8>(n != 0 ? s_sum[i] / n : 0);
        }
    }
    // Picture row y; `previous` is row y - 1 (unused for the first row).
    void row(int y, const u8* rgb, const u8* previous) {
        const int c = y - cy;
        if (c < 0 || c >= ch) { return; }
        if (small.rgb != nullptr) {
            const int oy = c * small.height / ch;
            if (oy != smallRow) {
                flushSmall();
                smallRow = oy;
                for (int i = 0; i < small.width; ++i) { s_count[i] = 0; }
                for (int i = 0; i < small.width * 3; ++i) { s_sum[i] = 0; }
            }
            const u8* p = rgb + cx * 3;
            for (int x = 0; x < cw; ++x, p += 3) {
                const int ox = x * small.width / cw;
                s_sum[ox * 3] += p[0];
                s_sum[ox * 3 + 1] += p[1];
                s_sum[ox * 3 + 2] += p[2];
                ++s_count[ox];
            }
        }
        if (large.rgb != nullptr) {
            while (largeRow < large.height) {
                const int fy = ((2 * largeRow + 1) * ch * 128) / large.height - 128;
                if (fy >= 0 && (fy >> 8) > c - 1) { break; }
                if (fy < 0 || c == 0) {
                    emitLarge(rgb, rgb, 0);
                } else {
                    emitLarge(previous, rgb, fy & 255);
                }
            }
        }
        last = rgb;
    }
    void finish() {
        flushSmall();
        while (large.rgb != nullptr && last != nullptr && largeRow < large.height) { emitLarge(last, last, 0); }
    }
};

// The picture's centred 16:9 part, scaled into the targets.
Scaler cropTo16x9(int width, int height, Target large, Target small) {
    Scaler scaler{};
    if (width * 9 <= height * 16) {
        scaler.cw = width;
        scaler.ch = width * 9 / 16;
    } else {
        scaler.ch = height;
        scaler.cw = height * 16 / 9;
    }
    scaler.cx = (width - scaler.cw) / 2;
    scaler.cy = (height - scaler.ch) / 2;
    scaler.large = large;
    scaler.small = small;
    return scaler;
}

struct Component {
    int id, h, v, quant, dc, ac, predictor;
};

// What the headers before the picture data describe.
struct Frame {
    Component comps[3];
    int count, width, height, restartInterval;
};

// Reads one 8x8 block's coefficients, dequantized, in natural order; false when the data is damaged.
bool decodeBlock(Bits& bits, Component& comp, s32 (&coef)[64]) {
    const Huffman& dcTable = s_huffman[0][comp.dc];
    const Huffman& acTable = s_huffman[1][comp.ac];
    const u16* q = s_quant[comp.quant];
    for (s32& value : coef) { value = 0; }
    const int dcSize = decodeSymbol(bits, dcTable);
    if (dcSize < 0 || dcSize > 11) { return false; }
    comp.predictor += extend(bits.get(dcSize), dcSize);
    coef[0] = comp.predictor * q[0];
    for (int k = 1; k < 64;) {
        const int rs = decodeSymbol(bits, acTable);
        if (rs < 0) { return false; }
        const int run = rs >> 4, size = rs & 15;
        if (size == 0) {
            if (run != 15) { break; }
            k += 16;
            continue;
        }
        k += run;
        if (k > 63) { return false; }
        coef[kZigzag[k]] = extend(bits.get(size), size) * q[k];
        ++k;
    }
    return true;
}

// Writes a block's samples: the inverse DCT, or for `full` false only its average.
void storeBlock(const s32* coef, u8* out, int stride, bool full) {
    if (full) {
        idct(coef, out, stride);
        return;
    }
    const u8 average = clamp((coef[0] >> 3) + 128);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) { out[y * stride + x] = average; }
    }
}

// One picture row to RGB; `cb` and `cr` are nullptr for grey. `hs` is 1 when colour is sampled every other column.
void convertRow(const u8* luma, const u8* cb, const u8* cr, int width, int hs, u8* out) {
    for (int x = 0; x < width; ++x) {
        const s32 l = luma[x];
        if (cb == nullptr) {
            out[x * 3] = out[x * 3 + 1] = out[x * 3 + 2] = static_cast<u8>(l);
            continue;
        }
        const s32 b = cb[x >> hs] - 128, r = cr[x >> hs] - 128;
        out[x * 3] = clamp(l + ((91881 * r) >> 16));
        out[x * 3 + 1] = clamp(l - ((22554 * b + 46802 * r) >> 16));
        out[x * 3 + 2] = clamp(l + ((116130 * b) >> 16));
    }
}

// Decodes the picture data one row of blocks (MCUs) at a time, converts each row to RGB and scales it.
const char* decodeScan(Stream& s, Frame& frame, Target large, Target small) {
    Component* comps = frame.comps;
    const int count = frame.count, width = frame.width, height = frame.height;
    const bool colour = count == 3;
    const int hmax = colour ? comps[0].h : 1, vmax = colour ? comps[0].v : 1;
    if (!colour) { comps[0].h = comps[0].v = 1; }
    const int mcuW = 8 * hmax, mcuH = 8 * vmax;
    const int mcusX = (width + mcuW - 1) / mcuW, mcusY = (height + mcuH - 1) / mcuH;
    const int lumaStride = mcusX * mcuW, chromaStride = mcusX * 8;
    const int hs = hmax == 2 ? 1 : 0, vs = vmax == 2 ? 1 : 0;
    const bool full = large.rgb != nullptr;
    Scaler scaler = cropTo16x9(width, height, large, small);

    Bits bits{s};
    s32 coef[64];
    int mcus = 0;
    for (int my = 0; my < mcusY; ++my) {
        for (int mx = 0; mx < mcusX; ++mx, ++mcus) {
            if (frame.restartInterval != 0 && mcus != 0 && mcus % frame.restartInterval == 0) {
                if (!bits.restart()) { return "picture data is damaged (restart marker)"; }
                for (int c = 0; c < count; ++c) { comps[c].predictor = 0; }
            }
            for (int c = 0; c < count; ++c) {
                Component& comp = comps[c];
                for (int bv = 0; bv < comp.v; ++bv) {
                    for (int bh = 0; bh < comp.h; ++bh) {
                        if (!decodeBlock(bits, comp, coef)) { return "picture data is damaged"; }
                        if (c == 0) {
                            storeBlock(coef, s_luma + bv * 8 * lumaStride + (mx * comp.h + bh) * 8, lumaStride, full);
                        } else {
                            storeBlock(coef, s_chroma[c - 1] + mx * 8, chromaStride, full);
                        }
                    }
                }
            }
        }
        for (int y = 0; y < mcuH; ++y) {
            const int pictureRow = my * mcuH + y;
            if (pictureRow >= height) { break; }
            u8* out = s_rgb[pictureRow & 1];
            const int chromaRow = (y >> vs) * chromaStride;
            convertRow(s_luma + y * lumaStride, colour ? s_chroma[0] + chromaRow : nullptr,
                       colour ? s_chroma[1] + chromaRow : nullptr, width, hs, out);
            scaler.row(pictureRow, out, s_rgb[(pictureRow - 1) & 1]);
        }
    }
    scaler.finish();
    return nullptr;
}

// ---- Header segments: each reads `length` bytes (the segment less its length field); nullptr or an error.

const char* readQuantTables(Stream& s, u32 length) {
    while (length >= 65) {
        u8 info;
        if (!s.byte(info) || (info & 15) > 3) { return "damaged quantization table"; }
        const bool wide = (info >> 4) != 0;
        if (length < (wide ? 129u : 65u)) { return "damaged quantization table"; }
        u16* q = s_quant[info & 3];
        for (int i = 0; i < 64; ++i) {
            u32 v = 0;
            u8 narrow = 0;
            if (wide ? !s.be16(v) : !s.byte(narrow)) { return "damaged quantization table"; }
            q[i] = static_cast<u16>(wide ? v : narrow);
        }
        s_quantReady[info & 3] = true;
        length -= wide ? 129 : 65;
    }
    s.skip(length);
    return nullptr;
}

const char* readHuffmanTables(Stream& s, u32 length) {
    while (length >= 17) {
        u8 info, counts[17];
        if (!s.byte(info) || (info & 15) > 3 || (info >> 4) > 1) { return "damaged Huffman table"; }
        u32 total = 0;
        for (int i = 1; i <= 16; ++i) {
            if (!s.byte(counts[i])) { return "damaged Huffman table"; }
            total += counts[i];
        }
        if (total > 256 || total + 17 > length) { return "damaged Huffman table"; }
        Huffman& h = s_huffman[info >> 4][info & 15];
        for (u32 i = 0; i < total; ++i) {
            if (!s.byte(h.values[i])) { return "damaged Huffman table"; }
        }
        s32 code = 0, index = 0;
        for (int len = 1; len <= 16; ++len) {
            h.valptr[len] = index - code;
            code += counts[len];
            index += counts[len];
            h.maxcode[len] = counts[len] != 0 ? code - 1 : -1;
            code <<= 1;
        }
        h.ready = true;
        length -= 17 + total;
    }
    s.skip(length);
    return nullptr;
}

// A baseline frame header: the picture's size and its components.
const char* readFrame(Stream& s, u32 length, Frame& frame) {
    u8 precision, n;
    u32 h, w;
    if (!s.byte(precision) || !s.be16(h) || !s.be16(w) || !s.byte(n)) { return "damaged frame header"; }
    if (length < 6u + 3u * n) { return "damaged frame header"; }
    if (precision != 8) { return "only 8-bit JPEG is supported"; }
    if (n != 1 && n != 3) { return "only grey or colour JPEG is supported"; }
    if (w < 16 || h < 9 || w > kMaxWidth || h > kMaxHeight) { return "picture size unsupported"; }
    Component* comps = frame.comps;
    for (int i = 0; i < n; ++i) {
        u8 id, sampling, quant;
        if (!s.byte(id) || !s.byte(sampling) || !s.byte(quant) || quant > 3) { return "damaged frame header"; }
        comps[i] = {id, sampling >> 4, sampling & 15, quant, 0, 0, 0};
    }
    if (n == 3) {
        const bool lumaOk = comps[0].h >= 1 && comps[0].h <= 2 && comps[0].v >= 1 && comps[0].v <= 2;
        if (!lumaOk || comps[1].h != 1 || comps[1].v != 1 || comps[2].h != 1 || comps[2].v != 1) {
            return "colour sampling unsupported";
        }
    }
    frame.count = n;
    frame.width = static_cast<int>(w);
    frame.height = static_cast<int>(h);
    s.skip(length - 6 - 3 * n);
    return nullptr;
}

// The scan header just before the picture data: each component's Huffman tables.
const char* readScanHeader(Stream& s, u32 length, Frame& frame) {
    u8 n;
    if (frame.count == 0) { return "picture data before the frame header"; }
    if (!s.byte(n) || n != frame.count) { return "only single-scan JPEG is supported"; }
    if (length < 4u + 2u * n) { return "damaged scan header"; }
    for (int i = 0; i < n; ++i) {
        Component& comp = frame.comps[i];
        u8 id, tables;
        if (!s.byte(id) || !s.byte(tables)) { return "damaged scan header"; }
        if (comp.id != id || (tables >> 4) > 3 || (tables & 15) > 3) { return "damaged scan header"; }
        comp.dc = tables >> 4;
        comp.ac = tables & 15;
        if (!s_huffman[0][comp.dc].ready || !s_huffman[1][comp.ac].ready || !s_quantReady[comp.quant]) {
            return "a table is missing";
        }
    }
    s.skip(length - 1 - 2 * n);
    return nullptr;
}

void forgetTables() {
    for (Huffman(&tables)[4] : s_huffman) {
        for (Huffman& table : tables) { table.ready = false; }
    }
    for (bool& ready : s_quantReady) { ready = false; }
}

}  // namespace

const char* decode(ReadFn read, void* context, Target large, Target small) {
    if (small.rgb != nullptr && (small.width > kMaxSmallWidth || small.width <= 0 || small.height <= 0)) {
        return "small picture size unsupported";
    }
    Stream s{read, context};
    u8 a, b;
    if (!s.byte(a) || !s.byte(b) || a != 0xFF || b != 0xD8) { return "not a JPEG file"; }
    forgetTables();
    Frame frame{};
    for (;;) {
        if (!s.byte(a)) { return "file ends before the picture data"; }
        if (a != 0xFF) { return "damaged file (marker expected)"; }
        do {
            if (!s.byte(b)) { return "file ends before the picture data"; }
        } while (b == 0xFF);
        if (b == 0xD8 || b == 0x01 || (b >= 0xD0 && b <= 0xD7)) { continue; }
        if (b == 0xD9) { return "no picture data"; }
        u32 length;
        if (!s.be16(length) || length < 2) { return "damaged file (segment length)"; }
        length -= 2;
        const char* error = nullptr;
        switch (b) {
        case 0xDB: error = readQuantTables(s, length); break;
        case 0xC4: error = readHuffmanTables(s, length); break;
        case 0xC0:
        case 0xC1: error = readFrame(s, length, frame); break;
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF: return "progressive or other non-baseline JPEG; save it as baseline";
        case 0xDD: {  // restart interval
            u32 interval;
            if (length < 2 || !s.be16(interval)) { return "damaged restart interval"; }
            frame.restartInterval = static_cast<int>(interval);
            s.skip(length - 2);
            break;
        }
        case 0xDA:
            error = readScanHeader(s, length, frame);
            return error != nullptr ? error : decodeScan(s, frame, large, small);
        default: s.skip(length); break;
        }
        if (error != nullptr) { return error; }
    }
}

}  // namespace warp::ui::jpeg
