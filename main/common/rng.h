#pragma once
// =====================================================================
//  CraftMiner  --  seeded hashing, noise and random streams
// ---------------------------------------------------------------------
//  Everything random in this game is a pure function of a seed and a
//  coordinate, or of an explicit stream the simulation advances. There
//  is no rand(), no esp_random(), and nothing reads the clock. That is
//  determinism rule 2 (claudeplans/craftminer.md, Part T), and it is
//  what makes a recorded input replay reproduce a run exactly.
//
//  WHY NOT THE DONOR'S hash01(). The showreel folded a lattice point
//  into one int key: hash01(ix * 7919 + iz * 104729, seed). Both
//  factors are prime, so (ix, iz) and (ix + 104729, iz - 7919) hash
//  identically, and iz * 104729 leaves int32 beyond |iz| = 20505. In a
//  128 x 128 world neither can happen. Here the cave carver and the Far
//  Lands sample a 3D density field at roughly block resolution, where
//  the lattice index IS the world coordinate -- and 20505 blocks is
//  well inside ordinary play. So the lattice hashes keep their
//  coordinates in separate 64-bit lanes and mix them there, which
//  cannot alias or overflow at any int32 coordinate (F-10).
//
//  Pure: no engine, no RTOS, no allocation.
// =====================================================================

#include <stdint.h>

// --- Hashing ----------------------------------------------------------

// SplitMix64's finaliser: a good avalanche in a handful of operations,
// which is what a per-cell noise lookup can afford.
static inline uint64_t cm_mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// A lattice hash over two or three signed coordinates plus a seed. The
// coordinates go into separate 64-bit lanes before mixing, so no pair
// of distinct points can alias and nothing overflows at any coordinate
// an int32 can hold.
static inline uint32_t cm_hash2(int32_t x, int32_t z, uint32_t seed) {
    uint64_t h = (uint64_t)(uint32_t)x;
    h = cm_mix64(h ^ ((uint64_t)(uint32_t)z << 32));
    return (uint32_t)(cm_mix64(h ^ seed) >> 32);
}

static inline uint32_t cm_hash3(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint64_t h = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)z << 32);
    h = cm_mix64(h ^ ((uint64_t)(uint32_t)y * 0xD6E8FEB86659FD93ULL));
    return (uint32_t)(cm_mix64(h ^ seed) >> 32);
}

// The same, as a float in [0, 1).
static inline float cm_rand2(int32_t x, int32_t z, uint32_t seed) {
    return (float)(cm_hash2(x, z, seed) >> 8) * (1.0f / 16777216.0f);
}
static inline float cm_rand3(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return (float)(cm_hash3(x, y, z, seed) >> 8) * (1.0f / 16777216.0f);
}

// --- Streams ----------------------------------------------------------
//
// xorshift64*, for anything that draws a sequence rather than a value
// at a point: an entity's behaviour, a loot roll. Seed it from a hash
// so the sequence is still a pure function of where it started.

typedef struct {
    uint64_t s;
} cm_rng_t;

static inline void cm_rng_seed(cm_rng_t* r, uint64_t seed) {
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;  // never zero: xorshift would stick
}

static inline uint32_t cm_rng_u32(cm_rng_t* r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

static inline float cm_rng_f(cm_rng_t* r) {
    return (float)(cm_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

// 0 .. n-1, unbiased enough for gameplay (n is always small here).
static inline uint32_t cm_rng_below(cm_rng_t* r, uint32_t n) {
    return n ? cm_rng_u32(r) % n : 0u;
}

// --- Noise ------------------------------------------------------------

// Smooth 2D value noise: the lattice hashed at integer steps of
// `scale`, interpolated with a smoothstep. Result in [0, 1).
float cm_noise2(float x, float z, float scale, uint32_t seed);

// The 3D version, for density fields (caves, the Far Lands).
float cm_noise3(float x, float y, float z, float scale, uint32_t seed);

// Several octaves of cm_noise2, each half the amplitude and twice the
// frequency of the last. Result in [0, 1).
float cm_fbm2(float x, float z, float scale, int octaves, uint32_t seed);
