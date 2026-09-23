// =====================================================================
//  CraftMiner  --  the furnace (see furnace.h)
// =====================================================================

#include "game/furnace.h"

#include "items/items.h"
#include "items/recipes.h"

uint16_t furnace_smelts_to(uint16_t item) {
    if (item == 0) return 0;
    for (int i = 0; i < recipe_count(); i++) {
        recipe_t const* r = recipe_at(i);
        if (r->station != RS_FURNACE) continue;
        if (r->n_in == 1 && r->in[0].item == item) return r->out;
    }
    return 0;
}

uint16_t furnace_fuel_ticks(uint16_t item) {
    return item_def(item).fuel;
}

// Can the output slot take one more of `out`?
static bool output_has_room(blockent_t const* be, uint16_t out) {
    inv_slot_t const* o = &be->slot[BE_FURNACE_OUTPUT];
    if (o->item == 0 || o->count == 0) return true;
    if (o->item != out) return false;
    return o->count < item_def(out).stack_max;
}

static void output_add(blockent_t* be, uint16_t out) {
    inv_slot_t* o = &be->slot[BE_FURNACE_OUTPUT];
    if (o->item == 0 || o->count == 0) {
        o->item  = out;
        o->count = 1;
        o->wear  = 0;
    } else {
        o->count++;
    }
}

// Take one fuel item and light it. False if there was none.
static bool light_next(blockent_t* be) {
    inv_slot_t* f = &be->slot[BE_FURNACE_FUEL];
    if (f->item == 0 || f->count == 0) return false;
    uint16_t const ticks = furnace_fuel_ticks(f->item);
    if (ticks == 0) return false;

    be->burn_left = ticks;
    be->burn_max  = ticks;
    if (--f->count == 0) {
        f->item = 0;
        f->wear = 0;
    }
    return true;
}

void furnace_catch_up(blockent_t* be, uint32_t now) {
    if (be == NULL || be->kind != BE_FURNACE) return;

    // Time only ever runs forward. A stamp from the future means the
    // world's clock moved back under it (a restored save, a test that
    // rewinds); treat it as no time at all rather than as four billion
    // ticks of free smelting.
    uint32_t elapsed = now >= be->stamp ? now - be->stamp : 0;
    be->stamp        = now;

    // The loop runs once per EVENT -- fuel lit, item finished, work
    // stopped -- not once per tick, so a furnace left for a week costs
    // the same handful of iterations as one left for a minute. Without
    // that, opening a chunk after a long absence would be a hundred
    // thousand trips round a loop for nothing.
    while (elapsed > 0) {
        inv_slot_t* in = &be->slot[BE_FURNACE_INPUT];
        uint16_t const out = furnace_smelts_to(in->count > 0 ? in->item : 0);
        if (out == 0 || !output_has_room(be, out)) {
            // Nothing to do. The fire is NOT spent while it waits --
            // Minecraft burns fuel into an empty furnace and this does
            // not, because nobody has ever been glad of that rule.
            be->cook = 0;
            break;
        }

        if (be->burn_left == 0 && !light_next(be)) {
            be->cook = 0;  // cold: the part-cooked item does not keep
            break;
        }

        uint32_t const need = FURNACE_COOK_TICKS - be->cook;
        uint32_t       step = elapsed;
        if (step > need) step = need;
        if (step > be->burn_left) step = be->burn_left;

        be->cook = (uint16_t)(be->cook + step);
        be->burn_left = (uint16_t)(be->burn_left - step);
        elapsed -= step;

        if (be->cook >= FURNACE_COOK_TICKS) {
            be->cook = 0;
            output_add(be, out);
            if (--in->count == 0) {
                in->item = 0;
                in->wear = 0;
            }
        }
    }

    if (be->burn_left == 0) be->burn_max = 0;
}

bool furnace_busy(blockent_t const* be) {
    // ACTIVELY SMELTING, which is not the same as "has fire in it".
    // Fuel left over when the last item finished stays lit and waiting
    // -- nothing is burned for nothing (above) -- so a furnace can have
    // 800 ticks of coal in it and still be doing nothing at all.
    return furnace_idle_reason(be) == FURNACE_IDLE_NONE;
}

int furnace_progress_pct(blockent_t const* be) {
    if (be == NULL) return 0;
    int const pct = (int)be->cook * 100 / FURNACE_COOK_TICKS;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

furnace_idle_t furnace_idle_reason(blockent_t const* be) {
    if (be == NULL || be->kind != BE_FURNACE) return FURNACE_IDLE_NO_INPUT;

    inv_slot_t const* in = &be->slot[BE_FURNACE_INPUT];
    uint16_t const out = furnace_smelts_to(in->count > 0 ? in->item : 0);
    if (out == 0) return FURNACE_IDLE_NO_INPUT;
    if (!output_has_room(be, out)) return FURNACE_IDLE_FULL;
    if (be->burn_left == 0 && !furnace_is_fuel(be->slot[BE_FURNACE_FUEL].item)) return FURNACE_IDLE_NO_FUEL;
    return FURNACE_IDLE_NONE;
}
