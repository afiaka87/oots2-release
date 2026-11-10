#include "menu.hpp"

namespace warp {
namespace {

constexpr u64 kButtonA = 1ull << 0;
constexpr u64 kButtonB = 1ull << 1;
constexpr u64 kButtonX = 1ull << 2;
constexpr u64 kButtonY = 1ull << 3;
constexpr u64 kButtonL3 = 1ull << 4;  // the left stick pressed in
constexpr u64 kButtonR3 = 1ull << 5;  // the right stick pressed in
constexpr u64 kButtonLeft = 1ull << 12;
constexpr u64 kButtonUp = 1ull << 13;
constexpr u64 kButtonRight = 1ull << 14;
constexpr u64 kButtonDown = 1ull << 15;
constexpr u64 kMenuChord = kButtonL3 | kButtonR3;  // both sticks pressed in: opens and closes the menu
constexpr u64 kMenuButtons =
    kButtonA | kButtonB | kButtonX | kButtonY | kButtonUp | kButtonDown | kButtonLeft | kButtonRight | kMenuChord;
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

int listRows(Age age) { return kTopRows + shownAreas(s_menu, age); }

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
    s_menu.area = shownArea(s_menu, ageFirst(s_menu.list), row > 0 ? row : 0);
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

#if FAST_TRAVEL_VISITED_ONLY
// The first row shown, so that the cursor row is visible and the list does not scroll past its end.
int scrolledTop(int cursor, int top, int count);

bool listOpen(const MenuView& view, Age age) { return age == Age::Young || view.visits.adult; }
bool spotShown(const MenuView& view, int spot) {
    return ((view.visits.places >> kSpotPlace[spot]) & 1) != 0 && hasSpot(view.visits.spots, spot);
}

// The list row of an area (-1 when the list does not show it).
int rowOfArea(Age age, int area) {
    const int count = shownAreas(s_menu, age);
    for (int n = 0; n < count; ++n) {
        if (shownArea(s_menu, ageFirst(age), n) == area) { return kTopRows + n; }
    }
    return -1;
}

// After the shown places change: each list's cursor stays on its area while that is still shown (otherwise on the
// nearest row; an empty list's cursor goes to its first area once it has one), and so does the spot cursor; the
// spots page goes back to the list when its area is no longer shown.
void fitCursors(const MenuView before) {
    for (const Age age : {Age::Young, Age::Adult}) {
        const int i = ageIndex(age);
        const int rowsBefore = kTopRows + shownAreas(before, age);
        const int rows = listRows(age);
        int row = s_areaRow[i];
        if (rowsBefore == kTopRows) {
            row = rows > kTopRows ? kTopRows : 0;
        } else if (row >= kTopRows && row < rowsBefore) {
            const int moved = rowOfArea(age, shownArea(before, ageFirst(age), row - kTopRows));
            if (moved >= 0) { row = moved; }
        }
        if (row >= rows) { row = rows - 1; }
        s_areaRow[i] = row;
        s_areaTop[i] = scrolledTop(row, s_areaTop[i], rows);
    }
    syncAreaCursor();
    if (s_spotsArea >= 0) {
        const Area& area = kAreas[s_spotsArea];
        const int count = shownSpots(s_menu, area);
        int spot = s_menu.spot;
        if (spot < shownSpots(before, area)) {
            const int was = shownSpot(before, area, spot);
            for (int n = 0; n < count; ++n) {
                if (shownSpot(s_menu, area, n) == was) { spot = n; }
            }
        }
        if (spot >= count) { spot = count - 1; }
        if (spot < 0) {
            s_spotsArea = -1;
            s_menu.spot = 0;
            s_menu.spotTop = 0;
        } else {
            s_menu.spot = spot;
            s_menu.spotTop = scrolledTop(spot, s_menu.spotTop, count);
        }
    }
    if (s_menu.page == Page::Spots && s_spotsArea != s_menu.area) { s_menu.page = Page::Areas; }
}
#endif

void closeMenu(u64 held) {
    if (!s_menu.open) { return; }
    s_menu.open = false;
    s_hideUntilReleased |= held & kMenuButtons;
    changed();
}

// L3 + R3: closes the menu, or opens it on Link's own age's list (or the last list shown while his age is not known).
void toggleMenu(u64 held) {
    if (s_menu.open) {
        closeMenu(held);
        WTLOG("menu closed (L3 + R3)");
        return;
    }
    if (s_menu.link != KnownAge::Unknown) { showList(s_menu.link == KnownAge::Young ? Age::Young : Age::Adult); }
#if FAST_TRAVEL_VISITED_ONLY
    fitCursors(s_menu);
#endif
    s_menu.open = true;
    changed();
    WTLOG("menu opened (L3 + R3): %s list, %s page, row %d, spot %d, status '%s'", ageName(s_menu.list),
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
    const int count = spots ? shownSpots(s_menu, kAreas[s_menu.area]) : listRows(s_menu.list);
#if FAST_TRAVEL_VISITED_ONLY
    if (count <= 0) { return; }
#endif
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
    s_pending = {s_menu.area, shownSpot(s_menu, area, s_menu.spot)};
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

// True on the update where the second stick goes in while the first is still held (either order, or both at once).
bool chordPressed(u64 pressed, u64 held) { return (pressed & kMenuChord) != 0 && (held & kMenuChord) == kMenuChord; }

void handlePresses(u64 pressed, u64 held) {
    if (chordPressed(pressed, held)) {
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

#if FAST_TRAVEL_VISITED_ONLY
int shownSpots(const MenuView& view, const Area& area) {
    if (!listOpen(view, area.age)) { return 0; }
    int n = 0;
    for (int spot = area.first; spot < area.first + area.count; ++spot) { n += spotShown(view, spot) ? 1 : 0; }
    return n;
}

int shownSpot(const MenuView& view, const Area& area, int n) {
    for (int spot = area.first; spot < area.first + area.count; ++spot) {
        if (spotShown(view, spot) && n-- == 0) { return spot; }
    }
    return area.first;
}

int shownAreas(const MenuView& view, Age age) {
    const int first = ageFirst(age);
    int n = 0;
    for (int area = first; area < first + ageCount(age); ++area) { n += shownSpots(view, kAreas[area]) > 0 ? 1 : 0; }
    return n;
}

int shownArea(const MenuView& view, int listFirst, int n) {
    const int end = listFirst + ageCount(kAreas[listFirst].age);
    for (int area = listFirst; area < end; ++area) {
        if (shownSpots(view, kAreas[area]) > 0 && n-- == 0) { return area; }
    }
    return listFirst;
}

void reportVisits(const Visits& visits) {
    LockGuard guard(s_lock);
    if (sameVisits(s_menu.visits, visits)) { return; }
    const MenuView before = s_menu;
    s_menu.visits = visits;
    fitCursors(before);
    changed();
}
#endif

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
        if (!s_firstPressLogged && chordPressed(pressed, held)) {
            s_firstPressLogged = true;
            WTLOG("first L3 + R3 press seen: form %d, pad id %#x, buttons %#llx", form, npadId != nullptr ? *npadId : 0u,
                  static_cast<unsigned long long>(held));
        }
        handlePresses(pressed, held);
    }
    s_hideUntilReleased &= held;
    // L3 and R3 on their own reach the game while the menu is closed; the chord that closes it is hidden until released.
    const u64 hide = s_hideUntilReleased | (s_menu.open ? kMenuButtons : 0);
    for (int i = 0; i < count; ++i) { states[i].buttons &= ~hide; }
}

}  // namespace warp
