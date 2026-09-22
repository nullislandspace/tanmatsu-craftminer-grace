#pragma once
// =====================================================================
//  CraftMiner  --  where the player's data lives
// ---------------------------------------------------------------------
//  Everything the game WRITES -- the worlds, settings.txt, replays,
//  screenshots -- lives in /sd/craftminer, outside the app's install
//  directory. The launcher owns /sd/apps/<slug>: an update or a
//  reinstall may empty it, and a player's worlds must not go with it.
//  The install directory keeps only what the app ships with (app.so,
//  the textures).
//
//  Builds before this kept the data in the install directory. On start,
//  datadir_adopt() MOVES it across -- a rename, so nothing is copied and
//  nothing can be half-copied -- one entry at a time, and only an entry
//  the new place does not have yet: it never overwrites. Run again, it
//  finds nothing to do.
//
//  Pure (host-tested by worldcheck).
// =====================================================================

#include <stdbool.h>
#include <stddef.h>

#define CM_DATA_DIR "/sd/craftminer"

// Move the player's data from `old_base` (the install directory) into
// `new_base`, creating it. Returns how many entries were moved; -1 if
// `new_base` could not be created. An entry present in both is left
// where it is (and logged by the caller from `report`), since merging
// two worlds directories is not something to do behind anyone's back.
// `report`, if not NULL, gets one line per entry moved or left.
int datadir_adopt(char const* old_base, char const* new_base, char* report, size_t report_len);
