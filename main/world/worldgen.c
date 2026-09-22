// =====================================================================
//  CraftMiner  --  terrain generation (see worldgen.h)
// ---------------------------------------------------------------------
//  The shape of a world, in the order it is built:
//
//    height    two octave stacks -- a broad one that decides land from
//              water, a finer one for hills -- added, so coastlines are
//              large and the ground in between is interesting.
//    strata    bedrock, stone, a few blocks of dirt, and a surface that
//              is grass above the waterline and sand at it.
//    sea       air below CH_SEA_LEVEL becomes water.
//    caves     a 3D density field carves the stone. Kept below the
//              surface so it opens as cave mouths rather than craters.
//    ores      coal pockets in the stone.
//    plants    flowers and tall grass on the grass.
//    trees     the cross-chunk pass described in worldgen.h.
//    signs     "Kurt was here" and friends, along the Far Lands edge.
//
//  West of the world's Far Lands edge none of this runs: those chunks are
//  Beta 1.7.3's, overflowed (farlands.h).
// =====================================================================

#include "world/worldgen.h"

#include <math.h>
#include <string.h>

#include "common/rng.h"
#include "world/farlands.h"

// Seed salts. Every field gets its own, so two of them can never line
// up and print the same pattern into the world.
#define S_CONT   0x1111u
#define S_HILL   0x2222u
#define S_CAVE   0x3333u
#define S_ORE    0x4444u
#define S_TREE   0x5555u
#define S_PLANT  0x6666u
#define S_DETAIL 0x7777u
#define S_SIGN   0x8888u

// Height band. Sea level is CH_SEA_LEVEL (24) of 64, so there is room
// for caves beneath and for building above.
#define H_BASE 14.0f
#define H_CONT 20.0f  // how far the broad field lifts the land
#define H_HILL 12.0f  // how far the fine field roughens it

int worldgen_height(int32_t x, int32_t z, uint32_t seed) {
    float const fx = (float)x, fz = (float)z;

    // Broad: decides land and sea. Pushed through a smoothstep-ish
    // curve so coasts are definite rather than endless shallows.
    float c = cm_fbm2(fx, fz, 320.0f, 3, seed ^ S_CONT);
    c = c * c * (3.0f - 2.0f * c);

    // Fine: hills. Squared, so flat ground is common and peaks are not.
    float const h = cm_fbm2(fx, fz, 56.0f, 4, seed ^ S_HILL);

    float y = H_BASE + c * H_CONT + h * h * H_HILL;

    // A little per-block wobble keeps long slopes from looking milled.
    y += (cm_noise2(fx, fz, 7.0f, seed ^ S_DETAIL) - 0.5f) * 1.5f;

    int const iy = (int)floorf(y);
    return iy < 1 ? 1 : iy > CH_H - 8 ? CH_H - 8 : iy;
}

// Caves. The field is sampled at block resolution, which is exactly the
// case the donor's lattice hash could not survive (F-10).
static bool cave_at(int32_t x, int y, int32_t z, int surface, uint32_t seed) {
    // No caves in the top few blocks: they would open as holes in the
    // ground rather than as mouths in a hillside.
    if (y > surface - 4) return false;
    if (y <= CH_BEDROCK + 1) return false;

    // Two fields at right angles to each other carve tunnels where both
    // are near their midpoint -- long worms rather than round bubbles.
    float const a = cm_noise3((float)x, (float)y * 2.0f, (float)z, 22.0f, seed ^ S_CAVE);
    float const b = cm_noise3((float)x, (float)y * 2.0f, (float)z, 22.0f, seed ^ (S_CAVE + 0x99u));
    float const da = fabsf(a - 0.5f), db = fabsf(b - 0.5f);

    // Wider with depth, so the deep world is more open than the shallow.
    float const depth = (float)(surface - y) / (float)CH_H;
    float const t     = 0.055f + depth * 0.045f;
    return da < t && db < t;
}

static void fill_column(chunk_t* c, int lx, int lz, int32_t wx, int32_t wz, uint32_t seed) {
    uint8_t* id  = &c->id[CH_IDX(lx, 0, lz)];
    int const sy = worldgen_height(wx, wz, seed);

    // How deep the dirt runs here, 3..5 blocks.
    int const soil = 3 + (int)(cm_rand2(wx, wz, seed ^ S_DETAIL) * 3.0f);

    bool const beach = sy <= CH_SEA_LEVEL + 1;

    for (int y = 0; y < CH_H; y++) {
        uint8_t b = BLK_AIR;
        if (y == CH_BEDROCK) {
            b = BLK_STONE;  // the floor of the world; unbreakable stone stands in for bedrock
        } else if (y < sy - soil) {
            b = BLK_STONE;
        } else if (y < sy) {
            b = beach ? BLK_SAND : BLK_DIRT;
        } else if (y == sy) {
            b = beach ? BLK_SAND : BLK_GRASS;
        } else if (y <= CH_SEA_LEVEL) {
            b = BLK_WATER;
        }

        // Carve, but never the bedrock course and never into the sea:
        // a cave under water would flood, and there is no fluid
        // simulation to flood it with.
        if (b == BLK_STONE && y > CH_BEDROCK && cave_at(wx, y, wz, sy, seed)) {
            if (sy > CH_SEA_LEVEL + 2 || y < CH_SEA_LEVEL - 3) b = BLK_AIR;
        }

        // Ore in what stone is left.
        if (b == BLK_STONE && y < 40 && y > CH_BEDROCK) {
            if (cm_rand3(wx, y, wz, seed ^ S_ORE) > 0.988f) b = BLK_COAL_ORE;
        }

        id[y] = b;
    }

    // Grass needs air above it. A cave mouth or an overhang can leave
    // it buried, and buried grass is a texture nobody sees.
    if (!beach && sy + 1 < CH_H && id[sy] == BLK_GRASS && id[sy + 1] != BLK_AIR) id[sy] = BLK_DIRT;
}

// --- Decorations ------------------------------------------------------

// Tree candidates sit on a coarse grid, one per TREE_GRID square, so
// two trees can never grow into each other.
#define TREE_GRID   5
#define TREE_CHANCE 0.28f
#define TREE_MIN_H  4
#define TREE_MAX_H  6

// Write a cell, but only if it falls inside this chunk. This is what
// lets a tree straddle a chunk border without either chunk reading the
// other (worldgen.h).
static void stamp(chunk_t* c, int32_t wx, int y, int32_t wz, uint8_t block, bool overwrite) {
    if (y < 0 || y >= CH_H) return;
    if (chunk_of(wx) != c->cx || chunk_of(wz) != c->cz) return;
    size_t const i = CH_IDX(chunk_off(wx), y, chunk_off(wz));
    if (!overwrite && c->id[i] != BLK_AIR) return;
    c->id[i] = block;
    // Generated, so ST_PLACED stays clear -- which is what makes this a
    // tree the felling rule will take whole (Part F).
    c->st[i] = 0;
}

// One tree, rooted at (wx, wz). Called for every candidate in the 3 x 3
// neighbourhood; stamp() drops whatever lands outside this chunk.
static void place_tree(chunk_t* c, int32_t wx, int32_t wz, uint32_t seed) {
    if (cm_rand2(wx, wz, seed ^ S_TREE) > TREE_CHANCE) return;

    // A tree needs grass to stand on, and the ground under it must be
    // the generated surface -- not the inside of a hill.
    int const sy = worldgen_height(wx, wz, seed);
    if (sy <= CH_SEA_LEVEL + 1) return;  // no trees on the beach or in the water

    int const h = TREE_MIN_H + (int)(cm_rand2(wx + 1, wz - 1, seed ^ S_TREE) * (TREE_MAX_H - TREE_MIN_H + 1));
    int const top = sy + h;
    if (top + 2 >= CH_H) return;

    // Canopy first, trunk after, so the trunk wins where they meet.
    for (int dy = -2; dy <= 1; dy++) {
        int const   y = top + dy;
        int const   r = (dy <= -1) ? 2 : 1;
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                // Clip the corners of the widest layers, so the canopy
                // is round rather than a slab.
                if (r == 2 && dx * dx + dz * dz > 5) continue;
                if (r == 2 && dx * dx + dz * dz == 5 && cm_rand3(wx + dx, y, wz + dz, seed ^ S_TREE) < 0.45f) continue;
                stamp(c, wx + dx, y, wz + dz, BLK_LEAVES, false);
            }
        }
    }
    for (int y = sy; y < top; y++) stamp(c, wx, y, wz, BLK_LOG, true);
    // Dirt under the trunk: a tree on a single grass block looks wrong
    // once the grass is gone.
    stamp(c, wx, sy - 1, wz, BLK_DIRT, true);
}

static void decorate_plants(chunk_t* c, uint32_t seed) {
    for (int lz = 0; lz < CH_D; lz++) {
        for (int lx = 0; lx < CH_W; lx++) {
            int32_t const wx = c->cx * CH_W + lx, wz = c->cz * CH_D + lz;
            uint8_t*      col = &c->id[CH_IDX(lx, 0, lz)];

            int sy = -1;
            for (int y = CH_H - 2; y > 0; y--) {
                if (col[y] == BLK_GRASS) {
                    sy = y;
                    break;
                }
            }
            if (sy < 0 || col[sy + 1] != BLK_AIR) continue;

            float const r = cm_rand2(wx, wz, seed ^ S_PLANT);
            uint8_t     p = BLK_AIR;
            if (r > 0.94f) {
                p = BLK_TALL_GRASS;
            } else if (r > 0.925f) {
                p = BLK_FLOWER_RED;
            } else if (r > 0.91f) {
                p = BLK_FLOWER_YELLOW;
            }
            if (p != BLK_AIR) col[sy + 1] = p;
        }
    }
}

// Signs along the Far Lands edge (Part X): in the last ordinary chunk
// before the wall, about one chunk in four gets one, a block or three from
// the edge, standing on the ground and facing east -- the way anyone
// walking up to the wall comes. Which text it shows follows from where it
// stands (voxel_sign_text).
#define SIGN_CHANCE 0.25f

static void place_edge_sign(chunk_t* c, uint32_t seed, int32_t edge_x) {
    if (edge_x == FARLANDS_NONE || c->cx * CH_W != edge_x) return;
    if (cm_rand2(c->cx, c->cz, seed ^ S_SIGN) > SIGN_CHANCE) return;
    int const lx = (int)(cm_rand2(c->cx + 1, c->cz, seed ^ S_SIGN) * 3.0f);
    int const lz = (int)(cm_rand2(c->cx, c->cz + 1, seed ^ S_SIGN) * (float)CH_D);
    uint8_t*  col = &c->id[CH_IDX(lx, 0, lz)];
    int       y   = CH_H - 2;
    while (y > 0 && !block_solid(col[y])) y--;
    // On dry ground, with room above: not in the sea, not under a tree.
    if (y <= CH_SEA_LEVEL || col[y] == BLK_LEAVES || col[y] == BLK_LOG) return;
    if (col[y + 1] != BLK_AIR && !block_replaceable(col[y + 1])) return;
    col[y + 1] = BLK_SIGN;
}

void worldgen_chunk(chunk_t* c, uint32_t seed, int32_t farlands_x) {
    if (c == NULL) return;

    memset(c->st, 0, CH_CELLS);
    bool const far = farlands_chunk_is(c->cx, farlands_x);
    if (far) {
        farlands_generate(c, seed, farlands_x);
    } else {
        for (int lz = 0; lz < CH_D; lz++) {
            for (int lx = 0; lx < CH_W; lx++) {
                fill_column(c, lx, lz, c->cx * CH_W + lx, c->cz * CH_D + lz, seed);
            }
        }
    }

    // Every tree candidate that could reach into this chunk: the 3 x 3
    // neighbourhood of chunks, walked on the tree grid. Candidates
    // outside are dropped cell by cell inside stamp().
    int32_t const x0 = (c->cx - 1) * CH_W, x1 = (c->cx + 2) * CH_W;
    int32_t const z0 = (c->cz - 1) * CH_D, z1 = (c->cz + 2) * CH_D;
    int32_t const gx0 = (int32_t)floorf((float)x0 / TREE_GRID), gx1 = (int32_t)floorf((float)x1 / TREE_GRID);
    int32_t const gz0 = (int32_t)floorf((float)z0 / TREE_GRID), gz1 = (int32_t)floorf((float)z1 / TREE_GRID);
    for (int32_t gz = gz0; gz <= gz1; gz++) {
        for (int32_t gx = gx0; gx <= gx1; gx++) {
            // The candidate's exact spot inside its grid square, so the
            // trees are not on a visible lattice.
            int32_t const wx = gx * TREE_GRID + (int32_t)(cm_rand2(gx, gz, seed ^ 0xA1u) * TREE_GRID);
            int32_t const wz = gz * TREE_GRID + (int32_t)(cm_rand2(gx, gz, seed ^ 0xB2u) * TREE_GRID);
            // Ordinary trees grow on ordinary ground only. One rooted
            // just east of the edge still leans its canopy over the wall,
            // so a Far Lands chunk runs this too.
            if (farlands_x != FARLANDS_NONE && wx < farlands_x) continue;
            place_tree(c, wx, wz, seed);
        }
    }

    if (!far) {
        decorate_plants(c, seed);
        place_edge_sign(c, seed, farlands_x);
    }

    c->flags |= CF_GENERATED;
    chunk_resummarise(c);
}
