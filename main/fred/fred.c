// =====================================================================
//  SynthMiner  --  Fred (see fred.h)
//  Ported from tanmatsu-showreel-grace, main/craftminer/assets/miner.c.
//  Changes here are SynthMiner's; the showreel stays the origin.
// =====================================================================

#include "fred/fred.h"

#include <math.h>
#include <stddef.h>

#include "common/texcache.h"
#include "fred/fred_mesh.h"
#include "items/items.h"
#include "math/camera.h"
#include "math/mesh_render.h"
#include "voxel/voxel_mesh.h"
#include "world/chunk_render.h"

static bool       s_ready;
static mesh_t     s_head, s_body, s_arm, s_fp_arm, s_leg, s_pick, s_axe, s_shovel, s_cube, s_sprite, s_torch;
static mesh_mat_t s_mats[FM_COUNT];

// The head of a tool, by level.
static uint32_t const HEAD_ARGB[4] = {0xFF9AA0A8u, 0xFF8A6238u, 0xFF8C9096u, 0xFFD2D6DCu};

void fred_init(void) {
    if (s_ready) return;
    s_mats[FM_SKIN]     = (mesh_mat_t){NULL, 0xFFDEAA80u, 0};
    s_mats[FM_SHIRT]    = (mesh_mat_t){NULL, 0xFFB8302Au, 0};
    s_mats[FM_OVERALLS] = (mesh_mat_t){NULL, 0xFF2E4C8Cu, 0};
    s_mats[FM_BOOTS]    = (mesh_mat_t){NULL, 0xFF4A3020u, 0};
    s_mats[FM_HAT]      = (mesh_mat_t){NULL, 0xFFF0C020u, 0};
    s_mats[FM_LAMP]     = (mesh_mat_t){NULL, 0xFFFFF8D0u, SE_TRI_EMISSIVE};
    s_mats[FM_HAIR]     = (mesh_mat_t){NULL, 0xFF60402Au, 0};
    s_mats[FM_FACE]     = (mesh_mat_t){texcache_get("miner_face.png"), 0xFFDEAA80u, 0};
    s_mats[FM_WOOD]     = (mesh_mat_t){NULL, 0xFF7A5230u, 0};
    s_mats[FM_IRON]     = (mesh_mat_t){NULL, 0xFF9AA0A8u, 0};
    fred_build_head(&s_head);
    fred_build_body(&s_body);
    fred_build_arm(&s_arm);
    fred_build_fp_arm(&s_fp_arm);
    fred_build_leg(&s_leg);
    fred_build_pick(&s_pick);
    fred_build_axe(&s_axe);
    fred_build_shovel(&s_shovel);
    // A block in the hand: a small one, in the block's textures.
    mesh_init(&s_cube);
    s_cube.name = "fred_block";
    voxel_build_cube(&s_cube, 0.14f);
    // A torch or a flower in the hand: two crossed quads, each wound
    // both ways, like voxel_mesh.c's plants -- material 1, the side.
    mesh_init(&s_sprite);
    s_sprite.name = "fred_sprite";
    {
        float const  r = 0.16f, h = 0.32f;
        float const  uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        vec3_t const ends[2][2] = {{{-r, 0, -r}, {r, 0, r}}, {{r, 0, -r}, {-r, 0, r}}};
        for (int k = 0; k < 2; k++) {
            for (int side = 0; side < 2; side++) {
                vec3_t const p = ends[k][side], q = ends[k][1 - side];
                int const    a = mesh_vert(&s_sprite, v3(p.x, h, p.z)), b = mesh_vert(&s_sprite, v3(q.x, h, q.z));
                int const    c = mesh_vert(&s_sprite, v3(q.x, 0, q.z)), d = mesh_vert(&s_sprite, v3(p.x, 0, p.z));
                mesh_quad(&s_sprite, a, b, c, d, 1, uv);
            }
        }
    }
    // A torch: a thin stick with the whole texture on every face, as
    // voxel_mesh.c draws one in the world (its texture is the stick, not
    // a sprite with holes round it).
    mesh_init(&s_torch);
    s_torch.name = "fred_torch";
    {
        float const r = 0.03f, h = 0.30f;
        int         v[8];
        for (int i = 0; i < 8; i++) v[i] = mesh_vert(&s_torch, v3(i & 1 ? r : -r, i & 2 ? h : 0.0f, i & 4 ? r : -r));
        float const uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        mesh_quad(&s_torch, v[3], v[7], v[5], v[1], 1, uv);  // +x
        mesh_quad(&s_torch, v[6], v[2], v[0], v[4], 1, uv);  // -x
        mesh_quad(&s_torch, v[2], v[6], v[7], v[3], 0, uv);  // +y: the flame end
        mesh_quad(&s_torch, v[0], v[1], v[5], v[4], 2, uv);  // -y
        mesh_quad(&s_torch, v[7], v[6], v[4], v[5], 1, uv);  // +z
        mesh_quad(&s_torch, v[2], v[3], v[1], v[0], 1, uv);  // -z
    }
    s_ready = true;
}

void fred_shutdown(void) {
    if (!s_ready) return;
    mesh_free(&s_head);
    mesh_free(&s_body);
    mesh_free(&s_arm);
    mesh_free(&s_fp_arm);
    mesh_free(&s_leg);
    mesh_free(&s_pick);
    mesh_free(&s_axe);
    mesh_free(&s_shovel);
    mesh_free(&s_cube);
    mesh_free(&s_sprite);
    mesh_free(&s_torch);
    s_ready = false;
}

fred_hold_t fred_hold_for(uint16_t item) {
    fred_hold_t h = {0};
    if (item == 0) return h;
    if (item_is_block(item)) {
        h.block             = item_block(item);
        block_kind_t const k = block_kind(h.block);
        h.kind              = k == K_TORCH ? FRED_HOLD_TORCH : k == K_PLANT ? FRED_HOLD_SPRITE : FRED_HOLD_BLOCK;
        return h;
    }
    item_def_t const d = item_def(item);
    if (d.tool == TOOL_PICK || d.tool == TOOL_AXE || d.tool == TOOL_SHOVEL) {
        h.kind  = FRED_HOLD_TOOL;
        h.tool  = d.tool;
        h.level = d.tool_level;
        return h;
    }
    h.kind = FRED_HOLD_ITEM;
    h.argb = d.argb;
    return h;
}

float fred_stroke(float phase) {
    float const p = phase - floorf(phase);
    if (p < 0.35f) return smoothstep(0.0f, 0.35f, p);        // up
    if (p < 0.5f) return 1.0f - smoothstep(0.35f, 0.5f, p);  // down, hard
    return 0.0f;                                             // rest
}

// A joint: `parent` then a turn `r` about the point `at` (in the parent).
static xform_t joint(xform_t const* parent, vec3_t at, mat3_t r) {
    xform_t const local = {r, at, 1.0f};
    return xform_mul(parent, &local);
}

static mat3_t ident(void) {
    return mat3_rot_x(0.0f);
}

// The materials, lit by `light`. A copy per draw: ten entries, and it
// keeps the table itself untouched.
static void lit(mesh_mat_t out[FM_COUNT], uint8_t light) {
    uint32_t const f = SE_TRI_LIGHT(mesh_light_level(light));
    for (int i = 0; i < FM_COUNT; i++) {
        out[i] = s_mats[i];
        out[i].flags |= f;
    }
}

// What the fist holds, in the arm's frame.
static void submit_held(xform_t const* arm, fred_hold_t const* hold, mesh_mat_t const* mats, uint8_t light) {
    uint32_t const f = SE_TRI_LIGHT(mesh_light_level(light));
    if (hold->kind == FRED_HOLD_TOOL) {
        xform_t const fist = joint(arm, v3(0.0f, FRED_FIST_Y, 0.0f), ident());
        mesh_mat_t    m[FM_COUNT];
        for (int i = 0; i < FM_COUNT; i++) m[i] = mats[i];
        m[FM_IRON].argb     = HEAD_ARGB[hold->level < 4 ? hold->level : 0];
        mesh_t const* tool = hold->tool == TOOL_AXE ? &s_axe : hold->tool == TOOL_SHOVEL ? &s_shovel : &s_pick;
        mesh_submit(tool, &fist, m, FM_COUNT);
    } else if (hold->kind == FRED_HOLD_SPRITE || hold->kind == FRED_HOLD_TORCH) {
        // Held out of the fist the way the tools are, top end forward.
        xform_t const at = joint(arm, v3(0.0f, FRED_FIST_Y, 0.02f), mat3_rot_x(1.57f));
        mesh_mat_t    m[3];
        chunk_render_block_mats(hold->block, m);
        for (int i = 0; i < 3; i++) m[i].flags |= f;
        mesh_submit(hold->kind == FRED_HOLD_TORCH ? &s_torch : &s_sprite, &at, m, 3);
    } else if (hold->kind == FRED_HOLD_BLOCK || hold->kind == FRED_HOLD_ITEM) {
        xform_t const at = joint(arm, v3(0.0f, FRED_FIST_Y, 0.16f), mat3_rot_y(0.5f));
        mesh_mat_t    m[3];
        if (hold->kind == FRED_HOLD_BLOCK) {
            chunk_render_block_mats(hold->block, m);
        } else {
            for (int i = 0; i < 3; i++) m[i] = (mesh_mat_t){NULL, hold->argb, 0};
        }
        for (int i = 0; i < 3; i++) m[i].flags |= f;
        mesh_submit(&s_cube, &at, m, 3);
    }
}

void fred_submit(xform_t const* root, fred_pose_t const* p, uint8_t light) {
    if (!s_ready) return;
    mesh_mat_t mats[FM_COUNT];
    lit(mats, light);

    xform_t const scaled = {root->r, root->pos, root->scale * FRED_SCALE};
    float const   leg    = 0.55f * p->stride * sinf(p->walk);
    float const   bob    = 0.04f * p->stride * fabsf(sinf(p->walk));
    xform_t const hips   = joint(&scaled, v3(0.0f, FRED_LEG_H + bob, 0.0f), ident());

    // Legs: the right one (on -x) forward when sin(walk) > 0.
    xform_t const leg_r = joint(&hips, v3(-FRED_HIP_X, 0.0f, 0.0f), mat3_rot_x(-leg));
    xform_t const leg_l = joint(&hips, v3(FRED_HIP_X, 0.0f, 0.0f), mat3_rot_x(leg));
    mesh_submit(&s_leg, &leg_r, mats, FM_COUNT);
    mesh_submit(&s_leg, &leg_l, mats, FM_COUNT);
    mesh_submit(&s_body, &hips, mats, FM_COUNT);

    // Arms swing against the legs; the tool arm also lifts the tool (a
    // turn about x of -angle swings the hand forward and up).
    //
    // WHICH SIDE IS HIS RIGHT: +x. The showreel built the miner with his
    // right hand on -x, but in this engine the camera's right at yaw 0 is
    // +x (forward (sin yaw, cos yaw), right (cos yaw, -sin yaw)), so a
    // figure facing +z has his right on +x -- and the showreel's miner,
    // seen from behind, held everything in his LEFT hand while the first-
    // person arm held it in the right (F-64).
    float const   side   = p->left_handed ? -1.0f : 1.0f;
    float const   raise  = p->hold.kind != FRED_HOLD_NONE ? 0.35f + 1.9f * p->swing : 1.2f * p->swing;
    float const   arm_sw = 0.45f * p->stride * sinf(p->walk);
    vec3_t const  sh_y   = v3(0.0f, FRED_BODY_H - 0.06f, 0.0f);
    xform_t const arm_t  = joint(&hips, v3_add(sh_y, v3(side * FRED_SHOULDER_X, 0, 0)), mat3_rot_x(arm_sw - raise));
    xform_t const arm_o  = joint(&hips, v3_add(sh_y, v3(-side * FRED_SHOULDER_X, 0, 0)), mat3_rot_x(-arm_sw));
    mesh_submit(&s_arm, &arm_t, mats, FM_COUNT);
    mesh_submit(&s_arm, &arm_o, mats, FM_COUNT);
    submit_held(&arm_t, &p->hold, mats, light);

    mat3_t const  yaw   = mat3_rot_y(p->head_yaw);
    mat3_t const  pitch = mat3_rot_x(p->head_pitch);
    xform_t const head  = joint(&hips, v3(0.0f, FRED_BODY_H, 0.0f), mat3_mul(&yaw, &pitch));
    mesh_submit(&s_head, &head, mats, FM_COUNT);
}

// First person. The showreel put the shoulder 0.9 blocks in front of the
// eye; here the player stands with a wall 0.3 blocks from it whenever
// they mine what is in front of them, and an arm that far out vanishes
// into the block it is hitting. So it is drawn at a third of the
// distance and a third of the size -- the same picture, since the size
// on screen is size over distance -- and stays in front of the wall.
#define FP_K 0.33f

void fred_submit_fp_arm(float swing, fred_hold_t const* hold, float bob, uint8_t light, bool left_handed) {
    if (!s_ready) return;
    mesh_mat_t mats[FM_COUNT];
    lit(mats, light);
    // In the camera's frame (x right, y up, z forward): the shoulder at
    // the lower right, the arm reaching forward and in towards the
    // middle; a swing lifts it and brings it down.
    // Left-handed, the same arm mirrored: the other side of the view,
    // turned in the other way.
    float const   side = left_handed ? -1.0f : 1.0f;
    xform_t const cam  = {camera_basis(), camera_eye(), 1.0f};
    mat3_t const  in   = mat3_rot_y(-0.3f * side);
    mat3_t const  up   = mat3_rot_x(-1.45f - 0.7f * swing);
    xform_t const arm0 = joint(&cam, v3_scale(v3(0.6f * side, -0.62f + bob, 0.9f), FP_K), mat3_mul(&in, &up));
    xform_t const arm  = {arm0.r, arm0.pos, 0.75f * FP_K};
    mesh_submit(&s_fp_arm, &arm, mats, FM_COUNT);
    submit_held(&arm, hold, mats, light);
}
