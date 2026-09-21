// =====================================================================
//  CraftMiner  --  the title, written in blocks (see title.h)
// =====================================================================

#include "ui/title.h"

#include <math.h>
#include <string.h>

#include "world/chunk.h"
#include "world/chunk_render.h"
#include "world/worldstore.h"

// The seed the title's landscape is generated from.
//
// Fixed, so the picture is the same every boot: a title screen that
// looked different each time would read as a bug rather than as
// variety. CHOSEN, not picked -- the first seed tried put the camera
// over open ocean, which is a flat blue band and says nothing about
// the game. This one was found by scoring every seed under 4000 on the
// ground it gives across the camera's field of view: land, well clear
// of the water, with real relief and no cliff. It comes out at 25..38
// over a sea level of 24.
#define TITLE_SEED 0x00000B05u

#define GLYPH_H    7
// The letters' bottom row. Clear of the tallest TREE, not merely the
// tallest ground: terrain reaches y41 and a tree on it another six, so
// anything below y47 gets a canopy in front of it. The first version
// sat at 44 and "Craft" spent the whole title behind an oak.
#define TITLE_Y    56
#define TITLE_Z    24   // the plane they stand in
#define TITLE_DEEP 2    // blocks thick, so they read as solid from an angle

#define BUILD_T0 0.5f    // the first block appears ...
#define BUILD_DT 0.013f  // ... and each next one this much later
#define LOOP_SECS 16.0f  // the whole drift, then it starts again

typedef struct {
    char        c;
    char const* rows[GLYPH_H];
} glyph_t;

// The showreel's font, unchanged: a '#' is a block.
static glyph_t const FONT[] = {
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'r', {"....", "....", "#.##", "##..", "#...", "#...", "#..."}},
    {'a', {"....", "....", ".##.", "...#", ".###", "#..#", ".###"}},
    {'f', {"..##", ".#..", "####", ".#..", ".#..", ".#..", ".#.."}},
    {'t', {".#..", ".#..", "###.", ".#..", ".#..", ".#.#", "..#."}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'i', {"#", ".", "#", "#", "#", "#", "#"}},
    {'n', {"....", "....", "###.", "#..#", "#..#", "#..#", "#..#"}},
    {'e', {"....", "....", ".##.", "#..#", "####", "#...", ".###"}},
};

static char const TEXT[] = "CraftMiner";
#define SPLIT 5  // "Craft" in grass | "Miner" in cobblestone

// One block of the title and when it appears.
typedef struct {
    int32_t x, y, z;
    uint8_t block;
    float   at;
    bool    placed;
} place_t;

#define MAX_BLOCKS 560
static place_t s_place[MAX_BLOCKS];
static int     s_n;
static float   s_mid_x, s_x0, s_x1;
static bool    s_active;

uint32_t title_seed(void) {
    return TITLE_SEED;
}

static glyph_t const* glyph(char c) {
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) return &FONT[i];
    }
    return NULL;
}

// The letters' blocks in the order they appear: column by column from
// the left, bottom to top, front layer then back.
static void build_title(void) {
    int width = -1;
    for (int k = 0; TEXT[k]; k++) width += (int)strlen(glyph(TEXT[k])->rows[0]) + 1;

    int const x0 = -width / 2;  // centred on the world origin
    s_x0         = (float)x0;
    s_x1         = (float)(x0 + width);

    s_n     = 0;
    int col = x0;
    for (int k = 0; TEXT[k]; k++) {
        glyph_t const* g = glyph(TEXT[k]);
        int const      w = (int)strlen(g->rows[0]);
        uint8_t const  b = (k < SPLIT) ? BLK_GRASS : BLK_COBBLE;
        for (int cx = 0; cx < w; cx++, col++) {
            for (int row = GLYPH_H - 1; row >= 0; row--) {
                if (g->rows[row][cx] != '#') continue;
                for (int dz = 0; dz < TITLE_DEEP && s_n < MAX_BLOCKS; dz++) {
                    s_place[s_n] = (place_t){
                        .x     = col,
                        .y     = TITLE_Y + (GLYPH_H - 1 - row),
                        .z     = TITLE_Z + dz,
                        .block = b,
                        .at    = BUILD_T0 + BUILD_DT * (float)s_n,
                    };
                    s_n++;
                }
            }
        }
        col++;  // the gap between letters
    }

    // The middle of what was actually written, not of the width the
    // glyphs were budgeted. The two differ by a few blocks -- the last
    // letter has no trailing gap -- and aiming the camera at the wrong
    // one puts the word off-centre on screen.
    int lo = 1 << 20, hi = -(1 << 20);
    for (int i = 0; i < s_n; i++) {
        if (s_place[i].x < lo) lo = s_place[i].x;
        if (s_place[i].x > hi) hi = s_place[i].x;
    }
    s_x0    = (float)lo;
    s_x1    = (float)hi + 1.0f;
    s_mid_x = 0.5f * (s_x0 + s_x1);
}

// The camera's path, 0..1 through the loop: a slow drift left to right
// and upwards, south of the letters and below them, so it looks up.
//
// FAR ENOUGH BACK TO FIT THE WORD. The letters are 48 blocks across and
// the projection sees 400/450 of the distance to either side, so at 34
// blocks it could show 60 -- until the drift moves the camera 10 off
// centre and needs 68. 52 blocks back fits it with room, and the extra
// distance is also what puts the ground in frame: the horizon sits at
// row 256 of 480, so looking up at something means looking past
// everything below it unless it is a long way off.
static void path(float s, double* x, float* y, double* z) {
    *x = (double)s_mid_x - 5.0 + 10.0 * (double)s;
    *y = (float)TITLE_Y - 8.0f + 3.0f * s;
    *z = (double)TITLE_Z - 44.0 + 5.0 * (double)s;
}

bool title_begin(void) {
    static world_meta_t   meta;
    static player_state_t player;
    if (!worldstore_open_scratch(TITLE_SEED, &meta, &player)) return false;
    build_title();
    for (int i = 0; i < s_n; i++) s_place[i].placed = false;
    s_active = true;
    return true;
}

void title_end(void) {
    s_active = false;
    worldstore_close();
}

void title_update(double t) {
    if (!s_active) return;
    float const ft = (float)fmod(t, (double)LOOP_SECS);

    for (int i = 0; i < s_n; i++) {
        place_t* p = &s_place[i];
        bool const due = ft >= p->at;
        if (due == p->placed) continue;

        // The loop restarting takes them all away again, which is what
        // makes the second pass look like the first.
        if (!due) {
            if (world_block(p->x, p->y, p->z) == p->block) world_set(p->x, p->y, p->z, BLK_AIR, 0);
            p->placed = false;
            continue;
        }
        // world_set no-ops on a chunk that is not resident yet, so this
        // is retried every frame until the streamer has caught up --
        // which is why `placed` is only set once it took.
        world_set(p->x, p->y, p->z, p->block, ST_PLACED);
        p->placed = world_block(p->x, p->y, p->z) == p->block;
    }
}

title_view_t title_camera(double t) {
    float const ft = (float)fmod(t, (double)LOOP_SECS);
    float const s  = ft / LOOP_SECS;

    title_view_t v;
    path(s, &v.wx, &v.wy, &v.wz);

    // Look at the middle of the letters, drifting along them so the eye
    // is led across the word rather than staring at its centre.
    double const tx = (double)s_mid_x + 4.0 * (double)s;
    // Aimed a little BELOW the middle of the letters, so the meadow
    // under them is in frame rather than just past the bottom edge.
    double const ty = (double)TITLE_Y + 0.5;
    double const tz = (double)TITLE_Z;

    double const dx = tx - v.wx, dy = ty - (double)v.wy, dz = tz - v.wz;
    // The engine's convention: forward is (sin yaw, cos yaw) in x and z,
    // and POSITIVE PITCH LOOKS DOWN (raycast.h).
    v.yaw   = (float)atan2(dx, dz);
    v.pitch = (float)atan2(-dy, sqrt(dx * dx + dz * dz));
    return v;
}

void title_stream_at(double t, double* wx, double* wz) {
    title_view_t const v = title_camera(t);
    if (wx != NULL) *wx = v.wx;
    if (wz != NULL) *wz = v.wz;
}

cm_view_t title_view(void) {
    // The title's own view, not one of the player's presets. The
    // letters stand 52 blocks off, and the near preset would have them
    // flat-shaded and six tenths of the way into the fog -- grey, where
    // "Craft" is supposed to be green. This keeps textures out to 60
    // and pushes the fog past the letters entirely, so the word reads
    // as the blocks it is made of.
    //
    // evict_radius 7 is the ring's limit (CH_EVICT_MAX) and the reason
    // draw_dist stops at 96: six chunks of loading is 96 blocks, and
    // drawing further than you load is drawing a hole.
    // DRAW DISTANCE IS A TRIANGLE BUDGET, not a preference. The engine's
    // lists hold 4096 flat and 2048 textured and a full list drops the
    // rest SILENTLY, in submission order -- so an over-generous view
    // does not degrade, it deletes an arbitrary corner of the world.
    // The first version of this asked for 96 blocks over 169 chunks and
    // lost "Craft" (F-48).
    //
    // 56 blocks of terrain behind letters that stand 44 away is enough
    // scenery, and it fits with room. `scene_drop_stats()` says so
    // rather than the picture having to.
    return (cm_view_t){
        .fancy_dist   = 20.0f,
        .tex_dist     = 52.0f,
        .coarse_dist  = 44.0f,
        .draw_dist    = 56.0f,
        .fog0         = 44.0f,
        .fog1         = 64.0f,
        .fog_argb     = CM_SKY_ARGB,
        .load_radius  = 4,
        .evict_radius = 5,
    };
}
