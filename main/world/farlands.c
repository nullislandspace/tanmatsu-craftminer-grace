// =====================================================================
//  CraftMiner  --  the Far Lands (see farlands.h)
// ---------------------------------------------------------------------
//  A port of Minecraft Beta 1.7.3's terrain generator: NoiseGeneratorPerlin,
//  NoiseGeneratorOctaves and ChunkProviderGenerate's density field,
//  terrain fill and surface pass. Names here are descriptive; the Beta
//  names are in the comments. Doubles throughout, as in Java: the
//  overflowed octave reaches 10^49, past what a float can hold.
//
//  Left out, knowingly: biomes (every column is grass over dirt, and the
//  temperature and humidity the density reads are constants -- in the Far
//  Lands they only touch the height falloff, which the overflow drowns);
//  ice; Beta's caves, ores, lakes, trees and flowers (the populate pass).
//
//  THE FAST PATH. In the Far Lands the finest octave of low and high is
//  overflowed and worth about 10^49; every other octave of them is worth
//  at most 2^15, and the height falloff a few hundred. Adding those
//  changes the density in its 35th significant digit, which a double
//  does not have -- so the fast path computes low and high from their
//  first octave only, skips the falloff's two noises, and gets the SAME
//  blocks for about a twentieth of the work. `farlands_beta_column(...,
//  full = true, ...)` is the unabridged port; worldcheck generates
//  chunks both ways and requires every block to match.
// =====================================================================

#include "world/farlands.h"

#include <math.h>
#include <string.h>

#include "common/psram.h"
#include "world/blocks.h"

// --- Java ------------------------------------------------------------------

int32_t java_d2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}

#define JR_MULT 0x5DEECE66DULL
#define JR_MASK ((1ULL << 48) - 1)

void java_random_seed(java_random_t* r, int64_t seed) {
    r->s = ((uint64_t)seed ^ JR_MULT) & JR_MASK;
}

static int32_t jr_next(java_random_t* r, int bits) {
    r->s = (r->s * JR_MULT + 0xBULL) & JR_MASK;
    return (int32_t)(uint32_t)(r->s >> (48 - bits));
}

int32_t java_random_next_int(java_random_t* r) {
    return jr_next(r, 32);
}

int32_t java_random_next_int_n(java_random_t* r, int32_t n) {
    if ((n & -n) == n) return (int32_t)(((int64_t)n * (int64_t)jr_next(r, 31)) >> 31);
    int32_t bits, val;
    do {
        bits = jr_next(r, 31);
        val  = bits % n;
    } while ((int32_t)((uint32_t)bits - (uint32_t)val + (uint32_t)(n - 1)) < 0);
    return val;
}

double java_random_next_double(java_random_t* r) {
    int64_t const hi = (int64_t)jr_next(r, 26) << 27;
    return (double)(hi + jr_next(r, 27)) * (1.0 / (double)(1LL << 53));
}

// --- NoiseGeneratorPerlin --------------------------------------------------

typedef struct {
    double  xo, yo, zo;  // xCoord, yCoord, zCoord
    uint8_t p[512];      // permutations; every entry is 0..255
} perlin_t;

static void perlin_init(perlin_t* g, java_random_t* r) {
    g->xo = java_random_next_double(r) * 256.0;
    g->yo = java_random_next_double(r) * 256.0;
    g->zo = java_random_next_double(r) * 256.0;
    int perm[256];
    for (int i = 0; i < 256; i++) perm[i] = i;
    for (int j = 0; j < 256; j++) {
        int const k = java_random_next_int_n(r, 256 - j) + j;
        int const l = perm[j];
        perm[j]     = perm[k];
        perm[k]     = l;
    }
    for (int i = 0; i < 256; i++) g->p[i] = g->p[i + 256] = (uint8_t)perm[i];
}

static inline double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

static inline double fade(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// grad()
static inline double grad3(int h, double x, double y, double z) {
    int const    j = h & 15;
    double const u = j >= 8 ? y : x;
    double const v = j >= 4 ? (j != 12 && j != 14 ? z : x) : y;
    return ((j & 1) != 0 ? -u : u) + ((j & 2) != 0 ? -v : v);
}

// func_4110_a(): Beta's 2D gradient, used for ONE of the four corners of
// its 2D noise (the other three use the 3D one with y = 0) -- a quirk,
// kept.
static inline double grad2(int h, double x, double z) {
    int const    j = h & 15;
    double const u = (double)(1 - ((j & 8) >> 3)) * x;
    double const v = j >= 4 ? (j != 12 && j != 14 ? z : x) : 0.0;
    return ((j & 1) != 0 ? -u : u) + ((j & 2) != 0 ? -v : v);
}

// Integer part as Java computes it: (int) cast, then one down for a
// negative non-integer. The -1 WRAPS: at INT32_MIN it gives INT32_MAX,
// which is how the west side's "fraction" reaches -4.3 billion.
static inline int32_t java_floor(double d) {
    int32_t i = java_d2i(d);
    if (d < (double)i) i = (int32_t)((uint32_t)i - 1u);
    return i;
}

// func_646_a(): add this octave into `out`, laid out x outer, z, y inner.
// A y size of 1 is Beta's 2D noise (y ignored).
static void perlin_add(perlin_t const* g, double* out, double x0, double y0, double z0, int nx, int ny, int nz,
                       double sx, double sy, double sz, double octave) {
    uint8_t const* p   = g->p;
    double const   amp = 1.0 / octave;
    if (ny == 1) {
        int n = 0;
        for (int i = 0; i < nx; i++) {
            double        x  = (x0 + (double)i) * sx + g->xo;
            int32_t const xi = java_floor(x);
            int const     X  = xi & 255;
            x -= (double)xi;
            double const u = fade(x);
            for (int k = 0; k < nz; k++) {
                double        z  = (z0 + (double)k) * sz + g->zo;
                int32_t const zi = java_floor(z);
                int const     Z  = zi & 255;
                z -= (double)zi;
                double const w  = fade(z);
                int const    A  = p[X] + 0;
                int const    AA = p[A] + Z;
                int const    B  = p[X + 1] + 0;
                int const    BA = p[B] + Z;
                double const a  = lerp(u, grad2(p[AA], x, z), grad3(p[BA], x - 1.0, 0.0, z));
                double const b  = lerp(u, grad3(p[AA + 1], x, 0.0, z - 1.0), grad3(p[BA + 1], x - 1.0, 0.0, z - 1.0));
                out[n++] += lerp(w, a, b) * amp;
            }
        }
        return;
    }
    int    n = 0, last = -1;
    double c0 = 0.0, c1 = 0.0, c2 = 0.0, c3 = 0.0;
    for (int i = 0; i < nx; i++) {
        double        x  = (x0 + (double)i) * sx + g->xo;
        int32_t const xi = java_floor(x);
        int const     X  = xi & 255;
        x -= (double)xi;
        double const u = fade(x);
        for (int k = 0; k < nz; k++) {
            double        z  = (z0 + (double)k) * sz + g->zo;
            int32_t const zi = java_floor(z);
            int const     Z  = zi & 255;
            z -= (double)zi;
            double const w = fade(z);
            for (int j = 0; j < ny; j++) {
                double        y  = (y0 + (double)j) * sy + g->yo;
                int32_t const yi = java_floor(y);
                int const     Y  = yi & 255;
                y -= (double)yi;
                double const v = fade(y);
                // Beta recomputes the corners only when the y lattice
                // cell changes -- and compares against the previous
                // COLUMN's cell as well. Kept, since it is what Beta did.
                if (j == 0 || Y != last) {
                    last         = Y;
                    int const A  = p[X] + Y;
                    int const AA = p[A] + Z;
                    int const AB = p[A + 1] + Z;
                    int const B  = p[X + 1] + Y;
                    int const BA = p[B] + Z;
                    int const BB = p[B + 1] + Z;
                    c0 = lerp(u, grad3(p[AA], x, y, z), grad3(p[BA], x - 1.0, y, z));
                    c1 = lerp(u, grad3(p[AB], x, y - 1.0, z), grad3(p[BB], x - 1.0, y - 1.0, z));
                    c2 = lerp(u, grad3(p[AA + 1], x, y, z - 1.0), grad3(p[BA + 1], x - 1.0, y, z - 1.0));
                    c3 = lerp(u, grad3(p[AB + 1], x, y - 1.0, z - 1.0), grad3(p[BB + 1], x - 1.0, y - 1.0, z - 1.0));
                }
                out[n++] += lerp(w, lerp(v, c0, c1), lerp(v, c2, c3)) * amp;
            }
        }
    }
}

// --- NoiseGeneratorOctaves -------------------------------------------------

typedef struct {
    int       n;
    perlin_t* g;
} octaves_t;

// generateNoiseOctaves(). `use` limits how many octaves are summed (the
// fast path's first-octave-only); the rest are skipped, not reordered.
static void octaves_gen(octaves_t const* o, double* out, double x0, double y0, double z0, int nx, int ny, int nz,
                        double sx, double sy, double sz, int use) {
    memset(out, 0, sizeof(double) * (size_t)(nx * ny * nz));
    double octave = 1.0;
    int const n   = use < o->n ? use : o->n;
    for (int i = 0; i < n; i++) {
        perlin_add(&o->g[i], out, x0, y0, z0, nx, ny, nz, sx * octave, sy * octave, sz * octave, octave);
        octave /= 2.0;
    }
}

// func_4109_a(): the 2D form.
static void octaves_gen2(octaves_t const* o, double* out, int x0, int z0, int nx, int nz, double sx, double sz) {
    octaves_gen(o, out, (double)x0, 10.0, (double)z0, nx, 1, nz, sx, 1.0, sz, o->n);
}

// --- ChunkProviderGenerate ---------------------------------------------------

// Everything one seed needs: the generators, built in Beta's order from
// one java.util.Random, since each takes its tables from the stream the
// one before left.
typedef struct {
    uint32_t  seed;
    bool      ready;
    perlin_t  all[16 + 16 + 8 + 4 + 4 + 10 + 16];
    octaves_t low, high, sel, sand, stone, scale, depth;
} beta_gen_t;

// Per-chunk scratch.
typedef struct {
    double density[5 * 17 * 5];
    double low[5 * 17 * 5], high[5 * 17 * 5], sel[5 * 17 * 5];
    double scale[5 * 5], depth[5 * 5];
    double sand[16 * 16], gravel[16 * 16], stone[16 * 16];
    uint8_t column[16 * 16 * 128];
} beta_scratch_t;

static beta_gen_t*     s_gen;
static beta_scratch_t* s_scr;

static bool beta_ready(uint32_t seed) {
    if (s_gen == NULL) s_gen = cm_calloc(1, sizeof(beta_gen_t));
    if (s_scr == NULL) s_scr = cm_alloc(sizeof(beta_scratch_t));
    if (s_gen == NULL || s_scr == NULL) return false;
    if (s_gen->ready && s_gen->seed == seed) return true;

    // Our seeds are 32-bit; Beta's were 64. Sign-extended, so a seed
    // typed as a negative number means what it would have in Beta.
    java_random_t r;
    java_random_seed(&r, (int64_t)(int32_t)seed);
    perlin_t* g = s_gen->all;
    octaves_t* const order[] = {&s_gen->low, &s_gen->high, &s_gen->sel, &s_gen->sand, &s_gen->stone, &s_gen->scale,
                                &s_gen->depth};
    int const sizes[] = {16, 16, 8, 4, 4, 10, 16};
    for (int k = 0; k < 7; k++) {
        order[k]->n = sizes[k];
        order[k]->g = g;
        for (int i = 0; i < sizes[k]; i++) perlin_init(g++, &r);
    }
    s_gen->seed  = seed;
    s_gen->ready = true;
    return true;
}

// func_4061_a(): the density at the 5 x 17 x 5 sample lattice of Beta
// chunk (bcx, bcz). Sample (i, j, k) is at block (i * 4, j * 8, k * 4).
static void beta_density(int32_t bcx, int32_t bcz, bool full) {
    beta_gen_t const*     g  = s_gen;
    beta_scratch_t* const s  = s_scr;
    int const             nx = 5, ny = 17, nz = 5;
    int const             x0 = bcx * 4, z0 = bcz * 4;
    double const          d  = 684.412;
    int const             lh = full ? 16 : 1;  // the fast path: the first octave only (see the top)

    if (full) {
        octaves_gen2(&g->scale, s->scale, x0, z0, nx, nz, 1.121, 1.121);
        octaves_gen2(&g->depth, s->depth, x0, z0, nx, nz, 200.0, 200.0);
    }
    octaves_gen(&g->sel, s->sel, x0, 0, z0, nx, ny, nz, d / 80.0, d / 160.0, d / 80.0, 8);
    octaves_gen(&g->low, s->low, x0, 0, z0, nx, ny, nz, d, d, d, lh);
    octaves_gen(&g->high, s->high, x0, 0, z0, nx, ny, nz, d, d, d, lh);

    // Biomes left out: a constant temperature and humidity, a plains-ish
    // 0.8 and 0.4. They only scale the falloff.
    double const temp = 0.8, hum = 0.4;
    int          n = 0, col = 0;
    for (int i = 0; i < nx; i++) {
        for (int k = 0; k < nz; k++) {
            double dd = 1.0 - hum * temp;
            dd *= dd;
            dd *= dd;
            dd = 1.0 - dd;
            double scale = 0.0, depth = 0.0;
            if (full) {
                scale = (s->scale[col] + 256.0) / 512.0;
                scale *= dd;
                if (scale > 1.0) scale = 1.0;
                depth = s->depth[col] / 8000.0;
                if (depth < 0.0) depth = -depth * 0.3;
                depth = depth * 3.0 - 2.0;
                if (depth < 0.0) {
                    depth /= 2.0;
                    if (depth < -1.0) depth = -1.0;
                    depth /= 1.4;
                    depth /= 2.0;
                    scale = 0.0;
                } else {
                    if (depth > 1.0) depth = 1.0;
                    depth /= 8.0;
                }
                if (scale < 0.0) scale = 0.0;
                scale += 0.5;
                depth = depth * (double)ny / 16.0;
            }
            double const mid = (double)ny / 2.0 + depth * 4.0;
            col++;
            for (int j = 0; j < ny; j++) {
                double falloff = 0.0;
                if (full) {
                    falloff = (((double)j - mid) * 12.0) / scale;
                    if (falloff < 0.0) falloff *= 4.0;
                }
                double const lo = s->low[n] / 512.0;
                double const hi = s->high[n] / 512.0;
                double const t  = (s->sel[n] / 10.0 + 1.0) / 2.0;
                double       v  = t < 0.0 ? lo : t > 1.0 ? hi : lo + (hi - lo) * t;
                v -= falloff;
                if (j > ny - 4) {
                    double const top = (double)(float)((float)(j - (ny - 4)) / 3.0f);
                    v                = v * (1.0 - top) + -10.0 * top;
                }
                s->density[n++] = v;
            }
        }
    }
}

// generateTerrain(): interpolate the lattice to blocks; stone where the
// density is positive, water below Beta's sea level (64), else air.
static void beta_terrain(void) {
    beta_scratch_t* const s   = s_scr;
    double const*         den = s->density;
#define D(i, k, j) den[((i) * 5 + (k)) * 17 + (j)]
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < 16; j++) {
                double       a  = D(i, k, j), b = D(i, k + 1, j), c = D(i + 1, k, j), e = D(i + 1, k + 1, j);
                double const da = (D(i, k, j + 1) - a) * 0.125, db = (D(i, k + 1, j + 1) - b) * 0.125;
                double const dc = (D(i + 1, k, j + 1) - c) * 0.125, de = (D(i + 1, k + 1, j + 1) - e) * 0.125;
                for (int y8 = 0; y8 < 8; y8++) {
                    double       xa = a, xb = b;
                    double const sa = (c - a) * 0.25, sb = (e - b) * 0.25;
                    for (int x4 = 0; x4 < 4; x4++) {
                        int const    x  = i * 4 + x4, y = j * 8 + y8;
                        double       v  = xa;
                        double const sv = (xb - xa) * 0.25;
                        for (int z4 = 0; z4 < 4; z4++) {
                            int const z = k * 4 + z4;
                            uint8_t   b8 = y < 64 ? BLK_WATER : BLK_AIR;
                            if (v > 0.0) b8 = BLK_STONE;
                            s->column[(x * 16 + z) * 128 + y] = b8;
                            v += sv;
                        }
                        xa += sa;
                        xb += sb;
                    }
                    a += da;
                    b += db;
                    c += dc;
                    e += de;
                }
            }
        }
    }
#undef D
}

// replaceBlocksForBiome(): grass over dirt, sand and gravel beaches round
// sea level, bedrock at the bottom -- with Beta's own per-chunk Random, so
// the beaches and the bedrock fall where Beta put them.
// A stand-in id for Beta's sandstone while the surface pass runs; never
// leaves it.
#define SANDSTONE 0xFE

static void beta_surface(int32_t bcx, int32_t bcz) {
    beta_gen_t const*     g = s_gen;
    beta_scratch_t* const s = s_scr;
    java_random_t         r;
    java_random_seed(&r, (int64_t)((uint64_t)(int64_t)bcx * 0x4F9939F508ULL + (uint64_t)(int64_t)bcz * 0x1EF1565BD5ULL));

    int const    sea = 64;
    double const d   = 0.03125;
    octaves_gen(&g->sand, s->sand, (double)bcx * 16, (double)bcz * 16, 0.0, 16, 16, 1, d, d, 1.0, g->sand.n);
    octaves_gen(&g->sand, s->gravel, (double)bcx * 16, 109.0134, (double)bcz * 16, 16, 1, 16, d, 1.0, d, g->sand.n);
    octaves_gen(&g->stone, s->stone, (double)bcx * 16, (double)bcz * 16, 0.0, 16, 16, 1, d * 2.0, d * 2.0, d * 2.0,
                g->stone.n);

    for (int k = 0; k < 16; k++) {      // z
        for (int l = 0; l < 16; l++) {  // x
            bool const sand   = s->sand[k + l * 16] + java_random_next_double(&r) * 0.2 > 0.0;
            bool const gravel = s->gravel[k + l * 16] + java_random_next_double(&r) * 0.2 > 3.0;
            int const  depth  = java_d2i(s->stone[k + l * 16] / 3.0 + 3.0 + java_random_next_double(&r) * 0.25);
            int        run    = -1;
            uint8_t    top = BLK_GRASS, fill = BLK_DIRT;
            for (int y = 127; y >= 0; y--) {
                uint8_t* const cell = &s->column[(l * 16 + k) * 128 + y];
                if (y <= java_random_next_int_n(&r, 5)) {
                    *cell = BLK_BEDROCK;
                    continue;
                }
                uint8_t const b = *cell;
                if (b == BLK_AIR) {
                    run = -1;
                    continue;
                }
                if (b != BLK_STONE) continue;
                if (run == -1) {
                    if (depth <= 0) {
                        top  = BLK_AIR;
                        fill = BLK_STONE;
                    } else if (y >= sea - 4 && y <= sea + 1) {
                        top  = BLK_GRASS;
                        fill = BLK_DIRT;
                        if (gravel) top = BLK_AIR;
                        if (gravel) fill = BLK_GRAVEL;
                        if (sand) top = BLK_SAND;
                        if (sand) fill = BLK_SAND;
                    }
                    if (y < sea && top == BLK_AIR) top = BLK_WATER;
                    run   = depth;
                    *cell = y >= sea - 1 ? top : fill;
                    continue;
                }
                if (run > 0) {
                    run--;
                    *cell = fill;
                    // Beta turns the sand's bottom into sandstone -- once,
                    // which is why it is tracked as its own fill until the
                    // column is done: a block we do not have, so it
                    // becomes sand below.
                    if (run == 0 && fill == BLK_SAND) {
                        run  = java_random_next_int_n(&r, 4);
                        fill = SANDSTONE;
                    }
                }
            }
            uint8_t* const colp = &s->column[(l * 16 + k) * 128];
            for (int y = 0; y < 128; y++)
                if (colp[y] == SANDSTONE) colp[y] = BLK_SAND;
        }
    }
}

bool farlands_beta_column(int32_t bcx, int32_t bcz, uint32_t seed, bool full, uint8_t* out) {
    if (!beta_ready(seed)) return false;
    beta_density(bcx, bcz, full);
    beta_terrain();
    beta_surface(bcx, bcz);
    if (out != NULL) memcpy(out, s_scr->column, sizeof(s_scr->column));
    return true;
}

// The first Beta chunk wholly past the overflow. Its lattice starts at
// sample x -3137712; the finest octave overflows from about -3137706
// (2^31 / 684.412, less the octave's random offset of up to 256), so all
// five of its x samples are overflowed. The next chunk east, -784427, is
// where Beta's Far Lands really began, part way in (x -12,550,821).
#define BETA_FIRST_FAR_CX (-784428)

int32_t farlands_beta_cx(int32_t cx, int32_t edge_x) {
    int32_t const first = edge_x / CH_W - 1;  // our first Far Lands chunk
    return (int32_t)((int64_t)cx - first + BETA_FIRST_FAR_CX);
}

// Our y 0..24 are Beta's 0..63, under the sea; our 25..63 its 64..127.
int farlands_beta_y(int y) {
    if (y <= CH_SEA_LEVEL) return (y * 63) / CH_SEA_LEVEL;
    return 64 + ((y - CH_SEA_LEVEL - 1) * 63) / (CH_H - CH_SEA_LEVEL - 2);
}

bool farlands_generate(chunk_t* c, uint32_t seed, int32_t edge_x) {
    memset(c->id, BLK_AIR, CH_CELLS);
    memset(c->st, 0, CH_CELLS);
    int32_t const bcx = farlands_beta_cx(c->cx, edge_x);
    if (!farlands_beta_column(bcx, c->cz, seed, false, NULL)) return false;

    uint8_t const* const col = s_scr->column;
    for (int z = 0; z < CH_D; z++) {
        for (int x = 0; x < CH_W; x++) {
            uint8_t* const       id = &c->id[CH_IDX(x, 0, z)];
            uint8_t const* const bc = &col[(x * 16 + z) * 128];
            for (int y = 0; y < CH_H; y++) id[y] = bc[farlands_beta_y(y)];
            // Sampling 128 rows into 64 can skip the row a surface was
            // on: grass that ends up under something is dirt, and dirt
            // that ends up under open sky above the sea is grass.
            for (int y = 0; y + 1 < CH_H; y++) {
                if (id[y] == BLK_GRASS && id[y + 1] != BLK_AIR) id[y] = BLK_DIRT;
                else if (id[y] == BLK_DIRT && id[y + 1] == BLK_AIR && y > CH_SEA_LEVEL) id[y] = BLK_GRASS;
            }
        }
    }
    return true;
}
