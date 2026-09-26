#pragma once
// =====================================================================
//  SynthMiner  --  where the player's data lives
// ---------------------------------------------------------------------
//  Everything the game WRITES -- the worlds, settings.txt, replays,
//  screenshots -- lives in /sd/synthminer, outside the app's install
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
//  THE GAME WAS CALLED CRAFTMINER (D-91), and its name was on the card
//  twice over: this directory was /sd/craftminer, and every file it
//  saved ended in .cmw or .cmr. Both are handled here, and both are
//  handled the same way -- by moving what is there, not by asking the
//  player to. The extensions are datadir_rename_saves(); the
//  directories are two more old bases to adopt from:
//
//      /sd/craftminer                 where any recent CraftMiner kept it
//      /sd/apps/at.cavac.craftminer   where one from before D-80 did
//
//  IN THAT ORDER, so a card with both keeps the newer layout: no
//  adoption overwrites, so whichever arrives first holds the place.
//
//  SynthMiner does NOT adopt from its own install directory. That
//  directory has never held a player's anything -- this game has
//  shipped under this name with the data already split out -- so there
//  is nothing there to find. It is the OLD install directory that has
//  to be emptied, and it has to be emptied before datadir_retire()
//  deletes it.
//
//  Pure (host-tested by worldcheck).
// =====================================================================

#include <stdbool.h>
#include <stddef.h>

#define SM_DATA_DIR "/sd/synthminer"

// Where CraftMiner kept it, and before that where it installed to.
// Both are adopted from on start and then deleted, because a player's
// worlds do not belong to a name and their card should not keep two
// generations of one game.
#define SM_DATA_DIR_WAS    "/sd/craftminer"
#define SM_INSTALL_WAS     "/sd/apps/at.cavac.craftminer"

// WHICH ENTRIES an old base is expected to hold. The two differ by more
// than length, which is why this is not one list: an INSTALL directory
// also holds what the app SHIPPED -- its textures and its eleven pieces
// of music -- and those are not the player's and must not be adopted.
// Moving a shipped `music/` into the data directory would put all
// eleven into the pool a second time, as the player's own (audio/music.c
// reads both).
typedef enum {
    DD_INSTALL = 0,  // an old install directory: only what a pre-D-80 build wrote there
    DD_DATA,         // an old data directory: everything one holds
} datadir_set_t;

// Move the player's data from `old_base` into `new_base`, creating it.
// Returns how many entries were moved; -1 if `new_base` could not be
// created. An entry present in both is left where it is (and logged by
// the caller from `report`), since merging two worlds directories is
// not something to do behind anyone's back.
// `report`, if not NULL, gets one line per entry moved or left.
int datadir_adopt(char const* old_base, char const* new_base, datadir_set_t set, char* report, size_t report_len);

// Give every saved file its new extension: `level.cmw` becomes
// `level.smw` and `r.<rx>.<rz>.cmr` becomes `.smr`, in every world
// under `base`, in the benchmark world beside them, and in the
// replays. Returns how many files were renamed, -1 if `base` is no
// good. `report`, if not NULL, gets a line per world that changed.
//
// It runs on EVERY start rather than once, because the alternative is
// a flag that can disagree with the card. A rename that did not finish
// -- the badge switched off mid-way, a file that would not move --
// finishes on the next start, and a start with nothing left to do
// costs one scan of each world directory and no writes at all.
//
// The readers accept both spellings of the magic inside these files
// (worldstore.h, region.h), so a world whose rename has not happened
// yet, or could not, still loads. This only stops the card carrying
// two generations of names for ever.
int datadir_rename_saves(char const* base, char* report, size_t report_len);

// Is `path` there at all? Two of these decide whether the player is
// TOLD a migration is happening: a start with nothing to migrate must
// not pay a second of splash for it (D-94).
bool datadir_exists(char const* path);

// --- Getting rid of the old one ---------------------------------------
//
// Delete `dir` and everything under it: the card should not keep two
// generations of one game (D-92). Called for both old bases after they
// have been adopted from, so what it deletes is an empty directory or
// an app's own shipped files.
//
// THIS IS THE ONE THING HERE THAT DESTROYS ANYTHING, so every doubt is
// a refusal rather than a judgement call. It refuses, and says why, if:
//
//   - `dir` is empty, "/", or has a ".." in it
//   - `dir` IS `keep`, or is a parent of it -- which is what stops a
//     mistake in a constant from taking the live directory, or /sd/apps
//   - anything in `set` is STILL INSIDE `dir`. This is the real guard:
//     adoption deliberately LEAVES an entry whose destination already
//     exists, and that entry is somebody's world. If adoption left
//     something, this must not run.
//
// Returns how many files and directories went, or -1 if it refused.
// `report` gets a line either way.
int datadir_retire(char const* dir, char const* keep, datadir_set_t set, char* report, size_t report_len);
