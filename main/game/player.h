#pragma once
// =====================================================================
//  CraftMiner  --  the player
// ---------------------------------------------------------------------
//  A body (physics.h), a direction to look, and what the two of them do
//  with a tick's worth of input. Health, hunger and the inventory are
//  block 4; this is walking, jumping, looking, breaking and placing.
//
//  IT TICKS AT 20 Hz AND THE FRAME INTERPOLATES (D-02). player_tick()
//  advances the simulation by exactly one tick from a bitmask;
//  player_eye() blends the last two poses for the frame being drawn.
//  That is what makes the movement the same at 10 fps and at 30, and
//  what makes a recorded bitmask stream replay to the same world.
//
//  SPEEDS ARE PER TICK, not per second, for the same reason: a number
//  that means "per tick" cannot silently acquire a frame-rate
//  dependency later.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game/input.h"
#include "game/physics.h"
#include "game/raycast.h"

// Blocks per tick. 0.215 is about 4.3 blocks a second, Minecraft's
// walk. Sneaking is a little under a third of it.
#define PL_WALK  0.215f
#define PL_SNEAK 0.065f
#define PL_ACCEL 0.35f  // share of the gap to target speed closed per tick

// The jump arc. These three are chosen together, by simulating the arc
// rather than by feel, because what matters is a number you can state:
//
//   apex 1.33 blocks, 0.40 s up, 0.85 s in the air.
//
// The apex has to clear a block WITH ROOM, or you cannot place one
// underneath yourself -- and pillaring up is how you get out of a hole,
// so it is not a trick, it is basic movement. 1.33 leaves a third of a
// block of margin at the top for the placement to happen in.
//
// Slower than Minecraft (0.60 s in the air) on purpose: at 15 fps a
// 0.60 s jump is nine frames from take-off to landing, and judging a
// landing in nine frames is not fair on the player. Same apex, longer
// arc -- which is the pair (v0 * k, g * k^2), here with k = 0.7.
#define PL_GRAVITY  0.04f  // blocks per tick per tick
#define PL_DRAG     0.98f  // per tick, on the vertical
#define PL_JUMP     0.32f  // -> apex 1.33 blocks
#define PL_TERMINAL 3.0f   // blocks a tick: nothing falls faster

typedef struct {
    phys_body_t body;
    float       yaw, pitch;

    // The previous tick's pose, so a frame drawn between two ticks can
    // interpolate instead of stepping.
    double prev_x, prev_y, prev_z;
    float  prev_yaw, prev_pitch;

    bool   in_air_last;   // for a landing sound, later
    int    selected;      // hotbar slot 0..5; block 4 gives it an inventory
    ray_hit_t aim;        // what the crosshair found this tick
    bool      aim_valid;
} player_t;

// Put the player at (x, z), standing on whatever is there.
void player_spawn(player_t* p, double x, double z, float yaw);

// Advance one simulation tick. `mask` is the tick's input, `pressed`
// the actions that went down since the last tick (so a held key places
// one block, not twenty).
void player_tick(player_t* p, cm_actions_t mask, cm_actions_t pressed);

// The eye for the frame being drawn. `alpha` is how far through the
// current tick it is, 0..1.
void player_eye(player_t const* p, float alpha, double* x, double* y, double* z, float* yaw, float* pitch);
