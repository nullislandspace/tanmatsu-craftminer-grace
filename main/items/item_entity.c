// =====================================================================
//  SynthMiner  --  things lying on the ground (see item_entity.h)
// =====================================================================

#include "items/item_entity.h"

#include <math.h>

#include "common/rng.h"
#include "world/chunk.h"

// Falls like the player but lighter, and with no terminal velocity
// worth the name: a dropped item never falls far.
#define IE_GRAVITY 0.04f
#define IE_DRAG    0.98f
#define IE_FRICTION 0.6f  // per tick, once it is resting on something

static item_entity_t s_pool[ITEM_ENTITY_MAX];
static int           s_live;
// The scatter is seeded from the cell, not from a global: two players
// breaking the same block get the same scatter, and a replay is exact.
static uint32_t      s_spawn_seq;

void item_entity_reset(void) {
    for (int i = 0; i < ITEM_ENTITY_MAX; i++) s_pool[i].alive = false;
    s_live      = 0;
    s_spawn_seq = 0;
}

int item_entity_copy(item_entity_t* out, int max) {
    int n = 0;
    for (int i = 0; i < ITEM_ENTITY_MAX && n < max; i++) {
        if (s_pool[i].alive) out[n++] = s_pool[i];
    }
    return n;
}

void item_entity_restore(item_entity_t const* in, int n) {
    item_entity_reset();
    for (int i = 0; i < n && i < ITEM_ENTITY_MAX; i++) {
        if (!in[i].alive) continue;
        s_pool[s_live++] = in[i];
    }
}

int item_entity_live(void) {
    return s_live;
}

item_entity_t const* item_entity_at(int i) {
    return (i >= 0 && i < ITEM_ENTITY_MAX) ? &s_pool[i] : NULL;
}

static item_entity_t* claim(void) {
    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        if (!s_pool[i].alive) return &s_pool[i];
    }
    return NULL;  // full: the drop is lost, which is better than a stall
}

int item_entity_spawn(int32_t x, int32_t y, int32_t z, uint16_t item, int count, uint16_t wear) {
    if (item == 0 || count <= 0) return 0;
    item_def_t const d   = item_def(item);
    int const        cap = d.stack_max < 1 ? 1 : d.stack_max;

    int made = 0;
    while (count > 0) {
        item_entity_t* e = claim();
        if (e == NULL) break;
        int const take = count < cap ? count : cap;

        // A little scatter, deterministic from the cell and a counter,
        // so a felled tree's logs do not all land on one point and a
        // replay still puts them in the same places.
        uint32_t const seq = s_spawn_seq++;
        float const    rx  = sm_rand3(x, (int32_t)(y * 3 + (int32_t)seq), z, 0x1D0Fu) - 0.5f;
        float const    rz  = sm_rand3(x, (int32_t)(y * 7 + (int32_t)seq), z, 0x2E1Au) - 0.5f;

        phys_body_init(&e->body, (double)x + 0.5 + (double)rx * 0.4, (double)y + 0.25, (double)z + 0.5 + (double)rz * 0.4);
        e->body.w  = ITEM_ENTITY_SIZE;
        e->body.h  = ITEM_ENTITY_SIZE;
        e->body.vx = rx * 0.06f;
        e->body.vz = rz * 0.06f;
        e->body.vy = 0.08f;  // a small hop out of the broken cell

        e->alive     = true;
        e->item      = item;
        e->count     = (uint8_t)take;
        e->wear      = wear;
        e->age       = 0;
        e->pickup_at = ITEM_PICKUP_DELAY;
        s_live++;
        made += take;
        count -= take;
    }
    return made;
}

int item_entity_throw(double x, double y, double z, uint16_t item, int count, uint16_t wear, float vx, float vy,
                      float vz, uint32_t pickup_delay) {
    if (item == 0 || count <= 0) return 0;
    item_entity_t* e = claim();
    if (e == NULL) return 0;

    item_def_t const d   = item_def(item);
    int const        cap = d.stack_max < 1 ? 1 : d.stack_max;
    int const        take = count < cap ? count : cap;

    phys_body_init(&e->body, x, y, z);
    e->body.w  = ITEM_ENTITY_SIZE;
    e->body.h  = ITEM_ENTITY_SIZE;
    e->body.vx = vx;
    e->body.vy = vy;
    e->body.vz = vz;

    e->alive     = true;
    e->item      = item;
    e->count     = (uint8_t)take;
    e->wear      = wear;
    e->age       = 0;
    e->pickup_at = pickup_delay;
    s_live++;
    return take;
}

int item_entity_tick(inventory_t* inv, double px, double py, double pz) {
    int picked = 0;
    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        item_entity_t* e = &s_pool[i];
        if (!e->alive) continue;
        // An item in a chunk that is not loaded HOLDS STILL: no fall (it
        // would drop through the missing ground), no age, no pickup. It
        // carries on when its chunk comes back -- walking away must not
        // cost the player their drops (D-33).
        if (chunk_find(chunk_of((int32_t)floor(e->body.x)), chunk_of((int32_t)floor(e->body.z))) == NULL) continue;

        // The age, in ticks, advancing only because this tick ran (D-51).
        e->age++;
        if (e->age >= ITEM_DESPAWN_TICKS) {
            e->alive = false;
            s_live--;
            continue;
        }

        phys_move(&e->body, (double)e->body.vx, (double)e->body.vy, (double)e->body.vz);
        phys_gravity(&e->body, IE_GRAVITY, IE_DRAG, 3.0f);
        if (e->body.on_ground) {
            e->body.vx *= IE_FRICTION;
            e->body.vz *= IE_FRICTION;
        }
        if (e->body.hit_x) e->body.vx = 0.0f;
        if (e->body.hit_z) e->body.vz = 0.0f;

        if (inv == NULL || e->age < e->pickup_at) continue;

        // Close enough? Measured to the player's middle, not their
        // feet, so an item on a ledge at head height counts.
        double const dx = e->body.x - px;
        double const dy = e->body.y - (py + 0.9);
        double const dz = e->body.z - pz;
        if (dx * dx + dy * dy + dz * dz > (double)(ITEM_PICKUP_RANGE * ITEM_PICKUP_RANGE)) continue;

        int const left = inv_add(inv, e->item, e->count, e->wear);
        if (left >= e->count) continue;  // inventory full: it stays on the ground
        picked += e->count - left;
        if (left == 0) {
            e->alive = false;
            s_live--;
        } else {
            e->count = (uint8_t)left;  // partially collected
        }
    }
    return picked;
}
