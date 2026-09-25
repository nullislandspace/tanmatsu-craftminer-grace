// =====================================================================
//  SynthMiner  --  what the player is carrying (see inventory.h)
// =====================================================================

#include "items/inventory.h"

#include <string.h>

void inv_clear(inventory_t* inv) {
    memset(inv, 0, sizeof(*inv));
    inv->selected = 0;
}

void inv_mark_seen(inventory_t* inv, uint16_t item) {
    if (inv == NULL || item == 0 || item >= ITEM_COUNT) return;
    inv->seen[item >> 5] |= (uint32_t)1u << (item & 31u);
}

bool inv_seen(inventory_t const* inv, uint16_t item) {
    if (inv == NULL || item == 0 || item >= ITEM_COUNT) return false;
    return (inv->seen[item >> 5] & ((uint32_t)1u << (item & 31u))) != 0;
}

int inv_add(inventory_t* inv, uint16_t item, int count, uint16_t wear) {
    if (item == 0 || count <= 0) return count > 0 ? count : 0;
    item_def_t const d   = item_def(item);
    int const        cap = d.stack_max < 1 ? 1 : d.stack_max;

    // Held once is held forever, for the recipe book. Marked HERE, on
    // the way in, because this is the one door everything comes
    // through. Marked even if nothing fits: the player has seen it --
    // it was in their hands long enough to bounce off a full pack.
    inv_mark_seen(inv, item);

    // PARTIAL STACKS FIRST, every one of them, before any empty slot is
    // touched. The other order leaves a player with five slots holding
    // three cobblestone each and no room for anything else.
    if (cap > 1) {
        for (int i = 0; i < INV_SLOTS && count > 0; i++) {
            inv_slot_t* s = &inv->slot[i];
            if (s->item != item || s->count >= cap) continue;
            // Wear has to match, or two differently-worn tools would
            // merge and one of them would silently repair.
            if (s->wear != wear) continue;
            int const room = cap - s->count;
            int const take = count < room ? count : room;
            s->count       = (uint8_t)(s->count + take);
            count -= take;
        }
    }

    // Then empties -- hotbar first, so what you pick up is to hand.
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
        inv_slot_t* s = &inv->slot[i];
        if (s->item != 0) continue;
        int const take = count < cap ? count : cap;
        s->item        = item;
        s->count       = (uint8_t)take;
        s->wear        = wear;
        count -= take;
    }
    return count;  // what would not fit
}

inv_slot_t* inv_held(inventory_t* inv) {
    int i = inv->selected;
    if (i < 0 || i >= INV_HOTBAR) i = 0;
    return &inv->slot[i];
}

bool inv_consume_held(inventory_t* inv) {
    inv_slot_t* s = inv_held(inv);
    if (s->item == 0 || s->count == 0) return false;
    if (--s->count == 0) {
        s->item = 0;
        s->wear = 0;
    }
    return true;
}

bool inv_wear_held(inventory_t* inv, int uses) {
    inv_slot_t* s = inv_held(inv);
    if (s->item == 0) return false;
    item_def_t const d = item_def(s->item);
    if (d.durability == 0) return false;  // not a tool, or one that never wears

    int const w = (int)s->wear + (uses < 1 ? 1 : uses);
    if (w < (int)d.durability) {
        s->wear = (uint16_t)w;
        return false;
    }
    // Spent. The slot empties: a broken tool leaves nothing behind.
    s->item  = 0;
    s->count = 0;
    s->wear  = 0;
    return true;
}

int inv_count(inventory_t const* inv, uint16_t item) {
    int n = 0;
    for (int i = 0; i < INV_SLOTS; i++) {
        if (inv->slot[i].item == item) n += inv->slot[i].count;
    }
    return n;
}

bool inv_take(inventory_t* inv, uint16_t item, int count) {
    if (inv == NULL || item == 0) return count <= 0;
    if (count <= 0) return true;
    if (inv_count(inv, item) < count) return false;  // all or nothing

    // Fullest partial stacks first: emptying the small ones would leave
    // the pack full of holes after a few crafts.
    while (count > 0) {
        int best = -1;
        for (int i = 0; i < INV_SLOTS; i++) {
            if (inv->slot[i].item != item || inv->slot[i].count == 0) continue;
            if (best < 0 || inv->slot[i].count > inv->slot[best].count) best = i;
        }
        if (best < 0) return false;  // inv_count lied; cannot happen

        inv_slot_t* s    = &inv->slot[best];
        int const   take = count < (int)s->count ? count : (int)s->count;
        s->count         = (uint8_t)(s->count - take);
        count -= take;
        if (s->count == 0) {
            s->item = 0;
            s->wear = 0;
        }
    }
    return true;
}

void inv_swap(inventory_t* inv, int a, int b) {
    if (a < 0 || b < 0 || a >= INV_SLOTS || b >= INV_SLOTS || a == b) return;
    inv_slot_t const t = inv->slot[a];
    inv->slot[a]       = inv->slot[b];
    inv->slot[b]       = t;
}

void inv_move_cursor(inventory_t* inv, int dx, int dy) {
    // In SCREEN rows, then back to a slot. Moving in slot order instead
    // walked the rows upside down: up from the hotbar went nowhere and
    // the bottom storage row was reached over the top edge.
    int x   = inv->cursor % INV_HOTBAR + dx;
    int row = inv_screen_row(inv->cursor) + dy;
    if (x < 0) x = 0;
    if (x >= INV_HOTBAR) x = INV_HOTBAR - 1;
    if (row < 0) row = 0;
    if (row > INV_ROWS) row = INV_ROWS;
    int const slot_row = row == INV_ROWS ? 0 : row + 1;
    inv->cursor        = slot_row * INV_HOTBAR + x;
}
