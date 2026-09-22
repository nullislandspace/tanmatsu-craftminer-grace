#pragma once
// =====================================================================
//  CraftMiner  --  starfield
// ---------------------------------------------------------------------
//  A fixed sky of single-pixel stars (scene_point). Each star is a
//  direction, drawn at the camera position plus that direction times a
//  large distance: they turn with the camera but never move with it --
//  infinitely far away, as stars should be. Seeded, so every run (and
//  every scene) sees the same sky.
//
//  Brightness follows a steep power law (a few bright stars, many faint
//  ones), with a mild white / blue / yellow tint spread; ~40% of the
//  stars crowd a band across the sky, a hint of a galactic plane.
//  Lifted from tanmatsu-showreel-grace,
//  main/common/starfield.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "math/xform.h"

// Build the star table. Idempotent.
void starfield_init(void);

// Submit the stars for the current camera. Call after the scene's camera
// is set; geometry drawn afterwards still hides them (depth test).
void starfield_submit(void);

// The same stars, the whole sky turned by `turn` (a rotation): another
// solar system's sky, the band crossing it elsewhere.
void starfield_submit_turned(mat3_t const* turn);

// Only the stars above the horizon (direction y >= min_y): for a world
// with ground, where the backdrop's flat ground would otherwise show the
// stars under it beyond the draw distance.
void starfield_submit_above(float min_y);
