#pragma once
// =====================================================================
//  CraftMiner  --  drawing the streamed world
// ---------------------------------------------------------------------
//  Each frame, for every chunk that should be resident: keep it loaded,
//  keep a level of detail meshed, and submit the ones the camera can
//  see. By distance from the eye (the ladder is the showreel's, whose
//  numbers were measured, F-03/F-04):
//
//    within fancy_dist   everything, textured: canopies you see into,
//                        plants
//    within tex_dist     textured, canopies opaque, no plants
//    within coarse_dist  flat mean colours fading to fog -- 3-4x cheaper
//                        to fill than textures
//    within draw_dist    the same at half resolution, a quarter of the
//                        triangles
//    beyond              nothing; the backdrop's fog-coloured ground is
//                        the world out there
//
//  THE RENDER ORIGIN. Everything is submitted relative to an integer
//  origin near the player, so the floats reaching the rasteriser stay
//  small and exact however far they have walked (D-01). The camera must
//  be set in the same space -- chunk_render_origin() is what puts it
//  there.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "math/mesh_render.h"
#include "synthengine3d.h"

// The distance ladder. Blocks.
typedef struct {
    float    fancy_dist;
    float    tex_dist;
    float    coarse_dist;
    float    draw_dist;
    float    fog0, fog1;
    uint32_t fog_argb;
    int      load_radius;   // chunks kept resident
    int      evict_radius;  // ... and dropped beyond (hysteresis)
} cm_view_t;

#define CM_SKY_ARGB 0xFF8EC4F0u

// Near / medium / far, as the graphics menu will offer them.
cm_view_t cm_view_preset(int level);  // 0 near (the default, D-76), 1 medium, 2 far

// Load the block textures and build the material tables. After
// texcache_init().
bool chunk_render_init(void);
void chunk_render_shutdown(void);

void             chunk_render_set_view(cm_view_t const* v);

// What the far chunks fade into, overriding the view's own fog colour:
// the sky's colour at this time of day (game/daytime.h). 0 goes back to
// the view's.
void chunk_render_set_fog(uint32_t argb);

// A block's three textured materials -- top, sides, bottom -- in the
// order voxel_build_cube() wants: for a block drawn outside the world,
// in Fred's hand or lying on the ground.
void chunk_render_block_mats(uint8_t block, mesh_mat_t out[3]);
cm_view_t const* chunk_render_view(void);

// Flat mean colours instead of textures everywhere: 3-4x cheaper to
// fill, and the graphics menu's cheapest setting.
void chunk_render_set_textured(bool on);
bool chunk_render_textured(void);

// Move the render origin to the chunk containing (wx, wz). Call when
// the player crosses a chunk boundary. Everything submitted afterwards
// is relative to it, the camera included.
void chunk_render_set_origin(int32_t wx, int32_t wz);
void chunk_render_origin(int32_t* ox, int32_t* oz);

// A world position in the coordinates the scene wants.
static inline void chunk_render_rel(int32_t ox, int32_t oz, double wx, double wy, double wz, float* rx, float* ry,
                                    float* rz) {
    *rx = (float)(wx - (double)ox);
    *ry = (float)wy;
    *rz = (float)(wz - (double)oz);
}

// Keep residency up to date around (wx, wz): request what is missing
// nearest-first, drop what has gone too far (saving it if it was
// edited). Call once a frame, before submitting.
void chunk_render_stream(double wx, double wz);

// How many of the NINE chunks around (wx, wz) are resident, 0..9.
//
// This is the gate on entering a world (claudeplans/craftminer.md,
// D-26): the chunk the player stands in and the eight they could step
// into, and nothing beyond. Play starts when it reaches 9 and the rest
// of the view distance streams in behind them, so a large view distance
// costs nothing at the moment of entering. It is also exactly the
// condition a tick needs -- determinism rule 4 says a tick never
// branches on load state, which holds because a tick only ever runs
// with its 3x3 present.
int chunk_render_nine(double wx, double wz);

// Submit everything visible. The camera must already be set.
void chunk_render_submit(double eye_wx, double eye_wz);

// What the last submit did, for the HUD and the perf records. A chunk
// counts as drawn when any of its vertical sections was (D-34), and
// `sections_drawn` is how many of the CH_SECT_N survived the cull --
// the number that says whether sectioning is earning its keep.
void chunk_render_stats(int* chunks_drawn, int* sections_drawn, int* resident, int* missing);

// Chunks dropped from the resident set since boot. A number that climbs
// while the camera is standing still means the residency radius and the
// eviction radius are fighting each other.
int chunk_render_evicted(void);

// The texture file a material is drawn with, or NULL. The inventory
// uses it to draw a block as ITSELF rather than as the flat average
// colour that stood in for it (D-03) -- one table of files, not two.
char const* chunk_render_mat_file(int mat);
