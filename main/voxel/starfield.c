// =====================================================================
//  CraftMiner  --  starfield (see starfield.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/common/starfield.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/starfield.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "math/camera.h"
#include "synthengine3d.h"
#include "math/xform.h"

#define STAR_COUNT     600
// How far out the stars are drawn. Far beyond any geometry (so the depth
// test puts them behind everything), well inside the depth encoding's
// ~30000-unit limit (se_scene.h).
#define STAR_DIST      1000.0f
// Share of the stars in the "galactic" band, and its half-width.
#define BAND_SHARE     0.4f
#define BAND_HALFWIDTH 0.18f  // radians

typedef struct {
    vec3_t   dir;
    uint32_t argb;
} star_t;

static star_t s_stars[STAR_COUNT];
static bool   s_ready;

void starfield_init(void) {
    if (s_ready) return;
    unsigned const seed = 0x57A2u;
    // The band's plane: tilted so it crosses the default views diagonally.
    mat3_t const   band = mat3_from_ypr(0.6f, 0.9f, 0.3f);
    for (int i = 0; i < STAR_COUNT; i++) {
        float const r0 = hash01(i * 5 + 0, seed), r1 = hash01(i * 5 + 1, seed);
        float const r2 = hash01(i * 5 + 2, seed), r3 = hash01(i * 5 + 3, seed);
        float const r4 = hash01(i * 5 + 4, seed);
        vec3_t      d;
        if (r4 < BAND_SHARE) {
            // In the band: a random longitude, latitude close to 0.
            float const lon = r0 * 2.0f * (float)M_PI;
            float const lat = (r1 - 0.5f) * 2.0f * BAND_HALFWIDTH * (0.3f + 0.7f * r1);
            d               = mat3_apply(&band, v3(cosf(lat) * cosf(lon), sinf(lat), cosf(lat) * sinf(lon)));
        } else {
            // Uniform on the sphere.
            float const zc = r0 * 2.0f - 1.0f;
            float const a  = r1 * 2.0f * (float)M_PI;
            float const rr = sqrtf(1.0f - zc * zc);
            d              = v3(rr * cosf(a), rr * sinf(a), zc);
        }
        // Brightness: steep power law, so most stars are faint.
        float const b  = 0.18f + 0.82f * powf(r2, 6.0f);
        // Tint: mostly white, some blue-white, some warm.
        float       tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (r3 < 0.2f) {
            tr = 0.75f;
            tg = 0.85f;
        } else if (r3 > 0.85f) {
            tg = 0.9f;
            tb = 0.7f;
        }
        uint32_t const cr = (uint32_t)(255.0f * b * tr), cg = (uint32_t)(255.0f * b * tg);
        uint32_t const cb = (uint32_t)(255.0f * b * tb);
        s_stars[i]        = (star_t){d, 0xFF000000u | (cr << 16) | (cg << 8) | cb};
    }
    s_ready = true;
}

void starfield_submit(void) {
    if (!s_ready) return;
    vec3_t const eye = camera_eye();
    for (int i = 0; i < STAR_COUNT; i++) {
        vec3_t const p = v3_add(eye, v3_scale(s_stars[i].dir, STAR_DIST));
        scene_point(p.x, p.y, p.z, s_stars[i].argb);
    }
}

void starfield_submit_turned(mat3_t const* turn) {
    if (!s_ready) return;
    vec3_t const eye = camera_eye();
    for (int i = 0; i < STAR_COUNT; i++) {
        vec3_t const p = v3_add(eye, v3_scale(mat3_apply(turn, s_stars[i].dir), STAR_DIST));
        scene_point(p.x, p.y, p.z, s_stars[i].argb);
    }
}

void starfield_submit_above(float min_y) {
    if (!s_ready) return;
    vec3_t const eye = camera_eye();
    for (int i = 0; i < STAR_COUNT; i++) {
        if (s_stars[i].dir.y < min_y) continue;
        vec3_t const p = v3_add(eye, v3_scale(s_stars[i].dir, STAR_DIST));
        scene_point(p.x, p.y, p.z, s_stars[i].argb);
    }
}
