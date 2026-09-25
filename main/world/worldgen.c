// =====================================================================
//  SynthMiner  --  terrain generation (see worldgen.h)
// ---------------------------------------------------------------------
//  The shape of a world, in the order it is built:
//
//    height    two octave stacks -- a broad one that decides land from
//              water, a finer one for hills -- added, so coastlines are
//              large and the ground in between is interesting.
//    biomes    two broad fields, temperature and humidity, pick one of
//              three rows: what the surface is, how deep the soil runs,
//              how many trees and how many flowers.
//    strata    bedrock, stone, the biome's soil, and its surface block --
//              except at the waterline, which is sand everywhere.
//    sea       air below CH_SEA_LEVEL becomes water.
//    caves     a 3D density field carves the stone, and a broad mouth
//              field decides the few places it may break the surface.
//    ores      VEINS of coal and iron, on a coarse candidate grid.
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
#define S_ORE2   0x4545u  // iron: its own hash, so coal's veins do not move
#define S_MOUTH  0x9999u  // where a cave is allowed to break the surface
#define S_TREE   0x5555u
#define S_PLANT  0x6666u
#define S_DETAIL 0x7777u
#define S_SIGN   0x8888u
#define S_TEMP   0xAAAAu
#define S_HUMID  0xBBBBu
#define S_VARIANT 0xCCCCu  // which KIND of forest
#define S_SNOW   0xDDDDu  // where the caps sit on the high ground

// Sea level is CH_SEA_LEVEL (24) of 64, so there is room for caves
// beneath and for building above. The three height numbers that used to
// live here are columns of the biome table now (worldgen.h).

// --- Biomes -----------------------------------------------------------
//
// Three, out of blocks the game already has: no new ids, nothing
// permanent committed (D-74), and a row here is the whole of what makes
// one place different from another.
// The height band each biome sits in. H_BASE/H_CONT/H_HILL used to be
// three constants for the whole world; they are these columns now, and
// PLAINS still holds exactly the old numbers so plains ground is
// unchanged to the block.
biome_def_t const BIOMES[BIOME_COUNT] = {
    // Open ground with the odd tree: what the whole world used to be,
    // kept at exactly its old numbers so a plains chunk generates as it
    // always did.
    [BIOME_PLAINS] = {.name  = "plains",
                      .surface = BLK_GRASS, .filler = BLK_DIRT,
                      .soil_min = 3, .soil_max = 5,
                      .tree_chance = 0.28f, .plant_chance = 0.09f, .flowers = 0.55f,
                      .h_base = 14.0f, .h_cont = 20.0f, .h_hill = 12.0f,
                      .log_block = BLK_LOG, .leaf_block = BLK_LEAVES,
                      .rock_above = 255, .snow_above = 255},

    // Trees close enough to walk between in shade, and more undergrowth
    // than flowers.
    [BIOME_FOREST] = {.name  = "forest",
                      .surface = BLK_GRASS, .filler = BLK_DIRT,
                      .soil_min = 3, .soil_max = 6,
                      .tree_chance = 0.66f, .plant_chance = 0.16f, .flowers = 0.25f,
                      .h_base = 14.0f, .h_cont = 20.0f, .h_hill = 15.0f,
                      .log_block = BLK_LOG, .leaf_block = BLK_LEAVES,
                      .rock_above = 255, .snow_above = 255},

    // Sand over sand, and nothing growing. No cactus: that would be a
    // new block, and a new block id is forever.
    [BIOME_SAND] = {.name  = "sand flats",
                    .surface = BLK_SAND, .filler = BLK_SAND,
                    .soil_min = 4, .soil_max = 7,
                    .tree_chance = 0.0f, .plant_chance = 0.0f, .flowers = 0.0f,
                    // FLAT, and that is most of what makes it read as a
                    // desert rather than as pale grassland.
                    .h_base = 13.0f, .h_cont = 17.0f, .h_hill = 4.0f,
                    .subsoil = BLK_SANDSTONE, .subsoil_depth = 5,
                    .column_plant = BLK_CACTUS, .column_chance = 0.010f, .column_min = 1, .column_max = 3,
                    .rock_above = 255, .snow_above = 255},

    // High, steep, and bare above the treeline. No snow and no new
    // stone: the rock is the stone already under everything, shown
    // rather than added, so this biome costs no permanent block id.
    [BIOME_MOUNTAIN] = {.name  = "mountains",
                        .surface = BLK_GRASS, .filler = BLK_DIRT,
                        .soil_min = 1, .soil_max = 3,
                        .tree_chance = 0.16f, .plant_chance = 0.05f, .flowers = 0.35f,
                        .h_base = 17.0f, .h_cont = 25.0f, .h_hill = 27.0f,
                        .log_block = BLK_LOG, .leaf_block = BLK_LEAVES,
                        .rock_above = 40, .snow_above = 41},

    // RARE, and the whole point of it is that it is rare: a stand of
    // white trunks you come across now and then is somewhere; one you
    // can always see is wallpaper.
    [BIOME_BIRCH] = {.name  = "birch wood",
                     .surface = BLK_GRASS, .filler = BLK_DIRT,
                     .soil_min = 3, .soil_max = 6,
                     .tree_chance = 0.70f, .plant_chance = 0.12f, .flowers = 0.45f,
                     .h_base = 14.0f, .h_cont = 20.0f, .h_hill = 13.0f,
                     .log_block = BLK_BIRCH_LOG, .leaf_block = BLK_BIRCH_LEAVES,
                     .rock_above = 255, .snow_above = 255},
};

// A smooth 0..1 crossing of `edge`, over a band either side of it.
// Every biome's share is built out of these, which is why the ground
// they shape has no steps in it.
#define BIOME_BAND 0.055f

static float step_up(float v, float edge) {
    float t = (v - (edge - BIOME_BAND)) / (2.0f * BIOME_BAND);
    t       = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

static float step_down(float v, float edge) {
    return 1.0f - step_up(v, edge);
}

void worldgen_biome_weights(int32_t x, int32_t z, uint32_t seed, float w[BIOME_COUNT]) {
    // SLOWER THAN THE HILLS AND FASTER THAN THE CONTINENTS: at 420 and
    // 360 blocks a biome is a few minutes across on foot, which is far
    // enough to feel like somewhere and near enough to find another.
    float const temp  = sm_fbm2((float)x, (float)z, 420.0f, 3, seed ^ S_TEMP);
    float const humid = sm_fbm2((float)x, (float)z, 360.0f, 3, seed ^ S_HUMID);

    float const cold = step_down(temp, 0.32f);
    float const hot  = step_up(temp, 0.60f);
    float const dry  = step_down(humid, 0.42f);
    float const wet  = step_up(humid, 0.56f);

    // A third field, slower still, splitting the wet ground into two
    // kinds of wood. It is its own field rather than a corner of the
    // temperature/humidity square because a birch wood is not a
    // climate -- it is which trees happened to win here.
    float const birchy = step_up(sm_fbm2((float)x, (float)z, 260.0f, 2, seed ^ S_VARIANT), 0.66f);

    // In order of precedence, each one taking what the ones before it
    // left: cold ground is mountains whatever else it is, hot AND dry
    // ground is sand, wet ground is forest, and the rest is plains.
    //
    // The cold edge was swept, not chosen: 0.38 makes mountains 24% of
    // the world (more than forest, which is not a mountain range, it is
    // a mountain planet), 0.32 makes them 12%, and 0.26 makes them 5%
    // and hard to ever find. worldcheck prints the share of each.
    w[BIOME_MOUNTAIN] = cold;
    w[BIOME_SAND]     = (1.0f - cold) * hot * dry;
    float const wood  = (1.0f - cold) * (1.0f - hot * dry) * wet;
    w[BIOME_BIRCH]    = wood * birchy;
    w[BIOME_FOREST]   = wood * (1.0f - birchy);
    float rest = 1.0f - w[BIOME_MOUNTAIN] - w[BIOME_SAND] - w[BIOME_FOREST] - w[BIOME_BIRCH];
    w[BIOME_PLAINS]   = rest < 0.0f ? 0.0f : rest;
}

bool worldgen_snow(int32_t x, int32_t z, uint32_t seed) {
    // 0.67 is where this field puts snow on a fifth of the high
    // ground, which is the number that was asked for. Swept, like the
    // cave mouths and the cold edge: 0.74 gives 9%, 0.70 gives 15%,
    // 0.66 gives 22%. worldcheck prints the share every run.
    return sm_fbm2((float)x, (float)z, 300.0f, 2, seed ^ S_SNOW) > 0.67f;
}

uint8_t worldgen_biome(int32_t x, int32_t z, uint32_t seed) {
    // THE LARGEST SHARE, not a second set of thresholds. One rule means
    // the block on the ground and the shape of the ground can never
    // disagree about where a biome starts.
    float w[BIOME_COUNT];
    worldgen_biome_weights(x, z, seed, w);

    int best = 0;
    for (int b = 1; b < BIOME_COUNT; b++) {
        if (w[b] > w[best]) best = b;
    }
    return (uint8_t)best;
}

int worldgen_height(int32_t x, int32_t z, uint32_t seed) {
    float const fx = (float)x, fz = (float)z;

    // Broad: decides land and sea. Pushed through a smoothstep-ish
    // curve so coasts are definite rather than endless shallows.
    float c = sm_fbm2(fx, fz, 320.0f, 3, seed ^ S_CONT);
    c = c * c * (3.0f - 2.0f * c);

    // Fine: hills. Squared, so flat ground is common and peaks are not.
    float const h = sm_fbm2(fx, fz, 56.0f, 4, seed ^ S_HILL);

    // The three numbers, mixed by each biome's smooth share of this
    // spot. Continuous because the weights are, so a biome border is a
    // slope and not a step (worldgen.h).
    float w[BIOME_COUNT];
    worldgen_biome_weights(x, z, seed, w);
    float base = 0.0f, cont = 0.0f, hill = 0.0f;
    for (int b = 0; b < BIOME_COUNT; b++) {
        base += w[b] * BIOMES[b].h_base;
        cont += w[b] * BIOMES[b].h_cont;
        hill += w[b] * BIOMES[b].h_hill;
    }

    float y = base + c * cont + h * h * hill;

    // A little per-block wobble keeps long slopes from looking milled.
    y += (sm_noise2(fx, fz, 7.0f, seed ^ S_DETAIL) - 0.5f) * 1.5f;

    int const iy = (int)floorf(y);
    return iy < 1 ? 1 : iy > CH_H - 8 ? CH_H - 8 : iy;
}

// WHERE A CAVE MAY REACH DAYLIGHT.
//
// A broad, slow field, true over a small part of the world: a tunnel
// that happens to run near the top opens a mouth there and stays
// buried everywhere else. Without it every shallow tunnel would break
// through -- which is the reason cave_at used to refuse the top four
// blocks outright, and so the reason there were no entrances at all.
//
// THE THRESHOLD IS STEEP AND IT WAS MEASURED, not chosen: this field
// rarely goes above 0.85, so 0.78 opens 8.7% of the land (a colander),
// 0.84 opens 1.7% (about one column in sixty, which reads as the
// occasional hole in a hillside) and 0.88 opens none at all.
// worldcheck's "ores" section prints the number and fails either way
// off it.
static bool cave_mouth(int32_t x, int32_t z, uint32_t seed) {
    return sm_fbm2((float)x, (float)z, 160.0f, 2, seed ^ S_MOUTH) > 0.84f;
}

// Caves. The field is sampled at block resolution, which is exactly the
// case the donor's lattice hash could not survive (F-10).
static bool cave_at(int32_t x, int y, int32_t z, int surface, uint32_t seed, bool mouth) {
    // Under the lid, unless this is one of the places allowed to open:
    // there, the tunnel may take the surface block itself.
    if (y > (mouth ? surface : surface - 4)) return false;
    if (y <= CH_BEDROCK + 1) return false;

    // Two fields at right angles to each other carve tunnels where both
    // are near their midpoint -- long worms rather than round bubbles.
    float const a = sm_noise3((float)x, (float)y * 2.0f, (float)z, 22.0f, seed ^ S_CAVE);
    float const b = sm_noise3((float)x, (float)y * 2.0f, (float)z, 22.0f, seed ^ (S_CAVE + 0x99u));
    float const da = fabsf(a - 0.5f), db = fabsf(b - 0.5f);

    // Wider with depth, so the deep world is more open than the shallow
    // -- and wider again at a mouth, because a one-block hole in a
    // hillside is a thing you fall down, not a thing you walk into.
    float const depth = (float)(surface - y) / (float)CH_H;
    float       t     = 0.055f + depth * 0.045f;
    if (mouth) t += 0.022f;
    return da < t && db < t;
}

// --- Ore veins --------------------------------------------------------
//
// ONE CANDIDATE PER COARSE CELL, and a block is ore if it falls inside
// any candidate near it. That shape is forced by how this generator
// works: fill_column asks about one cell at a time and must give the
// same answer from either side of a chunk border, so a vein cannot be
// grown by walking -- it has to be a function of the position. Same
// trick as the tree grid below, in three dimensions.
//
// Before this, every ore block was an independent coin flip and two
// together were a coincidence: no veins at all, and no reason to
// follow one. The user asked for Minecraft's clumps, and for the
// reason that matters -- a vein you can see the edge of is a reason to
// dig sideways.
#define VEIN_GRID 8

// How common each ore is, and how big a lump of it. Measured rather
// than guessed: worldcheck's "ores" section counts both the share of
// stone they take and how clustered they are, and fails if either
// drifts (F-84).
#define VEIN_COAL_CHANCE 0.30f
#define VEIN_COAL_R      2.0f

#define VEIN_IRON_CHANCE 0.20f
#define VEIN_IRON_R      1.7f

// True if (x, y, z) sits inside a vein. `chance` is how many coarse
// cells carry one, `r` its rough radius in blocks, `ymax` the highest
// it may appear.
static bool vein_at(int32_t x, int y, int32_t z, uint32_t seed, uint32_t salt, int ymax, float chance,
                    float r) {
    int32_t const cx = (x >= 0 ? x : x - VEIN_GRID + 1) / VEIN_GRID;
    int32_t const cz = (z >= 0 ? z : z - VEIN_GRID + 1) / VEIN_GRID;
    int const     cy = (y >= 0 ? y : y - VEIN_GRID + 1) / VEIN_GRID;

    // The 27 cells around this one: a vein reaches at most a little
    // over one cell, so nothing further can contain this block.
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                int32_t const gx = cx + dx, gz = cz + dz;
                int const     gy = cy + dy;
                if (sm_rand3(gx, gy, gz, seed ^ salt) > chance) continue;

                // Where in its cell the vein sits, and how it is shaped.
                float const ox = (float)(gx * VEIN_GRID) + sm_rand3(gx, gy, gz, seed ^ (salt + 1u)) * VEIN_GRID;
                float const oy = (float)(gy * VEIN_GRID) + sm_rand3(gx, gy, gz, seed ^ (salt + 2u)) * VEIN_GRID;
                float const oz = (float)(gz * VEIN_GRID) + sm_rand3(gx, gy, gz, seed ^ (salt + 3u)) * VEIN_GRID;
                if (oy > (float)ymax) continue;

                // Three radii, not one: a sphere of ore reads as a
                // decoration, a lumpy blob reads as a vein.
                float const rx = r * (0.75f + sm_rand3(gx, gy, gz, seed ^ (salt + 4u)) * 0.7f);
                float const ry = r * (0.60f + sm_rand3(gx, gy, gz, seed ^ (salt + 5u)) * 0.6f);
                float const rz = r * (0.75f + sm_rand3(gx, gy, gz, seed ^ (salt + 6u)) * 0.7f);

                float const fx = ((float)x + 0.5f - ox) / rx;
                float const fy = ((float)y + 0.5f - oy) / ry;
                float const fz = ((float)z + 0.5f - oz) / rz;
                if (fx * fx + fy * fy + fz * fz <= 1.0f) return true;
            }
        }
    }
    return false;
}

static void fill_column(chunk_t* c, int lx, int lz, int32_t wx, int32_t wz, uint32_t seed) {
    uint8_t* id  = &c->id[CH_IDX(lx, 0, lz)];
    int const sy = worldgen_height(wx, wz, seed);

    biome_def_t const* bd = &BIOMES[worldgen_biome(wx, wz, seed)];

    // How deep the soil runs here, from the biome's band.
    int const span = (int)bd->soil_max - (int)bd->soil_min + 1;
    int const soil = (int)bd->soil_min + (int)(sm_rand2(wx, wz, seed ^ S_DETAIL) * (float)span);

    // THE WATERLINE IS SAND WHATEVER THE BIOME IS. A beach is not a
    // place, it is an edge, and grass running into the sea looks wrong
    // in every biome there has ever been.
    bool const beach = sy <= CH_SEA_LEVEL + 1;
    // ... and high ground is bare rock, which is what a mountain looks
    // like without inventing a block to say so.
    bool const rock = !beach && sy >= (int)bd->rock_above;

    // SNOW ON ABOUT A FIFTH OF THE TALL GROUND (the user's number),
    // from a field slower than a mountain is wide -- so a summit is
    // snowy or it is bare, rather than the cap being speckled.
    bool const snow = !beach && sy >= (int)bd->snow_above &&
                      sm_fbm2((float)wx, (float)wz, 300.0f, 2, seed ^ S_SNOW) > 0.67f;
    // Asked once per column, not once per cell: it does not vary
    // with height and it is two octaves of noise.
    bool const mouth = !beach && cave_mouth(wx, wz, seed);

    for (int y = 0; y < CH_H; y++) {
        uint8_t b = BLK_AIR;
        if (y == CH_BEDROCK) {
            b = BLK_STONE;  // the floor of the world; unbreakable stone stands in for bedrock
        } else if (y < sy - soil - (int)bd->subsoil_depth) {
            b = BLK_STONE;
        } else if (y < sy - soil) {
            // Between the soil and the stone: sandstone under a desert,
            // and nothing at all anywhere else (subsoil_depth 0, so
            // this band is empty and the branch never fires).
            b = bd->subsoil != BLK_AIR ? bd->subsoil : BLK_STONE;
        } else if (y < sy) {
            b = beach ? BLK_SAND : rock ? BLK_STONE : bd->filler;
        } else if (y == sy) {
            b = snow ? BLK_SNOW : beach ? BLK_SAND : rock ? BLK_STONE : bd->surface;
        } else if (y <= CH_SEA_LEVEL) {
            b = BLK_WATER;
        }

        // Carve, but never the bedrock course and never into the sea:
        // a cave under water would flood, and there is no fluid
        // simulation to flood it with.
        // A cave may take the soil and the turf as well, but ONLY where
        // a mouth is allowed: that is what turns a tunnel into a way
        // in. Everywhere else it still stops at the stone, and the
        // ground above stays whole.
        bool const soft = (b == BLK_DIRT || b == BLK_GRASS || b == BLK_SAND);
        if ((b == BLK_STONE || (mouth && soft)) && y > CH_BEDROCK && cave_at(wx, y, wz, sy, seed, mouth)) {
            // Never into the sea: there is no fluid simulation to flood
            // what it would open.
            if (sy > CH_SEA_LEVEL + 2 || y < CH_SEA_LEVEL - 3) b = BLK_AIR;
        }

        // Ore in what stone is left, in VEINS (above). Iron is deeper
        // and rarer than coal, and tested first so the two never fight
        // over a cell -- iron inside a coal vein looks like a bug.
        if (b == BLK_STONE && y > CH_BEDROCK) {
            if (vein_at(wx, y, wz, seed, S_ORE2, VEIN_IRON_YMAX, VEIN_IRON_CHANCE, VEIN_IRON_R)) {
                b = BLK_IRON_ORE;
            } else if (vein_at(wx, y, wz, seed, S_ORE, VEIN_COAL_YMAX, VEIN_COAL_CHANCE, VEIN_COAL_R)) {
                b = BLK_COAL_ORE;
            }
        }

        id[y] = b;
    }

    // A surface block needs air above it. A cave mouth or an overhang
    // can leave it buried, and buried grass is a texture nobody sees.
    if (!beach && sy + 1 < CH_H && id[sy] == bd->surface && id[sy] != bd->filler && id[sy + 1] != BLK_AIR) {
        id[sy] = bd->filler;
    }
}

// --- Decorations ------------------------------------------------------

// Tree candidates sit on a coarse grid, one per TREE_GRID square, so
// two trees can never grow into each other.
#define TREE_GRID   5
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
    // The chance belongs to the biome the tree would stand in, not to
    // the chunk being filled: a forest that thinned out at its border
    // because the neighbouring chunk asked would not be a forest.
    biome_def_t const* bd = &BIOMES[worldgen_biome(wx, wz, seed)];
    if (sm_rand2(wx, wz, seed ^ S_TREE) > bd->tree_chance) return;
    if (bd->log_block == BLK_AIR) return;

    // A tree needs grass to stand on, and the ground under it must be
    // the generated surface -- not the inside of a hill.
    int const sy = worldgen_height(wx, wz, seed);
    if (sy <= CH_SEA_LEVEL + 1) return;  // no trees on the beach or in the water
    if (sy >= (int)bd->rock_above) return;  // nor above the treeline

    int const h = TREE_MIN_H + (int)(sm_rand2(wx + 1, wz - 1, seed ^ S_TREE) * (TREE_MAX_H - TREE_MIN_H + 1));
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
                if (r == 2 && dx * dx + dz * dz == 5 && sm_rand3(wx + dx, y, wz + dz, seed ^ S_TREE) < 0.45f) continue;
                stamp(c, wx + dx, y, wz + dz, bd->leaf_block, false);
            }
        }
    }
    for (int y = sy; y < top; y++) stamp(c, wx, y, wz, bd->log_block, true);
    // Dirt under the trunk: a tree on a single grass block looks wrong
    // once the grass is gone.
    stamp(c, wx, sy - 1, wz, BLK_DIRT, true);
}

// A plant that stands more than one block: the cactus, so far. Placed
// from the biome row, so a second one is a row and not a function.
static void place_column_plant(chunk_t* c, int lx, int lz, int32_t wx, int32_t wz, uint32_t seed,
                               biome_def_t const* bd, int sy) {
    if (bd->column_plant == BLK_AIR || bd->column_chance <= 0.0f) return;
    if (sm_rand2(wx, wz, seed ^ (S_PLANT + 0x51u)) > bd->column_chance) return;

    uint8_t* col = &c->id[CH_IDX(lx, 0, lz)];
    // It stands ON the surface, so the surface has to be there and the
    // air above it has to be air.
    if (sy + 1 >= CH_H || col[sy] != bd->surface || col[sy + 1] != BLK_AIR) return;

    int const span = (int)bd->column_max - (int)bd->column_min + 1;
    int       h    = (int)bd->column_min + (int)(sm_rand2(wx + 7, wz - 3, seed ^ S_PLANT) * (float)span);
    if (sy + h >= CH_H - 1) h = CH_H - 2 - sy;
    for (int i = 1; i <= h; i++) col[sy + i] = bd->column_plant;
}

static void decorate_plants(chunk_t* c, uint32_t seed) {
    for (int lz = 0; lz < CH_D; lz++) {
        for (int lx = 0; lx < CH_W; lx++) {
            int32_t const wx = c->cx * CH_W + lx, wz = c->cz * CH_D + lz;
            uint8_t*      col = &c->id[CH_IDX(lx, 0, lz)];

            biome_def_t const* bd = &BIOMES[worldgen_biome(wx, wz, seed)];

            // THE BIOME'S OWN SURFACE BLOCK, not grass: a desert has no
            // grass to find, and looking for it is why the cactus pass
            // below would never have fired.
            int sy = -1;
            for (int y = CH_H - 2; y > 0; y--) {
                if (col[y] == bd->surface) {
                    sy = y;
                    break;
                }
            }
            if (sy < 0 || col[sy + 1] != BLK_AIR) continue;

            place_column_plant(c, lx, lz, wx, wz, seed, bd, sy);
            if (bd->plant_chance <= 0.0f) continue;
            if (col[sy + 1] != BLK_AIR) continue;  // a cactus went there

            float const r = sm_rand2(wx, wz, seed ^ S_PLANT);
            float const t = 1.0f - bd->plant_chance;
            if (r <= t) continue;

            // Where in the biome's share of plants this one falls, so
            // the flower-to-grass mix is the row's business and not
            // three thresholds written out here.
            float const u = (r - t) / bd->plant_chance;
            uint8_t     p;
            if (u < bd->flowers) {
                p = (u < bd->flowers * 0.5f) ? BLK_FLOWER_RED : BLK_FLOWER_YELLOW;
            } else {
                p = BLK_TALL_GRASS;
            }
            col[sy + 1] = p;
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
    if (sm_rand2(c->cx, c->cz, seed ^ S_SIGN) > SIGN_CHANCE) return;
    int const lx = (int)(sm_rand2(c->cx + 1, c->cz, seed ^ S_SIGN) * 3.0f);
    int const lz = (int)(sm_rand2(c->cx, c->cz + 1, seed ^ S_SIGN) * (float)CH_D);
    uint8_t*  col = &c->id[CH_IDX(lx, 0, lz)];
    int       y   = CH_H - 2;
    while (y > 0 && !block_solid(col[y])) y--;
    // On dry ground, with room above: not in the sea, not under a tree.
    if (y <= CH_SEA_LEVEL || block_fellable(col[y])) return;  // not in the sea, not under a tree
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
            int32_t const wx = gx * TREE_GRID + (int32_t)(sm_rand2(gx, gz, seed ^ 0xA1u) * TREE_GRID);
            int32_t const wz = gz * TREE_GRID + (int32_t)(sm_rand2(gx, gz, seed ^ 0xB2u) * TREE_GRID);
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
