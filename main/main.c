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
#include <string.h>
#include "common/texcache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "game/flycam.h"
#include "gl_input.h"
#include "graceloader.h"
#include "math/mesh_render.h"
#include "synthengine3d.h"  // the whole public API
#include "testkit/devtest.h"
#include "testkit/profile.h"
#include "testkit/showtime.h"
#include "world/chunk.h"
#include "world/chunk_render.h"
#include "world/chunk_worker.h"
#include "world/region.h"
#include "world/vfs_compat.h"
#include "world/worldgen.h"
#include "world/worldstore.h"

static char const TAG[] = "craftminer";

// How many finished chunk jobs to take delivery of per frame. Applying
// a mesh means freeing the old one and touching the new, so a burst of
// completions has to be spread out or it lands as a dropped frame.
#define CHUNK_RESULTS_PER_FRAME 2

// --- Quarter-resolution rendering -----------------------------------------
//
// The scene is drawn at half the width and half the height -- a quarter
// of the pixels -- into a PSRAM layer, which the PPA then scales back up
// onto the screen for nothing. This is the single biggest lever the
// engine offers a voxel view, because that view is fill-bound: textured
// fill runs at about 5 Mpx/s, so a screenful of textured blocks costs
// tens of milliseconds whatever the triangle count (F-03).
//
// The cost is sharpness: the PPA's scaler interpolates, so the result is
// soft rather than crisp 2x2 blocks. That is the trade the graphics menu
// will expose (D-06); half is the default because full resolution
// measured 7.8 fps.
#define JOB_UPSCALE 1u

static se_ppa_layer_t s_half;
static bool           s_quarter = true;

// --- The camera ---------------------------------------------------------
//
// There is no player yet (block 2), so the world is looked at two ways,
// and which one is in charge is decided by whether a test is running:
//
//   A TEST IS RUNNING   a fixed circular path, a pure function of the
//                       show clock. That is what makes the `shots`
//                       framebuffer hashes mean anything and the `perf`
//                       numbers comparable between runs (devtest.h).
//
//   NOBODY IS WATCHING  free flight off the keyboard (game/flycam.h),
//                       so the world can be looked at from wherever it
//                       is suspected of being wrong.
//
// The scripted path is also what answers the question the whole render
// plan rests on: what does a real voxel view cost?

#define FLY_SPEED  6.0f   // blocks a second
#define FLY_RADIUS 90.0f  // of the circle it walks
#define FLY_EYE_H  3.0f   // above the ground below it
#define FLY_PITCH  0.18f  // POSITIVE IS DOWN (se_scene.c, camera_build_basis)

static double   s_time_off;  // show time subtracted while paused
static double   s_paused_at;
static bool     s_flying = true;
static flycam_t s_free;
static bool     s_free_ready;  // the free camera has been put somewhere sensible

static double fly_time(void) {
    return (s_flying ? showtime_now() : s_paused_at) - s_time_off;
}

// Where the scripted camera is, in WORLD coordinates. The render origin
// turns this into the small numbers the scene sees.
static void fly_pose(double t, double* wx, double* wz, float* yaw) {
    double const a = (double)FLY_SPEED * t / (double)FLY_RADIUS;
    *wx            = cos(a) * (double)FLY_RADIUS;
    *wz            = sin(a) * (double)FLY_RADIUS;
    // Along the direction of travel, which is d/da of the position:
    // (-sin a, cos a). The engine's forward in x and z is
    // (sin yaw, cos yaw), so the yaw that matches both components is
    // -a. (The showreel's +pi/2 matched neither, and the flight has
    // been looking sideways ever since.)
    *yaw           = (float)(-a);
}

// This frame's camera, in world coordinates, filled in on_update and
// used by on_render. One place decides, so the streaming and the
// drawing can never disagree about where the eye is.
static struct {
    double wx, wz;
    float  wy, yaw, pitch;
} s_cam;

// --- Memory ---------------------------------------------------------------
//
// What is actually free, logged at boot. The resident chunk set is by
// far this game's largest allocation (chunk.h: 256 slots x 32 KiB), and
// how many chunks can be resident is what sets the view distance -- so
// the free figure is a design input, not a curiosity. Logged before and
// after the slab so both the headroom and the cost are on record.
//
// Called from on_init, which the engine runs AFTER its own bootstrap:
// the two framebuffers, the depth plane and the geometry lists are
// already allocated by then, so this is the memory the game really has.
static void log_memory(char const* when) {
    size_t const ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t const ps_big  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t const in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t const in_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "mem %-14s PSRAM %u KiB free (largest %u KiB) | internal %u KiB free (largest %u KiB)", when,
             (unsigned)(ps_free / 1024), (unsigned)(ps_big / 1024), (unsigned)(in_free / 1024),
             (unsigned)(in_big / 1024));
}

// --- The test kit ---------------------------------------------------------
//
// The kit drives the app over the debug console: the host asks for a
// test, the app runs it inside this frame loop, reports, and returns to
// the launcher by itself, so `make cycle` needs nobody at the badge.
// It only needs to be able to name a piece of content, start it, and
// say how far in it is (devtest.h).
//
// Today there is one: the block. When the streaming world and the input
// replay arrive (steps 2 and 3), this is where the replay scenes get
// named, and `select` starts one from its beginning.

static char const* s_content = "block";
static double      s_content_t0;

static bool content_select(char const* name) {
    if (name == NULL || strcmp(name, "block") != 0) return false;
    s_content    = "block";
    s_content_t0 = showtime_now();
    s_time_off   = s_content_t0;  // the content's own t = 0
    s_flying     = true;
    return true;
}
static float content_duration(void) {
    return -1.0f;
}  // endless
static double content_started(void) {
    return s_content_t0;
}
static char const* content_name(void) {
    return s_content;
}
static char const* content_shot(void) {
    return "";
}

static devtest_content_t const CONTENT = {
    .select    = content_select,
    .duration  = content_duration,
    .started   = content_started,
    .name      = content_name,
    .shot_name = content_shot,
};

static devtest_config_t const TEST = {
    .app      = "at.cavac.craftminer",
    .shot_dir = SCREENSHOT_DIR,
    .content  = &CONTENT,
};

// Once a second: the frame rate the kit reports, and the phase split.
static void frame_stats(void);

static void frame_stats(void) {
    static int64_t s_window_us;
    static int     s_frames;
    static int64_t s_last_us;

    int64_t const now = esp_timer_get_time();
    if (s_last_us != 0) s_window_us += now - s_last_us;
    s_last_us = now;
    s_frames++;
    prof_frame();

    if (s_window_us < 1000000) return;

    float const fps      = (float)s_frames * 1000000.0f / (float)s_window_us;
    float const frame_ms = (float)s_window_us / 1000.0f / (float)s_frames;
    devtest_period(fps, frame_ms);

    char split[192];
    prof_flush(split, sizeof(split), frame_ms);
    ESP_LOGI(TAG, "%s", split);

    int tested = 0, passed = 0, drawn = 0, sections = 0, resident = 0, missing = 0;
    mesh_submit_counters(&tested, &passed);
    chunk_render_stats(&drawn, &sections, &resident, &missing);
    ESP_LOGI(TAG, "world: %d chunks / %d sections drawn of %d resident (%d missing), %d tris tested -> %d submitted",
             drawn, sections, resident, missing, tested, passed);

    s_window_us = 0;
    s_frames    = 0;
}

// --- Callbacks ----------------------------------------------------------

// Once, after the engine has booted the display, audio, input and scene.
static void on_init(void* user) {
    (void)user;
    ESP_LOGI(TAG, "CraftMiner on SynthEngine3D %s", se_version_string());
    devtest_start(&TEST);

    se_splash_ex("CraftMiner", "a block world", 1.2f);

    // A sun over the left shoulder. `brightness` is the directional
    // share of the light; the rest is fill, so a face turned away goes
    // dim rather than black.
    se_light_set(&(se_light_t){.x = -600.0f, .y = 900.0f, .z = -400.0f, .brightness = 0.55f, .two_sided = false});

    // Both output-neutral, both off by default. Frustum culling is a
    // near-pure win. Depth ordering trades a sort against overdraw, and
    // a voxel view is exactly the dense case where it pays -- the
    // showreel measured a canopy shot at 220 -> 175 ms (F-03).
    scene_set_options(&(se_scene_options_t){.frustum_cull = true, .depth_order = true});

    // --- The world ---------------------------------------------------
    log_memory("engine booted");

    if (!chunk_store_init()) {
        ESP_LOGE(TAG, "chunk_store_init failed (%u KiB)", (unsigned)(chunk_store_bytes() / 1024));
        return;
    }
    ESP_LOGI(TAG, "chunk slab: %u KiB (%d slots x %u KiB)", (unsigned)(chunk_store_bytes() / 1024), CH_SLOT_COUNT,
             (unsigned)(chunk_store_bytes() / CH_SLOT_COUNT / 1024));
    log_memory("world resident");

    // The half-size layer the scene draws into. It has to match the
    // framebuffer's format and orientation, which the engine resolved at
    // bootstrap.
    if (se_ppa_init()) {
        se_display_info_t di;
        se_display_info(&di);
        if (se_ppa_layer_alloc(&s_half, DISPLAY_LOG_W / 2, DISPLAY_LOG_H / 2, di.pax_format, di.reversed,
                               di.orientation)) {
            ESP_LOGI(TAG, "quarter-resolution layer: %dx%d", DISPLAY_LOG_W / 2, DISPLAY_LOG_H / 2);
        } else {
            ESP_LOGW(TAG, "no quarter-resolution layer; drawing at full resolution");
            s_quarter = false;
        }
    } else {
        ESP_LOGW(TAG, "PPA unavailable; drawing at full resolution");
        s_quarter = false;
    }

    // Textures, then the material tables that map blocks onto them.
    static char tex_dir[160];
    snprintf(tex_dir, sizeof(tex_dir), "%s/textures", graceloader_get_install_basepath());
    texcache_init(tex_dir);
    if (!chunk_render_init()) {
        ESP_LOGE(TAG, "chunk_render_init failed");
        return;
    }
    // Near, not medium: medium submitted about 4400 triangles a frame,
    // past the engine's 4096 flat cap, where the overflow is dropped
    // silently. The graphics menu will offer the others (D-06).
    chunk_render_set_view(&(cm_view_t){0});
    cm_view_t const v = cm_view_preset(0);
    chunk_render_set_view(&v);
    log_memory("textures loaded");

    // A world to fly over. Step 5 gives this a menu; for now it is one
    // fixed world, created if it is not there and opened if it is, so
    // the second run exercises loading rather than generation.
    if (!worldstore_init(graceloader_get_install_basepath())) {
        ESP_LOGE(TAG, "worldstore_init failed");
        return;
    }
    static world_meta_t   meta;
    static player_state_t player;
    if (!worldstore_open("flyover", &meta, &player)) {
        if (!worldstore_create("flyover", 0xC0FFEEu, &meta, &player)) {
            ESP_LOGE(TAG, "could not create the flyover world");
            return;
        }
        ESP_LOGI(TAG, "world 'flyover' created, seed %u", (unsigned)meta.seed);
    } else {
        ESP_LOGI(TAG, "world 'flyover' opened, seed %u", (unsigned)meta.seed);
    }

    if (!chunk_worker_start(meta.seed)) {
        ESP_LOGE(TAG, "chunk_worker_start failed");
        return;
    }
    ESP_LOGI(TAG, "chunk worker running, %s", chunk_worker_synchronous() ? "SYNCHRONOUS" : "on core 1");
    log_memory("world ready");
}

// Per frame. `dt` is seconds since the last frame, already clamped --
// unused for now: the content is drawn from the show clock instead.
static void on_update(float dt, void* user) {
    (void)user;
    showtime_frame();
    devtest_update();

    // Where the camera will be this frame decides what has to exist.
    if (devtest_running()) {
        float yaw;
        fly_pose(fly_time(), &s_cam.wx, &s_cam.wz, &yaw);
        s_cam.yaw        = yaw;
        s_cam.pitch      = FLY_PITCH;
        // Follows the ground, so the flight stays over the terrain
        // rather than through it.
        int const ground = world_ground((int32_t)floor(s_cam.wx), (int32_t)floor(s_cam.wz));
        s_cam.wy         = (float)(ground > 0 ? ground : CH_SEA_LEVEL) + FLY_EYE_H;
    } else {
        if (!s_free_ready) {
            // Start where the scripted path starts, so the first thing
            // seen by hand is the same view the tests measure.
            double wx, wz;
            float  yaw;
            fly_pose(0.0, &wx, &wz, &yaw);
            flycam_reset(&s_free, wx, wz, (float)CH_SEA_LEVEL + 8.0f, yaw);
            s_free_ready = true;
        }
        flycam_update(&s_free, dt);
        s_cam.wx    = s_free.wx;
        s_cam.wz    = s_free.wz;
        s_cam.wy    = s_free.wy;
        s_cam.yaw   = s_free.yaw;
        s_cam.pitch = s_free.pitch;
    }

    // Take delivery of what core 1 finished, with a budget so a burst
    // cannot blow a frame, then ask for what is still missing.
    chunk_worker_collect(CHUNK_RESULTS_PER_FRAME);
    chunk_render_stream(s_cam.wx, s_cam.wz);
}

// Whatever the engine did not consume itself (it takes volume, the
// audio jack, and F1 while f1_exits is set). Scancodes arrive for the
// release too, with BSP_INPUT_SCANCODE_RELEASE_MODIFIER set, so an exact
// match fires on the press only.
//
// Movement is NOT here: flycam.h polls the keys it wants, because a key
// held down is a state and not an event. What is here is the handful of
// things that toggle, which is exactly what an event is for.
//
//   P  pause the scripted flight (free flight has nothing to pause)
//   T  textures <-> flat mean colours
//   V  view distance: near / medium / far
static void on_input(bsp_input_event_t const* ev, void* user) {
    (void)user;
    if (ev->type != INPUT_EVENT_TYPE_SCANCODE) return;
    switch (ev->args_scancode.scancode) {
        case BSP_INPUT_SCANCODE_P:
            if (s_flying) {
                s_paused_at = showtime_now();
            } else {
                s_time_off += showtime_now() - s_paused_at;
            }
            s_flying = !s_flying;
            ESP_LOGI(TAG, "scripted flight %s", s_flying ? "on" : "off");
            break;

        case BSP_INPUT_SCANCODE_T:
            chunk_render_set_textured(!chunk_render_textured());
            ESP_LOGI(TAG, "shading: %s", chunk_render_textured() ? "textured" : "flat");
            break;

        case BSP_INPUT_SCANCODE_V: {
            // 0 near, 1 medium, 2 far -- and back round. Told apart by
            // the draw distance, which is what the presets differ in.
            static int level  = 0;
            level             = (level + 1) % 3;
            cm_view_t const v = cm_view_preset(level);
            chunk_render_set_view(&v);
            ESP_LOGI(TAG, "view distance: %s (%d blocks, %d chunks resident)",
                     level == 0   ? "near"
                     : level == 1 ? "medium"
                                  : "far",
                     (int)v.draw_dist, v.load_radius);
        } break;

        default:
            break;
    }
}

// The engine clears the framebuffer to cfg.backdrop_argb every frame
// when no backdrop callback is registered -- a full 800x480 CPU fill.
// The upscaled layer covers every pixel of it, so that clear is pure
// waste. Registering this empty callback is how a game says "I paint
// all of it myself".
static void on_backdrop(pax_buf_t* fb, void* user) {
    (void)fb;
    (void)user;
}

// Per frame, after the engine has cleared the backdrop.
static void on_render(pax_buf_t* fb, void* user) {
    (void)user;

    // Everything is submitted relative to an integer origin near the
    // camera, so the floats the rasteriser sees stay small however far
    // out this is (D-01). The camera goes into the same space.
    chunk_render_set_origin((int32_t)floor(s_cam.wx), (int32_t)floor(s_cam.wz));
    int32_t ox, oz;
    chunk_render_origin(&ox, &oz);

    float rx, ry, rz;
    chunk_render_rel(ox, oz, s_cam.wx, (double)s_cam.wy, s_cam.wz, &rx, &ry, &rz);
    render_set_camera_6dof(rx, ry, rz, s_cam.yaw, s_cam.pitch, 0.0f);

    pax_buf_t* const target = s_quarter ? &s_half.buf : fb;
    scene_set_render_scale(s_quarter ? 2 : 1);

    // The sky. The engine clears `fb`, but the scene is drawing into the
    // half-size layer, so that is what needs filling -- whatever it does
    // not cover is what shows through after the upscale.
    prof_begin(PROF_FILL);
    if (s_quarter) {
        se_ppa_fill(target, 0, 0, DISPLAY_LOG_H / 2, CM_SKY_ARGB);
        se_ppa_wait_job(0);
    }
    prof_end(PROF_FILL);

    scene_begin(target);

    prof_begin(PROF_SUBMIT);
    mesh_submit_counters_reset();
    chunk_render_submit(s_cam.wx, s_cam.wz);
    prof_end(PROF_SUBMIT);

    prof_begin(PROF_PREPARE);
    scene_prepare(SE_RENDER_ZBUFFER);
    prof_end(PROF_PREPARE);

    prof_begin(PROF_RASTER);
    int64_t const t0 = esp_timer_get_time();
    scene_rasterize(SE_RENDER_ZBUFFER);
    int64_t const rast_us = esp_timer_get_time() - t0;
    prof_end(PROF_RASTER);

    if (s_quarter) {
        prof_begin(PROF_WAIT);
        // The CPU's pixels have to reach PSRAM before the PPA's DMA
        // reads them: the layer is small enough to sit in cache.
        se_ppa_layer_sync(&s_half);
        if (se_ppa_blit_scaled(fb, JOB_UPSCALE, &s_half, 2)) {
            se_ppa_wait_job(JOB_UPSCALE);
            se_ppa_buf_invalidate(fb);  // the screenshot path reads fb
        }
        prof_end(PROF_WAIT);
    }

    devtest_after_render(fb, rast_us);
    frame_stats();
}

void app_main(void) {
    static se_app_config_t const cfg = {
        .f1_exits      = true,         // the engine returns to the launcher
        .backdrop_argb = 0xFF6EA8D8u,  // a flat daylight sky, for now
    };
    static se_app_callbacks_t const cb = {
        .on_init     = on_init,
        .on_backdrop = on_backdrop,
        .on_input    = on_input,
        .on_update   = on_update,
        .on_render   = on_render,
    };
    se_run(&cfg, &cb, NULL);  // no user context yet: the state is static
}
