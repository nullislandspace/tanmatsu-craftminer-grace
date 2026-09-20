#pragma once
// =====================================================================
//  CraftMiner  --  the sky over the block world
// ---------------------------------------------------------------------
//  A square sun and a square moon, unlit, far out along their directions
//  (behind everything, so the terrain hides them at the horizon), and a
//  layer of flat, blocky clouds drifting east at VOX_CLOUD_Y. The sky's
//  colour itself is the scene's backdrop; the stars, at night, are the
//  shared starfield.
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_sky.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdint.h>
#include "math/xform.h"

#define VOX_CLOUD_Y 42.0f

// `sun_dir` points at the sun (normalised); the moon is opposite.
// `fog_argb`: what the far clouds fade into (the horizon's colour).
// `light` 0..1 darkens the clouds (1 day, towards 0 at night).
void voxel_sky_submit(float t, vec3_t sun_dir, uint32_t fog_argb, float light);
