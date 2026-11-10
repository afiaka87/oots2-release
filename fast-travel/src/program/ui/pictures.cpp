#include "pictures.hpp"

#include "../game.hpp"
#include "../menu.hpp"
#include "jpeg.hpp"

#if FAST_TRAVEL_PATCH
// The SDK's nn::fs::MountSdCard, found from the game's tick import entry (patch/adapters.S). The game does not import it.
extern "C" std::uintptr_t patch_sdk_mount_sdcard();
#endif

namespace warp::ui::pictures {
namespace {

using MountFn = u32 (*)(const char* name);
using OpenFileFn = u32 (*)(FileHandle* out, const char* path, int mode);
using GetFileSizeFn = u32 (*)(s64* size, FileHandle file);
using ReadFileFn = u32 (*)(size_t* read, FileHandle file, s64 offset, void* buffer, size_t size);
using CloseFileFn = void (*)(FileHandle file);

constexpr char kMountName[] = "ftsd";
constexpr char kFolder[] = "ftsd:/fast-travel/pictures/";
constexpr char kExtension[] = ".jpg";
// First words of nn::fs::MountSdCard in SDK build 1643A838 (sdk.bin +0x10F0C0); patch/build_patch.py checks the same.
constexpr u32 kMountWords[4] = {0xD503237F, 0xD10103FF, 0xA9017BFD, 0xF90013F5};

constexpr int kMaxPictures = 64;
constexpr int kNameBytes = 48;
// Sizes per screen scale (index 0: scale 2, 1: scale 3); they match the frames in panel.cpp (16 x 9 and 150 x 84
// units at 2.5 and 3.75 pixels per unit).
constexpr int kThumbW[2] = {40, 60}, kThumbH[2] = {22, 34};
constexpr int kPreviewW[2] = {375, 562}, kPreviewH[2] = {210, 315};
constexpr size_t kThumbBytes = kThumbW[1] * kThumbH[1] * 3;
constexpr size_t kPreviewBytes = kPreviewW[1] * kPreviewH[1] * 3;

enum class Search { Off, Unchecked, Found, None };
Search s_search = Search::Off;

enum class Slot : u8 { Unread, Ready, Failed };
const char* s_names[kMaxPictures];
int s_count = 0;
Slot s_thumbSlot[kMaxPictures];  // for the scale in s_thumbScale
int s_thumbScale = -1;
u8 s_thumbs[kMaxPictures][kThumbBytes];
bool s_previewFailed[kMaxPictures];
int s_previewKey = -1;
alignas(16) u8 s_preview[kPreviewBytes];

int scaleIndex(int scale) { return scale == 3 ? 1 : (scale == 2 ? 0 : -1); }

bool sameName(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (*a != *b) { return false; }
        if (*a == '\0') { return true; }
    }
}

MountFn mountRoutine() {
#if FAST_TRAVEL_PATCH
    const auto* words = reinterpret_cast<const u32*>(patch_sdk_mount_sdcard());
    for (int i = 0; i < 4; ++i) {
        if (words[i] != kMountWords[i]) {
            WTLOG("pictures: the SD card mount routine's word %d is %08x, expected %08x; no pictures", i, words[i],
                  kMountWords[i]);
            return nullptr;
        }
    }
    return reinterpret_cast<MountFn>(words);
#else
    WTLOG("pictures: this build cannot mount the SD card; no pictures");
    return nullptr;
#endif
}

// A picture file, closed when it goes out of scope.
struct File {
    FileHandle handle{};
    bool open = false;
    s64 size = 0;

    explicit File(const char* name) {
        char path[sizeof(kFolder) + kNameBytes + sizeof(kExtension)];
        size_t n = 0;
        for (const char* c = kFolder; *c != '\0'; ++c) { path[n++] = *c; }
        for (const char* c = name; *c != '\0' && n < sizeof(kFolder) + kNameBytes - 1; ++c) { path[n++] = *c; }
        for (const char* c = kExtension; *c != '\0'; ++c) { path[n++] = *c; }
        path[n] = '\0';
        if (gameFunction<OpenFileFn>(kOpenFile)(&handle, path, kOpenRead) != 0) { return; }
        open = true;
        if (gameFunction<GetFileSizeFn>(kGetFileSize)(&size, handle) != 0) { size = 0; }
    }
    ~File() {
        if (open) { gameFunction<CloseFileFn>(kCloseFile)(handle); }
    }

    static size_t read(void* context, int64_t offset, void* buffer, size_t bytes) {
        const File& file = *static_cast<const File*>(context);
        if (offset >= file.size) { return 0; }
        if (static_cast<s64>(bytes) > file.size - offset) { bytes = static_cast<size_t>(file.size - offset); }
        size_t got = 0;
        return gameFunction<ReadFileFn>(kReadFile)(&got, file.handle, offset, buffer, bytes) == 0 ? got : 0;
    }
};

// Decodes picture `picture` into the given targets; logs and returns false when its file is missing or unreadable.
bool decode(int picture, jpeg::Target large, jpeg::Target small) {
    File file(s_names[picture]);
    if (!file.open) {
        WTLOG("pictures: %s%s not found; its frame stays empty", s_names[picture], kExtension);
        return false;
    }
    const u64 start = svcGetSystemTick();
    const char* error = jpeg::decode(File::read, &file, large, small);
    const u64 ticks = svcGetSystemTick() - start;
    if (error != nullptr) {
        WTLOG("pictures: %s%s not shown: %s", s_names[picture], kExtension, error);
        return false;
    }
    WTLOG("pictures: %s%s decoded (%s) in %llu us", s_names[picture], kExtension,
          large.rgb != nullptr ? "large" : "small", static_cast<unsigned long long>(ticks * 1000000 / 19200000));
    return true;
}

void useThumbScale(int index) {
    if (s_thumbScale == index) { return; }
    for (Slot& slot : s_thumbSlot) { slot = Slot::Unread; }
    s_thumbScale = index;
}

}  // namespace

void enable() {
    if (s_search == Search::Off) { s_search = Search::Unchecked; }
}

bool available() {
    if (s_search != Search::Unchecked) { return s_search == Search::Found; }
    s_search = Search::None;
    const MountFn mount = mountRoutine();
    if (mount == nullptr) { return false; }
    const u32 mounted = mount(kMountName);
    if (mounted != 0) {
        WTLOG("pictures: SD card mount refused (%#x); the menu shows the list alone", mounted);
        return false;
    }
    for (int i = 0; i < kAreaCount; ++i) {
        if (File(kAreas[i].banc).open) {
            s_search = Search::Found;
            WTLOG("pictures: found %s%s; the menu shows pictures", kAreas[i].banc, kExtension);
            return true;
        }
    }
    WTLOG("pictures: no area pictures in fast-travel/pictures on the SD card; the menu shows the list alone");
    return false;
}

int find(const char* name) {
    if (name == nullptr || s_search != Search::Found) { return -1; }
    for (int i = 0; i < s_count; ++i) {
        if (sameName(s_names[i], name)) { return i; }
    }
    int length = 0;
    while (name[length] != '\0') { ++length; }
    if (s_count == kMaxPictures || length >= kNameBytes) { return -1; }
    s_names[s_count] = name;
    return s_count++;
}

Picture thumbnail(int picture, int scale) {
    const int index = scaleIndex(scale);
    if (picture < 0 || picture >= s_count || index < 0) { return {}; }
    useThumbScale(index);
    Slot& slot = s_thumbSlot[picture];
    if (slot == Slot::Unread) {
        slot = decode(picture, {}, {s_thumbs[picture], kThumbW[index], kThumbH[index]}) ? Slot::Ready : Slot::Failed;
    }
    if (slot != Slot::Ready) { return {}; }
    return {s_thumbs[picture], kThumbW[index], kThumbH[index]};
}

Picture preview(int picture, int scale) {
    const int index = scaleIndex(scale);
    if (picture < 0 || picture >= s_count || index < 0 || s_previewFailed[picture]) { return {}; }
    const Picture shown{s_preview, kPreviewW[index], kPreviewH[index]};
    const int key = picture * 4 + scale;
    if (s_previewKey == key) { return shown; }
    s_previewKey = -1;
    useThumbScale(index);
    Slot& slot = s_thumbSlot[picture];
    // One pass fills the thumbnail too when it is not read yet.
    const jpeg::Target small = slot == Slot::Unread ? jpeg::Target{s_thumbs[picture], kThumbW[index], kThumbH[index]}
                                                    : jpeg::Target{};
    if (!decode(picture, {s_preview, kPreviewW[index], kPreviewH[index]}, small)) {
        s_previewFailed[picture] = true;
        if (slot == Slot::Unread) { slot = Slot::Failed; }
        return {};
    }
    if (slot == Slot::Unread) { slot = Slot::Ready; }
    s_previewKey = key;
    return shown;
}

}  // namespace warp::ui::pictures
