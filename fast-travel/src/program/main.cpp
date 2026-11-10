// Fast Travel for the Ocarina of Time remake (Switch 2): an L3 + R3 menu at the bottom left
// of the screen lists the areas young or adult Link can visit (X switches the list) and, in each, the spots where the
// game itself places him; A warps there. The all-places build lists every area and spot from the start; the visited
// build only those Link has reached (menu.hpp). A warp fills the game's reserved next-area slots and requests the game's
// half-second silent event; when that event ends, the game's own sequence loads the area with its usual fade. A spot
// in the other age's world goes to the Temple of Time first and runs the game's own age change there. The game's
// automatic saves are held mid-way through an age change, and a menu switch turns them off for the session.
// Supported game build: main E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348; every hooked and called routine is checked
// against it before anything is installed.

#include "game.hpp"
#include "menu.hpp"
#include "overlay.hpp"
#include "visits.hpp"
#include "warp.hpp"

namespace warp {
namespace {

constexpr uintptr_t kGameControllerSlots[5] = {0xB0, 0x110, 0x118, 0x120, 0xB8};
constexpr uintptr_t kPausedByte = 0x139;  // bit 0 set while the game is paused

int gameControllerIndex(void* controller) {
    void* module = *reinterpret_cast<void**>(mainBase() + kControllerModuleInstance);
    if (module == nullptr) { return -1; }
    for (int i = 0; i < 5; ++i) {
        if (field<void*>(module, kGameControllerSlots[i]) == controller) { return i; }
    }
    return -1;
}

HOOK_DEFINE_TRAMPOLINE(ControllerCalc) {
    static void Callback(void* controller) {
        if (gameControllerIndex(controller) != 0) {
            Orig(controller);
            return;
        }
        if ((field<u8>(controller, kPausedByte) & 1) != 0) {
            serviceWarpsWhilePaused();
            Orig(controller);
            return;
        }
        serviceWarps();
        Orig(controller);
    }
};

// Every automatic save request. While saves are held it answers "not requested", as the game itself does when a save
// is not allowed; both callers ignore the answer, so no scene waits on it.
HOOK_DEFINE_TRAMPOLINE(AutoSaveRequest) {
    static bool Callback(void* manager, u32 reason, void* actor) {
        const char* held = autosaveHeldBecause();
        if (held != nullptr) {
            WTLOG("autosave (reason %u) skipped: %s", reason, held);
            return false;
        }
        const bool requested = Orig(manager, reason, actor);
        WTLOG("autosave (reason %u) allowed: %s", reason, requested ? "requested" : "the game declined it");
        return requested;
    }
};

using GetNpadStatesFn = int (*)(NpadState*, int, const u32*);

#define WARP_NPAD_HOOK(N)                                                    \
    HOOK_DEFINE_TRAMPOLINE(NpadStates##N) {                                  \
        static int Callback(NpadState* out, int count, const u32* npadId) {  \
            const int result = Orig(out, count, npadId);                     \
            onPadStates(N, out, count, npadId);                              \
            return result;                                                   \
        }                                                                    \
    };

WARP_NPAD_HOOK(0)
WARP_NPAD_HOOK(1)
WARP_NPAD_HOOK(2)
WARP_NPAD_HOOK(3)
WARP_NPAD_HOOK(4)

#undef WARP_NPAD_HOOK

bool layoutMatches() {
#if FAST_TRAVEL_PATCH
    return true;  // the patch file applies only to build E0F3CCFA
#else
    const auto& main = exl::util::GetMainModuleInfo();
    const uintptr_t base = main.m_Total.m_Start;
    if (main.m_Rodata.m_Start - base != kMainRodataOffset || main.m_Data.m_Start - base != kMainDataOffset) {
        WTLOG("main layout differs from build E0F3CCFA (rodata +%#lx, data +%#lx); nothing installed",
              main.m_Rodata.m_Start - base, main.m_Data.m_Start - base);
        return false;
    }
    return true;
#endif
}

// What the start-up line adds about the lists.
#if FAST_TRAVEL_VISITED_ONLY
#define FAST_TRAVEL_LIST_NOTE "; the lists show visited places and the spots reached in them"
#else
#define FAST_TRAVEL_LIST_NOTE ""
#endif

// The patch build's own branch replaces each hooked routine's first word, so it checks the rest.
#if FAST_TRAVEL_PATCH
constexpr int kHookedFirstWord = 1;
#else
constexpr int kHookedFirstWord = 0;
#endif

bool siteMatches(const Site& site, const char* consequence, bool hooked = false) {
    const auto* words = reinterpret_cast<const u32*>(mainBase() + site.offset);
    for (int i = hooked ? kHookedFirstWord : 0; i < 4; ++i) {
        if (words[i] != site.words[i]) {
            WTLOG("%s at main+%#lx: word %d is %08x, expected %08x (another code mod?); %s", site.what,
                  static_cast<unsigned long>(site.offset), i, words[i], site.words[i], consequence);
            return false;
        }
    }
    return true;
}

}  // namespace
}  // namespace warp

extern "C" void exl_main(void* /*x0*/, void* /*x1*/) {
    using namespace warp;
    WTLOG("step 1/3: module started");
    if (!layoutMatches()) { return; }
    // The menu needs the pad hooks (its buttons), the drawing hooks (its box) and the controller update (where warps
    // run). Warps also need every called routine. Each pad form is installed on its own match.
    const bool drawing =
        siteMatches(kLoadGraphicsProcs, "the menu is not drawn", true) &&
        siteMatches(kDrawLayer, "the menu is not drawn", true);
    const bool update = siteMatches(kControllerCalc, "warps cannot run", true);
    bool called = true;
    for (const Site* site : {&kSetString64, &kGetString64, &kSetVec3, &kGetBool, &kEventRequestInit, &kEventRequest,
                             &kEventRequestMemberEnd}) {
        called = siteMatches(*site, "warps cannot run") && called;
    }
    // Holding saves is installed only with warps (the hold follows the warp state), and on its own match.
    const bool saves = siteMatches(kAutoSaveRequest, "automatic saves are not held", true) &&
                       siteMatches(kAutoSaveRequestBody, "automatic saves are not held");
    bool pads[5];
    int padCount = 0;
    for (int i = 0; i < 5; ++i) {
        pads[i] = siteMatches(kNpadStates[i], "that controller type cannot open the menu", true);
        padCount += pads[i] ? 1 : 0;
    }
    const bool warps = update && called;
    // The menu's area pictures are read from the SD card through the file routines; the visited build hooks OpenFile.
    bool pictureFiles = true;
    for (const Site* site : {&kOpenFile, &kGetFileSize, &kReadFile, &kCloseFile}) {
        pictureFiles = siteMatches(*site, "the menu shows no area pictures",
                                   FAST_TRAVEL_VISITED_ONLY && site == &kOpenFile) && pictureFiles;
    }
#if FAST_TRAVEL_VISITED_ONLY
    // The spot record beside each save slot needs the file open hook and every file function it calls. Without them
    // the spots reached still show for the session, but are not kept with the save.
    bool record = warps;
    for (const Site* site : {&kOpenFile, &kGetFileSize, &kCloseFile, &kCreateFile, &kWriteFile, &kSetFileSize,
                             &kReadFile}) {
        record = siteMatches(*site, "the spots reached are not kept with the save", site == &kOpenFile) && record;
    }
#endif
    if (padCount == 0) {
        WTLOG("no controller read site matched; nothing installed");
        return;
    }
    WTLOG("step 2/3: sites checked: menu drawing %s, warps %s, autosave hold %s, controller types %d of 5",
          drawing ? "yes" : "no", warps ? "yes" : "no", warps && saves ? "yes" : "no", padCount);
    exl::hook::Initialize();
    if (drawing) { installOverlayHooks(pictureFiles); }
    if (warps) { ControllerCalc::InstallAtOffset(kControllerCalc.offset); }
    if (warps && saves) { AutoSaveRequest::InstallAtOffset(kAutoSaveRequest.offset); }
#if FAST_TRAVEL_VISITED_ONLY
    if (record) { installSaveRecordHook(); }
    WTLOG("spot record beside each save slot: %s", record ? "on" : "off (spots reached show for this session only)");
#endif
    if (pads[0]) { NpadStates0::InstallAtOffset(kNpadStates[0].offset); }
    if (pads[1]) { NpadStates1::InstallAtOffset(kNpadStates[1].offset); }
    if (pads[2]) { NpadStates2::InstallAtOffset(kNpadStates[2].offset); }
    if (pads[3]) { NpadStates3::InstallAtOffset(kNpadStates[3].offset); }
    if (pads[4]) { NpadStates4::InstallAtOffset(kNpadStates[4].offset); }
    WTLOG("step 3/3: hooks installed (menu drawing %s, warps %s, autosave hold %s, controller types %d of 5); "
          "L3 + R3 opens the menu" FAST_TRAVEL_LIST_NOTE,
          drawing ? "on" : "off", warps ? "on" : "off", warps && saves ? "on" : "off", padCount);
}

#if !FAST_TRAVEL_PATCH
extern "C" NORETURN void exl_exception_entry() { EXL_ABORT("unreachable"); }
#endif
