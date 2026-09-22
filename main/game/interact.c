// =====================================================================
//  CraftMiner  --  breaking and placing (see interact.h)
// =====================================================================

#include "game/interact.h"

#include "common/rng.h"
#include "items/item_entity.h"
#include "world/chunk.h"

// Which tool the current fell is being done with. A parameter would
// have to thread through the flood fill's whole frontier; the fell is
// one call on one task, so this is simply set around it.
static uint16_t s_fell_tool;

static inline int32_t fl(double v) {
    int32_t const i = (int32_t)v;
    return (v < (double)i) ? i - 1 : i;
}

// A grown tree block: fellable, and not one a player put there.
static bool grown_tree(int32_t x, int32_t y, int32_t z) {
    if (!block_fellable(world_block(x, y, z))) return false;
    return (world_state(x, y, z) & ST_PLACED) == 0;
}

// Drop what a block yields, on the ground where it stood.
static int drop_for(uint8_t block, int32_t x, int32_t y, int32_t z, uint16_t tool_item) {
    block_def_t const* d = block_def(block);
    if (d->drop_item == ITEM_NONE || d->drop_max == 0) return 0;
    if (!item_can_harvest(block, tool_item)) return 0;

    int n = d->drop_min;
    if (d->drop_max > d->drop_min) {
        // Deterministic from the cell, so a replay drops the same
        // number and two players breaking the same block agree.
        float const r = cm_rand3(x, y, z, 0x0D40Fu);
        n += (int)(r * (float)(d->drop_max - d->drop_min + 1));
        if (n > d->drop_max) n = d->drop_max;
    }
    return item_entity_spawn(x, y, z, d->drop_item, n, 0);
}

int interact_fell(int32_t x0, int32_t y0, int32_t z0) {
    // The frontier, as explicit storage rather than recursion: a canopy
    // is hundreds of blocks and this runs on the game task.
    static struct {
        int32_t x, y, z;
    } stack[FELL_MAX];
    int n = 0, taken = 0;

    stack[n].x = x0;
    stack[n].y = y0;
    stack[n].z = z0;
    n++;
    // Take the first one immediately, so it cannot be pushed again.
    uint8_t const first = world_block(x0, y0, z0);
    world_set(x0, y0, z0, BLK_AIR, 0);
    drop_for(first, x0, y0, z0, s_fell_tool);
    taken++;

    while (n > 0) {
        n--;
        int32_t const cx = stack[n].x, cy = stack[n].y, cz = stack[n].z;

        // The 26 neighbours, not 6: a canopy is diagonal everywhere, and
        // a 6-neighbour fill leaves half a tree hanging in the air.
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    int32_t const nx = cx + dx, ny = cy + dy, nz = cz + dz;

                    // Never below the block that was broken: the stump
                    // stays, and a tree on a cliff edge does not reach
                    // down it.
                    if (ny < y0) continue;
                    if (ny - y0 > FELL_HEIGHT) continue;
                    int32_t const ax = nx - x0 < 0 ? x0 - nx : nx - x0;
                    int32_t const az = nz - z0 < 0 ? z0 - nz : nz - z0;
                    if (ax > FELL_RADIUS || az > FELL_RADIUS) continue;

                    if (!grown_tree(nx, ny, nz)) continue;
                    if (taken >= FELL_MAX || n >= FELL_MAX) return taken;

                    // Clear it as it is pushed, not as it is popped:
                    // that is what stops it being reached twice, and it
                    // is why no "visited" set is needed.
                    uint8_t const was = world_block(nx, ny, nz);
                    world_set(nx, ny, nz, BLK_AIR, 0);
                    drop_for(was, nx, ny, nz, s_fell_tool);
                    taken++;
                    stack[n].x = nx;
                    stack[n].y = ny;
                    stack[n].z = nz;
                    n++;
                }
            }
        }
    }
    return taken;
}

break_result_t interact_break(int32_t x, int32_t y, int32_t z, uint16_t tool_item) {
    break_result_t r = {0};
    uint8_t const  b = world_block(x, y, z);
    r.block          = b;

    if (b == BLK_AIR) return r;
    if (block_def(b)->hardness == HARDNESS_UNBREAKABLE) return r;  // bedrock, and the world's edge
    if (chunk_find(chunk_of(x), chunk_of(z)) == NULL) return r;

    int const before = item_entity_live();
    bool const placed = (world_state(x, y, z) & ST_PLACED) != 0;
    if (block_fellable(b) && !placed) {
        s_fell_tool = tool_item;
        r.felled    = interact_fell(x, y, z);
        r.was_tree  = true;
        r.ok        = true;
        r.dropped   = item_entity_live() - before;
        return r;
    }

    world_set(x, y, z, BLK_AIR, 0);
    drop_for(b, x, y, z, tool_item);
    r.felled  = 1;
    r.ok      = true;
    r.dropped = item_entity_live() - before;
    return r;
}

bool interact_place(ray_hit_t const* hit, uint8_t block, phys_body_t const* avoid) {
    if (hit == NULL || block == BLK_AIR || block >= BLK_COUNT) return false;
    // A hit with no face is the player standing inside a block: there
    // is no "in front of" to place into.
    if (hit->face == 0xFFu) return false;

    // Aimed at something a placement overwrites -- tall grass -- the
    // block goes INTO that cell, not in front of it: that is what the
    // highlighted box promised.
    bool const    into = block_replaceable(hit->block);
    int32_t const x = into ? hit->x : hit->px, y = into ? hit->y : hit->py, z = into ? hit->z : hit->pz;
    if (y < 0 || y >= CH_H) return false;
    if (chunk_find(chunk_of(x), chunk_of(z)) == NULL) return false;
    if (!block_replaceable(world_block(x, y, z))) return false;

    // Not inside the player. Only matters for solid blocks -- a torch
    // or a flower may share the cell.
    if (avoid != NULL && block_solid(block)) {
        double const hw = (double)avoid->w * 0.5;
        double const x0 = avoid->x - hw, x1 = avoid->x + hw;
        double const y0 = avoid->y, y1 = avoid->y + (double)avoid->h;
        double const z0 = avoid->z - hw, z1 = avoid->z + hw;
        bool const   over_x = fl(x0) <= x && x <= fl(x1);
        bool const   over_y = fl(y0) <= y && y <= fl(y1 - 1e-9);
        bool const   over_z = fl(z0) <= z && z <= fl(z1);
        if (over_x && over_y && over_z) return false;
    }

    world_set(x, y, z, block, ST_PLACED);
    return true;
}
