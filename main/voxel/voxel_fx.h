#pragma once
// =====================================================================
//  CraftMiner  --  mining and building effects
// ---------------------------------------------------------------------
//  Everything a scene draws on and round the blocks it changes, each a
//  pure function of the time since its event:
//
//    outline   the thin black box round the block being aimed at;
//    cracks    dark cracks spreading over the block's faces as it is
//              hit (0..1 of the way to breaking) -- lines, no textures;
//    break     the block bursting into small chips of its colours,
//              thrown out and falling;
//    item      the dropped block: a small spinning cube that falls to
//              the floor, bobs, and flies to the miner when picked up;
//    pop       a placed block growing into place (the world gets the
//              block when the pop is over: VOXEL_POP_SECS later).
//
//  Coordinates are block cells (voxel_world.h); a cell's cube is
//  [x, x+1] x [y, y+1] x [z, z+1].
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_fx.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "math/xform.h"

#define VOXEL_POP_SECS   0.15f
#define VOXEL_BREAK_SECS 0.9f

void voxel_fx_init(void);
void voxel_fx_shutdown(void);

void voxel_fx_outline(int x, int y, int z);

// `progress` 0..1: how far the block is from breaking. `seed` varies the
// pattern from block to block. Drawn on the faces the camera sees.
void voxel_fx_cracks(int x, int y, int z, float progress, unsigned seed);

// The burst of block `block` broken `since` seconds ago (nothing after
// VOXEL_BREAK_SECS).
void voxel_fx_break(int x, int y, int z, uint8_t block, float since, unsigned seed);

// The item dropped by breaking block `block` at (x, y, z) `since` seconds
// ago, lying on the floor at height `floor_y` (the top of the block
// below), picked up at `pick_at` seconds after the break by a miner at
// `to` (drawn flying there over 0.25 s, then gone).
void voxel_fx_item(int x, int y, int z, uint8_t block, float since, float floor_y, float pick_at, vec3_t to);

// The flame of the torch in cell (x, y, z): two crossed quads of the
// torch-flame texture over the stick's tip, unlit, flickering.
void voxel_fx_flame(int x, int y, int z, float t, unsigned seed);

// Block `block` placed at (x, y, z) `since` seconds ago: nothing after
// VOXEL_POP_SECS (the world has it by then).
void voxel_fx_pop(int x, int y, int z, uint8_t block, float since);
