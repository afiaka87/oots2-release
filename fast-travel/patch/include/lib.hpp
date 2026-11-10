// Stand-ins for the exlaunch names Fast Travel uses, for the patch-file build (FAST_TRAVEL_PATCH=1). The patch has no
// module of its own: build_patch.py places the compiled code in unused space inside the game's main executable, routes
// the declarations below to the game's own import entries and to runtime.cpp, and generates each hook's entry gate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

extern "C" {
u64 svcGetSystemTick();                    // the game's nn::os::GetSystemTick import entry
void* patch_current_thread();              // the game's nn::os::GetCurrentThread import entry
std::uintptr_t patch_main_base();          // start of main, from the patch code's own position in it
void patch_enable_hook(std::ptrdiff_t site);  // opens the entry gate at a hooked routine (runtime.cpp)
}

struct PatchLogger {
    static void Log(const char* format, ...);
};
inline PatchLogger Logging;

namespace exl::hook {
inline void Initialize() {}
}  // namespace exl::hook

namespace patch {

// The patch writes a branch to the hook's gate over the routine's first word; Original runs that word and continues
// in the routine. InstallAtOffset keeps the callback in the build and opens the gate.
template <class Hook, void (*Original)()>
struct Trampoline {
    template <class... Args>
    static auto Orig(Args... args) {
        return reinterpret_cast<decltype(&Hook::Callback)>(Original)(args...);
    }
    static void InstallAtOffset(std::ptrdiff_t site) {
        asm volatile("" ::"r"(&Hook::Callback));
        patch_enable_hook(site);
    }
};

}  // namespace patch

#define HOOK_DEFINE_TRAMPOLINE(name)          \
    extern "C" void patch_original_##name(); \
    struct name : ::patch::Trampoline<name, &patch_original_##name>
