// =====================================================================
//  Showreel  --  submitting meshes to the scene (see mesh_render.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/mesh_render.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "math/mesh_render.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "math/camera.h"

static char const TAG[] = "mesh";

// World-space copies of the vertices, shared by every mesh_submit() and
// grown to the largest mesh seen.
static vec3_t* s_world;
static int     s_world_cap;

vec3_t const* mesh_last_world(void) {
    return s_world;
}

// Transform vertices [v0, v0 + vn) and submit triangles [t0, t0 + tn),
// which reference only those.
static void submit_range(mesh_t const* m, int v0, int vn, int t0, int tn, xform_t const* x, mesh_mat_t const* mats,
                         int mat_n) {
    if (m == NULL || m->vn == 0) return;
    if (m->vn > s_world_cap) {
        vec3_t* nw = heap_caps_realloc(s_world, (size_t)m->vn * sizeof(vec3_t), MALLOC_CAP_SPIRAM);
        if (nw == NULL) {
            ESP_LOGE(TAG, "no PSRAM for %d world vertices", m->vn);
            return;
        }
        s_world     = nw;
        s_world_cap = m->vn;
    }
    for (int i = v0; i < v0 + vn; i++) s_world[i] = xform_apply(x, m->v[i]);

    vec3_t const eye = camera_eye();
    for (int i = t0; i < t0 + tn; i++) {
        mesh_tri_t const* t = &m->t[i];
        if (t->mat >= mat_n) continue;
        vec3_t const a = s_world[t->a], b = s_world[t->b], c = s_world[t->c];
        if (!tri_faces_point(a, b, c, eye)) continue;
        mesh_mat_t const* mat = &mats[t->mat];
        if (mat->tex != NULL) {
            se_tex_vertex_t const tv[3] = {
                {a.x, a.y, a.z, t->uv[0][0], t->uv[0][1]},
                {b.x, b.y, b.z, t->uv[1][0], t->uv[1][1]},
                {c.x, c.y, c.z, t->uv[2][0], t->uv[2][1]},
            };
            scene_textured_tri(tv, mat->tex, mat->flags);
        } else {
            scene_tri(a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, mat->argb, mat->flags);
        }
    }
}

void mesh_submit(mesh_t const* m, xform_t const* x, mesh_mat_t const* mats, int mat_n) {
    if (m == NULL) return;
    submit_range(m, 0, m->vn, 0, m->tn, x, mats, mat_n);
}

void mesh_submit_part(mesh_t const* m, int part, xform_t const* x, mesh_mat_t const* mats, int mat_n) {
    if (m == NULL || part < 0 || part >= m->pn) return;
    mesh_part_t const* p = &m->parts[part];
    submit_range(m, p->v0, p->vn, p->t0, p->tn, x, mats, mat_n);
}

// --- Chunk meshes: already world-space, placed by an offset ------------
//
// See mesh_render.h for why this exists rather than reusing
// submit_range() with an identity transform.

static int s_tested, s_passed;

void mesh_submit_counters(int* tested, int* passed) {
    if (tested != NULL) *tested = s_tested;
    if (passed != NULL) *passed = s_passed;
}
void mesh_submit_counters_reset(void) {
    s_tested = 0;
    s_passed = 0;
}

void mesh_submit_world(mesh_t const* m, vec3_t origin_rel, mesh_mat_t const* mats, int mat_n) {
    if (m == NULL || m->vn == 0 || m->tn == 0) return;

    vec3_t const eye = camera_eye();
    // The eye in the mesh's own coordinates, so the cull compares two
    // numbers that are both small: subtracting the offset once here
    // beats adding it to every vertex.
    float const eye_axis[3] = {eye.x - origin_rel.x, eye.y - origin_rel.y, eye.z - origin_rel.z};

    s_tested += m->tn;
    for (int i = 0; i < m->tn; i++) {
        mesh_tri_t const* t = &m->t[i];
        if (t->mat >= mat_n) continue;

        // Most triangles in a chunk are facing away, so the cull runs
        // first and reads as little as it can: one vertex, and for an
        // axis-aligned face one coordinate of it. Fetching all three
        // vertices up front was costing nine PSRAM floats per triangle
        // to throw most of them away.
        vec3_t const va = m->v[t->a];

        if (t->dir != MESH_DIR_NONE) {
            // An axis-aligned face: every vertex shares the plane, so
            // one of them tells us where it is. Visible only from the
            // side its normal points at.
            int const   axis  = t->dir >> 1;
            float const plane = axis == 0 ? va.x : axis == 1 ? va.y : va.z;
            float const delta = eye_axis[axis] - plane;
            if ((t->dir & 1) ? delta >= 0.0f : delta <= 0.0f) continue;
        } else {
            vec3_t const vb = m->v[t->b], vc = m->v[t->c];
            vec3_t const wa = {va.x + origin_rel.x, va.y + origin_rel.y, va.z + origin_rel.z};
            vec3_t const wb = {vb.x + origin_rel.x, vb.y + origin_rel.y, vb.z + origin_rel.z};
            vec3_t const wc = {vc.x + origin_rel.x, vc.y + origin_rel.y, vc.z + origin_rel.z};
            if (!tri_faces_point(wa, wb, wc, eye)) continue;
        }

        vec3_t const vb = m->v[t->b], vc = m->v[t->c];
        float const  ax = va.x + origin_rel.x, ay = va.y + origin_rel.y, az = va.z + origin_rel.z;
        float const  bx = vb.x + origin_rel.x, by = vb.y + origin_rel.y, bz = vb.z + origin_rel.z;
        float const  cx = vc.x + origin_rel.x, cy = vc.y + origin_rel.y, cz = vc.z + origin_rel.z;

        s_passed++;
        mesh_mat_t const* mat = &mats[t->mat];
        if (mat->tex != NULL) {
            se_tex_vertex_t const tv[3] = {
                {ax, ay, az, t->uv[0][0], t->uv[0][1]},
                {bx, by, bz, t->uv[1][0], t->uv[1][1]},
                {cx, cy, cz, t->uv[2][0], t->uv[2][1]},
            };
            scene_textured_tri(tv, mat->tex, mat->flags);
        } else {
            scene_tri(ax, ay, az, bx, by, bz, cx, cy, cz, mat->argb, mat->flags);
        }
    }
}
