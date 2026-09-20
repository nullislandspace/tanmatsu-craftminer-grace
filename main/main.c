// =====================================================================
//  CraftMiner  --  the app skeleton
// ---------------------------------------------------------------------
//  The whole app for now: one block turning in front of the camera, lit
//  by a sun. It is here to prove the shape of an engine app end to end
//  -- run loop, scene, lighting, input -- and to be the thing the block
//  world grows out of.
//
//  The engine owns the loop (se_run): device bootstrap, the frame clock,
//  the input pump, the device-global keys, vsync and the blit. This file
//  is content plus per-frame logic, which is the whole point of the
//  inversion: see synthengine3D/docs/architecture.md.
//
//  What is deliberately NOT here yet: the world, a mesh builder,
//  textures, a camera that moves with the player. Those arrive with the
//  game.
// =====================================================================

#include <math.h>
#include "esp_log.h"
#include "synthengine3d.h"  // the whole public API

static char const TAG[] = "craftminer";

// The one block: half-extent, where it sits, and how fast it turns.
#define BLOCK_HALF 0.5f
#define BLOCK_X    0.0f
#define BLOCK_Y    0.0f
#define BLOCK_Z    3.0f
#define SPIN_RATE  0.6f  // radians per second

static float s_angle;  // block spin, radians
static bool  s_spinning = true;

// Grass on top, dirt underneath, grass-over-dirt on the sides: the
// colours a block world starts from, before there are any textures.
#define ARGB_GRASS 0xFF6BA13Au
#define ARGB_SIDE  0xFF8A6A42u
#define ARGB_DIRT  0xFF6B4F2Fu

// --- The block ---------------------------------------------------------
//
// Eight corners, six faces, two triangles each, wound counter-clockwise
// seen from outside so the engine's back-face cull keeps them. Written
// out rather than generated: a mesh builder is the game's business, and
// this file should stay readable as the one place that draws.

static void block_face(float const a[3], float const b[3], float const c[3], float const d[3], uint32_t argb) {
    scene_tri(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2], argb, 0);
    scene_tri(a[0], a[1], a[2], c[0], c[1], c[2], d[0], d[1], d[2], argb, 0);
}

static void draw_block(float angle) {
    float const s = sinf(angle), c = cosf(angle);
    float const h = BLOCK_HALF;

    // The four corner offsets in the ground plane, turned by `angle`.
    float const cx[4] = {-h, h, h, -h};
    float const cz[4] = {-h, -h, h, h};
    float       px[4], pz[4];
    for (int i = 0; i < 4; i++) {
        px[i] = BLOCK_X + cx[i] * c - cz[i] * s;
        pz[i] = BLOCK_Z + cx[i] * s + cz[i] * c;
    }
    float const yb = BLOCK_Y - h, yt = BLOCK_Y + h;

    // Corners: 0..3 bottom (ccw seen from above), 4..7 the same on top.
    float bot[4][3], top[4][3];
    for (int i = 0; i < 4; i++) {
        bot[i][0] = px[i];
        bot[i][1] = yb;
        bot[i][2] = pz[i];
        top[i][0] = px[i];
        top[i][1] = yt;
        top[i][2] = pz[i];
    }

    block_face(top[0], top[1], top[2], top[3], ARGB_GRASS);  // up
    block_face(bot[3], bot[2], bot[1], bot[0], ARGB_DIRT);   // down
    for (int i = 0; i < 4; i++) {                            // the four sides
        int const j = (i + 1) & 3;
        block_face(bot[i], bot[j], top[j], top[i], ARGB_SIDE);
    }
}

// --- Callbacks ----------------------------------------------------------

// Once, after the engine has booted the display, audio, input and scene.
static void on_init(void* user) {
    (void)user;
    ESP_LOGI(TAG, "CraftMiner on SynthEngine3D %s", se_version_string());

    se_splash_ex("CraftMiner", "a block world", 1.2f);

    // A sun over the left shoulder. `brightness` is the directional
    // share of the light; the rest is fill, so a face turned away goes
    // dim rather than black.
    se_light_set(&(se_light_t){.x = -600.0f, .y = 900.0f, .z = -400.0f, .brightness = 0.55f, .two_sided = false});

    // Output-neutral scene passes, both off by default. Frustum culling
    // is a near-pure win; depth ordering trades work against overdraw,
    // so it waits until there are real scenes to measure.
    scene_set_options(&(se_scene_options_t){.frustum_cull = true, .depth_order = false});
}

// Per frame. `dt` is seconds since the last frame, already clamped.
static void on_update(float dt, void* user) {
    (void)user;
    if (s_spinning) s_angle += SPIN_RATE * dt;
}

// Whatever the engine did not consume itself (it takes volume, the
// audio jack, and F1 while f1_exits is set). Scancodes arrive for the
// release too, with BSP_INPUT_SCANCODE_RELEASE_MODIFIER set, so an exact
// match fires on the press only.
static void on_input(bsp_input_event_t const* ev, void* user) {
    (void)user;
    if (ev->type != INPUT_EVENT_TYPE_SCANCODE) return;
    if (ev->args_scancode.scancode == BSP_INPUT_SCANCODE_SPACE) {
        s_spinning = !s_spinning;
        ESP_LOGI(TAG, "spin %s", s_spinning ? "on" : "off");
    }
}

// Per frame, after the engine has cleared the backdrop.
static void on_render(pax_buf_t* fb, void* user) {
    (void)user;
    // Eye slightly above the block, looking down the +z axis at it.
    render_set_camera_6dof(0.0f, 0.9f, -1.2f, 0.0f, -0.22f, 0.0f);

    scene_begin(fb);
    draw_block(s_angle);
    scene_render(SE_RENDER_ZBUFFER);
}

void app_main(void) {
    static se_app_config_t const cfg = {
        .f1_exits      = true,         // the engine returns to the launcher
        .backdrop_argb = 0xFF6EA8D8u,  // a flat daylight sky, for now
    };
    static se_app_callbacks_t const cb = {
        .on_init   = on_init,
        .on_input  = on_input,
        .on_update = on_update,
        .on_render = on_render,
    };
    se_run(&cfg, &cb, NULL);  // no user context yet: the state is static
}
