#include "overlay.hpp"

#include <cstring>

#include "font8x8.hpp"
#include "menu.hpp"

namespace warp {
namespace {

// ---- Graphics library (NVN) values; these match the Switch 1 headers, as Mirror Mode's copies confirmed ----------

constexpr int kCopyEngine2D = 0x2;
constexpr int kBarrierOrderFragments = 0x2;
constexpr int kBarrierInvalidateTexture = 0x10;
constexpr int kPoolCpuUncached = 0x2;
constexpr int kPoolCpuCached = 0x4;
constexpr int kPoolGpuCached = 0x20;
constexpr int kTextureTarget2D = 0x1;
constexpr int kTextureFlagLinear = 0x10;
constexpr int kDeviceInfoLinearStrideAlignment = 0x38;
constexpr int kFormatRGBA8 = 0x25;
constexpr int kFormatRGBA8sRGB = 0x38;
constexpr int kFormatR11G11B10F = 0x3F;

struct CopyRegion {
    int x, y, z, width, height, depth;
};

// Graphics objects are opaque and may be larger on Switch 2 than on Switch 1, so each gets generous storage.
struct alignas(16) Opaque {
    unsigned char bytes[1024];
};

using GenericFn = void (*)();
using LookupFn = GenericFn (*)(const void* device, const char* name);
using Forward8Fn = void (*)(u64, u64, u64, u64, u64, u64, u64, u64);

struct Api {
    int (*textureWidth)(const void*);
    int (*textureHeight)(const void*);
    int (*textureFormat)(const void*);
    int (*textureTarget)(const void*);
    int (*textureSamples)(const void*);
    void (*copy)(void* cmd, const void* src, const void* srcView, const CopyRegion* srcRegion, const void* dst,
                 const void* dstView, const CopyRegion* dstRegion, int flags);
    void (*barrier)(void* cmd, int bits);
    void (*poolDefaults)(void*);
    void (*poolDevice)(void*, const void*);
    void (*poolStorage)(void*, void*, size_t);
    void (*poolFlags)(void*, int);
    u8 (*poolInit)(void*, const void*);
    void (*texDefaults)(void*);
    void (*texDevice)(void*, const void*);
    void (*texTarget)(void*, int);
    void (*texSize2D)(void*, int, int);
    void (*texFormat)(void*, int);
    void (*texFlags)(void*, int);
    void (*texStride)(void*, ptrdiff_t);
    size_t (*texStorageSize)(const void*);
    size_t (*texStorageAlignment)(const void*);
    void (*texStorage)(void*, const void*, ptrdiff_t);
    u8 (*texInit)(void*, const void*);
    void (*texFinalize)(void*);
    // Optional: the pool's CPU mapping, its flush, and the device's stride alignment.
    void* (*poolMap)(const void*);
    void (*poolFlush)(const void*, ptrdiff_t, size_t);
    void (*deviceInteger)(const void*, int, int*);
};

LookupFn s_realLookup = nullptr;
const void* s_device = nullptr;
std::atomic<Forward8Fn> s_realSetRenderTargets{nullptr};
Api s_api{};
std::atomic<bool> s_apiReady{false};
bool s_apiFailed = false;

template <typename T>
bool resolve(T& out, const char* name, bool required = true) {
    out = reinterpret_cast<T>(s_realLookup(s_device, name));
    if (out == nullptr) {
        WTLOG("graphics function %s not found; %s", name, required ? "the menu cannot be drawn" : "that route is off");
    }
    return out != nullptr || !required;
}

void resolveApi() {
    if (s_apiReady.load() || s_apiFailed || s_realLookup == nullptr || s_device == nullptr) { return; }
    Api& a = s_api;
    const bool ok =
        resolve(a.textureWidth, "nvnTextureGetWidth") && resolve(a.textureHeight, "nvnTextureGetHeight") &&
        resolve(a.textureFormat, "nvnTextureGetFormat") && resolve(a.textureTarget, "nvnTextureGetTarget") &&
        resolve(a.textureSamples, "nvnTextureGetSamples") &&
        resolve(a.copy, "nvnCommandBufferCopyTextureToTexture") && resolve(a.barrier, "nvnCommandBufferBarrier") &&
        resolve(a.poolDefaults, "nvnMemoryPoolBuilderSetDefaults") &&
        resolve(a.poolDevice, "nvnMemoryPoolBuilderSetDevice") &&
        resolve(a.poolStorage, "nvnMemoryPoolBuilderSetStorage") &&
        resolve(a.poolFlags, "nvnMemoryPoolBuilderSetFlags") && resolve(a.poolInit, "nvnMemoryPoolInitialize") &&
        resolve(a.texDefaults, "nvnTextureBuilderSetDefaults") &&
        resolve(a.texDevice, "nvnTextureBuilderSetDevice") && resolve(a.texTarget, "nvnTextureBuilderSetTarget") &&
        resolve(a.texSize2D, "nvnTextureBuilderSetSize2D") && resolve(a.texFormat, "nvnTextureBuilderSetFormat") &&
        resolve(a.texFlags, "nvnTextureBuilderSetFlags") && resolve(a.texStride, "nvnTextureBuilderSetStride") &&
        resolve(a.texStorageSize, "nvnTextureBuilderGetStorageSize") &&
        resolve(a.texStorageAlignment, "nvnTextureBuilderGetStorageAlignment") &&
        resolve(a.texStorage, "nvnTextureBuilderSetStorage") && resolve(a.texInit, "nvnTextureInitialize") &&
        resolve(a.texFinalize, "nvnTextureFinalize") && resolve(a.poolMap, "nvnMemoryPoolMap", false) &&
        resolve(a.poolFlush, "nvnMemoryPoolFlushMappedRange", false) &&
        resolve(a.deviceInteger, "nvnDeviceGetInteger", false);
    if (!ok) {
        s_apiFailed = true;
        return;
    }
    s_apiReady.store(true);
    WTLOG("graphics functions resolved (device %p)", s_device);
}

// ---- Layers: which layer each drawing thread is in ----------------------------------------------------------

enum class LayerKind : u8 { Other, Hud, HudOverlay };

struct LayerInfo {
    const void* layer = nullptr;
    LayerKind kind = LayerKind::Other;
};

constexpr int kMaxLayers = 32;
LayerInfo s_layers[kMaxLayers];
std::atomic<int> s_layerCount{0};
SpinLock s_layerLock;

LayerKind kindFor(const char* name) {
    if (name == nullptr) { return LayerKind::Other; }
    if (sameText(name, "UI 2D")) { return LayerKind::Hud; }
    if (sameText(name, "UI_2D_Overlay")) { return LayerKind::HudOverlay; }
    return LayerKind::Other;
}

constexpr uintptr_t kLayerNameOffset = 0xE0;
constexpr uintptr_t kLayerIndexOffset = 0x94;

LayerKind layerKind(const void* layer) {
    const int count = s_layerCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (s_layers[i].layer == layer) { return s_layers[i].kind; }
    }
    LockGuard guard(s_layerLock);
    const int now = s_layerCount.load(std::memory_order_relaxed);
    for (int i = count; i < now; ++i) {
        if (s_layers[i].layer == layer) { return s_layers[i].kind; }
    }
    const char* name = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(layer) + kLayerNameOffset);
    const LayerKind kind = kindFor(name);
    if (now < kMaxLayers) {
        s_layers[now] = {layer, kind};
        s_layerCount.store(now + 1, std::memory_order_release);
        WTLOG("layer '%s' (index %u) first drawn%s", name != nullptr ? name : "(no name)",
              *reinterpret_cast<const u32*>(reinterpret_cast<uintptr_t>(layer) + kLayerIndexOffset),
              kind == LayerKind::Other ? "" : ": the menu is copied at its screen-image binds");
    }
    return kind;
}

struct ThreadSlot {
    std::atomic<uintptr_t> key{0};
    LayerKind kind = LayerKind::Other;
};

constexpr int kMaxSlots = 32;
ThreadSlot s_slots[kMaxSlots];

ThreadSlot* findSlot(uintptr_t key) {
    for (auto& slot : s_slots) {
        if (slot.key.load(std::memory_order_acquire) == key) { return &slot; }
    }
    return nullptr;
}

ThreadSlot* claimSlot(uintptr_t key) {
    for (auto& slot : s_slots) {
        uintptr_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) { return &slot; }
    }
    return nullptr;
}

// ---- The menu image ------------------------------------------------------------------------------------------

constexpr int kColumns = 36;
constexpr int kLines = 13;  // title, subtitle, list rows, status, controls
static_assert(kVisibleRows + 4 == kLines, "menu layout");
constexpr size_t kImageBytes = 2u << 20;
constexpr int kMinScreenWidth = 640;
constexpr int kMinScreenHeight = 360;

alignas(65536) unsigned char s_imageMemory[kImageBytes];
Opaque s_pool;
Opaque s_image;
bool s_poolReady = false;
bool s_poolCached = false;
bool s_imageReady = false;
bool s_imageFailed = false;
unsigned char* s_pixels = nullptr;
int s_imageFormat = -1, s_scale = 0, s_width = 0, s_height = 0, s_stride = 0;
size_t s_imageSize = 0;
u32 s_paintedVersion = ~0u;
SpinLock s_imageLock;
u64 s_copies = 0;

enum Color { kBackground, kBorder, kTitle, kText, kSelected, kDim, kStatus, kColors };

struct Rgb {
    float r, g, b;
};
// Display values (0-1). Linear formats get them gamma-decoded so they look the same after the game's output pass.
constexpr Rgb kPalette[kColors] = {
    {0.05f, 0.05f, 0.07f}, {0.45f, 0.45f, 0.50f}, {0.55f, 0.85f, 1.00f}, {0.92f, 0.92f, 0.92f},
    {1.00f, 0.82f, 0.25f}, {0.60f, 0.60f, 0.62f}, {0.60f, 1.00f, 0.60f},
};
u32 s_packed[kColors] = {};

u32 floatBits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Unsigned small float (no sign) with a 5-bit exponent and the given mantissa width; values are 0-1 here.
u32 smallFloat(float value, int mantissaBits) {
    if (!(value > 0.0f)) { return 0; }
    const u32 bits = floatBits(value);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) { return 0; }
    if (exponent >= 31) { return (30u << mantissaBits) | ((1u << mantissaBits) - 1); }
    return (static_cast<u32>(exponent) << mantissaBits) | ((bits & 0x7FFFFF) >> (23 - mantissaBits));
}

float gammaDecode(float value) {
    // About value^2.2 on 0-1 (within 0.017), close enough for menu colours: value^2.25 times a factor near 1.
    float result = value * value;
    float root = value;
    for (int i = 0; i < 3; ++i) { root = __builtin_sqrtf(root); }  // value^(1/8)
    return result * root * root * (0.6f + 0.4f * root);
}

u8 toByte(float value) {
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<u8>(clamped * 255.0f + 0.5f);
}

u32 pack(const Rgb& c, int format) {
    if (format == kFormatR11G11B10F) {
        return smallFloat(gammaDecode(c.r), 6) | (smallFloat(gammaDecode(c.g), 6) << 11) |
               (smallFloat(gammaDecode(c.b), 5) << 22);
    }
    return toByte(c.r) | (toByte(c.g) << 8) | (toByte(c.b) << 16) | (0xFFu << 24);
}

bool packable(int format) {
    return format == kFormatR11G11B10F || format == kFormatRGBA8 || format == kFormatRGBA8sRGB;
}

// Rows run top-down on the screen image (seen in game).
void fillRow(int y, int x0, int x1, u32 value) {
    if (y < 0 || y >= s_height) { return; }
    auto* pixel = reinterpret_cast<u32*>(s_pixels + static_cast<size_t>(y) * s_stride);
    for (int x = x0 < 0 ? 0 : x0; x < x1 && x < s_width; ++x) { pixel[x] = value; }
}

void drawText(int line, int firstColumn, const char* text, Color color) {
    const int cell = 8 * s_scale;
    const int top = 4 * s_scale + line * 10 * s_scale + s_scale;
    const int left = 4 * s_scale;
    const u32 value = s_packed[color];
    for (int i = 0; firstColumn + i < kColumns && text[i] != '\0'; ++i) {
        const u8* rows = font::glyph(text[i]);
        for (int gy = 0; gy < font::kSize; ++gy) {
            for (int gx = 0; gx < font::kSize; ++gx) {
                if (((rows[gy] >> gx) & 1) == 0) { continue; }
                const int x = left + (firstColumn + i) * cell + gx * s_scale;
                for (int sy = 0; sy < s_scale; ++sy) { fillRow(top + gy * s_scale + sy, x, x + s_scale, value); }
            }
        }
    }
}

constexpr int textLength(const char* text) {
    int n = 0;
    while (text[n] != '\0') { ++n; }
    return n;
}

// One menu line being composed: blanks, with text placed at columns.
struct Line {
    char text[kColumns + 1];

    Line() {
        for (char& c : text) { c = ' '; }
        text[kColumns] = '\0';
    }

    void put(int column, const char* value, int maxLength = kColumns) {
        for (int i = 0; value[i] != '\0' && i < maxLength && column + i < kColumns; ++i) {
            if (column + i >= 0) { text[column + i] = value[i]; }
        }
    }

    // Places text so that it ends just before column `end`.
    void putRight(int end, const char* value) { put(end - textLength(value), value); }
};

// Decimal text of a small non-negative number.
struct Number {
    char text[12];

    explicit Number(int value) {
        char reversed[12];
        int n = 0;
        do {
            reversed[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value > 0 && n < 11);
        for (int i = 0; i < n; ++i) { text[i] = reversed[n - 1 - i]; }
        text[n] = '\0';
    }
};

// "position/count", as in "3/22".
struct Position {
    char text[24];

    Position(int position, int count) {
        const Number a(position), b(count);
        int n = 0;
        for (const char* c = a.text; *c != '\0'; ++c) { text[n++] = *c; }
        text[n++] = '/';
        for (const char* c = b.text; *c != '\0'; ++c) { text[n++] = *c; }
        text[n] = '\0';
    }
};

// Columns of a list row: the cursor at 0; for areas the name at 2 and the spot count ending at 34; for spots the
// number ending at 4 and the label at 5; the scroll marks at 35.
constexpr int kAreaTitleColumn = 2;
constexpr int kAreaTitleLength = 26;
constexpr int kAreaCountEnd = kColumns - 2;
constexpr int kSpotNumberEnd = 4;
constexpr int kSpotLabelColumn = 5;
constexpr int kSpotLabelLength = kColumns - 1 - kSpotLabelColumn;
constexpr int kMarkColumn = kColumns - 1;

constexpr bool listFits() {
    for (const Area& area : kAreas) {
        if (textLength(area.title) > kAreaTitleLength || area.count > 99) { return false; }
    }
    for (const Spot& spot : kSpots) {
        if (textLength(spot.label) > kSpotLabelLength) { return false; }
    }
    return kAreaCount <= 99;
}
static_assert(listFits(), "every area name, spot label and count fits its menu columns (catalog.py checks the same)");

void paintFrame() {
    for (int y = 0; y < s_height; ++y) { fillRow(y, 0, s_width, s_packed[kBackground]); }
    for (int i = 0; i < s_scale; ++i) {
        fillRow(i, 0, s_width, s_packed[kBorder]);
        fillRow(s_height - 1 - i, 0, s_width, s_packed[kBorder]);
    }
    for (int y = 0; y < s_height; ++y) {
        fillRow(y, 0, s_scale, s_packed[kBorder]);
        fillRow(y, s_width - s_scale, s_width, s_packed[kBorder]);
    }
}

// The list page's status line while no warp status is shown: what A does on the cursor row.
const char* listHint(const MenuView& view, bool spots, bool autosaveRow) {
#if FAST_TRAVEL_VISITED_ONLY
    if (!spots && shownAreas(view, view.list) == 0) {
        return view.list == Age::Adult && !view.visits.adult ? "Opens when Link first grows up" : "No places visited yet";
    }
#endif
    // Link's age differs from the list's: a warp changes his age first (at the Temple of Time).
    if (view.link != KnownAge::Unknown && (view.link == KnownAge::Young) != (view.list == Age::Young)) {
        if (spots) { return "A: change age, then warp"; }
        return view.link == KnownAge::Young ? "Link is young: warps change age" : "Link is adult: warps change age";
    }
    if (spots) { return "Pick a spot, then press A"; }
    if (autosaveRow) { return view.autosave ? "A: turn autosave off" : "A: turn autosave on"; }
    return "Pick an area, then press A";
}

const char* listHelp(bool spots, bool autosaveRow) {
    if (spots) { return "Dpad:move/page  A:warp  B:back"; }
    return autosaveRow ? "Dpad:move/page  A:switch  B:close" : "Dpad:move/page  A:open  B:close";
}

// The list ("Autosave", then the areas) or an area's spots.
void paintList(const MenuView& view) {
    Line title;
    title.put(0, view.list == Age::Young ? "FAST TRAVEL: young Link" : "FAST TRAVEL: adult Link");
    title.putRight(kColumns, view.list == Age::Young ? "X:adult" : "X:young");
    drawText(0, 0, title.text, kTitle);
    const bool spots = view.page == Page::Spots;
    const Area& area = kAreas[view.area];
    const int listFirst = ageFirst(view.list);
    const int count = spots ? shownSpots(view, area) : kTopRows + shownAreas(view, view.list);
    const int cursor = spots ? view.spot : view.areaRow;
    const int top = spots ? view.spotTop : view.areaTop;
    Line subtitle;
    subtitle.put(0, spots ? area.title : "Areas");
    subtitle.putRight(kColumns, Position(cursor + 1, count).text);
    drawText(1, 0, subtitle.text, kDim);
    for (int row = 0; row < kVisibleRows && top + row < count; ++row) {
        const int i = top + row;
        const bool selected = i == cursor;
        Line line;
        line.put(0, selected ? ">" : " ");
        if (spots) {
            line.putRight(kSpotNumberEnd, Number(i + 1).text);
            line.put(kSpotLabelColumn, kSpots[shownSpot(view, area, i)].label, kSpotLabelLength);
        } else if (i < kTopRows) {
            line.put(kAreaTitleColumn, "Autosave", kAreaTitleLength);
            line.putRight(kAreaCountEnd, view.autosave ? "on" : "off");
        } else {
            const Area& rowArea = kAreas[shownArea(view, listFirst, i - kTopRows)];
            line.put(kAreaTitleColumn, rowArea.title, kAreaTitleLength);
            line.putRight(kAreaCountEnd, Number(shownSpots(view, rowArea)).text);
        }
        drawText(2 + row, 0, line.text, selected ? kSelected : kText);
    }
    if (top > 0) { drawText(2, kMarkColumn, "^", kDim); }
    if (top + kVisibleRows < count) { drawText(1 + kVisibleRows, kMarkColumn, "v", kDim); }
    const bool autosaveRow = !spots && cursor < kTopRows;
    const char* status = view.status != Status::Ready ? statusText(view.status) : listHint(view, spots, autosaveRow);
    drawText(2 + kVisibleRows, 0, status, kStatus);
    drawText(3 + kVisibleRows, 0, listHelp(spots, autosaveRow), kDim);
}

void paint(const MenuView& view) {
    paintFrame();
    paintList(view);
    if (s_poolCached && s_api.poolFlush != nullptr) { s_api.poolFlush(&s_pool, 0, s_imageSize); }
    s_paintedVersion = view.version;
}

bool failImage(const char* what, long long value) {
    s_imageFailed = true;
    WTLOG("menu image set-up failed at %s (%lld); the menu is not drawn (warps still work)", what, value);
    return false;
}

size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

bool ensurePool() {
    if (s_poolReady) { return true; }
    const Api& a = s_api;
    const int flagSets[2] = {kPoolCpuUncached | kPoolGpuCached, kPoolCpuCached | kPoolGpuCached};
    for (const int flags : flagSets) {
        alignas(16) Opaque builder{};
        a.poolDefaults(&builder);
        a.poolDevice(&builder, s_device);
        a.poolStorage(&builder, s_imageMemory, sizeof(s_imageMemory));
        a.poolFlags(&builder, flags);
        if (!a.poolInit(&s_pool, &builder)) {
            WTLOG("menu memory pool with flags %#x refused", flags);
            continue;
        }
        s_poolReady = true;
        s_poolCached = (flags & kPoolCpuCached) != 0;
        void* mapped = a.poolMap != nullptr ? a.poolMap(&s_pool) : nullptr;
        s_pixels = mapped != nullptr ? static_cast<unsigned char*>(mapped) : s_imageMemory;
        WTLOG("menu memory pool ready: flags %#x, CPU view %p (storage %p)", flags, s_pixels, s_imageMemory);
        return true;
    }
    return failImage("memory pool", sizeof(s_imageMemory));
}

// The menu image: linear, in the screen image's format when the module can write that format (RGBA8 otherwise, and
// the 2D engine converts). Scale 3 at 1000+ rows, else 2: 888x414 or 592x276 pixels.
bool ensureImage(int screenFormat, int screenHeight) {
    const int format = packable(screenFormat) ? screenFormat : kFormatRGBA8;
    const int scale = screenHeight >= 1000 ? 3 : 2;
    if (s_imageReady && format == s_imageFormat && scale == s_scale) { return true; }
    if (s_imageFailed || !ensurePool()) { return false; }
    const Api& a = s_api;
    if (s_imageReady) {
        a.texFinalize(&s_image);
        s_imageReady = false;
    }
    const int width = (kColumns * 8 + 8) * scale;
    const int height = (kLines * 10 + 8) * scale;
    int alignment = 0;
    if (a.deviceInteger != nullptr) { a.deviceInteger(s_device, kDeviceInfoLinearStrideAlignment, &alignment); }
    const int usedAlignment = (alignment >= 16 && alignment <= 4096 && isPowerOfTwo(alignment)) ? alignment : 256;
    const int stride = static_cast<int>(alignUp(static_cast<size_t>(width) * 4, static_cast<size_t>(usedAlignment)));
    alignas(16) Opaque builder{};
    a.texDefaults(&builder);
    a.texDevice(&builder, s_device);
    a.texTarget(&builder, kTextureTarget2D);
    a.texFormat(&builder, format);
    a.texSize2D(&builder, width, height);
    a.texFlags(&builder, kTextureFlagLinear);
    a.texStride(&builder, stride);
    const size_t bytes = a.texStorageSize(&builder), storageAlignment = a.texStorageAlignment(&builder);
    if (bytes == 0 || bytes > kImageBytes || static_cast<size_t>(stride) * height > kImageBytes) {
        return failImage("image size", static_cast<long long>(bytes));
    }
    a.texStorage(&builder, &s_pool, 0);
    if (!a.texInit(&s_image, &builder)) { return failImage("image", format); }
    s_imageReady = true;
    s_imageFormat = format;
    s_scale = scale;
    s_width = width;
    s_height = height;
    s_stride = stride;
    s_imageSize = bytes;
    s_paintedVersion = ~0u;
    for (int i = 0; i < kColors; ++i) { s_packed[i] = pack(kPalette[i], format); }
    WTLOG("menu image ready: %dx%d, format %d (screen format %d), row stride %d (device alignment %d), %llu bytes, "
          "storage alignment %llu",
          width, height, format, screenFormat, stride, alignment, static_cast<unsigned long long>(bytes),
          static_cast<unsigned long long>(storageAlignment));
    return true;
}

// The menu is drawn only on the screen: the largest widescreen UI target seen, in the first one's format. The
// slingshot's aiming view binds a second, smaller target in the same frame; rebuilding the menu image for it while
// copies recorded earlier in the frame still read the old image garbled the menu and stopped the emulator. An area
// load can bind a large square target in another format (2000x2000, format 37); taken as the screen, it hid the menu
// for the rest of the session. So only a larger widescreen target in the screen's format replaces the screen (one
// rebuild, and copies recorded before it read inside the bigger image); every other target is skipped.
int s_screenWidth = 0, s_screenHeight = 0, s_screenFormat = -1;
int s_skipLogs = 0;
constexpr int kMaxSkipLogs = 8;

// Width over height between 1.70 and 1.85 (16:9 is 1.78).
constexpr bool widescreen(int width, int height) {
    return static_cast<long long>(width) * 100 >= static_cast<long long>(height) * 170 &&
           static_cast<long long>(width) * 100 <= static_cast<long long>(height) * 185;
}
static_assert(widescreen(1920, 1080) && widescreen(1280, 720) && !widescreen(2000, 2000));

bool acceptScreen(int width, int height, int format) {
    if (width == s_screenWidth && height == s_screenHeight && format == s_screenFormat) { return true; }
    const long long area = static_cast<long long>(width) * height;
    if (widescreen(width, height) && (s_screenFormat < 0 || format == s_screenFormat) &&
        area > static_cast<long long>(s_screenWidth) * s_screenHeight) {
        WTLOG("menu screen: %dx%d format %d%s", width, height, format,
              s_screenWidth > 0 ? " (larger than the one before)" : "");
        s_screenWidth = width;
        s_screenHeight = height;
        s_screenFormat = format;
        return true;
    }
    if (s_skipLogs < kMaxSkipLogs) {
        ++s_skipLogs;
        WTLOG("menu not drawn on a %dx%d format %d target (the screen is %dx%d format %d)%s", width, height, format,
              s_screenWidth, s_screenHeight, s_screenFormat, s_skipLogs == kMaxSkipLogs ? "; not logged again" : "");
    }
    return false;
}

void copyMenu(void* cmd, const void* screen, LayerKind kind) {
    const Api& a = s_api;
    const int width = a.textureWidth(screen), height = a.textureHeight(screen);
    if (a.textureSamples(screen) > 1 || a.textureTarget(screen) != kTextureTarget2D || width < kMinScreenWidth ||
        height < kMinScreenHeight) {
        return;
    }
    const int format = a.textureFormat(screen);
    const MenuView view = menuView();
    if (!view.open) { return; }
    {
        LockGuard guard(s_imageLock);
        if (!acceptScreen(width, height, format)) { return; }
        if (!ensureImage(format, height)) { return; }
        if (view.version != s_paintedVersion) { paint(view); }
    }
    if (s_width > width || s_height > height) { return; }
    const int x = width / 40;
    const int y = height - s_height - height / 16;
    const CopyRegion source{0, 0, 0, s_width, s_height, 1};
    const CopyRegion target{x, y, 0, s_width, s_height, 1};
    constexpr int kBarrier = kBarrierOrderFragments | kBarrierInvalidateTexture;
    a.barrier(cmd, kBarrier);
    a.copy(cmd, &s_image, nullptr, &source, screen, nullptr, &target, kCopyEngine2D);
    a.barrier(cmd, kBarrier);
    const u64 copies = ++s_copies;
    if (isPowerOfTwo(copies) && copies <= (1u << 16)) {
        WTLOG("menu copy %llu in %s: screen %p %dx%d format %d, box at (%d, %d)", static_cast<unsigned long long>(copies),
              kind == LayerKind::Hud ? "UI 2D" : "UI_2D_Overlay", screen, width, height, format, x, y);
    }
}

// ---- Hooks ---------------------------------------------------------------------------------------------------

void wrappedSetRenderTargets(u64 cmd, u64 count, u64 colors, u64 views, u64 depth, u64 depthView, u64 a6, u64 a7) {
    const auto* list = reinterpret_cast<const void* const*>(colors);
    if (s_apiReady.load(std::memory_order_acquire) && static_cast<int>(static_cast<u32>(count)) > 0 &&
        list != nullptr && list[0] != nullptr) {
        ThreadSlot* slot = findSlot(threadKey());
        if (slot != nullptr && slot->kind != LayerKind::Other) {
            copyMenu(reinterpret_cast<void*>(cmd), list[0], slot->kind);
        }
    }
    s_realSetRenderTargets.load(std::memory_order_relaxed)(cmd, count, colors, views, depth, depthView, a6, a7);
}

GenericFn wrappedLookup(const void* device, const char* name) {
    const GenericFn real = s_realLookup(device, name);
    if (real == nullptr || name == nullptr) { return real; }
    if (sameText(name, "nvnCommandBufferSetRenderTargets")) {
        s_realSetRenderTargets.store(reinterpret_cast<Forward8Fn>(real));
        return reinterpret_cast<GenericFn>(&wrappedSetRenderTargets);
    }
    return real;
}

HOOK_DEFINE_TRAMPOLINE(LoadGraphicsProcs) {
    static void Callback(const void* device, LookupFn lookup) {
        if (lookup == nullptr) {
            WTLOG("graphics table load without a lookup function; left unwrapped");
            Orig(device, lookup);
            return;
        }
        s_realLookup = lookup;
        if (device != nullptr) { s_device = device; }
        Orig(device, &wrappedLookup);
        WTLOG("graphics table loaded (device %p): render-target bind %s", device,
              s_realSetRenderTargets.load() ? "wrapped" : "missing");
        resolveApi();
    }
};

HOOK_DEFINE_TRAMPOLINE(DrawLayer) {
    static void Callback(void* display, void* layer, u32 key) {
        const LayerKind kind = layer != nullptr ? layerKind(layer) : LayerKind::Other;
        const uintptr_t thread = threadKey();
        ThreadSlot* slot = findSlot(thread);
        const bool nested = slot != nullptr;
        const LayerKind outer = nested ? slot->kind : LayerKind::Other;
        if (slot == nullptr) { slot = claimSlot(thread); }
        if (slot != nullptr) { slot->kind = kind; }
        Orig(display, layer, key);
        if (slot == nullptr) { return; }
        if (nested) {
            slot->kind = outer;
        } else {
            slot->kind = LayerKind::Other;
            slot->key.store(0, std::memory_order_release);
        }
    }
};

}  // namespace

void installOverlayHooks() {
    LoadGraphicsProcs::InstallAtOffset(kLoadGraphicsProcs.offset);
    DrawLayer::InstallAtOffset(kDrawLayer.offset);
}

}  // namespace warp
