// =====================================================================
//  Showreel  --  static meshes and procedural builders (see mesh.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/mesh.c. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "math/mesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "common/psram.h"
// Mesh data is read once per frame, sequentially: PSRAM on the badge,
// plain malloc on the host (common/psram.h).
#define MESH_REALLOC(p, n) sm_realloc((p), (n))
#define MESH_FREE(p)       sm_free(p)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mesh_set_dir(mesh_t* m, uint8_t dir) {
    if (m != NULL) m->dir = dir;
}

void mesh_set_light(mesh_t* m, uint8_t light) {
    if (m != NULL) m->light = light;
}

void mesh_init(mesh_t* m) {
    memset(m, 0, sizeof(*m));
    m->dir = MESH_DIR_NONE;  // only the mesher knows better
    m->light = MESH_LIGHT_FULL;
}

void mesh_free(mesh_t* m) {
    MESH_FREE(m->v);
    MESH_FREE(m->t);
    MESH_FREE(m->parts);
    mesh_init(m);
}

// Record the solid a builder just made: vertices from v0 and triangles
// from t0 to the end (a builder only references its own vertices).
static void part_add(mesh_t* m, int v0, int t0) {
    if (m->failed || m->tn == t0) return;
    if (m->pn == m->pcap) {
        int const    cap = m->pcap ? m->pcap * 2 : 8;
        mesh_part_t* np  = MESH_REALLOC(m->parts, (size_t)cap * sizeof(mesh_part_t));
        if (np == NULL) {
            m->failed = true;
            return;
        }
        m->parts = np;
        m->pcap  = cap;
    }
    m->parts[m->pn++] = (mesh_part_t){v0, m->vn - v0, t0, m->tn - t0};
}

vec3_t mesh_part_centre(mesh_t const* m, int part) {
    mesh_part_t const* p   = &m->parts[part];
    vec3_t             sum = v3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < p->vn; i++) sum = v3_add(sum, m->v[p->v0 + i]);
    return p->vn ? v3_scale(sum, 1.0f / (float)p->vn) : sum;
}

int mesh_vert(mesh_t* m, vec3_t p) {
    if (m->failed) return -1;
    if (m->vn >= 65535) {
        m->failed = true;
        return -1;
    }
    if (m->vn == m->vcap) {
        int const cap = m->vcap ? m->vcap * 2 : 64;
        void*     nv  = MESH_REALLOC(m->v, (size_t)cap * sizeof(vec3_t));
        if (nv == NULL) {
            m->failed = true;
            return -1;
        }
        m->v    = nv;
        m->vcap = cap;
    }
    m->v[m->vn] = p;
    return m->vn++;
}

void mesh_tri(mesh_t* m, int a, int b, int c, uint8_t mat, float const uv[3][2]) {
    if (m->failed || a < 0 || b < 0 || c < 0) return;
    if (m->tn == m->tcap) {
        int const cap = m->tcap ? m->tcap * 2 : 64;
        void*     nt  = MESH_REALLOC(m->t, (size_t)cap * sizeof(mesh_tri_t));
        if (nt == NULL) {
            m->failed = true;
            return;
        }
        m->t    = nt;
        m->tcap = cap;
    }
    mesh_tri_t* t = &m->t[m->tn++];
    t->a          = (uint16_t)a;
    t->b          = (uint16_t)b;
    t->c          = (uint16_t)c;
    t->mat        = mat;
    t->dir        = m->dir;
    t->light      = m->light;
    memcpy(t->uv, uv, sizeof(t->uv));
}

void mesh_quad(mesh_t* m, int a, int b, int c, int d, uint8_t mat, float const uv[4][2]) {
    float const t0[3][2] = {{uv[0][0], uv[0][1]}, {uv[1][0], uv[1][1]}, {uv[2][0], uv[2][1]}};
    float const t1[3][2] = {{uv[0][0], uv[0][1]}, {uv[2][0], uv[2][1]}, {uv[3][0], uv[3][1]}};
    mesh_tri(m, a, b, c, mat, t0);
    mesh_tri(m, a, c, d, mat, t1);
}

void mesh_transform_from(mesh_t* m, int first, xform_t const* x) {
    for (int i = first; i < m->vn; i++) m->v[i] = xform_apply(x, m->v[i]);
}

float mesh_signed_volume(mesh_t const* m, int first_tri) {
    double vol = 0.0;
    for (int i = first_tri; i < m->tn; i++) {
        vec3_t const a = m->v[m->t[i].a], b = m->v[m->t[i].b], c = m->v[m->t[i].c];
        vol += (double)v3_dot(a, v3_cross(b, c));
    }
    return (float)(vol / 6.0);
}

// --- Orientation-safe emitters ----------------------------------------
//
// The builders say which way a face should point; these check the
// winding against that and flip it if needed, so a builder cannot get a
// face backwards by listing its corners in the wrong order.

static bool faces(mesh_t const* m, int a, int b, int c, vec3_t out) {
    vec3_t const n = v3_cross(v3_sub(m->v[b], m->v[a]), v3_sub(m->v[c], m->v[a]));
    return v3_dot(n, out) >= 0.0f;
}

static void quad_out(mesh_t* m, int a, int b, int c, int d, uint8_t mat, float const uv[4][2], vec3_t out) {
    if (m->failed) return;
    if (faces(m, a, b, c, out)) {
        mesh_quad(m, a, b, c, d, mat, uv);
    } else {
        float const r[4][2] = {{uv[0][0], uv[0][1]}, {uv[3][0], uv[3][1]}, {uv[2][0], uv[2][1]}, {uv[1][0], uv[1][1]}};
        mesh_quad(m, a, d, c, b, mat, r);
    }
}

static void tri_out(mesh_t* m, int a, int b, int c, uint8_t mat, float const uv[3][2], vec3_t out) {
    if (m->failed) return;
    if (faces(m, a, b, c, out)) {
        mesh_tri(m, a, b, c, mat, uv);
    } else {
        float const r[3][2] = {{uv[0][0], uv[0][1]}, {uv[2][0], uv[2][1]}, {uv[1][0], uv[1][1]}};
        mesh_tri(m, a, c, b, mat, r);
    }
}

// Planar texture coordinates for a point on a face pointing `out`: the
// plate lies flat on whichever axis plane the face is most square to.
static void planar_uv(vec3_t p, vec3_t out, float rep, float uv[2]) {
    float const ax = fabsf(out.x), ay = fabsf(out.y), az = fabsf(out.z);
    if (ay >= ax && ay >= az) {
        uv[0] = p.x / rep;
        uv[1] = p.z / rep;
    } else if (ax >= az) {
        uv[0] = p.z / rep;
        uv[1] = -p.y / rep;
    } else {
        uv[0] = p.x / rep;
        uv[1] = -p.y / rep;
    }
}

static void quad_planar(mesh_t* m, int a, int b, int c, int d, uint8_t mat, float rep, vec3_t out) {
    if (m->failed) return;
    float uv[4][2];
    planar_uv(m->v[a], out, rep, uv[0]);
    planar_uv(m->v[b], out, rep, uv[1]);
    planar_uv(m->v[c], out, rep, uv[2]);
    planar_uv(m->v[d], out, rep, uv[3]);
    quad_out(m, a, b, c, d, mat, uv, out);
}

static void tri_planar(mesh_t* m, int a, int b, int c, uint8_t mat, float rep, vec3_t out) {
    if (m->failed) return;
    float uv[3][2];
    planar_uv(m->v[a], out, rep, uv[0]);
    planar_uv(m->v[b], out, rep, uv[1]);
    planar_uv(m->v[c], out, rep, uv[2]);
    tri_out(m, a, b, c, mat, uv, out);
}

// --- Builders ---------------------------------------------------------

void mesh_box(mesh_t* m, vec3_t lo, vec3_t hi, uint8_t mat, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    // Corner i: bit 0 = x at hi, bit 1 = y at hi, bit 2 = z at hi.
    int       v[8];
    for (int i = 0; i < 8; i++) {
        v[i] = mesh_vert(m, v3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z));
    }
    quad_planar(m, v[0], v[2], v[6], v[4], mat, rep, v3(-1, 0, 0));  // -x
    quad_planar(m, v[1], v[3], v[7], v[5], mat, rep, v3(1, 0, 0));   // +x
    quad_planar(m, v[0], v[1], v[5], v[4], mat, rep, v3(0, -1, 0));  // -y
    quad_planar(m, v[2], v[3], v[7], v[6], mat, rep, v3(0, 1, 0));   // +y
    quad_planar(m, v[0], v[1], v[3], v[2], mat, rep, v3(0, 0, -1));  // -z
    quad_planar(m, v[4], v[5], v[7], v[6], mat, rep, v3(0, 0, 1));   // +z
    part_add(m, part_v0, part_t0);
}

// Rim vertex k of a circle radius r at height z (k wraps).
static vec3_t rim(float r, float z, int k, int sides) {
    float const a = (float)k * (2.0f * (float)M_PI / (float)sides);
    return v3(r * cosf(a), r * sinf(a), z);
}

static vec3_t radial(int k, float half, int sides) {
    float const a = ((float)k + half) * (2.0f * (float)M_PI / (float)sides);
    return v3(cosf(a), sinf(a), 0.0f);
}

void mesh_cylinder(mesh_t* m, float r, float z0, float z1, int sides, bool cap0, bool cap1, uint8_t mat_side,
                   uint8_t mat_cap, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    int const base = m->vn;
    for (int k = 0; k < sides; k++) {
        mesh_vert(m, rim(r, z0, k, sides));
        mesh_vert(m, rim(r, z1, k, sides));
    }
    float const arc = 2.0f * (float)M_PI * r / (float)sides;
    for (int k = 0; k < sides; k++) {
        int const   n  = (k + 1) % sides;
        int const   b0 = base + 2 * k, t0 = b0 + 1, b1 = base + 2 * n, t1 = b1 + 1;
        float const u0 = (float)k * arc / rep, u1 = (float)(k + 1) * arc / rep;
        float const uv[4][2] = {{u0, z0 / rep}, {u1, z0 / rep}, {u1, z1 / rep}, {u0, z1 / rep}};
        quad_out(m, b0, b1, t1, t0, mat_side, uv, radial(k, 0.5f, sides));
    }
    if (cap0) {
        int const c = mesh_vert(m, v3(0.0f, 0.0f, z0));
        for (int k = 0; k < sides; k++) {
            tri_planar(m, c, base + 2 * k, base + 2 * ((k + 1) % sides), mat_cap, rep, v3(0, 0, -1));
        }
    }
    if (cap1) {
        int const c = mesh_vert(m, v3(0.0f, 0.0f, z1));
        for (int k = 0; k < sides; k++) {
            tri_planar(m, c, base + 2 * k + 1, base + 2 * ((k + 1) % sides) + 1, mat_cap, rep, v3(0, 0, 1));
        }
    }
    part_add(m, part_v0, part_t0);
}

void mesh_ring(mesh_t* m, float r_in, float r_out, float z0, float z1, int segs, uint8_t mat_outer, uint8_t mat_inner,
               uint8_t mat_face, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    // Per segment angle: inner-z0, inner-z1, outer-z0, outer-z1.
    int const base = m->vn;
    for (int k = 0; k < segs; k++) {
        mesh_vert(m, rim(r_in, z0, k, segs));
        mesh_vert(m, rim(r_in, z1, k, segs));
        mesh_vert(m, rim(r_out, z0, k, segs));
        mesh_vert(m, rim(r_out, z1, k, segs));
    }
    float const step  = 2.0f * (float)M_PI / (float)segs;
    float const r_mid = 0.5f * (r_in + r_out);
    for (int k = 0; k < segs; k++) {
        int const    n   = (k + 1) % segs;
        int const    i0b = base + 4 * k, i0t = i0b + 1, o0b = i0b + 2, o0t = i0b + 3;
        int const    i1b = base + 4 * n, i1t = i1b + 1, o1b = i1b + 2, o1t = i1b + 3;
        vec3_t const rad = radial(k, 0.5f, segs);

        // Outer and inner walls: u runs round the ring, v across it.
        float const uo0 = (float)k * step * r_out / rep, uo1 = (float)(k + 1) * step * r_out / rep;
        float const uvo[4][2] = {{uo0, z0 / rep}, {uo1, z0 / rep}, {uo1, z1 / rep}, {uo0, z1 / rep}};
        quad_out(m, o0b, o1b, o1t, o0t, mat_outer, uvo, rad);
        float const ui0 = (float)k * step * r_in / rep, ui1 = (float)(k + 1) * step * r_in / rep;
        float const uvi[4][2] = {{ui0, z0 / rep}, {ui1, z0 / rep}, {ui1, z1 / rep}, {ui0, z1 / rep}};
        quad_out(m, i0b, i1b, i1t, i0t, mat_inner, uvi, v3_scale(rad, -1.0f));

        // Flat faces: u round the ring at mid radius, v outward.
        float const uf0 = (float)k * step * r_mid / rep, uf1 = (float)(k + 1) * step * r_mid / rep;
        float const uvf[4][2] = {{uf0, r_in / rep}, {uf0, r_out / rep}, {uf1, r_out / rep}, {uf1, r_in / rep}};
        quad_out(m, i0b, o0b, o1b, i1b, mat_face, uvf, v3(0, 0, -1));
        quad_out(m, i0t, o0t, o1t, i1t, mat_face, uvf, v3(0, 0, 1));
    }
    part_add(m, part_v0, part_t0);
}

void mesh_loft(mesh_t* m, int n_sec, int n_pts, float const z[], float const (*xy)[MESH_LOFT_MAX_PTS][2],
               uint8_t mat_side, uint8_t mat_cap, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    if (n_sec < 2 || n_pts < 3 || n_pts > MESH_LOFT_MAX_PTS) {
        m->failed = true;
        return;
    }
    int const base = m->vn;
    for (int k = 0; k < n_sec; k++) {
        for (int i = 0; i < n_pts; i++) mesh_vert(m, v3(xy[k][i][0], xy[k][i][1], z[k]));
    }
    // Section centroids: the outward direction of a side face is away
    // from the axis through them.
    vec3_t cen[2];
    for (int k = 0; k + 1 < n_sec; k++) {
        for (int e = 0; e < 2; e++) {
            float sx = 0.0f, sy = 0.0f;
            for (int i = 0; i < n_pts; i++) {
                sx += xy[k + e][i][0];
                sy += xy[k + e][i][1];
            }
            cen[e] = v3(sx / (float)n_pts, sy / (float)n_pts, z[k + e]);
        }
        for (int i = 0; i < n_pts; i++) {
            int const    n = (i + 1) % n_pts;
            int const    a = base + k * n_pts + i, b = base + k * n_pts + n;
            int const    c = base + (k + 1) * n_pts + n, d = base + (k + 1) * n_pts + i;
            // Outward: from the axis to the middle of the face.
            vec3_t const mid = v3_scale(v3_add(v3_add(m->v[a], m->v[b]), v3_add(m->v[c], m->v[d])), 0.25f);
            vec3_t const ax  = v3_lerp(cen[0], cen[1], 0.5f);
            vec3_t       out = v3_sub(mid, ax);
            out.z            = 0.0f;
            // u: running length round the section; v: z.
            float const u0 = (float)i / (float)n_pts * 4.0f, u1 = (float)(i + 1) / (float)n_pts * 4.0f;
            float const uv[4][2] = {{u0, z[k] / rep}, {u1, z[k] / rep}, {u1, z[k + 1] / rep}, {u0, z[k + 1] / rep}};
            quad_out(m, a, b, c, d, mat_side, uv, out);
        }
    }
    // Caps, as fans from each end section's first point (sections are
    // convex).
    for (int e = 0; e < 2; e++) {
        int const    k   = e ? n_sec - 1 : 0;
        vec3_t const out = v3(0.0f, 0.0f, e ? 1.0f : -1.0f);
        for (int i = 1; i + 1 < n_pts; i++) {
            tri_planar(m, base + k * n_pts, base + k * n_pts + i, base + k * n_pts + i + 1, mat_cap, rep, out);
        }
    }
    part_add(m, part_v0, part_t0);
}

void mesh_stroke(mesh_t* m, int n, float const (*pts)[2], bool closed, float width, float z0, float z1,
                 uint8_t mat_face, uint8_t mat_side, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    if (n < 2 || n > MESH_STROKE_MAX_PTS || (closed && n < 3)) {
        m->failed = true;
        return;
    }
    int const   segs = closed ? n : n - 1;
    float const half = 0.5f * width;

    // Left normal (+90 degrees) of segment k, from point k to point k+1.
    float nx[MESH_STROKE_MAX_PTS], ny[MESH_STROKE_MAX_PTS], len[MESH_STROKE_MAX_PTS];
    for (int k = 0; k < segs; k++) {
        int const   j  = (k + 1) % n;
        float const dx = pts[j][0] - pts[k][0], dy = pts[j][1] - pts[k][1];
        len[k]        = sqrtf(dx * dx + dy * dy);
        float const l = len[k] > 1e-9f ? len[k] : 1.0f;
        nx[k]         = -dy / l;
        ny[k]         = dx / l;
    }

    // Per point: its left and right offset, at z0 and z1. At a joint the
    // offset is the mitre (the bisector, lengthened so both sides stay
    // `half` from their segment); at an open end, straight across.
    int const base = m->vn;
    for (int i = 0; i < n; i++) {
        int const in  = closed ? (i + n - 1) % n : i - 1;   // segment arriving at i
        int const out = closed ? i : (i < n - 1 ? i : -1);  // segment leaving i
        float     ox, oy;
        if (in >= 0 && out >= 0) {
            float       bx = nx[in] + nx[out], by = ny[in] + ny[out];
            float const bl  = sqrtf(bx * bx + by * by);
            bx             /= bl;
            by             /= bl;
            float const s   = half / (bx * nx[out] + by * ny[out]);
            ox              = bx * s;
            oy              = by * s;
        } else {
            int const k = in >= 0 ? in : out;
            ox          = nx[k] * half;
            oy          = ny[k] * half;
        }
        float const x = pts[i][0], y = pts[i][1];
        mesh_vert(m, v3(x + ox, y + oy, z0));  // base + 4i + 0: left, z0
        mesh_vert(m, v3(x - ox, y - oy, z0));  //            + 1: right, z0
        mesh_vert(m, v3(x + ox, y + oy, z1));  //            + 2: left, z1
        mesh_vert(m, v3(x - ox, y - oy, z1));  //            + 3: right, z1
    }

    float s0 = 0.0f;  // path length so far, for the sides' u
    for (int k = 0; k < segs; k++) {
        int const    a = base + 4 * k, b = base + 4 * ((k + 1) % n);
        vec3_t const left = v3(nx[k], ny[k], 0.0f), right = v3(-nx[k], -ny[k], 0.0f);
        quad_planar(m, a + 0, b + 0, b + 1, a + 1, mat_face, rep, v3(0.0f, 0.0f, -1.0f));
        quad_planar(m, a + 2, b + 2, b + 3, a + 3, mat_face, rep, v3(0.0f, 0.0f, 1.0f));
        float const u0 = s0 / rep, u1 = (s0 + len[k]) / rep;
        float const sl[4][2] = {{u0, z0 / rep}, {u1, z0 / rep}, {u1, z1 / rep}, {u0, z1 / rep}};
        quad_out(m, a + 0, b + 0, b + 2, a + 2, mat_side, sl, left);
        quad_out(m, a + 1, b + 1, b + 3, a + 3, mat_side, sl, right);
        s0 += len[k];
    }
    if (!closed) {
        // End caps, facing back along the first segment and on along the
        // last.
        int const    a = base, b = base + 4 * (n - 1);
        // The direction of travel is (ny, -nx) (the left normal turned back).
        vec3_t const back = v3(-ny[0], nx[0], 0.0f), fwd = v3(ny[segs - 1], -nx[segs - 1], 0.0f);
        float const  cu        = width / rep;
        float const  cap[4][2] = {{0.0f, z0 / rep}, {cu, z0 / rep}, {cu, z1 / rep}, {0.0f, z1 / rep}};
        quad_out(m, a + 0, a + 1, a + 3, a + 2, mat_side, cap, back);
        quad_out(m, b + 0, b + 1, b + 3, b + 2, mat_side, cap, fwd);
    }
    part_add(m, part_v0, part_t0);
}

void mesh_sphere(mesh_t* m, float r, int segs, int rings, uint8_t mat) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    if (segs < 3 || rings < 2) {
        m->failed = true;
        return;
    }
    // Poles, then the (rings - 1) latitude circles of `segs` points.
    int const north = mesh_vert(m, v3(0.0f, r, 0.0f));
    int const south = mesh_vert(m, v3(0.0f, -r, 0.0f));
    int const base  = m->vn;
    for (int j = 1; j < rings; j++) {
        float const lat = (float)M_PI * (float)j / (float)rings;  // from the north pole
        for (int k = 0; k < segs; k++) {
            float const lon = 2.0f * (float)M_PI * (float)k / (float)segs;
            mesh_vert(m, v3(r * sinf(lat) * sinf(lon), r * cosf(lat), r * sinf(lat) * cosf(lon)));
        }
    }
    // u per corner from the segment index, so the seam at u = 1 needs no
    // duplicate vertex: the last segment runs to u = 1, not back to 0.
    for (int k = 0; k < segs; k++) {
        int const   n  = (k + 1) % segs;
        float const u0 = (float)k / (float)segs, u1 = (float)(k + 1) / (float)segs, um = 0.5f * (u0 + u1);
        float const v1 = 1.0f / (float)rings, vl = (float)(rings - 1) / (float)rings;
        int const   a = base + k, b = base + n;
        float const uvn[3][2] = {{um, 0.0f}, {u0, v1}, {u1, v1}};
        tri_out(m, north, a, b, mat, uvn, v3_add(v3_add(m->v[north], m->v[a]), m->v[b]));
        int const   la = base + (rings - 2) * segs + k, lb = base + (rings - 2) * segs + n;
        float const uvs[3][2] = {{um, 1.0f}, {u0, vl}, {u1, vl}};
        tri_out(m, south, la, lb, mat, uvs, v3_add(v3_add(m->v[south], m->v[la]), m->v[lb]));
        for (int j = 0; j + 2 < rings; j++) {
            int const   p = base + j * segs + k, q = base + j * segs + n;
            int const   s = base + (j + 1) * segs + n, t = base + (j + 1) * segs + k;
            float const va = (float)(j + 1) / (float)rings, vb = (float)(j + 2) / (float)rings;
            float const uv[4][2] = {{u0, va}, {u1, va}, {u1, vb}, {u0, vb}};
            quad_out(m, p, q, s, t, mat, uv, v3_add(v3_add(m->v[p], m->v[q]), v3_add(m->v[s], m->v[t])));
        }
    }
    part_add(m, part_v0, part_t0);
}

// Index of the vertex half way along edge (a, b), made once and shared by
// both triangles on the edge (a linear search: blobs are built once, at
// startup, and have a few hundred edges).
typedef struct {
    int a, b, mid;
} blob_edge_t;

static int blob_mid(mesh_t* m, blob_edge_t* e, int* en, int a, int b) {
    if (a > b) {
        int const t = a;
        a           = b;
        b           = t;
    }
    for (int i = 0; i < *en; i++) {
        if (e[i].a == a && e[i].b == b) return e[i].mid;
    }
    int const mid = mesh_vert(m, v3_norm(v3_add(m->v[a], m->v[b])));
    e[(*en)++]    = (blob_edge_t){a, b, mid};
    return mid;
}

#define BLOB_MAX_SUBDIV 3

void mesh_blob(mesh_t* m, int subdiv, float (*radius)(vec3_t dir, void* user), void* user, uint8_t mat, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    if (subdiv < 0 || subdiv > BLOB_MAX_SUBDIV) {
        m->failed = true;
        return;
    }
    // The icosahedron on the unit sphere.
    float const      p             = 1.6180340f;  // golden ratio
    float const      ico[12][3]    = {{-1, p, 0},  {1, p, 0},  {-1, -p, 0}, {1, -p, 0}, {0, -1, p},  {0, 1, p},
                                      {0, -1, -p}, {0, 1, -p}, {p, 0, -1},  {p, 0, 1},  {-p, 0, -1}, {-p, 0, 1}};
    static int const faces0[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                      {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                      {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                      {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    int const        base          = m->vn;
    for (int i = 0; i < 12; i++) mesh_vert(m, v3_norm(v3(ico[i][0], ico[i][1], ico[i][2])));

    // Subdivide on the unit sphere: every triangle into four.
    int cap = 20;
    for (int s = 0; s < subdiv; s++) cap *= 4;
    int*         tri   = malloc(sizeof(int) * 3 * (size_t)cap);
    int*         next  = malloc(sizeof(int) * 3 * (size_t)cap);
    blob_edge_t* edges = malloc(sizeof(blob_edge_t) * (size_t)cap * 3 / 2 + 64);
    if (tri == NULL || next == NULL || edges == NULL) {
        free(tri);
        free(next);
        free(edges);
        m->failed = true;
        return;
    }
    int tn = 20;
    for (int i = 0; i < 20; i++) {
        for (int k = 0; k < 3; k++) tri[i * 3 + k] = base + faces0[i][k];
    }
    for (int s = 0; s < subdiv; s++) {
        int en = 0, nn = 0;
        for (int i = 0; i < tn; i++) {
            int const a = tri[i * 3], b = tri[i * 3 + 1], c = tri[i * 3 + 2];
            int const ab = blob_mid(m, edges, &en, a, b), bc = blob_mid(m, edges, &en, b, c);
            int const ca      = blob_mid(m, edges, &en, c, a);
            int const q[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
            for (int k = 0; k < 4; k++) {
                next[nn * 3] = q[k][0], next[nn * 3 + 1] = q[k][1], next[nn * 3 + 2] = q[k][2];
                nn++;
            }
        }
        int* t = tri;
        tri    = next;
        next   = t;
        tn     = nn;
        if (m->failed) break;  // out of memory: mesh_vert returned -1
    }
    if (m->failed) {
        free(tri);
        free(next);
        free(edges);
        return;
    }
    // Push every vertex out to its radius, then emit, facing out.
    for (int i = base; i < m->vn; i++) m->v[i] = v3_scale(m->v[i], radius(m->v[i], user));
    for (int i = 0; i < tn; i++) {
        int const a = tri[i * 3], b = tri[i * 3 + 1], c = tri[i * 3 + 2];
        tri_planar(m, a, b, c, mat, rep, v3_add(v3_add(m->v[a], m->v[b]), m->v[c]));
    }
    free(tri);
    free(next);
    free(edges);
    part_add(m, part_v0, part_t0);
}

void mesh_cone(mesh_t* m, float r, float z0, float z1, int sides, uint8_t mat_side, uint8_t mat_base, float rep) {
    int const part_v0 = m->vn, part_t0 = m->tn;
    int const base = m->vn;
    for (int k = 0; k < sides; k++) mesh_vert(m, rim(r, z0, k, sides));
    int const   apex = mesh_vert(m, v3(0.0f, 0.0f, z1));
    float const arc  = 2.0f * (float)M_PI * r / (float)sides;
    float const len  = fabsf(z1 - z0);
    for (int k = 0; k < sides; k++) {
        int const   n  = (k + 1) % sides;
        float const u0 = (float)k * arc / rep, u1 = (float)(k + 1) * arc / rep;
        float const uv[3][2] = {{u0, 0.0f}, {u1, 0.0f}, {0.5f * (u0 + u1), len / rep}};
        tri_out(m, base + k, base + n, apex, mat_side, uv, radial(k, 0.5f, sides));
    }
    int const c = mesh_vert(m, v3(0.0f, 0.0f, z0));
    for (int k = 0; k < sides; k++) {
        tri_planar(m, c, base + k, base + (k + 1) % sides, mat_base, rep, v3(0, 0, z0 < z1 ? -1.0f : 1.0f));
    }
    part_add(m, part_v0, part_t0);
}

bool mesh_raycast(mesh_t const* m, xform_t const* x, vec3_t from, vec3_t dir, float max, float* dist) {
    if (m == NULL || m->tn == 0 || x->scale <= 0.0f) return false;
    // Into model space: R^T (p - pos) / scale. R is orthonormal, so the
    // direction stays normalised and model distances are world / scale.
    vec3_t const rel = v3_sub(from, x->pos);
    vec3_t const o =
        v3_scale(v3(v3_dot(rel, x->r.right), v3_dot(rel, x->r.up), v3_dot(rel, x->r.fwd)), 1.0f / x->scale);
    vec3_t const d    = v3(v3_dot(dir, x->r.right), v3_dot(dir, x->r.up), v3_dot(dir, x->r.fwd));
    float        best = max / x->scale;
    bool         hit  = false;
    for (int i = 0; i < m->tn; i++) {
        // Moller-Trumbore.
        vec3_t const p  = m->v[m->t[i].a];
        vec3_t const e1 = v3_sub(m->v[m->t[i].b], p), e2 = v3_sub(m->v[m->t[i].c], p);
        vec3_t const h   = v3_cross(d, e2);
        float const  det = v3_dot(e1, h);
        if (fabsf(det) < 1e-9f) continue;
        float const  inv = 1.0f / det;
        vec3_t const s   = v3_sub(o, p);
        float const  u   = inv * v3_dot(s, h);
        if (u < 0.0f || u > 1.0f) continue;
        vec3_t const q = v3_cross(s, e1);
        float const  v = inv * v3_dot(d, q);
        if (v < 0.0f || u + v > 1.0f) continue;
        float const f = inv * v3_dot(e2, q);
        if (f > 0.0f && f < best) {
            best = f;
            hit  = true;
        }
    }
    if (hit && dist) *dist = best * x->scale;
    return hit;
}
