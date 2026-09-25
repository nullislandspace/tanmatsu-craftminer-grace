#pragma once
// =====================================================================
//  SynthMiner  --  the Far Lands (claudeplans/synthminer.md, Part X)
// ---------------------------------------------------------------------
//  West of a world's edge (`farlands_x`, -2048 unless the world says
//  otherwise) every column is Far Lands: Minecraft Beta 1.7.3's own
//  terrain generator, ported, and fed coordinates past the point where
//  its noise overflowed -- so what comes out is Beta's Edge Far Lands,
//  not an imitation of them (D-78).
//
//  WHY BETA'S TERRAIN BROKE. Beta's ground is a density field: two
//  16-octave Perlin noises ("low" and "high") blended by an 8-octave
//  "selector", sampled every 4 blocks across and 8 up. Each octave takes
//  the integer part of its coordinate with a Java (int) cast, and the
//  finest octave of low and high advances 171.103 a block -- past 2^31 at
//  x = 12,550,824. There Java's cast SATURATES, the "fraction" left over
//  is no longer 0..1 but billions, and the smoothing polynomial
//  extrapolates it to about 10^49: one octave outweighs everything that
//  gives the terrain its shape, the height falloff included. The result
//  is a wall from bedrock to sky, riddled with tunnels that run straight
//  west -- the overflowed axis always hits the same noise values.
//
//  Two things C does differently from Java, and the whole effect rests
//  on both: an out-of-range double-to-int cast is undefined in C (Java
//  clamps -- java_d2i below), and int arithmetic that overflows is
//  undefined in C (Java wraps -- done in uint32_t).
//
//  FITTING IT IN. Beta's world was 128 high with the sea at 64; ours is
//  64 with the sea filling y <= 24. Beta's column is generated whole and
//  then sampled into ours, the part below its sea level into our y 0..24
//  and the part above into 25..63, so the water lines up with our sea.
//
//  Pure (host-tested by worldcheck). Allocates its noise tables once per
//  seed, in PSRAM on the badge (common/psram.h), and is NOT reentrant:
//  one thread generates at a time -- the chunk worker's.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk.h"

// Where a new world's Far Lands begin: every column WEST of this x is
// Far Lands. About eight minutes' walk from spawn (D-78). A world keeps
// the edge it was given (world_meta_t.farlands_x), so changing this
// reaches new worlds only. Must be a multiple of the chunk width.
#define FARLANDS_X_DEFAULT (-2048)

// No Far Lands at all (the host checks' "ordinary world").
#define FARLANDS_NONE INT32_MIN

_Static_assert(FARLANDS_X_DEFAULT % CH_W == 0, "the Far Lands edge must be on a chunk boundary");

// Is chunk `cx` Far Lands, for a world whose edge is `edge_x`?
static inline bool farlands_chunk_is(int32_t cx, int32_t edge_x) {
    return edge_x != FARLANDS_NONE && (int64_t)cx * CH_W + CH_W <= (int64_t)edge_x;
}

// Fill `c` with Far Lands: its id plane (and a cleared st plane). The
// caller (worldgen_chunk) decides that it is Far Lands and does the
// bookkeeping. False if the noise tables could not be allocated -- the
// chunk is then left as air.
bool farlands_generate(chunk_t* c, uint32_t seed, int32_t edge_x);

// --- Exposed for the host checks ----------------------------------------

// Java's (int) cast of a double: NaN is 0, and out-of-range values
// clamp to INT32_MIN / INT32_MAX instead of being undefined.
int32_t java_d2i(double d);

// java.util.Random, exactly.
typedef struct {
    uint64_t s;
} java_random_t;

void    java_random_seed(java_random_t* r, int64_t seed);
int32_t java_random_next_int(java_random_t* r);            // nextInt()
int32_t java_random_next_int_n(java_random_t* r, int32_t n);  // nextInt(n)
double  java_random_next_double(java_random_t* r);

// The Beta chunk coordinate our chunk `cx` is generated as. Chunks west
// of the edge map onto Beta's chunks west of its overflow, the first
// Far Lands chunk onto the first Beta chunk wholly past it.
int32_t farlands_beta_cx(int32_t cx, int32_t edge_x);

// Generate Beta's own 16 x 16 x 128 column for Beta chunk (bcx, bcz)
// into `out` (Beta's layout: index (x * 16 + z) * 128 + y), in our block
// ids. `full` runs every octave of every noise, the way Beta did; the
// game runs the fast path, which skips what cannot change a block (see
// farlands.c). The host check proves the two agree.
bool farlands_beta_column(int32_t bcx, int32_t bcz, uint32_t seed, bool full, uint8_t* out);

// The Beta row our row `y` shows.
int farlands_beta_y(int y);
