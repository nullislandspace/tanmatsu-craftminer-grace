// =====================================================================
//  Showreel  --  vectors, transforms and paths (see xform.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/xform.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "math/xform.h"
#include <stdint.h>

mat3_t mat3_from_ypr(float yaw, float pitch, float roll) {
    // Same expansion as the engine's camera_build_basis(), so an object
    // posed with these angles and a camera given the same angles agree.
    float const cy = cosf(yaw), sy = sinf(yaw);
    float const cp = cosf(pitch), sp = sinf(pitch);
    float const cr = cosf(roll), sr = sinf(roll);
    return (mat3_t){
        .right = v3(cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr),
        .up    = v3(-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr),
        .fwd   = v3(sy * cp, -sp, cy * cp),
    };
}

mat3_t mat3_rot_x(float a) {
    return mat3_from_ypr(0.0f, a, 0.0f);
}
mat3_t mat3_rot_y(float a) {
    return mat3_from_ypr(a, 0.0f, 0.0f);
}
mat3_t mat3_rot_z(float a) {
    return mat3_from_ypr(0.0f, 0.0f, a);
}

mat3_t mat3_mul(mat3_t const* a, mat3_t const* b) {
    return (mat3_t){
        .right = mat3_apply(a, b->right),
        .up    = mat3_apply(a, b->up),
        .fwd   = mat3_apply(a, b->fwd),
    };
}

mat3_t mat3_stretch(mat3_t const* m, vec3_t s) {
    return (mat3_t){
        .right = v3_scale(m->right, s.x),
        .up    = v3_scale(m->up, s.y),
        .fwd   = v3_scale(m->fwd, s.z),
    };
}

mat3_t mat3_axis_angle(vec3_t k, float angle) {
    // Rodrigues: R p = p cos + (k x p) sin + k (k . p)(1 - cos), applied
    // to the three unit axes to get the columns.
    float const c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    return (mat3_t){
        .right = v3(t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y),
        .up    = v3(t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x),
        .fwd   = v3(t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c),
    };
}

float mat3_det(mat3_t const* m) {
    return v3_dot(m->right, v3_cross(m->up, m->fwd));
}

mat3_t mat3_from_fwd_up(vec3_t fwd, vec3_t up_hint, float roll) {
    vec3_t const f = v3_norm(fwd);
    vec3_t       r = v3_cross(up_hint, f);  // right = up x fwd in this axis system
    if (v3_len(r) < 1e-4f) {
        // Looking straight along the hint: any perpendicular will do.
        r = v3_cross(fabsf(f.y) < 0.9f ? v3(0.0f, 1.0f, 0.0f) : v3(1.0f, 0.0f, 0.0f), f);
    }
    r               = v3_norm(r);
    vec3_t const u  = v3_cross(f, r);
    // Roll about forward, as Rz(roll) does for the camera: the right
    // axis turns towards up.
    float const  cr = cosf(roll), sr = sinf(roll);
    return (mat3_t){
        .right = v3_add(v3_scale(r, cr), v3_scale(u, sr)),
        .up    = v3_sub(v3_scale(u, cr), v3_scale(r, sr)),
        .fwd   = f,
    };
}

void look_at_angles(vec3_t eye, vec3_t target, float* yaw, float* pitch) {
    vec3_t const d = v3_sub(target, eye);
    // forward = (sin yaw cos pitch, -sin pitch, cos yaw cos pitch)
    *yaw           = atan2f(d.x, d.z);
    *pitch         = atan2f(-d.y, sqrtf(d.x * d.x + d.z * d.z));
}

// --- Paths ------------------------------------------------------------

// Catmull-Rom on one segment between p1 and p2, at u in [0, 1].
static vec3_t cr_pos(vec3_t p0, vec3_t p1, vec3_t p2, vec3_t p3, float u) {
    float const u2 = u * u, u3 = u2 * u;
    float const a = -0.5f * u3 + u2 - 0.5f * u;
    float const b = 1.5f * u3 - 2.5f * u2 + 1.0f;
    float const c = -1.5f * u3 + 2.0f * u2 + 0.5f * u;
    float const d = 0.5f * u3 - 0.5f * u2;
    return v3(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y,
              a * p0.z + b * p1.z + c * p2.z + d * p3.z);
}

// d/du of cr_pos.
static vec3_t cr_deriv(vec3_t p0, vec3_t p1, vec3_t p2, vec3_t p3, float u) {
    float const u2 = u * u;
    float const a  = -1.5f * u2 + 2.0f * u - 0.5f;
    float const b  = 4.5f * u2 - 5.0f * u;
    float const c  = -4.5f * u2 + 4.0f * u + 0.5f;
    float const d  = 1.5f * u2 - u;
    return v3(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y,
              a * p0.z + b * p1.z + c * p2.z + d * p3.z);
}

// The control points around segment `i`, with the ends mirrored so the
// curve starts and ends on its first and last points.
static void segment(path_t const* p, int i, vec3_t q[4]) {
    int const n = p->n;
    q[1]        = p->pts[i];
    q[2]        = p->pts[i + 1];
    q[0]        = (i > 0) ? p->pts[i - 1] : v3_sub(v3_scale(q[1], 2.0f), q[2]);
    q[3]        = (i + 2 < n) ? p->pts[i + 2] : v3_sub(v3_scale(q[2], 2.0f), q[1]);
}

// Split time t into a segment index and a local u in [0, 1]. `beyond`
// gets how far (in seconds) t lies outside the path, signed.
static void locate(path_t const* p, float t, int* seg, float* u, float* beyond) {
    float const s    = (t - p->t0) / p->dt;
    int const   last = p->n - 2;
    *beyond          = 0.0f;
    if (s <= 0.0f) {
        *seg    = 0;
        *u      = 0.0f;
        *beyond = t - p->t0;
    } else if (s >= (float)(last + 1)) {
        *seg    = last;
        *u      = 1.0f;
        *beyond = t - (p->t0 + (float)(last + 1) * p->dt);
    } else {
        *seg = (int)s;
        *u   = s - (float)*seg;
    }
}

vec3_t path_vel(path_t const* p, float t) {
    int    seg;
    float  u, beyond;
    vec3_t q[4];
    locate(p, t, &seg, &u, &beyond);
    segment(p, seg, q);
    return v3_scale(cr_deriv(q[0], q[1], q[2], q[3], u), 1.0f / p->dt);
}

vec3_t path_pos(path_t const* p, float t) {
    int    seg;
    float  u, beyond;
    vec3_t q[4];
    locate(p, t, &seg, &u, &beyond);
    segment(p, seg, q);
    vec3_t pos = cr_pos(q[0], q[1], q[2], q[3], u);
    if (beyond != 0.0f) {
        // Off either end: continue straight along the end tangent.
        vec3_t const v = v3_scale(cr_deriv(q[0], q[1], q[2], q[3], u), 1.0f / p->dt);
        pos            = v3_add(pos, v3_scale(v, beyond));
    }
    return pos;
}

// --- Deterministic noise ------------------------------------------------

float hash01(int key, unsigned seed) {
    // Integer finaliser (lowbias32); plenty for visual jitter.
    uint32_t x  = (uint32_t)key * 0x9E3779B9u ^ (uint32_t)seed * 0x85EBCA6Bu;
    x          ^= x >> 16;
    x          *= 0x7FEB352Du;
    x          ^= x >> 15;
    x          *= 0x846CA68Bu;
    x          ^= x >> 16;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

float value_noise(float t, float rate, unsigned seed) {
    float const s = t * rate;
    float const k = floorf(s);
    float const f = s - k;
    float const a = hash01((int)k, seed);
    float const b = hash01((int)k + 1, seed);
    return a + (b - a) * f;
}
