#include "pictures.hpp"

#include <cstring>

#include "../game.hpp"

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
constexpr char kPath[] = "ftsd:/fast-travel/pictures.bin";
// First words of nn::fs::MountSdCard in SDK build 1643A838 (sdk.bin +0x10F0C0); patch/build_patch.py checks the same.
constexpr u32 kMountWords[4] = {0xD503237F, 0xD10103FF, 0xA9017BFD, 0xF90013F5};

// Layout of the picture file (tools/make_assets.py documents it); every offset is from the start of the file.
struct FileHeader {
    char magic[4];
    u32 imageCount, reserved[2];
};

struct Entry {
    char name[40];
    u32 preview3, preview2, thumb3, thumb2, reserved[2];
};
static_assert(sizeof(Entry) == 64);

constexpr int kMaxPictures = 48;
constexpr int kThumbW[2] = {32, 48}, kThumbH[2] = {18, 27};       // index 0: scale 2, 1: scale 3
constexpr int kPreviewW[2] = {300, 450}, kPreviewH[2] = {168, 252};
constexpr size_t kThumbBytes[2] = {32 * 18 * 3, 48 * 27 * 3};
constexpr size_t kPreviewBytes[2] = {300 * 168 * 3, 450 * 252 * 3};

enum class State { Off, Unread, Ready, Failed };
State s_state = State::Off;
int s_count = 0;
Entry s_entries[kMaxPictures];

// The thumbnails of one screen scale (the screen's), read all at once on first use.
u8 s_thumbs[kMaxPictures][kThumbBytes[1]];
int s_thumbScale = -1;    // scale index the thumbnails hold, or -1
bool s_thumbFailed[2] = {};

// The cursor's area's preview, with the pictures that could not be read. One slot, so that the menu's two images fit
// in the module's memory.
alignas(16) u8 s_preview[kPreviewBytes[1]];
int s_previewKey = -1;
u64 s_failed[2] = {};

int scaleIndex(int scale) { return scale == 3 ? 1 : (scale == 2 ? 0 : -1); }

bool terminated(const char (&name)[sizeof(Entry::name)]) {
    for (const char c : name) {
        if (c == '\0') { return true; }
    }
    return false;
}

bool sameName(const char* a, const char* b) {
    for (size_t i = 0; i < sizeof(Entry::name); ++i) {
        if (a[i] != b[i]) { return false; }
        if (a[i] == '\0') { return true; }
    }
    return false;
}

MountFn mountRoutine() {
#if FAST_TRAVEL_PATCH
    const auto* words = reinterpret_cast<const u32*>(patch_sdk_mount_sdcard());
    for (int i = 0; i < 4; ++i) {
        if (words[i] != kMountWords[i]) {
            WTLOG("pictures: the SD card mount routine's word %d is %08x, expected %08x; pictures are not shown", i,
                  words[i], kMountWords[i]);
            return nullptr;
        }
    }
    return reinterpret_cast<MountFn>(words);
#else
    WTLOG("pictures: this build cannot mount the SD card; pictures are not shown");
    return nullptr;
#endif
}

// An open picture file, closed when it goes out of scope.
struct File {
    FileHandle handle{};
    bool open = false;
    s64 size = 0;

    u32 openRead() {
        u32 result = gameFunction<OpenFileFn>(kOpenFile)(&handle, kPath, kOpenRead);
        if (result != 0) { return result; }
        open = true;
        return gameFunction<GetFileSizeFn>(kGetFileSize)(&size, handle);
    }
    bool read(s64 offset, void* buffer, size_t bytes) {
        size_t got = 0;
        return gameFunction<ReadFileFn>(kReadFile)(&got, handle, offset, buffer, bytes) == 0 && got == bytes;
    }
    ~File() {
        if (open) { gameFunction<CloseFileFn>(kCloseFile)(handle); }
    }
};

bool within(u64 offset, u64 bytes, s64 size) { return offset != 0 && offset + bytes <= static_cast<u64>(size); }

bool fail(const char* what, long long value) {
    s_state = State::Failed;
    WTLOG("pictures: %s (%#llx); the menu draws empty frames", what, value);
    return false;
}

bool load() {
    if (s_state != State::Unread) { return s_state == State::Ready; }
    const MountFn mount = mountRoutine();
    if (mount == nullptr) { return fail("no SD card mount routine", 0); }
    const u32 mounted = mount(kMountName);
    if (mounted != 0) { return fail("SD card mount refused", mounted); }
    File file;
    const u32 opened = file.openRead();
    if (opened != 0) { return fail("cannot open fast-travel/pictures.bin on the SD card", opened); }
    FileHeader header;
    if (!file.read(0, &header, sizeof(header)) || std::memcmp(header.magic, "FTI1", 4) != 0) {
        return fail("pictures.bin is not a picture file", file.size);
    }
    if (header.imageCount == 0 || header.imageCount > kMaxPictures ||
        !file.read(sizeof(header), s_entries, header.imageCount * sizeof(Entry))) {
        return fail("pictures.bin picture count unsupported or table unreadable; count", header.imageCount);
    }
    for (u32 i = 0; i < header.imageCount; ++i) {
        const Entry& e = s_entries[i];
        if (!terminated(e.name) || !within(e.preview3, kPreviewBytes[1], file.size) ||
            !within(e.preview2, kPreviewBytes[0], file.size) || !within(e.thumb3, kThumbBytes[1], file.size) ||
            !within(e.thumb2, kThumbBytes[0], file.size)) {
            return fail("pictures.bin entry out of range; entry", i);
        }
    }
    s_count = static_cast<int>(header.imageCount);
    s_state = State::Ready;
    WTLOG("pictures: table of %d areas read from the SD card (%lld bytes)", s_count, static_cast<long long>(file.size));
    return true;
}

bool loadThumbs(int index) {
    if (s_thumbScale == index) { return true; }
    if (s_thumbFailed[index]) { return false; }
    s_thumbScale = -1;
    File file;
    const u32 opened = file.openRead();
    for (int i = 0; i < s_count; ++i) {
        if (opened != 0 || !file.read(index == 1 ? s_entries[i].thumb3 : s_entries[i].thumb2, s_thumbs[i],
                                      kThumbBytes[index])) {
            s_thumbFailed[index] = true;
            WTLOG("pictures: thumbnails at scale %d not shown (entry %d, file open %#x)", index + 2, i, opened);
            return false;
        }
    }
    s_thumbScale = index;
    return true;
}

}  // namespace

void enable() {
    if (s_state == State::Off) { s_state = State::Unread; }
}

int find(const char* name) {
    if (name == nullptr || !load()) { return -1; }
    for (int i = 0; i < s_count; ++i) {
        if (sameName(s_entries[i].name, name)) { return i; }
    }
    return -1;
}

Picture thumbnail(int picture, int scale) {
    const int index = scaleIndex(scale);
    if (s_state != State::Ready || picture < 0 || picture >= s_count || index < 0 || !loadThumbs(index)) { return {}; }
    return {s_thumbs[picture], kThumbW[index], kThumbH[index]};
}

Picture preview(int picture, int scale) {
    const int index = scaleIndex(scale);
    if (s_state != State::Ready || picture < 0 || picture >= s_count || index < 0 ||
        (s_failed[index] >> picture & 1) != 0) {
        return {};
    }
    const int key = picture * 4 + scale;
    const Picture shown{s_preview, kPreviewW[index], kPreviewH[index]};
    if (s_previewKey == key) { return shown; }
    s_previewKey = -1;
    const Entry& e = s_entries[picture];
    File file;
    const u32 opened = file.openRead();
    if (opened != 0 || !file.read(index == 1 ? e.preview3 : e.preview2, s_preview, kPreviewBytes[index])) {
        s_failed[index] |= u64(1) << picture;
        WTLOG("pictures: %s preview at scale %d not shown (file open %#x)", e.name, scale, opened);
        return {};
    }
    s_previewKey = key;
    return shown;
}

}  // namespace warp::ui::pictures
