#include "overlay.hpp"

#include <cstring>

#include "menu.hpp"
#include "ui/assets.hpp"
#include "ui/panel.hpp"
#include "ui/pictures.hpp"

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

// The panel is painted on the CPU (ui/panel.cpp) into one of two linear images in the module's own memory, converted
// in place into the screen image's format, and copied onto the screen image each time the "UI 2D" or "UI_2D_Overlay"
// layer binds it as a drawing target. The copy follows the panel's rounded outline: one copy per run of rows with the
// same corner inset, so the game shows through outside the corners.
// Copies queued for earlier frames may still be reading the shown image, so a repaint goes into the other image and
// the copies switch to it once it is finished. Painting the shown image in place flickered in colour on
// every repaint: its pixels held unconverted bytes until the conversion pass.
constexpr size_t kImageSlotBytes = 624u << 12;  // 2,555,904: one 1200x525 image at a row stride of up to 4864
constexpr size_t kImageBytes = 2 * kImageSlotBytes;
constexpr int kMinScreenWidth = 640;
constexpr int kMinScreenHeight = 360;
constexpr int kMaxPanelWidth = ui::panelWidth(3, true), kMaxPanelHeight = ui::panelHeight(3);
constexpr int kMaxCorner = ui::panelCorner(3);

alignas(65536) unsigned char s_imageMemory[kImageBytes];
Opaque s_pool;
Opaque s_image[2];
int s_front = 0;  // the image the copies read
bool s_poolReady = false;
bool s_poolCached = false;
bool s_imageReady = false;
bool s_imageFailed = false;
unsigned char* s_pixels = nullptr;
// The panel is painted here first, a band of rows at a time (see paint); the widest panel is 1200 pixels.
constexpr int kBandRows = 128;
constexpr int kBandWidth = 1200;
alignas(16) u32 s_canvas[kBandWidth * kBandRows];
static_assert(ui::panelWidth(3, true) <= kBandWidth);
int s_imageFormat = -1, s_scale = 0, s_width = 0, s_height = 0, s_stride = 0;
bool s_pictures = false;  // the panel layout with pictures (picture files were found on the SD card)
size_t s_imageSize = 0;
u32 s_paintedVersion = ~0u;
SpinLock s_imageLock;
u64 s_copies = 0;
int s_corner = 0;
int s_inset[kMaxCorner] = {};  // pixels left out at each end of the top rows (mirrored at the bottom)
u32 s_lut[3][256] = {};        // per-channel conversion of display bytes for the floating-point screen format

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
    // About value^2.2 on 0-1 (within 0.017), close enough for the menu: value^2.25 times a factor near 1.
    float result = value * value;
    float root = value;
    for (int i = 0; i < 3; ++i) { root = __builtin_sqrtf(root); }  // value^(1/8)
    return result * root * root * (0.6f + 0.4f * root);
}

bool packable(int format) {
    return format == kFormatR11G11B10F || format == kFormatRGBA8 || format == kFormatRGBA8sRGB;
}

// The floating-point screen format holds linear light, so display values are gamma-decoded first and look the same
// after the game's output pass. The RGBA8 formats take the display bytes as they are.
void buildLut(int format) {
    if (format != kFormatR11G11B10F) { return; }
    for (int i = 0; i < 256; ++i) {
        const float linear = gammaDecode(static_cast<float>(i) / 255.0f);
        s_lut[0][i] = smallFloat(linear, 6);
        s_lut[1][i] = smallFloat(linear, 6) << 11;
        s_lut[2][i] = smallFloat(linear, 5) << 22;
    }
}

// Corner insets of the rounded outline: pixel x of row y (y < r) is inside when its centre is within the circle.
void buildInsets(int radius) {
    s_corner = radius < kMaxCorner ? radius : kMaxCorner;
    for (int y = 0; y < s_corner; ++y) {
        const float dy = static_cast<float>(s_corner) - (static_cast<float>(y) + 0.5f);
        const float span = __builtin_sqrtf(static_cast<float>(s_corner * s_corner) - dy * dy);
        const float outside = static_cast<float>(s_corner) - span - 0.5f;
        int inset = static_cast<int>(outside);
        if (outside > static_cast<float>(inset)) { ++inset; }
        s_inset[y] = inset < 0 ? 0 : inset;
    }
}

int insetAt(int y) {
    if (y < s_corner) { return s_inset[y]; }
    if (y >= s_height - s_corner) { return s_inset[s_height - 1 - y]; }
    return 0;
}

void paint(const MenuView& view) {
    const u64 t0 = svcGetSystemTick();
    const int back = s_front ^ 1;
    // Painted in ordinary memory, then written once into the image: the painting reads and blends each pixel several
    // times, which is slow in the image's memory pool (under an emulator above all). The canvas's pixel layout is the
    // RGBA8 one, so those formats are a plain copy.
    unsigned char* image = s_pixels + back * kImageSlotBytes;
    for (int top = 0; top < s_height; top += kBandRows) {
        const int rows = s_height - top < kBandRows ? s_height - top : kBandRows;
        ui::Canvas canvas(s_canvas, s_width, s_height, s_width, top, rows);
        ui::paintPanel(canvas, view, s_scale, s_pictures);
        for (int y = top; y < top + rows; ++y) {
            const u32* src = canvas.row(y);
            auto* dst = reinterpret_cast<u32*>(image + static_cast<size_t>(y) * s_stride);
            if (s_imageFormat != kFormatR11G11B10F) {
                std::memcpy(dst, src, static_cast<size_t>(s_width) * 4);
                continue;
            }
            for (int x = 0; x < s_width; ++x) {
                const u32 p = src[x];
                dst[x] = s_lut[0][p & 0xFF] | s_lut[1][(p >> 8) & 0xFF] | s_lut[2][(p >> 16) & 0xFF];
            }
        }
    }
    const u64 t1 = svcGetSystemTick();
    if (s_poolCached && s_api.poolFlush != nullptr) {
        s_api.poolFlush(&s_pool, static_cast<ptrdiff_t>(back * kImageSlotBytes), s_imageSize);
    }
    s_front = back;
    s_paintedVersion = view.version;
    const u64 t2 = svcGetSystemTick();
    WTLOG("timing: paint %llu us (panel+copy %llu us, flush %llu us)", (t2 - t0) * 1000000 / 19200000,
          (t1 - t0) * 1000000 / 19200000, (t2 - t1) * 1000000 / 19200000);
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
// the 2D engine converts). Scale 3 at 1000+ rows, else 2: 1200x525 or 800x350 pixels with pictures, 615x525 or
// 410x350 for the list alone. The first call looks for picture files on the SD card to choose the layout.
bool ensureImage(int screenFormat, int screenHeight) {
    const int format = packable(screenFormat) ? screenFormat : kFormatRGBA8;
    const int scale = screenHeight >= 1000 ? 3 : 2;
    const bool pictures = ui::pictures::available();
    if (s_imageReady && format == s_imageFormat && scale == s_scale && pictures == s_pictures) { return true; }
    if (s_imageFailed || !ensurePool()) { return false; }
    const Api& a = s_api;
    if (s_imageReady) {
        a.texFinalize(&s_image[0]);
        a.texFinalize(&s_image[1]);
        s_imageReady = false;
    }
    const int width = ui::panelWidth(scale, pictures);
    const int height = ui::panelHeight(scale);
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
    if (bytes == 0 || bytes > kImageSlotBytes || static_cast<size_t>(stride) * height > kImageSlotBytes ||
        kImageSlotBytes % (storageAlignment != 0 ? storageAlignment : 1) != 0) {
        return failImage("image size", static_cast<long long>(bytes));
    }
    for (int i = 0; i < 2; ++i) {
        a.texStorage(&builder, &s_pool, static_cast<ptrdiff_t>(i * kImageSlotBytes));
        if (!a.texInit(&s_image[i], &builder)) {
            if (i == 1) { a.texFinalize(&s_image[0]); }
            return failImage("image", format);
        }
    }
    s_front = 0;
    s_imageReady = true;
    s_imageFormat = format;
    s_scale = scale;
    s_pictures = pictures;
    s_width = width;
    s_height = height;
    s_stride = stride;
    s_imageSize = bytes;
    s_paintedVersion = ~0u;
    buildLut(format);
    buildInsets(ui::panelCorner(scale));
    WTLOG("menu images ready (two, %s): %dx%d, format %d (screen format %d), row stride %d (device alignment %d), "
          "%llu bytes, storage alignment %llu",
          pictures ? "with pictures" : "list only", width, height, format, screenFormat, stride, alignment,
          static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(storageAlignment));
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
    int front = 0;
    {
        LockGuard guard(s_imageLock);
        if (!acceptScreen(width, height, format)) { return; }
        if (!ensureImage(format, height)) { return; }
        if (view.version != s_paintedVersion) { paint(view); }
        front = s_front;
    }
    if (s_width > width || s_height > height) { return; }
    const int x = width / 40 + s_width <= width ? width / 40 : width - s_width;
    const int y = height - s_height - height / 16;
    constexpr int kBarrier = kBarrierOrderFragments | kBarrierInvalidateTexture;
    a.barrier(cmd, kBarrier);
    // One copy per run of rows with the same inset: the corner runs, then the middle block.
    int runs = 0;
    for (int top = 0; top < s_height;) {
        const int inset = insetAt(top);
        int bottom = top + 1;
        while (bottom < s_height && insetAt(bottom) == inset) { ++bottom; }
        const CopyRegion source{inset, top, 0, s_width - 2 * inset, bottom - top, 1};
        const CopyRegion target{x + inset, y + top, 0, s_width - 2 * inset, bottom - top, 1};
        if (source.width > 0) {
            a.copy(cmd, &s_image[front], nullptr, &source, screen, nullptr, &target, kCopyEngine2D);
            ++runs;
        }
        top = bottom;
    }
    a.barrier(cmd, kBarrier);
    const u64 copies = ++s_copies;
    if (isPowerOfTwo(copies) && copies <= (1u << 16)) {
        WTLOG("menu copy %llu in %s: screen %p %dx%d format %d, panel at (%d, %d) in %d runs",
              static_cast<unsigned long long>(copies), kind == LayerKind::Hud ? "UI 2D" : "UI_2D_Overlay", screen,
              width, height, format, x, y, runs);
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

void installOverlayHooks(bool pictureFiles) {
    const bool fonts = ui::assets::init();
    if (pictureFiles) { ui::pictures::enable(); }
    WTLOG("menu fonts %s; area pictures %s", fonts ? "ready" : "malformed: the menu is drawn without text",
          pictureFiles ? "read from the SD card on first open" : "off (file routines differ)");
    LoadGraphicsProcs::InstallAtOffset(kLoadGraphicsProcs.offset);
    DrawLayer::InstallAtOffset(kDrawLayer.offset);
}

}  // namespace warp
