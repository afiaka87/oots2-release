// The Fast Travel menu: its state and the controller handling that drives it. L3 opens and closes it. It shows one
// age's list at a time (X switches to the other age's list; the menu opens on Link's current age). Pages:
// - the list: "Autosave" (on or off for this session), then the areas. A switches autosave or opens the area, B
//   closes the menu;
// - an area's spots: A warps, B goes back.
// D-pad up and down move the cursor, left and right move a page at a time. A spot in the other age's list changes
// Link's age first. A, B, X, Y and the D-pad are hidden from the game while the menu is open (and until each is
// released after it closes); L3 is always hidden from the game.
#pragma once

#include "destinations.hpp"

namespace warp {

// What the menu's status line says. Each names what the player sees or can do, not the internal reason.
enum class Status : u8 {
    Ready,
    ToTemple,
    ChangingAge,
    Warping,
    SceneRunning,
    AgeUnknown,
    StillLoading,
    GameBusy,
    Refused,
    Paused,
    DidNotStart,
    DidNotFinish,
    AgeDidNotChange,
    AlreadyWarping,
    AutosaveOn,
    AutosaveOff,
};

const char* statusText(Status status);
// True while a warp (or one step of an age change) is under way: the status stays until it finishes.
constexpr bool inProgress(Status status) {
    return status == Status::ToTemple || status == Status::ChangingAge || status == Status::Warping;
}

enum class Page : u8 { Areas, Spots };

constexpr int kVisibleRows = 9;  // list rows shown at once; the list scrolls

// Link's age as the game last reported it to the warp code.
enum class KnownAge : u8 { Unknown, Young, Adult };

// Rows before the areas in each age's list: "Autosave" (row 0).
constexpr int kTopRows = 1;

// Index of an age in per-age arrays: young 0, adult 1.
constexpr int ageIndex(Age age) { return age == Age::Young ? 0 : 1; }

// A copy of the menu state for drawing.
struct MenuView {
    bool open = false;
    Page page = Page::Areas;
    Age list = Age::Young;  // the age whose list is shown
    KnownAge link = KnownAge::Unknown;
    int area = 0;      // the area under the cursor (index into kAreas), or the list's first area on "Autosave"; also
                       // the area whose spots the spots page shows
    int areaRow = kTopRows;  // the list cursor (0 = "Autosave"); it starts on the first area
    int areaTop = 0;   // first list row shown
    int spot = 0;      // spot cursor within the area (0 = its first spot)
    int spotTop = 0;   // first spot row shown
    bool autosave = true;  // the game's automatic saves, for this session
    Status status = Status::Ready;
    u32 version = 0;  // changes whenever anything shown changes
};

MenuView menuView();

// A warp chosen in the menu, consumed by the controller update. None pending while area is -1.
struct WarpRequest {
    int area = -1;  // index into kAreas
    int spot = -1;  // index into kSpots
};

bool warpRequested();
WarpRequest takeWarpRequest();
void reportWarp(Status status, bool closeMenu);
// Called by the controller update with Link's age each time it reads it.
void reportLinkAge(KnownAge age);
// The menu's autosave switch (on unless the player turned it off this session).
bool autosaveOn();

// One nn::hid Npad state (NpadFullKeyState and the other forms share it).
struct NpadState {
    s64 samplingNumber;
    u64 buttons;
    s32 sticks[4];
    u32 attributes;
    u32 reserved;
};
static_assert(sizeof(NpadState) == 0x28, "the game asks for 16 states in a 0x280-byte buffer");

// Called after each GetNpadStates call returns, before the game reads the states.
void onPadStates(int form, NpadState* states, int count, const u32* npadId);

}  // namespace warp
