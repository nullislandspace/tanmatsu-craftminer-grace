// =====================================================================
//  CraftMiner  --  seeded noise (see rng.h)
// =====================================================================

#include "common/rng.h"

#include <math.h>

// Hermite smoothstep on the cell fraction. Using it on BOTH axes is
// what makes the noise C1-continuous across lattice cells, so terrain
// has no visible grid.
static inline float fade(float t) {
    return t * t * (3.0f - 2.0f * t);
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

float cm_noise2(float x, float z, float scale, uint32_t seed) {
    float const fx = x / scale, fz = z / scale;
    float const flx = floorf(fx), flz = floorf(fz);
    int32_t const ix = (int32_t)flx, iz = (int32_t)flz;
    float const u = fade(fx - flx), v = fade(fz - flz);

    float const n00 = cm_rand2(ix, iz, seed);
    float const n10 = cm_rand2(ix + 1, iz, seed);
    float const n01 = cm_rand2(ix, iz + 1, seed);
    float const n11 = cm_rand2(ix + 1, iz + 1, seed);

    return lerpf(lerpf(n00, n10, u), lerpf(n01, n11, u), v);
}

float cm_noise3(float x, float y, float z, float scale, uint32_t seed) {
    float const fx = x / scale, fy = y / scale, fz = z / scale;
    float const flx = floorf(fx), fly = floorf(fy), flz = floorf(fz);
    int32_t const ix = (int32_t)flx, iy = (int32_t)fly, iz = (int32_t)flz;
    float const u = fade(fx - flx), v = fade(fy - fly), w = fade(fz - flz);

    float const c000 = cm_rand3(ix, iy, iz, seed);
    float const c100 = cm_rand3(ix + 1, iy, iz, seed);
    float const c010 = cm_rand3(ix, iy + 1, iz, seed);
    float const c110 = cm_rand3(ix + 1, iy + 1, iz, seed);
    float const c001 = cm_rand3(ix, iy, iz + 1, seed);
    float const c101 = cm_rand3(ix + 1, iy, iz + 1, seed);
    float const c011 = cm_rand3(ix, iy + 1, iz + 1, seed);
    float const c111 = cm_rand3(ix + 1, iy + 1, iz + 1, seed);

    float const x00 = lerpf(c000, c100, u), x10 = lerpf(c010, c110, u);
    float const x01 = lerpf(c001, c101, u), x11 = lerpf(c011, c111, u);
    return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w);
}

float cm_fbm2(float x, float z, float scale, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, s = scale;
    for (int i = 0; i < octaves; i++) {
        // A different seed per octave, so two octaves never line up.
        sum += amp * cm_noise2(x, z, s, seed + (uint32_t)i * 0x9E3779B9u);
        norm += amp;
        amp *= 0.5f;
        s *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}
