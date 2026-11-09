// Mirror Mode for the Ocarina of Time remake (Switch 2): the 3D world is shown flipped left to right, while the HUD
// and dialog stay readable. The game draws its screen as a stack of layers into one shared image; this module flips
// that image just before the "UI 2D" layer (HUD and dialog) starts drawing on top of it. The flip is a set of mirrored
// 2D-engine copies through two scratch images. Both sticks are mirrored for the game controllers the player and camera
// read; menus read other controllers and keep normal directions. Motion (gyro) samples are mirrored across the
// controller's horizontal axis where they enter the game, so gyro aiming turns the mirrored way. Supported game
// build: main E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348; each hook site is checked against it before installing.

#include <lib.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

#define MMLOG(...) Logging.Log("[oot-mirror] " __VA_ARGS__)

namespace {

// ---- Game addresses (image offsets in main) -------------------------------------------------------------------

// Fills the game's graphics function table: (device, lookup). Called twice at start-up, first without a device.
constexpr ptrdiff_t kLoadGraphicsProcs = 0x129E290;
// Draws one screen layer: (display, layer, sort key). Every layer job reaches it; it returns nothing.
constexpr ptrdiff_t kDrawLayer = 0x4CD60;
constexpr u32 kLoadGraphicsProcsWords[4] = {0xD503237F, 0xA9BE7BFD, 0xA9014FF4, 0x910003FD};
constexpr u32 kDrawLayerWords[4] = {0xD503237F, 0xA9BA7BFD, 0xA9016FFC, 0xA90267FA};
constexpr uintptr_t kMainRodataOffset = 0x03C47000;
constexpr uintptr_t kMainDataOffset = 0x0484D000;
// Layer fields, from the layer set-up routine at 0xF6F5B0: name text pointer and layer index.
constexpr uintptr_t kLayerNameOffset = 0xE0;
constexpr uintptr_t kLayerIndexOffset = 0x94;

// ---- Graphics library (NVN) values; these match the Switch 1 headers and the game's own copy calls -------------

constexpr int kCopyEngine2D = 0x2;
constexpr int kCopyMirrorX = 0x4;
constexpr int kBarrierOrderFragments = 0x2;
constexpr int kBarrierInvalidateTexture = 0x10;
constexpr int kPoolCpuNoAccess = 0x1;
constexpr int kPoolGpuCached = 0x20;
constexpr int kTextureTarget2D = 0x1;

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
    size_t (*texStorageSize)(const void*);
    size_t (*texStorageAlignment)(const void*);
    void (*texStorage)(void*, const void*, ptrdiff_t);
    u8 (*texInit)(void*, const void*);
    void (*texFinalize)(void*);
};

// ---- Shared state ----------------------------------------------------------------------------------------------

LookupFn s_realLookup = nullptr;
const void* s_device = nullptr;
std::atomic<Forward8Fn> s_realSetRenderTargets{nullptr};
std::atomic<Forward8Fn> s_realCopy{nullptr};
Api s_api{};
std::atomic<bool> s_apiReady{false};
bool s_apiFailed = false;

bool sameText(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (*a != *b) { return false; }
        if (*a == '\0') { return true; }
    }
}

template <typename T>
bool resolve(T& out, const char* name) {
    out = reinterpret_cast<T>(s_realLookup(s_device, name));
    if (out == nullptr) { MMLOG("graphics function %s not found; mirroring disabled", name); }
    return out != nullptr;
}

// Runs inside the start-up hook once the device is known, before any frame is drawn.
void resolveApi() {
    if (s_apiReady.load() || s_apiFailed || s_realLookup == nullptr || s_device == nullptr) { return; }
    Api& a = s_api;
    const bool ok = resolve(a.textureWidth, "nvnTextureGetWidth") && resolve(a.textureHeight, "nvnTextureGetHeight") &&
                    resolve(a.textureFormat, "nvnTextureGetFormat") && resolve(a.textureTarget, "nvnTextureGetTarget") &&
                    resolve(a.textureSamples, "nvnTextureGetSamples") &&
                    resolve(a.copy, "nvnCommandBufferCopyTextureToTexture") &&
                    resolve(a.barrier, "nvnCommandBufferBarrier") &&
                    resolve(a.poolDefaults, "nvnMemoryPoolBuilderSetDefaults") &&
                    resolve(a.poolDevice, "nvnMemoryPoolBuilderSetDevice") &&
                    resolve(a.poolStorage, "nvnMemoryPoolBuilderSetStorage") &&
                    resolve(a.poolFlags, "nvnMemoryPoolBuilderSetFlags") &&
                    resolve(a.poolInit, "nvnMemoryPoolInitialize") &&
                    resolve(a.texDefaults, "nvnTextureBuilderSetDefaults") &&
                    resolve(a.texDevice, "nvnTextureBuilderSetDevice") &&
                    resolve(a.texTarget, "nvnTextureBuilderSetTarget") &&
                    resolve(a.texSize2D, "nvnTextureBuilderSetSize2D") &&
                    resolve(a.texFormat, "nvnTextureBuilderSetFormat") &&
                    resolve(a.texStorageSize, "nvnTextureBuilderGetStorageSize") &&
                    resolve(a.texStorageAlignment, "nvnTextureBuilderGetStorageAlignment") &&
                    resolve(a.texStorage, "nvnTextureBuilderSetStorage") &&
                    resolve(a.texInit, "nvnTextureInitialize") && resolve(a.texFinalize, "nvnTextureFinalize");
    if (!ok) {
        s_apiFailed = true;
        return;
    }
    s_apiReady.store(true);
    MMLOG("graphics functions resolved (device %p)", s_device);
}

// Thread identity without importing system functions: the per-thread register every Horizon program reads.
uintptr_t threadKey() {
    uintptr_t value;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

class SpinLock {
public:
    void lock() {
        while (m_flag.test_and_set(std::memory_order_acquire)) {}
    }
    void unlock() { m_flag.clear(std::memory_order_release); }

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

// ---- Layers ----------------------------------------------------------------------------------------------------

enum class LayerKind : u8 {
    World,     // anything not listed below; its draws count as world image writes
    Hud,       // "UI 2D": the flip runs at its first bind of a fresh world image
    Overlay,   // "Tool 2D" and "Tool 2D Super": debug overlays, never flipped and never counted as world writes
};

struct LayerInfo {
    const void* layer = nullptr;
    char name[40]{};
    LayerKind kind = LayerKind::World;
    std::atomic<u64> draws{0};
    const void* lastTarget = nullptr;
    u8 targetLogs = 0;
};

constexpr int kMaxLayers = 32;
LayerInfo s_layers[kMaxLayers];
std::atomic<int> s_layerCount{0};
SpinLock s_layerLock;
bool s_layerTableFullLogged = false;

void copyName(char (&out)[40], const char* text) {
    int i = 0;
    if (text != nullptr) {
        for (; i < 39 && text[i] != '\0'; ++i) { out[i] = text[i]; }
    }
    out[i] = '\0';
}

bool nameMatches(const LayerInfo& info, const char* text) {
    if (text == nullptr) { return info.name[0] == '\0'; }
    for (int i = 0; i < 40; ++i) {
        if (info.name[i] != text[i]) { return i == 39 && info.name[i] == '\0'; }
        if (text[i] == '\0') { return true; }
    }
    return true;
}

LayerKind kindFor(const char* name) {
    if (name == nullptr) { return LayerKind::World; }
    if (sameText(name, "UI 2D")) { return LayerKind::Hud; }
    if (sameText(name, "Tool 2D") || sameText(name, "Tool 2D Super")) { return LayerKind::Overlay; }
    return LayerKind::World;
}

const char* kindText(LayerKind kind) {
    switch (kind) {
    case LayerKind::Hud: return "HUD layer: flipped image is drawn under it";
    case LayerKind::Overlay: return "debug overlay";
    default: return "world (counts as a world image write)";
    }
}

LayerInfo* noteLayer(const void* layer, u32 key) {
    const char* name = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(layer) + kLayerNameOffset);
    const int count = s_layerCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (s_layers[i].layer == layer && nameMatches(s_layers[i], name)) { return &s_layers[i]; }
    }
    s_layerLock.lock();
    LayerInfo* info = nullptr;
    const int now = s_layerCount.load(std::memory_order_relaxed);
    for (int i = 0; i < now && info == nullptr; ++i) {
        if (s_layers[i].layer == layer) { info = &s_layers[i]; }
    }
    if (info == nullptr && now < kMaxLayers) {
        info = &s_layers[now];
        info->layer = layer;
        s_layerCount.store(now + 1, std::memory_order_release);
    }
    if (info != nullptr && !nameMatches(*info, name)) {
        copyName(info->name, name);
        info->kind = kindFor(name);
        info->lastTarget = nullptr;
        info->targetLogs = 0;
        const u32 index = *reinterpret_cast<const u32*>(reinterpret_cast<uintptr_t>(layer) + kLayerIndexOffset);
        MMLOG("layer '%s' (index %u, key %#x, object %p) first drawn: %s", info->name, index, key, layer,
              kindText(info->kind));
    }
    if (info == nullptr && !s_layerTableFullLogged) {
        s_layerTableFullLogged = true;
        MMLOG("layer table full (%d); further layers are treated as world layers without logging", kMaxLayers);
    }
    s_layerLock.unlock();
    return info;
}

// ---- Which thread is drawing which layer ---------------------------------------------------------------------

struct ThreadSlot {
    std::atomic<uintptr_t> key{0};
    LayerInfo* layer = nullptr;
    bool flipPending = false;
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

// ---- Fresh world images ----------------------------------------------------------------------------------------
// The flip runs only on an image that a world layer drew since the previous HUD pass. Without this, a frame where
// the world layer did not redraw would be flipped a second time, so the picture would alternate every frame.

std::atomic<u32> s_hudPass{0};

struct WorldMark {
    std::atomic<uintptr_t> texture{0};
    std::atomic<u32> pass{0};
};

constexpr int kMarkBits = 9;
constexpr int kMarks = 1 << kMarkBits;
constexpr int kMarkProbes = 16;
WorldMark s_marks[kMarks];
std::atomic<bool> s_marksFullLogged{false};

u32 markIndex(const void* texture) {
    return static_cast<u32>((reinterpret_cast<uintptr_t>(texture) * 0x9E3779B97F4A7C15ull) >> (64 - kMarkBits));
}

void markWorldWrite(const void* texture) {
    const auto key = reinterpret_cast<uintptr_t>(texture);
    const u32 pass = s_hudPass.load(std::memory_order_relaxed);
    u32 index = markIndex(texture);
    for (int probe = 0; probe < kMarkProbes; ++probe, index = (index + 1) & (kMarks - 1)) {
        WorldMark& mark = s_marks[index];
        uintptr_t current = mark.texture.load(std::memory_order_acquire);
        if (current == 0) {
            if (mark.texture.compare_exchange_strong(current, key, std::memory_order_acq_rel)) { current = key; }
        }
        if (current == key) {
            mark.pass.store(pass, std::memory_order_release);
            return;
        }
    }
    if (!s_marksFullLogged.exchange(true)) {
        MMLOG("world image table full; image %p is not tracked (it will not be flipped)", texture);
    }
}

// True if a world layer drew this image during the previous or the current HUD pass.
bool isFreshWorldImage(const void* texture, u32 hudPass) {
    const auto key = reinterpret_cast<uintptr_t>(texture);
    u32 index = markIndex(texture);
    for (int probe = 0; probe < kMarkProbes; ++probe, index = (index + 1) & (kMarks - 1)) {
        const WorldMark& mark = s_marks[index];
        const uintptr_t current = mark.texture.load(std::memory_order_acquire);
        if (current == 0) { return false; }
        if (current == key) { return mark.pass.load(std::memory_order_acquire) + 1 >= hudPass; }
    }
    return false;
}

// ---- The flip --------------------------------------------------------------------------------------------------

constexpr size_t kScratchBytes = 16u << 20;
constexpr int kMinFlipWidth = 640;
constexpr int kMinFlipHeight = 360;
alignas(65536) unsigned char s_scratchMemory[kScratchBytes];
Opaque s_scratchPool;
Opaque s_scratch[2];
bool s_poolReady = false;
bool s_scratchReady = false;
bool s_flipFailed = false;
int s_scratchFormat = -1, s_scratchHeight = 0, s_imageWidth = 0, s_stripWidth = 0;
size_t s_scratchSize = 0, s_scratchOffset = 0;
u64 s_flips = 0, s_unflippedPasses = 0, s_unsuitable = 0;

bool fail(const char* what, long long value) {
    s_flipFailed = true;
    MMLOG("flip set-up failed at %s (%lld); the world is shown unflipped from now on", what, value);
    return false;
}

size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// Scratch images are two vertical strips of the screen image, in its own format. Strips are as wide as half the
// image when that fits in the scratch memory; otherwise they are narrower and the flip takes more copies.
bool ensureScratch(int format, int width, int height) {
    if (s_scratchReady && format == s_scratchFormat && width == s_imageWidth && height == s_scratchHeight) {
        return true;
    }
    if (s_flipFailed) { return false; }
    const Api& a = s_api;
    if (!s_poolReady) {
        alignas(16) Opaque builder{};
        a.poolDefaults(&builder);
        a.poolDevice(&builder, s_device);
        a.poolStorage(&builder, s_scratchMemory, sizeof(s_scratchMemory));
        a.poolFlags(&builder, kPoolCpuNoAccess | kPoolGpuCached);
        if (!a.poolInit(&s_scratchPool, &builder)) { return fail("scratch memory pool", sizeof(s_scratchMemory)); }
        s_poolReady = true;
    }
    if (s_scratchReady) {
        a.texFinalize(&s_scratch[0]);
        a.texFinalize(&s_scratch[1]);
        s_scratchReady = false;
    }
    const int half = width / 2;
    for (int strip = half; strip >= 16; strip = (strip + 1) / 2) {
        alignas(16) Opaque builder{};
        a.texDefaults(&builder);
        a.texDevice(&builder, s_device);
        a.texTarget(&builder, kTextureTarget2D);
        a.texFormat(&builder, format);
        a.texSize2D(&builder, strip, height);
        const size_t alignment = a.texStorageAlignment(&builder), bytes = a.texStorageSize(&builder);
        if (alignment == 0 || (alignment & (alignment - 1)) != 0 || bytes == 0) {
            return fail("scratch image layout", static_cast<long long>(alignment));
        }
        const size_t second = alignUp(bytes, alignment);
        if (second + bytes > kScratchBytes) { continue; }
        for (int i = 0; i < 2; ++i) {
            a.texStorage(&builder, &s_scratchPool, static_cast<ptrdiff_t>(i == 0 ? 0 : second));
            if (!a.texInit(&s_scratch[i], &builder)) {
                if (i == 1) { a.texFinalize(&s_scratch[0]); }
                return fail("scratch image", i);
            }
        }
        s_scratchReady = true;
        s_scratchFormat = format;
        s_scratchHeight = height;
        s_imageWidth = width;
        s_stripWidth = strip;
        s_scratchSize = bytes;
        s_scratchOffset = second;
        MMLOG("scratch ready: screen image %dx%d format %d; strips %dx%d, %llu bytes each, %d copy rounds", width,
              height, format, strip, height, static_cast<unsigned long long>(bytes), (half + strip - 1) / strip);
        return true;
    }
    return fail("scratch memory too small for screen height", height);
}

bool isPowerOfTwo(u64 value) { return value != 0 && (value & (value - 1)) == 0; }

// Flips one image in place: each round copies a left strip and the matching right strip into the two scratch images
// with the 2D engine's horizontal mirror, then copies them back to the opposite sides. Returns false for an image
// that is not screen-sized, so the HUD pass keeps waiting for the real screen image.
bool flipImage(void* cmd, const void* image) {
    const Api& a = s_api;
    const int width = a.textureWidth(image), height = a.textureHeight(image);
    const int format = a.textureFormat(image), samples = a.textureSamples(image), target = a.textureTarget(image);
    // Only screen-sized single-sample 2D images are flipped; anything else is left alone and counted.
    if (samples > 1 || target != kTextureTarget2D || width < kMinFlipWidth || height < kMinFlipHeight) {
        if (isPowerOfTwo(++s_unsuitable)) {
            MMLOG("image %p not flipped (%llu times): %dx%d, format %d, target %d, samples %d", image,
                  static_cast<unsigned long long>(s_unsuitable), width, height, format, target, samples);
        }
        return false;
    }
    if (!ensureScratch(format, width, height)) { return true; }
    constexpr int kBarrier = kBarrierOrderFragments | kBarrierInvalidateTexture;
    constexpr int kMirror = kCopyEngine2D | kCopyMirrorX;
    a.barrier(cmd, kBarrier);
    const int half = width / 2;
    int rounds = 0;
    for (int x = 0; x < half; x += s_stripWidth, ++rounds) {
        const int strip = (half - x < s_stripWidth) ? half - x : s_stripWidth;
        const CopyRegion left{x, 0, 0, strip, height, 1};
        const CopyRegion right{width - x - strip, 0, 0, strip, height, 1};
        const CopyRegion scratch{0, 0, 0, strip, height, 1};
        a.copy(cmd, image, nullptr, &left, &s_scratch[0], nullptr, &scratch, kMirror);
        a.copy(cmd, image, nullptr, &right, &s_scratch[1], nullptr, &scratch, kMirror);
        a.barrier(cmd, kBarrier);
        a.copy(cmd, &s_scratch[0], nullptr, &scratch, image, nullptr, &right, kCopyEngine2D);
        a.copy(cmd, &s_scratch[1], nullptr, &scratch, image, nullptr, &left, kCopyEngine2D);
        a.barrier(cmd, kBarrier);
    }
    ++s_flips;
    if (isPowerOfTwo(s_flips) && s_flips <= (1u << 20)) {
        MMLOG("flip %llu: image %p %dx%d, %d rounds (HUD passes without a flip so far: %llu)",
              static_cast<unsigned long long>(s_flips), image, width, height, rounds,
              static_cast<unsigned long long>(s_unflippedPasses));
    }
    return true;
}

// ---- Hooks -----------------------------------------------------------------------------------------------------

void logTarget(LayerInfo* layer, const void* texture) {
    if (layer == nullptr || layer->lastTarget == texture || layer->targetLogs >= 4 || !s_apiReady.load()) { return; }
    layer->lastTarget = texture;
    ++layer->targetLogs;
    const Api& a = s_api;
    MMLOG("layer '%s' draws to image %p: %dx%d, format %d, samples %d", layer->name, texture, a.textureWidth(texture),
          a.textureHeight(texture), a.textureFormat(texture), a.textureSamples(texture));
}

// The HUD layer's first write to a freshly drawn world image, a bind or a copy, flips it first. Everything the HUD
// layer puts on the image afterwards stays unmirrored, including a picture another mod copies onto the screen.
bool flipIfDue(ThreadSlot* slot, void* cmd, const void* texture) {
    if (!slot->flipPending || !s_apiReady.load() || s_flipFailed) { return false; }
    if (!isFreshWorldImage(texture, s_hudPass.load(std::memory_order_relaxed))) { return false; }
    if (!flipImage(cmd, texture)) { return false; }
    slot->flipPending = false;
    return true;
}

void onColorBind(void* cmd, const void* texture) {
    ThreadSlot* slot = findSlot(threadKey());
    LayerInfo* layer = slot != nullptr ? slot->layer : nullptr;
    logTarget(layer, texture);
    if (layer == nullptr || layer->kind == LayerKind::World) {
        markWorldWrite(texture);
        return;
    }
    if (layer->kind == LayerKind::Hud) { flipIfDue(slot, cmd, texture); }
}

void wrappedSetRenderTargets(u64 cmd, u64 count, u64 colors, u64 views, u64 depth, u64 depthView, u64 a6, u64 a7) {
    const auto* list = reinterpret_cast<const void* const*>(colors);
    if (static_cast<int>(static_cast<u32>(count)) > 0 && list != nullptr && list[0] != nullptr) {
        onColorBind(reinterpret_cast<void*>(cmd), list[0]);
    }
    s_realSetRenderTargets.load(std::memory_order_relaxed)(cmd, count, colors, views, depth, depthView, a6, a7);
}

u64 s_flipsAtCopy = 0;

// Image copies done by a world layer (or an untracked thread) count as world image writes. A copy in the HUD layer
// flips its target first if it is due: Fast Travel copies its menu onto the screen before the HUD layer's bind
// reaches this module, so without this the menu would be mirrored.
void wrappedCopy(u64 cmd, u64 src, u64 srcView, u64 srcRegion, u64 dst, u64 dstView, u64 dstRegion, u64 flags) {
    if (dst != 0) {
        ThreadSlot* slot = findSlot(threadKey());
        LayerInfo* layer = slot != nullptr ? slot->layer : nullptr;
        if (layer == nullptr || layer->kind == LayerKind::World) {
            markWorldWrite(reinterpret_cast<const void*>(dst));
        } else if (layer->kind == LayerKind::Hud &&
                   flipIfDue(slot, reinterpret_cast<void*>(cmd), reinterpret_cast<const void*>(dst)) &&
                   isPowerOfTwo(++s_flipsAtCopy) && s_flipsAtCopy <= (1u << 20)) {
            MMLOG("flip ran at a HUD-layer image copy onto image %#lx (another mod drawing on the screen): %llu times",
                  static_cast<unsigned long>(dst), static_cast<unsigned long long>(s_flipsAtCopy));
        }
    }
    s_realCopy.load(std::memory_order_relaxed)(cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags);
}

GenericFn wrappedLookup(const void* device, const char* name) {
    const GenericFn real = s_realLookup(device, name);
    if (real == nullptr || name == nullptr) { return real; }
    if (sameText(name, "nvnCommandBufferSetRenderTargets")) {
        s_realSetRenderTargets.store(reinterpret_cast<Forward8Fn>(real));
        return reinterpret_cast<GenericFn>(&wrappedSetRenderTargets);
    }
    if (sameText(name, "nvnCommandBufferCopyTextureToTexture")) {
        s_realCopy.store(reinterpret_cast<Forward8Fn>(real));
        return reinterpret_cast<GenericFn>(&wrappedCopy);
    }
    return real;
}

HOOK_DEFINE_TRAMPOLINE(LoadGraphicsProcs) {
    static void Callback(const void* device, LookupFn lookup) {
        if (lookup == nullptr) {
            MMLOG("graphics table load without a lookup function; left unwrapped");
            Orig(device, lookup);
            return;
        }
        s_realLookup = lookup;
        if (device != nullptr) { s_device = device; }
        Orig(device, &wrappedLookup);
        MMLOG("graphics table loaded (device %p): render-target bind %s, image copy %s", device,
              s_realSetRenderTargets.load() ? "wrapped" : "missing", s_realCopy.load() ? "wrapped" : "missing");
        resolveApi();
    }
};

HOOK_DEFINE_TRAMPOLINE(DrawLayer) {
    static void Callback(void* display, void* layer, u32 key) {
        LayerInfo* info = layer != nullptr ? noteLayer(layer, key) : nullptr;
        const uintptr_t thread = threadKey();
        ThreadSlot* slot = findSlot(thread);
        const bool nested = slot != nullptr;
        LayerInfo* const outerLayer = nested ? slot->layer : nullptr;
        const bool outerPending = nested && slot->flipPending;
        if (slot == nullptr) { slot = claimSlot(thread); }
        const bool hud = info != nullptr && info->kind == LayerKind::Hud;
        if (info != nullptr) {
            const u64 draws = info->draws.fetch_add(1, std::memory_order_relaxed) + 1;
            if (hud && isPowerOfTwo(draws) && draws >= 64 && draws <= (1u << 16)) {
                MMLOG("HUD pass %llu: flips %llu, HUD passes without a flip %llu", static_cast<unsigned long long>(draws),
                      static_cast<unsigned long long>(s_flips), static_cast<unsigned long long>(s_unflippedPasses));
                const int count = s_layerCount.load(std::memory_order_acquire);
                for (int i = 0; i < count; ++i) {
                    MMLOG("  layer '%s' drawn %llu times", s_layers[i].name,
                          static_cast<unsigned long long>(s_layers[i].draws.load(std::memory_order_relaxed)));
                }
            }
        }
        if (hud) { s_hudPass.fetch_add(1, std::memory_order_relaxed); }
        if (slot != nullptr) {
            slot->layer = info;
            slot->flipPending = hud;
        }
        Orig(display, layer, key);
        if (slot == nullptr) { return; }
        if (hud && slot->flipPending) { ++s_unflippedPasses; }
        if (nested) {
            slot->layer = outerLayer;
            slot->flipPending = outerPending;
        } else {
            slot->layer = nullptr;
            slot->flipPending = false;
            slot->key.store(0, std::memory_order_release);
        }
    }
};

// ---- Stick mirroring -------------------------------------------------------------------------------------------
//
// The game's controller module keeps five game controllers, numbered 0-4 by its lookup routine (0x3B25E00). The
// player, camera and AI stick queries read controller 0. The module pauses exactly these five when the game pauses
// (0x857EB0), so menus read other controllers. Each game controller is a combined controller that takes the stick of
// its first moving input controller. While a game controller updates, the module mirrors its input controllers'
// sticks (x negated, left and right direction bits swapped) and restores them afterwards. The game controller and
// everything derived from it see mirrored input; every other controller sees the real values.

// Controller update shared by every controller class: returns at once while the controller is paused, then combines
// the inputs, derives direction bits and repeats, and updates the controller's wrappers.
constexpr ptrdiff_t kControllerCalc = 0x36A220;
constexpr u32 kControllerCalcWords[4] = {0x3944E408, 0x37001788, 0xD503237F, 0xD10243FF};
constexpr ptrdiff_t kCombinedCalcImpl = 0x3B27EA0;          // the combined controller's own input step
constexpr ptrdiff_t kControllerModuleInstance = 0x4E92F10;  // controller module pointer (zero until created)
constexpr uintptr_t kGameControllerSlots[5] = {0xB0, 0x110, 0x118, 0x120, 0xB8};
constexpr uintptr_t kCalcImplVtableSlot = 0x38;
constexpr uintptr_t kPausedByte = 0x139;  // bit 0 set while the game is paused
constexpr uintptr_t kCrossStartLeft = 0x104;
constexpr uintptr_t kCrossStartRight = 0x108;
constexpr uintptr_t kHoldMask = 0x114;
constexpr uintptr_t kLeftStickX = 0x120;
constexpr uintptr_t kRightStickX = 0x128;
constexpr uintptr_t kInputCount = 0x180;
constexpr uintptr_t kInputs = 0x188;
constexpr int kCrossLeft = 2;   // direction bits: up, down, left, right from the start bit (0x36AA98, 0x36ABAC)
constexpr int kCrossRight = 3;
constexpr int kMaxInputs = 16;
constexpr int kGameControllers = 5;

template <typename T>
T& field(void* object, uintptr_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(object) + offset);
}

uintptr_t mainBase() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

int gameControllerIndex(void* controller) {
    void* module = *reinterpret_cast<void**>(mainBase() + kControllerModuleInstance);
    if (module == nullptr) { return -1; }
    for (int i = 0; i < kGameControllers; ++i) {
        if (field<void*>(module, kGameControllerSlots[i]) == controller) { return i; }
    }
    return -1;
}

void swapCrossBits(void* input, uintptr_t startOffset) {
    const s32 start = field<s32>(input, startOffset);
    if (start < 0 || start > 28) { return; }
    u32& hold = field<u32>(input, kHoldMask);
    const u32 left = 1u << (start + kCrossLeft);
    const u32 right = 1u << (start + kCrossRight);
    if (((hold & left) != 0) != ((hold & right) != 0)) { hold ^= left | right; }
}

// Mirrors one input controller's sticks; applying it twice restores them.
void mirrorInput(void* input) {
    field<float>(input, kLeftStickX) = -field<float>(input, kLeftStickX);
    field<float>(input, kRightStickX) = -field<float>(input, kRightStickX);
    swapCrossBits(input, kCrossStartLeft);
    swapCrossBits(input, kCrossStartRight);
}

u64 s_mirroredUpdates = 0;
bool s_gameControllerLogged[kGameControllers] = {};
bool s_notCombinedLogged[kGameControllers] = {};

// ---- Motion (gyro) mirroring -------------------------------------------------------------------------------------
//
// Every motion sample reaches the game through one system call, nn::hid::GetSixAxisSensorStates. Its only callers are
// in the Npad device update (0x34693D0), which asks for 16 samples per motion sensor into the pad's sample buffers and
// ignores the returned count. Everything that reads motion (the camera's gyro aim and the controller motion add-ons)
// works from those buffers. The module wraps the call at its import entry (main+0x3C428A0) and mirrors each sample the
// call wrote across the sensor's x axis, the controller's horizontal axis in both grips (P = diag(-1, 1, 1)):
// acceleration x is negated; rotation rate and angle keep x and negate y and z; the orientation becomes P * D * P (the
// entries mixing x with y or z are negated). Pitch is kept; yaw and roll reverse. Each call copies fresh samples from
// the system, so a sample is mirrored exactly once. Written samples are found by marking each sample's time step
// before the call; a sample the call did not write gets its time step back unchanged.

constexpr ptrdiff_t kGetSixAxisStates = 0x3C428A0;
constexpr u32 kGetSixAxisStatesWords[4] = {0xB00083F0, 0xF9430E11, 0x91186210, 0xD61F0220};

struct MotionSample {  // nn::hid::SixAxisSensorState
    s64 timeStep;
    s64 sampleNumber;
    float acceleration[3];
    float rotationRate[3];
    float angle[3];
    float orientation[3][3];
    u32 attributes;
    u32 reserved;
};
static_assert(sizeof(MotionSample) == 0x60, "the game's sample buffers use 0x60-byte samples");
static_assert(offsetof(MotionSample, rotationRate) == 0x1C && offsetof(MotionSample, orientation) == 0x34);

constexpr s64 kUnwrittenMark = 0x7FF5A5A5A5A5A5A5ll;  // never a real time step
constexpr int kMaxMarkedSamples = 32;

u64 s_motionCalls = 0, s_motionSamples = 0, s_motionCountDiffers = 0, s_motionUnmarked = 0;
const void* s_motionHandlesLogged[4] = {};

void mirrorSample(MotionSample& s) {
    s.acceleration[0] = -s.acceleration[0];
    s.rotationRate[1] = -s.rotationRate[1];
    s.rotationRate[2] = -s.rotationRate[2];
    s.angle[1] = -s.angle[1];
    s.angle[2] = -s.angle[2];
    s.orientation[0][1] = -s.orientation[0][1];
    s.orientation[0][2] = -s.orientation[0][2];
    s.orientation[1][0] = -s.orientation[1][0];
    s.orientation[2][0] = -s.orientation[2][0];
}

void logMotion(const void* handle, int requested, int returned, int written, const MotionSample* out) {
    bool known = false;
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        known = known || s_motionHandlesLogged[i] == handle;
        if (slot < 0 && s_motionHandlesLogged[i] == nullptr) { slot = i; }
    }
    if (!known && slot >= 0 && written > 0) {
        s_motionHandlesLogged[slot] = handle;
        MMLOG("motion mirror: sensor %p (handle %08x): %d asked, call returned %d, %d written and mirrored; newest "
              "rotation rate now (%.3f, %.3f, %.3f)",
              handle, handle != nullptr ? *static_cast<const u32*>(handle) : 0u, requested, returned, written,
              static_cast<double>(out[0].rotationRate[0]), static_cast<double>(out[0].rotationRate[1]),
              static_cast<double>(out[0].rotationRate[2]));
    }
    if (isPowerOfTwo(s_motionCalls) && s_motionCalls >= 1024 && s_motionCalls <= (1u << 20)) {
        MMLOG("motion mirror: %llu calls, %llu samples mirrored, %llu calls whose returned count differed from the "
              "samples written",
              static_cast<unsigned long long>(s_motionCalls), static_cast<unsigned long long>(s_motionSamples),
              static_cast<unsigned long long>(s_motionCountDiffers));
    }
}

HOOK_DEFINE_TRAMPOLINE(GetSixAxisStates) {
    static int Callback(MotionSample* out, int count, const void* handle) {
        if (out == nullptr || count <= 0 || count > kMaxMarkedSamples) {
            if (isPowerOfTwo(++s_motionUnmarked)) {
                MMLOG("motion mirror: call with %d samples (buffer %p) passed through unmirrored (%llu times)", count,
                      out, static_cast<unsigned long long>(s_motionUnmarked));
            }
            return Orig(out, count, handle);
        }
        s64 saved[kMaxMarkedSamples];
        for (int i = 0; i < count; ++i) {
            saved[i] = out[i].timeStep;
            out[i].timeStep = kUnwrittenMark;
        }
        const int returned = Orig(out, count, handle);
        int written = 0;
        for (int i = 0; i < count; ++i) {
            if (out[i].timeStep == kUnwrittenMark) {
                out[i].timeStep = saved[i];
                continue;
            }
            mirrorSample(out[i]);
            ++written;
        }
        ++s_motionCalls;
        s_motionSamples += static_cast<u64>(written);
        if (returned != written) { ++s_motionCountDiffers; }
        logMotion(handle, count, returned, written, out);
        return returned;
    }
};

HOOK_DEFINE_TRAMPOLINE(ControllerCalc) {
    static void Callback(void* controller) {
        // A paused controller's update returns at once and changes nothing.
        if ((field<u8>(controller, kPausedByte) & 1) != 0) {
            Orig(controller);
            return;
        }
        updateWithMirroredSticks(controller);
    }

    static void updateWithMirroredSticks(void* controller) {
        const int index = gameControllerIndex(controller);
        if (index < 0) {
            Orig(controller);
            return;
        }
        void* const* vtable = field<void* const*>(controller, 0);
        const s32 count = field<s32>(controller, kInputCount);
        void** inputs = field<void**>(controller, kInputs);
        if (reinterpret_cast<uintptr_t>(vtable[kCalcImplVtableSlot / sizeof(void*)]) != mainBase() + kCombinedCalcImpl ||
            inputs == nullptr || count < 0 || count > kMaxInputs) {
            if (!s_notCombinedLogged[index]) {
                s_notCombinedLogged[index] = true;
                MMLOG("stick mirror: game controller %d (%p) is not a combined controller with up to %d inputs "
                      "(input step %#lx, %d inputs); its sticks are not mirrored",
                      index, controller, kMaxInputs,
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(vtable[kCalcImplVtableSlot / sizeof(void*)]) -
                                                 mainBase()),
                      count);
            }
            Orig(controller);
            return;
        }
        void* mirrored[kMaxInputs];
        int mirroredCount = 0;
        int skippedGame = 0;
        for (int i = 0; i < count; ++i) {
            void* input = inputs[i];
            if (input == nullptr) { continue; }
            // An input that is itself a game controller already holds mirrored values.
            if (gameControllerIndex(input) >= 0) {
                ++skippedGame;
                continue;
            }
            bool seen = false;
            for (int j = 0; j < mirroredCount; ++j) { seen = seen || mirrored[j] == input; }
            if (seen) { continue; }
            mirrorInput(input);
            mirrored[mirroredCount++] = input;
        }
        if (!s_gameControllerLogged[index]) {
            s_gameControllerLogged[index] = true;
            MMLOG("stick mirror: game controller %d (%p) combines %d inputs: %d mirrored, %d are game controllers",
                  index, controller, count, mirroredCount, skippedGame);
        }
        Orig(controller);
        for (int i = 0; i < mirroredCount; ++i) { mirrorInput(mirrored[i]); }
        ++s_mirroredUpdates;
        if (isPowerOfTwo(s_mirroredUpdates) && s_mirroredUpdates >= 1024 && s_mirroredUpdates <= (1u << 20)) {
            MMLOG("stick mirror: %llu game-controller updates mirrored; controller %d sticks now L(%.2f, %.2f) "
                  "R(%.2f, %.2f)",
                  static_cast<unsigned long long>(s_mirroredUpdates), index,
                  static_cast<double>(field<float>(controller, kLeftStickX)),
                  static_cast<double>(field<float>(controller, kLeftStickX + 4)),
                  static_cast<double>(field<float>(controller, kRightStickX)),
                  static_cast<double>(field<float>(controller, kRightStickX + 4)));
        }
    }
};

bool layoutMatches() {
    const auto& main = exl::util::GetMainModuleInfo();
    const uintptr_t base = main.m_Total.m_Start;
    if (main.m_Rodata.m_Start - base != kMainRodataOffset || main.m_Data.m_Start - base != kMainDataOffset) {
        MMLOG("main layout differs from build E0F3CCFA (rodata +%#lx, data +%#lx); nothing installed",
              main.m_Rodata.m_Start - base, main.m_Data.m_Start - base);
        return false;
    }
    return true;
}

// Another mod's load-time hook near main replaces only the first word with a branch into that mod (exlaunch's short
// form). Modules start from the highest slot down, so Fast Travel (slot 5) is already in place when this module
// (slot 4) starts. Mirror Mode installs on top: its hook runs first and then calls the other mod's hook, which calls the
// game. Any other first word, or a branch back into main, is still a mismatch.
bool isOtherModHook(ptrdiff_t offset, u32 word, uintptr_t& target) {
    if ((word & 0xFC000000u) != 0x14000000u) { return false; }
    const auto& main = exl::util::GetMainModuleInfo().m_Total;
    const s64 delta = static_cast<s64>(static_cast<s32>(word << 6) >> 6) * 4;
    target = main.m_Start + offset + delta;
    return !main.InRange(target);
}

bool siteMatches(const char* what, ptrdiff_t offset, const u32 (&expected)[4], const char* consequence,
                 bool shareable = false) {
    const auto* words = reinterpret_cast<const u32*>(exl::util::GetMainModuleInfo().m_Total.m_Start + offset);
    uintptr_t target = 0;
    const bool hooked = shareable && words[0] != expected[0] && isOtherModHook(offset, words[0], target);
    for (int i = hooked ? 1 : 0; i < 4; ++i) {
        if (words[i] != expected[i]) {
            MMLOG("%s at main+%#lx: word %d is %08x, expected %08x (another code mod?); %s", what,
                  static_cast<unsigned long>(offset), i, words[i], expected[i], consequence);
            return false;
        }
    }
    if (hooked) {
        MMLOG("%s at main+%#lx is already hooked by another code mod (branch to %#lx); installing on top of it", what,
              static_cast<unsigned long>(offset), static_cast<unsigned long>(target));
    }
    return true;
}

} // namespace

extern "C" void exl_main(void* /*x0*/, void* /*x1*/) {
    MMLOG("step 1/3: module started");
    // The three hooks Fast Travel shares may already hold its branch; the motion call site must be vanilla.
    if (!layoutMatches() ||
        !siteMatches("graphics table load", kLoadGraphicsProcs, kLoadGraphicsProcsWords, "nothing installed", true) ||
        !siteMatches("layer draw", kDrawLayer, kDrawLayerWords, "nothing installed", true)) {
        return;
    }
    // The stick and motion mirrors are installed only when their own sites match; the picture flip does not depend on
    // them.
    const bool sticks =
        siteMatches("controller update", kControllerCalc, kControllerCalcWords, "sticks are not mirrored", true);
    const bool motion =
        siteMatches("motion sample call", kGetSixAxisStates, kGetSixAxisStatesWords, "motion is not mirrored");
    MMLOG("step 2/3: hook sites checked: picture flip yes, stick mirror %s, motion mirror %s",
          sticks ? "yes" : "no", motion ? "yes" : "no");
    exl::hook::Initialize();
    LoadGraphicsProcs::InstallAtOffset(kLoadGraphicsProcs);
    DrawLayer::InstallAtOffset(kDrawLayer);
    if (sticks) { ControllerCalc::InstallAtOffset(kControllerCalc); }
    if (motion) { GetSixAxisStates::InstallAtOffset(kGetSixAxisStates); }
    MMLOG("step 3/3: hooks installed (graphics table load main+%#lx, layer draw main+%#lx, controller update %s, "
          "motion sample call %s)",
          static_cast<unsigned long>(kLoadGraphicsProcs), static_cast<unsigned long>(kDrawLayer),
          sticks ? "main+0x36a220" : "not installed", motion ? "main+0x3c428a0" : "not installed");
}

extern "C" NORETURN void exl_exception_entry() { EXL_ABORT("unreachable"); }
