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
#include "bsp/device.h"
#include "common/texcache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "game/flycam.h"
#include "game/hud.h"
#include "game/input.h"
#include "game/interact.h"
#include "game/membench.h"
#include "game/player.h"
#include "items/item_entity.h"
#include "game/tick.h"
#include "gl_input.h"
#include "graceloader.h"
#include "math/mesh_render.h"
#include "synthengine3d.h"  // the whole public API
#include "testkit/devtest.h"
#include "testkit/profile.h"
#include "testkit/showtime.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "ui/settings.h"
#include "ui/title.h"
#include "world/chunk.h"
#include "world/chunk_render.h"
#include "world/chunk_worker.h"
#include "world/region.h"
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
} app_state_t;

static app_state_t s_app = APP_TITLE;
static double      s_title_t0;

static bool enter_title(void);
static bool enter_world(int slot, bool create, char const* name, uint32_t seed);
static void save_world(char const* why);
static void pregenerate(double wx, double wz, char const* why);

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
    {"near", 0},
    {"medium", 1},
    {"far", 2},
};

static bool content_select(char const* name) {
    if (name == NULL) return false;
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
    ESP_LOGI(TAG, "world: %d chunks / %d sections drawn of %d resident (%d missing), %d tris tested -> %d submitted",
             drawn, sections, resident, missing, tested, passed);

    // A full list drops in submission order, so anything here is a hole
    // in the picture -- a corner of the world, a chunk, half a title.
    int dropped_tri = 0, dropped_ttri = 0;
    scene_drop_stats(&dropped_tri, &dropped_ttri);
    if (dropped_tri > 0 || dropped_ttri > 0) {
        ESP_LOGW(TAG, "GEOMETRY DROPPED: %d flat past the %d cap, %d textured past %d -- the view is incomplete",
                 dropped_tri, SE_SCENE_TRI_CAP, dropped_ttri, SE_SCENE_TEXTURED_TRI_CAP);
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
    log_memory_map();
    membench_run();

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
    // The player's graphics and audio choices. The view distance itself
    // is applied on entering a world: the title has a view of its own.
    // Bindings registered first: the settings file restores them.
    input_init();
    settings_load(graceloader_get_install_basepath());
    // The launcher's key-cap PNGs, for the Controls menu (synthracer's
    // icons.c). Missing ones fall back to a text label.
    icons_load();
    chunk_render_set_textured(settings_textured());
    chunk_render_set_view(&(cm_view_t){0});
    cm_view_t const v = cm_view_preset(settings_view());
    chunk_render_set_view(&v);
    texcache_report();
    log_memory("textures loaded");

    // The worlds on the card.
    if (!worldstore_init(graceloader_get_install_basepath())) {
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

static void drain_and_clear(void) {
    // Synchronous mode drains the queues as part of switching, which is
    // exactly the guarantee needed here.
    bool const was_async = !chunk_worker_synchronous();
    chunk_worker_set_synchronous(true);
    chunk_store_clear();
    if (was_async) chunk_worker_set_synchronous(false);
}

static bool enter_title(void) {
    drain_and_clear();
    worldstore_close();
    if (!title_begin()) return false;
    chunk_worker_set_seed(title_seed());
    cm_view_t const tv = title_view();
    chunk_render_set_view(&tv);

    // Everything the drift will look at, before the first letter is
    // written. The path is short and the view radius covers all of it
    // from the middle, so one point is enough.
    double px, pz;
    title_stream_at(0.5 * 16.0, &px, &pz);  // the middle of the loop
    pregenerate(px, pz, "the title");
    s_title_t0 = showtime_now();
    s_app      = APP_TITLE;
    s_cam_mode = CAM_PLAYER;
    menu_open_title();
    ESP_LOGI(TAG, "title");
    return true;
}

// Open the world in `slot` -- or, with `create`, make one there first.
static bool enter_world(int slot, bool create, char const* name, uint32_t seed) {
    drain_and_clear();
    title_end();

    char slug[CM_WORLD_SLUG_MAX];
    worldstore_slot_slug(slot, slug, sizeof(slug));
    bool const ok = create ? worldstore_create_in(slot, name, seed, &s_meta, &s_saved)
                           : worldstore_open(slug, &s_meta, &s_saved);
    if (!ok) {
        ESP_LOGE(TAG, "could not %s the world in slot %d", create ? "create" : "open", slot + 1);
        enter_title();
        menu_status(create ? "Could not create the world" : "Could not open that world");
        return false;
    }
    ESP_LOGI(TAG, "world \"%s\" (slot %d) %s, seed %u", s_meta.name, slot + 1, create ? "created" : "opened",
             (unsigned)s_meta.seed);

    chunk_worker_set_seed(s_meta.seed);
    // The ground under the player before the player is on it (D-26).
    // The rest streams in behind them while they are already walking,
    // which is what the freeze below covers.
    pregenerate(s_saved.x, s_saved.z, "the spawn");
    // The player's own view distance: the title's is generous because
    // it is looking at one static word, not walking.
    cm_view_t const pv = cm_view_preset(settings_view());
    chunk_render_set_view(&pv);
    item_entity_reset();

    // WHO THEY WERE. Health, hunger and what they carry come back from
    // the save; a player with nothing saved gets the starting kit.
    player_reset(&s_player);
    s_player.health = s_saved.health;
    s_player.hunger = s_saved.hunger;
    if (s_saved.has_inv) {
        memcpy(s_player.inv.slot, s_saved.inv, sizeof(s_player.inv.slot));
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
    s_app      = APP_PLAY;
    s_cam_mode = CAM_PLAYER;
    menu_close();
    ESP_LOGI(TAG, "entering at %.1f, %.1f, %.1f (%s)", s_saved.x, s_saved.y, s_saved.z,
             s_saved.placed ? "where they left" : "a new player");
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

    bool const ok = worldstore_save(&s_meta, &s_saved);
    ESP_LOGI(TAG, "saved (%s): %d chunk(s), level.cmw %s", why, chunks, ok ? "written" : "FAILED");
    menu_status(ok ? "Saved" : "SAVING FAILED");
}

// Generate every chunk the view will want, NOW, before anything is
// drawn or animated.
//
// The streamer asks for four chunks a frame on purpose -- it is built
// so that walking never stalls -- but "never stalls" and "is complete"
// are different promises, and an opening sequence needs the second
// one. The title writes its letters into the world with world_set(),
// which NO-OPS on a chunk that is not resident yet; a title that starts
// before its world exists spends its first seconds spelling half a
// word, and which half depends on the SD card that morning.
//
// So: switch the worker inline, run the streamer until it says nothing
// is missing, switch back. On the badge that is 56 ms a chunk (F-23)
// and a few seconds for a full view -- which is why it happens while
// the screen still says nothing, and never once the player is in
// control.
static void pregenerate(double wx, double wz, char const* why) {
    int64_t const t0       = esp_timer_get_time();
    bool const    to_async = !chunk_worker_synchronous();
    chunk_worker_set_synchronous(true);

    int missing = 0;
    int rounds  = 0;
    for (; rounds < 600; rounds++) {
        chunk_render_stream(wx, wz);
        chunk_render_stats(NULL, NULL, NULL, &missing);
        if (missing == 0) break;
    }
    if (to_async) chunk_worker_set_synchronous(false);

    int resident = 0;
    chunk_render_stats(NULL, NULL, &resident, NULL);
    ESP_LOGI(TAG, "pregenerated %s: %d chunks resident in %d rounds, %lld ms%s", why, resident, rounds,
             (long long)((esp_timer_get_time() - t0) / 1000), missing == 0 ? "" : " (INCOMPLETE)");
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

// Per frame. `dt` is seconds since the last frame, already clamped --
// unused for now: the content is drawn from the show clock instead.
static void on_update(float dt, void* user) {
    (void)user;
    showtime_frame();
    devtest_update();

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
                save_world("quitting to the title");
                enter_title();
                break;
            case MENU_CMD_LEAVE:
                ESP_LOGI(TAG, "leaving for the launcher");
                audio_mixer_shutdown();  // a speaker left running across the restart squeals
                bsp_device_restart_to_launcher();
                break;
            case MENU_CMD_GRAPHICS:
                chunk_render_set_textured(settings_textured());
                if (s_app == APP_PLAY) {
                    cm_view_t const v = cm_view_preset(settings_view());
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

    cam_mode_t const mode = devtest_running() ? CAM_SCRIPTED : s_cam_mode;
    s_cam_effective       = mode;

    // Where the camera will be this frame decides what has to exist.
    if (mode == CAM_SCRIPTED) {
        float yaw;
        fly_pose(fly_time(), &s_cam.wx, &s_cam.wz, &yaw);
        s_cam.yaw        = yaw;
        s_cam.pitch      = FLY_PITCH;
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
        input_gyro_frame(dt, settings_gyro() && s_player_ready && !s_player.inv.open);
        int const n = tick_due(&s_tick, showtime_now());
        for (int i = 0; i < n; i++) {
            cm_actions_t const mask = input_sample();
            player_tick(&s_player, mask, input_pressed());
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
    }

    // Take delivery of what core 1 finished, with a budget so a burst
    // cannot blow a frame, then ask for what is still missing.
    chunk_worker_collect(CHUNK_RESULTS_PER_FRAME);
    chunk_render_stream(s_cam.wx, s_cam.wz);
    if (devtest_deterministic()) settle_world(s_cam.wx, s_cam.wz);

    // The ground under the player has to exist before they may fall
    // through it (D-26). Frozen until the chunk they are standing in is
    // resident, which on entering a world is the first thing that
    // arrives.
    if (mode == CAM_PLAYER && !menu_active()) {
        bool const standing = chunk_find(chunk_of((int32_t)floor(s_player.body.x)),
                                         chunk_of((int32_t)floor(s_player.body.z))) != NULL;
        tick_freeze(&s_tick, !standing);
        if (standing && !s_player_ready) {
            // The terrain is here, so the position can be settled: put
            // a returning player back exactly where they left, and a new
            // one -- or one whose spot is now inside something -- on the
            // ground at their column.
            bool const exact = s_saved.placed && player_place(&s_player, s_saved.x, s_saved.y, s_saved.z,
                                                              s_saved.yaw, s_saved.pitch);
            if (!exact) player_spawn(&s_player, s_saved.x, s_saved.z, s_player.yaw);
            s_player_ready = true;
            ESP_LOGI(TAG, "player standing at %.1f, %.1f, %.1f", s_player.body.x, s_player.body.y, s_player.body.z);
        }
    }
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

    // The debug keys stand aside for a key a player has bound to
    // something: binding Jump to F must not also start the flying camera.
    if (input_key_bound(sc)) return;
    switch (sc) {
        case BSP_INPUT_SCANCODE_F:
            s_cam_mode   = (s_cam_mode == CAM_PLAYER) ? CAM_FREE : CAM_PLAYER;
            s_free_ready = false;  // re-place the free camera where the player is
            // Whichever was not running has a stale clock; start it
            // clean so the player does not get a tick's worth of
            // movement owed from however long they were flying.
            tick_reset(&s_tick, showtime_now());
            ESP_LOGI(TAG, "camera: %s", s_cam_mode == CAM_PLAYER ? "player" : "free flight");
            break;

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

    bool const       half   = s_half_ok && settings_half_res();
    pax_buf_t* const target = half ? &s_half.buf : fb;
    scene_set_render_scale(half ? 2 : 1);

    // The sky. The engine clears `fb`, but the scene is drawing into the
    // half-size layer, so that is what needs filling -- whatever it does
    // not cover is what shows through after the upscale.
    prof_begin(PROF_FILL);
    if (half) {
        se_ppa_fill(target, 0, 0, DISPLAY_LOG_H / 2, CM_SKY_ARGB);
        se_ppa_wait_job(0);
    } else {
        // Full resolution draws straight into the framebuffer, which
        // the empty on_backdrop left as it was: the sky goes in here.
        // A CPU clear, not a PPA fill: the CPU draws into this buffer
        // next, and a DMA fill under its cache is a coherence problem
        // this path is not worth having.
        pax_background(fb, CM_SKY_ARGB);
    }
    prof_end(PROF_FILL);

    scene_begin(target);

    prof_begin(PROF_SUBMIT);
    mesh_submit_counters_reset();
    chunk_render_submit(s_cam.wx, s_cam.wz);
    // The box round the block the crosshair found, while the player is
    // the one aiming. It is an edge, so the engine draws it after every
    // triangle and depth-tests it without writing depth.
    if (s_cam_effective == CAM_PLAYER && s_player.aim_valid) {
        hud_block_outline(s_player.aim.x, s_player.aim.y, s_player.aim.z);
    }
    hud_dropped_items();
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
        hud_crosshair(fb);
        hud_mine_progress(fb, player_mine_progress(&s_player));
        hud_player(fb, &s_player);
        hud_inventory(fb, &s_player);
        prof_end(PROF_HUD);
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
