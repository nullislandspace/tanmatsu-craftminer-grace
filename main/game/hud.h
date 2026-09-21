#pragma once
// =====================================================================
//  CraftMiner  --  what is drawn over the world
// ---------------------------------------------------------------------
//  Two things, and both exist for the same reason: without them you
//  cannot tell what you are about to hit.
//
//    the crosshair    where the ray goes
//    the outline      the block it found
//
//  THE CROSSHAIR IS NOT AT THE CENTRE OF THE SCREEN. The engine
//  projects the camera's forward axis to (RENDER_HALF_W,
//  RENDER_HORIZON_Y), and RENDER_HORIZON_Y is 256 on a 480-row display,
//  not 240 (se_config.h -- a game can move its horizon). Drawing the
//  crosshair at the geometric centre would put it sixteen pixels below
//  where the pick actually points, which is exactly the kind of aiming
//  error nobody thinks to suspect. It is derived from the projection
//  constants instead, so it follows if they ever change.
//
//  Block 4 adds the hotbar, the hearts and the hunger row here.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"

// The wireframe box round the block being aimed at, RELATIVE TO THE
// RENDER ORIGIN like everything else that reaches the scene (D-01).
// Call between scene_begin() and scene_prepare().
void hud_block_outline(int32_t bx, int32_t by, int32_t bz);

// The crosshair, drawn into the framebuffer AFTER the quarter-
// resolution layer has been upscaled onto it -- so it is crisp at full
// resolution rather than doubled up with everything else.
void hud_crosshair(pax_buf_t* fb);
