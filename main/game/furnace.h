#pragma once
// =====================================================================
//  SynthMiner  --  the furnace
// ---------------------------------------------------------------------
//  Three slots -- what goes in, what burns, what comes out -- and the
//  user's design, not a filtered recipe book (2026-09-23, replacing the
//  sketch in Part C). Selecting the output takes it; selecting either
//  of the other two opens a picker over the inventory.
//
//  IT NEVER TICKS. The whole of the furnace's work is done from
//  `now - stamp` at the moment somebody opens it, which is the trick
//  the user asked for on the trashcan and which is worth more here:
//
//    * no per-tick list of furnaces to walk
//    * a furnace in a chunk nobody has visited costs exactly nothing
//    * it is automatically right across a save, an eviction, a world
//      closed for a week, and the debug key that jumps the clock
//
//  Fuel is anything that burns, as asked: logs, planks, sticks, wooden
//  tools -- and coal, which is what it is for. That lives in the ITEM
//  table (items.h, `fuel`), so a new wooden thing brings its own
//  burn time with it rather than needing a line here.
//
//  Pure: no engine, no allocation. tools/worldcheck.c builds it as-is.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "items/inventory.h"
#include "world/blockent.h"

// Ticks to smelt one item: ten seconds at 20 Hz, which is Minecraft's.
#define FURNACE_COOK_TICKS 200

// What `item` smelts into, or 0 if it does not smelt. Reads the recipe
// table (RS_FURNACE), so adding a smelt is a row there.
uint16_t furnace_smelts_to(uint16_t item);

// How long `item` burns for, in ticks. 0 for anything that does not.
uint16_t furnace_fuel_ticks(uint16_t item);

static inline bool furnace_is_fuel(uint16_t item) {
    return furnace_fuel_ticks(item) > 0;
}

// Run the furnace forward to `now`, then stamp it. Called when it is
// opened, when it is saved and when it is drawn -- anywhere the state
// has to be true rather than merely stored. Doing it twice in a row is
// free, since the second call has no elapsed time to spend.
void furnace_catch_up(blockent_t* be, uint32_t now);

// Is it actually smelting, and how far through the current item? The
// two questions the screen asks.
//
// "Busy" is not "has fire in it": fuel left over when an item finishes
// stays lit and waiting, since a furnace here never burns fuel it has
// no use for. `pct` is 0..100.
bool furnace_busy(blockent_t const* be);
int  furnace_progress_pct(blockent_t const* be);

// Why it is doing nothing, for the line under the slots.
typedef enum {
    FURNACE_IDLE_NONE = 0,  // it is working
    FURNACE_IDLE_NO_INPUT,  // nothing to smelt
    FURNACE_IDLE_NO_FUEL,   // nothing to burn
    FURNACE_IDLE_FULL,      // the output slot cannot take any more
} furnace_idle_t;

furnace_idle_t furnace_idle_reason(blockent_t const* be);
