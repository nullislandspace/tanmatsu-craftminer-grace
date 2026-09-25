#pragma once
// =====================================================================
//  SynthMiner  --  the furnace screen
// ---------------------------------------------------------------------
//  Three rows -- Input, Fuel, Output -- and the user's design:
//
//    * enter on OUTPUT takes what is there into the inventory
//    * enter on INPUT or FUEL opens a picker over what you are carrying
//
//  So nothing is ever dragged. With arrow keys and no pointer, a picker
//  is not a poor substitute for dragging a stack between two grids; it
//  is straightforwardly better, and it can say what each stack would
//  BECOME, which no amount of dragging tells you.
//
//  The picker shows only what fits the slot -- what can be smelted, or
//  what burns -- because a list with everything in it and no way to
//  tell which entries do anything is a list that answers no question.
// =====================================================================

#include <stdbool.h>

#include "bsp/input.h"
#include "items/inventory.h"
#include "pax_gfx.h"
#include "world/blockent.h"

// Open the furnace at (x, y, z). False if there is no furnace there,
// which the caller should treat as "nothing happened".
bool furnace_ui_open(int32_t x, int32_t y, int32_t z);
void furnace_ui_close(void);
bool furnace_ui_active(void);

// Every input event while it is up: it has the keyboard, like a menu.
void furnace_ui_event(bsp_input_event_t const* ev);

// Once a frame. `now` is the world clock, which is what the furnace
// runs on (game/furnace.h -- it never ticks; it catches up).
void furnace_ui_update(inventory_t* inv, uint32_t now);

void furnace_ui_draw(pax_buf_t* fb, inventory_t const* inv);
