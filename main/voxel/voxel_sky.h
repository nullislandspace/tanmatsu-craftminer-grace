#pragma once
// =====================================================================
//  SynthMiner  --  the sky over the block world
// ---------------------------------------------------------------------
//  A square sun and a square moon, unlit, far out along their directions
//  (behind everything, so the terrain hides them at the horizon), and a
//  layer of flat, blocky clouds drifting east at VOX_CLOUD_Y. The sky's
//  colour itself is the scene's backdrop; the stars, at night, are the
//  shared starfield.
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_sky.h. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "math/xform.h"

// Above the tallest trees of a 64-high world (the showreel's was 32 high
// and put them at 42).
#define VOX_CLOUD_Y 56.0f

// `sun_dir` points at the sun (normalised); the moon is opposite.
// `fog_argb`: what the far clouds fade into (the horizon's colour).
// `light` 0..1 darkens the clouds (1 day, towards 0 at night) and brings
// out the stars below 0.3. `ox`, `oz`: the render origin (chunk_render.h)
// -- the clouds are laid out in WORLD coordinates, so moving the origin
// does not move them. `clouds` false leaves them out.
void voxel_sky_submit(float t, vec3_t sun_dir, uint32_t fog_argb, float light, int32_t ox, int32_t oz, bool clouds);
