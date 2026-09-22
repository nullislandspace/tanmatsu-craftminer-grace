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
    [VM_BEDROCK]       = {"bedrock.png", 0xFF4A4A4Au},
    [VM_GRAVEL]        = {"gravel.png", 0xFF847C78u},
    [VM_SIGN_0]        = {"sign_kurt.png", 0xFFA4804Eu},
    [VM_SIGN_1]        = {"sign_wolfie.png", 0xFFA4804Eu},
    [VM_SIGN_2]        = {"sign_flob.png", 0xFFA4804Eu},
};

static mesh_mat_t s_tex_mats[VM_COUNT];
static uint32_t   s_mean[VM_COUNT];
static bool       s_ready;
static bool       s_textured = true;
static cm_view_t  s_view;
static int32_t    s_origin_x, s_origin_z;
static int        s_drawn, s_sections, s_resident, s_missing;
static int        s_evicted_total;  // chunks dropped from the resident set, since boot

// The three presets' radii, named so the compiler can check them.
//
// FAR's eviction radius used to be 8. A radius of 8 keeps a 17-wide
// square of chunks, the ring is 16 across, and the slot is the low four
// bits of the coordinate -- so two resident chunks landed on the same
// slot and evicted each other forever, reloading the world while the
// player stood still (F-41). The static assertions below are why that
// cannot come back: raise a radius past what the ring holds and the
// build stops.
#define VIEW_NEAR_LOAD   3
#define VIEW_NEAR_EVICT  5
#define VIEW_MED_LOAD    5
#define VIEW_MED_EVICT   7
#define VIEW_FAR_LOAD    6
#define VIEW_FAR_EVICT   7  // NOT 8: see above

_Static_assert(VIEW_NEAR_EVICT <= CH_EVICT_MAX, "near view evicts beyond what the chunk ring can hold");
_Static_assert(VIEW_MED_EVICT <= CH_EVICT_MAX, "medium view evicts beyond what the chunk ring can hold");
_Static_assert(VIEW_FAR_EVICT <= CH_EVICT_MAX, "far view evicts beyond what the chunk ring can hold");
// Hysteresis: eviction must run further out than loading, or a slot is
// wanted before the chunk using it has let go.
_Static_assert(VIEW_NEAR_LOAD < VIEW_NEAR_EVICT, "near view has no residency hysteresis");
_Static_assert(VIEW_MED_LOAD < VIEW_MED_EVICT, "medium view has no residency hysteresis");
_Static_assert(VIEW_FAR_LOAD < VIEW_FAR_EVICT, "far view has no residency hysteresis");

cm_view_t cm_view_preset(int level) {
    switch (level) {
        case 0:
            return (cm_view_t){8.0f,  14.0f, 24.0f, 40.0f, 18.0f, 44.0f, CM_SKY_ARGB,
                               VIEW_NEAR_LOAD, VIEW_NEAR_EVICT};
        case 2:
            return (cm_view_t){12.0f, 20.0f, 40.0f, 72.0f, 30.0f, 78.0f, CM_SKY_ARGB,
                               VIEW_FAR_LOAD, VIEW_FAR_EVICT};
        default:
            return (cm_view_t){12.0f, 20.0f, 32.0f, 56.0f, 24.0f, 60.0f, CM_SKY_ARGB,
                               VIEW_MED_LOAD, VIEW_MED_EVICT};
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

void chunk_render_block_mats(uint8_t block, mesh_mat_t out[3]) {
    static vox_face_t const FACE[3] = {VF_TOP, VF_SIDE, VF_BOTTOM};
    for (int i = 0; i < 3; i++) {
        int const m = voxel_face_mat(block, FACE[i]);
        // Leaves in the hand take the opaque texture: a cube of cut-out
        // leaves at arm's length is mostly holes.
        out[i] = s_tex_mats[m >= 0 && m != VM_LEAVES ? m : VM_LEAVES_FAST];
    }
}

static uint32_t s_fog_override;

void chunk_render_set_fog(uint32_t argb) {
    s_fog_override = argb;
}

void chunk_render_set_view(cm_view_t const* v) {
    if (v == NULL) return;
    s_view = *v;
    // The ring cannot hold a bigger radius than this, and exceeding it
    // does not degrade -- it makes two chunks share a slot and evict
    // each other for as long as the player stands there (chunk.h,
    // CH_EVICT_MAX). Clamp rather than trust the caller: this is the
    // one place every view setting passes through.
    if (s_view.evict_radius > CH_EVICT_MAX) s_view.evict_radius = CH_EVICT_MAX;
    if (s_view.load_radius >= s_view.evict_radius) s_view.load_radius = s_view.evict_radius - 1;
    if (s_view.load_radius < 1) s_view.load_radius = 1;
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

int chunk_render_evicted(void) {
    return s_evicted_total;
}

void chunk_render_stats(int* chunks_drawn, int* sections_drawn, int* resident, int* missing) {
    if (chunks_drawn != NULL) *chunks_drawn = s_drawn;
    if (sections_drawn != NULL) *sections_drawn = s_sections;
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
        int32_t const d = (dx < 0 ? -dx : dx) > (dz < 0 ? -dz : dz) ? (dx < 0 ? -dx : dx) : (dz < 0 ? -dz : dz);
        if (d <= s_view.evict_radius) continue;

        if ((c->flags & CF_EDITED) != 0) {
            // Save before letting go. If the queue is full it simply
            // stays another frame -- an unsaved chunk is never dropped.
            chunk_worker_request_save(c->cx, c->cz);
            continue;
        }
        for (int m = 0; m < CH_MESH_N; m++) mesh_free(&c->lod[m]);
        c->lod_built    = 0;
        c->lod_stale    = 0;
        c->lod_inflight = 0;
        c->cstate       = CS_FREE;
        s_evicted_total++;
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

// Distance from the eye to the box lo..hi, zero inside it.
static float box_dist(vec3_t lo, vec3_t hi, vec3_t eye) {
    float const dx = fmaxf(fmaxf(lo.x - eye.x, eye.x - hi.x), 0.0f);
    float const dy = fmaxf(fmaxf(lo.y - eye.y, eye.y - hi.y), 0.0f);
    float const dz = fmaxf(fmaxf(lo.z - eye.z, eye.z - hi.z), 0.0f);
    return sqrtf(dx * dx + dy * dy + dz * dz);
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
    s_drawn    = 0;
    s_sections = 0;

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

        // The whole chunk first, as one box: most of the resident set
        // is behind the eye or past the fog, and rejecting it here
        // saves four section tests.
        vec3_t const lo = v3(ox, (float)c->bottom, oz);
        vec3_t const hi = v3(ox + CH_W, (float)c->top_max + 1.0f, oz + CH_D);

        float const dist = box_dist(lo, hi, eye);
        if (dist > s_view.draw_dist) continue;
        if (outside_view(lo, hi, eye, &basis)) continue;

        // THE LEVEL OF DETAIL IS THE CHUNK'S, not the section's. Two
        // stacked sections at different resolutions would not line up
        // where they meet, and the coarse skirt only closes the sides
        // (chunkmesh.c). Sections decide what is DRAWN, not how.
        int const lod = dist < s_view.fancy_dist ? LOD_FANCY : dist < s_view.coarse_dist ? LOD_FAST : LOD_COARSE;

        vec3_t const at  = v3(ox, 0.0f, oz);
        bool         any = false;

        // Now the sections. This is the whole point of D-34: a chunk's
        // underground half is 68% of its triangles (F-33) and none of
        // it is visible from the surface, so the frustum test gets to
        // throw it away -- and, because a section is only meshed when
        // it is about to be drawn, never builds it in the first place.
        for (int sect = 0; sect < CH_SECT_N; sect++) {
            int const ylo = sect * CH_SECT, yhi = ylo + CH_SECT;
            // Below `bottom` is buried stone and above `top_max` is
            // sky: neither can carry a face.
            if (yhi <= (int)c->bottom || ylo >= (int)c->top_max) continue;

            vec3_t const slo = v3(lo.x, (float)(ylo > (int)c->bottom ? ylo : (int)c->bottom), lo.z);
            vec3_t const shi = v3(hi.x, (float)(yhi < (int)c->top_max ? yhi : (int)c->top_max), hi.z);

            float const sdist = box_dist(slo, shi, eye);
            if (sdist > s_view.draw_dist) continue;
            if (outside_view(slo, shi, eye, &basis)) continue;

            // The level this section WANTS. If it is not built yet, ask
            // for it -- and then draw a level that is, rather than
            // nothing.
            //
            // That fallback is the whole difference between a world
            // that streams and one that blinks. A level of detail is a
            // separate mesh, so crossing a distance band asks for a
            // mesh that has never existed; flying upwards moves every
            // chunk into LOD_COARSE at once, which is a hundred and
            // sixty section meshes that do not exist yet. Drawing
            // nothing until they arrive is what "it reloaded the whole
            // world" looks like from the outside. The wrong level for a
            // few frames is not noticeable; a hole in the ground is.
            uint16_t const bit = CH_MESH_BIT(lod, sect);

            // Out of date? Ask for a new one -- but go on drawing the
            // old one meanwhile. Breaking a block marks every level of
            // its section stale, and a chunk that stopped drawing until
            // the worker caught up made the whole area blink.
            if ((c->lod_stale & bit) != 0) chunk_worker_request_mesh(c->cx, c->cz, lod, sect);

            int use = -1;
            if ((c->lod_built & bit) != 0) {
                use = lod;
            } else if ((c->lod_stale & bit) == 0) {
                // Built, not stale, and empty: solid rock or open sky.
                // There is genuinely nothing here.
                continue;
            } else {
                // Never built at this level. Draw the nearest level
                // that HAS been, in detail order -- a step too sharp
                // reads better than a step too blurry. This is what
                // stops flying upwards (every chunk into LOD_COARSE at
                // once) from emptying the world.
                for (int away = 1; away < LOD_COUNT && use < 0; away++) {
                    int const lower = lod - away, higher = lod + away;
                    if (lower >= 0 && (c->lod_built & CH_MESH_BIT(lower, sect)) != 0) use = lower;
                    else if (higher < LOD_COUNT && (c->lod_built & CH_MESH_BIT(higher, sect)) != 0) use = higher;
                }
            }
            if (use < 0) continue;  // nothing built at any level yet; the fog covers it

            mesh_t const* m = chunk_mesh(c, use, sect);
            if (m->tn == 0) continue;

            if (s_textured && sdist < s_view.tex_dist) {
                mesh_submit_world(m, at, s_tex_mats, VM_COUNT);
            } else {
                // Flat, fading into the fog. Three to four times cheaper
                // to fill than textured, which is what makes the far
                // half of the view affordable at all.
                float const f    = fminf(fmaxf((sdist - s_view.fog0) / (s_view.fog1 - s_view.fog0), 0.0f), 1.0f);
                int const   step = (int)(f * (FOG_STEPS - 1) + 0.5f);
                if (!flat_built[step]) {
                    float const qf = (float)step / (float)(FOG_STEPS - 1);
                    for (int mi = 0; mi < VM_COUNT; mi++) {
                        uint32_t const fog = s_fog_override != 0 ? s_fog_override : s_view.fog_argb;
                        flat_cache[step][mi] = (mesh_mat_t){NULL, mix_argb(s_mean[mi], fog, qf), 0};
                    }
                    flat_built[step] = true;
                }
                mesh_submit_world(m, at, flat_cache[step], VM_COUNT);
            }
            s_sections++;
            any = true;
        }
        if (any) s_drawn++;
    }
}
