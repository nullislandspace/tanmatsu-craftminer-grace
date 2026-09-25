// =====================================================================
//  SynthMiner  --  the sky over the block world (see voxel_sky.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_sky.c. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/voxel_sky.h"
#include <math.h>
#include "math/camera.h"
#include "synthengine3d.h"
#include "common/rng.h"
#include "voxel/starfield.h"
#include "world/chunk.h"

#define BODY_DIST   350.0f  // the sun and moon: far behind the terrain (draw distance 64)
#define SUN_HALF    22.0f
#define MOON_HALF   16.0f
#define CLOUD_CELL  8.0f  // blocks
#define CLOUD_THICK 2.0f
#define CLOUD_SPEED 0.8f   // blocks/s, east (+x)
// Clouds this far round the eye are drawn. The showreel's 100 would be
// a thousand cells here, for a frame rate that has none to spare; 64
// is about two hundred cells and still reaches the draw distance.
#define CLOUD_R 64.0f
#define STARS_AT 0.3f  // stars once the daylight is below this

static void quad(vec3_t a, vec3_t b, vec3_t c, vec3_t d, uint32_t argb) {
    scene_tri(a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, argb, SE_TRI_EMISSIVE);
    scene_tri(a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z, argb, SE_TRI_EMISSIVE);
}

// A square facing the eye, `dist` along `dir`.
static void body(vec3_t eye, vec3_t dir, float half, uint32_t argb) {
    vec3_t const c     = v3_add(eye, v3_scale(dir, BODY_DIST));
    vec3_t const hint  = fabsf(dir.y) > 0.95f ? v3(1, 0, 0) : v3(0, 1, 0);
    vec3_t const right = v3_scale(v3_norm(v3_cross(hint, dir)), half);
    vec3_t const up    = v3_scale(v3_norm(v3_cross(dir, right)), half);
    quad(v3_sub(v3_sub(c, right), up), v3_sub(v3_add(c, right), up), v3_add(v3_add(c, right), up),
         v3_add(v3_sub(c, right), up), argb);
}

static uint32_t mix(uint32_t a, uint32_t b, float f) {
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float const ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << s;
    }
    return out;
}

// One slab of cloud, x0..x1 by z0..z1: the faces turned towards the eye.
static void slab(vec3_t eye, float x0, float x1, float z0, float z1, uint32_t top, uint32_t side, uint32_t bottom) {
    float const y0 = VOX_CLOUD_Y, y1 = VOX_CLOUD_Y + CLOUD_THICK;
    if (eye.y > y1) quad(v3(x0, y1, z0), v3(x0, y1, z1), v3(x1, y1, z1), v3(x1, y1, z0), top);
    if (eye.y < y0) quad(v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y0, z1), v3(x0, y0, z1), bottom);
    if (eye.x < x0) quad(v3(x0, y1, z1), v3(x0, y1, z0), v3(x0, y0, z0), v3(x0, y0, z1), side);
    if (eye.x > x1) quad(v3(x1, y1, z0), v3(x1, y1, z1), v3(x1, y0, z1), v3(x1, y0, z0), side);
    if (eye.z < z0) quad(v3(x0, y1, z0), v3(x1, y1, z0), v3(x1, y0, z0), v3(x0, y0, z0), side);
    if (eye.z > z1) quad(v3(x1, y1, z1), v3(x0, y1, z1), v3(x0, y0, z1), v3(x1, y0, z1), side);
}

// Cloud cells in WORLD cell coordinates: blobs of a smooth noise, not
// a salt-and-pepper hash.
static int cloudy(int i, int k) {
    return sm_noise2((float)i, (float)k, 3.2f, 50u) > 0.6f;
}

void voxel_sky_submit(float t, vec3_t sun_dir, uint32_t fog_argb, float light, int32_t ox, int32_t oz, bool clouds) {
    vec3_t const eye = camera_eye();
    // The stars first, so everything else draws over them.
    if (light < STARS_AT) {
        starfield_init();
        starfield_submit_above(0.03f);
    }
    if (sun_dir.y > -0.2f) body(eye, sun_dir, SUN_HALF, 0xFFFFF4C0u);
    vec3_t const moon = v3_scale(sun_dir, -1.0f);
    if (moon.y > -0.2f) body(eye, moon, MOON_HALF, 0xFFE4E8F4u);

    if (!clouds) return;
    // Clouds: cells of a noise pattern that drifts east; a run of cloudy
    // cells along x is one slab. Worked out in world coordinates (the
    // eye plus the origin) and drawn relative to the origin. The drift
    // wraps every few hours of play, so the float stays small.
    float const shift = fmodf(CLOUD_SPEED * t, CLOUD_CELL * 4096.0f);
    double const wex  = (double)eye.x + (double)ox, wez = (double)eye.z + (double)oz;
    int const    i0   = (int)floor((wex - shift - CLOUD_R) / CLOUD_CELL),
              i1      = (int)ceil((wex - shift + CLOUD_R) / CLOUD_CELL);
    int const k0 = (int)floor((wez - CLOUD_R) / CLOUD_CELL), k1 = (int)ceil((wez + CLOUD_R) / CLOUD_CELL);
    uint32_t const top = mix(0xFF000000u, 0xFFF8F8FAu, light), side = mix(0xFF000000u, 0xFFE6EAF0u, light),
                   bottom = mix(0xFF000000u, 0xFFD2D8E2u, light);
    for (int k = k0; k <= k1; k++) {
        for (int i = i0; i <= i1; i++) {
            if (!cloudy(i, k) || cloudy(i - 1, k)) continue;  // the start of a run
            int j = i;
            while (j + 1 <= i1 && cloudy(j + 1, k)) j++;
            float const x0 = (float)((double)i * CLOUD_CELL + shift - (double)ox);
            float const x1 = x0 + (float)(j - i + 1) * CLOUD_CELL;
            float const z0 = (float)((double)k * CLOUD_CELL - (double)oz), z1 = z0 + CLOUD_CELL;
            float const dx = fmaxf(fmaxf(x0 - eye.x, eye.x - x1), 0.0f),
                        dz = fmaxf(fmaxf(z0 - eye.z, eye.z - z1), 0.0f);
            float const f  = 0.7f * smoothstep(50.0f, CLOUD_R, sqrtf(dx * dx + dz * dz));
            slab(eye, x0, x1, z0, z1, mix(top, fog_argb, f), mix(side, fog_argb, f), mix(bottom, fog_argb, f));
            i = j;
        }
    }
}
