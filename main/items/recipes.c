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

    // Containers. The "+ one coal" on the last two is the user's
    // design: the destructive ones cost a little fire to build.
    {.out     = BLK_CHEST, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 1,
     .in      = {{BLK_PLANKS, 8}}},

    {.out     = BLK_TRASH, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_PLANKS, 8}, {ITEM_COAL, 1}}},

    {.out     = BLK_BENCH, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{BLK_PLANKS, 4}, {ITEM_COAL, 1}}},

    // Iron tools. Ingots come out of the furnace, which is why these
    // could not exist before it did.
    {.out     = ITEM_PICK_IRON, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{ITEM_IRON_INGOT, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_AXE_IRON, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{ITEM_IRON_INGOT, 3}, {ITEM_STICK, 2}}},
    {.out     = ITEM_SHOVEL_IRON, .out_n = 1, .station = RS_TABLE, .flags = RF_REVERSIBLE,
     .n_in    = 2,
     .in      = {{ITEM_IRON_INGOT, 1}, {ITEM_STICK, 2}}},

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

    // NOT reversible: an ingot does not go back to ore, which is the
    // user's own example of what the bench must refuse.
    {.out     = ITEM_IRON_INGOT, .out_n = 1, .station = RS_FURNACE,
     .n_in    = 1,
     .in      = {{BLK_IRON_ORE, 1}}},
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

// The recipe that makes `item` at this station or a lesser one, or
// NULL. Never a furnace: see the header.
static recipe_t const* maker_of(uint16_t item, int station) {
    for (int i = 0; i < RECIPE_N; i++) {
        recipe_t const* r = &RECIPES[i];
        if (r->out != item || r->station == RS_FURNACE) continue;
        if (recipe_station_allows(station, r->station)) return r;
    }
    return NULL;
}

// Bring the number of `item` carried up to `need`, crafting if it must.
static bool provide(uint16_t item, int need, inventory_t* inv, int station, int depth) {
    if (inv_count(inv, item) >= need) return true;
    if (depth <= 0) return false;

    recipe_t const* r = maker_of(item, station);
    if (r == NULL) return false;

    // Its own ingredients first, then one batch, then look again. The
    // loop terminates because a recipe never makes what it consumes
    // (worldcheck: "a recipe for %s is made of itself") and every batch
    // adds at least one, so the count strictly rises.
    while (inv_count(inv, item) < need) {
        for (int i = 0; i < r->n_in; i++) {
            if (!provide(r->in[i].item, r->in[i].count, inv, station, depth - 1)) return false;
        }
        if (recipe_make(r, inv, 1) < 1) return false;  // no room, or a cycle we cannot see
    }
    return true;
}

int recipe_make_auto(recipe_t const* r, inventory_t* inv, int n, int station) {
    if (r == NULL || inv == NULL || n <= 0) return 0;
    // The recipe itself has to belong here. The book never offers one
    // that does not, but a planner that would make a pickaxe in your
    // bare hands because nobody asked it not to is a planner nobody
    // should trust with the inventory.
    if (!recipe_station_allows(station, r->station)) return 0;

    int made = 0;
    for (; made < n; made++) {
        // The snapshot covers the WHOLE plan, not just the last step: a
        // plan that turns three logs into planks and then finds there
        // are no sticks must give the logs back, not leave the player
        // with planks they did not ask for.
        inventory_t const before = *inv;

        // MORE THAN ONE PASS, and this is the whole subtlety of the
        // thing. Ingredients are provided one after another, and a
        // later one can EAT what an earlier one just made: asked for a
        // wooden pickaxe from two logs, the planner turns a log into
        // four planks, then turns two of those planks into sticks --
        // and the three planks it had a moment ago are now two.
        //
        // Nothing here reserves. It simply asks again: each pass
        // re-provides whatever is short, and the loop ends when the
        // recipe can actually be made, when a pass achieves nothing,
        // or when something cannot be provided at all.
        bool ok = false;
        for (int pass = 0; pass < RECIPE_AUTO_DEPTH && !ok; pass++) {
            inventory_t const snap = *inv;
            bool              all  = true;
            for (int i = 0; i < r->n_in && all; i++) {
                all = provide(r->in[i].item, r->in[i].count, inv, station, RECIPE_AUTO_DEPTH);
            }
            if (!all) break;
            if (recipe_can_make(r, inv, 1) >= 1) ok = true;
            else if (memcmp(&snap, inv, sizeof(snap)) == 0) break;  // no progress; it will not converge
        }
        if (!ok || recipe_make(r, inv, 1) < 1) {
            *inv = before;
            break;
        }
    }
    return made;
}

int recipe_can_make_auto(recipe_t const* r, inventory_t const* inv, int station) {
    if (r == NULL || inv == NULL) return 0;
    inventory_t scratch = *inv;  // ask by doing, on a copy: one code path
    return recipe_make_auto(r, &scratch, 1, station);
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
