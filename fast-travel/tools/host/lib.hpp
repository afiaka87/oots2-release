// Host stand-in for exlaunch's lib.hpp, so the menu painter can be built on a PC by tools/preview.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <atomic>
#include <cstring>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
namespace exl::util {
struct Range { uintptr_t m_Start, m_End; };
struct ModuleInfo { Range m_Total, m_Text, m_Rodata, m_Data; };
inline const ModuleInfo& GetMainModuleInfo() { static ModuleInfo m{}; return m; }
}  // namespace exl::util
struct HostLogger { template <class... A> void Log(const char*, A...) {} };
inline HostLogger Logging;
