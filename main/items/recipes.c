// =====================================================================
//  CraftMiner  --  the recipe table (see recipes.h)
// ---------------------------------------------------------------------
//  Quantities are Minecraft's, as the user asked. Where CraftMiner
//  invents something -- the disassembly bench, the trashcan -- the row
//  says so.
// =====================================================================

#include "items/recipes.h"

#include <string.h>

static recipe_t const RECIPES[] = {
    // --- Anywhere, from the Tab screen ------------------------------
    //
    // The user's short list: what a player must be able to make with
    // nothing but their hands, so that finding wood is enough to get
    // started. Everything else waits for a crafting table.

    {.out     = BLK_PLANKS, .out_n = 4, .station = RS_INVENTORY,
     .n_in    = 1,
     .in      = {{BLK_LOG, 1}}},

    {.out     = ITEM_STICK, .out_n = 4, .station = RS_INVENTORY,
     .n_in    = 1,
     .in      = {{BLK_PLANKS, 2}}},

    {.out     = BLK_TORCH, .out_n = 4, .station = RS_INVENTORY,
     .n_in    = 2,
     .in      = {{ITEM_COAL, 1}, {ITEM_STICK, 1}}},

    // None of the three is RF_REVERSIBLE: planks and sticks are what
    // the user called basic resources, and a torch that came back as
    // coal and a stick would be a way of un-burning it.

    // The table itself is made without one, or there would be no way
    // to ever have one.
    {.out     = BLK_CRAFTING_TABLE, .out_n = 1, .station = RS_INVENTORY, .flags = RF_REVERSIBLE,
     .n_in    = 1,
     .in      = {{BLK_PLANKS, 4}}},

    // --- At a crafting table ----------------------------------------
    //
    // Minecraft's quantities, and Minecraft's shapes are what made a
    // pickaxe and an axe different recipes there. Here they are simply
    // two rows with the same ingredients, which is allowed and which is
    // why the "no two collide" rule went away with the grid (Part C).

    {.out     = ITEM_PICK_WOOD, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_PLANKS, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_AXE_WOOD, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_PLANKS, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_SHOVEL_WOOD, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_PLANKS, 1}, {ITEM_STICK, 2}}},

    {.out     = ITEM_PICK_STONE, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_COBBLE, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_AXE_STONE, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_COBBLE, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_SHOVEL_STONE, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_COBBLE, 1}, {ITEM_STICK, 2}}},

    {.out     = BLK_FURNACE, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 1,
     .in      = {{BLK_COBBLE, 8}}},

    // --- In a furnace -----------------------------------------------
    //
    // One input, and the fuel is NOT an ingredient -- it has a slot of
    // its own (game/furnace.h), so these rows name only what goes in
    // and what comes out.
    //
    // Wood becomes COAL, not charcoal: the user's simplification, and
    // it means one fewer item that burns exactly like another one.
    {.out     = ITEM_COAL, .out_n = 1, .station = RS_FURNACE,
     .n_in    = 1,
     .in      = {{BLK_LOG, 1}}},

    // Sand to glass, and cobblestone back to stone. Neither was asked
    // for, both are Minecraft's, and each unlocks a block that had NO
    // way of being obtained at all: glass drops nothing when broken,
    // and stone drops cobblestone. Without these the furnace's only job
    // would be turning wood into coal you could have dug up.
    {.out     = BLK_GLASS, .out_n = 1, .station = RS_FURNACE,
     .n_in    = 1,
     .in      = {{BLK_SAND, 1}}},

    {.out     = BLK_STONE, .out_n = 1, .station = RS_FURNACE,
     .n_in    = 1,
     .in      = {{BLK_COBBLE, 1}}},
};

#define RECIPE_N ((int)(sizeof(RECIPES) / sizeof(RECIPES[0])))

int recipe_count(void) {
    return RECIPE_N;
}

recipe_t const* recipe_at(int i) {
    return (i >= 0 && i < RECIPE_N) ? &RECIPES[i] : NULL;
}

bool recipe_known(recipe_t const* r, inventory_t const* inv) {
    if (r == NULL || inv == NULL) return false;
    // ANY one ingredient, not all of them: the book is a hint about
    // what the material in your hand is for.
    for (int i = 0; i < r->n_in; i++) {
        if (inv_seen(inv, r->in[i].item)) return true;
    }
    return false;
}

int recipe_can_make(recipe_t const* r, inventory_t const* inv, int cap) {
    if (r == NULL || inv == NULL || cap <= 0 || r->n_in == 0) return 0;

    int n = cap;
    for (int i = 0; i < r->n_in && n > 0; i++) {
        int const have = inv_count(inv, r->in[i].item);
        int const need = r->in[i].count;
        int const from = need > 0 ? have / need : cap;
        if (from < n) n = from;
    }
    return n < 0 ? 0 : n;
}

int recipe_missing(recipe_t const* r, inventory_t const* inv, int ing) {
    if (r == NULL || inv == NULL || ing < 0 || ing >= r->n_in) return 0;
    int const need = r->in[ing].count;
    int const have = inv_count(inv, r->in[ing].item);
    return have >= need ? 0 : need - have;
}

int recipe_make(recipe_t const* r, inventory_t* inv, int n) {
    if (r == NULL || inv == NULL || n <= 0) return 0;

    int made = 0;
    for (; made < n; made++) {
        if (recipe_can_make(r, inv, 1) < 1) break;

        // ALL OR NOTHING PER UNIT. The output may not fit -- a full
        // inventory, or a tool that cannot stack onto the one already
        // held -- and crafting must never eat the materials in that
        // case. So the whole inventory is snapshotted before the unit
        // and put back if anything goes wrong. It is 130-odd bytes of
        // struct copy against a class of bug that is very hard to see
        // and impossible to undo.
        inventory_t const before = *inv;

        bool ok = true;
        for (int i = 0; i < r->n_in && ok; i++) {
            ok = inv_take(inv, r->in[i].item, r->in[i].count);
        }
        if (ok && inv_add(inv, r->out, r->out_n, 0) != 0) ok = false;

        if (!ok) {
            *inv = before;
            break;
        }
    }
    return made;
}
