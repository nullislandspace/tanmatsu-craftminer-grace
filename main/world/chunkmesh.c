// =====================================================================
//  CraftMiner  --  turning a chunk into triangles (see chunkmesh.h)
// =====================================================================

#include "world/chunkmesh.h"

#include <string.h>

#include "voxel/voxel_mesh.h"

// The fine box: the chunk plus a cell of border all round.
#define FINE_W (CH_W + 2)
#define FINE_H (CH_H + 2)
#define FINE_D (CH_D + 2)
#define FINE_CELLS ((size_t)FINE_W * FINE_H * FINE_D)

// The coarse box: two blocks to a cell, so half of everything.
#define COARSE_W (CH_W / 2 + 2)
#define COARSE_H (CH_H / 2 + 2)
#define COARSE_D (CH_D / 2 + 2)
#define COARSE_CELLS ((size_t)COARSE_W * COARSE_H * COARSE_D)

size_t chunkmesh_scratch_bytes(void) {
    return FINE_CELLS > COARSE_CELLS ? FINE_CELLS : COARSE_CELLS;
}

// The mesher's indexing, from voxel_mesh.h: cell (x, y, z), each from
// -1 to w/h/d, is cells[((z + 1) * (w + 2) + (x + 1)) * (h + 2) + (y + 1)].
#define BOX(w, h, x, y, z) ((((size_t)((z) + 1) * (size_t)((w) + 2)) + (size_t)((x) + 1)) * (size_t)((h) + 2) + \
                            (size_t)((y) + 1))

static void fill_fine(uint8_t* cells, int32_t cx, int32_t cz) {
    int32_t const wx0 = cx * CH_W, wz0 = cz * CH_D;
    for (int z = -1; z <= CH_D; z++) {
        for (int x = -1; x <= CH_W; x++) {
            // Inside the chunk a whole column is contiguous in both the
            // source and the box, which is the point of the column-major
            // layout -- but the box's stride differs, so it is still a
            // loop rather than a memcpy.
            for (int y = -1; y <= CH_H; y++) {
                uint8_t b;
                if (y < 0 || y >= CH_H) {
                    b = (y < 0) ? BLK_STONE : BLK_AIR;  // bedrock below, sky above
                } else {
                    b = world_block(wx0 + x, y, wz0 + z);
                }
                cells[BOX(CH_W, CH_H, x, y, z)] = b;
            }
        }
    }
}

// One coarse cell is 2 x 2 x 2 blocks. Solid wins ties, because a hole
// that should not be there reads much worse at distance than a block
// that should not be.
static uint8_t coarse_cell(int32_t wx, int y, int32_t wz) {
    int     counts[BLK_COUNT];
    memset(counts, 0, sizeof(counts));
    int solid = 0;
    for (int dz = 0; dz < 2; dz++) {
        for (int dx = 0; dx < 2; dx++) {
            for (int dy = 0; dy < 2; dy++) {
                uint8_t const b = world_block(wx + dx, y + dy, wz + dz);
                if (b < BLK_COUNT) counts[b]++;
                if (block_solid(b)) solid++;
            }
        }
    }
    if (solid < 4) return BLK_AIR;  // mostly air: leave it open

    int best = BLK_STONE, best_n = 0;
    for (int i = 1; i < BLK_COUNT; i++) {
        if (!block_solid((uint8_t)i)) continue;
        if (counts[i] > best_n) {
            best_n = counts[i];
            best   = i;
        }
    }
    return (uint8_t)best;
}

static void fill_coarse(uint8_t* cells, int32_t cx, int32_t cz) {
    int32_t const wx0 = cx * CH_W, wz0 = cz * CH_D;
    int const     w = CH_W / 2, h = CH_H / 2, d = CH_D / 2;
    for (int z = -1; z <= d; z++) {
        for (int x = -1; x <= w; x++) {
            for (int y = -1; y <= h; y++) {
                uint8_t b;
                if (y < 0) {
                    b = BLK_STONE;
                } else if (y >= h) {
                    b = BLK_AIR;
                } else {
                    b = coarse_cell(wx0 + x * 2, y * 2, wz0 + z * 2);
                }
                cells[BOX(w, h, x, y, z)] = b;
            }
        }
    }
}

bool chunkmesh_build(int32_t cx, int32_t cz, int lod, uint8_t* scratch, mesh_t* out) {
    if (scratch == NULL || out == NULL) return false;
    if (chunk_find(cx, cz) == NULL) return false;

    mesh_init(out);

    if (lod == LOD_COARSE) {
        fill_coarse(scratch, cx, cz);
        vox_grid_t const g = {
            .cells = scratch,
            .w     = CH_W / 2,
            .h     = CH_H / 2,
            .d     = CH_D / 2,
            .x0    = 0,
            .z0    = 0,
            .step  = 2,
            // Skirts close the step where a half-resolution chunk meets
            // a full-resolution one; without them the sky shows through
            // the crack.
            .skirt = true,
        };
        voxel_mesh_build(out, &g, VOX_MESH_FAST);
    } else {
        fill_fine(scratch, cx, cz);
        vox_grid_t const g = {
            .cells = scratch,
            .w     = CH_W,
            .h     = CH_H,
            .d     = CH_D,
            .x0    = 0,
            .z0    = 0,
            .step  = 1,
            .skirt = false,
        };
        voxel_mesh_build(out, &g, lod == LOD_FANCY ? VOX_MESH_FANCY : VOX_MESH_FAST);
    }

    return !out->failed;
}
