#pragma once
// =====================================================================
//  Showreel  --  scene camera helpers
// ---------------------------------------------------------------------
//  Thin wrappers over render_set_camera_6dof(). Call once per frame,
//  first thing in a scene's submit: the engine transforms vertices into
//  camera space as they are submitted, so the camera must be in place
//  before any geometry goes in.
//  Lifted from tanmatsu-showreel-grace,
//  main/camera.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "math/xform.h"

// Eye at `eye`, looking at `target`, rolled by `roll` radians.
void camera_look_at(vec3_t eye, vec3_t target, float roll);

// The current camera's eye position (for back-face culling).
vec3_t camera_eye(void);

// The current camera's orientation: its right, up and forward axes in
// world space (mat3_t columns), e.g. for culling whole objects against
// the view before submitting them.
mat3_t camera_basis(void);
