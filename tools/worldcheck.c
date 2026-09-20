// =====================================================================
//  CraftMiner  --  host-side check of the pure game modules
// ---------------------------------------------------------------------
//  Built and run by `make worldcheck` with the host compiler. No badge,
//  no engine, no RTOS: everything here compiles with CM_HOST and plain
//  malloc (main/common/psram.h). Exit status 0 = all checks passed.
//
//  Sections arrive with the milestones they guard
//  (claudeplans/craftminer.md, Part H):
//    registries   now      the block table's invariants
//    worldgen     step 1   determinism, cross-chunk equivalence
//    far lands    step 7   the wall, the tunnels, the ramp, the asymmetry
//    codec        step 1   RLE and region round trips, torn-write recovery
//    physics      step 3   swept AABB, no tunnelling, step-up
//    raycast      step 3   DDA against a brute-force march
//    felling      step 3   the tree rule
//    items        step 4   stacking, recipes
// =====================================================================

#include <stdio.h>
#include <string.h>

#include <math.h>
#include <stdlib.h>

#include "common/rng.h"
#include "common/tags.h"
#include "world/blocks.h"
#include "world/chunk.h"
#include "world/chunk_codec.h"
#include "world/region.h"
#include "world/vfs_compat.h"
#include "world/worldgen.h"
#include "world/chunk_worker.h"
#include "world/chunkmesh.h"
#include "world/worldstore.h"

static int s_fail = 0;

#define CHECK(cond, ...)                    \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: " __VA_ARGS__); \
            printf("\n");                   \
            s_fail++;                       \
        }                                   \
    } while (0)

// ---------------------------------------------------------------------
//  Registries
//
//  Cheap, but they catch the failure mode a table-driven design invites:
//  a row added without its materials, or an id that outgrows the byte
//  the chunk planes store it in.
// ---------------------------------------------------------------------

static void check_blocks(void) {
    printf("blocks: %d entries\n", BLK_COUNT);

    CHECK(BLK_COUNT <= 255, "BLK_COUNT is %d: a block id must fit the chunk's uint8 plane", BLK_COUNT);

    for (int i = 0; i < BLK_COUNT; i++) {
        block_def_t const* d = &BLOCKS[i];
        CHECK(d->name != NULL && d->name[0] != '\0', "block %d has no name", i);
        CHECK(d->kind <= K_TORCH, "block %s: kind %u out of range", d->name ? d->name : "?", d->kind);

        // Every block that meshes needs three real materials, or the
        // mesher indexes a texture that was never loaded.
        if (d->kind != K_AIR) {
            for (int f = 0; f < 3; f++) {
                CHECK(d->mat[f] < VM_COUNT, "block %s: face %d material %u past VM_COUNT", d->name, f, d->mat[f]);
            }
        }

        CHECK(d->drop_max >= d->drop_min, "block %s: drop_max %u below drop_min %u", d->name, d->drop_max,
              d->drop_min);
        CHECK(d->growth_max <= 7, "block %s: growth_max %u does not fit state bits 1..3", d->name, d->growth_max);
        CHECK(d->light <= 15, "block %s: light %u above 15", d->name, d->light);

        // An unbreakable block that drops something is a contradiction
        // the interaction code would have to special-case.
        if (d->hardness == HARDNESS_UNBREAKABLE) {
            CHECK(d->drop_item == ITEM_NONE, "block %s is unbreakable but drops item %u", d->name, d->drop_item);
        }
    }

    // Names are the stable id a future save format would key on, so
    // they have to be unique.
    for (int i = 0; i < BLK_COUNT; i++) {
        for (int j = i + 1; j < BLK_COUNT; j++) {
            CHECK(strcmp(BLOCKS[i].name, BLOCKS[j].name) != 0, "blocks %d and %d share the name \"%s\"", i, j,
                  BLOCKS[i].name);
        }
    }

    // The specific invariants the rest of the code relies on.
    CHECK(BLOCKS[BLK_AIR].kind == K_AIR, "air must mesh as K_AIR");
    CHECK(block_replaceable(BLK_AIR), "air must be replaceable, or nothing can be placed");
    CHECK(!block_solid(BLK_AIR), "air must not be solid");
    CHECK(block_solid(BLK_BARRIER), "the barrier must be solid (D-14): the player stands on the world's edge");
    CHECK(BLOCKS[BLK_BARRIER].hardness == HARDNESS_UNBREAKABLE, "the barrier must be unbreakable");
    CHECK(block_fellable(BLK_LOG) && block_fellable(BLK_LEAVES), "logs and leaves carry the felling rule (Part F)");
    CHECK((BLOCKS[BLK_LEAVES].flags & BF_SEE_SELF) != 0, "leaves show their faces against other leaves");
    CHECK((BLOCKS[BLK_GLASS].flags & BF_SEE_SELF) == 0, "glass hides glass");

    // block_def() must be total: a corrupt save can hand it anything.
    CHECK(block_def(255) == &BLOCKS[BLK_BARRIER],
          "block_def(255) must fall back to the barrier, not read past the table");
    CHECK(block_def(BLK_COUNT) == &BLOCKS[BLK_BARRIER], "block_def(BLK_COUNT) must fall back to the barrier");
}

// ---------------------------------------------------------------------
//  Chunks
//
//  The coordinate maths is the classic place a voxel game goes wrong:
//  x = -1 belongs to chunk -1 at offset 15, not to chunk 0 at offset
//  -1. A plain division rounds towards zero and gets it wrong, so
//  chunk_of / chunk_off shift and mask -- and that is worth pinning
//  down before anything is built on top of it.
// ---------------------------------------------------------------------

static void check_chunk_coords(void) {
    printf("chunk coords\n");

    struct {
        int32_t w;
        int32_t cx;
        int     off;
    } const CASES[] = {
        {0, 0, 0},      {1, 0, 1},       {15, 0, 15},      {16, 1, 0},      {31, 1, 15},
        {-1, -1, 15},   {-16, -1, 0},    {-17, -2, 15},    {-32, -2, 0},
        // The Far Lands are at -100000: the maths has to hold there too.
        {-100000, -6250, 0}, {-99999, -6250, 1}, {-100001, -6251, 15},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        CHECK(chunk_of(CASES[i].w) == CASES[i].cx, "chunk_of(%d) = %d, expected %d", CASES[i].w,
              chunk_of(CASES[i].w), CASES[i].cx);
        CHECK(chunk_off(CASES[i].w) == CASES[i].off, "chunk_off(%d) = %d, expected %d", CASES[i].w,
              chunk_off(CASES[i].w), CASES[i].off);
    }

    // The two must compose back into the original coordinate, over a
    // range that crosses zero in both directions.
    for (int32_t w = -5000; w <= 5000; w++) {
        int32_t const back = chunk_of(w) * CH_W + chunk_off(w);
        CHECK(back == w, "chunk_of/chunk_off do not compose at %d (got %d)", w, back);
        if (s_fail) break;
    }

    // CH_IDX must be a bijection onto [0, CH_CELLS).
    CHECK(CH_IDX(0, 0, 0) == 0, "CH_IDX(0,0,0) is not 0");
    CHECK(CH_IDX(CH_W - 1, CH_H - 1, CH_D - 1) == (size_t)CH_CELLS - 1, "CH_IDX does not end at CH_CELLS-1");
    // A column is contiguous -- this is what lets the mesher read it
    // and what CH_IDX exists to guarantee.
    CHECK(CH_IDX(3, 1, 5) == CH_IDX(3, 0, 5) + 1, "a column is not contiguous in y");
}

static void check_chunk_state_bits(void) {
    printf("chunk state byte\n");

    // The block's own 7-bit field must round-trip across its whole
    // range without touching ST_PLACED, which is the one bit with a
    // meaning that does not belong to the block type.
    for (uint32_t d = 0; d <= ST_DATA_MAX; d++) {
        uint8_t const st = st_with_data(ST_PLACED, (uint8_t)d);
        CHECK(st_data(st) == d, "block data %u did not round-trip (got %u)", d, st_data(st));
        CHECK((st & ST_PLACED) != 0, "block data %u clobbered ST_PLACED", d);

        uint8_t const st0 = st_with_data(0, (uint8_t)d);
        CHECK((st0 & ST_PLACED) == 0, "block data %u invented an ST_PLACED bit", d);
    }

    // Growth is that same field under a name the crop code can read.
    CHECK(st_growth(st_with_growth(0, 7)) == 7, "growth did not round-trip");
    CHECK(ST_DATA_MAX >= 15, "the block data field is too small for a redstone power level");

    // The two fields must not overlap, and must cover the byte.
    CHECK((ST_PLACED & ST_DATA_MASK) == 0, "ST_PLACED overlaps the block data field");
    CHECK((ST_PLACED | ST_DATA_MASK) == 0xFFu, "the state byte has bits belonging to nobody");
}

static void check_chunk_store(void) {
    printf("chunk store: %d slots, %zu bytes\n", CH_SLOT_COUNT, chunk_store_bytes());

    // Residency: claim a chunk, fill it, read it back through world_*.
    chunk_t* c = chunk_claim(-7, 3);
    CHECK(c != NULL, "could not claim a free slot");
    if (c == NULL) return;
    c->cstate = CS_READY;

    int32_t const wx = -7 * CH_W + 2, wz = 3 * CH_W + 9;
    world_set(wx, 10, wz, BLK_STONE, 0);
    CHECK(world_block(wx, 10, wz) == BLK_STONE, "a block written did not read back");
    CHECK(world_block(wx, 11, wz) == BLK_AIR, "a neighbouring cell was disturbed");
    CHECK((c->flags & CF_EDITED) != 0, "a write did not mark the chunk edited");

    // The state byte rides along.
    world_set(wx, 12, wz, BLK_LOG, ST_PLACED);
    CHECK(world_state(wx, 12, wz) == ST_PLACED, "the state byte did not read back");

    // The column summary tracks writes both ways.
    CHECK(world_ground(wx, wz) == 13, "world_ground = %d, expected 13", world_ground(wx, wz));
    world_set(wx, 12, wz, BLK_AIR, 0);
    CHECK(world_ground(wx, wz) == 11, "world_ground after a break = %d, expected 11", world_ground(wx, wz));

    // Anything not resident is the barrier, so callers never need a
    // "might be missing" branch (D-14).
    CHECK(world_block(900000, 10, 900000) == BLK_BARRIER, "a non-resident chunk did not read as the barrier");
    CHECK(world_solid_at(900000, 10, 900000), "the barrier must be solid");
    // Above the world is air, below it is not passable.
    CHECK(world_block(wx, CH_H, wz) == BLK_AIR, "above the world must be air");
    CHECK(world_block(wx, -1, wz) == BLK_BARRIER, "below bedrock must not be passable");

    // A slot holding a different chunk reads as absent, not as the
    // wrong terrain: this is what makes the ring safe (D-13).
    CHECK(chunk_find(-7 + CH_RING, 3) == NULL, "an aliasing chunk coordinate returned the wrong chunk");
    CHECK(chunk_slot(-7, 3) == chunk_slot(-7 + CH_RING, 3), "the test's premise is wrong: those should alias");

    // An edited chunk must not be silently evicted -- its edits are not
    // on disk yet.
    CHECK(chunk_claim(-7 + CH_RING, 3) == NULL, "an unsaved edited chunk was evicted");
    c->flags &= (uint8_t)~CF_EDITED;
    CHECK(chunk_claim(-7 + CH_RING, 3) != NULL, "a saved chunk could not be evicted");
}

// ---------------------------------------------------------------------
//  Hashing and noise
//
//  The donor folded a lattice point into one int key,
//  hash01(ix*7919 + iz*104729). That aliases -- (ix, iz) and
//  (ix + 104729, iz - 7919) hash the same -- and overflows int32 beyond
//  |iz| = 20505, which the block-resolution density field for caves and
//  the Far Lands would reach in ordinary play (F-10). These checks are
//  what stop it coming back.
// ---------------------------------------------------------------------

static int cmp_u32(void const* a, void const* b) {
    uint32_t const x = *(uint32_t const*)a, y = *(uint32_t const*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void check_rng(void) {
    printf("hashing and noise\n");

    // Determinism: the same point always gives the same value.
    CHECK(cm_hash2(3, -9, 42) == cm_hash2(3, -9, 42), "cm_hash2 is not deterministic");
    CHECK(cm_hash3(3, 4, -9, 42) == cm_hash3(3, 4, -9, 42), "cm_hash3 is not deterministic");

    // A different seed gives a different world.
    CHECK(cm_hash2(3, -9, 42) != cm_hash2(3, -9, 43), "the seed does not change cm_hash2");

    // Injectivity where it matters: hash a 200 x 200 lattice block
    // AROUND THE FAR LANDS and count collisions. The old formulation
    // fails this twice over -- it aliases, and the multiply overflows.
    enum { N = 200 };
    static uint32_t h[N * N];
    int             n = 0;
    for (int32_t dz = 0; dz < N; dz++) {
        for (int32_t dx = 0; dx < N; dx++) h[n++] = cm_hash2(-100000 + dx, -50 + dz, 12345u);
    }
    qsort(h, (size_t)n, sizeof(h[0]), cmp_u32);
    int dup = 0;
    for (int i = 1; i < n; i++) dup += (h[i] == h[i - 1]);
    // 40000 draws from 2^32 collide about 0.19 times by chance; more
    // than a handful means the map is not injective.
    printf("  %d lattice points near x=-100000, %d hash collisions\n", n, dup);
    CHECK(dup <= 3, "%d hash collisions in %d points: the lattice hash aliases (F-10)", dup, n);

    // The donor's exact aliasing pair: 104729 in x and -7919 in z left
    // its key unchanged, so those two points grew identical terrain.
    CHECK(cm_hash2(0, 0, 7u) != cm_hash2(104729, -7919, 7u), "cm_hash2 aliases the donor's way");
    CHECK(cm_hash3(0, 5, 0, 7u) != cm_hash3(104729, 5, -7919, 7u), "cm_hash3 aliases the donor's way");

    // The donor's overflow range: |iz| = 20505 is where iz * 104729
    // left int32. A block-resolution density field reaches that in
    // ordinary play, so hash and noise must stay healthy well past it.
    for (int32_t z = 20000; z <= 21000; z += 250) {
        CHECK(cm_hash3(7, 30, z, 3u) != cm_hash3(7, 30, z + 1, 3u), "cm_hash3 degenerates around z = %d", z);
    }
    {
        float lo3 = 1.0f, hi3 = 0.0f;
        for (int i = 0; i < 4000; i++) {
            // scale 1: the lattice index IS the block coordinate, which
            // is the case the donor could not survive.
            float const v = cm_noise3(9.5f, 30.25f, 20000.0f + (float)i * 0.5f, 1.0f, 3u);
            if (v < lo3) lo3 = v;
            if (v > hi3) hi3 = v;
        }
        printf("  noise3 at block resolution near z=20000: range %.3f..%.3f\n", lo3, hi3);
        CHECK(hi3 - lo3 > 0.5f, "cm_noise3 went degenerate past the donor's overflow point (%g..%g)", lo3, hi3);
    }

    // Noise stays in range, including far from the origin, and is
    // continuous (neighbouring samples cannot jump by much).
    float lo = 1.0f, hi = 0.0f, maxstep = 0.0f, prev = 0.0f;
    for (int i = 0; i < 20000; i++) {
        float const x = -100000.0f + (float)i * 0.25f;
        float const v = cm_noise2(x, 17.0f, 24.0f, 99u);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (i > 0) {
            float const d = fabsf(v - prev);
            if (d > maxstep) maxstep = d;
        }
        prev = v;
    }
    printf("  noise2 near x=-100000: range %.3f..%.3f, largest step over 0.25 blocks %.4f\n", lo, hi, maxstep);
    CHECK(lo >= 0.0f && hi < 1.0f, "cm_noise2 left [0,1): %g..%g", lo, hi);
    CHECK(hi - lo > 0.5f, "cm_noise2 barely varies near the Far Lands (%g..%g): it has gone degenerate", lo, hi);
    CHECK(maxstep < 0.08f, "cm_noise2 is not continuous near the Far Lands (step %g)", maxstep);

    // fbm stays in range too.
    lo = 1.0f;
    hi = 0.0f;
    for (int i = 0; i < 5000; i++) {
        float const v = cm_fbm2((float)i * 0.7f, (float)i * -0.3f, 64.0f, 4, 5u);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    CHECK(lo >= 0.0f && hi < 1.0f, "cm_fbm2 left [0,1): %g..%g", lo, hi);

    // The stream never sticks and never repeats too soon.
    cm_rng_t r;
    cm_rng_seed(&r, 0);
    uint32_t const first = cm_rng_u32(&r);
    int            same  = 0;
    for (int i = 0; i < 1000; i++) same += (cm_rng_u32(&r) == first);
    CHECK(same == 0, "cm_rng repeated its first value %d times in 1000 draws", same);
}

// ---------------------------------------------------------------------
//  World generation
//
//  Two properties carry the whole design:
//
//    determinism          a seed and a coordinate always give the same
//                         block, so a world can be regenerated instead
//                         of stored, for ever.
//    load-order independence
//                         a chunk generated alone equals the same chunk
//                         generated with its neighbours around it. This
//                         is the one that breaks silently: the moment a
//                         decoration reads a neighbour chunk, terrain
//                         starts depending on which way the player
//                         walked into it, and a replay stops matching.
// ---------------------------------------------------------------------

#define GEN_SEED 0xC0FFEEu

// Generate into a standalone chunk, outside the ring store, so the
// checks can hold several at once and compare them.
static void gen_into(chunk_t* c, uint8_t* id, uint8_t* st, int32_t cx, int32_t cz, uint32_t seed) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->st = st;
    c->cx = cx;
    c->cz = cz;
    worldgen_chunk(c, seed);
}

static void check_worldgen(void) {
    printf("worldgen\n");

    static uint8_t ia[CH_CELLS], sa[CH_CELLS], ib[CH_CELLS], sb[CH_CELLS];
    chunk_t        a, b;

    // Determinism, including out at the Far Lands and far from the
    // origin in z, where the donor's hash could not have held (F-10).
    struct {
        int32_t cx, cz;
    } const WHERE[] = {{0, 0}, {1, -3}, {-40, 17}, {6250, -6250}, {-6250, 0}, {0, 1400}, {-1, -1}};
    for (size_t i = 0; i < sizeof(WHERE) / sizeof(WHERE[0]); i++) {
        gen_into(&a, ia, sa, WHERE[i].cx, WHERE[i].cz, GEN_SEED);
        gen_into(&b, ib, sb, WHERE[i].cx, WHERE[i].cz, GEN_SEED);
        CHECK(memcmp(ia, ib, CH_CELLS) == 0, "chunk (%d,%d) generated differently twice", WHERE[i].cx, WHERE[i].cz);
        CHECK(memcmp(sa, sb, CH_CELLS) == 0, "chunk (%d,%d) state differed between runs", WHERE[i].cx, WHERE[i].cz);
    }

    // A different seed is a different world.
    gen_into(&a, ia, sa, 0, 0, GEN_SEED);
    gen_into(&b, ib, sb, 0, 0, GEN_SEED + 1);
    CHECK(memcmp(ia, ib, CH_CELLS) != 0, "two seeds produced the identical chunk");

    // Nothing generated may carry ST_PLACED: that bit means "a player
    // put this here", and the felling rule turns on it (Part F).
    int placed = 0;
    for (int i = 0; i < CH_CELLS; i++) placed += (sa[i] & ST_PLACED) != 0;
    CHECK(placed == 0, "%d generated cells carry ST_PLACED", placed);

    // Load-order independence. Generate a 3 x 3 block of chunks one at
    // a time, then generate the middle one again on its own: it must
    // come out identical. A tree rooted in a neighbour reaches into it,
    // so this is a real test of the stamp() approach, not a tautology.
    for (int32_t cz = -1; cz <= 1; cz++) {
        for (int32_t cx = -1; cx <= 1; cx++) {
            gen_into(&a, ia, sa, cx, cz, GEN_SEED);  // the neighbours, discarded
        }
    }
    gen_into(&a, ia, sa, 0, 0, GEN_SEED);
    gen_into(&b, ib, sb, 0, 0, GEN_SEED);
    CHECK(memcmp(ia, ib, CH_CELLS) == 0, "a chunk differed when generated after its neighbours");

    // And the decoration really does cross borders -- otherwise the
    // check above proves nothing. Count leaf cells touching an edge.
    int edge_leaves = 0, total_leaves = 0, logs = 0;
    for (int lz = 0; lz < CH_D; lz++) {
        for (int lx = 0; lx < CH_W; lx++) {
            for (int y = 0; y < CH_H; y++) {
                uint8_t const t = ia[CH_IDX(lx, y, lz)];
                if (t == BLK_LEAVES) {
                    total_leaves++;
                    if (lx == 0 || lx == CH_W - 1 || lz == 0 || lz == CH_D - 1) edge_leaves++;
                }
                logs += (t == BLK_LOG);
            }
        }
    }
    printf("  chunk (0,0): %d logs, %d leaves (%d on an edge)\n", logs, total_leaves, edge_leaves);
    CHECK(total_leaves > 0 && logs > 0, "chunk (0,0) grew no trees: the test cannot say anything");
    CHECK(edge_leaves > 0, "no decoration reaches a chunk edge, so load-order independence is untested here");

    // Structure: bedrock everywhere, nothing above the roof, water only
    // up to sea level, and a surface that is reachable.
    int no_floor = 0, water_above_sea = 0, solid_roof = 0;
    for (int lz = 0; lz < CH_D; lz++) {
        for (int lx = 0; lx < CH_W; lx++) {
            uint8_t const* col = &ia[CH_IDX(lx, 0, lz)];
            no_floor += (col[CH_BEDROCK] == BLK_AIR);
            solid_roof += (col[CH_H - 1] != BLK_AIR);
            for (int y = CH_SEA_LEVEL + 1; y < CH_H; y++) water_above_sea += (col[y] == BLK_WATER);
        }
    }
    CHECK(no_floor == 0, "%d columns have no floor at y = 0: the player could fall out of the world", no_floor);
    CHECK(water_above_sea == 0, "%d water cells sit above sea level", water_above_sea);
    CHECK(solid_roof == 0, "%d columns reach the top of the world", solid_roof);

    // worldgen_height agrees with what was actually written, for the
    // columns a tree or a cave has not rearranged.
    int checked = 0, mismatch = 0;
    for (int lz = 0; lz < CH_D; lz++) {
        for (int lx = 0; lx < CH_W; lx++) {
            int32_t const wx = lx, wz = lz;
            int const     h   = worldgen_height(wx, wz, GEN_SEED);
            uint8_t const at  = ia[CH_IDX(lx, h, lz)];
            if (at == BLK_GRASS || at == BLK_SAND) {
                checked++;
                if (ia[CH_IDX(lx, h + 1, lz)] != BLK_AIR && ia[CH_IDX(lx, h + 1, lz)] != BLK_WATER &&
                    ia[CH_IDX(lx, h + 1, lz)] != BLK_TALL_GRASS && ia[CH_IDX(lx, h + 1, lz)] != BLK_FLOWER_RED &&
                    ia[CH_IDX(lx, h + 1, lz)] != BLK_FLOWER_YELLOW && ia[CH_IDX(lx, h + 1, lz)] != BLK_LOG &&
                    ia[CH_IDX(lx, h + 1, lz)] != BLK_LEAVES) {
                    mismatch++;
                }
            }
        }
    }
    printf("  %d surface columns checked against worldgen_height, %d odd\n", checked, mismatch);
    CHECK(checked > 100, "too few surface columns to say anything (%d)", checked);
    CHECK(mismatch == 0, "%d columns have something unexpected on the surface", mismatch);

    // A spread of chunks: the world must contain caves, ore, water and
    // dry land, or the generator is producing one boring thing.
    int air_below = 0, ore = 0, water = 0, grass = 0;
    for (int32_t cz = 0; cz < 4; cz++) {
        for (int32_t cx = 0; cx < 4; cx++) {
            gen_into(&a, ia, sa, cx * 7 - 13, cz * 7 - 13, GEN_SEED);
            for (int i = 0; i < CH_CELLS; i++) {
                int const y = (int)(i % CH_H);
                if (y > CH_BEDROCK && y < 18 && ia[i] == BLK_AIR) air_below++;
                ore += (ia[i] == BLK_COAL_ORE);
                water += (ia[i] == BLK_WATER);
                grass += (ia[i] == BLK_GRASS);
            }
        }
    }
    printf("  16 chunks: %d deep air (caves), %d coal, %d water, %d grass\n", air_below, ore, water, grass);
    CHECK(air_below > 0, "no caves anywhere in 16 chunks");
    CHECK(ore > 0, "no coal anywhere in 16 chunks");
    CHECK(grass > 0, "no grass anywhere in 16 chunks");
}

// ---------------------------------------------------------------------
//  Persistence
//
//  The properties that matter, in order of how badly they fail:
//
//    round trip     a chunk written and read back is the same chunk,
//                   including the ST_PLACED bits the felling rule needs.
//    corruption     a damaged region loses chunks, never the world, and
//                   never returns terrain that is subtly wrong.
//    torn writes    a power cut mid-save leaves the previous save
//                   readable, because the writer updates the STALE
//                   directory copy and the reader picks by serial.
//    compaction     preserves every chunk while shrinking the file.
// ---------------------------------------------------------------------

#define TEST_DIR "build/host/worldtest"

static uint8_t g_ia[CH_CELLS], g_sa[CH_CELLS], g_ib[CH_CELLS], g_sb[CH_CELLS];

static void fill_chunk(chunk_t* c, uint8_t* id, uint8_t* st, int32_t cx, int32_t cz, uint32_t seed) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->st = st;
    c->cx = cx;
    c->cz = cz;
    worldgen_chunk(c, seed);
}

static void check_codec(void) {
    printf("chunk codec\n");

    chunk_t a, b;
    fill_chunk(&a, g_ia, g_sa, 3, -5, GEN_SEED);

    // A generated chunk carries no state bits, so plant some: the
    // ST_PLACED bit is what the felling rule turns on, and losing it in
    // a save would be a bug nobody notices until a tree misbehaves.
    for (int i = 0; i < CH_CELLS; i += 97) g_sa[i] = ST_PLACED;
    g_sa[CH_IDX(2, 30, 4)] = st_with_growth(ST_PLACED, 5);

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    size_t const   n = chunk_encode(&a, NULL, 0, buf, sizeof(buf));
    printf("  a generated chunk packs to %zu bytes (raw would be %d)\n", n, 2 * CH_CELLS);
    CHECK(n > 0, "chunk_encode failed");
    CHECK(n < (size_t)(2 * CH_CELLS), "the encoding (%zu) is no smaller than raw (%d)", n, 2 * CH_CELLS);

    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = a.cx;
    b.cz = a.cz;
    CHECK(chunk_decode(buf, n, &b, NULL), "chunk_decode failed");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "the block plane did not survive the round trip");
    CHECK(memcmp(g_sa, g_sb, CH_CELLS) == 0, "the state plane did not survive the round trip");
    CHECK(st_growth(g_sb[CH_IDX(2, 30, 4)]) == 5, "a growth stage did not survive the round trip");

    // The worst case the codec has to survive: no runs at all, where RLE
    // would double the size. It must fall back to raw, not fail.
    for (int i = 0; i < CH_CELLS; i++) g_ia[i] = (uint8_t)((i & 1) ? BLK_STONE : BLK_DIRT);
    size_t const worst = chunk_encode(&a, NULL, 0, buf, sizeof(buf));
    CHECK(worst > 0, "chunk_encode gave up on an incompressible chunk instead of storing it raw");
    CHECK(worst <= CHUNK_PAYLOAD_MAX, "an incompressible chunk (%zu) exceeded CHUNK_PAYLOAD_MAX (%zu)", worst,
          (size_t)CHUNK_PAYLOAD_MAX);
    CHECK(chunk_decode(buf, worst, &b, NULL), "an incompressible chunk did not decode");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "an incompressible chunk did not round-trip");

    // Truncation must fail cleanly, and leave nothing half-written.
    CHECK(!chunk_decode(buf, worst / 2, &b, NULL), "a truncated payload decoded anyway");
    int nonzero = 0;
    for (int i = 0; i < CH_CELLS; i++) nonzero += (g_ib[i] != BLK_AIR);
    CHECK(nonzero == 0, "a failed decode left %d cells behind instead of clearing", nonzero);
    CHECK(!chunk_decode(buf, 0, &b, NULL), "an empty payload decoded anyway");
}

static void check_region(void) {
    printf("region files\n");
    CHECK(cm_mkdir_p(TEST_DIR), "could not create " TEST_DIR);

    // Clear anything a previous run left.
    for (int32_t rz = -1; rz <= 1; rz++) {
        for (int32_t rx = -1; rx <= 1; rx++) {
            char path[192];
            region_path(path, sizeof(path), TEST_DIR, rx, rz);
            cm_remove(path);
        }
    }

    // Region coordinate maths, negatives included.
    CHECK(region_of(0) == 0 && region_local(0) == 0, "region_of/local wrong at 0");
    CHECK(region_of(7) == 0 && region_local(7) == 7, "region_of/local wrong at 7");
    CHECK(region_of(8) == 1 && region_local(8) == 0, "region_of/local wrong at 8");
    CHECK(region_of(-1) == -1 && region_local(-1) == 7, "region_of/local wrong at -1");
    CHECK(region_of(-8) == -1 && region_local(-8) == 0, "region_of/local wrong at -8");
    CHECK(region_of(-9) == -2 && region_local(-9) == 7, "region_of/local wrong at -9");

    chunk_t a, b;

    // Absent is not an error.
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 2;
    b.cz = 2;
    CHECK(region_read_chunk(TEST_DIR, &b, NULL) == 0, "reading from a region that does not exist was an error");

    // Write a whole region's worth, spanning both signs, then read back.
    int written = 0;
    for (int32_t cz = -8; cz < 8; cz += 3) {
        for (int32_t cx = -8; cx < 8; cx += 3) {
            fill_chunk(&a, g_ia, g_sa, cx, cz, GEN_SEED);
            g_sa[CH_IDX(1, 20, 1)] = ST_PLACED;  // something to recognise it by
            if (!region_write_chunk(TEST_DIR, &a)) {
                CHECK(false, "region_write_chunk failed at (%d,%d)", cx, cz);
                return;
            }
            written++;
        }
    }
    int read_back = 0, mismatch = 0;
    for (int32_t cz = -8; cz < 8; cz += 3) {
        for (int32_t cx = -8; cx < 8; cx += 3) {
            fill_chunk(&a, g_ia, g_sa, cx, cz, GEN_SEED);
            g_sa[CH_IDX(1, 20, 1)] = ST_PLACED;
            memset(&b, 0, sizeof(b));
            b.id = g_ib;
            b.st = g_sb;
            b.cx = cx;
            b.cz = cz;
            int const r = region_read_chunk(TEST_DIR, &b, NULL);
            if (r != 1) {
                mismatch++;
                continue;
            }
            read_back++;
            if (memcmp(g_ia, g_ib, CH_CELLS) != 0 || memcmp(g_sa, g_sb, CH_CELLS) != 0) mismatch++;
        }
    }
    printf("  wrote %d chunks across 4 regions, read back %d\n", written, read_back);
    CHECK(mismatch == 0, "%d chunks did not come back as written", mismatch);
    CHECK(read_back == written, "wrote %d chunks but read back %d", written, read_back);

    // Summaries must be rebuilt by the load, not left at zero: the
    // renderer's AABB and world_ground() both read them.
    CHECK(b.top[1 * CH_W + 1] > 0, "chunk_decode did not rebuild the column summaries");

    // Rewriting the same chunk must not corrupt its neighbours, and must
    // win over the old copy.
    fill_chunk(&a, g_ia, g_sa, 2, 2, GEN_SEED);
    a.id[CH_IDX(5, 40, 5)] = BLK_GLASS;
    a.st[CH_IDX(5, 40, 5)] = ST_PLACED;
    CHECK(region_write_chunk(TEST_DIR, &a), "rewrite failed");
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 2;
    b.cz = 2;
    CHECK(region_read_chunk(TEST_DIR, &b, NULL) == 1, "could not read the rewritten chunk");
    CHECK(g_ib[CH_IDX(5, 40, 5)] == BLK_GLASS, "the rewrite did not take");
    CHECK((g_sb[CH_IDX(5, 40, 5)] & ST_PLACED) != 0, "the rewrite lost ST_PLACED");
}

// Damage a region on purpose and check what survives.
static void poke(char const* path, long off, int len, uint8_t with) {
    FILE* f = fopen(path, "r+b");
    if (f == NULL) {
        CHECK(false, "cannot open %s to damage it", path);
        return;
    }
    fseek(f, off, SEEK_SET);
    for (int i = 0; i < len; i++) fwrite(&with, 1, 1, f);
    fclose(f);
}

static void check_region_damage(void) {
    printf("region damage\n");
    char path[192];
    region_path(path, sizeof(path), TEST_DIR, 0, 0);

    chunk_t a, b;
    fill_chunk(&a, g_ia, g_sa, 1, 1, GEN_SEED);
    CHECK(region_write_chunk(TEST_DIR, &a), "setup write failed");

    // Directory copy A sits at 0x040. Destroy it: copy B must carry the
    // region, because the writer alternates and both are kept current
    // within one save of each other.
    poke(path, 0x040, 64, 0xAB);
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 1;
    b.cz = 1;
    int const r = region_read_chunk(TEST_DIR, &b, NULL);
    printf("  directory copy A destroyed: read returned %d\n", r);
    CHECK(r == 1, "losing one directory copy lost the chunk (the dual copy is not working)");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "the chunk came back wrong after losing a directory copy");

    // Now destroy the other copy too: the region is beyond saving and
    // must say so, rather than hand back rubbish.
    poke(path, 0x248, 64, 0xCD);
    int const r2 = region_read_chunk(TEST_DIR, &b, NULL);
    printf("  both directory copies destroyed: read returned %d\n", r2);
    CHECK(r2 == -1, "a region with no valid directory did not report an error (got %d)", r2);

    // A damaged payload must read as "no chunk", so it is regenerated,
    // not as broken terrain.
    char path2[192];
    region_path(path2, sizeof(path2), TEST_DIR, -1, -1);
    cm_remove(path2);
    fill_chunk(&a, g_ia, g_sa, -8, -8, GEN_SEED);
    CHECK(region_write_chunk(TEST_DIR, &a), "setup write failed");
    poke(path2, 0x480 + 8, 200, 0xEE);
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = -8;
    b.cz = -8;
    int const r3 = region_read_chunk(TEST_DIR, &b, NULL);
    printf("  payload corrupted: read returned %d\n", r3);
    CHECK(r3 == 0 || r3 == -1, "a corrupt payload decoded as if it were fine");
    if (r3 == 0) {
        int nonzero = 0;
        for (int i = 0; i < CH_CELLS; i++) nonzero += (g_ib[i] != BLK_AIR);
        CHECK(nonzero == 0, "a rejected payload still left %d cells behind", nonzero);
    }
}

// The durability claim, tested rather than asserted: a save interrupted
// part way must leave the PREVIOUS save readable.
//
// The writer appends the payload, then rewrites whichever directory copy
// is stale with serial+1, which makes it current. A crash during that
// directory write leaves it garbage -- so destroying the current copy is
// exactly what an interrupted save looks like from the reader's side.
// The region is fresh, so the sequence of serials is known: create
// writes A=1, B=0; the first save writes B=2; the second writes A=3.
// After two saves the current copy is A, and behind it sits the first
// save, intact.
static void check_torn_write(void) {
    printf("torn write\n");
    char path[192];
    region_path(path, sizeof(path), TEST_DIR, 1, 0);
    cm_remove(path);

    chunk_t a, b;
    size_t const marker = CH_IDX(6, 35, 6);

    fill_chunk(&a, g_ia, g_sa, 8, 0, GEN_SEED);
    g_ia[marker] = BLK_GLASS;                       /* save 1 */
    CHECK(region_write_chunk(TEST_DIR, &a), "first save failed");

    fill_chunk(&a, g_ia, g_sa, 8, 0, GEN_SEED);
    g_ia[marker] = BLK_PLANKS;                      /* save 2 */
    CHECK(region_write_chunk(TEST_DIR, &a), "second save failed");

    // Sanity: save 2 is what a healthy region returns.
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 8;
    b.cz = 0;
    CHECK(region_read_chunk(TEST_DIR, &b, NULL) == 1, "the healthy region did not read");
    CHECK(g_ib[marker] == BLK_PLANKS, "the second save is not the one that reads back");

    // Now interrupt save 2: destroy the directory copy it wrote.
    poke(path, 0x040, 520, 0x5A);

    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 8;
    b.cz = 0;
    int const r = region_read_chunk(TEST_DIR, &b, NULL);
    printf("  save interrupted: read returned %d, marker is %s\n", r,
           g_ib[marker] == BLK_GLASS    ? "the FIRST save (recovered)"
           : g_ib[marker] == BLK_PLANKS ? "the second save"
                                        : "neither");
    CHECK(r == 1, "an interrupted save lost the chunk entirely (got %d)", r);
    CHECK(g_ib[marker] == BLK_GLASS,
          "an interrupted save did not fall back to the previous one -- the dual directory is not doing its job");

    // And the region must still be writable afterwards: recovery is not
    // much use if the next save fails.
    fill_chunk(&a, g_ia, g_sa, 8, 0, GEN_SEED);
    g_ia[marker] = BLK_COBBLE;
    CHECK(region_write_chunk(TEST_DIR, &a), "could not write to a region after recovering from a torn save");
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    b.cx = 8;
    b.cz = 0;
    CHECK(region_read_chunk(TEST_DIR, &b, NULL) == 1, "the region did not read after a post-recovery save");
    CHECK(g_ib[marker] == BLK_COBBLE, "the post-recovery save did not take");
}

static void check_compaction(void) {
    printf("compaction\n");
    char path[192];
    region_path(path, sizeof(path), TEST_DIR, 0, 1);
    cm_remove(path);

    chunk_t a;
    // Rewrite the same few chunks many times, so the file fills with
    // dead copies.
    for (int pass = 0; pass < 12; pass++) {
        for (int32_t cz = 8; cz < 11; cz++) {
            for (int32_t cx = 0; cx < 3; cx++) {
                fill_chunk(&a, g_ia, g_sa, cx, cz, GEN_SEED);
                g_ia[CH_IDX(0, 40, 0)] = (uint8_t)(pass % 2 ? BLK_GLASS : BLK_PLANKS);
                CHECK(region_write_chunk(TEST_DIR, &a), "compaction setup write failed");
            }
        }
    }

    FILE* f = fopen(path, "rb");
    CHECK(f != NULL, "no region to compact");
    if (f == NULL) return;
    fseek(f, 0, SEEK_END);
    long const before = ftell(f);
    fclose(f);

    CHECK(region_should_compact(TEST_DIR, 0, 1), "12 rewrites did not make the region worth compacting");
    CHECK(region_compact(TEST_DIR, 0, 1), "region_compact failed");

    f = fopen(path, "rb");
    CHECK(f != NULL, "the region vanished during compaction");
    if (f == NULL) return;
    fseek(f, 0, SEEK_END);
    long const after = ftell(f);
    fclose(f);
    printf("  %ld bytes -> %ld after compaction\n", before, after);
    CHECK(after < before, "compaction did not shrink the region (%ld -> %ld)", before, after);

    // And every chunk still reads, with the last value written.
    int lost = 0, wrong = 0;
    for (int32_t cz = 8; cz < 11; cz++) {
        for (int32_t cx = 0; cx < 3; cx++) {
            chunk_t b;
            memset(&b, 0, sizeof(b));
            b.id = g_ib;
            b.st = g_sb;
            b.cx = cx;
            b.cz = cz;
            if (region_read_chunk(TEST_DIR, &b, NULL) != 1) {
                lost++;
                continue;
            }
            if (g_ib[CH_IDX(0, 40, 0)] != BLK_GLASS) wrong++;
        }
    }
    CHECK(lost == 0, "compaction lost %d chunks", lost);
    CHECK(wrong == 0, "compaction resurrected %d stale chunk copies", wrong);
    CHECK(!region_should_compact(TEST_DIR, 0, 1), "the region still wants compacting afterwards");
}

// ---------------------------------------------------------------------
//  Tagged fields
//
//  The point of this codec is that formats can grow without a migration
//  for every change, so the checks are about exactly that: a build that
//  does not know a field must step over it, and a build that expects a
//  field the save does not have must keep its default. Everything else
//  is bookkeeping.
// ---------------------------------------------------------------------

// A cow, as an OLD build knows it.
typedef struct {
    float   x, y, z;
    int32_t health;
} cow_v1_t;

// The same cow after someone added the things the user asked for:
// whether it is aggressive, whether the dog is sitting, a name.
typedef struct {
    float   x, y, z;
    int32_t health;
    int32_t aggressive;
    int32_t sitting;
    char    nametag[24];
    int64_t bred_at;
} cow_v2_t;

static void cow_v1_write(tag_writer_t* w, cow_v1_t const* c) {
    tag_put_f32(w, "x", c->x);
    tag_put_f32(w, "y", c->y);
    tag_put_f32(w, "z", c->z);
    tag_put_i32(w, "health", c->health);
}

static void cow_v2_write(tag_writer_t* w, cow_v2_t const* c) {
    tag_put_f32(w, "x", c->x);
    tag_put_f32(w, "y", c->y);
    tag_put_f32(w, "z", c->z);
    tag_put_i32(w, "health", c->health);
    tag_put_i32(w, "aggressive", c->aggressive);
    tag_put_i32(w, "sitting", c->sitting);
    tag_put_str(w, "nametag", c->nametag);
    tag_put_i64(w, "bred_at", c->bred_at);
}

// Both readers follow the same three steps: default, overwrite what is
// recognised, skip the rest.
static void cow_v1_read(tag_reader_t* r, cow_v1_t* c) {
    *c = (cow_v1_t){.x = 0, .y = 0, .z = 0, .health = 10};
    char name[TAG_NAME_MAX + 1];
    for (;;) {
        int const t = tag_next(r, name, sizeof(name));
        if (t == TAG_END) break;
        if (t == TAG_F32 && strcmp(name, "x") == 0) c->x = tag_get_f32(r);
        else if (t == TAG_F32 && strcmp(name, "y") == 0) c->y = tag_get_f32(r);
        else if (t == TAG_F32 && strcmp(name, "z") == 0) c->z = tag_get_f32(r);
        else if (t == TAG_I32 && strcmp(name, "health") == 0) c->health = tag_get_i32(r);
        else tag_skip(r, t);
    }
}

static void cow_v2_read(tag_reader_t* r, cow_v2_t* c) {
    memset(c, 0, sizeof(*c));
    c->health     = 10;
    c->aggressive = 0;
    c->sitting    = 0;
    c->bred_at    = -1;      /* the default a new field gets in an old save */
    snprintf(c->nametag, sizeof(c->nametag), "%s", "");
    char name[TAG_NAME_MAX + 1];
    for (;;) {
        int const t = tag_next(r, name, sizeof(name));
        if (t == TAG_END) break;
        if (t == TAG_F32 && strcmp(name, "x") == 0) c->x = tag_get_f32(r);
        else if (t == TAG_F32 && strcmp(name, "y") == 0) c->y = tag_get_f32(r);
        else if (t == TAG_F32 && strcmp(name, "z") == 0) c->z = tag_get_f32(r);
        else if (t == TAG_I32 && strcmp(name, "health") == 0) c->health = tag_get_i32(r);
        else if (t == TAG_I32 && strcmp(name, "aggressive") == 0) c->aggressive = tag_get_i32(r);
        else if (t == TAG_I32 && strcmp(name, "sitting") == 0) c->sitting = tag_get_i32(r);
        else if (t == TAG_STR && strcmp(name, "nametag") == 0) tag_get_str(r, c->nametag, sizeof(c->nametag));
        else if (t == TAG_I64 && strcmp(name, "bred_at") == 0) c->bred_at = tag_get_i64(r);
        else tag_skip(r, t);
    }
}

static void check_tags(void) {
    printf("tagged fields\n");
    static uint8_t buf[512];

    // Every type survives a round trip.
    {
        tag_writer_t w;
        tag_write_init(&w, buf, sizeof(buf));
        tag_put_i8(&w, "a", -7);
        tag_put_i16(&w, "b", -30000);
        tag_put_i32(&w, "c", -123456789);
        tag_put_i64(&w, "d", -1234567890123LL);
        tag_put_f32(&w, "e", 3.25f);
        tag_put_str(&w, "f", "a dog called Rex");
        uint8_t const blob[5] = {1, 2, 3, 4, 5};
        tag_put_blob(&w, "g", blob, sizeof(blob));
        size_t const n = tag_write_done(&w);
        CHECK(n > 0, "the writer overflowed on a small record");

        tag_reader_t r;
        tag_read_init(&r, buf, n);
        char name[TAG_NAME_MAX + 1];
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_I8 && tag_get_i8(&r) == -7, "i8 did not round-trip");
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_I16 && tag_get_i16(&r) == -30000, "i16 did not round-trip");
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_I32 && tag_get_i32(&r) == -123456789, "i32 did not round-trip");
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_I64 && tag_get_i64(&r) == -1234567890123LL,
              "i64 did not round-trip");
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_F32 && tag_get_f32(&r) == 3.25f, "f32 did not round-trip");
        char str[32];
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_STR, "str tag lost its type");
        tag_get_str(&r, str, sizeof(str));
        CHECK(strcmp(str, "a dog called Rex") == 0, "str did not round-trip (got \"%s\")", str);
        uint8_t back[5] = {0};
        CHECK(tag_next(&r, name, sizeof(name)) == TAG_BLOB, "blob tag lost its type");
        CHECK(tag_get_blob(&r, back, sizeof(back)) == 5 && memcmp(back, blob, 5) == 0, "blob did not round-trip");
        CHECK(!r.error, "the reader errored on a well-formed record");
    }

    // THE POINT, part one: a NEW save read by an OLD build. The fields
    // it does not know must be stepped over, and the ones it does must
    // come back right.
    cow_v2_t const newer = {.x = 1.5f, .y = 64.0f, .z = -2.25f, .health = 18, .aggressive = 1, .sitting = 1,
                            .nametag = "Rex", .bred_at = 999};
    tag_writer_t w;
    tag_write_init(&w, buf, sizeof(buf));
    cow_v2_write(&w, &newer);
    size_t const n2 = tag_write_done(&w);
    CHECK(n2 > 0, "writing the newer record overflowed");

    tag_reader_t r;
    tag_read_init(&r, buf, n2);
    cow_v1_t old_read;
    cow_v1_read(&r, &old_read);
    CHECK(!r.error, "an old build errored reading a newer record");
    CHECK(old_read.x == newer.x && old_read.z == newer.z, "an old build misread the fields it does know");
    CHECK(old_read.health == 18, "an old build lost a field it does know");

    // THE POINT, part two: an OLD save read by a NEW build. The fields
    // that did not exist yet must keep their defaults, not rubbish.
    cow_v1_t const older = {.x = -8.0f, .y = 30.0f, .z = 4.0f, .health = 7};
    tag_write_init(&w, buf, sizeof(buf));
    cow_v1_write(&w, &older);
    size_t const n1 = tag_write_done(&w);

    tag_read_init(&r, buf, n1);
    cow_v2_t new_read;
    cow_v2_read(&r, &new_read);
    CHECK(!r.error, "a new build errored reading an older record");
    CHECK(new_read.x == older.x && new_read.health == 7, "a new build misread an older record");
    CHECK(new_read.aggressive == 0 && new_read.sitting == 0, "a field absent from an old save did not default");
    CHECK(new_read.bred_at == -1, "a new field did not keep its default (%lld)", (long long)new_read.bred_at);
    CHECK(new_read.nametag[0] == '\0', "a new string field did not default to empty");

    // Nesting, and skipping a whole compound: a furnace's inventory is
    // the case this has to survive.
    {
        tag_write_init(&w, buf, sizeof(buf));
        tag_put_i32(&w, "burn", 40);
        tag_begin(&w, "items");
        tag_put_i32(&w, "slot0", 11);
        tag_begin(&w, "nested");
        tag_put_i32(&w, "deep", 5);
        tag_end(&w);
        tag_put_i32(&w, "slot1", 22);
        tag_end(&w);
        tag_put_i32(&w, "after", 77);
        size_t const n = tag_write_done(&w);
        CHECK(n > 0, "the nested record overflowed");

        tag_read_init(&r, buf, n);
        char name[TAG_NAME_MAX + 1];
        int  burn = 0, after = 0;
        for (;;) {
            int const t = tag_next(&r, name, sizeof(name));
            if (t == TAG_END) break;
            if (t == TAG_I32 && strcmp(name, "burn") == 0) burn = tag_get_i32(&r);
            else if (t == TAG_I32 && strcmp(name, "after") == 0) after = tag_get_i32(&r);
            else tag_skip(&r, t);   /* the whole "items" compound, nesting included */
        }
        CHECK(!r.error, "skipping a nested compound errored");
        CHECK(burn == 40, "the field before a skipped compound was lost");
        CHECK(after == 77, "skipping a nested compound did not land on the next field (got %d)", after);
    }

    // A truncated record must fail, not read past its buffer.
    tag_read_init(&r, buf, 3);
    char name[TAG_NAME_MAX + 1];
    while (tag_next(&r, name, sizeof(name)) != TAG_END) { /* drain */ }
    CHECK(r.pos <= r.len, "the reader ran past the end of a truncated record");

    // And a writer given too little room must report it rather than
    // write a record that decodes as something else.
    uint8_t tiny[8];
    tag_write_init(&w, tiny, sizeof(tiny));
    cow_v2_write(&w, &newer);
    CHECK(tag_write_done(&w) == 0, "a writer that overflowed still reported a length");
}

// ---------------------------------------------------------------------
//  Sections, and the world store
// ---------------------------------------------------------------------

typedef struct {
    int     seen;
    uint8_t last_id;
    size_t  last_len;
    uint8_t first_byte;
} section_tally_t;

static void tally_section(uint8_t id, uint8_t const* data, size_t len, void* user) {
    section_tally_t* t = user;
    t->seen++;
    t->last_id    = id;
    t->last_len   = len;
    t->first_byte = len > 0 ? data[0] : 0;
}

static void check_sections(void) {
    printf("chunk sections\n");

    chunk_t a, b;
    fill_chunk(&a, g_ia, g_sa, 0, 0, GEN_SEED);

    // Build two sections by hand: one this build knows about, and one
    // with an id it has never heard of -- which is what a save from a
    // future build looks like.
    uint8_t sec[64];
    size_t  w = 0;
    uint8_t const payload_known[3]   = {0xA1, 0xA2, 0xA3};
    uint8_t const payload_unknown[5] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5};

    sec[w++] = SECTION_ENTITIES;
    sec[w++] = (uint8_t)sizeof(payload_known);
    sec[w++] = 0;
    sec[w++] = 0;
    sec[w++] = 0;
    memcpy(&sec[w], payload_known, sizeof(payload_known));
    w += sizeof(payload_known);

    sec[w++] = 99;  // from the future
    sec[w++] = (uint8_t)sizeof(payload_unknown);
    sec[w++] = 0;
    sec[w++] = 0;
    sec[w++] = 0;
    memcpy(&sec[w], payload_unknown, sizeof(payload_unknown));
    w += sizeof(payload_unknown);

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    size_t const   n = chunk_encode(&a, sec, w, buf, sizeof(buf));
    CHECK(n > 0, "encoding a chunk with sections failed");

    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    section_tally_t t = {0};
    CHECK(chunk_decode_ex(buf, n, &b, NULL, tally_section, &t), "decoding a chunk with sections failed");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "sections disturbed the block plane");
    CHECK(t.seen == 2, "expected 2 sections, saw %d", t.seen);
    CHECK(t.last_id == 99 && t.last_len == 5 && t.first_byte == 0xB1,
          "the unknown section did not arrive intact (id %u len %zu)", t.last_id, t.last_len);

    // And the decoder that does NOT care about sections must step over
    // them without complaint -- that is an old build reading this save.
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    CHECK(chunk_decode(buf, n, &b, NULL), "a section-unaware decode rejected a chunk with sections");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "a section-unaware decode got the wrong blocks");

    // A section whose length runs off the end is corruption.
    buf[n - 1] = 0xFF;
    uint8_t bad[CHUNK_PAYLOAD_MAX];
    memcpy(bad, buf, n);
    bad[n - w + 1] = 0xFF;  /* blow up the known section's length */
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    CHECK(!chunk_decode(bad, n, &b, NULL), "a section running past the end decoded anyway");
}

#define STORE_BASE "build/host/storetest"

static void check_worldstore(void) {
    printf("world store\n");

    CHECK(worldstore_init(STORE_BASE), "worldstore_init failed");

    world_meta_t   meta;
    player_state_t player;

    // Delete anything a previous run left, so the check is repeatable.
    world_meta_t old[CM_WORLDS_MAX];
    int const    prior = worldstore_list(old, CM_WORLDS_MAX);
    for (int i = 0; i < prior; i++) worldstore_delete(old[i].slug);
    CHECK(worldstore_list(old, CM_WORLDS_MAX) == 0, "could not clear the world directory");

    // Create. The slug must be a legal FAT name whatever was typed.
    CHECK(worldstore_create("Kurt's Far Lands!", 12345u, &meta, &player), "worldstore_create failed");
    printf("  \"%s\" -> slug \"%s\", seed %u\n", meta.name, meta.slug, meta.seed);
    CHECK(strcmp(meta.name, "Kurt's Far Lands!") == 0, "the display name was mangled");
    for (char const* c = meta.slug; *c; c++) {
        CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_',
              "slug \"%s\" has a character FAT will not like", meta.slug);
    }

    // The player defaults, and a round trip of every field.
    CHECK(player.health == 20 && player.hunger == 20, "a new player did not get default health/hunger");
    player.x       = -100000.5;   /* out at the Far Lands, to prove the doubles survive */
    player.y       = 71.25;
    player.z       = 12.5;
    player.yaw     = 1.5f;
    player.health  = 13;
    player.hunger  = 7;
    player.has_bed = true;
    player.bed_x   = -99998;
    player.bed_y   = 70;
    player.bed_z   = 11;
    player.time_of_day = 4321;
    meta.play_secs     = 99;
    CHECK(worldstore_save(&meta, &player), "worldstore_save failed");

    world_meta_t   m2;
    player_state_t p2;
    worldstore_close();
    CHECK(worldstore_open(meta.slug, &m2, &p2), "worldstore_open failed");
    CHECK(strcmp(m2.name, meta.name) == 0, "the name did not survive a save/load");
    CHECK(m2.seed == 12345u, "the seed did not survive a save/load");
    CHECK(m2.play_secs == 99, "play_secs did not survive");
    CHECK(p2.x == player.x && p2.y == player.y && p2.z == player.z, "the player position did not survive");
    CHECK(p2.health == 13 && p2.hunger == 7, "player health/hunger did not survive");
    CHECK(p2.has_bed && p2.bed_x == -99998, "the bed spawn did not survive");
    CHECK(p2.time_of_day == 4321, "time of day did not survive");

    // A second world with the same name must not collide.
    world_meta_t   m3;
    player_state_t p3;
    CHECK(worldstore_create("Kurt's Far Lands!", 777u, &m3, &p3), "creating a second world failed");
    CHECK(strcmp(m3.slug, meta.slug) != 0, "two worlds of the same name got the same slug (%s)", m3.slug);

    // Listing finds both.
    world_meta_t list[CM_WORLDS_MAX];
    int const    n = worldstore_list(list, CM_WORLDS_MAX);
    printf("  %d worlds listed\n", n);
    CHECK(n == 2, "expected 2 worlds, listed %d", n);

    // Chunks go through the store, which owns the paths.
    CHECK(worldstore_open(meta.slug, &m2, &p2), "reopening failed");
    chunk_t c;
    fill_chunk(&c, g_ia, g_sa, 4, -9, m2.seed);
    g_sa[CH_IDX(3, 33, 3)] = ST_PLACED;
    CHECK(world_chunk_save(&c), "world_chunk_save failed");

    chunk_t back;
    memset(&back, 0, sizeof(back));
    back.id = g_ib;
    back.st = g_sb;
    back.cx = 4;
    back.cz = -9;
    CHECK(world_chunk_load(&back) == 1, "world_chunk_load did not find the chunk it just saved");
    CHECK(memcmp(g_ia, g_ib, CH_CELLS) == 0, "the chunk did not survive the store round trip");
    CHECK((g_sb[CH_IDX(3, 33, 3)] & ST_PLACED) != 0, "ST_PLACED did not survive the store round trip");

    // Deleting removes the world and its regions.
    CHECK(worldstore_delete(m3.slug), "worldstore_delete failed");
    CHECK(worldstore_list(list, CM_WORLDS_MAX) == 1, "the deleted world is still listed");
    CHECK(!worldstore_open(m3.slug, &m3, &p3), "a deleted world still opens");
}

// The remap is what lets block ids be added, reordered or removed
// without breaking existing worlds. Two things have to hold: the
// palette really is written into level.cmw (so a future build can read
// what the ids meant), and chunk_decode really applies a remap.
static void check_palette(void) {
    printf("block palette\n");

    // 1. level.cmw carries every block's NAME. Without that, a future
    //    build has nothing to map old ids by.
    char path[256];
    snprintf(path, sizeof(path), "%s/worlds/kurt_s_far_lands/level.cmw", STORE_BASE);
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL, "no level.cmw at %s", path);
    if (f == NULL) return;
    static char blob[16384];
    size_t const got = fread(blob, 1, sizeof(blob) - 1, f);
    fclose(f);
    blob[got] = '\0';

    // The magic carries the major version, ahead of the NBT.
    CHECK(got > 4 && memcmp(blob, CM_LEVEL_MAGIC, 3) == 0, "level.cmw does not start with its magic");
    CHECK(blob[3] == CM_LEVEL_MAJOR, "level.cmw's major version byte is '%c', expected '%c'", blob[3],
          CM_LEVEL_MAJOR);

    int found = 0;
    for (int i = 0; i < BLK_COUNT; i++) {
        size_t const n = strlen(BLOCKS[i].name);
        for (size_t at = 0; at + n <= got; at++) {
            if (memcmp(&blob[at], BLOCKS[i].name, n) == 0) {
                found++;
                break;
            }
        }
    }
    printf("  level.cmw names %d of %d blocks\n", found, BLK_COUNT);
    CHECK(found == BLK_COUNT, "the palette names only %d of %d blocks", found, BLK_COUNT);

    // 2. A remap really is applied on decode. Stand in for "a future
    //    build renumbered everything" by shifting every id by hand.
    chunk_t a, b;
    fill_chunk(&a, g_ia, g_sa, 1, 1, GEN_SEED);

    // Count what we are about to move, so the check cannot pass vacuously.
    int stone_before = 0;
    for (int i = 0; i < CH_CELLS; i++) stone_before += (g_ia[i] == BLK_STONE);
    CHECK(stone_before > 0, "the test chunk has no stone to remap");

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    size_t const   n = chunk_encode(&a, NULL, 0, buf, sizeof(buf));

    // A remap that turns every saved stone into cobblestone and leaves
    // the rest alone -- the shape of what a renumbering produces.
    uint8_t remap[256];
    for (int i = 0; i < 256; i++) remap[i] = (uint8_t)i;
    remap[BLK_STONE] = BLK_COBBLE;

    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    CHECK(chunk_decode(buf, n, &b, remap), "decoding with a remap failed");

    int stone_after = 0, cobble_after = 0;
    for (int i = 0; i < CH_CELLS; i++) {
        stone_after += (g_ib[i] == BLK_STONE);
        cobble_after += (g_ib[i] == BLK_COBBLE);
    }
    printf("  %d stone cells remapped\n", stone_before);
    CHECK(stone_after == 0, "%d cells kept the old id despite the remap", stone_after);
    CHECK(cobble_after >= stone_before, "the remapped cells did not arrive as the new id");

    // And a remap that drops a block (it no longer exists) turns it into
    // air rather than into whatever happens to sit at that number.
    remap[BLK_STONE] = BLK_AIR;
    memset(&b, 0, sizeof(b));
    b.id = g_ib;
    b.st = g_sb;
    CHECK(chunk_decode(buf, n, &b, remap), "decoding with a dropping remap failed");
    int air_now = 0;
    for (int i = 0; i < CH_CELLS; i++) air_now += (g_ib[i] == BLK_AIR);
    CHECK(air_now >= stone_before, "a removed block did not become air");
}

// ---------------------------------------------------------------------
//  Streaming
//
//  On the host the worker runs every job inline, which is the same code
//  path the badge takes in synchronous mode -- so this exercises the
//  request / do / apply cycle end to end without a badge.
//
//  It exists because the first version of that cycle looked perfectly
//  correct and loaded nothing at all: do_load() reached for the chunk
//  with chunk_find(), which deliberately hides a chunk while it is
//  CS_LOADING so no game code can read a half-filled one -- and the
//  loader is the code doing the filling. The badge drew an empty world
//  at a confident 30 fps. Nothing below would have passed.
// ---------------------------------------------------------------------

static void check_streaming(void) {
    printf("streaming\n");

    // The store was shut down after the earlier checks; streaming needs
    // it back.
    CHECK(chunk_store_init(), "chunk_store_init failed");

    world_meta_t   meta;
    player_state_t player;
    CHECK(worldstore_init(STORE_BASE), "worldstore_init failed");
    CHECK(worldstore_create("stream test", 4242u, &meta, &player), "could not create the streaming world");
    CHECK(chunk_worker_start(meta.seed), "chunk_worker_start failed");
    CHECK(chunk_worker_synchronous(), "the host worker should be synchronous");

    // A chunk nobody has visited: it must be generated and resident.
    CHECK(chunk_find(0, 0) == NULL, "chunk (0,0) was already resident");
    CHECK(chunk_worker_request_load(0, 0), "request_load(0,0) was refused");
    chunk_t* c = chunk_find(0, 0);
    CHECK(c != NULL, "a requested chunk never became resident");
    if (c == NULL) return;
    CHECK(c->cstate == CS_READY, "a loaded chunk is in state %u, expected CS_READY", c->cstate);
    CHECK((c->flags & CF_GENERATED) != 0 || (c->flags & CF_EDITED) != 0, "a loaded chunk has no content flags");

    int solid = 0;
    for (int i = 0; i < CH_CELLS; i++) solid += (c->id[i] != BLK_AIR);
    printf("  chunk (0,0) streamed in with %d non-air cells\n", solid);
    CHECK(solid > 0, "a streamed chunk is entirely air");
    CHECK(world_ground(4, 4) > 0, "the streamed chunk has no ground to stand on");

    // Its neighbours, so meshing has real borders to work with.
    for (int32_t dz = -1; dz <= 1; dz++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0) continue;
            CHECK(chunk_worker_request_load(dx, dz), "request_load(%d,%d) was refused", dx, dz);
        }
    }

    // Meshing, at each level of detail.
    uint8_t* scratch = malloc(chunkmesh_scratch_bytes());
    CHECK(scratch != NULL, "no scratch for the mesher");
    if (scratch == NULL) return;
    for (int lod = 0; lod < LOD_COUNT; lod++) {
        mesh_t m;
        CHECK(chunkmesh_build(0, 0, lod, scratch, &m), "chunkmesh_build failed at lod %d", lod);
        printf("  lod %d: %d verts, %d tris\n", lod, m.vn, m.tn);
        CHECK(m.tn > 0, "lod %d produced no triangles", lod);
        CHECK(m.vn <= 40000, "lod %d produced %d vertices, close to mesh_t's 65535 limit (F-13)", lod, m.vn);

        // Every greedy face must carry a direction, or the renderer's
        // cull silently drops it.
        int none = 0;
        for (int i = 0; i < m.tn; i++) none += (m.t[i].dir == MESH_DIR_NONE);
        CHECK(none == 0 || lod == LOD_FANCY, "lod %d has %d triangles with no face direction", lod, none);

        // The mesh must fit inside its chunk, or the renderer's bounding
        // box and its frustum cull are both lies.
        float const span = lod == LOD_COARSE ? (float)CH_W : (float)CH_W;
        for (int i = 0; i < m.vn; i++) {
            CHECK(m.v[i].x >= -0.01f && m.v[i].x <= span + 0.01f, "lod %d vertex %d is outside the chunk in x (%g)",
                  lod, i, m.v[i].x);
            CHECK(m.v[i].z >= -0.01f && m.v[i].z <= span + 0.01f, "lod %d vertex %d is outside the chunk in z (%g)",
                  lod, i, m.v[i].z);
            CHECK(m.v[i].y >= -0.01f && m.v[i].y <= (float)CH_H + 0.01f, "lod %d vertex %d is outside in y (%g)", lod,
                  i, m.v[i].y);
            if (s_fail) break;
        }
        mesh_free(&m);
    }
    free(scratch);

    // Edits must survive the round trip through the worker's save path.
    world_set(3, world_ground(3, 3), 3, BLK_GLASS, ST_PLACED);
    CHECK((c->flags & CF_EDITED) != 0, "an edit did not mark the chunk for saving");
    uint8_t const seq_before = c->edit_seq;
    CHECK(chunk_worker_request_save(0, 0), "request_save was refused");
    CHECK((c->flags & CF_EDITED) == 0, "a saved chunk is still marked edited");
    CHECK(c->edit_seq == seq_before, "saving changed the edit sequence");

    chunk_worker_stop();
    chunk_store_shutdown();
}

int main(void) {
    check_blocks();
    check_rng();
    check_tags();
    check_chunk_coords();
    check_chunk_state_bits();
    if (!chunk_store_init()) {
        printf("  FAIL: chunk_store_init()\n");
        return 1;
    }
    check_chunk_store();
    chunk_store_shutdown();
    check_worldgen();
    check_codec();
    check_region();
    check_region_damage();
    check_torn_write();
    check_compaction();
    check_sections();
    check_worldstore();
    check_palette();
    check_streaming();
    if (s_fail) {
        printf("\nworldcheck: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("\nworldcheck: all checks passed\n");
    return 0;
}
