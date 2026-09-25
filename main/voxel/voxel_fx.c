// =====================================================================
//  SynthMiner  --  mining and building effects (see voxel_fx.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_fx.c. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/voxel_fx.h"
#include <math.h>
#include "common/texcache.h"
#include "math/camera.h"
#include "math/mesh_render.h"
#include "synthengine3d.h"
#include "voxel/voxel_mesh.h"
#include "world/chunk_render.h"

#define OUTLINE_ARGB 0xFF141414u
#define CRACK_ARGB   0xFF1C1A18u
#define CRACK_RAYS   5
#define CRACK_STEPS  4
#define CHIPS        14
#define GRAVITY      14.0f  // blocks/s^2
#define ITEM_HALF    0.12f
#define ITEM_FALL    0.3f   // seconds to reach the floor
#define ITEM_FLY     0.25f  // seconds from pick-up to the miner

static int                 s_users;
static mesh_t              s_item, s_pop;
static se_texture_t const* s_flame;

void voxel_fx_init(void) {
    if (s_users++ > 0) return;
    mesh_init(&s_item);
    s_item.name = "fx_item";
    voxel_build_cube(&s_item, ITEM_HALF);
    mesh_init(&s_pop);
    s_pop.name = "fx_pop";
    voxel_build_cube(&s_pop, 0.5f);
    s_flame = texcache_get("synthminer/torch_flame.png");
}

void voxel_fx_shutdown(void) {
    if (s_users == 0 || --s_users > 0) return;
    mesh_free(&s_item);
    mesh_free(&s_pop);
}

static void line(vec3_t a, vec3_t b, uint32_t argb) {
    scene_line(a.x, a.y, a.z, b.x, b.y, b.z, argb);
}

void voxel_fx_outline(int x, int y, int z) {
    float const  e  = 0.004f;  // just outside the faces
    vec3_t const lo = v3((float)x - e, (float)y - e, (float)z - e);
    vec3_t const hi = v3((float)x + 1 + e, (float)y + 1 + e, (float)z + 1 + e);
    for (int i = 0; i < 8; i++) {
        vec3_t const p = v3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (i & bit) continue;  // each edge once, from its low end
            int const    j = i | bit;
            vec3_t const q = v3(j & 1 ? hi.x : lo.x, j & 2 ? hi.y : lo.y, j & 4 ? hi.z : lo.z);
            line(p, q, OUTLINE_ARGB);
        }
    }
}

// The six faces: outward normal, a corner, and the two edge directions
// from it (u, v: the face spans corner + [0,1] u + [0,1] v).
typedef struct {
    vec3_t n, o, u, v;
} face_t;

static face_t face_of(int k, float x, float y, float z) {
    switch (k) {
        case 0:
            return (face_t){{1, 0, 0}, {x + 1, y, z}, {0, 0, 1}, {0, 1, 0}};
        case 1:
            return (face_t){{-1, 0, 0}, {x, y, z}, {0, 0, 1}, {0, 1, 0}};
        case 2:
            return (face_t){{0, 1, 0}, {x, y + 1, z}, {1, 0, 0}, {0, 0, 1}};
        case 3:
            return (face_t){{0, -1, 0}, {x, y, z}, {1, 0, 0}, {0, 0, 1}};
        case 4:
            return (face_t){{0, 0, 1}, {x, y, z + 1}, {1, 0, 0}, {0, 1, 0}};
        default:
            return (face_t){{0, 0, -1}, {x, y, z}, {1, 0, 0}, {0, 1, 0}};
    }
}

void voxel_fx_cracks(int x, int y, int z, float progress, unsigned seed) {
    if (progress <= 0.0f) return;
    vec3_t const eye = camera_eye();
    float const  X = (float)x, Y = (float)y, Z = (float)z;
    // Rays from a point near the middle, each a jagged line of
    // CRACK_STEPS pieces, all growing together: progress 1 is the whole
    // pattern.
    float const  grown = progress * (float)CRACK_STEPS;
    for (int k = 0; k < 6; k++) {
        face_t const f      = face_of(k, X, Y, Z);
        vec3_t const centre = v3_add(f.o, v3_add(v3_scale(f.u, 0.5f), v3_scale(f.v, 0.5f)));
        if (v3_dot(f.n, v3_sub(eye, centre)) <= 0.0f) continue;  // a face turned away
        vec3_t const   lift = v3_scale(f.n, 0.004f);
        unsigned const s    = seed * 31u + (unsigned)k;
        float          cu = 0.4f + 0.2f * hash01((int)s, 60u), cv = 0.4f + 0.2f * hash01((int)s, 61u);
        for (int r = 0; r < CRACK_RAYS; r++) {
            float a  = 6.2831853f * ((float)r + 0.6f * hash01((int)(s * 7u) + r, 62u)) / (float)CRACK_RAYS;
            float pu = cu, pv = cv;
            for (int step = 0; step < CRACK_STEPS && (float)step < grown; step++) {
                float const part  = fminf(grown - (float)step, 1.0f);
                float const len   = (0.09f + 0.07f * hash01((int)(s * 97u) + r * 8 + step, 63u)) * part;
                a                += 0.9f * (hash01((int)(s * 131u) + r * 8 + step, 64u) - 0.5f);
                float const  qu = clampf(pu + len * cosf(a), 0.0f, 1.0f), qv = clampf(pv + len * sinf(a), 0.0f, 1.0f);
                vec3_t const p = v3_add(v3_add(f.o, lift), v3_add(v3_scale(f.u, pu), v3_scale(f.v, pv)));
                vec3_t const q = v3_add(v3_add(f.o, lift), v3_add(v3_scale(f.u, qu), v3_scale(f.v, qv)));
                line(p, q, CRACK_ARGB);
                pu = qu, pv = qv;
            }
        }
    }
}

static uint32_t shade_argb(uint32_t c, float f) {
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        float const v  = (float)((c >> sh) & 0xFF) * f;
        out           |= (uint32_t)(v > 255.0f ? 255.0f : v) << sh;
    }
    return out;
}

// A small square facing the camera, flat colour, unlit.
static void chip(vec3_t p, float size, uint32_t argb, mat3_t const* cam) {
    vec3_t const r = v3_scale(cam->right, size), u = v3_scale(cam->up, size);
    vec3_t const a = v3_sub(v3_sub(p, r), u), b = v3_sub(v3_add(p, r), u);
    vec3_t const c = v3_add(v3_add(p, r), u), d = v3_add(v3_sub(p, r), u);
    scene_tri(a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, argb, SE_TRI_EMISSIVE);
    scene_tri(a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z, argb, SE_TRI_EMISSIVE);
}

void voxel_fx_break(int x, int y, int z, uint8_t block, float since, unsigned seed) {
    if (since < 0.0f || since > VOXEL_BREAK_SECS) return;
    mat3_t const   cam  = camera_basis();
    int const      mat  = voxel_face_mat(block, VF_SIDE);
    uint32_t const base = voxel_mat_argb(mat);
    for (int i = 0; i < CHIPS; i++) {
        int const    key = (int)seed * 53 + i;
        float const  ox = hash01(key, 70u) - 0.5f, oy = hash01(key, 71u) - 0.5f, oz = hash01(key, 72u) - 0.5f;
        vec3_t const p0  = v3((float)x + 0.5f + 0.6f * ox, (float)y + 0.5f + 0.6f * oy, (float)z + 0.5f + 0.6f * oz);
        vec3_t const v0  = v3(2.2f * ox, 1.8f + 2.2f * hash01(key, 73u), 2.2f * oz);
        vec3_t       p   = v3_add(p0, v3_scale(v0, since));
        p.y             -= 0.5f * GRAVITY * since * since;
        if (p.y < (float)y + 0.05f) p.y = (float)y + 0.05f;  // they come to rest on the floor
        float const size = 0.06f * (1.0f - smoothstep(0.55f * VOXEL_BREAK_SECS, VOXEL_BREAK_SECS, since));
        chip(p, size, shade_argb(base, 0.8f + 0.4f * hash01(key, 74u)), &cam);
    }
}

void voxel_fx_item(int x, int y, int z, uint8_t block, float since, float floor_y, float pick_at, vec3_t to) {
    if (since < 0.0f) return;
    float const fly = (since - pick_at) / ITEM_FLY;
    if (fly >= 1.0f) return;  // picked up
    float const land = smoothstep(0.0f, ITEM_FALL, since);
    float const rest = floor_y + ITEM_HALF + 0.08f + 0.05f * sinf(4.0f * since);
    vec3_t      p    = v3((float)x + 0.5f, (float)y + 0.5f + (rest - ((float)y + 0.5f)) * land, (float)z + 0.5f);
    float       sc   = 1.0f;
    if (fly > 0.0f) {
        p  = v3_lerp(p, to, fly * fly);
        sc = 1.0f - 0.6f * fly;
    }
    xform_t const at = {mat3_rot_y(2.0f * since), p, sc};
    mesh_mat_t    mats[3];
    voxel_cube_mats(block, mats);
    mesh_submit(&s_item, &at, mats, 3);
}

void voxel_fx_flame(int x, int y, int z, float t, unsigned seed) {
    if (!s_flame) return;
    float const  h    = 0.26f * (0.8f + 0.4f * value_noise(t, 11.0f, seed));
    float const  w    = 0.09f;
    vec3_t const base = v3((float)x + 0.5f, (float)y + 0.6f, (float)z + 0.5f);
    // u runs up the flame (white-hot at the base, red at the tip), v across.
    for (int k = 0; k < 2; k++) {
        vec3_t const          across = k ? v3(w, 0, w) : v3(w, 0, -w);
        vec3_t const          a = v3_sub(base, across), b = v3_add(base, across);
        vec3_t const          c = v3_add(b, v3(0, h, 0)), d = v3_add(a, v3(0, h, 0));
        se_tex_vertex_t const t1[3] = {
            {a.x, a.y, a.z, 0.02f, 0.0f}, {b.x, b.y, b.z, 0.02f, 1.0f}, {c.x, c.y, c.z, 0.95f, 1.0f}};
        se_tex_vertex_t const t2[3] = {
            {a.x, a.y, a.z, 0.02f, 0.0f}, {c.x, c.y, c.z, 0.95f, 1.0f}, {d.x, d.y, d.z, 0.95f, 0.0f}};
        scene_textured_tri(t1, s_flame, SE_TRI_EMISSIVE);
        scene_textured_tri(t2, s_flame, SE_TRI_EMISSIVE);
    }
}

void voxel_fx_pop(int x, int y, int z, uint8_t block, float since) {
    if (since < 0.0f || since >= VOXEL_POP_SECS) return;
    float const   sc = 0.8f + 0.2f * smoothstep(0.0f, VOXEL_POP_SECS, since);
    xform_t const at = {mat3_rot_y(0.0f), v3((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f), sc};
    mesh_mat_t    mats[3];
    voxel_cube_mats(block, mats);
    mesh_submit(&s_pop, &at, mats, 3);
}
