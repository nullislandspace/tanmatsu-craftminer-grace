#pragma once
// =====================================================================
//  CraftMiner  --  terrain generation
// ---------------------------------------------------------------------
//  A chunk's terrain is a pure function of (seed, cx, cz). Nothing here
//  reads another chunk, reads the clock, or draws from anything but a
//  seeded hash -- so a world is reproducible from its seed for ever,
//  and the order chunks happen to be generated in cannot change what
//  they contain. That is determinism rule 3
//  (claudeplans/craftminer.md, Part T), and it is what the host check
//  `worldgen: cross-chunk equivalence` exists to defend.
//
//  DECORATIONS SPAN CHUNKS. A tree rooted near a chunk's edge has
//  branches in the next one. Rather than let a chunk read its
//  neighbour (which would make the result depend on load order), every
//  chunk walks the candidate tree positions of the 3 x 3 neighbourhood
//  around it and writes only the cells that land inside itself. Each
//  chunk therefore reaches the same answer alone as it would in company.
//
//  Pure: no engine, no RTOS, no allocation.
// =====================================================================

#include <stdint.h>

#include "world/chunk.h"

// Fill `c`'s id and st planes with the terrain of its own coordinates.
// The chunk must already carry cx / cz (chunk_claim sets them). Marks
// CF_GENERATED and refreshes the column summaries; leaves cstate alone
// (the caller owns the state machine).
//
// `farlands_x` is the world's Far Lands edge (world_meta_t.farlands_x,
// farlands.h): chunks wholly west of it are Far Lands. FARLANDS_NONE for
// a world without them.
void worldgen_chunk(chunk_t* c, uint32_t seed, int32_t farlands_x);

// The surface height at a column: the y of the first air above the
// solid ground, before caves and decorations. Exposed because spawn
// selection and the host checks want it without generating a chunk.
int worldgen_height(int32_t x, int32_t z, uint32_t seed);

// How deep each ore may appear. Here rather than in the .c so the host
// check measures the generator's own numbers and not a second copy of
// them -- the whole point of worldcheck's "ores" section is that these
// cannot drift without something noticing.
#define VEIN_COAL_YMAX 40
#define VEIN_IRON_YMAX 28

// --- Biomes -----------------------------------------------------------
//
// A FOURTH REGISTRY, and for the same reason as the other three: adding
// a place is a row, not an edit in five functions. Before this, every
// column in the world was grass over dirt with the same chance of a
// tree and the same three flowers -- a forest, a plain and a hillside
// differed only in how high they were.
//
// Chosen from two broad, slow fields (temperature and humidity) the way
// Minecraft chooses, and NOT from the height field: a biome that
// followed the terrain would put the same place on every hilltop.
//
// THE TERRAIN IS BLENDED AND THE SURFACE IS NOT, and that difference
// is the whole design. A biome id is a step function, so height taken
// from a lookup would put a vertical cliff at every border. What is
// blended instead is the WEIGHTS: each biome's membership is a smooth
// function of temperature and humidity, the three height numbers are
// mixed by those weights, and the ground comes out continuous because
// every term in it is. The surface BLOCK still changes abruptly, which
// is right -- grass meets sand at a line in every game that has both.
//
// It costs four multiplies on two noise values the column already
// needed. The obvious alternative -- sample the biome at a grid of
// offsets and average -- would be 25 lookups and 50 noise fields per
// column, and this world already spends 134 ms a chunk.
typedef enum {
    BIOME_PLAINS = 0,
    BIOME_FOREST,
    BIOME_SAND,
    BIOME_MOUNTAIN,
    BIOME_BIRCH,
    BIOME_COUNT
} biome_t;

typedef struct {
    char const* name;
    uint8_t     surface;      // the top block, above the waterline
    uint8_t     filler;       // what lies under it
    uint8_t     soil_min;     // how deep that runs, in blocks
    uint8_t     soil_max;
    float       tree_chance;  // per candidate on the tree grid
    float       plant_chance; // that a surface block carries a plant
    float       flowers;      // of those plants, the share that are flowers

    // What the ground DOES here. The three numbers worldgen_height has
    // always used, now one set per biome and blended between them.
    float       h_base;       // the floor this biome sits on
    float       h_cont;       // how far the broad field lifts it
    float       h_hill;       // how far the fine field roughens it

    // Which tree grows here. Two species so far, and adding a third is
    // this pair plus its blocks -- nothing in place_tree changes.
    uint8_t     log_block;
    uint8_t     leaf_block;

    // What lies under the soil before the stone starts, and how deep:
    // sandstone under a desert. BLK_AIR for "straight to stone".
    uint8_t     subsoil;
    uint8_t     subsoil_depth;

    // A plant that stands more than one block tall -- the cactus, so
    // far. BLK_AIR for none.
    uint8_t     column_plant;
    float       column_chance;
    uint8_t     column_min, column_max;

    // Bare rock at or above this height, whatever the surface block
    // would have been. What makes a mountain read as a mountain without
    // a single new block id. 255 for a biome that never shows rock.
    uint8_t     rock_above;

    // ... and snow above THIS height, where the snow field allows it.
    // 255 for a biome that never sees any.
    uint8_t     snow_above;
} biome_def_t;

extern biome_def_t const BIOMES[BIOME_COUNT];

// The biome at (x, z). A pure function of the position, like everything
// else here, so two chunks agree along their border without either
// reading the other.
uint8_t worldgen_biome(int32_t x, int32_t z, uint32_t seed);

// Each biome's smooth share of (x, z), summing to 1. The discrete biome
// above is simply the largest of these, so the block on the ground and
// the shape of the ground can never disagree about where a place is.
void worldgen_biome_weights(int32_t x, int32_t z, uint32_t seed, float w[BIOME_COUNT]);

// Does snow lie at (x, z), if the ground there is high enough? A field
// SLOWER THAN A MOUNTAIN IS WIDE, so a whole summit is snowy or bare
// rather than the cap being speckled -- which means the share has to be
// measured over many peaks, not along one ridge.
bool worldgen_snow(int32_t x, int32_t z, uint32_t seed);
