// The visited-places build: reads the save's record of the places Link has visited, follows the spots he arrives at
// or goes through, and keeps those spots in a small file beside each save slot's progress.sav, for the menu's lists.
#pragma once

#include "menu.hpp"

namespace warp {

#if FAST_TRAVEL_VISITED_ONLY
// Runs in the controller update (not paused), after Link's age is read. Follows the game's trips between areas on
// every update (warping: a Fast Travel warp is under way); reads the flags and Link's position about once a second and
// gives the result to the menu.
void noteVisits(void* manager, KnownAge age, bool warping);
// Hooks the game's file open, so that the spot record is written beside each save slot when the game saves it and read
// when the game reads the slot. Call only after every save-file site matched; installs nothing else.
void installSaveRecordHook();
#endif

}  // namespace warp
