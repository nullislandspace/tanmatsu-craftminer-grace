#pragma once
// =====================================================================
//  SynthMiner  --  the crafting book
// ---------------------------------------------------------------------
//  Not a 3x3 grid. The badge has a keyboard and no pointer, so crafting
//  is a SEARCHABLE LIST of recipes the player has discovered (the
//  user's call, 2026-09-23; claudeplans/synthminer.md, Part C):
//
//      * it starts empty and fills as materials are picked up
//      * every letter typed narrows it
//      * the cursor's recipe says what it needs and what is carried
//      * enter makes one
//
//  THE SEARCH BOX EATS EVERY LETTER, so nothing here is on a letter
//  key: the arrows move, enter crafts, backspace edits, esc closes.
//  Every command has to be reachable while typing.
//
//  And the letters it eats are ASCII, because the Tanmatsu has exactly
//  one QWERTY and the game speaks 32 languages -- so a name is matched
//  through i18n/fold.h, which folds "Кирка" and "Kömür" down to
//  something a player can actually type.
// =====================================================================

#include <stdbool.h>

#include "bsp/input.h"
#include "items/inventory.h"
#include "items/recipes.h"  // recipe_station_t: what craft_ui_open takes
#include "pax_gfx.h"

// Open the book at `station` (recipes.h, recipe_station_t). Reopening
// clears the search box: a filter left over from last time reads as an
// empty book.
void craft_ui_open(int station);
void craft_ui_close(void);
bool craft_ui_active(void);

// Every input event while the book is up -- it has the keyboard, all of
// it, the way a menu does.
void craft_ui_event(bsp_input_event_t const* ev);

// Once a frame: act on this frame's keys against `inv`.
void craft_ui_update(inventory_t* inv);

// Draw over `fb`, at full resolution, after everything else.
void craft_ui_draw(pax_buf_t* fb, inventory_t const* inv);
