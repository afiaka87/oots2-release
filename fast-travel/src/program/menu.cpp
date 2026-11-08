#include "menu.hpp"

namespace warp {
namespace {

constexpr u64 kButtonA = 1ull << 0;
constexpr u64 kButtonB = 1ull << 1;
constexpr u64 kButtonX = 1ull << 2;
constexpr u64 kButtonY = 1ull << 3;
constexpr u64 kButtonL3 = 1ull << 4;  // the left stick pressed in
constexpr u64 kButtonLeft = 1ull << 12;
constexpr u64 kButtonUp = 1ull << 13;
constexpr u64 kButtonRight = 1ull << 14;
constexpr u64 kButtonDown = 1ull << 15;
constexpr u64 kMenuButtons =
    kButtonA | kButtonB | kButtonX | kButtonY | kButtonUp | kButtonDown | kButtonLeft | kButtonRight;
constexpr u32 kAttributeConnected = 1u << 0;

constexpr int kForms = 5;
constexpr int kPads = 10;  // Npad ids 0-7, handheld (0x20) and other (0x10)

SpinLock s_lock;
MenuView s_menu;
int s_spotsArea = -1;  // the area whose spot cursor s_menu.spot is
// Each age's list cursor and first row shown, kept while the other age's list is shown.
int s_areaRow[2] = {kTopRows, kTopRows};
int s_areaTop[2] = {};
WarpRequest s_pending;
u64 s_held[kForms][kPads] = {};
u64 s_heldBefore = 0;
u64 s_hideUntilReleased = 0;
bool s_firstPressLogged = false;

int padSlot(const u32* npadId) {
    if (npadId == nullptr) { return -1; }
    const u32 id = *npadId;
    if (id < 8) { return static_cast<int>(id); }
    if (id == 0x20) { return 8; }
    if (id == 0x10) { return 9; }
    return -1;
}

void changed() { ++s_menu.version; }

int listRows(Age age) { return kTopRows + ageCount(age); }

// A cursor move or page change clears an old message, but a warp's status stays until the warp has finished.
void clearStatus() {
    if (!inProgress(s_menu.status)) { s_menu.status = Status::Ready; }
}

// Copies the shown list's cursor into the view.
void syncAreaCursor() {
    const int i = ageIndex(s_menu.list);
    s_menu.areaRow = s_areaRow[i];
    s_menu.areaTop = s_areaTop[i];
    const int row = s_areaRow[i] - kTopRows;
    s_menu.area = ageFirst(s_menu.list) + (row > 0 ? row : 0);
}

// Shows an age's list (its list page, with that list's own cursor).
void showList(Age age) {
    if (s_menu.list == age) { return; }
    s_menu.list = age;
    s_menu.page = Page::Areas;
    syncAreaCursor();
    clearStatus();
    changed();
}

void closeMenu(u64 held) {
    if (!s_menu.open) { return; }
    s_menu.open = false;
    s_hideUntilReleased |= held & kMenuButtons;
    changed();
}

// L3: closes the menu, or opens it on Link's own age's list (or the last list shown while his age is not known).
void toggleMenu(u64 held) {
    if (s_menu.open) {
        closeMenu(held);
        WTLOG("menu closed (L3)");
        return;
    }
    if (s_menu.link != KnownAge::Unknown) { showList(s_menu.link == KnownAge::Young ? Age::Young : Age::Adult); }
    s_menu.open = true;
    changed();
    WTLOG("menu opened (L3): %s list, %s page, row %d, spot %d, status '%s'", ageName(s_menu.list),
          s_menu.page == Page::Spots ? "spots" : "list", s_menu.areaRow + 1, s_menu.spot + 1,
          statusText(s_menu.status));
}

// The first row shown, so that the cursor row is visible and the list does not scroll past its end.
int scrolledTop(int cursor, int top, int count) {
    if (cursor < top) { top = cursor; }
    if (cursor >= top + kVisibleRows) { top = cursor - kVisibleRows + 1; }
    const int lastTop = count > kVisibleRows ? count - kVisibleRows : 0;
    if (top > lastTop) { top = lastTop; }
    return top < 0 ? 0 : top;
}

// Up and down wrap around; a page (left and right) stops at either end.
void moveCursor(int delta, bool wrap) {
    const bool spots = s_menu.page == Page::Spots;
    const int list = ageIndex(s_menu.list);
    int& cursor = spots ? s_menu.spot : s_areaRow[list];
    int& top = spots ? s_menu.spotTop : s_areaTop[list];
    const int count = spots ? kAreas[s_menu.area].count : listRows(s_menu.list);
    int next = cursor + delta;
    if (wrap) {
        next = ((next % count) + count) % count;
    } else {
        next = next < 0 ? 0 : (next >= count ? count - 1 : next);
    }
    if (next == cursor) { return; }
    cursor = next;
    top = scrolledTop(cursor, top, count);
    syncAreaCursor();
    clearStatus();
    changed();
}

void openArea() {
    if (s_spotsArea != s_menu.area) {
        s_spotsArea = s_menu.area;
        s_menu.spot = 0;
        s_menu.spotTop = 0;
    }
    s_menu.page = Page::Spots;
    clearStatus();
    changed();
}

bool busy() { return s_pending.area >= 0 || inProgress(s_menu.status); }

void chooseSpot() {
    if (busy()) { return; }
    const Area& area = kAreas[s_menu.area];
    s_pending = {s_menu.area, area.first + s_menu.spot};
    s_menu.status = Status::Warping;
    changed();
    WTLOG("warp chosen: %s (%s list), spot %d '%s'", area.title, ageName(area.age), s_menu.spot + 1,
          kSpots[s_pending.spot].label);
}

void toggleAutosave() {
    s_menu.autosave = !s_menu.autosave;
    s_menu.status = s_menu.autosave ? Status::AutosaveOn : Status::AutosaveOff;
    changed();
    WTLOG("autosave switched %s (this session)", s_menu.autosave ? "on" : "off");
}

void handlePresses(u64 pressed, u64 held) {
    if ((pressed & kButtonL3) != 0) {
        toggleMenu(held);
        return;
    }
    if (!s_menu.open) { return; }
    if ((pressed & kButtonX) != 0) {
        showList(otherAge(s_menu.list));
        WTLOG("list switched (X): %s", ageName(s_menu.list));
        return;
    }
    if ((pressed & kButtonUp) != 0) { moveCursor(-1, true); }
    if ((pressed & kButtonDown) != 0) { moveCursor(1, true); }
    if ((pressed & kButtonLeft) != 0) { moveCursor(-kVisibleRows, false); }
    if ((pressed & kButtonRight) != 0) { moveCursor(kVisibleRows, false); }
    if ((pressed & kButtonA) != 0) {
        if (s_menu.page == Page::Spots) {
            chooseSpot();
        } else if (s_menu.areaRow < kTopRows) {
            toggleAutosave();
        } else {
            openArea();
        }
    } else if ((pressed & kButtonB) != 0) {
        if (s_menu.page == Page::Spots) {
            s_menu.page = Page::Areas;
            clearStatus();
            changed();
        } else {
            closeMenu(held);
            WTLOG("menu closed (B)");
        }
    }
}

}  // namespace

const char* statusText(Status status) {
    switch (status) {
    case Status::Ready: return "Ready";
    case Status::ToTemple: return "Going to the Temple of Time...";
    case Status::ChangingAge: return "Changing age...";
    case Status::Warping: return "Warping...";
    case Status::SceneRunning: return "Not now: a scene is playing";
    case Status::AgeUnknown: return "Not now: age unknown";
    case Status::StillLoading: return "Not now: still loading";
    case Status::GameBusy: return "Not now: game is busy";
    case Status::Refused: return "The game refused the warp";
    case Status::Paused: return "Not now: game is paused";
    case Status::DidNotStart: return "Warp did not start";
    case Status::DidNotFinish: return "Warp did not finish";
    case Status::AgeDidNotChange: return "Age change did not happen";
    case Status::AlreadyWarping: return "A warp is already running";
    case Status::AutosaveOn: return "Autosave is on";
    case Status::AutosaveOff: return "Autosave off until the game closes";
    }
    return "";
}

MenuView menuView() {
    LockGuard guard(s_lock);
    return s_menu;
}

bool warpRequested() {
    LockGuard guard(s_lock);
    return s_pending.area >= 0;
}

WarpRequest takeWarpRequest() {
    LockGuard guard(s_lock);
    const WarpRequest request = s_pending;
    s_pending = {};
    return request;
}

void reportWarp(Status status, bool close) {
    LockGuard guard(s_lock);
    if (s_menu.status != status) {
        s_menu.status = status;
        changed();
    }
    if (close) { closeMenu(s_heldBefore); }
}

void reportLinkAge(KnownAge age) {
    LockGuard guard(s_lock);
    if (s_menu.link == age) { return; }
    s_menu.link = age;
    changed();
}

bool autosaveOn() {
    LockGuard guard(s_lock);
    return s_menu.autosave;
}

void onPadStates(int form, NpadState* states, int count, const u32* npadId) {
    if (states == nullptr || count <= 0 || form < 0 || form >= kForms) { return; }
    const int slot = padSlot(npadId);
    LockGuard guard(s_lock);
    // Buttons held on every connected pad, newest state of each. One physical pad can be reported through more than
    // one form; combining them means a press is seen once.
    if (slot >= 0) {
        s_held[form][slot] = (states[0].attributes & kAttributeConnected) != 0 ? states[0].buttons : 0;
    }
    u64 held = 0;
    for (const auto& pads : s_held) {
        for (const u64 buttons : pads) { held |= buttons; }
    }
    const u64 pressed = held & ~s_heldBefore;
    s_heldBefore = held;
    if (pressed != 0) {
        if (!s_firstPressLogged && (pressed & kButtonL3) != 0) {
            s_firstPressLogged = true;
            WTLOG("first L3 press seen: form %d, pad id %#x, buttons %#llx", form, npadId != nullptr ? *npadId : 0u,
                  static_cast<unsigned long long>(held));
        }
        handlePresses(pressed, held);
    }
    s_hideUntilReleased &= held;
    const u64 hide = kButtonL3 | s_hideUntilReleased | (s_menu.open ? kMenuButtons : 0);
    for (int i = 0; i < count; ++i) { states[i].buttons &= ~hide; }
}

}  // namespace warp
