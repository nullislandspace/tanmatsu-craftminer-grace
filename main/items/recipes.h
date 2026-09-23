#pragma once
// =====================================================================
//  CraftMiner  --  the recipe registry
// ---------------------------------------------------------------------
//  A THIRD TABLE, like blocks.h and items.h, and adding a recipe is a
//  row. See claudeplans/craftminer.md, Part C, for the design; the two
//  things worth knowing before reading this file:
//
//  1. A RECIPE IS A MULTISET, NOT A GRID. The badge has a keyboard and
//     no pointer, so crafting is a searchable book rather than nine
//     cells you drag things into (the user's call, 2026-09-23). With no
//     grid the SHAPE of a recipe is not merely unused, it is
//     unobservable -- there is no surface on which a player could see
//     or express it. So there is no w, no h, no in[9] and no offsets,
//     and a pickaxe and an axe are free to want the same three planks
//     and two sticks, as they do in Minecraft. Nothing here ever infers
//     a recipe from a pile of ingredients: forward, the player names
//     it; backward, the disassembly bench starts from the item.
//
//  2. QUANTITIES ARE MINECRAFT'S, as asked.
//
//  Pure: no engine, no allocation. tools/worldcheck.c builds it as-is.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "items/inventory.h"
#include "items/items.h"

// The most distinct ingredients one recipe may name. Minecraft's whole
// tree needs three; four leaves room without making the table fat.
#define RECIPE_IN_MAX 4

// Where a recipe can be made. The player carries the first one with
// them; the others are blocks they have to stand in front of.
typedef enum {
    RS_INVENTORY = 0,  // anywhere, from the Tab screen
    RS_TABLE,          // at a crafting table
    RS_FURNACE,        // smelting
    RS_COUNT
} recipe_station_t;

// The disassembly bench may undo this one. It is a bit in the table
// rather than a rule in the bench, which is how the user's "doesn't
// work for basic resources like iron ingots turning into ore or sticks
// turning into planks" stays a piece of DATA about each recipe.
#define RF_REVERSIBLE (1u << 0)

typedef struct {
    uint16_t item;
    uint8_t  count;
} ingredient_t;

typedef struct {
    uint16_t     out;
    uint8_t      out_n;
    uint8_t      station;  // recipe_station_t
    uint8_t      flags;    // RF_*
    uint8_t      n_in;
    ingredient_t in[RECIPE_IN_MAX];
} recipe_t;

int             recipe_count(void);
recipe_t const* recipe_at(int i);

// --- What the player knows --------------------------------------------
//
// The user's rule: a recipe appears once the player has picked up at
// least one of the materials it needs. Stored the OTHER WAY ROUND --
// a bit per item, in the inventory (inventory.h, inv_seen), set by
// inv_add -- because a bitmask over RECIPES would make recipe numbering
// permanent the way block ids are (D-74) and would need a migration
// every time a recipe was inserted. Item names are already what saves
// key on. A recipe added by a later build is then correctly already
// known to a player who has handled its ingredients.

bool recipe_known(recipe_t const* r, inventory_t const* inv);

// --- Making things ----------------------------------------------------

// How many times `r` could be made from what is carried, capped at
// `cap` so a caller asking "any?" does not pay for counting to sixty.
int recipe_can_make(recipe_t const* r, inventory_t const* inv, int cap);

// Make `n` of `r`, taking the ingredients and adding the output.
// Returns how many were actually made.
//
// ALL OR NOTHING PER UNIT: if the output would not fit, that unit is
// not made and its ingredients are not taken. Crafting into a full
// inventory must never eat the materials.
int recipe_make(recipe_t const* r, inventory_t* inv, int n);

// How many of `ing` are still missing to make `r` once. 0 when the
// player has enough -- this is what puts "2 x Stick -- have 0" under
// the cursor in the book.
int recipe_missing(recipe_t const* r, inventory_t const* inv, int ing);
