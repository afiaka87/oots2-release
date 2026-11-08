// Carrying out a warp the way the game's own warps do: fill the reserved next-area GameData slots, then request a
// short silent event. When any event ends, the game's sequence loads the reserved area at the reserved spawn point.
#pragma once

#include "game.hpp"

namespace warp {

// Runs in the game controller update for the player's controller (game controller 0) while it is not paused.
void serviceWarps();
// Runs in the same place while that controller is paused (the game is paused).
void serviceWarpsWhilePaused();
// Why the game's automatic saves are held right now, or nullptr when they are allowed. Safe on any thread.
const char* autosaveHeldBecause();

}  // namespace warp
