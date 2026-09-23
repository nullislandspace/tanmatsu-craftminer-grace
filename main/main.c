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
#include <time.h>
#include "audio/audio.h"
#include "audio/music.h"
#include "audio/sfx.h"
#include "bsp/device.h"
#include "common/texcache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "game/daytime.h"
#include "fred/fred.h"
#include "game/flycam.h"
#include "game/raycast.h"
#include "game/hud.h"
#include "game/input.h"
#include "game/interact.h"
#include "game/membench.h"
#include "game/player.h"
#include "game/replay.h"
#include "items/item_entity.h"
#include "game/tick.h"
#include "gl_input.h"
#include "graceloader.h"
#include "i18n/i18n.h"
#include "math/mesh_render.h"
#include "synthengine3d.h"  // the whole public API
#include "testkit/devtest.h"
#include "testkit/profile.h"
#include "testkit/report.h"
#include "testkit/showtime.h"
#include "ui/icons.h"
#include "ui/bench_ui.h"
#include "ui/chest_ui.h"
#include "ui/craft_ui.h"
#include "ui/furnace_ui.h"
#include "ui/menu.h"
#include "ui/settings.h"
#include "ui/title.h"
#include "voxel/voxel_sky.h"
#include "world/chunk.h"
#include "world/chunk_render.h"
#include "world/chunk_worker.h"
#include "world/chunkmesh.h"
#include "world/light.h"
#include "world/region.h"
#include "testkit/screenshot.h"
#include "world/datadir.h"
#include "world/vfs_compat.h"
#include "world/worldgen.h"
#include "world/worldstore.h"

static char const TAG[] = "craftminer";

// The near clip plane has to be closer than the nearest thing the eye
// can legitimately be to, or that thing is clipped away and the player
// sees through the world. Two cases, and the ceiling is the tight one:
//
//   a wall they are touching   half the body width          0.30
//   a ceiling they stand under h - eye height = 1.8 - 1.62  0.18
//
// Set in CMakeLists.txt, where the reasoning is; checked here, where
// both numbers are visible at once. It was 0.5 and you could put your
// face through a tree.
_Static_assert(RENDER_NEAR_CLIP_Z < (double)(PHYS_PLAYER_H - PHYS_PLAYER_EYE),
               "the near clip plane is further than the player's head is from a ceiling: "
               "they will see through it");
_Static_assert(RENDER_NEAR_CLIP_Z < (double)(PHYS_PLAYER_W * 0.5f),
               "the near clip plane is further than the player's eye is from a wall they touch: "
               "they will see through it");

// How many finished chunk jobs to take delivery of per frame.
//
// Taking delivery is a pointer swap and a free, so the budget only
// needs to stop a burst landing as one dropped frame -- and it has to
// be big enough to keep up with what is asked for, or the worker fills
// its result queue, blocks, and stops loading chunks as well. Two was
// right when a chunk had three meshes. It now has twelve (CH_MESH_N),
// and two a frame is about thirty a second against a fast flight's
// ninety: the world falls behind the camera and looks like it is
// reloading itself. Eight covers it with room to spare, and each one
// is a quarter the size it used to be.
#define CHUNK_RESULTS_PER_FRAME 8

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
// Whether the half-size layer exists. Whether it is USED is the
// player's choice (settings_half_res); this is whether it can be.
static bool           s_half_ok = true;

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

// --- Walking, rather than flying ----------------------------------------
//
// Block 3. The player is the default now; the free camera is still
// there on a key, because looking at the world from above is how half
// the render bugs so far were found.
//
// WHO DRIVES THE CAMERA:
//   a test is running   the scripted circle, a pure function of the
//                       show clock, so `shots` hashes mean something
//   F key               the free camera (flycam.h)
//   otherwise           the player (player.h), at a fixed 20 Hz

typedef enum { CAM_PLAYER = 0, CAM_FREE, CAM_SCRIPTED } cam_mode_t;

// --- What the app is doing ------------------------------------------------
//
// The title runs on its own scratch world (ui/title.h), so entering the
// game means closing that and opening a real one. That is the whole
// reason this is a state machine rather than a flag: the WORLD changes
// with the screen, and the chunk store has to be told.
typedef enum {
    APP_TITLE = 0,  // "CraftMiner" in blocks over a generated meadow
    APP_PLAY,       // a real world, open and saving
    APP_LOADING,    // generating what the next state will look at, with a progress bar
} app_state_t;

static app_state_t s_app = APP_TITLE;
static double      s_title_t0;

static bool enter_title(void);
static bool enter_world(int slot, bool create, char const* name, uint32_t seed);
static void save_world(char const* why);
// How much of the world has to be there before play starts (D-26).
typedef enum {
    LOAD_GATE_ALL = 0,  // everything in the view distance
    LOAD_GATE_3X3,      // the nine chunks around the player, and no more
} load_gate_t;

static void start_loading(double wx, double wz, app_state_t next, char const* what, load_gate_t gate);

// --- The time of day ------------------------------------------------------
//
// The world's clock (world_meta_t.time_of_day) moves one tick per
// simulation tick while a world is played, and everything the sky and
// the light need comes from it (game/daytime.h), worked out once a
// frame. The title stands at a fixed morning.
#define TITLE_TIME (DAY_START + 2500)

static daytime_t s_day;

// A screenshot asked for (CM_SCREENSHOT), and the line that says where it
// went (take_screenshot).
static bool   s_shot_wanted;
static char   s_shot_msg[64];
static double s_shot_msg_until;

// The position overlay (CM_INFO, Backspace by default).
static bool s_info;

// Fred (fred/fred.h): his walk cycle and the swing of his arm, advanced
// each frame from what the player is doing -- drawing, not simulation,
// so they need not be replayable.
#define FRED_BACK      4.0f   // third person: blocks behind his eyes
#define FRED_STROKES   1.8f   // swings a second while mining
static float s_walk, s_stride, s_swing_t;
// The `replay_third` test scene: third person for this run only, without
// touching the player's settings.txt.
static bool  s_force_third;
// ... and `replay_left`: third person, left-handed, likewise for this run only.
static bool  s_force_left;

// Test-only overrides for the replay scenes (see content_select).
static int  s_force_view = -1;
static bool s_force_noclouds, s_force_nolight;

static int view_setting(void) {
    return s_force_view >= 0 ? s_force_view : settings_view();
}

static bool left_handed(void) {
    return s_force_left || settings_left_handed();
}

static bool third_person(void) {
    return s_force_third || settings_third_person();
}

// Replays (game/replay.h). A recording is started and stopped with R
// in a world; a replay is played by the `replay` test scene, on a
// scratch world of the recorded seed. `s_replay_t0` is the show time of
// its first tick, so a test that SETS the clock still gets the tick
// that belongs to that moment.
static double s_replay_t0;
// This run is a replay scene: the player keeps the camera even under a
// test, since the replay is itself the reproducible thing a test wants.
static bool   s_in_replay;
static bool enter_replay(void);
static bool run_savecheck(void);
static bool enter_flight(void);
// The title's clock. A test scene can move it ("title_night") to look at
// the night sky without playing through a day.
static int64_t   s_title_time = TITLE_TIME;
static uint8_t   s_lut[256];  // light byte -> brightness, for mesh_render

// What being underwater looks like (D-86). The two tint factors are
// 0..32, where 32 leaves a channel alone: red and green come down to
// about four tenths, blue stays near full, which is what water does to
// light. WATER_ARGB is both the sky and the fog -- from under the
// surface the distance IS more water, so the two are the same colour,
// and it is dark because the light has been through metres of it.
#define WATER_TINT_RG 13u
#define WATER_TINT_B  27u
#define WATER_ARGB    0xFF14375Fu

static player_t     s_player;
static tick_clock_t s_tick;
static bool         s_player_ready;
static cam_mode_t   s_cam_mode = CAM_PLAYER;
// What actually drove the camera this frame. Not the same as
// s_cam_mode: a running test overrides it, and on_render has to agree
// with on_update about which it was or the overlay describes a camera
// that is not the one being drawn.
static cam_mode_t   s_cam_effective = CAM_PLAYER;
static int          s_ticks_last_frame;

static double fly_time(void) {
    return (s_flying ? showtime_now() : s_paused_at) - s_time_off;
}

// Where the scripted camera is, in WORLD coordinates. The render origin
// turns this into the small numbers the scene sees.
// The "farlands" scene flies this instead: from 64 blocks east of the
// Far Lands edge towards it at walking pace, looking west at the wall,
// and stops 12 blocks short.
static bool s_fl_scene;
#define FL_SCENE_START 64.0
#define FL_SCENE_STOP  12.0
#define FL_SCENE_SPEED 4.3  // blocks a second: walking (PL_WALK at 20 Hz)

static void fly_pose(double t, double* wx, double* wz, float* yaw) {
    if (s_fl_scene) {
        double d = FL_SCENE_START - FL_SCENE_SPEED * t;
        if (d < FL_SCENE_STOP) d = FL_SCENE_STOP;
        *wx  = (double)FARLANDS_X_DEFAULT + d;  // a scratch world: the default edge
        *wz  = 40.5;
        *yaw = (float)(-M_PI / 2.0);  // forward (sin yaw, cos yaw) = (-1, 0): west
        return;
    }
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

// Where the internal SRAM has gone, once, at boot.
//
// "Free" on its own does not answer that: it says how much is left, not
// what took the rest or whether any of it is ours to give back. This
// prints the heap totals AND the addresses of the three kinds of
// storage the app has, because the decisive question -- does an app's
// .bss cost internal SRAM? -- is answered by which region a static
// lives in, and nothing else.
static void log_memory_map(void) {
    static int s_probe;  // a plain static: wherever .bss went, this went
    void*      internal = heap_caps_malloc(64, MALLOC_CAP_INTERNAL);
    void*      psram    = heap_caps_malloc(64, MALLOC_CAP_SPIRAM);

    multi_heap_info_t in, ps;
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "internal heap: %u KiB free of %u KiB (%u KiB used, %u blocks), low water %u KiB",
             (unsigned)(in.total_free_bytes / 1024), (unsigned)((in.total_free_bytes + in.total_allocated_bytes) / 1024),
             (unsigned)(in.total_allocated_bytes / 1024), (unsigned)in.allocated_blocks,
             (unsigned)(in.minimum_free_bytes / 1024));
    ESP_LOGI(TAG, "PSRAM heap:    %u KiB free of %u KiB (%u KiB used, %u blocks)",
             (unsigned)(ps.total_free_bytes / 1024), (unsigned)((ps.total_free_bytes + ps.total_allocated_bytes) / 1024),
             (unsigned)(ps.total_allocated_bytes / 1024), (unsigned)ps.allocated_blocks);
    ESP_LOGI(TAG, "addresses: app static %p | internal alloc %p | PSRAM alloc %p", (void*)&s_probe, internal, psram);
    ESP_LOGI(TAG, "the app's own statics are in %s",
             ((uintptr_t)&s_probe ^ (uintptr_t)psram) < 0x08000000u ? "PSRAM (kbelf loads app.so there)"
                                                                   : "INTERNAL SRAM");
    heap_caps_free(internal);
    heap_caps_free(psram);
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

// The scenes a test can ask for. "block" is the default flight; the
// three view distances are that same flight with the graphics setting
// the menu will offer (D-06), so `perf scene=far` measures what a
// player choosing Far actually gets -- frame rate AND the PSRAM the
// meshes hold, which is the half of the cost a frame rate never shows.
static struct {
    char const* name;
    int         preset;  // -1: leave the view alone
} const SCENES[] = {
    {"block", -1},
    {"title", -1},
    {"title_night", -1},
    {"near", 0},
    {"medium", 1},
    {"far", 2},
};

static bool content_select(char const* name) {
    if (name == NULL) return false;
    s_fl_scene = false;
    // Any scene may be asked for in a language: `scene=menu_settings.de`.
    // It lasts for this run, like the other scene options, and never
    // reaches settings.txt -- it is here so a screenful of Bulgarian can
    // be photographed without anybody's saved settings being touched.
    static char base[48];  // the size of the scene buffers below
    char const* dot = strrchr(name, '.');
    if (dot != NULL) {
        cm_lang_t lang;
        if (i18n_language_from_code(dot + 1, &lang)) {
            i18n_set_language(lang);
            i18n_load_overrides(CM_DATA_DIR);
            snprintf(base, sizeof(base), "%.*s", (int)(dot - name), name);
            name = base;
            ESP_LOGI(TAG, "scene language: %s", i18n_language_code(lang));
        }
    }
    // "savecheck" -- block 5's acceptance: 200 edits through a save.
    if (strcmp(name, "savecheck") == 0) {
        s_content    = "savecheck";
        s_content_t0 = showtime_now();
        return run_savecheck();
    }
    // "replay" -- play replays/test.cmr (or the last recording) on a
    // scratch world: the reproducible walk the perf and shots tests want.
    // Options follow as _words, for measuring one feature against another
    // on the same walk: third, left, near / medium / far, nolight,
    // noclouds. They last for this run only; settings.txt is untouched.
    if (strcmp(name, "replay") == 0 || strncmp(name, "replay_", 7) == 0) {
        static char scene[48];
        snprintf(scene, sizeof(scene), "%s", name);
        s_content       = scene;
        s_content_t0    = showtime_now();
        s_force_third   = strstr(name, "_third") != NULL || strstr(name, "_left") != NULL;
        s_force_left    = strstr(name, "_left") != NULL;
        s_force_view    = strstr(name, "_near") ? 0 : strstr(name, "_medium") ? 1 : strstr(name, "_far") ? 2 : -1;
        s_force_noclouds = strstr(name, "_noclouds") != NULL;
        chunkmesh_set_lighting(strstr(name, "_nolight") == NULL);
        s_force_nolight = strstr(name, "_nolight") != NULL;
        return enter_replay();
    }
    // "flight" -- the scripted debug flight (fly_pose) over a scratch world
    // of the old flyover's seed: the scene the frame rates of 2026-09-21
    // were measured on (F-36, F-39), so today's build can be held against
    // them. Near view unless told otherwise; _nolight / _noclouds as for
    // the replays.
    // "farlands" -- walk up to the Far Lands wall (fly_pose), same options.
    if (strcmp(name, "flight") == 0 || strncmp(name, "flight_", 7) == 0 || strcmp(name, "farlands") == 0 ||
        strncmp(name, "farlands_", 9) == 0) {
        static char scene[48];
        s_fl_scene = strncmp(name, "farlands", 8) == 0;
        snprintf(scene, sizeof(scene), "%s", name);
        s_content        = scene;
        s_content_t0     = showtime_now();
        s_time_off       = s_content_t0;
        s_flying         = true;
        s_force_third    = false;
        s_force_left     = false;
        s_force_view     = strstr(name, "_medium") ? 1 : strstr(name, "_far") ? 2 : 0;
        s_force_noclouds = strstr(name, "_noclouds") != NULL;
        s_force_nolight  = strstr(name, "_nolight") != NULL;
        chunkmesh_set_lighting(!s_force_nolight);
        return enter_flight();
    }
    // "menu_<screen>" -- the title with one menu screen open over it.
    // An underscore, not a colon: the scene name is part of the shot's
    // file name, and FAT refuses a colon.
    if (strncmp(name, "menu_", 5) == 0) {
        if (s_app != APP_TITLE) {
            save_world("a test asked for a menu");
            enter_title();
        }
        if (!menu_show(name + 5)) return false;
        s_content    = "title";
        s_content_t0 = showtime_now();
        s_title_t0   = s_content_t0;
        return true;
    }
    for (size_t i = 0; i < sizeof(SCENES) / sizeof(SCENES[0]); i++) {
        if (strcmp(name, SCENES[i].name) != 0) continue;
        s_content    = SCENES[i].name;
        s_title_time = strcmp(name, "title_night") == 0 ? TITLE_TIME + DAY_TICKS / 2 + 3000 : TITLE_TIME;
        s_content_t0 = showtime_now();
        s_time_off   = s_content_t0;  // the content's own t = 0
        // The title is a pure function of ITS clock, so a test's t = 0
        // has to be the title's t = 0 or a shot hash means nothing.
        s_title_t0 = s_content_t0;
        s_flying     = true;
        if (SCENES[i].preset >= 0) {
            cm_view_t const v = cm_view_preset(SCENES[i].preset);
            chunk_render_set_view(&v);
            ESP_LOGI(TAG, "scene '%s': draw %d blocks, load radius %d, evict %d", SCENES[i].name, (int)v.draw_dist,
                     v.load_radius, v.evict_radius);
        }
        return true;
    }
    return false;
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
    int     flat_n = 0, tex_n = 0, lines_n = 0;
    int64_t unused_us = 0;
    scene_raster_stats(&flat_n, &lines_n, &unused_us, &unused_us);
    scene_textured_stats(&tex_n, &unused_us);
    ESP_LOGI(TAG,
             "world: %d chunks / %d sections drawn of %d resident (%d missing), %d tris tested -> %d submitted "
             "(flat %d/%d, textured %d/%d)",
             drawn, sections, resident, missing, tested, passed, flat_n, SE_SCENE_TRI_CAP, tex_n,
             SE_SCENE_TEXTURED_TRI_CAP);

    int     gen_n[2];
    int64_t gen_us[2];
    chunk_worker_gen_stats(&gen_n[0], &gen_us[0], &gen_n[1], &gen_us[1]);
    if (gen_n[0] + gen_n[1] > 0) {
        ESP_LOGI(TAG, "generated: %d ordinary chunks at %.1f ms, %d Far Lands at %.1f ms", gen_n[0],
                 gen_n[0] ? (double)gen_us[0] / 1000.0 / gen_n[0] : 0.0, gen_n[1],
                 gen_n[1] ? (double)gen_us[1] / 1000.0 / gen_n[1] : 0.0);
    }

    // A full list drops in submission order, so anything here is a hole
    // in the picture -- a corner of the world, a chunk, half a title.
    int dropped_tri = 0, dropped_ttri = 0;
    scene_drop_stats(&dropped_tri, &dropped_ttri);
    if (dropped_tri > 0 || dropped_ttri > 0) {
        ESP_LOGW(TAG, "GEOMETRY DROPPED: %d flat past the %d cap, %d textured past %d -- the view is incomplete",
                 dropped_tri, SE_SCENE_TRI_CAP, dropped_ttri, SE_SCENE_TEXTURED_TRI_CAP);
    }

    if (s_app == APP_PLAY) {
        ESP_LOGI(TAG, "player at %.2f %.2f %.2f%s", s_player.body.x, s_player.body.y, s_player.body.z,
                 phys_fits(&s_player.body, s_player.body.x, s_player.body.y, s_player.body.z) ? "" : " INSIDE A BLOCK");
    }

    // The streaming's flow, as rates. A world that lags behind the
    // camera looks the same whatever the cause: this says which it is.
    static chunk_worker_flow_t prev;
    static int                 prev_evicted;
    chunk_worker_flow_t        f;
    chunk_worker_flow(&f);
    int const evicted = chunk_render_evicted();
    ESP_LOGI(TAG, "stream/s: asked %d applied %d refused %d | loaded %d meshed %d saved %d evicted %d | queue %d/%d",
             f.asked - prev.asked, f.applied - prev.applied, f.refused - prev.refused, f.loaded - prev.loaded,
             f.meshed - prev.meshed, f.saved - prev.saved, evicted - prev_evicted, f.in_flight, f.capacity);
    prev         = f;
    prev_evicted = evicted;

    // What the meshes are holding. The 8 MiB chunk slab is fixed at
    // boot; this is the part that grows with the view distance.
    int          mesh_n = 0, chunk_n = 0;
    size_t const mesh_bytes = chunk_store_mesh_bytes(&mesh_n, &chunk_n);
    ESP_LOGI(TAG, "psram: meshes %u KiB in %d of %d built (%d chunks resident) | slab %u KiB | %u KiB free",
             (unsigned)(mesh_bytes / 1024), mesh_n, chunk_n * CH_MESH_N, chunk_n,
             (unsigned)(chunk_store_bytes() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    s_window_us = 0;
    s_frames    = 0;
}

// --- Callbacks ----------------------------------------------------------

// Once, after the engine has booted the display, audio, input and scene.
static void on_init(void* user) {
    (void)user;
    ESP_LOGI(TAG, "CraftMiner on SynthEngine3D %s", se_version_string());
    devtest_start(&TEST);

    // THE ENGINE'S OWN SPLASH, with its version -- the first thing the
    // program shows. `se_splash()` rather than `se_splash_ex()`,
    // because the default subtitle is "Version <se_version_string()>"
    // and tracks the engine instead of going stale in a string here.
    //
    // The "CraftMiner" card that used to be here was a placeholder and
    // is gone: the game's own title belongs on the title screen (step
    // 5.1), not on a second text splash the player has to sit through.
    se_splash();

    // The sun is set every frame from the time of day (on_render); this
    // is only what the first frame sees.
    se_light_set(&(se_light_t){.x = -600.0f, .y = 900.0f, .z = -400.0f, .brightness = 0.45f, .two_sided = false});

    // Both output-neutral, both off by default. Frustum culling is a
    // near-pure win. Depth ordering trades a sort against overdraw, and
    // a voxel view is exactly the dense case where it pays -- the
    // showreel measured a canopy shot at 220 -> 175 ms (F-03).
    scene_set_options(&(se_scene_options_t){.frustum_cull = true, .depth_order = true});

    // --- The world ---------------------------------------------------
    log_memory("engine booted");
    log_memory_map();
    membench_run();

    if (!chunk_store_init()) {
        ESP_LOGE(TAG, "chunk_store_init failed (%u KiB)", (unsigned)(chunk_store_bytes() / 1024));
        return;
    }
    ESP_LOGI(TAG, "chunk slab: %u KiB (%d slots x %u KiB)", (unsigned)(chunk_store_bytes() / 1024), CH_SLOT_COUNT,
             (unsigned)(chunk_store_bytes() / CH_SLOT_COUNT / 1024));
    // Torchlight and daylight need their flood queues. Without them the
    // world is simply drawn fully lit.
    if (!light_init()) ESP_LOGW(TAG, "no light queues: the world will be fully lit");
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
            s_half_ok = false;
        }
    } else {
        ESP_LOGW(TAG, "PPA unavailable; drawing at full resolution");
        s_half_ok = false;
    }

    // Textures, then the material tables that map blocks onto them.
    static char tex_dir[160];
    snprintf(tex_dir, sizeof(tex_dir), "%s/textures", graceloader_get_install_basepath());
    texcache_init(tex_dir);
    if (!chunk_render_init()) {
        ESP_LOGE(TAG, "chunk_render_init failed");
        return;
    }
    fred_init();  // after the block textures: a block in his hand uses them
    // The player's graphics and audio choices. The view distance itself
    // is applied on entering a world: the title has a view of its own.
    // Bindings registered first: the settings file restores them.
    input_init();
    // THE PLAYER'S DATA lives in /sd/craftminer, where the launcher cannot
    // empty it on an update (datadir.h). Earlier builds kept it in the
    // install directory; it moves across here, before anything reads it.
    {
        char      report[1024];
        int const moved = datadir_adopt(graceloader_get_install_basepath(), CM_DATA_DIR, report, sizeof(report));
        if (moved < 0) ESP_LOGE(TAG, "could not create %s", CM_DATA_DIR);
        for (char* line = report; *line != '\0';) {
            char* const end = strchr(line, '\n');
            if (end != NULL) *end = '\0';
            ESP_LOGI(TAG, "data: %s", line);
            if (end == NULL) break;
            line = end + 1;
        }
    }
    settings_load(CM_DATA_DIR);
    // settings.txt named the language; this is where a player's own
    // corrections to that language, if they have put any on the card,
    // come in over the baked-in text (i18n.h).
    i18n_load_overrides(CM_DATA_DIR);
    ESP_LOGI(TAG, "language: %s (%s)", i18n_language_code(i18n_language()),
             i18n_language_name(i18n_language()));
    // The speaker, now that settings.txt has said whether the player
    // wants music and effects. Failing to start is not fatal: the game
    // runs silent (audio.h).
    cm_audio_init();
    // The launcher's key-cap PNGs, for the Controls menu (synthracer's
    // icons.c). Missing ones fall back to a text label.
    icons_load();
    chunk_render_set_textured(settings_textured());
    chunk_render_set_view(&(cm_view_t){0});
    cm_view_t const v = cm_view_preset(view_setting());
    chunk_render_set_view(&v);
    texcache_report();
    log_memory("textures loaded");

    // The worlds on the card.
    if (!worldstore_init(CM_DATA_DIR)) {
        ESP_LOGE(TAG, "worldstore_init failed");
        return;
    }
    // THE TESTWORLD. Builds before save slots kept one fixed world in
    // worlds/flyover, and people have been playing in it. It moves into
    // the first slot, renamed, the first time this build starts -- and
    // after that there is no flyover to find, so this is a no-op on
    // every later start and on every card that never had it.
    int const adopted = worldstore_adopt_legacy("flyover", "Testworld");
    if (adopted >= 0) {
        ESP_LOGI(TAG, "the world from before save slots is now slot %d, \"Testworld\"", adopted + 1);
    } else if (adopted == -2) {
        ESP_LOGE(TAG, "found the pre-slots world but could not move it; it is untouched in worlds/flyover");
    }

    if (!chunk_worker_start(0)) {
        ESP_LOGE(TAG, "chunk_worker_start failed");
        return;
    }
    ESP_LOGI(TAG, "chunk worker running, %s", chunk_worker_synchronous() ? "SYNCHRONOUS" : "on core 1");

    // The title, on its own scratch world. A real one is opened when
    // the player picks it.
    if (!enter_title()) {
        ESP_LOGE(TAG, "could not open the title world");
        return;
    }

    log_memory("title ready");
}

// --- Changing worlds ------------------------------------------------------
//
// The title runs on a scratch world and the game on a real one, so
// moving between them swaps the world under the chunk store. Both
// directions do the same three things in the same order, and the order
// is the whole content of this: DRAIN the worker first, because a load
// still in flight would land in a slot that is about to be freed; then
// clear the store, because the title's terrain must not still be in
// the ring when the player spawns; only then open the new world.

static world_meta_t   s_meta;
static player_state_t s_saved;
static world_items_t  s_items;  // what lies on the ground, as saved and loaded

static void drain_and_clear(void) {
    // Synchronous mode drains the queues as part of switching, which is
    // exactly the guarantee needed here.
    bool const was_async = !chunk_worker_synchronous();
    chunk_worker_set_synchronous(true);
    chunk_store_clear();
    if (was_async) chunk_worker_set_synchronous(false);
}

static bool enter_title(void) {
    s_in_replay = false;
    // Stop mid-stride: a footstep left ringing across the world change,
    // and a step accumulator carried into the next world, would both be
    // heard (audio.h).
    cm_audio_leave_world();
    drain_and_clear();
    worldstore_close();
    if (!title_begin()) return false;
    chunk_worker_set_world(title_seed(), FARLANDS_X_DEFAULT);
    cm_view_t const tv = title_view();
    chunk_render_set_view(&tv);

    // Everything the drift will look at, before the first letter is
    // written. The path is short and the view radius covers all of it
    // from the middle, so one point is enough.
    double px, pz;
    title_stream_at(0.5 * 16.0, &px, &pz);  // the middle of the loop
    s_cam_mode = CAM_PLAYER;
    menu_close();  // opened when the loading is done
    start_loading(px, pz, APP_TITLE, T(CM_STR_LOADING_PLAIN), LOAD_GATE_ALL);
    return true;
}

// Open the world in `slot` -- or, with `create`, make one there first.
static bool enter_world(int slot, bool create, char const* name, uint32_t seed) {
    s_in_replay = false;
    drain_and_clear();
    title_end();

    char slug[CM_WORLD_SLUG_MAX];
    worldstore_slot_slug(slot, slug, sizeof(slug));
    bool const ok = create ? worldstore_create_in(slot, name, seed, &s_meta, &s_saved)
                           : worldstore_open(slug, &s_meta, &s_saved, &s_items);
    if (!ok) {
        ESP_LOGE(TAG, "could not %s the world in slot %d", create ? "create" : "open", slot + 1);
        enter_title();
        menu_status(create ? "Could not create the world" : "Could not open that world");
        return false;
    }
    ESP_LOGI(TAG, "world \"%s\" (slot %d) %s, seed %u", s_meta.name, slot + 1, create ? "created" : "opened",
             (unsigned)s_meta.seed);

    chunk_worker_set_world(s_meta.seed, s_meta.farlands_x);
    // The player's own view distance: the title's is generous because
    // it is looking at one static word, not walking.
    cm_view_t const pv = cm_view_preset(view_setting());
    chunk_render_set_view(&pv);
    // What was lying on the ground when they left.
    item_entity_restore(s_items.e, create ? 0 : s_items.n);

    // WHO THEY WERE. Health, hunger and what they carry come back from
    // the save; a player with nothing saved gets the starting kit.
    player_reset(&s_player);
    s_player.health = s_saved.health;
    s_player.hunger = s_saved.hunger;
    if (s_saved.has_inv) {
        memcpy(s_player.inv.slot, s_saved.inv, sizeof(s_player.inv.slot));
        memcpy(s_player.inv.seen, s_saved.seen, sizeof(s_player.inv.seen));
        s_player.inv.selected = (int)s_saved.inv_selected;
    }
    s_player.inv.open = false;

    // WHERE THEY WERE, exactly, if they were anywhere. Settled on the
    // first frame their chunk is resident (on_update), because until
    // then there is nothing to test the position against.
    phys_body_init(&s_player.body, s_saved.x, s_saved.y, s_saved.z);
    s_player.yaw        = s_saved.yaw;
    s_player.pitch      = s_saved.placed ? s_saved.pitch : 0.0f;
    s_player.prev_x     = s_saved.x;
    s_player.prev_y     = s_saved.y;
    s_player.prev_z     = s_saved.z;
    s_player.prev_yaw   = s_player.yaw;
    s_player.prev_pitch = s_player.pitch;
    s_player_ready      = false;
    tick_reset(&s_tick, showtime_now());
    // Frozen until the chunk under them is resident: nobody falls
    // through terrain that has not arrived yet (D-26).
    tick_freeze(&s_tick, true);
    s_cam_mode = CAM_PLAYER;
    menu_close();
    // The ground under the player before the player is on it (D-26),
    // behind a progress bar (5.5). The rest streams in behind them while
    // they are already walking, which is what the freeze above covers.
    start_loading(s_saved.x, s_saved.z, APP_PLAY, T(create ? CM_STR_LOADING_CREATING : CM_STR_LOADING_WORLD),
                  create ? LOAD_GATE_ALL : LOAD_GATE_3X3);
    ESP_LOGI(TAG, "entering at %.1f, %.1f, %.1f (%s)", s_saved.x, s_saved.y, s_saved.z,
             s_saved.placed ? "where they left" : "a new player");
    return true;
}

// Finish a recording and write it to replays/last.cmr.
static void stop_recording(void) {
    if (!replay_recording()) return;
    char dir[160], path[192];
    snprintf(dir, sizeof(dir), "%s/replays", CM_DATA_DIR);
    cm_mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/last.cmr", dir);
    bool const ok = replay_record_end(path);
    ESP_LOGI(TAG, "replay recording stopped: %s %s", path, ok ? "written" : "NOT WRITTEN");
}

// THE SAVE CHECK (block 5's acceptance): make a world, edit 200 blocks
// across three chunks, save it, throw everything away, open it again and
// count the edits that came back. In a world of its own -- "savecheck",
// outside the slots, so no menu ever shows it -- deleted afterwards
// whatever the result. `make cycle TEST="perf scene=savecheck secs=3"`:
// a SAVECHECK record carries the count, and a miss ends the test "bad".
#define SAVECHECK_EDITS 200

static void settle_world(double wx, double wz);

static bool run_savecheck(void) {
    static struct {
        int32_t x, y, z;
        uint8_t b, st;
    } e[SAVECHECK_EDITS];
    static uint8_t const CYCLE[5] = {BLK_GLASS, BLK_AIR, BLK_PLANKS, BLK_TORCH, BLK_COBBLE};

    drain_and_clear();
    title_end();
    worldstore_close();
    worldstore_delete("savecheck");  // whatever a crashed run left
    world_meta_t   m;
    player_state_t p;
    if (!worldstore_create("savecheck", 0x5AFE5AFEu, &m, &p)) {
        devtest_content_failed("could not create the savecheck world");
        return true;
    }
    chunk_worker_set_world(m.seed, m.farlands_x);
    chunk_worker_set_synchronous(true);
    settle_world(8.0, 8.0);

    // Three chunks, a spread of heights, placing and removing both.
    for (int i = 0; i < SAVECHECK_EDITS; i++) {
        int32_t const bx = (i % 3 == 1) ? CH_W : 0, bz = (i % 3 == 2) ? CH_D : 0;
        e[i].x           = bx + (i * 7) % CH_W;
        e[i].z           = bz + (i * 11) % CH_D;
        e[i].y           = 12 + (i * 5) % 40;
        uint8_t const b  = CYCLE[i % 5];
        world_set(e[i].x, e[i].y, e[i].z, b, b == BLK_AIR ? 0 : ST_PLACED);
    }
    // What the world says now is what has to come back -- a later edit
    // may land on an earlier one's cell.
    for (int i = 0; i < SAVECHECK_EDITS; i++) {
        e[i].b  = world_block(e[i].x, e[i].y, e[i].z);
        e[i].st = world_state(e[i].x, e[i].y, e[i].z);
    }

    int saved = 0;
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t const* c = chunk_slot_at(i);
        if (c->cstate == CS_READY && (c->flags & CF_EDITED) != 0 && chunk_worker_request_save(c->cx, c->cz)) saved++;
    }
    bool const level_ok = worldstore_save(&m, &p, NULL);

    // Everything gone from memory, then back from the card.
    drain_and_clear();
    worldstore_close();
    int survived = 0;
    if (worldstore_open("savecheck", &m, &p, NULL)) {
        chunk_worker_set_world(m.seed, m.farlands_x);
        settle_world(8.0, 8.0);
        for (int i = 0; i < SAVECHECK_EDITS; i++) {
            if (world_block(e[i].x, e[i].y, e[i].z) == e[i].b && world_state(e[i].x, e[i].y, e[i].z) == e[i].st)
                survived++;
        }
    }
    report_emitf("SAVECHECK", "{\"t\":\"savecheck\",\"edits\":%d,\"survived\":%d,\"chunks_saved\":%d,\"level\":%s}",
                 SAVECHECK_EDITS, survived, saved, level_ok ? "true" : "false");
    ESP_LOGI(TAG, "savecheck: %d of %d edits survived (%d chunks saved, level.cmw %s)", survived, SAVECHECK_EDITS,
             saved, level_ok ? "written" : "FAILED");
    if (survived != SAVECHECK_EDITS || !level_ok) devtest_content_failed("edits were lost across a save and reload");

    drain_and_clear();
    worldstore_close();
    worldstore_delete("savecheck");
    enter_title();
    return true;
}

// The debug flight on a scratch world of seed 0xC0FFEE -- what the old
// fixed "flyover" world was -- for comparing frame rates with the builds
// that measured on it. The camera is the scripted one (a test is running,
// and this is not a replay).
static bool enter_flight(void) {
    drain_and_clear();
    title_end();
    worldstore_open_scratch(0xC0FFEEu, &s_meta, &s_saved);
    s_meta.time_of_day = TITLE_TIME;  // a morning, as the old builds always were
    chunk_worker_set_world(0xC0FFEEu, s_meta.farlands_x);
    cm_view_t const pv = cm_view_preset(view_setting());
    chunk_render_set_view(&pv);
    item_entity_reset();
    player_reset(&s_player);
    double wx, wz;
    float  yaw;
    fly_pose(0.0, &wx, &wz, &yaw);
    phys_body_init(&s_player.body, wx, 40.0, wz);
    s_player_ready = false;
    s_in_replay    = false;
    s_cam_mode     = CAM_PLAYER;
    menu_close();
    // The debug flight is a camera, not a player: it can be anywhere in
    // the view next frame, so it waits for all of it.
    start_loading(wx, wz, APP_PLAY, s_fl_scene ? "Loading the Far Lands" : "Loading flight", LOAD_GATE_ALL);
    return true;
}

// Play a replay: its seed as a scratch world -- nothing saved, nobody's
// world touched -- the player where the recording started, carrying what
// they carried then.
static bool enter_replay(void) {
    char           path[192];
    replay_start_t st;
    snprintf(path, sizeof(path), "%s/replays/test.cmr", CM_DATA_DIR);
    if (!replay_load(path, &st)) {
        snprintf(path, sizeof(path), "%s/replays/last.cmr", CM_DATA_DIR);
        if (!replay_load(path, &st)) {
            ESP_LOGW(TAG, "no replay to play (replays/test.cmr or replays/last.cmr)");
            return false;
        }
    }
    ESP_LOGI(TAG, "replay %s: %d ticks, seed %u", path, replay_length(), (unsigned)st.seed);
    drain_and_clear();
    title_end();
    worldstore_open_scratch(st.seed, &s_meta, &s_saved);
    s_meta.time_of_day = st.time_of_day;
    chunk_worker_set_world(st.seed, s_meta.farlands_x);
    cm_view_t const pv = cm_view_preset(view_setting());
    chunk_render_set_view(&pv);
    item_entity_reset();
    player_reset(&s_player);
    memcpy(s_player.inv.slot, st.inv, sizeof(s_player.inv.slot));
    s_player.inv.selected = (int)st.selected;
    s_player.inv.open     = false;
    s_saved.x = st.x, s_saved.y = st.y, s_saved.z = st.z, s_saved.yaw = st.yaw, s_saved.pitch = st.pitch;
    s_saved.placed = true;
    phys_body_init(&s_player.body, st.x, st.y, st.z);
    s_player.yaw = s_player.prev_yaw = st.yaw;
    s_player.pitch = s_player.prev_pitch = st.pitch;
    s_player.prev_x = st.x, s_player.prev_y = st.y, s_player.prev_z = st.z;
    s_player_ready = false;
    input_feed(0);
    tick_reset(&s_tick, showtime_now());
    tick_freeze(&s_tick, true);
    s_cam_mode = CAM_PLAYER;
    menu_close();
    // A replay must run against the same world every time, so it waits
    // for the whole view rather than starting on nine chunks.
    start_loading(st.x, st.z, APP_PLAY, "Loading replay", LOAD_GATE_ALL);
    s_in_replay = true;
    return true;
}

// Write everything the open world owns: the player, and every resident
// chunk that has been edited. NEVER on a tick (Part N) -- only here, on
// an explicit save, on leaving, and on eviction.
static void save_world(char const* why) {
    if (s_app != APP_PLAY) return;

    if (s_player_ready) {
        s_saved.x     = s_player.body.x;
        s_saved.y     = s_player.body.y;
        s_saved.z     = s_player.body.z;
        s_saved.yaw   = s_player.yaw;
        s_saved.pitch = s_player.pitch;
    }
    s_saved.health = s_player.health;
    s_saved.hunger = s_player.hunger;
    // Only once they have stood somewhere real: saving during the
    // entering freeze must not turn the spawn guess into a position.
    s_saved.placed = s_saved.placed || s_player_ready;
    s_saved.has_inv = true;
    memcpy(s_saved.inv, s_player.inv.slot, sizeof(s_saved.inv));
    memcpy(s_saved.seen, s_player.inv.seen, sizeof(s_saved.seen));
    s_saved.inv_selected = s_player.inv.selected;
    int64_t const now    = (int64_t)time(NULL);
    if (now > 0) s_meta.last_played = now;

    int chunks = 0;
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t const* c = chunk_slot_at(i);
        if (c->cstate != CS_READY || (c->flags & CF_EDITED) == 0) continue;
        if (chunk_worker_request_save(c->cx, c->cz)) chunks++;
    }
    // The saves were queued; take delivery of them all before claiming
    // the world is on the card.
    while (!chunk_worker_idle()) chunk_worker_collect(64);

    s_items.n     = item_entity_copy(s_items.e, ITEM_ENTITY_MAX);
    bool const ok = worldstore_save(&s_meta, &s_saved, &s_items);
    ESP_LOGI(TAG, "saved (%s): %d chunk(s), level.cmw %s", why, chunks, ok ? "written" : "FAILED");
    menu_status(ok ? "Saved" : "SAVING FAILED");
}

// --- Loading ---------------------------------------------------------------
//
// Every chunk the next screen will look at is generated BEFORE it
// starts (D-57): the title writes its letters into the world and a
// player must not stand on terrain that is still arriving. The streamer
// asks for four chunks a frame on purpose -- it is built so that walking
// never stalls -- but "never stalls" and "is complete" are different
// promises, and an opening needs the second one.
//
// So the worker runs inline and the streamer is driven to completion --
// a slice of it each frame, with a frame drawn in between, so the player
// watches a progress bar fill instead of a frozen screen (5.5). A new
// world's spawn is 56 ms a chunk to generate (F-23), which is seconds.

#define LOAD_SLICE_US 60000  // generation per frame; the bar redraws in between
#define LOAD_GUARD    2000   // streamer rounds before giving up on "complete"

static struct {
    double      wx, wz;
    app_state_t next;
    char const* what;
    int64_t     t0;
    int         rounds;
    float       progress;  // 0..1, for the bar
    load_gate_t gate;
} s_load;

static void start_loading(double wx, double wz, app_state_t next, char const* what, load_gate_t gate) {
    // A test that sets the clock needs the whole view present, because a
    // frame it photographs may look anywhere (D-59). Its reproducibility
    // beats a fast entry it is not timing.
    if (devtest_deterministic()) gate = LOAD_GATE_ALL;
    s_load =
        (typeof(s_load)){.wx = wx, .wz = wz, .next = next, .what = what, .t0 = esp_timer_get_time(), .gate = gate};
    chunk_worker_set_synchronous(true);
    s_app = APP_LOADING;
}


static void loading_step(void) {
    int64_t const t0      = esp_timer_get_time();
    int           missing = 0, resident = 0;
    bool          ready   = false;
    int           nine    = 0;
    do {
        chunk_render_stream(s_load.wx, s_load.wz);
        chunk_render_stats(NULL, NULL, &resident, &missing);
        s_load.rounds++;
        if (s_load.gate == LOAD_GATE_3X3) {
            nine  = chunk_render_nine(s_load.wx, s_load.wz);
            ready = nine == 9;
        } else {
            ready = missing == 0;
        }
        // A test that sets the clock renders a frame or two per moment:
        // for it, loading is one step, however long it takes.
    } while (!ready && s_load.rounds < LOAD_GUARD &&
             (devtest_deterministic() || esp_timer_get_time() - t0 < LOAD_SLICE_US));
    // The bar measures what is being WAITED for, not what will eventually
    // arrive: gated on nine chunks it must not crawl across the whole
    // view distance and then jump.
    if (s_load.gate == LOAD_GATE_3X3) {
        s_load.progress = (float)nine / 9.0f;
    } else {
        s_load.progress = resident + missing > 0 ? (float)resident / (float)(resident + missing) : 1.0f;
    }
    if (!ready && s_load.rounds < LOAD_GUARD) return;

    // Done. Back to streaming on core 1 -- unless a test wants the
    // world to be exactly reproducible (D-59).
    chunk_worker_set_synchronous(devtest_deterministic());
    ESP_LOGI(TAG, "loaded (%s): %d chunks resident (%d missing) in %d rounds, %lld ms%s", s_load.what, resident,
             missing, s_load.rounds, (long long)((esp_timer_get_time() - s_load.t0) / 1000),
             ready ? (s_load.gate == LOAD_GATE_3X3 && missing > 0 ? " -- the rest streams in behind the player" : "")
                   : " (INCOMPLETE)");
    s_app = s_load.next;
    if (s_app == APP_TITLE) {
        // A test that picked a moment of the title keeps its own clock.
        if (!devtest_running()) s_title_t0 = showtime_now();
        if (!menu_active()) menu_open_title();
        ESP_LOGI(TAG, "title");
    } else {
        tick_reset(&s_tick, showtime_now());
        // A replay's first tick is now -- or, under a test, the moment the
        // test called t = 0, so its clock and the replay's agree.
        s_replay_t0 = devtest_running() ? s_content_t0 : showtime_now();
    }
}

// The loading screen: what is happening, and how far along it is.
static void draw_loading(pax_buf_t* fb) {
    pax_background(fb, 0xFF14181Eu);
    float const       w   = (float)DISPLAY_LOG_W, h = (float)DISPLAY_LOG_H;
    char const* const msg = s_load.what != NULL ? s_load.what : T(CM_STR_LOADING_PLAIN);
    pax_vec2f const   sz  = rendertext_size(NULL, 30.0f, msg);
    rendertext_draw(fb, 0xFFFFFFFFu, NULL, 30.0f, (w - sz.x) * 0.5f, h * 0.40f, msg);
    // The world's name -- not a scratch world's placeholder.
    if (s_load.next == APP_PLAY && s_meta.name[0] != '\0' && s_meta.name[0] != '(') {
        pax_vec2f const nz = rendertext_size(NULL, 18.0f, s_meta.name);
        rendertext_draw(fb, 0xFFA0A8B0u, NULL, 18.0f, (w - nz.x) * 0.5f, h * 0.40f + 42.0f, s_meta.name);
    }
    float const bw = 420.0f, bh = 14.0f, bx = (w - bw) * 0.5f, by = h * 0.62f;
    pax_simple_rect(fb, 0xFF3A4048u, bx, by, bw, bh);
    pax_simple_rect(fb, 0xFF6CC24Au, bx, by, bw * s_load.progress, bh);
}

// Fill the world in before drawing, for a test that must be exactly
// reproducible.
//
// Synchronous loading alone is not enough: the streamer asks for four
// chunks a frame on purpose, so a `shots` run -- which renders a
// handful of frames at a clock it SET -- would photograph whichever
// quarter of the world had arrived. That is not "the world at t", it
// is "the world at t on this machine on this day", and hashing it
// would be worse than not hashing it at all.
//
// So when determinism is asked for, the streamer is run to completion
// first. Slow, and irrelevant: a shots run is not measuring time.
static void settle_world(double wx, double wz) {
    for (int guard = 0; guard < 600; guard++) {
        chunk_render_stream(wx, wz);
        int missing = 0;
        chunk_render_stats(NULL, NULL, NULL, &missing);
        if (missing == 0) return;
    }
    ESP_LOGW(TAG, "the world would not settle; a shot will be incomplete");
}

// The ground under the player has to exist before they may fall
// through it (D-26). Frozen until the chunk they are standing in is
// resident, which on entering a world is the first thing that arrives.
//
// BEFORE the frame's ticks, not after: settling puts the player back at
// their saved position, and done after the ticks it would undo them. One
// frame of that is invisible in play; a test that jumps the clock runs a
// hundred ticks in that frame, and lost them all.
static void settle_player(cam_mode_t mode) {
    if (mode != CAM_PLAYER || menu_active()) return;
    bool const standing =
        chunk_find(chunk_of((int32_t)floor(s_player.body.x)), chunk_of((int32_t)floor(s_player.body.z))) != NULL;
    tick_freeze(&s_tick, !standing);
    if (standing && !s_player_ready) {
        // The terrain is here, so the position can be settled: put a
        // returning player back exactly where they left, and a new one --
        // or one whose spot is now inside something -- on the ground at
        // their column.
        bool const exact =
            s_saved.placed && player_place(&s_player, s_saved.x, s_saved.y, s_saved.z, s_saved.yaw, s_saved.pitch);
        if (!exact) player_spawn(&s_player, s_saved.x, s_saved.z, s_player.yaw);
        s_player_ready = true;
        ESP_LOGI(TAG, "player standing at %.1f, %.1f, %.1f", s_player.body.x, s_player.body.y, s_player.body.z);
    }
}

// Per frame. `dt` is seconds since the last frame, already clamped --
// unused for now: the content is drawn from the show clock instead.
static void on_update(float dt, void* user) {
    (void)user;
    showtime_frame();
    devtest_update();
    // The music's long silences are counted in real seconds, not ticks:
    // it is not part of the world and a paused game should not freeze
    // mid-piece. Reads the card when a new piece is due, which is why it
    // is here on the game thread and not in the mixer (music.h).
    cm_audio_frame(dt);

    // Generating what the next screen needs, a slice a frame.
    if (s_app == APP_LOADING) {
        loading_step();
        // Still loading: nothing else to do. Just finished: carry on with
        // this frame, or it is drawn with a camera that was never set --
        // which is the frame a test that sets the clock photographs.
        if (s_app == APP_LOADING) return;
    }

    // A `shots` test SETS the clock instead of running it, so a frame
    // has to be able to draw a world that arrived in no time at all.
    // Synchronous chunk loading is what D-15 put there for exactly
    // this: generation happens inline, the frame waits, and the picture
    // is of the world rather than of the sky it had not loaded yet
    // (F-45). Slow -- 56 ms a chunk -- and that is fine, because a
    // shots run is not measuring time.
    {
        bool const want_sync = devtest_deterministic();
        if (want_sync != chunk_worker_synchronous()) chunk_worker_set_synchronous(want_sync);
    }

    // The crafting book. A screen like the menus, but it changes only
    // the inventory, so it needs no command back: it does its own work
    // and main.c's part is to freeze the player while it is up.
    if (craft_ui_active()) craft_ui_update(&s_player.inv);
    // The furnace runs on the world's own clock, not on wall time: it
    // never ticks, it catches up (game/furnace.h).
    if (furnace_ui_active()) furnace_ui_update(&s_player.inv, (uint32_t)s_meta.time_of_day);
    // The trashcan empties by the same clock, and for the same reason.
    if (chest_ui_active()) chest_ui_update(&s_player.inv, (uint32_t)s_meta.time_of_day);
    if (bench_ui_active()) bench_ui_update(&s_player.inv);
    s_player.ui_open = craft_ui_active() || furnace_ui_active() || chest_ui_active() || bench_ui_active();

    // The menus. Whatever changes the world or the game's running state
    // comes back as a command and is acted on here, in one place.
    if (menu_active()) {
        menu_cmd_t const cmd = menu_update();
        switch (cmd.kind) {
            case MENU_CMD_PLAY: enter_world(cmd.slot, false, NULL, 0); break;
            case MENU_CMD_CREATE: enter_world(cmd.slot, true, cmd.name, cmd.seed); break;
            case MENU_CMD_RESUME:
                menu_close();
                // Owed nothing for the time spent in the menu.
                tick_reset(&s_tick, showtime_now());
                break;
            case MENU_CMD_SAVE: save_world("from the pause menu"); break;
            case MENU_CMD_SAVE_QUIT:
                stop_recording();
                replay_stop();
                save_world("quitting to the title");
                enter_title();
                break;
            case MENU_CMD_LEAVE:
                ESP_LOGI(TAG, "leaving for the launcher");
                cm_audio_shutdown();  // a speaker left running across the restart squeals
                bsp_device_restart_to_launcher();
                break;
            case MENU_CMD_GRAPHICS:
                chunk_render_set_textured(settings_textured());
                if (s_app == APP_PLAY) {
                    cm_view_t const v = cm_view_preset(view_setting());
                    chunk_render_set_view(&v);
                }
                break;
            default: break;
        }
    }

    // The title has its own camera and its own world. It streams like
    // any other, which is the point: it is a real view of the game.
    if (s_app == APP_TITLE) {
        double const       t = showtime_now() - s_title_t0;
        title_view_t const v = title_camera(t);
        // The chunks the letters stand in must exist before a letter
        // can be written into one: world_set() no-ops on a chunk that
        // is not resident. Live, the title retries every frame and they
        // fill in within a second; for a shot there is only one frame,
        // so the world is settled first.
        if (devtest_deterministic()) settle_world(v.wx, v.wz);
        title_update(t);

        s_cam.wx        = v.wx;
        s_cam.wy        = v.wy;
        s_cam.wz        = v.wz;
        s_cam.yaw       = v.yaw;
        s_cam.pitch     = v.pitch;
        s_cam_effective = CAM_SCRIPTED;  // nothing of the player's is drawn
        s_ticks_last_frame = 0;
        chunk_worker_collect(CHUNK_RESULTS_PER_FRAME);
        chunk_render_stream(s_cam.wx, s_cam.wz);
        return;
    }

    cam_mode_t const mode = devtest_running() && !s_in_replay ? CAM_SCRIPTED : s_cam_mode;
    s_cam_effective       = mode;
    settle_player(mode);

    // Where the camera will be this frame decides what has to exist.
    if (mode == CAM_SCRIPTED) {
        float yaw;
        fly_pose(fly_time(), &s_cam.wx, &s_cam.wz, &yaw);
        s_cam.yaw        = yaw;
        s_cam.pitch      = s_fl_scene ? -0.12f : FLY_PITCH;  // the wall wants looking up at
        // Follows the ground, so the flight stays over the terrain
        // rather than through it.
        int const ground = world_ground((int32_t)floor(s_cam.wx), (int32_t)floor(s_cam.wz));
        s_cam.wy         = (float)(ground > 0 ? ground : CH_SEA_LEVEL) + FLY_EYE_H;
        s_ticks_last_frame = 0;
    } else if (mode == CAM_FREE) {
        if (!s_free_ready) {
            // Start where the player is, so switching to the free
            // camera looks at what the player was looking at.
            flycam_reset(&s_free, s_player.body.x, s_player.body.z, (float)s_player.body.y + PHYS_PLAYER_EYE,
                         s_player.yaw);
            s_free.placed = true;
            s_free_ready  = true;
        }
        flycam_update(&s_free, dt);
        s_cam.wx    = s_free.wx;
        s_cam.wz    = s_free.wz;
        s_cam.wy    = s_free.wy;
        s_cam.yaw   = s_free.yaw;
        s_cam.pitch = s_free.pitch;
        s_ticks_last_frame = 0;
    } else if (menu_active()) {
        // Paused. The world holds still and the camera with it; chunks
        // keep streaming below, so nothing is missing on resume.
        s_ticks_last_frame = 0;
    } else {
        // THE SIMULATION. A fixed number of whole 20 Hz ticks, from the
        // show clock so the testkit can drive it; the frame then draws
        // between the last two (D-02).
        // Turning the badge, added up over the frame; the ticks below
        // hand it to the look along with the cursor keys. Only while the
        // player is actually looking round -- not reading the inventory,
        // not frozen waiting for ground.
        input_gyro_frame(dt, settings_gyro() && s_player_ready && !s_player.inv.open && !replay_playing());
        int n = tick_due(&s_tick, showtime_now());
        if (replay_playing() && devtest_deterministic()) {
            // A test that SETS the clock gets exactly the ticks that
            // belong to that moment, however many -- not five a frame
            // with the rest forgiven.
            int const want = (int)((showtime_now() - s_replay_t0) * (double)TICK_HZ);
            n              = want > replay_position() ? want - replay_position() : 0;
        }
        for (int i = 0; i < n; i++) {
            cm_actions_t mask;
            if (replay_playing()) {
                uint32_t m  = 0;
                float    gy = 0.0f, gp = 0.0f;
                if (!replay_next(&m, &gy, &gp)) ESP_LOGI(TAG, "replay finished after %d ticks", replay_length());
                mask = input_feed(m);
                input_gyro_set_owed(gy, gp);
            } else {
                mask = input_sample();
                if (replay_recording()) {
                    float gy = 0.0f, gp = 0.0f;
                    input_gyro_owed(&gy, &gp);
                    replay_record_tick(mask, gy, gp);
                }
            }
            player_tick(&s_player, mask, input_pressed());
            cm_audio_player_tick(&s_player);  // footsteps and landings, AFTER the tick
            // A block the player opened. The registry says WHICH blocks
            // open something (BF2_USABLE); what each one opens is here.
            // Only if it is not already showing: OPENING a screen
            // resets its cursor and empties its search box, so a
            // repeated "use" must not reach it. player.c clears
            // used_block every tick now, and this is the second lock on
            // the same door -- it was worth two.
            if (s_player.used_block == BLK_CRAFTING_TABLE && !craft_ui_active()) {
                s_player.inv.open = false;
                craft_ui_open(RS_TABLE);
            } else if (s_player.used_block == BLK_FURNACE && !furnace_ui_active()) {
                s_player.inv.open = false;
                furnace_ui_open(s_player.aim.x, s_player.aim.y, s_player.aim.z);
            } else if ((s_player.used_block == BLK_CHEST || s_player.used_block == BLK_TRASH) &&
                       !chest_ui_active()) {
                s_player.inv.open = false;
                chest_ui_open(s_player.aim.x, s_player.aim.y, s_player.aim.z);
            } else if (s_player.used_block == BLK_BENCH && !bench_ui_active()) {
                s_player.inv.open = false;
                bench_ui_open();
            }
            s_meta.time_of_day++;  // the world's clock is its own ticks (D-51)
        }
        s_ticks_last_frame = n;

        double x, y, z;
        float  yaw, pitch;
        player_eye(&s_player, tick_alpha(&s_tick), &x, &y, &z, &yaw, &pitch);
        s_cam.wx    = x;
        s_cam.wy    = (float)y;
        s_cam.wz    = z;
        s_cam.yaw   = yaw;
        s_cam.pitch = pitch;

        // Fred's animation: a stride that follows how fast he is going,
        // a walk cycle that runs with it, and a swing while he mines.
        float const speed = sqrtf(s_player.body.vx * s_player.body.vx + s_player.body.vz * s_player.body.vz);
        float const want  = s_player.body.on_ground ? fminf(speed / PL_WALK, 1.0f) : 0.0f;
        s_stride += (want - s_stride) * fminf(dt * 8.0f, 1.0f);
        s_walk += dt * 9.0f * s_stride;
        s_swing_t = s_player.mining ? s_swing_t + dt * FRED_STROKES : 0.0f;

        // THIRD PERSON: behind him along the line he looks down, pulled
        // in when something is in the way so the camera never ends up in
        // a wall -- the ray is cast back from his eyes, and the camera
        // stops a little short of whatever it hits.
        if (third_person()) {
            float fx, fy, fz;
            ray_forward(yaw, pitch, &fx, &fy, &fz);
            float     back = FRED_BACK;
            ray_hit_t h;
            if (ray_pick(x, y, z, -fx, -fy, -fz, FRED_BACK, true, &h)) back = fmaxf(h.dist - 0.3f, 0.4f);
            s_cam.wx = x - (double)(fx * back);
            s_cam.wy = (float)y - fy * back;
            s_cam.wz = z - (double)(fz * back);
        }
    }

    // Take delivery of what core 1 finished, with a budget so a burst
    // cannot blow a frame, then ask for what is still missing.
    chunk_worker_collect(CHUNK_RESULTS_PER_FRAME);
    chunk_render_stream(s_cam.wx, s_cam.wz);
    if (devtest_deterministic()) settle_world(s_cam.wx, s_cam.wz);

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
//   F  walk <-> fly (the debug camera; the player is the default)
//   P  pause the scripted flight (nothing else has anything to pause)
//   T  textures <-> flat mean colours
//   V  view distance: near / medium / far
//
// The PLAYER's own keys are not here: they are bindings, polled once a
// tick (input.h), because a key being held is a state and not an event.
static void on_input(bsp_input_event_t const* ev, void* user) {
    (void)user;
    // A menu that is showing has the keyboard, all of it.
    if (menu_active()) {
        menu_event(ev);
        return;
    }
    // ... and so does the crafting book, because every letter typed
    // goes into its search box.
    if (craft_ui_active()) {
        craft_ui_event(ev);
        return;
    }
    if (furnace_ui_active()) {
        furnace_ui_event(ev);
        return;
    }
    if (chest_ui_active()) {
        chest_ui_event(ev);
        return;
    }
    if (bench_ui_active()) {
        bench_ui_event(ev);
        return;
    }
    if (ev->type != INPUT_EVENT_TYPE_SCANCODE) return;
    uint16_t const sc = ev->args_scancode.scancode;
    if ((sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return;
    if (s_app != APP_PLAY) return;

    // PAUSE: the bound key, and Esc whatever it is bound to -- a player
    // who rebinds Pause must not lose the way out. The inventory closes
    // first, the way it does everywhere else. Opening the menu SAVES:
    // on a handheld, pausing is what people do before switching it off.
    if (sc == input_key(CM_PAUSE) || sc == BSP_INPUT_SCANCODE_ESC) {
        if (s_player.inv.open) {
            s_player.inv.open = false;
            return;
        }
        if (s_cam_mode == CAM_FREE) {
            s_cam_mode = CAM_PLAYER;
            tick_reset(&s_tick, showtime_now());
        }
        save_world("pausing");
        menu_open_pause();
        return;
    }

    // The crafting book, on its own binding. An event and not a polled
    // binding, like the overlay below: opening a screen happens once,
    // when the key goes down.
    if (sc == input_key(CM_CRAFT)) {
        s_player.inv.open = false;  // one full-screen thing at a time
        craft_ui_open(RS_INVENTORY);
        return;
    }

    // The position overlay, on its own binding.
    if (sc == input_key(CM_INFO)) {
        s_info = !s_info;
        return;
    }

    // A screenshot: taken at the end of the frame being drawn, once
    // everything including the HUD is on it (on_render).
    if (sc == input_key(CM_SCREENSHOT)) {
        s_shot_wanted = true;
        return;
    }

    // The debug keys stand aside for a key a player has bound to
    // something: binding Jump to F must not also start the flying camera.
    if (input_key_bound(sc)) return;
    switch (sc) {
        case BSP_INPUT_SCANCODE_R:
            // Record a replay: from here, until R again. Written to
            // replays/last.cmr; copy it to test.cmr to make it the one
            // the `replay` scene plays.
            if (replay_recording()) {
                stop_recording();
            } else if (!replay_playing()) {
                replay_start_t st = {
                    .seed        = s_meta.seed,
                    .time_of_day = s_meta.time_of_day,
                    .x           = s_player.body.x,
                    .y           = s_player.body.y,
                    .z           = s_player.body.z,
                    .yaw         = s_player.yaw,
                    .pitch       = s_player.pitch,
                    .selected    = s_player.inv.selected,
                };
                memcpy(st.inv, s_player.inv.slot, sizeof(st.inv));
                bool const ok = replay_record_begin(&st);
                ESP_LOGI(TAG, "replay recording %s", ok ? "started (R to stop)" : "could not start");
            }
            break;

        case BSP_INPUT_SCANCODE_F:
            s_cam_mode   = (s_cam_mode == CAM_PLAYER) ? CAM_FREE : CAM_PLAYER;
            s_free_ready = false;  // re-place the free camera where the player is
            // Whichever was not running has a stale clock; start it
            // clean so the player does not get a tick's worth of
            // movement owed from however long they were flying.
            tick_reset(&s_tick, showtime_now());
            ESP_LOGI(TAG, "camera: %s", s_cam_mode == CAM_PLAYER ? "player" : "free flight");
            break;

        case BSP_INPUT_SCANCODE_N: {
            // Testing: the world's clock a quarter of a day on -- morning,
            // noon, evening, midnight -- so night can be looked at without
            // waiting ten minutes for it. Saved like any other time.
            s_meta.time_of_day += DAY_TICKS / 4;
            int hh = 0, mm = 0;
            daytime_clock(s_meta.time_of_day, &hh, &mm);
            ESP_LOGI(TAG, "time of day: %02d:%02d", hh, mm);
        } break;

        case BSP_INPUT_SCANCODE_P:
            if (s_flying) {
                s_paused_at = showtime_now();
            } else {
                s_time_off += showtime_now() - s_paused_at;
            }
            s_flying = !s_flying;
            ESP_LOGI(TAG, "scripted flight %s", s_flying ? "on" : "off");
            break;

        default:
            break;
    }
}

// The position overlay: where the player is, which way they face, the
// world's time, and whether a replay is being made or played.
//
// THE COMPASS: +z is north and +x east. The engine's camera turns right
// from +z towards +x (forward (sin yaw, cos yaw), right (cos yaw,
// -sin yaw)), which is north-to-east on a map with north up; and the sun
// rises at +x (game/daytime.c), so east is where it should be.
// --- Screenshots (CM_SCREENSHOT, 0 by default) ---------------------------
//
// The player's own, not the test kit's: the frame as they see it, HUD and
// all, into /sd/craftminer/screenshots/shotNNN.png -- next to the worlds,
// so it comes off the card with them. The "saved" line shows on the frames
// AFTER the capture, so it is never in the picture.

static void take_screenshot(pax_buf_t* fb) {
    char dir[160], path[192];
    snprintf(dir, sizeof(dir), "%s/screenshots", CM_DATA_DIR);
    cm_mkdir_p(dir);
    // The next free number. The stdio here has no stat(), so "free" is
    // "does not open".
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), "%s/shot%03d.png", dir, n);
        FILE* f = fopen(path, "rb");
        if (f == NULL) break;
        fclose(f);
    }
    bool const ok = n < 1000 && screenshot_capture_to(fb, path);
    if (ok) {
        i18n_fmt(s_shot_msg, sizeof(s_shot_msg), CM_STR_SHOT_SAVED, n);
    } else {
        snprintf(s_shot_msg, sizeof(s_shot_msg), "%s", T(n < 1000 ? CM_STR_SHOT_FAILED : CM_STR_SHOT_FULL));
    }
    ESP_LOGI(TAG, "screenshot: %s", s_shot_msg);
    s_shot_msg_until = showtime_now() + 2.5;
}

static void draw_info(pax_buf_t* fb) {
    static cm_str_t const NAMES[8] = {CM_STR_DIR_N,  CM_STR_DIR_NE, CM_STR_DIR_E,  CM_STR_DIR_SE,
                                      CM_STR_DIR_S,  CM_STR_DIR_SW, CM_STR_DIR_W,  CM_STR_DIR_NW};
    float deg = s_player.yaw * (180.0f / 3.14159265f);
    deg       = fmodf(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    int const octant = (int)((deg + 22.5f) / 45.0f) & 7;

    int hh = 0, mm = 0;
    daytime_clock(s_meta.time_of_day, &hh, &mm);

    char pos[64], face[48], clock[48], extra[48];
    i18n_fmt(pos, sizeof(pos), CM_STR_INFO_POSITION, (double)s_player.body.x, (double)s_player.body.y,
             (double)s_player.body.z);
    i18n_fmt(face, sizeof(face), CM_STR_INFO_FACING, T(NAMES[octant]), (int)(deg + 0.5f) % 360);
    i18n_fmt(clock, sizeof(clock), CM_STR_INFO_CLOCK, hh, mm, (long long)(s_meta.time_of_day / DAY_TICKS) + 1);
    extra[0] = '\0';
    if (replay_recording()) snprintf(extra, sizeof(extra), "%s", T(CM_STR_INFO_RECORDING));
    if (replay_playing()) i18n_fmt(extra, sizeof(extra), CM_STR_INFO_REPLAY, replay_position(), replay_length());
    bool const        msg      = showtime_now() < s_shot_msg_until;
    char const* const lines[5] = {pos, face, clock, extra, msg ? s_shot_msg : NULL};
    hud_text_lines(fb, lines, 5);
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
    if (s_app == APP_LOADING) {
        draw_loading(fb);
        devtest_after_render(fb, 0);
        frame_stats();
        return;
    }

    // Everything is submitted relative to an integer origin near the
    // camera, so the floats the rasteriser sees stay small however far
    // out this is (D-01). The camera goes into the same space.
    chunk_render_set_origin((int32_t)floor(s_cam.wx), (int32_t)floor(s_cam.wz));
    int32_t ox, oz;
    chunk_render_origin(&ox, &oz);

    float rx, ry, rz;
    chunk_render_rel(ox, oz, s_cam.wx, (double)s_cam.wy, s_cam.wz, &rx, &ry, &rz);
    render_set_camera_6dof(rx, ry, rz, s_cam.yaw, s_cam.pitch, 0.0f);

    // The time of day: the sky, the fog, the sun, and how bright each
    // light level is. Cheap -- a 256-entry table -- so every frame.
    s_day = daytime_at(s_app == APP_PLAY ? s_meta.time_of_day : s_title_time);
    daytime_light_lut(s_day.day, s_lut);
    mesh_set_light_lut(s_force_nolight ? NULL : s_lut);

    // --- Under the water (D-86) ---------------------------------------
    //
    // Water swallows red first and green next and leaves blue, so
    // everything drawn takes a blue cast: se_scene_set_tint() scales the
    // red and green of every triangle down and the blue hardly at all.
    // The sky is not sky from down here either -- it is the colour of
    // more water -- and the sun, the moon, the clouds and the stars are
    // not in the picture at all.
    //
    // A RENDER-side test, not a tick one: what the camera is inside of
    // must never reach the simulation (Part T).
    bool const eye_wet =
        block_liquid(world_block((int32_t)floor(s_cam.wx), (int32_t)floor((double)s_cam.wy),
                                 (int32_t)floor(s_cam.wz)));
    if (eye_wet) {
        se_scene_set_tint(WATER_TINT_RG, WATER_TINT_B);
        s_day.sky_argb = WATER_ARGB;
        s_day.fog_argb = WATER_ARGB;
    } else {
        se_scene_set_tint(32, 32);
    }
    chunk_render_set_fog(s_day.fog_argb);
    {
        // The directional light comes from whichever of the sun and the
        // moon is up, far off along its direction, so it models the
        // sides of blocks by day and by night alike. How BRIGHT it is
        // belongs to the light table, not to this.
        vec3_t const d = s_day.sun_dir.y > -0.05f ? s_day.sun_dir : v3_scale(s_day.sun_dir, -1.0f);
        se_light_set(&(se_light_t){
            .x = rx + d.x * 5000.0f, .y = ry + d.y * 5000.0f, .z = rz + d.z * 5000.0f, .brightness = 0.45f});
    }

    bool const       half   = s_half_ok && settings_half_res();
    pax_buf_t* const target = half ? &s_half.buf : fb;
    scene_set_render_scale(half ? 2 : 1);

    // The sky. The engine clears `fb`, but the scene is drawing into the
    // half-size layer, so that is what needs filling -- whatever it does
    // not cover is what shows through after the upscale.
    prof_begin(PROF_FILL);
    if (half) {
        se_ppa_fill(target, 0, 0, DISPLAY_LOG_H / 2, s_day.sky_argb);
        se_ppa_wait_job(0);
    } else {
        // Full resolution draws straight into the framebuffer, which
        // the empty on_backdrop left as it was: the sky goes in here.
        // A CPU clear, not a PPA fill: the CPU draws into this buffer
        // next, and a DMA fill under its cache is a coherence problem
        // this path is not worth having.
        pax_background(fb, s_day.sky_argb);
    }
    prof_end(PROF_FILL);

    scene_begin(target);

    prof_begin(PROF_SUBMIT);
    mesh_submit_counters_reset();
    // The sky first -- sun or moon, clouds, stars -- so the world draws
    // over it wherever they overlap.
    // No clouds on the title: its letters stand at the height they fly at.
    if (!eye_wet) {
        voxel_sky_submit((float)showtime_now(), s_day.sun_dir, s_day.fog_argb, s_day.day, ox, oz,
                         settings_clouds() && !s_force_noclouds && s_app == APP_PLAY);
    }
    chunk_render_submit(s_cam.wx, s_cam.wz);
    // The box round the block the crosshair found, while the player is
    // the one aiming. It is an edge, so the engine draws it after every
    // triangle and depth-tests it without writing depth.
    if (s_cam_effective == CAM_PLAYER && s_player.aim_valid) {
        hud_block_outline(s_player.aim.x, s_player.aim.y, s_player.aim.z);
    }
    hud_dropped_items();
    // Fred: his arm and what it holds in first person, all of him in
    // third. Lit by the cell he is in, like the world round him.
    if (s_app == APP_PLAY && s_cam_effective == CAM_PLAYER) {
        fred_hold_t const hold  = fred_hold_for(inv_held(&s_player.inv)->item);
        float const       swing = s_player.mining ? fred_stroke(s_swing_t) : 0.0f;
        if (third_person()) {
            double px, py, pz;
            float  pyaw, ppitch;
            player_eye(&s_player, tick_alpha(&s_tick), &px, &py, &pz, &pyaw, &ppitch);
            float rfx, rfy, rfz;
            chunk_render_rel(ox, oz, px, py - (double)PHYS_PLAYER_EYE, pz, &rfx, &rfy, &rfz);
            xform_t const     root = {mat3_rot_y(pyaw), v3(rfx, rfy, rfz), 1.0f};
            fred_pose_t const pose = {
                .walk        = s_walk,
                .stride      = s_stride,
                .swing       = swing,
                .head_pitch  = ppitch,
                .hold        = hold,
                .left_handed = left_handed()};
            fred_submit(&root, &pose,
                        world_light((int32_t)floor(px), (int32_t)floor(py - 0.6), (int32_t)floor(pz)));
        } else {
            float const bob = 0.03f * s_stride * fabsf(sinf(s_walk));
            fred_submit_fp_arm(swing, &hold, bob,
                               world_light((int32_t)floor(s_cam.wx), (int32_t)floor(s_cam.wy), (int32_t)floor(s_cam.wz)),
                               left_handed());
        }
    }
    prof_end(PROF_SUBMIT);

    prof_begin(PROF_PREPARE);
    scene_prepare(SE_RENDER_ZBUFFER);
    prof_end(PROF_PREPARE);

    prof_begin(PROF_RASTER);
    int64_t const t0 = esp_timer_get_time();
    scene_rasterize(SE_RENDER_ZBUFFER);
    int64_t const rast_us = esp_timer_get_time() - t0;
    prof_end(PROF_RASTER);

    if (half) {
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

    // After the upscale, so it is drawn at full resolution rather than
    // doubled up with the world. Before the shot is taken, so a
    // screenshot shows what the player saw.
    // Not while the debug camera is flying, where it would mean
    // nothing -- but yes during a test, so a reference screenshot
    // covers the overlay as well as the world.
    // Not while the debug camera is flying, where they belong to
    // nobody -- but yes during a test, so a reference shot covers the
    // overlay as well as the world.
    if (s_app == APP_TITLE) {
        prof_begin(PROF_HUD);
        menu_draw(fb);
        prof_end(PROF_HUD);
    } else if (menu_active()) {
        prof_begin(PROF_HUD);
        menu_draw(fb);
        prof_end(PROF_HUD);
    } else if (s_cam_effective != CAM_FREE) {
        prof_begin(PROF_HUD);
        // In third person the pick ray still starts at his eyes, so a
        // crosshair in the middle of a camera behind him would point at
        // the wrong thing: the block outline shows what he aims at.
        if (!third_person()) hud_crosshair(fb);
        hud_mine_progress(fb, player_mine_progress(&s_player));
        hud_player(fb, &s_player);
        hud_inventory(fb, &s_player);
        if (craft_ui_active()) craft_ui_draw(fb, &s_player.inv);
        if (furnace_ui_active()) furnace_ui_draw(fb, &s_player.inv);
        if (chest_ui_active()) chest_ui_draw(fb, &s_player.inv);
        if (bench_ui_active()) bench_ui_draw(fb, &s_player.inv);
        if (s_player.needs_tool != 0) {
            char line[96];
            i18n_fmt(line, sizeof(line), CM_STR_HUD_NEEDS_TOOL, T(item_label(s_player.needs_tool)));
            char const* const lines = line;
            hud_text_lines(fb, &lines, 1);
        } else if (s_info || replay_recording()) {
            draw_info(fb);
        } else if (showtime_now() < s_shot_msg_until) {
            char const* const line = s_shot_msg;
            hud_text_lines(fb, &line, 1);
        }
        prof_end(PROF_HUD);
    }

    // Last, so the shot has everything the player sees on it.
    if (s_shot_wanted) {
        s_shot_wanted = false;
        take_screenshot(fb);
    }

    devtest_after_render(fb, rast_us);
    frame_stats();
}

void app_main(void) {
    static se_app_config_t const cfg = {
        // F1-F6 are the hotbar (D-05), so the engine does not get F1.
        // Leaving is Esc, which becomes the pause menu in step 5.3 --
        // and that menu saves before it quits, which is the reason the
        // key had to come back from the engine in the first place.
        .f1_exits      = false,
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
