// SPDX-License-Identifier: MIT
// Copyright (c) HanSoloBlaster, afiaka87
// Startup and services for the patch build, adapted from Ultimate Ascend.
#include <lib.hpp>

#include <cstdarg>
#include <cstdio>

extern "C" void exl_main(void*, void*);

namespace {

// The generated code reads this anchor in main's writable padding.
struct alignas(16) PatchAnchor {
    std::uintptr_t data;
    u64 hooks;
};

// The first 32 stored bytes; LZ4-packed state follows.
struct PatchHeader {
    u32 magic, packedBytes, rawBytes, regionBytes, hash, fixupOffset, dataFixups, mainFixups;
};
static_assert(sizeof(PatchAnchor) == 16 && sizeof(PatchHeader) == 32);

constexpr u32 kMagic = 0x31505446;  // "FTP1"
constexpr std::size_t kRegionAlignment = 0x10000;  // the menu image's memory pool storage is 64 KiB aligned
constexpr std::size_t kPage = 0x1000;
constexpr std::size_t kMaxRegionBytes = 0x800000;  // 8 MiB
constexpr u32 kTextEnd = 0x3C461D0;

struct HookBit {
    std::ptrdiff_t site;
    u32 bit;
};
constexpr HookBit kHookBits[] = {
#include "patch_hooks.inc"
};

}  // namespace

extern "C" {
extern PatchAnchor patch_anchor;   // fixed main offset, set by build_patch.py
extern const u32 patch_chain_head;  // first stored chunk, set by build_patch.py
std::uintptr_t patch_debug_routine();  // the SDK's debug output routine, found from the game's tick import entry
u32 patch_allocate_region(std::uintptr_t* address, std::size_t size);  // nn::os::AllocateAddressRegion
u32 patch_allocate_pages(std::uintptr_t address, std::size_t size);    // nn::os::AllocateMemoryPages
u32 patch_free_region(std::uintptr_t address);                         // nn::os::FreeAddressRegion
u32 patch_free_pages(std::uintptr_t address, std::size_t size);        // nn::os::FreeMemoryPages
}

namespace {

// Each chunk starts with the next main offset divided by four in the high 24 bits and byte count in the low 8.
struct ChainReader {
    std::uintptr_t base;
    u32 next;
    std::size_t left;
    const u8* bytes = nullptr;
    u32 count = 0;
    u32 index = 0;

    std::size_t remaining() const { return left; }
    bool operator()(u8& byte) {
        if (left == 0) { return false; }
        while (index == count) {
            if (next == 0 || (next & 3) != 0 || next > kTextEnd - 8) { return false; }
            const u32 header = *reinterpret_cast<const u32*>(base + next);
            count = header & 0xFF;
            if (count == 0 || count > 24 || count > kTextEnd - next - 4) { return false; }
            bytes = reinterpret_cast<const u8*>(base + next + 4);
            index = 0;
            next = (header >> 8) << 2;
        }
        byte = bytes[index++];
        --left;
        return true;
    }
    bool ended() const { return left == 0 && index == count && next == 0; }
};

// Bounded LZ4 block decoding (Ultimate Ascend's Decode.hpp). Offsets refer only to output already produced.
template <class Read>
bool decodeBlock(Read& read, u8* output, std::size_t size) {
    std::size_t written = 0;
    const auto length = [&](std::size_t base, std::size_t& result) {
        result = base;
        if (base != 15) { return true; }
        u8 byte;
        do {
            if (!read(byte) || result > size || byte > size - result) { return false; }
            result += byte;
        } while (byte == 255);
        return true;
    };
    while (read.remaining()) {
        u8 token;
        if (!read(token)) { return false; }
        std::size_t count;
        if (!length(token >> 4, count) || count > size - written) { return false; }
        for (std::size_t i = 0; i < count; ++i) {
            if (!read(output[written++])) { return false; }
        }
        if (!read.remaining()) { return written == size; }
        u8 low, high;
        if (!read(low) || !read(high)) { return false; }
        const std::size_t offset = low | (std::size_t(high) << 8);
        if (!offset || offset > written || !length(token & 15, count) || count > size - written ||
            size - written - count < 4) {
            return false;
        }
        count += 4;
        for (std::size_t i = 0; i < count; ++i, ++written) { output[written] = output[written - offset]; }
    }
    return written == size;
}

consteval u64 packed(const char (&text)[9]) {
    u64 result = 0;
    for (int i = 0; i < 8; ++i) { result |= u64(static_cast<unsigned char>(text[i])) << (8 * i); }
    return result;
}

// Volatile stores keep the format on the stack while patch state is unavailable.
void startFailed(u32 step, u32 code) {
    volatile u64 format[8];
    format[0] = packed("[fast-tr");
    format[1] = packed("avel] pa");
    format[2] = packed("tch star");
    format[3] = packed("t-up fai");
    format[4] = packed("led at s");
    format[5] = packed("tep %u (");
    format[6] = packed("%08x); n");
    format[7] = packed("o hooks\0");
    PatchLogger::Log(const_cast<const char*>(reinterpret_cast<volatile const char*>(format)), step, code);
}

[[gnu::noinline]] void started(const PatchHeader& header, const void* region) {
    Logging.Log("[fast-travel] patch: state unpacked (%u bytes stored, %u in use, %u pointers) at %p",
                header.packedBytes, header.regionBytes, header.dataFixups + header.mainFixups, region);
    exl_main(nullptr, nullptr);
}

bool readHeader(ChainReader& reader, PatchHeader& header) {
    auto* out = reinterpret_cast<u8*>(&header);
    for (std::size_t i = 0; i < sizeof(header); ++i) {
        if (!reader(out[i])) { return false; }
    }
    return true;
}

}  // namespace

void PatchLogger::Log(const char* format, ...) {
    // svcOutputDebugString(text, length) must still match the verified SDK wrapper.
    const auto* routine = reinterpret_cast<const u32*>(patch_debug_routine());
    if (routine[0] != 0xD40004E1u || routine[1] != 0xD65F03C0u) { return; }
    constexpr int kBufferSize = 512;
    char buffer[kBufferSize];
    std::va_list arguments;
    va_start(arguments, format);
    const int result = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (result <= 0) { return; }
    const int length = result < kBufferSize ? result : kBufferSize - 1;
    reinterpret_cast<void (*)(const char*, std::size_t)>(routine)(buffer, static_cast<std::size_t>(length));
}

extern "C" void patch_enable_hook(std::ptrdiff_t site) {
    for (const HookBit& hook : kHookBits) {
        if (hook.site == site) {
            __atomic_fetch_or(&patch_anchor.hooks, u64(1) << hook.bit, __ATOMIC_RELEASE);
            return;
        }
    }
    Logging.Log("[fast-travel] patch: no gate at main+%#lx; that hook stays off", static_cast<unsigned long>(site));
}

extern "C" void patch_initialize() {
    if (__atomic_load_n(&patch_anchor.data, __ATOMIC_ACQUIRE) != 0) { return; }
    svcGetSystemTick();  // binds the game's tick import entry, which the logger follows to the debug output routine
    const std::uintptr_t base = patch_main_base();
    ChainReader reader{base, static_cast<u32>(reinterpret_cast<std::uintptr_t>(&patch_chain_head) - base),
                       sizeof(PatchHeader)};
    PatchHeader header{};
    if (!readHeader(reader, header) || header.magic != kMagic) {
        startFailed(1, header.magic);
        return;
    }
    const std::size_t regionBytes = (static_cast<std::size_t>(header.regionBytes) + kPage - 1) & ~(kPage - 1);
    const u64 fixupBytes = 4ull * (u64(header.dataFixups) + header.mainFixups);
    if (regionBytes == 0 || regionBytes > kMaxRegionBytes || header.rawBytes > regionBytes || header.packedBytes == 0 ||
        (header.fixupOffset & 3) != 0 || header.fixupOffset + fixupBytes != header.rawBytes) {
        startFailed(2, header.regionBytes);
        return;
    }
    std::uintptr_t reserved = 0;
    const u32 reserveResult = patch_allocate_region(&reserved, regionBytes + kRegionAlignment);
    if (reserveResult != 0 || reserved == 0) {
        startFailed(3, reserveResult);
        return;
    }
    const std::uintptr_t region = (reserved + kRegionAlignment - 1) & ~(kRegionAlignment - 1);
    const u32 mapResult = patch_allocate_pages(region, regionBytes);
    if (mapResult != 0) {
        patch_free_region(reserved);
        startFailed(4, mapResult);
        return;
    }
    auto* output = reinterpret_cast<u8*>(region);
    std::memset(output, 0, regionBytes);
    reader.left = header.packedBytes;
    u32 failedStep = 0, failedCode = 0;
    if (!decodeBlock(reader, output, header.rawBytes) || !reader.ended()) {
        failedStep = 5;
        failedCode = header.rawBytes;
    } else {
        u32 hash = 2166136261u;
        for (std::size_t i = 0; i < header.rawBytes; ++i) { hash = (hash ^ output[i]) * 16777619u; }
        if (hash != header.hash) {
            failedStep = 6;
            failedCode = hash;
        }
    }
    const auto* fixups = reinterpret_cast<const u32*>(output + header.fixupOffset);
    for (u32 i = 0; failedStep == 0 && i < header.dataFixups + header.mainFixups; ++i) {
        const u32 at = fixups[i];
        if ((at & 7) != 0 || at + 8ull > header.fixupOffset) {
            failedStep = 7;
            failedCode = at;
            break;
        }
        *reinterpret_cast<u64*>(output + at) += i < header.dataFixups ? region : base;
    }
    if (failedStep != 0) {
        patch_free_pages(region, regionBytes);
        patch_free_region(reserved);
        startFailed(failedStep, failedCode);
        return;
    }
    std::memset(output + header.fixupOffset, 0, fixupBytes);
    __atomic_store_n(&patch_anchor.data, region, __ATOMIC_RELEASE);
    started(header, output);
}
