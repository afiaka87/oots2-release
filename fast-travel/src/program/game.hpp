// Game addresses and small helpers shared by the Fast Travel module. Supported game build: main
// E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348 (update v131072). Every address is an offset from the start of main; each
// hooked or called routine is checked against its first four instruction words before anything is installed.
#pragma once

#include <lib.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

#define WTLOG(...) Logging.Log("[fast-travel] " __VA_ARGS__)

namespace warp {

struct Site {
    const char* what;
    ptrdiff_t offset;
    u32 words[4];
};

// ---- Hooked routines ------------------------------------------------------------------------------------------

// Fills the game's graphics function table: (device, lookup). Same hook as Mirror Mode.
constexpr Site kLoadGraphicsProcs{"graphics table load", 0x129E290, {0xD503237F, 0xA9BE7BFD, 0xA9014FF4, 0x910003FD}};
// Draws one screen layer: (display, layer, sort key). Same hook as Mirror Mode.
constexpr Site kDrawLayer{"layer draw", 0x4CD60, {0xD503237F, 0xA9BA7BFD, 0xA9016FFC, 0xA90267FA}};
// Controller update shared by every controller class (returns at once while paused). Same hook as Mirror Mode.
constexpr Site kControllerCalc{"controller update", 0x36A220, {0x3944E408, 0x37001788, 0xD503237F, 0xD10243FF}};
// Import entries of the five nn::hid::GetNpadStates forms; each has one caller, in the Npad device update.
constexpr Site kNpadStates[5] = {
    {"pad states (dual Joy-Con)", 0x3C427F0, {0xB00083F0, 0xF942E211, 0x91170210, 0xD61F0220}},
    {"pad states (Pro Controller)", 0x3C42800, {0xB00083F0, 0xF942E611, 0x91172210, 0xD61F0220}},
    {"pad states (left Joy-Con)", 0x3C42810, {0xB00083F0, 0xF942EA11, 0x91174210, 0xD61F0220}},
    {"pad states (right Joy-Con)", 0x3C42820, {0xB00083F0, 0xF942EE11, 0x91176210, 0xD61F0220}},
    {"pad states (handheld)", 0x3C42830, {0xB00083F0, 0xF942F211, 0x91178210, 0xD61F0220}},
};
// Every automatic save request: (autosave manager, reason, actor) -> requested. Callers ignore the result; the game
// itself returns false when a save is not allowed. Checked at its start and at its call into the save check.
constexpr Site kAutoSaveRequest{"autosave request", 0xBA7730, {0xD503237F, 0xD10103FF, 0xA9017BFD, 0xA90257F6}};
constexpr Site kAutoSaveRequestBody{"autosave request (body)", 0xBA7754,
                                    {0xAA1403E1, 0x2A1F03E2, 0xAA1503E3, 0x97FFFEDC}};

// ---- Called routines ------------------------------------------------------------------------------------------

// GameData String64 write, no index: (manager, const char** value, name hash). Queued; halts on a full queue.
constexpr Site kSetString64{"text flag write", 0xD24E50, {0xD503237F, 0xD10103FF, 0xA9017BFD, 0xA90257F6}};
// GameData Vec3 write, no index: (manager, const float[3]* value, name hash). Queued; halts on a full queue.
constexpr Site kSetVec3{"vector flag write", 0xE856D0, {0x34000742, 0xD503237F, 0xD10103FF, 0xA9027BFD}};
// GameData String64 read: (manager, const char** out, name hash) -> found; *out points at the stored text. Used by
// the game the same way, for example where it reads Sequence_CurrentBanc and tests for an empty name.
constexpr Site kGetString64{"text flag read", 0xCB0EA0, {0x34000862, 0xD503237F, 0xD100C3FF, 0xA9017BFD}};
// GameData Bool read: (manager, bool* out, name hash) -> found. Reads pending writes first.
constexpr Site kGetBool{"true/false flag read", 0x20FD90, {0x34000362, 0xD503237F, 0xD100C3FF, 0xA9017BFD}};
// Event request set-up, the request itself and the request's member clean-up, as SeqOneShotRequestEvent uses them.
constexpr Site kEventRequestInit{"event request set-up", 0xD79F10, {0xD503237F, 0xA9BD7BFD, 0xF9000BF5, 0xA9024FF4}};
constexpr Site kEventRequest{"event request", 0xD7A820, {0xB940B808, 0x37580808, 0xD503237F, 0xD10103FF}};
constexpr Site kEventRequestMemberEnd{"event request clean-up", 0x20B6F0,
                                      {0xD503237F, 0xA9BC7BFD, 0xF9000BF7, 0xA90257F6}};

// ---- Data -----------------------------------------------------------------------------------------------------

constexpr ptrdiff_t kGameDataManagerSlot = 0x4C7E180;  // two loads give the GameData manager
constexpr ptrdiff_t kEventManagerSlot = 0x4C7E090;     // two loads give the event manager
constexpr ptrdiff_t kControllerModuleInstance = 0x4E92F10;
constexpr ptrdiff_t kEventRequestMemberVtable = 0x4974B20;  // 0x4974B10 + 0x10, stored before each member clean-up

constexpr size_t kEventRequestSize = 0x1B20;
constexpr uintptr_t kEventRequestNameBuffer = 0x1A90;  // char*
constexpr uintptr_t kEventRequestNameCapacity = 0x1A98;  // int
constexpr uintptr_t kEventRequestMembers[2] = {0x1968, 0x1950};
constexpr uintptr_t kEventManagerPending = 0x68;  // non-zero: the main sequence switches to its Event state
constexpr uintptr_t kEventManagerFlags = 0xB8;    // bit 0x800: requests are refused

// GameData queues (String64 and Vec3): capacity and claimed-count words.
constexpr uintptr_t kString64QueueCapacity = 1584;
constexpr uintptr_t kString64QueueCount = 1600;
constexpr uintptr_t kVec3QueueCapacity = 1008;
constexpr uintptr_t kVec3QueueCount = 1024;

constexpr uintptr_t kMainRodataOffset = 0x03C47000;
constexpr uintptr_t kMainDataOffset = 0x0484D000;

// ---- Helpers --------------------------------------------------------------------------------------------------

inline uintptr_t mainBase() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

template <typename T>
T& field(void* object, uintptr_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(object) + offset);
}

template <typename Fn>
Fn gameFunction(const Site& site) {
    return reinterpret_cast<Fn>(mainBase() + site.offset);
}

// Follows a slot that holds the address of a static instance pointer: two loads, as the game's own code does.
inline void* instanceAt(ptrdiff_t slot) {
    void* const* holder = *reinterpret_cast<void* const* const*>(mainBase() + slot);
    return holder != nullptr ? *holder : nullptr;
}

constexpr bool isPowerOfTwo(u64 value) { return value != 0 && (value & (value - 1)) == 0; }

// Compares two names. Every caller passes one of the module's own short names, so the loop ends within it.
constexpr bool sameText(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (*a != *b) { return false; }
        if (*a == '\0') { return true; }
    }
}
static_assert(sameText("UI 2D", "UI 2D") && !sameText("UI 2D", "UI_2D_Overlay") && !sameText("UI_2D_Overlay", "UI 2D"));

// murmur3 (32-bit, seed 0), the hash the game uses for GameData names.
constexpr u32 murmur3(const char* text) {
    u32 length = 0;
    while (text[length] != '\0') { ++length; }
    u32 h = 0;
    const u32 c1 = 0xCC9E2D51, c2 = 0x1B873593;
    u32 i = 0;
    for (; i + 4 <= length; i += 4) {
        u32 k = static_cast<u8>(text[i]) | static_cast<u8>(text[i + 1]) << 8 | static_cast<u8>(text[i + 2]) << 16 |
                static_cast<u32>(static_cast<u8>(text[i + 3])) << 24;
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xE6546B64;
    }
    u32 k = 0;
    switch (length & 3) {
    case 3: k ^= static_cast<u32>(static_cast<u8>(text[i + 2])) << 16; [[fallthrough]];
    case 2: k ^= static_cast<u32>(static_cast<u8>(text[i + 1])) << 8; [[fallthrough]];
    case 1:
        k ^= static_cast<u8>(text[i]);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
    }
    h ^= length;
    h ^= h >> 16;
    h *= 0x85EBCA6B;
    h ^= h >> 13;
    h *= 0xC2B2AE35;
    h ^= h >> 16;
    return h;
}

static_assert(murmur3("Sequence_ReservedNextBanc") == 0xEE27BF71);
static_assert(murmur3("Sequence_ReservedNextStartPosName") == 0x0B570AB2);
static_assert(murmur3("Sequence_ReservedNextPosition") == 0x52046B85);
static_assert(murmur3("Sequence_ReservedNextCancelSleepRot") == 0x1662143E);
static_assert(murmur3("Sequence_IsChildhood") == 0xCEA220D3);
static_assert(murmur3("Sequence_CurrentBanc") == 0x1D6189DA);
static_assert(murmur3("IsGameOverTrig") == 0x2FD0BC6C);

class SpinLock {
public:
    void lock() {
        while (m_flag.test_and_set(std::memory_order_acquire)) {}
    }
    void unlock() { m_flag.clear(std::memory_order_release); }

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : m_lock(lock) { m_lock.lock(); }
    ~LockGuard() { m_lock.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SpinLock& m_lock;
};

// Thread identity without importing system functions: the per-thread register every Horizon program reads.
inline uintptr_t threadKey() {
    uintptr_t value;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

}  // namespace warp
