// =====================================================================
//  CraftMiner  --  drawing the streamed world (see chunk_render.h)
// ---------------------------------------------------------------------
//  The distance ladder, the frustum test and the fog tint are the
//  showreel's, whose numbers were measured on this hardware (F-03,
//  F-04). What is new here is that the world streams: which chunks
//  exist is decided every frame from where the player is, and each is
//  drawn relative to a moving origin so the floats stay small (D-01).
// =====================================================================

#include "world/chunk_render.h"

#include <math.h>
#include <string.h>

#include "common/texcache.h"
#include "math/camera.h"
#include "math/mesh_render.h"
#include "voxel/voxel_mesh.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"

static struct {
    char const* file;
    uint32_t    argb;  // if the texture will not load
} const MAT_FILES[VM_COUNT] = {
    [VM_GRASS_TOP]     = {"grass_top.png", 0xFF5C9634u},
    [VM_GRASS_SIDE]    = {"grass_side.png", 0xFF7A5A3Au},
    [VM_DIRT]          = {"dirt.png", 0xFF7A563Au},
    [VM_STONE]         = {"stone.png", 0xFF7A7A7Cu},
    [VM_COBBLE]        = {"cobble.png", 0xFF767676u},
    [VM_SAND]          = {"sand.png", 0xFFD6C896u},
    [VM_WATER]         = {"water.png", 0xFF3054C4u},
    [VM_LOG_SIDE]      = {"log_side.png", 0xFF644C2Eu},
    [VM_LOG_TOP]       = {"log_top.png", 0xFFA88452u},
    [VM_PLANKS]        = {"planks.png", 0xFFA4804Eu},
    [VM_LEAVES]        = {"leaves.png", 0xFF3A7026u},
    [VM_COAL]          = {"coal_ore.png", 0xFF606062u},
    [VM_GLASS]         = {"glass.png", 0xFFC8D8DEu},
    [VM_TORCH]         = {"torch.png", 0xFF6E502Cu},
    [VM_FLOWER_RED]    = {"flower_red.png", 0xFFD62824u},
    [VM_FLOWER_YELLOW] = {"flower_yellow.png", 0xFFFAD428u},
    [VM_TALL_GRASS]    = {"tall_grass.png", 0xFF5C9634u},
    [VM_LEAVES_FAST]   = {"leaves_fast.png", 0xFF305C20u},
};

static mesh_mat_t s_tex_mats[VM_COUNT];
static uint32_t   s_mean[VM_COUNT];
static bool       s_ready;
static bool       s_textured = true;
static cm_view_t  s_view;
static int32_t    s_origin_x, s_origin_z;
static int        s_drawn, s_resident, s_missing;

cm_view_t cm_view_preset(int level) {
    switch (level) {
        case 0:
            return (cm_view_t){8.0f, 14.0f, 24.0f, 40.0f, 18.0f, 44.0f, CM_SKY_ARGB, 3, 5};
        case 2:
            return (cm_view_t){12.0f, 20.0f, 40.0f, 72.0f, 30.0f, 78.0f, CM_SKY_ARGB, 6, 8};
        default:
            return (cm_view_t){12.0f, 20.0f, 32.0f, 56.0f, 24.0f, 60.0f, CM_SKY_ARGB, 5, 7};
    }
}

bool chunk_render_init(void) {
    for (int m = 0; m < VM_COUNT; m++) {
        se_texture_t const* tex = texcache_get(MAT_FILES[m].file);
        s_tex_mats[m]           = (mesh_mat_t){tex, MAT_FILES[m].argb, 0};
        s_mean[m]               = tex != NULL ? tex->mean_argb : MAT_FILES[m].argb;
    }
    s_view  = cm_view_preset(1);
    s_ready = true;
    return true;
}

void chunk_render_shutdown(void) {
    s_ready = false;
}

void chunk_render_set_view(cm_view_t const* v) {
    if (v != NULL) s_view = *v;
}
cm_view_t const* chunk_render_view(void) {
    return &s_view;
}
void chunk_render_set_textured(bool on) {
    s_textured = on;
}
bool chunk_render_textured(void) {
    return s_textured;
}

void chunk_render_set_origin(int32_t wx, int32_t wz) {
    s_origin_x = chunk_of(wx) * CH_W;
    s_origin_z = chunk_of(wz) * CH_D;
}
void chunk_render_origin(int32_t* ox, int32_t* oz) {
    if (ox != NULL) *ox = s_origin_x;
    if (oz != NULL) *oz = s_origin_z;
}

void chunk_render_stats(int* chunks_drawn, int* resident, int* missing) {
    if (chunks_drawn != NULL) *chunks_drawn = s_drawn;
    if (resident != NULL) *resident = s_resident;
    if (missing != NULL) *missing = s_missing;
}

// --- Streaming --------------------------------------------------------

void chunk_render_stream(double wx, double wz) {
    if (!s_ready) return;
    int32_t const pcx = chunk_of((int32_t)floor(wx)), pcz = chunk_of((int32_t)floor(wz));

    // Drop what has gone too far. Hysteresis (evict further out than we
    // load) is what guarantees a slot is free before it is wanted, so
    // chunk_claim never has to refuse.
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t* c = chunk_slot_at(i);
        if (c->cstate != CS_READY) continue;
        int32_t const dx = c->cx - pcx, dz = c->cz - pcz;
        int32_t const d  = (dx < 0 ? -dx : dx) > (dz < 0 ? -dz : dz) ? (dx < 0 ? -dx : dx) : (dz < 0 ? -dz : dz);
        if (d <= s_view.evict_radius) continue;

        if ((c->flags & CF_EDITED) != 0) {
            // Save before letting go. If the queue is full it simply
            // stays another frame -- an unsaved chunk is never dropped.
            chunk_worker_request_save(c->cx, c->cz);
            continue;
        }
        for (int l = 0; l < LOD_COUNT; l++) mesh_free(&c->lod[l]);
        c->cstate = CS_FREE;
    }

    // Ask for what is missing, nearest first: a ring at a time outwards,
    // so the ground under the player arrives before the horizon.
    s_resident = 0;
    s_missing  = 0;
    int asked  = 0;
    for (int ring = 0; ring <= s_view.load_radius; ring++) {
        for (int32_t dz = -ring; dz <= ring; dz++) {
            for (int32_t dx = -ring; dx <= ring; dx++) {
                // Only the ring's edge; the inside was done already.
                if (ring > 0 && (dx > -ring && dx < ring && dz > -ring && dz < ring)) continue;
                int32_t const cx = pcx + dx, cz = pcz + dz;
                if (chunk_find(cx, cz) != NULL) {
                    s_resident++;
                    continue;
                }
                s_missing++;
                // A budget per frame, so a long walk cannot flood the
                // queue and starve saves.
                if (asked < 4 && chunk_worker_request_load(cx, cz)) asked++;
            }
        }
    }
}

// --- Drawing ----------------------------------------------------------

static uint32_t mix_argb(uint32_t a, uint32_t b, float f) {
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float const ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        out |= (uint32_t)lroundf(ca + (cb - ca) * f) << s;
    }
    return out;
}

// Whether the box lo..hi is wholly outside the view: all eight corners
// beyond one of its planes. Camera space, using the engine's own
// projection constants, so it tracks the FOV automatically.
static bool outside_view(vec3_t lo, vec3_t hi, vec3_t eye, mat3_t const* b) {
    float const kx     = RENDER_HALF_W / RENDER_FOCAL_LEN;
    float const ku     = RENDER_HORIZON_Y / RENDER_FOCAL_LEN;
    float const kd     = ((float)DISPLAY_LOG_H - RENDER_HORIZON_Y) / RENDER_FOCAL_LEN;
    int         out[5] = {0};
    for (int i = 0; i < 8; i++) {
        vec3_t const p = v3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        vec3_t const d = v3_sub(p, eye);
        float const  x = v3_dot(d, b->right), y = v3_dot(d, b->up), z = v3_dot(d, b->fwd);
        out[0] += z < RENDER_NEAR_CLIP_Z;
        out[1] += x > kx * z;
        out[2] += x < -kx * z;
        out[3] += y > ku * z;
        out[4] += y < -kd * z;
    }
    for (int k = 0; k < 5; k++) {
        if (out[k] == 8) return true;
    }
    return false;
}

void chunk_render_submit(double eye_wx, double eye_wz) {
    if (!s_ready) return;
    s_drawn = 0;

    vec3_t const eye   = camera_eye();
    mat3_t const basis = camera_basis();
    (void)eye_wx;
    (void)eye_wz;

    // The flat palette depends only on how foggy a chunk is, so it is
    // built once per fog step rather than once per chunk -- it was 54
    // lroundf calls per chunk, for a colour the eye cannot tell from
    // its neighbour's.
    #define FOG_STEPS 12
    static mesh_mat_t flat_cache[FOG_STEPS][VM_COUNT];
    bool              flat_built[FOG_STEPS] = {false};

    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t* c = chunk_slot_at(i);
        if (c->cstate != CS_READY && c->cstate != CS_SAVING) continue;

        // Where this chunk sits relative to the render origin. Small
        // numbers, whatever the world coordinates are.
        float const ox = (float)(c->cx * CH_W - s_origin_x);
        float const oz = (float)(c->cz * CH_D - s_origin_z);

        vec3_t const lo = v3(ox, (float)c->bottom, oz);
        vec3_t const hi = v3(ox + CH_W, (float)c->top_max + 1.0f, oz + CH_D);

        // Distance from the eye to the box, zero inside it.
        float const dx = fmaxf(fmaxf(lo.x - eye.x, eye.x - hi.x), 0.0f);
        float const dy = fmaxf(fmaxf(lo.y - eye.y, eye.y - hi.y), 0.0f);
        float const dz = fmaxf(fmaxf(lo.z - eye.z, eye.z - hi.z), 0.0f);
        float const dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist > s_view.draw_dist) continue;
        if (outside_view(lo, hi, eye, &basis)) continue;

        int const lod = dist < s_view.fancy_dist  ? LOD_FANCY
                        : dist < s_view.coarse_dist ? LOD_FAST
                                                    : LOD_COARSE;

        // Mesh it if it is not ready. Until it is, the chunk is simply
        // not drawn -- the fog covers the gap.
        if (c->lod_stale[lod] || c->lod[lod].tn == 0) {
            chunk_worker_request_mesh(c->cx, c->cz, lod);
            if (c->lod_stale[lod]) continue;
        }
        if (c->lod[lod].tn == 0) continue;

        vec3_t const at = v3(ox, 0.0f, oz);

        if (s_textured && dist < s_view.tex_dist) {
            mesh_submit_world(&c->lod[lod], at, s_tex_mats, VM_COUNT);
        } else {
            // Flat, fading into the fog. Three to four times cheaper to
            // fill than textured, which is what makes the far half of
            // the view affordable at all.
            float const f    = fminf(fmaxf((dist - s_view.fog0) / (s_view.fog1 - s_view.fog0), 0.0f), 1.0f);
            int const   step = (int)(f * (FOG_STEPS - 1) + 0.5f);
            if (!flat_built[step]) {
                float const qf = (float)step / (float)(FOG_STEPS - 1);
                for (int m = 0; m < VM_COUNT; m++) {
                    flat_cache[step][m] = (mesh_mat_t){NULL, mix_argb(s_mean[m], s_view.fog_argb, qf), 0};
                }
                flat_built[step] = true;
            }
            mesh_submit_world(&c->lod[lod], at, flat_cache[step], VM_COUNT);
        }
        s_drawn++;
    }
}
