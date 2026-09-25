#pragma once
// =====================================================================
//  SynthMiner  --  the cheat console
// ---------------------------------------------------------------------
//  Type part of an item's name, pick it, get a stack of it. Opened with
//  the backtick key, which is where a console goes and which nothing
//  else uses.
//
//  ENGLISH ONLY, on purpose (the user's call, 2026-09-23): it lists
//  items by their STABLE NAME -- "pickaxe_wood", "iron_ore" -- which is
//  the id the save format keys on and is never translated. That makes
//  it unambiguous, it makes it searchable with the plain letters on the
//  badge's keyboard, and it means the console needs no strings.
//
//  It is a cheat, so it is honest about being one: it says so at the
//  top, and everything it gives is marked as held, which is what the
//  crafting book's discovery rule reads (items/recipes.h).
// =====================================================================

#include <stdbool.h>

#include "bsp/input.h"
#include "items/inventory.h"
#include "pax_gfx.h"

void cheat_ui_open(void);
void cheat_ui_close(void);
bool cheat_ui_active(void);

void cheat_ui_event(bsp_input_event_t const* ev);
void cheat_ui_update(inventory_t* inv);
void cheat_ui_draw(pax_buf_t* fb);
