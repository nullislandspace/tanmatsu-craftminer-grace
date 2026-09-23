#pragma once
// =====================================================================
//  CraftMiner  --  the disassembly bench
// ---------------------------------------------------------------------
//  A list of what you are carrying that comes apart, and enter takes
//  one of it apart. Nothing else: the bench has no slots and keeps
//  nothing between uses, so it needs no record (world/blockent.h).
//
//  WHAT COMES APART is a bit in the recipe table (RF_REVERSIBLE), not a
//  rule here -- the user's "doesn't work for basic resources like iron
//  ingots turning into ore or sticks turning into planks" is a fact
//  about each recipe, so it is stored with each recipe.
//
//  A worn tool gives back its FULL ingredient list (the user's call,
//  asked explicitly). Salvaging and recrafting is therefore a repair
//  that costs the one coal in the bench's own recipe, and that is the
//  intended price.
// =====================================================================

#include <stdbool.h>

#include "bsp/input.h"
#include "items/inventory.h"
#include "pax_gfx.h"

void bench_ui_open(void);
void bench_ui_close(void);
bool bench_ui_active(void);

void bench_ui_event(bsp_input_event_t const* ev);
void bench_ui_update(inventory_t* inv);
void bench_ui_draw(pax_buf_t* fb, inventory_t const* inv);
