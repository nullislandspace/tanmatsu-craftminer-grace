#pragma once
// =====================================================================
//  Showreel  --  the backdrop: space, or sky and ground, painted by the PPA
// ---------------------------------------------------------------------
//  Large flat areas cost the CPU nothing when the PPA paints them (the
//  approach of Stunt Racer's main/backdrop.c). A scene declares its
//  backdrop (scene.h):
//
//    space        one colour everywhere (zero-initialised: black) -- one
//                 full-screen FILL, as the showreel always had
//    sky/ground   the horizon (horizon.h) splits the screen. The PPA
//                 fills every row wholly on one side with that side's
//                 colour; the CPU paints only the wedge a rolled horizon
//                 cuts out of the band between, which is empty when the
//                 camera is level.
//
//  The ground is a plane at infinity: geometry drawn over it (a landing
//  pad, buildings) stands on it, and the backdrop needs no depth.
//
//  Per frame: set the camera, backdrop_begin() (queues the fills), do the
//  pixel-free work (submit, prepare) while the PPA runs, then
//  backdrop_finish() before the first framebuffer write.
//  Lifted from tanmatsu-showreel-grace,
//  main/backdrop.h. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "pax_gfx.h"

typedef struct {
    uint32_t sky_argb;     // ARGB; alpha ignored. 0, the default, is black
    uint32_t ground_argb;  // used only with `ground`
    bool     ground;       // false: sky_argb everywhere (space)
} backdrop_t;

// Once, before the first frame: brings up the PPA. Without it every
// frame falls back to painting on the CPU -- slower, never blank.
void backdrop_init(void);

// After the frame's camera is set, before submitting geometry: queue the
// PPA fills for `bd` (NULL = black). `fb` is the screen, or the
// half-size buffer of a quarter-resolution frame (scene_set_render_scale):
// the horizon is worked out in screen coordinates and scaled to it.
void backdrop_begin(pax_buf_t* fb, backdrop_t const* bd);

// Before anything writes the framebuffer: wait for the fills, then paint
// what the PPA did not (the horizon wedge, or everything if it refused).
void backdrop_finish(pax_buf_t* fb);
