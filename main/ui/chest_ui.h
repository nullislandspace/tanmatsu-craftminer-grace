#pragma once
// =====================================================================
//  SynthMiner  --  chests, and the trashcan
// ---------------------------------------------------------------------
//  TWO GRIDS SIDE BY SIDE: the container and what you are carrying.
//  Tab swaps which one the arrow keys belong to, enter moves the stack
//  under the cursor across. That is the whole interface, and with no
//  pointer it is better than dragging would be -- one key per stack,
//  and nothing can be dropped between the two.
//
//  The trashcan is the SAME SCREEN. It is a chest whose contents rot:
//  anything still in it after CHEST_TRASH_TICKS of playing is gone,
//  worked out when it is opened rather than on a timer (the user's
//  rule, and the same lazy clock the furnace runs on).
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "bsp/input.h"
#include "items/inventory.h"
#include "pax_gfx.h"

// A chest holds exactly what the player does -- 24, in the same 6 x 4
// grid. Minecraft's is 27, which does not divide into six rows of
// anything; matching the pack means the two halves of the screen are
// the same shape, and "as much as you can carry" is a rule a player
// can hold in their head.
#define CHEST_COLS  6
#define CHEST_ROWS  4
#define CHEST_SLOTS (CHEST_COLS * CHEST_ROWS)

// SMALLER SLOTS THAN THE TAB SCREEN, because there are two grids here
// and not one: at the Tab screen's 60 px a pair of them is 808 px on an
// 800 px display, which is a screen with its right-hand column off the
// edge. 44 leaves a margin either side.
#define CHEST_SLOT_W 44

// Open the chest or trashcan at (x, y, z). False if there is none.
bool chest_ui_open(int32_t x, int32_t y, int32_t z);
void chest_ui_close(void);
bool chest_ui_active(void);

void chest_ui_event(bsp_input_event_t const* ev);

// Once a frame. `now` is the world clock, which is what the trashcan
// empties by.
void chest_ui_update(inventory_t* inv, uint32_t now);
void chest_ui_draw(pax_buf_t* fb, inventory_t const* inv);
