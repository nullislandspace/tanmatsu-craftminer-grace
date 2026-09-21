// =====================================================================
//  CraftMiner  --  breaking and placing (see interact.h)
// =====================================================================

#include "game/interact.h"

#include "world/chunk.h"

static inline int32_t fl(double v) {
    int32_t const i = (int32_t)v;
    return (v < (double)i) ? i - 1 : i;
}

// A grown tree block: fellable, and not one a player put there.
static bool grown_tree(int32_t x, int32_t y, int32_t z) {
    if (!block_fellable(world_block(x, y, z))) return false;
    return (world_state(x, y, z) & ST_PLACED) == 0;
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
    world_set(x0, y0, z0, BLK_AIR, 0);
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
                    world_set(nx, ny, nz, BLK_AIR, 0);
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

break_result_t interact_break(int32_t x, int32_t y, int32_t z) {
    break_result_t r = {0};
    uint8_t const  b = world_block(x, y, z);
    r.block          = b;

    if (b == BLK_AIR) return r;
    if (block_def(b)->hardness == HARDNESS_UNBREAKABLE) return r;  // bedrock, and the world's edge
    if (chunk_find(chunk_of(x), chunk_of(z)) == NULL) return r;

    bool const placed = (world_state(x, y, z) & ST_PLACED) != 0;
    if (block_fellable(b) && !placed) {
        r.felled   = interact_fell(x, y, z);
        r.was_tree = true;
        r.ok       = true;
        return r;
    }

    world_set(x, y, z, BLK_AIR, 0);
    r.felled = 1;
    r.ok     = true;
    return r;
}

bool interact_place(ray_hit_t const* hit, uint8_t block, phys_body_t const* avoid) {
    if (hit == NULL || block == BLK_AIR || block >= BLK_COUNT) return false;
    // A hit with no face is the player standing inside a block: there
    // is no "in front of" to place into.
    if (hit->face == 0xFFu) return false;

    int32_t const x = hit->px, y = hit->py, z = hit->pz;
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
