#pragma once
// =====================================================================
//  SynthMiner  --  what is drawn over the world
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

#include "game/player.h"
#include "pax_gfx.h"

// The wireframe box round the block being aimed at, RELATIVE TO THE
// RENDER ORIGIN like everything else that reaches the scene (D-01).
// Call between scene_begin() and scene_prepare().
void hud_block_outline(int32_t bx, int32_t by, int32_t bz);

// The crosshair, drawn into the framebuffer AFTER the quarter-
// resolution layer has been upscaled onto it -- so it is crisp at full
// resolution rather than doubled up with everything else.
void hud_crosshair(pax_buf_t* fb);

// The hotbar, the hearts and the hunger row. Same place in the frame
// as the crosshair, and for the same reason: text and thin borders at
// quarter resolution are unreadable.
void hud_player(pax_buf_t* fb, player_t const* p);

// The Tab screen, over everything. Nothing if it is not open.
void hud_inventory(pax_buf_t* fb, player_t const* p);

// The cracks over the block being mined: a progress bar is not what
// Minecraft does, but a bar is legible where cracks need a texture set
// that does not exist yet (the crack overlay is `voxel_fx`, waiting on
// this). Drawn with the world, so it sits on the block.
void hud_mine_progress(pax_buf_t* fb, float progress);

// The dropped items lying about, as small cubes. Submitted with the
// world, between scene_begin() and scene_prepare().
void hud_dropped_items(void);

// A few lines of text at the top left, with a shadow so they read over
// sky and ground alike: the position overlay (SM_INFO).
void hud_text_lines(pax_buf_t* fb, char const* const* lines, int n);

// A grid of inventory slots, drawn the way the Tab screen draws them.
// Exposed because the chest screen needs TWO of them side by side, and
// a second copy of "box, frame, icon, count" would be a second place
// for the slot to stop looking like a slot.
//
// `cursor` is the slot to ring in yellow, or -1 for none; `active`
// dims the whole grid when the cursor is on the other one, which is
// what tells a player which side the arrow keys belong to.
void hud_slot_grid(pax_buf_t* fb, int x, int y, inv_slot_t const* slot, int n, int cols, int slot_w, int cursor,
                   bool active);

// The size the Tab screen and the chest screen draw slots at.
#define HUD_INV_SLOT_W 60
