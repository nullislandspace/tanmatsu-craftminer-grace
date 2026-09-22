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
//    far lands    step 7   Java's maths, the wall, the tunnels, the cliff, the asymmetry
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
#include "world/farlands.h"
#include "world/datadir.h"
#include <sys/stat.h>
#include <time.h>
#include "world/chunk_worker.h"
#include "world/chunkmesh.h"
#include "world/light.h"
#include "world/worldstore.h"
#include "se_nbt.h"
#include "game/physics.h"
#include "game/raycast.h"
#include "game/interact.h"
#include "game/player.h"
#include "game/replay.h"
#include "items/inventory.h"
#include "items/items.h"
#include "items/item_entity.h"

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
        CHECK(d->kind <= K_SIGN, "block %s: kind %u out of range", d->name ? d->name : "?", d->kind);

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

// THE ID REGISTRY (D-74). Block ids and names, and item names, are what
// saves on people's cards are made of. tools/ids.txt is the list of every
// one ever shipped, and this is the check that the code still agrees with
// it -- both ways, so a new block cannot slip in unlisted either.
static void check_ids(void) {
    printf("the id registry\n");
    FILE* f = fopen("tools/ids.txt", "r");
    CHECK(f != NULL, "tools/ids.txt is missing");
    if (f == NULL) return;
    bool block_listed[BLK_COUNT] = {false};
    bool item_listed[ITEM_COUNT] = {false};
    int  blocks = 0, items = 0;
    char line[160];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int  id = -1;
        char name[64];
        if (sscanf(line, "block %d %63s", &id, name) == 2) {
            blocks++;
            CHECK(id >= 0 && id < BLK_COUNT, "ids.txt lists block %d (%s), which the code does not have: a block "
                  "is never removed, only retired", id, name);
            if (id < 0 || id >= BLK_COUNT) continue;
            CHECK(strcmp(BLOCKS[id].name, name) == 0, "block %d is \"%s\" in the code but \"%s\" in ids.txt: ids "
                  "and names never change once shipped", id, BLOCKS[id].name, name);
            CHECK(!block_listed[id], "block %d is listed twice in ids.txt", id);
            block_listed[id] = true;
        } else if (sscanf(line, "item %63s", name) == 1) {
            items++;
            uint16_t const it = item_by_name(name);
            CHECK(it >= BLK_COUNT, "ids.txt lists item \"%s\", which the code does not have: an item is never "
                  "renamed or removed", name);
            if (it >= BLK_COUNT && it < ITEM_COUNT) item_listed[it] = true;
        } else {
            CHECK(false, "ids.txt: cannot read the line \"%s\"", line);
        }
    }
    fclose(f);
    for (int b = 0; b < BLK_COUNT; b++)
        CHECK(block_listed[b], "block %d (%s) is not in tools/ids.txt: append \"block %d %s\" to it", b,
              BLOCKS[b].name, b, BLOCKS[b].name);
    for (int i = BLK_COUNT; i < ITEM_COUNT; i++)
        CHECK(item_listed[i], "item \"%s\" is not in tools/ids.txt: append \"item %s\" to it", item_def(i).name,
              item_def(i).name);
    printf("  %d blocks and %d items, all as shipped\n", blocks, items);
}

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
    worldgen_chunk(c, seed, FARLANDS_X_DEFAULT);
}

// ---------------------------------------------------------------------
//  The Far Lands (Part X, D-78)
//
//  Beta 1.7.3's generator, overflowed. What is checked: that the Java it
//  depends on behaves like Java; that the fast path makes the same blocks
//  as the unabridged port; that the Far Lands look like the Edge Far
//  Lands (a wall to the top, tunnels running west, flooded below the
//  sea); that the change at the edge is sudden; and that nothing east,
//  north or south of it changed -- the guard against a sign bug turning
//  the whole world into Far Lands.
// ---------------------------------------------------------------------

#define FL_SEED 0xC0FFEEu

static bool is_rock(uint8_t b) {
    return b == BLK_STONE || b == BLK_BEDROCK || b == BLK_GRAVEL || b == BLK_COAL_ORE;
}

static void check_farlands(void) {
    printf("far lands\n");

    // Java, where C would differ.
    CHECK(java_d2i(3.0e9) == INT32_MAX && java_d2i(-3.0e9) == INT32_MIN, "java_d2i does not saturate");
    CHECK(java_d2i(-1.7) == -1 && java_d2i(1.7) == 1 && java_d2i(2147483646.9) == 2147483646,
          "java_d2i does not truncate toward zero");
    CHECK(java_d2i(NAN) == 0, "java_d2i(NaN) is not 0");
    java_random_t jr;
    java_random_seed(&jr, 42);
    CHECK(java_random_next_int(&jr) == -1170105035, "java.util.Random(42).nextInt() is not -1170105035");
    java_random_seed(&jr, 0);
    double const d0 = java_random_next_double(&jr);
    CHECK(fabs(d0 - 0.730967787376657) < 1e-15, "java.util.Random(0).nextDouble() is %.17g, not 0.730967787376657", d0);

    // Where the edge is.
    CHECK(farlands_chunk_is(-129, -2048) && !farlands_chunk_is(-128, -2048), "the edge at -2048 is not between chunks -129 and -128");
    CHECK(!farlands_chunk_is(-100000, FARLANDS_NONE), "a world without Far Lands has some");
    CHECK(farlands_beta_cx(-129, -2048) == -784428 && farlands_beta_cx(-1, 0) == -784428,
          "the first Far Lands chunk is not Beta chunk -784428");
    CHECK(farlands_beta_y(0) == 0 && farlands_beta_y(CH_SEA_LEVEL) == 63 && farlands_beta_y(CH_SEA_LEVEL + 1) == 64 &&
              farlands_beta_y(CH_H - 1) == 127,
          "Beta's rows do not map onto ours end to end (%d %d %d %d)", farlands_beta_y(0), farlands_beta_y(CH_SEA_LEVEL),
          farlands_beta_y(CH_SEA_LEVEL + 1), farlands_beta_y(CH_H - 1));
    for (int y = 1; y < CH_H; y++) CHECK(farlands_beta_y(y) > farlands_beta_y(y - 1), "Beta row mapping not rising at %d", y);

    // The fast path against the unabridged port, block for block.
    static uint8_t fast[16 * 16 * 128], full[16 * 16 * 128];
    long           diff = 0, cells = 0;
    clock_t        t_fast = 0, t_full = 0;
    for (int32_t bx = -784431; bx <= -784428; bx++) {
        for (int32_t bz = -2; bz <= 1; bz++) {
            clock_t t = clock();
            CHECK(farlands_beta_column(bx, bz, FL_SEED, false, fast), "no Far Lands tables");
            t_fast += clock() - t;
            t = clock();
            farlands_beta_column(bx, bz, FL_SEED, true, full);
            t_full += clock() - t;
            for (size_t i = 0; i < sizeof(fast); i++) diff += fast[i] != full[i];
            cells += (long)sizeof(fast);
        }
    }
    printf("  fast path against the full port: %ld of %ld cells differ; %.1f ms against %.1f ms a chunk (host)\n", diff,
           cells, 1000.0 * (double)t_fast / CLOCKS_PER_SEC / 16.0, 1000.0 * (double)t_full / CLOCKS_PER_SEC / 16.0);
    CHECK(diff == 0, "the fast path makes %ld blocks the full port does not", diff);

    // What the Far Lands are made of, over a block of chunks at the edge.
    static uint8_t id[CH_CELLS], st[CH_CELLS];
    chunk_t        c;
    long           n_rock = 0, n_air = 0, n_water = 0, n_soil = 0, n_sand = 0, n_other = 0;
    int            cols = 0, tall = 0, floor_ok = 0;
    long           same_x = 0, pairs_x = 0, same_z = 0, pairs_z = 0;
    for (int32_t cx = -132; cx <= -129; cx++) {
        for (int32_t cz = -4; cz <= 3; cz++) {
            gen_into(&c, id, st, cx, cz, FL_SEED);
            for (int z = 0; z < CH_D; z++) {
                for (int x = 0; x < CH_W; x++) {
                    int top = -1;
                    for (int y = 0; y < CH_H; y++) {
                        uint8_t const b = id[CH_IDX(x, y, z)];
                        if (is_rock(b)) n_rock++;
                        else if (b == BLK_AIR) n_air++;
                        else if (b == BLK_WATER) n_water++;
                        else if (b == BLK_DIRT || b == BLK_GRASS) n_soil++;
                        else if (b == BLK_SAND) n_sand++;
                        else n_other++;
                        if (b != BLK_AIR && b != BLK_WATER) top = y;
                        // Tunnels: is a cell the same kind (open or not)
                        // as its neighbour along x, and along z?
                        bool const open = b == BLK_AIR || b == BLK_WATER;
                        if (x + 1 < CH_W) {
                            uint8_t const nb = id[CH_IDX(x + 1, y, z)];
                            same_x += open == (nb == BLK_AIR || nb == BLK_WATER);
                            pairs_x++;
                        }
                        if (z + 1 < CH_D) {
                            uint8_t const nb = id[CH_IDX(x, y, z + 1)];
                            same_z += open == (nb == BLK_AIR || nb == BLK_WATER);
                            pairs_z++;
                        }
                    }
                    cols++;
                    tall += top >= CH_H - 8;
                    floor_ok += id[CH_IDX(x, 0, z)] == BLK_BEDROCK;
                }
            }
        }
    }
    long const all = n_rock + n_air + n_water + n_soil + n_sand + n_other;
    printf("  composition: %.0f%% rock, %.0f%% air, %.0f%% water, %.0f%% dirt and grass, %.0f%% sand, %.0f%% other\n",
           100.0 * n_rock / all, 100.0 * n_air / all, 100.0 * n_water / all, 100.0 * n_soil / all, 100.0 * n_sand / all,
           100.0 * n_other / all);
    printf("  %d of %d columns reach within 8 of the top; bedrock under %d\n", tall, cols, floor_ok);
    printf("  neighbours alike: %.1f%% along x (west), %.1f%% along z\n", 100.0 * same_x / pairs_x, 100.0 * same_z / pairs_z);
    CHECK(floor_ok == cols, "%d of %d Far Lands columns have no bedrock floor", cols - floor_ok, cols);
    // Measured when this was written: 67% of columns reach within 8 of
    // the top (the top is full of holes, as Beta's was), 98.7% of
    // neighbours alike along x and 87% along z, and 42% rock, 30% air,
    // 19% water, 9% dirt and grass -- against the wiki's 36 / 25 / 23 /
    // 10 for Beta's own Edge Far Lands.
    CHECK(tall * 2 >= cols, "only %d of %d Far Lands columns reach the top: that is no wall", tall, cols);
    CHECK(same_x * 100 >= pairs_x * 97, "the Far Lands change along x (%.1f%% alike): the tunnels do not run west",
          100.0 * same_x / pairs_x);
    CHECK(same_z * 100 <= pairs_z * 95, "the Far Lands hardly change along z either (%.1f%% alike)", 100.0 * same_z / pairs_z);
    CHECK(n_water * 10 >= all, "the Far Lands are not flooded below the sea (%.0f%% water)", 100.0 * n_water / all);
    CHECK(n_air * 10 >= all, "the Far Lands have no tunnels (%.0f%% air)", 100.0 * n_air / all);

    // Sudden: the last ordinary chunk is ordinary, the first Far Lands
    // chunk is the wall. Chunk -127 is untouched by the edge in every
    // cell; -128 may differ only where a sign stands or where a tree
    // rooted west of the edge would have leaned in.
    static uint8_t id2[CH_CELLS], st2[CH_CELLS];
    chunk_t        c2;
    memset(&c2, 0, sizeof(c2));
    c2.id = id2, c2.st = st2, c2.cx = -127, c2.cz = 0;
    worldgen_chunk(&c2, FL_SEED, FARLANDS_NONE);
    gen_into(&c, id, st, -127, 0, FL_SEED);
    CHECK(memcmp(id, id2, CH_CELLS) == 0, "the chunk east of the edge's chunk changed with the Far Lands");
    int tops_east = 0, tops_west = 0;
    gen_into(&c, id, st, -128, 0, FL_SEED);
    for (int z = 0; z < CH_D; z++) tops_east += worldgen_height(-2048, z, FL_SEED);
    gen_into(&c, id, st, -129, 0, FL_SEED);
    for (int z = 0; z < CH_D; z++) {
        int y = CH_H - 1;
        while (y > 0 && (id[CH_IDX(CH_W - 1, y, z)] == BLK_AIR || id[CH_IDX(CH_W - 1, y, z)] == BLK_WATER)) y--;
        tops_west += y;
    }
    printf("  at the edge: ground %.1f high on the ordinary side, the wall %.1f on the other\n", tops_east / 16.0,
           tops_west / 16.0);
    CHECK(tops_west - tops_east >= 16 * 15, "no cliff at the edge: ground %.1f against %.1f", tops_east / 16.0,
          tops_west / 16.0);

    // The asymmetry guard: east, north and south of the origin, and far
    // out, the Far Lands world is the ordinary world.
    struct {
        int32_t cx, cz;
    } const ORDINARY[] = {{128, 0}, {0, 128}, {0, -128}, {6250, 0}, {0, 6250}, {0, -6250}, {-127, 6250}};
    for (size_t i = 0; i < sizeof(ORDINARY) / sizeof(ORDINARY[0]); i++) {
        gen_into(&c, id, st, ORDINARY[i].cx, ORDINARY[i].cz, FL_SEED);
        memset(&c2, 0, sizeof(c2));
        c2.id = id2, c2.st = st2, c2.cx = ORDINARY[i].cx, c2.cz = ORDINARY[i].cz;
        worldgen_chunk(&c2, FL_SEED, FARLANDS_NONE);
        CHECK(memcmp(id, id2, CH_CELLS) == 0, "chunk (%d,%d) is not ordinary terrain", ORDINARY[i].cx, ORDINARY[i].cz);
    }

    // Signs along the edge: in the edge's chunk only, on the ground,
    // about one chunk in four.
    int signs = 0, misplaced = 0;
    for (int32_t cz = -64; cz < 64; cz++) {
        for (int32_t cx = -128; cx <= -127; cx++) {
            gen_into(&c, id, st, cx, cz, FL_SEED);
            for (int i = 0; i < CH_CELLS; i++) {
                if (id[i] != BLK_SIGN) continue;
                int const y = i % CH_H;
                if (cx != -128 || !block_solid(id[i - 1])) misplaced++;
                else signs++;
                (void)y;
            }
        }
    }
    printf("  %d signs along 2048 blocks of edge\n", signs);
    CHECK(misplaced == 0, "%d signs away from the edge or not standing on the ground", misplaced);
    CHECK(signs >= 10 && signs <= 40, "%d signs in 128 chunks of edge, expected about a quarter", signs);
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
    worldgen_chunk(c, seed, FARLANDS_X_DEFAULT);
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
    meta.time_of_day   = 4321;
    meta.play_secs     = 99;
    CHECK(worldstore_save(&meta, &player, NULL), "worldstore_save failed");

    world_meta_t   m2;
    player_state_t p2;
    worldstore_close();
    CHECK(worldstore_open(meta.slug, &m2, &p2, NULL), "worldstore_open failed");
    CHECK(strcmp(m2.name, meta.name) == 0, "the name did not survive a save/load");
    CHECK(m2.seed == 12345u, "the seed did not survive a save/load");
    CHECK(m2.play_secs == 99, "play_secs did not survive");
    CHECK(p2.x == player.x && p2.y == player.y && p2.z == player.z, "the player position did not survive");
    CHECK(p2.health == 13 && p2.hunger == 7, "player health/hunger did not survive");
    CHECK(p2.has_bed && p2.bed_x == -99998, "the bed spawn did not survive");
    CHECK(m2.time_of_day == 4321, "the world's time of day did not survive");

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
    CHECK(worldstore_open(meta.slug, &m2, &p2, NULL), "reopening failed");
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
    CHECK(!worldstore_open(m3.slug, &m3, &p3, NULL), "a deleted world still opens");
}

// A level.cmw exactly as the builds before save slots wrote it: no
// "placed", no inventory. The Testworld people already have on their
// cards looks like this, so this is the file the adoption must handle.
static void write_legacy_level(char const* slug, double x, double y, double z) {
    char dir[192], path[224];
    snprintf(dir, sizeof(dir), "%s/worlds/%s/region", STORE_BASE, slug);
    CHECK(cm_mkdir_p(dir), "could not make the legacy world's directory");
    snprintf(path, sizeof(path), "%s/worlds/%s/level.cmw", STORE_BASE, slug);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL, "could not write the legacy level.cmw");
    if (f == NULL) return;
    fwrite("CMW1", 1, 4, f);
    NbtWriter w;
    nbt_write_open(&w, f);
    nbt_write_compound(&w, "level");
    nbt_write_int32(&w, "format", 1);
    nbt_write_string(&w, "name", slug);
    nbt_write_int32(&w, "seed", (int32_t)0xC0FFEEu);
    nbt_write_int64(&w, "created", 1);
    nbt_write_int64(&w, "last_played", 1);
    nbt_write_int32(&w, "play_secs", 0);
    nbt_write_int32(&w, "spawn_x", 0);
    nbt_write_int32(&w, "spawn_y", CH_SEA_LEVEL + 2);
    nbt_write_int32(&w, "spawn_z", 0);
    nbt_write_compound(&w, "player");
    nbt_write_double(&w, "x", x);
    nbt_write_double(&w, "y", y);
    nbt_write_double(&w, "z", z);
    nbt_write_double(&w, "yaw", 0.5);
    nbt_write_double(&w, "pitch", 0.0);
    nbt_write_int32(&w, "health", 20);
    nbt_write_int32(&w, "hunger", 20);
    nbt_write_int64(&w, "time_of_day", 7777);  // where builds before D-52 kept the clock
    nbt_write_end(&w);
    nbt_write_compound(&w, "palette");
    for (int i = 0; i < BLK_COUNT; i++) nbt_write_int32(&w, BLOCKS[i].name, i);
    nbt_write_end(&w);
    nbt_write_end(&w);
    fclose(f);
}

static void clear_store(void) {
    world_meta_t old[CM_WORLDS_MAX];
    int const    prior = worldstore_list(old, CM_WORLDS_MAX);
    for (int i = 0; i < prior; i++) worldstore_delete(old[i].slug);
}

// The player's data moving out of the install directory (datadir.h):
// everything moves, a second start finds nothing to do, and an entry
// already at the new place is never overwritten.
static bool dd_write(char const* path, char const* text) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static bool dd_reads(char const* path, char const* text) {
    char  buf[64] = {0};
    FILE* f       = fopen(path, "rb");
    if (f == NULL) return false;
    size_t const n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static bool dd_exists(char const* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void check_datadir(void) {
    printf("the data directory\n");
    char const* const OLD = "build/host/ddtest/apps/at.cavac.craftminer";
    char const* const NEW = "build/host/ddtest/craftminer";
    // A clean slate: what an earlier run left is renamed out of the way
    // by removing the files it could have made.
    char const* const LEFT[] = {"build/host/ddtest/craftminer/worlds/slot1/level.cmw", "build/host/ddtest/craftminer/settings.txt",
                                "build/host/ddtest/craftminer/replays/last.cmr", "build/host/ddtest/craftminer/screenshots/shot001.png"};
    for (size_t i = 0; i < sizeof(LEFT) / sizeof(LEFT[0]); i++) remove(LEFT[i]);
    remove("build/host/ddtest/craftminer/worlds/slot1");
    remove("build/host/ddtest/craftminer/worlds");
    remove("build/host/ddtest/craftminer/replays");
    remove("build/host/ddtest/craftminer/screenshots");
    remove("build/host/ddtest/craftminer");

    // An install directory as a build before this left it.
    CHECK(cm_mkdir_p("build/host/ddtest/apps/at.cavac.craftminer/worlds/slot1"), "could not make the old worlds");
    CHECK(cm_mkdir_p("build/host/ddtest/apps/at.cavac.craftminer/replays"), "could not make the old replays");
    CHECK(cm_mkdir_p("build/host/ddtest/apps/at.cavac.craftminer/screenshots"), "could not make the old screenshots");
    CHECK(cm_mkdir_p("build/host/ddtest/apps/at.cavac.craftminer/textures"), "could not make the textures");
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/worlds/slot1/level.cmw", "testworld");
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/settings.txt", "view=2");
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/replays/last.cmr", "replay");
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/screenshots/shot001.png", "png");
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/textures/dirt.png", "dirt");

    char      report[1024];
    int const moved = datadir_adopt(OLD, NEW, report, sizeof(report));
    printf("  %d entries moved\n", moved);
    CHECK(moved == 4, "%d entries moved, expected 4 (worlds, settings.txt, replays, screenshots)", moved);
    CHECK(dd_reads("build/host/ddtest/craftminer/worlds/slot1/level.cmw", "testworld"), "the world did not arrive");
    CHECK(dd_reads("build/host/ddtest/craftminer/settings.txt", "view=2"), "settings.txt did not arrive");
    CHECK(dd_reads("build/host/ddtest/craftminer/replays/last.cmr", "replay"), "the replay did not arrive");
    CHECK(dd_reads("build/host/ddtest/craftminer/screenshots/shot001.png", "png"), "the screenshot did not arrive");
    CHECK(!dd_exists("build/host/ddtest/apps/at.cavac.craftminer/worlds"), "the old worlds are still there");
    CHECK(dd_reads("build/host/ddtest/apps/at.cavac.craftminer/textures/dirt.png", "dirt"),
          "the app's own files were touched");

    // Started again: nothing to do.
    CHECK(datadir_adopt(OLD, NEW, report, sizeof(report)) == 0, "a second start moved something");

    // An old build run after this one writes settings into the install
    // directory again: the new place's copy wins, nothing is overwritten.
    dd_write("build/host/ddtest/apps/at.cavac.craftminer/settings.txt", "view=0");
    CHECK(datadir_adopt(OLD, NEW, report, sizeof(report)) == 0, "an entry was moved over an existing one");
    CHECK(dd_reads("build/host/ddtest/craftminer/settings.txt", "view=2"), "the new settings.txt was overwritten");
    CHECK(strstr(report, "left") != NULL, "a clash was not reported: \"%s\"", report);
    remove("build/host/ddtest/apps/at.cavac.craftminer/settings.txt");
}

static void check_slots(void) {
    printf("save slots\n");
    CHECK(worldstore_init(STORE_BASE), "worldstore_init failed");
    clear_store();

    world_meta_t   meta, peek;
    player_state_t player;
    for (int i = 0; i < CM_SLOTS; i++) CHECK(!worldstore_slot_peek(i, &peek), "slot %d is not empty", i + 1);

    // Nothing to adopt on a fresh card: the common case, and it must be
    // a quiet no-op.
    CHECK(worldstore_adopt_legacy("flyover", "Testworld") == -1, "adopted a world from an empty card");

    // Creating fills exactly the slot asked for, and refuses a taken one.
    CHECK(worldstore_create_in(2, "My World", 99u, &meta, &player), "worldstore_create_in failed");
    CHECK(strcmp(meta.slug, "slot3") == 0, "slot 3 went into \"%s\"", meta.slug);
    CHECK(worldstore_slot_peek(2, &peek) && strcmp(peek.name, "My World") == 0, "slot 3 does not show its world");
    CHECK(!worldstore_create_in(2, "Other", 1u, &meta, &player), "created a world over an existing one");
    CHECK(worldstore_slot_peek(2, &peek) && peek.seed == 99u, "the refused create damaged slot 3");

    // The inventory and the exact position round trip, by name.
    CHECK(!player.has_inv && !player.placed, "a new player claims a saved inventory or position");
    player.has_inv = true;
    memset(player.inv, 0, sizeof(player.inv));
    player.inv[0]       = (inv_slot_t){ITEM_PICK_STONE, 1, 17};
    player.inv[4]       = (inv_slot_t){BLK_COBBLE, 37, 0};
    player.inv[INV_SLOTS - 1] = (inv_slot_t){BLK_TORCH, 5, 0};
    player.inv_selected = 4;
    player.placed       = true;
    player.x            = 12.25;
    player.y            = 11.0;  // in a cave, far below the surface
    player.z            = -3.75;
    CHECK(worldstore_save(&meta, &player, NULL), "saving the slot world failed");
    worldstore_close();

    player_state_t back;
    CHECK(worldstore_open("slot3", &meta, &back, NULL), "reopening slot 3 failed");
    CHECK(back.placed && back.y == 11.0 && back.x == 12.25 && back.z == -3.75, "the exact position did not survive");
    CHECK(back.has_inv, "the inventory did not come back");
    CHECK(back.inv_selected == 4, "the selected slot did not survive (%d)", (int)back.inv_selected);
    CHECK(back.inv[0].item == ITEM_PICK_STONE && back.inv[0].count == 1 && back.inv[0].wear == 17,
          "the worn pickaxe did not survive");
    CHECK(back.inv[4].item == BLK_COBBLE && back.inv[4].count == 37, "the cobblestone did not survive");
    CHECK(back.inv[INV_SLOTS - 1].item == BLK_TORCH && back.inv[INV_SLOTS - 1].count == 5,
          "the last slot did not survive");
    int filled = 0;
    for (int i = 0; i < INV_SLOTS; i++) filled += back.inv[i].item != 0;
    CHECK(filled == 3, "expected 3 filled slots back, got %d", filled);

    // Dropped items round-trip with the world, by name, age and delay
    // intact (D-68).
    static world_items_t items, items_back;
    memset(&items, 0, sizeof(items));
    items.n    = 2;
    items.e[0] = (item_entity_t){.alive = true, .item = BLK_LOG, .count = 5, .age = 300, .pickup_at = 310};
    phys_body_init(&items.e[0].body, 4.25, 30.0, -6.5);
    items.e[1] = (item_entity_t){.alive = true, .item = ITEM_AXE_STONE, .count = 1, .wear = 40, .age = 11999,
                                 .pickup_at = 12039};
    phys_body_init(&items.e[1].body, -1.0, 12.5, 2.0);
    CHECK(worldstore_open("slot3", &meta, &back, NULL), "reopening slot 3 for the items failed");
    CHECK(worldstore_save(&meta, &back, &items), "saving the items failed");
    worldstore_close();
    CHECK(worldstore_open("slot3", &meta, &back, &items_back), "reopening slot 3 with its items failed");
    CHECK(items_back.n == 2, "%d items came back, not 2", items_back.n);
    CHECK(items_back.e[0].item == BLK_LOG && items_back.e[0].count == 5 && items_back.e[0].age == 300 &&
              items_back.e[0].pickup_at == 310 && items_back.e[0].body.x == 4.25,
          "the logs on the ground did not survive");
    CHECK(items_back.e[1].item == ITEM_AXE_STONE && items_back.e[1].wear == 40 && items_back.e[1].age == 11999,
          "the axe on the ground did not survive");
    // In an unloaded chunk, an item holds still: it neither falls nor
    // ages. (Nothing is resident here: the store was cleared.)
    item_entity_restore(items_back.e, items_back.n);
    item_entity_tick(NULL, 0.0, 0.0, 0.0);
    CHECK(item_entity_at(0)->age == 300 && item_entity_at(0)->body.y == 30.0,
          "an item in an unloaded chunk aged or fell (age %u, y %.2f)", item_entity_at(0)->age,
          item_entity_at(0)->body.y);
    item_entity_reset();
    worldstore_close();

    // Renaming touches the name only.
    CHECK(worldstore_rename("slot3", "Renamed"), "rename failed");
    CHECK(worldstore_open("slot3", &meta, &back, NULL), "reopening after the rename failed");
    CHECK(strcmp(meta.name, "Renamed") == 0 && meta.seed == 99u, "rename lost the seed or the name");
    CHECK(back.inv[4].count == 37 && back.y == 11.0, "rename lost the player");
    worldstore_close();

    // THE TESTWORLD. A pre-slots world, with a chunk in it, goes into
    // the first free slot under its new name, with its terrain and its
    // player intact -- and a second start finds nothing more to do.
    write_legacy_level("flyover", 40.5, 30.0, -7.5);
    CHECK(worldstore_open("flyover", &meta, &back, NULL), "the legacy world does not open as it is");
    chunk_t c;
    fill_chunk(&c, g_ia, g_sa, 2, 3, meta.seed);
    g_ia[CH_IDX(5, 40, 5)] = BLK_GLASS;
    g_sa[CH_IDX(5, 40, 5)] = ST_PLACED;
    CHECK(world_chunk_save(&c), "could not save a chunk into the legacy world");
    worldstore_close();

    int const slot = worldstore_adopt_legacy("flyover", "Testworld");
    printf("  the legacy world went to slot %d\n", slot + 1);
    CHECK(slot == 0, "the legacy world went to slot %d, not the first free one", slot + 1);
    CHECK(worldstore_slot_peek(0, &peek) && strcmp(peek.name, "Testworld") == 0, "slot 1 is not called Testworld");
    CHECK(peek.seed == 0xC0FFEEu, "the Testworld lost its seed");
    CHECK(!worldstore_open("flyover", &meta, &back, NULL), "the legacy world is still where it was");
    CHECK(worldstore_open("slot1", &meta, &back, NULL), "the adopted world does not open");
    CHECK(back.placed && back.x == 40.5 && back.y == 30.0, "a legacy player who had played was not put back exactly");
    CHECK(!back.has_inv, "a legacy player got an inventory they never saved");
    CHECK(meta.time_of_day == 7777, "the clock from the old player record was not moved to the world (%lld)",
          (long long)meta.time_of_day);
    chunk_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.id = g_ib;
    cb.st = g_sb;
    cb.cx = 2;
    cb.cz = 3;
    CHECK(world_chunk_load(&cb) == 1, "the adopted world lost its terrain");
    CHECK(g_ib[CH_IDX(5, 40, 5)] == BLK_GLASS && (g_sb[CH_IDX(5, 40, 5)] & ST_PLACED) != 0,
          "the player's block did not come with the world");
    worldstore_close();
    CHECK(worldstore_adopt_legacy("flyover", "Testworld") == -1, "the adoption is not idempotent");
    CHECK(worldstore_slot_peek(2, &peek) && strcmp(peek.name, "Renamed") == 0, "the adoption disturbed slot 3");

    // A legacy player who never left through Esc still has the default
    // at the spawn column's centre: that is a guess, not a position.
    write_legacy_level("flyover", 0.5, (double)CH_SEA_LEVEL + 2.0, 0.5);
    CHECK(worldstore_open("flyover", &meta, &back, NULL), "the second legacy world does not open");
    CHECK(!back.placed, "the untouched default position was taken as a real one");
    worldstore_close();
    CHECK(worldstore_adopt_legacy("flyover", "Testworld") == 1, "with slot 1 taken, the next free slot is 2");

    // A world from a newer build is not an empty slot: it is told apart,
    // and nothing may be created over it. Nor is a damaged one.
    {
        char dir[192], path[224];
        snprintf(dir, sizeof(dir), "%s/worlds/slot8", STORE_BASE);
        CHECK(cm_mkdir_p(dir), "could not make slot 8's directory");
        snprintf(path, sizeof(path), "%s/level.cmw", dir);
        FILE* f = fopen(path, "wb");
        if (f != NULL) {
            fwrite("CMW9 something a later build understands", 1, 41, f);
            fclose(f);
        }
        CHECK(worldstore_slot_state(7, &peek) == SLOT_NEWER, "a world from a newer build does not read as newer");
        CHECK(!worldstore_create_in(7, "Over it", 1u, &meta, &player), "a world was created over a newer build's");
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite("CMW1 not nbt at all", 1, 19, f);
            fclose(f);
        }
        CHECK(worldstore_slot_state(7, &peek) == SLOT_DAMAGED, "an unreadable level.cmw does not read as damaged");
        CHECK(worldstore_slot_state(6, &peek) == SLOT_EMPTY, "an empty slot does not read as empty");
        CHECK(worldstore_slot_state(0, &peek) == SLOT_WORLD && strcmp(peek.name, "Testworld") == 0,
              "a readable world does not read as a world");
        worldstore_delete("slot8");
        CHECK(worldstore_slot_state(7, &peek) == SLOT_EMPTY, "deleting a damaged world did not free its slot");
    }

    // Deleting frees the slot, directory and all.
    CHECK(worldstore_delete("slot3"), "deleting slot 3 failed");
    CHECK(!worldstore_slot_peek(2, &peek), "slot 3 still shows a world");
    CHECK(worldstore_create_in(2, "Again", 5u, &meta, &player), "the freed slot could not be reused");
    worldstore_close();
    clear_store();
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
        int total = 0, surface = 0, empty = 0;
        for (int sect = 0; sect < CH_SECT_N; sect++) {
            mesh_t m;
            CHECK(chunkmesh_build(0, 0, lod, sect, scratch, &m), "chunkmesh_build failed at lod %d section %d", lod,
                  sect);
            total += m.tn;
            if (m.tn == 0) empty++;
            CHECK(m.vn <= 40000, "lod %d section %d produced %d vertices, close to mesh_t's 65535 limit (F-13)", lod,
                  sect, m.vn);

            // Every greedy face must carry a direction, or the
            // renderer's cull silently drops it.
            int none = 0;
            for (int i = 0; i < m.tn; i++) none += (m.t[i].dir == MESH_DIR_NONE);
            CHECK(none == 0 || lod == LOD_FANCY, "lod %d section %d has %d triangles with no face direction", lod,
                  sect, none);

            // The mesh must fit inside ITS SECTION's box, or the
            // renderer's per-section bounding box and its frustum cull
            // are both lies -- and a section drawn in the wrong band is
            // exactly the bug sectioning could introduce (D-34).
            float const ylo = (float)(sect * CH_SECT), yhi = ylo + (float)CH_SECT;
            for (int i = 0; i < m.vn; i++) {
                CHECK(m.v[i].x >= -0.01f && m.v[i].x <= (float)CH_W + 0.01f,
                      "lod %d section %d vertex %d is outside the chunk in x (%g)", lod, sect, i, m.v[i].x);
                CHECK(m.v[i].z >= -0.01f && m.v[i].z <= (float)CH_D + 0.01f,
                      "lod %d section %d vertex %d is outside the chunk in z (%g)", lod, sect, i, m.v[i].z);
                CHECK(m.v[i].y >= ylo - 0.01f && m.v[i].y <= yhi + 0.01f,
                      "lod %d section %d vertex %d is at y %g, outside its band %g..%g", lod, sect, i, m.v[i].y, ylo,
                      yhi);
                if (s_fail) break;
            }

            // Which section the ground is in, for the line below: the
            // one holding the most triangles.
            if (m.tn > surface) surface = m.tn;
            mesh_free(&m);
            if (s_fail) break;
        }
        printf("  lod %d: %d tris over %d sections (%d empty, biggest %d)\n", lod, total, CH_SECT_N, empty, surface);
        CHECK(total > 0, "lod %d produced no triangles at all", lod);
        if (s_fail) break;
    }

    // Where a chunk's triangles sit, per section (D-34).
    //
    // THIS IS THE ORIGIN, AND THE ORIGIN IS OCEAN. Do not read a claim
    // about the world out of these three numbers -- that is exactly the
    // mistake F-33 made, and D-37 is the rule that came out of it. The
    // world-wide figures are in F-35, measured over 49 chunks spread
    // across 4000 blocks: 34% underground, 47% surface, 17% above. This
    // print is here so that a change which quietly moves geometry
    // between sections shows up in `make check` at all.
    if (!s_fail) {
        int by_sect[CH_SECT_N];
        memset(by_sect, 0, sizeof(by_sect));
        int chunks = 0;
        for (int32_t cz = -1; cz <= 1; cz++) {
            for (int32_t cx = -1; cx <= 1; cx++) {
                if (chunk_find(cx, cz) == NULL) continue;
                chunks++;
                for (int sect = 0; sect < CH_SECT_N; sect++) {
                    mesh_t m;
                    if (!chunkmesh_build(cx, cz, LOD_FAST, sect, scratch, &m)) continue;
                    by_sect[sect] += m.tn;
                    mesh_free(&m);
                }
            }
        }
        int total = 0, top = 0;
        for (int i = 0; i < CH_SECT_N; i++) {
            total += by_sect[i];
            if (by_sect[i] > by_sect[top]) top = i;
        }
        printf("  the origin 3x3 (ocean, not typical -- F-35): %d chunks, %d tris: ", chunks, total);
        for (int i = 0; i < CH_SECT_N; i++) {
            printf("y %2d-%2d: %d%s", i * CH_SECT, (i + 1) * CH_SECT - 1, by_sect[i], i + 1 < CH_SECT_N ? ", " : "");
        }
        printf("\n");
        CHECK(by_sect[top] > 0, "every section of the origin 3x3 meshed to nothing");
    }

    // The Far Lands wall looked like the most face-dense terrain there
    // could be: a full-height chunk of holes. Its meshes must fit mesh_t
    // with room to spare (F-13), at every level of detail. (Measured: it
    // is the other way round. The tunnels do not change along x, so
    // nearly every face runs the chunk's whole width and the greedy
    // mesher takes it in one rectangle -- a couple of hundred triangles
    // a chunk.)
    if (!s_fail) {
        int32_t const fx = FARLANDS_X_DEFAULT / CH_W - 2;  // two chunks into the wall
        for (int32_t cz = -1; cz <= 1; cz++)
            for (int32_t cx = fx - 1; cx <= fx + 1; cx++) chunk_worker_request_load(cx, cz);
        int most = 0, tris = 0;
        for (int lod = 0; lod < LOD_COUNT; lod++) {
            for (int sect = 0; sect < CH_SECT_N; sect++) {
                mesh_t m;
                CHECK(chunkmesh_build(fx, 0, lod, sect, scratch, &m), "a Far Lands chunk failed to mesh (lod %d, section %d)",
                      lod, sect);
                if (m.vn > most) most = m.vn;
                if (lod == LOD_FANCY) tris += m.tn;
                CHECK(m.vn <= 40000, "a Far Lands section meshed to %d vertices (lod %d), close to mesh_t's 65535 (F-13)",
                      m.vn, lod);
                mesh_free(&m);
            }
        }
        printf("  a Far Lands chunk: %d triangles in full detail, at most %d vertices a section\n", tris, most);
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

// --- The player ------------------------------------------------------
//
// Physics, picking and the felling rule are pure, so all three are
// tested here in seconds rather than by walking into things on the
// badge. A hand-built chunk is the test fixture: exact terrain, no
// generation, no seed.

// Claim chunk (0,0) and its ring, fill them with a flat floor at y, and
// hand back the chunk so a test can carve shapes into it.
static chunk_t* flat_world(int floor_y) {
    for (int32_t cz = -1; cz <= 1; cz++) {
        for (int32_t cx = -1; cx <= 1; cx++) {
            chunk_t* c = chunk_claim(cx, cz);
            if (c == NULL) continue;
            memset(c->id, BLK_AIR, CH_CELLS);
            memset(c->st, 0, CH_CELLS);
            for (int z = 0; z < CH_D; z++) {
                for (int x = 0; x < CH_W; x++) {
                    for (int y = 0; y < floor_y; y++) c->id[CH_IDX(x, y, z)] = BLK_STONE;
                }
            }
            c->cstate = CS_READY;
            chunk_resummarise(c);
        }
    }
    return chunk_find(0, 0);
}

static void set_block(int32_t x, int32_t y, int32_t z, uint8_t b, uint8_t st) {
    chunk_t* c = chunk_find(chunk_of(x), chunk_of(z));
    if (c == NULL) return;
    c->id[CH_IDX(chunk_off(x), y, chunk_off(z))] = b;
    c->st[CH_IDX(chunk_off(x), y, chunk_off(z))] = st;
    chunk_resummarise(c);
}

// Light (world/light.h): the floods have to agree with the rule they
// implement, both ways -- light arriving where it should, and going
// away again when its source does. A solid world 20 deep with rooms
// carved into it, lit and changed through world_set like the game does.
static int lsky(int32_t x, int32_t y, int32_t z) {
    return light_sky(world_light(x, y, z));
}
static int lblk(int32_t x, int32_t y, int32_t z) {
    return light_block(world_light(x, y, z));
}

static void check_light(void) {
    printf("light\n");
    CHECK(light_init(), "light_init failed");
    chunk_store_clear();  // flat_world will not reuse a chunk an earlier check edited
    CHECK(flat_world(20) != NULL, "the light world would not become resident");
    for (int32_t cz = -1; cz <= 1; cz++)
        for (int32_t cx = -1; cx <= 1; cx++) light_chunk_ready(chunk_find(cx, cz));

    CHECK(lsky(3, 20, 3) == 15 && lsky(3, 40, 3) == 15, "open air above the ground is not full sky");
    CHECK(lsky(3, 19, 3) == 0, "solid stone carries sky light");

    // A sealed room underground: dark.
    for (int x = 2; x <= 8; x++)
        for (int z = 2; z <= 8; z++)
            for (int y = 10; y <= 12; y++) world_set(x, y, z, BLK_AIR, 0);
    CHECK(lsky(5, 11, 5) == 0 && lblk(5, 11, 5) == 0, "a sealed room is not dark");

    // A torch lights it, one level less per block, and takes it back.
    world_set(5, 10, 5, BLK_TORCH, ST_PLACED);
    printf("  torch: %d at it, %d one away, %d three away, %d round a corner\n", lblk(5, 10, 5), lblk(6, 10, 5),
           lblk(8, 10, 5), lblk(8, 12, 8));
    CHECK(lblk(5, 10, 5) == 14, "a torch's own cell is %d, not 14", lblk(5, 10, 5));
    CHECK(lblk(6, 10, 5) == 13 && lblk(8, 10, 5) == 11, "torchlight does not fall off one level a block");
    CHECK(lblk(8, 12, 8) == 14 - (3 + 2 + 3), "torchlight does not travel by the Manhattan path");
    CHECK(lblk(5, 10, 10) == 0, "torchlight went through solid stone");
    world_set(5, 10, 5, BLK_AIR, 0);
    int left = 0;
    for (int x = 2; x <= 8; x++)
        for (int z = 2; z <= 8; z++)
            for (int y = 10; y <= 12; y++) left += lblk(x, y, z);
    CHECK(left == 0, "removing the torch left %d levels of its light behind", left);

    // A shaft to the surface: daylight falls straight down it undimmed,
    // spreads into the room, and goes when the shaft is capped.
    for (int y = 13; y <= 19; y++) world_set(5, y, 5, BLK_AIR, 0);
    printf("  shaft: %d at its foot, %d beside it, %d in the far corner\n", lsky(5, 10, 5), lsky(6, 10, 5),
           lsky(2, 10, 2));
    CHECK(lsky(5, 12, 5) == 15 && lsky(5, 10, 5) == 15, "daylight does not reach the foot of an open shaft");
    CHECK(lsky(6, 10, 5) == 14, "daylight does not spread from the shaft into the room");
    world_set(5, 19, 5, BLK_STONE, ST_PLACED);
    CHECK(lsky(5, 10, 5) == 0 && lsky(6, 11, 5) == 0 && lsky(5, 18, 5) == 0, "capping the shaft did not darken it");
    world_set(5, 19, 5, BLK_AIR, 0);
    CHECK(lsky(5, 10, 5) == 15, "uncapping the shaft did not bring the daylight back");

    // Leaves and water dim light rather than stop it.
    CHECK(light_filter(BLK_LEAVES) == 1 && light_filter(BLK_WATER) == 2 && light_filter(BLK_GLASS) == 0 &&
              light_filter(BLK_STONE) == 15 && light_filter(BLK_TORCH) == 0,
          "the light filters are not what light.h says");

    // Across a chunk border, and into a chunk that arrives afterwards.
    for (int x = 12; x <= 19; x++) world_set(x, 11, 5, BLK_AIR, 0);  // a tunnel through x = 15|16
    world_set(15, 11, 5, BLK_TORCH, ST_PLACED);
    CHECK(lblk(16, 11, 5) == 13 && lblk(19, 11, 5) == 10, "torchlight stops at the chunk border");
    chunk_t* nb = chunk_find(1, 0);
    memset(nb->lt, 0, CH_CELLS);  // as if (1, 0) had only just arrived
    light_chunk_ready(nb);
    CHECK(lblk(16, 11, 5) == 13 && lblk(19, 11, 5) == 10, "a chunk arriving next to a torch was not lit by it");
    CHECK(lsky(20, 20, 3) == 15, "the newly arrived chunk has no daylight");
    world_set(15, 11, 5, BLK_AIR, 0);
    CHECK(lblk(17, 11, 5) == 0, "removing a torch left its light in the next chunk");
    chunk_store_clear();
}

// A replay is a start and a stream of per-tick inputs; it has to come
// back from the card exactly, gyro turns included, or it replays some
// other walk.
static void check_replay(void) {
    printf("replays\n");
    replay_start_t st = {.seed = 0xC0FFEEu, .time_of_day = 4242, .x = 12.5, .y = 30.0, .z = -7.25,
                         .yaw = 1.25f, .pitch = -0.3f, .selected = 3};
    st.inv[0] = (inv_slot_t){ITEM_PICK_STONE, 1, 9};
    st.inv[5] = (inv_slot_t){BLK_TORCH, 17, 0};
    CHECK(replay_record_begin(&st), "could not start recording");
    for (int i = 0; i < 300; i++) replay_record_tick((uint32_t)(i * 2654435761u), (float)i * 0.01f, -(float)i * 0.02f);
    CHECK(replay_record_end("build/host/test.cmr"), "could not write the replay");
    replay_start_t back;
    CHECK(replay_load("build/host/test.cmr", &back), "could not read the replay back");
    CHECK(back.seed == st.seed && back.time_of_day == 4242 && back.x == 12.5 && back.z == -7.25 &&
              back.yaw == 1.25f && back.selected == 3,
          "the replay's start did not survive");
    CHECK(back.inv[0].item == ITEM_PICK_STONE && back.inv[0].wear == 9 && back.inv[5].count == 17,
          "the replay's inventory did not survive");
    int      n  = 0;
    bool     ok = true;
    uint32_t m;
    float    gy, gp;
    while (replay_next(&m, &gy, &gp)) {
        ok = ok && m == (uint32_t)(n * 2654435761u) && gy == (float)n * 0.01f && gp == -(float)n * 0.02f;
        n++;
    }
    CHECK(n == 300 && ok, "the replay played back %d ticks%s", n, ok ? "" : ", not the ones recorded");
    CHECK(!replay_playing(), "a finished replay says it is still playing");
}

static void check_physics(void) {
    printf("physics\n");
    CHECK(flat_world(8) != NULL, "the test world would not become resident");

    phys_body_t b;

    // Falls onto the floor and stops exactly on top of it.
    phys_body_init(&b, 8.5, 20.0, 8.5);
    for (int i = 0; i < 200; i++) phys_move(&b, 0.0, -0.4, 0.0);
    printf("  fell to y = %.3f (floor top is 8)\n", b.y);
    CHECK(b.on_ground, "a body that fell 12 blocks is not on the ground");
    CHECK(b.y > 7.99 && b.y < 8.01, "a body came to rest at y %g, expected 8", b.y);

    // A fast fall must not pass through the floor: 3 blocks a tick is
    // terminal velocity and the floor is 8 thick, but one sub-step must
    // never cross more than a block.
    phys_body_init(&b, 8.5, 40.0, 8.5);
    for (int i = 0; i < 60; i++) phys_move(&b, 0.0, -3.0, 0.0);
    CHECK(b.y > 7.99 && b.y < 8.01, "at terminal velocity a body tunnelled to y %g", b.y);

    // Walks into a wall and stops against it, without stopping dead in
    // the other axis (it must slide).
    set_block(11, 8, 8, BLK_STONE, 0);
    set_block(11, 9, 8, BLK_STONE, 0);
    phys_body_init(&b, 8.5, 8.0, 8.5);
    for (int i = 0; i < 40; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  stopped at x = %.3f against a wall whose face is at 11\n", b.x);
    CHECK(b.x < 10.71 && b.x > 10.69, "a body stopped at x %g, expected 10.70 (11 - 0.3)", b.x);
    CHECK(b.hit_x, "a body against a wall does not report hit_x");

    // A one-block step is walked up without jumping: a plateau at y = 8,
    // wide enough that the walk ends standing ON it rather than having
    // crossed it and dropped off the far side.
    CHECK(flat_world(8) != NULL, "the test world would not rebuild");
    for (int32_t x = 11; x <= 24; x++) {
        for (int32_t z = 6; z <= 10; z++) set_block(x, 8, z, BLK_STONE, 0);
    }
    phys_body_init(&b, 8.5, 8.0, 8.5);
    for (int i = 0; i < 40; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  after walking at a 1-block step: x %.2f, y %.2f\n", b.x, b.y);
    CHECK(b.x > 12.0, "a body stopped at a 1-block rise instead of stepping up (x %g)", b.x);
    CHECK(b.y > 8.99 && b.y < 9.01, "a body is at y %g on top of a 1-block rise, expected 9", b.y);
    CHECK(b.on_ground, "a body that stepped up is not on the ground");

    // Two of them in a row: a staircase is walkable, which is what the
    // step is for.
    for (int32_t x = 15; x <= 24; x++) {
        for (int32_t z = 6; z <= 10; z++) set_block(x, 9, z, BLK_STONE, 0);
    }
    phys_body_init(&b, 8.5, 8.0, 8.5);
    for (int i = 0; i < 60; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  after a two-step staircase: x %.2f, y %.2f\n", b.x, b.y);
    CHECK(b.y > 9.99 && b.y < 10.01, "a body is at y %g after two steps, expected 10", b.y);

    // A two-block wall is NOT climbed. This is the bound that keeps
    // the step from being a cheat: walls stay walls.
    CHECK(flat_world(8) != NULL, "the test world would not rebuild");
    for (int32_t z = 6; z <= 10; z++) {
        set_block(14, 8, z, BLK_STONE, 0);
        set_block(14, 9, z, BLK_STONE, 0);
    }
    phys_body_init(&b, 8.5, 8.0, 8.5);
    for (int i = 0; i < 60; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  against a 2-block wall: x %.2f, y %.2f\n", b.x, b.y);
    CHECK(b.y < 8.6, "a body climbed a 2-block wall (y %g)", b.y);
    CHECK(b.x < 13.8, "a body passed through a 2-block wall (x %g)", b.x);

    // A 2-block gap is walkable; a 1-block one is not (the player is
    // 1.8 tall).
    CHECK(flat_world(8) != NULL, "the test world would not rebuild");
    for (int32_t x = 20; x <= 26; x++) {
        set_block(x, 10, 8, BLK_STONE, 0);  // a ceiling 2 blocks above the floor
    }
    phys_body_init(&b, 19.5, 8.0, 8.5);
    for (int i = 0; i < 60; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  through a 2-high gap: x %.2f\n", b.x);
    CHECK(b.x > 26.0, "a body could not walk through a 2-block-high gap (x %g)", b.x);

    for (int32_t x = 30; x <= 36; x++) {
        set_block(x, 9, 8, BLK_STONE, 0);  // a ceiling 1 block above the floor
    }
    phys_body_init(&b, 29.5, 8.0, 8.5);
    for (int i = 0; i < 60; i++) phys_move(&b, 0.2, -0.1, 0.0);
    printf("  at a 1-high gap: x %.2f (should be stopped near 30)\n", b.x);
    CHECK(b.x < 30.0, "a body 1.8 tall walked through a 1-block-high gap (x %g)", b.x);

    // THE JUMP ARC, asserted rather than felt.
    //
    // It has to clear a whole block with room to spare, because placing
    // a block under yourself is how you get out of a hole and it needs
    // the apex to last long enough to press a key in. The first version
    // of this peaked at 0.83 blocks -- it could not clear a one-block
    // ledge -- and nothing in the code said so: it took simulating the
    // arc to see it, which is exactly what this does.
    CHECK(flat_world(8) != NULL, "the test world would not rebuild");
    phys_body_init(&b, 8.5, 8.0, 8.5);
    {
        double const start = b.y;
        double       peak  = b.y;
        int          up = 0, total = 0;
        b.vy = PL_JUMP;  // as player_tick does on the ground
        for (int t = 0; t < 200; t++) {
            // phys_move then phys_gravity: exactly what player_tick
            // does, because it is the same two calls and not a copy of
            // them.
            phys_move(&b, 0.0, (double)b.vy, 0.0);
            phys_gravity(&b, PL_GRAVITY, PL_DRAG, PL_TERMINAL);
            total++;
            if (b.y > peak) {
                peak = b.y;
                up   = total;
            }
            if (b.on_ground && total > 2) break;
        }
        printf("  jump: apex %.2f blocks, %.2f s up, %.2f s in the air\n", peak - start, (double)up / 20.0,
               (double)total / 20.0);
        CHECK(peak - start > 1.15, "a jump reaches %g blocks: it cannot clear one with room to place under itself",
              peak - start);
        CHECK(peak - start < 2.0, "a jump reaches %g blocks, which clears two: that is a bug, not a feature",
              peak - start);
        CHECK(up >= 6, "a jump reaches its apex in %d ticks (%.2f s); too quick to aim a placement at", up,
              (double)up / 20.0);
        CHECK(b.y > 7.99 && b.y < 8.01, "a jump landed at y %g instead of back on the floor", b.y);
    }

    // And it lands ON a one-block ledge rather than bouncing off it.
    for (int32_t x = 11; x <= 14; x++) {
        for (int32_t z = 6; z <= 10; z++) set_block(x, 8, z, BLK_STONE, 0);
    }
    phys_body_init(&b, 9.5, 8.0, 8.5);
    b.vy = PL_JUMP;
    for (int t = 0; t < 60; t++) {
        phys_move(&b, 0.15, (double)b.vy, 0.0);
        phys_gravity(&b, PL_GRAVITY, PL_DRAG, PL_TERMINAL);
        if (b.on_ground && t > 2) break;
    }
    printf("  jumping onto a 1-block ledge landed at x %.2f, y %.2f\n", b.x, b.y);
    CHECK(b.y > 8.99 && b.y < 9.01, "a jump onto a 1-block ledge ended at y %g, expected 9", b.y);

    // The edge of the world is a wall, not a hole (D-14). A clean
    // world for this one: the obstacles above are in the way.
    CHECK(flat_world(8) != NULL, "the test world would not rebuild");
    phys_body_init(&b, 8.5, 8.0, 8.5);
    for (int i = 0; i < 400; i++) phys_move(&b, 0.25, -0.1, 0.0);
    printf("  walking off the resident set stopped at x = %.1f\n", b.x);
    CHECK(b.x < 48.0, "a body walked out of the resident world to x %g", b.x);
}

// A brute-force march, fine enough that it cannot miss a block: the
// reference the DDA has to agree with.
static bool brute_pick(double ox, double oy, double oz, float dx, float dy, float dz, float max, int32_t* bx,
                       int32_t* by, int32_t* bz) {
    float const len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return false;
    double const ux = dx / len, uy = dy / len, uz = dz / len;
    for (double t = 0.0; t <= (double)max; t += 0.0005) {
        int32_t const x = (int32_t)floor(ox + ux * t);
        int32_t const y = (int32_t)floor(oy + uy * t);
        int32_t const z = (int32_t)floor(oz + uz * t);
        if (!block_solid(world_block(x, y, z))) continue;
        *bx = x;
        *by = y;
        *bz = z;
        return true;
    }
    return false;
}

static void check_raycast(void) {
    printf("picking\n");
    CHECK(flat_world(8) != NULL, "the test world would not become resident");
    set_block(10, 9, 8, BLK_COBBLE, 0);
    set_block(10, 10, 8, BLK_COBBLE, 0);
    set_block(6, 9, 12, BLK_LOG, 0);

    // The face reported must be the one the ray came in through, and
    // the placement cell must be the empty one in front of it.
    ray_hit_t h;
    CHECK(ray_pick(8.5, 9.5, 8.5, 1.0f, 0.0f, 0.0f, RAY_REACH, true, &h), "a ray straight at a block missed it");
    printf("  hit (%d,%d,%d) face %u, place at (%d,%d,%d), %.2f blocks away\n", h.x, h.y, h.z, h.face, h.px, h.py,
           h.pz, h.dist);
    CHECK(h.x == 10 && h.y == 9 && h.z == 8, "hit (%d,%d,%d), expected (10,9,8)", h.x, h.y, h.z);
    CHECK(h.face == MESH_DIR_NX, "face %u, expected -x (%u)", h.face, MESH_DIR_NX);
    CHECK(h.px == 9 && h.py == 9 && h.pz == 8, "placement cell (%d,%d,%d), expected (9,9,8)", h.px, h.py, h.pz);
    CHECK(!block_solid(world_block(h.px, h.py, h.pz)), "the placement cell is not empty");

    // Reach: the same ray from further away finds nothing.
    CHECK(!ray_pick(0.5, 9.5, 8.5, 1.0f, 0.0f, 0.0f, RAY_REACH, true, &h), "a ray reached further than RAY_REACH");

    // A placed torch can be pointed at (F-56): the crosshair ray is the
    // non-solid one, and it must stop at the torch -- while a solid-only
    // ray looks straight through it. Water is looked through by both.
    set_block(8, 9, 11, BLK_TORCH, ST_PLACED);
    CHECK(ray_pick(8.5, 9.5, 8.5, 0.0f, 0.0f, 1.0f, RAY_REACH, false, &h) && h.block == BLK_TORCH && h.z == 11,
          "the crosshair ray does not stop at a placed torch");
    CHECK(!ray_pick(8.5, 9.5, 8.5, 0.0f, 0.0f, 1.0f, RAY_REACH, true, &h) || h.block != BLK_TORCH,
          "a solid-only ray stopped at a torch");
    set_block(8, 9, 11, BLK_AIR, 0);
    set_block(8, 11, 8, BLK_WATER, 0);
    set_block(8, 10, 8, BLK_WATER, 0);
    CHECK(ray_pick(8.5, 12.5, 8.5, 0.0f, -1.0f, 0.0f, RAY_REACH, false, &h) && h.y == 7,
          "water hid the floor from the crosshair (hit y %d)", h.y);
    set_block(8, 11, 8, BLK_AIR, 0);
    set_block(8, 10, 8, BLK_AIR, 0);

    // Straight down finds the floor.
    CHECK(ray_pick(8.5, 12.0, 8.5, 0.0f, -1.0f, 0.0f, RAY_REACH, true, &h), "a ray straight down missed the floor");
    CHECK(h.y == 7 && h.face == MESH_DIR_PY, "downward ray hit y %d face %u, expected y 7 face +y", h.y, h.face);

    // An unloaded chunk is solid to the BODY (D-14) and invisible to
    // the PICKER. Reporting it would put a highlight box round a piece
    // of fog and offer to mine it.
    {
        ray_hit_t hb;
        CHECK(!ray_pick(8.5, 9.5, 8.5, 0.0f, 0.0f, -1.0f, 200.0f, true, &hb),
              "a ray picked BLK_BARRIER: the edge of the loaded world is not a block");
        phys_body_t edge;
        phys_body_init(&edge, 8.5, 9.0, 8.5);
        CHECK(!phys_fits(&edge, 8.5, 9.0, -100.0), "an unloaded chunk is not solid to the body (D-14)");
    }

    // And the real test: a fan of directions, every one of which must
    // agree with a brute-force march. A DDA that skips a corner is the
    // classic bug and it is invisible until someone mines through a
    // wall diagonally.
    int checked = 0, agreed = 0;
    for (int a = 0; a < 64; a++) {
        for (int e = -12; e <= 12; e += 3) {
            float const yaw = (float)a * 0.0982f, pitch = (float)e * 0.09f;
            float       dx, dy, dz;
            ray_forward(yaw, pitch, &dx, &dy, &dz);
            double const ox = 8.37, oy = 9.61, oz = 8.23;  // deliberately not on a boundary
            int32_t      bx = 0, by = 0, bz = 0;
            bool const   want = brute_pick(ox, oy, oz, dx, dy, dz, RAY_REACH, &bx, &by, &bz);
            bool const   got  = ray_pick(ox, oy, oz, dx, dy, dz, RAY_REACH, true, &h);
            checked++;
            CHECK(want == got, "yaw %g pitch %g: brute force says %d, the DDA says %d", (double)yaw, (double)pitch,
                  want, got);
            if (!want || !got) continue;
            CHECK(h.x == bx && h.y == by && h.z == bz, "yaw %g pitch %g: DDA hit (%d,%d,%d), brute force (%d,%d,%d)",
                  (double)yaw, (double)pitch, h.x, h.y, h.z, bx, by, bz);
            agreed++;
            if (s_fail) return;
        }
    }
    printf("  %d directions checked against a brute-force march, %d hits, all agreed\n", checked, agreed);
}

static void check_felling(void) {
    printf("the logging rule\n");
    CHECK(flat_world(8) != NULL, "the test world would not become resident");
    item_entity_reset();

    // A tree: a trunk with a canopy, all GROWN (no ST_PLACED).
    int32_t const tx = 8, tz = 8, base = 8;
    for (int y = 0; y < 5; y++) set_block(tx, base + y, tz, BLK_LOG, 0);
    int leaves = 0;
    for (int dy = 3; dy <= 5; dy++) {
        for (int dz = -2; dz <= 2; dz++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dz == 0 && dy < 5) continue;
                set_block(tx + dx, base + dy, tz + dz, BLK_LEAVES, 0);
                leaves++;
            }
        }
    }
    // A SECOND tree far enough away that its canopy does not touch.
    for (int y = 0; y < 4; y++) set_block(tx + 12, base + y, tz, BLK_LOG, 0);

    // A player-placed log, in the first tree's trunk.
    set_block(tx, base + 2, tz, BLK_LOG, ST_PLACED);

    // Breaking the PLACED one takes exactly that block.
    break_result_t r = interact_break(tx, base + 2, tz, ITEM_AXE_STONE);
    printf("  breaking a placed log took %d block(s), tree=%d\n", r.felled, (int)r.was_tree);
    CHECK(r.ok, "breaking a placed log failed");
    CHECK(!r.was_tree, "breaking a PLACED log felled the tree");
    CHECK(r.felled == 1, "breaking a placed log took %d blocks, expected 1", r.felled);
    CHECK(world_block(tx, base + 3, tz) == BLK_LOG, "the trunk above a placed log was taken");

    // Breaking a GROWN one fells everything from there up.
    r = interact_break(tx, base + 3, tz, ITEM_AXE_STONE);
    printf("  breaking a grown log took %d block(s), tree=%d\n", r.felled, (int)r.was_tree);
    CHECK(r.ok && r.was_tree, "breaking a grown log did not fell the tree");
    CHECK(r.felled > 20, "felling took only %d blocks; the canopy should have gone too", r.felled);

    // The stump stays: y >= the broken block, never below.
    CHECK(world_block(tx, base, tz) == BLK_LOG, "felling took the stump at the bottom of the trunk");
    CHECK(world_block(tx, base + 1, tz) == BLK_LOG, "felling reached below the block that was broken");
    // Everything at and above it is gone.
    CHECK(world_block(tx, base + 4, tz) == BLK_AIR, "felling left a log above the break");
    int left = 0;
    for (int dy = 3; dy <= 5; dy++) {
        for (int dz = -2; dz <= 2; dz++) {
            for (int dx = -2; dx <= 2; dx++) left += world_block(tx + dx, base + dy, tz + dz) == BLK_LEAVES;
        }
    }
    CHECK(left == 0, "%d leaves survived the fell", left);

    // The NEIGHBOUR is untouched. This is the bound that matters: one
    // tree must never take the forest.
    int neighbour = 0;
    for (int y = 0; y < 4; y++) neighbour += world_block(tx + 12, base + y, tz) == BLK_LOG;
    printf("  the neighbouring tree still has %d of its 4 logs\n", neighbour);
    CHECK(neighbour == 4, "felling one tree took %d logs off a tree 12 blocks away", 4 - neighbour);

    // Placing sets ST_PLACED, which is what makes all of the above work.
    ray_hit_t h = {.x = 20, .y = base, .z = 20, .px = 20, .py = base, .pz = 20, .face = MESH_DIR_PY};
    CHECK(interact_place(&h, BLK_LOG, NULL), "placing a log failed");
    CHECK(world_block(20, base, 20) == BLK_LOG, "the placed log is not there");
    CHECK((world_state(20, base, 20) & ST_PLACED) != 0, "a placed block does not have ST_PLACED set");
    r = interact_break(20, base, 20, ITEM_AXE_STONE);
    CHECK(!r.was_tree && r.felled == 1, "a just-placed log felled as a tree");
}

static void check_items(void) {
    printf("items and the inventory\n");

    // The two id spaces meet without a gap, which is what lets a block
    // be an item without a second table to keep in step.
    CHECK(item_is_block(BLK_COBBLE), "a block id is not an item");
    CHECK(!item_is_block(ITEM_COAL), "coal is being treated as a block");
    CHECK(item_block(BLK_COBBLE) == BLK_COBBLE, "a block item does not place its own block");
    CHECK(item_block(ITEM_COAL) == BLK_AIR, "coal claims to place a block");
    CHECK(item_def(BLK_COBBLE).name != NULL && item_def(BLK_COBBLE).name[0] != 0,
          "a block item has no name; the block table should have supplied it");

    // Tools speed up their own class and nothing else. A pickaxe that
    // digs dirt faster makes carrying a shovel pointless.
    int const stone_hand  = item_break_ticks(BLK_STONE, 0);
    int const stone_pick  = item_break_ticks(BLK_STONE, ITEM_PICK_STONE);
    int const stone_shov  = item_break_ticks(BLK_STONE, ITEM_SHOVEL_STONE);
    int const dirt_hand   = item_break_ticks(BLK_DIRT, 0);
    int const dirt_shovel = item_break_ticks(BLK_DIRT, ITEM_SHOVEL_STONE);
    printf("  stone: %d ticks by hand, %d with a stone pickaxe, %d with a shovel\n", stone_hand, stone_pick,
           stone_shov);
    printf("  dirt:  %d ticks by hand, %d with a stone shovel\n", dirt_hand, dirt_shovel);
    CHECK(stone_pick < stone_hand, "a pickaxe does not speed up stone");
    CHECK(stone_shov == stone_hand, "a shovel speeds up stone; only the right class should");
    CHECK(dirt_shovel < dirt_hand, "a shovel does not speed up dirt");
    CHECK(item_break_ticks(BLK_BARRIER, ITEM_PICK_STONE) < 0, "the edge of the world is breakable");

    // Too soft a tool still breaks the block; it just yields nothing.
    CHECK(!item_can_harvest(BLK_STONE, 0), "bare hands harvest stone");
    CHECK(item_can_harvest(BLK_STONE, ITEM_PICK_WOOD), "a wooden pickaxe cannot harvest stone");
    CHECK(item_can_harvest(BLK_DIRT, 0), "bare hands cannot harvest dirt");

    // --- Stacking ----------------------------------------------------
    inventory_t inv;
    inv_clear(&inv);
    CHECK(inv_add(&inv, BLK_COBBLE, 10, 0) == 0, "10 cobblestone would not fit in an empty inventory");
    CHECK(inv_add(&inv, BLK_COBBLE, 10, 0) == 0, "a second 10 would not fit");
    CHECK(inv_count(&inv, BLK_COBBLE) == 20, "20 cobblestone are not all there: %d", inv_count(&inv, BLK_COBBLE));

    // THE POINT: they must be in ONE slot, not two of ten. A partial
    // stack has to be filled before an empty slot is taken.
    int used = 0;
    for (int i = 0; i < INV_SLOTS; i++) used += inv.slot[i].item != 0;
    printf("  20 cobblestone in %d slot(s)\n", used);
    CHECK(used == 1, "20 cobblestone are spread over %d slots; partial stacks are not being filled first", used);

    // Overflow goes to a second slot, and the whole inventory fills.
    inv_clear(&inv);
    int const cap  = INV_SLOTS * ITEM_STACK_MAX;
    int const left = inv_add(&inv, BLK_DIRT, cap + 7, 0);
    printf("  an inventory holds %d dirt; %d of %d were refused\n", cap, left, cap + 7);
    CHECK(left == 7, "a full inventory refused %d, expected 7", left);
    CHECK(inv_count(&inv, BLK_DIRT) == cap, "a full inventory holds %d, expected %d", inv_count(&inv, BLK_DIRT), cap);

    // Tools never stack, and two differently-worn ones stay two.
    inv_clear(&inv);
    inv_add(&inv, ITEM_PICK_STONE, 1, 0);
    inv_add(&inv, ITEM_PICK_STONE, 1, 40);
    used = 0;
    for (int i = 0; i < INV_SLOTS; i++) used += inv.slot[i].item != 0;
    CHECK(used == 2, "two pickaxes merged into %d slot(s): one of them silently repaired", used);

    // --- Durability ---------------------------------------------------
    inv_clear(&inv);
    inv_add(&inv, ITEM_PICK_WOOD, 1, 0);
    uint16_t const life = item_def(ITEM_PICK_WOOD).durability;
    int            uses = 0;
    while (uses < 1000) {
        uses++;
        if (inv_wear_held(&inv, 1)) break;
    }
    printf("  a wooden pickaxe lasted %d uses (durability %u)\n", uses, life);
    CHECK(uses == (int)life, "a pickaxe lasted %d uses, expected %u", uses, life);
    CHECK(inv_held(&inv)->item == 0, "a broken tool left something in the slot");
    CHECK(!inv_wear_held(&inv, 1), "wearing an empty slot reported a break");

    // A block never wears, however hard it is swung.
    inv_clear(&inv);
    inv_add(&inv, BLK_COBBLE, 5, 0);
    CHECK(!inv_wear_held(&inv, 100), "a stack of cobblestone broke like a tool");
    CHECK(inv_held(&inv)->count == 5, "wearing a block stack changed its count");

    // Placing consumes exactly one.
    CHECK(inv_consume_held(&inv), "placing from a stack of 5 failed");
    CHECK(inv_held(&inv)->count == 4, "placing took %d, expected 1", 5 - inv_held(&inv)->count);
}

// The Tab screen draws the hotbar at the BOTTOM and the storage rows
// above it, the reverse of slot order. The cursor has to move the way
// the screen looks, which it did not: up from the hotbar went nowhere.
static void check_inv_cursor(void) {
    printf("the inventory cursor\n");
    inventory_t inv;
    inv_clear(&inv);
    inv.cursor = 2;  // a hotbar slot
    CHECK(inv_screen_row(inv.cursor) == INV_ROWS, "the hotbar is not the bottom row on screen");
    inv_move_cursor(&inv, 0, -1);
    CHECK(inv_screen_row(inv.cursor) == INV_ROWS - 1 && inv.cursor % INV_HOTBAR == 2,
          "up from the hotbar went to slot %d, not the storage row just above", inv.cursor);
    for (int i = 0; i < INV_ROWS + 3; i++) inv_move_cursor(&inv, 0, -1);
    CHECK(inv_screen_row(inv.cursor) == 0, "up did not stop at the top row");
    for (int i = 0; i < INV_ROWS + 3; i++) inv_move_cursor(&inv, 0, 1);
    CHECK(inv.cursor == 2, "down did not come back to the hotbar slot it started from (%d)", inv.cursor);
    // Every slot is reachable, and each is visited once walking the grid.
    int seen[INV_SLOTS] = {0};
    for (int r = 0; r <= INV_ROWS; r++) {
        for (int c = 0; c < INV_HOTBAR; c++) {
            inv.cursor = 0;
            for (int i = 0; i < INV_ROWS; i++) inv_move_cursor(&inv, 0, -1);  // to the top row
            inv_move_cursor(&inv, -INV_HOTBAR, 0);
            inv_move_cursor(&inv, c, r);
            CHECK(inv_screen_row(inv.cursor) == r, "walking to row %d landed on row %d", r, inv_screen_row(inv.cursor));
            seen[inv.cursor]++;
        }
    }
    for (int i = 0; i < INV_SLOTS; i++) CHECK(seen[i] == 1, "slot %d was reached %d times", i, seen[i]);
}

static void check_drops(void) {
    printf("drops and despawn\n");
    CHECK(flat_world(8) != NULL, "the test world would not become resident");
    item_entity_reset();
    inventory_t inv;
    inv_clear(&inv);

    // Stone drops cobblestone -- but only to a tool that qualifies.
    set_block(8, 8, 8, BLK_STONE, 0);
    break_result_t r = interact_break(8, 8, 8, 0);  // bare hands
    CHECK(r.ok, "stone would not break by hand");
    printf("  stone broken by hand dropped %d\n", r.dropped);
    CHECK(r.dropped == 0, "bare hands harvested stone");

    set_block(8, 8, 8, BLK_STONE, 0);
    r = interact_break(8, 8, 8, ITEM_PICK_STONE);
    printf("  stone broken with a pickaxe dropped %d\n", r.dropped);
    CHECK(r.dropped == 1, "a pickaxe on stone dropped %d, expected 1", r.dropped);
    CHECK(item_entity_live() == 1, "the drop is not on the ground");

    // It falls, then is collected when the player comes near -- and
    // NOT before ITEM_PICKUP_DELAY, or breaking a block under your feet
    // snatches it back before it is visible.
    int picked = 0;
    for (uint32_t t = 0; t < ITEM_PICKUP_DELAY - 1; t++) picked += item_entity_tick(&inv, 8.5, 8.0, 8.5);
    CHECK(picked == 0, "a drop was collected before ITEM_PICKUP_DELAY");
    for (int t = 0; t < 20 && item_entity_live() > 0; t++) picked += item_entity_tick(&inv, 8.5, 8.0, 8.5);
    printf("  picked up %d after the delay; %d still on the ground\n", picked, item_entity_live());
    CHECK(picked == 1, "the drop was not collected: %d", picked);
    CHECK(inv_count(&inv, BLK_COBBLE) == 1, "the cobblestone is not in the inventory");

    // WHAT THE PLAYER THROWS MUST STAY THROWN for a moment. An item
    // lands inside the 1.4-block pickup radius whichever way you face,
    // so without a longer delay pressing G looks like it does nothing:
    // the item leaves the inventory and is collected again half a
    // second later.
    item_entity_reset();
    inv_clear(&inv);
    CHECK(item_entity_throw(8.5, 9.3, 8.5, BLK_DIRT, 1, 0, 0.2f, 0.12f, 0.0f, ITEM_THROW_DELAY) == 1,
          "throwing an item failed");
    picked = 0;
    for (uint32_t t = 0; t < ITEM_THROW_DELAY - 1; t++) picked += item_entity_tick(&inv, 8.5, 8.0, 8.5);
    printf("  a thrown item was still on the ground after %u ticks with the player standing on it\n",
           ITEM_THROW_DELAY - 1);
    CHECK(picked == 0, "a thrown item was collected again after %d ticks; G would look like it does nothing",
          ITEM_THROW_DELAY - 1);
    CHECK(item_entity_live() == 1, "the thrown item vanished");
    // ... and then it can be picked up again, or you could never
    // change your mind.
    // It comes to rest CLEAR of where it was thrown from -- which is
    // the other half of G working: an item you have to walk back to.
    double rest_x = 0.0;
    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        if (!item_entity_at(i)->alive) continue;
        rest_x = item_entity_at(i)->body.x;
        break;
    }
    printf("  it came to rest %.2f blocks from the thrower (pickup range %.2f)\n", rest_x - 8.5,
           (double)ITEM_PICKUP_RANGE);
    CHECK(rest_x - 8.5 > (double)ITEM_PICKUP_RANGE,
          "a thrown item settled %.2f blocks away, inside the pickup radius: it would be scooped straight back up",
          rest_x - 8.5);

    // ... and walking to it picks it up, or you could never change
    // your mind.
    for (int t = 0; t < 10 && item_entity_live() > 0; t++) picked += item_entity_tick(&inv, rest_x, 8.0, 8.5);
    CHECK(picked == 1, "a thrown item could not be picked back up by walking to it");

    // A drop nobody collects despawns at ITEM_DESPAWN_TICKS -- in
    // TICKS, so a pause or a week away does not age it (D-51).
    item_entity_reset();
    CHECK(item_entity_spawn(20, 9, 20, BLK_DIRT, 1, 0) == 1, "spawning a drop failed");
    uint32_t t = 0;
    while (item_entity_live() > 0 && t < ITEM_DESPAWN_TICKS * 2) {
        item_entity_tick(NULL, 0.0, 0.0, 0.0);  // no player: nothing collects it
        t++;
    }
    printf("  an uncollected drop despawned after %u ticks (%.1f minutes at 20 Hz)\n", t,
           (double)t / 20.0 / 60.0);
    CHECK(t == ITEM_DESPAWN_TICKS, "a drop despawned after %u ticks, expected %u", t, ITEM_DESPAWN_TICKS);

    // Felling drops every block it takes, which is the point of felling.
    item_entity_reset();
    for (int y = 0; y < 5; y++) set_block(30, 8 + y, 30, BLK_LOG, 0);
    r = interact_break(30, 8, 30, ITEM_AXE_STONE);
    printf("  felling a 5-log trunk took %d blocks and dropped %d\n", r.felled, r.dropped);
    CHECK(r.was_tree, "the trunk did not fell");
    CHECK(r.dropped == r.felled, "felling took %d blocks but dropped %d", r.felled, r.dropped);

    // The pool is finite and a full one must refuse, not corrupt.
    item_entity_reset();
    int made = 0;
    for (int i = 0; i < ITEM_ENTITY_MAX + 20; i++) made += item_entity_spawn(40, 9, 40, ITEM_PICK_WOOD, 1, 0);
    printf("  the pool took %d of %d single-item drops\n", made, ITEM_ENTITY_MAX + 20);
    CHECK(made == ITEM_ENTITY_MAX, "the pool took %d, expected its size %d", made, ITEM_ENTITY_MAX);
    CHECK(item_entity_live() == ITEM_ENTITY_MAX, "the live count disagrees with what was made");
}

int main(void) {
    check_blocks();
    check_ids();
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
    check_farlands();
    check_codec();
    check_region();
    check_region_damage();
    check_torn_write();
    check_compaction();
    check_sections();
    check_worldstore();
    check_palette();
    check_slots();
    check_datadir();
    check_streaming();
    if (!chunk_store_init()) {
        printf("  FAIL: chunk_store_init() for the player checks\n");
        return 1;
    }
    check_physics();
    check_raycast();
    check_felling();
    check_items();
    check_inv_cursor();
    check_light();
    check_replay();
    check_drops();
    chunk_store_shutdown();
    if (s_fail) {
        printf("\nworldcheck: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("\nworldcheck: all checks passed\n");
    return 0;
}
