// =====================================================================
//  SynthMiner  --  what the crosshair is pointing at (see raycast.h)
// =====================================================================

#include "game/raycast.h"

#include <math.h>

#include "math/mesh.h"  // MESH_DIR_*
#include "world/chunk.h"

static inline int32_t fl(double v) {
    int32_t const i = (int32_t)v;
    return (v < (double)i) ? i - 1 : i;
}

void ray_forward(float yaw, float pitch, float* dx, float* dy, float* dz) {
    float const cp = cosf(pitch);
    *dx            = sinf(yaw) * cp;
    *dy            = -sinf(pitch);
    *dz            = cosf(yaw) * cp;
}

bool ray_pick(double ox, double oy, double oz, float dx, float dy, float dz, float max, bool want_solid,
              ray_hit_t* out) {
    if (out == NULL) return false;
    float const len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return false;
    double const d[3] = {(double)(dx / len), (double)(dy / len), (double)(dz / len)};
    double const o[3] = {ox, oy, oz};

    int32_t cell[3] = {fl(ox), fl(oy), fl(oz)};
    int     stp[3];
    double  tmax[3], tdelta[3];

    for (int a = 0; a < 3; a++) {
        if (d[a] > 0.0) {
            stp[a]    = 1;
            tdelta[a] = 1.0 / d[a];
            tmax[a]   = ((double)(cell[a] + 1) - o[a]) / d[a];
        } else if (d[a] < 0.0) {
            stp[a]    = -1;
            tdelta[a] = -1.0 / d[a];
            tmax[a]   = ((double)cell[a] - o[a]) / d[a];
        } else {
            // Parallel to this axis: it never crosses a boundary on it.
            stp[a]    = 0;
            tdelta[a] = 1e30;
            tmax[a]   = 1e30;
        }
    }

    // BLK_BARRIER IS NOT A BLOCK. It is what an unloaded chunk reads as
    // (D-14) -- solid, so the player stops at the edge of the world
    // instead of falling out of it, but there is nothing there to aim
    // at. Reporting it would draw a highlight box round a piece of fog
    // and offer to mine it. A ray that reaches one has run out of
    // world, so it stops without a hit.
#define RAY_HITS(b)                                                                                                    \
    ((b) != BLK_BARRIER &&                                                                                             \
     (want_solid ? block_solid(b) : ((b) != BLK_AIR && (block_def(b)->flags & BF_LIQUID) == 0)))

    // The starting cell counts: standing inside a block, the crosshair
    // is pointing at it.
    uint8_t b = world_block(cell[0], cell[1], cell[2]);
    if (b == BLK_BARRIER) return false;
    if (RAY_HITS(b)) {
        out->x = cell[0];
        out->y = cell[1];
        out->z = cell[2];
        out->px = cell[0];
        out->py = cell[1];
        out->pz = cell[2];
        out->block = b;
        out->face  = MESH_DIR_NONE;
        out->dist  = 0.0f;
        return true;
    }

    double t = 0.0;
    while (t <= (double)max) {
        // Cross whichever boundary is nearest.
        int axis = 0;
        if (tmax[1] < tmax[0]) axis = 1;
        if (tmax[2] < tmax[axis]) axis = 2;

        t = tmax[axis];
        if (t > (double)max) break;
        cell[axis] += stp[axis];
        tmax[axis] += tdelta[axis];

        b = world_block(cell[0], cell[1], cell[2]);
        if (b == BLK_BARRIER) return false;  // out of the loaded world
        if (!RAY_HITS(b)) continue;

        out->x     = cell[0];
        out->y     = cell[1];
        out->z     = cell[2];
        out->block = b;
        out->dist  = (float)t;
        // We came in through the face on `axis`, facing back the way we
        // came -- so the free cell is one step back along it.
        out->px = cell[0] - (axis == 0 ? stp[0] : 0);
        out->py = cell[1] - (axis == 1 ? stp[1] : 0);
        out->pz = cell[2] - (axis == 2 ? stp[2] : 0);
        out->face = (uint8_t)(axis * 2 + (stp[axis] > 0 ? 1 : 0));
        return true;
    }
    return false;
#undef RAY_HITS
}
