#pragma once
// =====================================================================
//  CraftMiner  --  what the player is carrying
// ---------------------------------------------------------------------
//  Slots of (item, count, wear). The first INV_HOTBAR are the hotbar,
//  reachable with F1-F6; the rest is the Tab screen. One array, because
//  a hotbar that is a separate array is a second place for a stack to
//  get lost.
//
//  STACKING IS THE WHOLE POINT and the part worth testing: picking
//  something up fills partial stacks of the same item FIRST and only
//  then takes an empty slot, or a player ends up with six slots of
//  three cobblestone. Tools never stack (stack_max 1) and each carries
//  its own wear, so two half-worn pickaxes stay two.
//
//  Pure: no engine, no allocation. tools/worldcheck.c exercises it.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "items/items.h"

#define INV_HOTBAR 6                     // F1-F6, as the user specified
#define INV_ROWS   3                     // the Tab screen, above the hotbar
#define INV_SLOTS  (INV_HOTBAR * (INV_ROWS + 1))

typedef struct {
    uint16_t item;   // 0 = empty
    uint8_t  count;
    uint16_t wear;   // uses spent, for a tool; 0 otherwise
} inv_slot_t;

typedef struct {
    inv_slot_t slot[INV_SLOTS];
    int        selected;  // 0..INV_HOTBAR-1
    // The Tab screen. Open, it takes the movement keys for navigation
    // and the player stands still -- picking things up while reading a
    // grid is how you walk into lava.
    bool       open;
    int        cursor;    // 0..INV_SLOTS-1, where the Tab screen is pointing
} inventory_t;

void inv_clear(inventory_t* inv);

// Put `count` of `item` in. Returns how many did NOT fit, so a caller
// that cannot drop the remainder knows it has to refuse.
int inv_add(inventory_t* inv, uint16_t item, int count, uint16_t wear);

// The selected hotbar slot, never NULL.
inv_slot_t* inv_held(inventory_t* inv);

// Take one off the selected stack (a block was placed). False if it was
// empty.
bool inv_consume_held(inventory_t* inv);

// Wear the held tool by one use. Returns true if it BROKE, which the
// caller usually wants to make a noise about.
bool inv_wear_held(inventory_t* inv, int uses);

// How many of `item` are carried in total.
int inv_count(inventory_t const* inv, uint16_t item);

// Swap two slots. How a stack gets from the Tab screen onto the
// hotbar, and it is a swap rather than a move so the hotbar slot's
// contents are never dropped on the floor to make room.
void inv_swap(inventory_t* inv, int a, int b);

// Move the Tab screen's cursor by (dx, dy) as the grid is DRAWN, clamped
// at the edges: dy < 0 is up the screen. The screen puts the storage
// rows on top and the hotbar at the bottom, where it sits when the
// screen is closed -- which is the reverse of slot order, since the
// hotbar is slots 0..5. So up from the hotbar is the lowest storage row,
// not nothing. inv_screen_row() is that mapping, and the screen draws
// with the same function.
void inv_move_cursor(inventory_t* inv, int dx, int dy);

// The on-screen row of slot `i`: 0 is the top storage row, INV_ROWS the
// hotbar.
static inline int inv_screen_row(int i) {
    return i < INV_HOTBAR ? INV_ROWS : i / INV_HOTBAR - 1;
}
