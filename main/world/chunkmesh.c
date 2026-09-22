// =====================================================================
//  CraftMiner  --  turning a chunk into triangles (see chunkmesh.h)
// =====================================================================

#include "world/chunkmesh.h"
#include <string.h>
#include "voxel/voxel_mesh.h"
#include "world/light.h"

// The fine box: one section of the chunk plus a cell of border all
// round. The border above and below is the neighbouring section's
// bottom / top row of real blocks, which is what makes the faces at a
// seam come out the same as they would from one tall box.
#define FINE_W     (CH_W + 2)
#define FINE_H     (CH_SECT + 2)
#define FINE_D     (CH_D + 2)
#define FINE_CELLS ((size_t)FINE_W * FINE_H * FINE_D)

// The coarse box: two blocks to a cell, so half of everything.
#define COARSE_W     (CH_W / 2 + 2)
#define COARSE_H     (CH_SECT / 2 + 2)
#define COARSE_D     (CH_D / 2 + 2)
#define COARSE_CELLS ((size_t)COARSE_W * COARSE_H * COARSE_D)

// Two boxes: the blocks, then their light, laid out the same.
#define BOX_CELLS (FINE_CELLS > COARSE_CELLS ? FINE_CELLS : COARSE_CELLS)

size_t chunkmesh_scratch_bytes(void) {
    return 2 * BOX_CELLS;
}

// The mesher's indexing, from voxel_mesh.h: cell (x, y, z), each from
// -1 to w/h/d, is cells[((z + 1) * (w + 2) + (x + 1)) * (h + 2) + (y + 1)].
#define BOX(w, h, x, y, z) \
    ((((size_t)((z) + 1) * (size_t)((w) + 2)) + (size_t)((x) + 1)) * (size_t)((h) + 2) + (size_t)((y) + 1))

// The light a face merges on (F-63). Light in the merge key splits faces
// that would otherwise be one rectangle -- 46% more triangles in the
// nearest meshes, measured on a real world. Up close every level shows,
// so they keep all of them; further off, a pair of levels either side is
// invisible, and rounding to every fourth level takes lighting's cost in
// those meshes from 16% to 4%.
#define LIGHT_COARSE_MASK 0xCCu

static void fill_fine(uint8_t* cells, uint8_t* lights, int32_t cx, int32_t cz, int sect, uint8_t lmask) {
    int32_t const wx0 = cx * CH_W, wz0 = cz * CH_D;
    int const     wy0 = sect * CH_SECT;
    for (int z = -1; z <= CH_D; z++) {
        for (int x = -1; x <= CH_W; x++) {
            for (int y = -1; y <= CH_SECT; y++) {
                int const wy = wy0 + y;
                uint8_t   b;
                if (wy < 0 || wy >= CH_H) {
                    b = (wy < 0) ? BLK_STONE : BLK_AIR;  // bedrock below, sky above
                } else {
                    b = world_block(wx0 + x, wy, wz0 + z);
                }
                cells[BOX(CH_W, CH_SECT, x, y, z)]  = b;
                lights[BOX(CH_W, CH_SECT, x, y, z)] = world_light(wx0 + x, wy, wz0 + z) & lmask;
            }
        }
    }
}

// One coarse cell is 2 x 2 x 2 blocks. Solid wins ties, because a hole
// that should not be there reads much worse at distance than a block
// that should not be.
static uint8_t coarse_cell(int32_t wx, int y, int32_t wz) {
    int counts[BLK_COUNT];
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

// The light of a coarse cell: the brightest of its eight blocks, per
// channel -- a coarse face stands for the lit side of its blocks.
static uint8_t coarse_light(int32_t wx, int y, int32_t wz) {
    int sky = 0, blk = 0;
    for (int dz = 0; dz < 2; dz++) {
        for (int dx = 0; dx < 2; dx++) {
            for (int dy = 0; dy < 2; dy++) {
                uint8_t const l = world_light(wx + dx, y + dy, wz + dz);
                if (light_sky(l) > sky) sky = light_sky(l);
                if (light_block(l) > blk) blk = light_block(l);
            }
        }
    }
    return (uint8_t)(sky << 4 | blk);
}

static void fill_coarse(uint8_t* cells, uint8_t* lights, int32_t cx, int32_t cz, int sect) {
    int32_t const wx0 = cx * CH_W, wz0 = cz * CH_D;
    int const     w = CH_W / 2, h = CH_SECT / 2, d = CH_D / 2;
    int const     wy0 = sect * CH_SECT;  // in blocks; a coarse cell is 2 of them
    for (int z = -1; z <= d; z++) {
        for (int x = -1; x <= w; x++) {
            for (int y = -1; y <= h; y++) {
                int const wy = wy0 + y * 2;
                uint8_t   b;
                if (wy < 0) {
                    b = BLK_STONE;
                } else if (wy >= CH_H) {
                    b = BLK_AIR;
                } else {
                    b = coarse_cell(wx0 + x * 2, wy, wz0 + z * 2);
                }
                cells[BOX(w, h, x, y, z)]  = b;
                lights[BOX(w, h, x, y, z)] = coarse_light(wx0 + x * 2, wy, wz0 + z * 2) & LIGHT_COARSE_MASK;
            }
        }
    }
}

bool chunkmesh_build(int32_t cx, int32_t cz, int lod, int sect, uint8_t* scratch, mesh_t* out) {
    if (scratch == NULL || out == NULL) return false;
    if (sect < 0 || sect >= CH_SECT_N) return false;
    if (chunk_find(cx, cz) == NULL) return false;

    mesh_init(out);

    if (lod == LOD_COARSE) {
        fill_coarse(scratch, scratch + BOX_CELLS, cx, cz, sect);
        vox_grid_t const g = {
            .cells = scratch,
            .w     = CH_W / 2,
            .h     = CH_SECT / 2,
            .d     = CH_D / 2,
            .x0    = 0,
            // In cells, and multiplied by `step`: half as many cells,
            // each twice as tall, lands on the same block.
            .y0    = sect * (CH_SECT / 2),
            .z0    = 0,
            .step  = 2,
            // Skirts close the step where a half-resolution chunk meets
            // a full-resolution one; without them the sky shows through
            // the crack. Sides only -- a section's top and bottom meet
            // another section of the same chunk, at the same resolution.
            .skirt = true,
            .lights = scratch + BOX_CELLS,
        };
        voxel_mesh_build(out, &g, VOX_MESH_FAST);
    } else {
        fill_fine(scratch, scratch + BOX_CELLS, cx, cz, sect, lod == LOD_FANCY ? 0xFFu : LIGHT_COARSE_MASK);
        vox_grid_t const g = {
            .cells = scratch,
            .w     = CH_W,
            .h     = CH_SECT,
            .d     = CH_D,
            .x0    = 0,
            .y0    = sect * CH_SECT,
            .z0    = 0,
            .step  = 1,
            .skirt = false,
            .lights = scratch + BOX_CELLS,
        };
        voxel_mesh_build(out, &g, lod == LOD_FANCY ? VOX_MESH_FANCY : VOX_MESH_FAST);
    }

    return !out->failed;
}
