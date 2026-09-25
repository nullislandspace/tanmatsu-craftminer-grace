#pragma once
// =====================================================================
//  SynthMiner  --  the player
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
#include "items/inventory.h"

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

// SWIMMING (D-86). Water is not solid, so a player in it falls -- but
// slowly, because buoyancy cancels almost all of gravity, and they can
// push themselves back up. The numbers follow from phys_gravity's
// recurrence `vy = (vy - g) * drag` rather than from feel:
//
//   sinking, hands down:  4 * (0 - 0.008)         = -0.032/tick, 0.6 b/s
//   swimming up:          4 * (0.030 - 0.008)     = +0.088/tick, 1.8 b/s
//   diving:               4 * (-0.030 - 0.008)    = -0.152/tick, 3.0 b/s
//
// (the 4 is drag/(1 - drag) with drag = 0.8.) Jump swims up and sneak
// dives, which is free because the speed in water does not depend on
// sneaking the way it does on land.
#define PL_SWIM       0.11f   // blocks a tick: about half a walk
#define PL_SWIM_UP    0.030f  // added to vy per tick while jump (or sneak) is held
#define PL_WATER_GRAV 0.008f  // what is left of gravity once the water holds you
#define PL_WATER_DRAG 0.80f   // per tick: water kills a fall in well under a second
#define PL_WATER_TERM 0.50f   // blocks a tick: how deep a running jump can plunge you

// Where the body is tested for being in water. Not the feet, which are
// in the water the moment a toe touches it, and not the eye, which is
// out of it while you are still swimming: the middle.
#define PL_WADE_Y 0.9f

// Survival, in Minecraft's units: 20 is full, and the HUD draws them
// as ten hearts and ten drumsticks. The SYSTEMS that move them --
// starvation, regeneration, fall damage, eating -- are block 12; this
// carries and shows them.
#define PL_HEALTH_MAX 20
#define PL_HUNGER_MAX 20

typedef struct {
    phys_body_t body;
    float       yaw, pitch;
    inventory_t inv;
    int         health, hunger;

    // Breaking is HELD, not tapped: a block takes item_break_ticks() of
    // them, which is what makes hardness and tool choice mean anything
    // and what the crack overlay will animate. Tracked against the cell
    // being mined, so looking away abandons the progress.
    int32_t mine_x, mine_y, mine_z;
    int     mine_ticks;   // ticks spent on that cell
    int     mine_needed;  // ticks it takes, for the progress bar
    bool    mining;

    // The previous tick's pose, so a frame drawn between two ticks can
    // interpolate instead of stepping.
    double prev_x, prev_y, prev_z;
    float  prev_yaw, prev_pitch;

    bool      in_air_last;  // for a landing sound, later
    ray_hit_t aim;          // what the crosshair found this tick
    bool      aim_valid;

    // The player swung at a block that will not break with what they
    // are holding, and THIS is the tool it wants (an item id, 0 for
    // none). Reported so the HUD can name it -- a swing that does
    // nothing and says nothing is a bug as far as anyone can tell.
    uint16_t  needs_tool;

    // The Use key was pressed on a block that opens something (a
    // crafting table; later a furnace or a chest). The block's id, or
    // BLK_AIR for "nothing was used this tick". REPORTED, not acted on:
    // player.c has no business knowing what a screen is, and main.c
    // already owns every other screen in the game.
    uint8_t   used_block;

    // A full-screen UI is up -- the crafting book (ui/craft_ui.h).
    // Mirrored here once a frame by main.c rather than reached for,
    // because player.c has no business knowing what a pax_buf_t is.
    // The effect is the inventory screen's: stand still, keep falling.
    bool      ui_open;
} player_t;

// How far through breaking the aimed block, 0..1. Zero when not mining.
float player_mine_progress(player_t const* p);

// A player who has never played: full health and hunger, the starting
// kit, nothing in progress. Position is left alone -- that is
// player_spawn's or player_place's job.
void player_reset(player_t* p);

// Put the player at (x, z), standing on whatever is there. Only moves
// them: health, hunger and the inventory are untouched.
void player_spawn(player_t* p, double x, double z, float yaw);

// Put the player back EXACTLY where they were -- a cave, a ledge, a
// tower they built. Returns false, and moves nothing, if the body would
// not fit there (a chunk that changed, a save from a build with
// different terrain); the caller then stands them on the ground with
// player_spawn instead.
bool player_place(player_t* p, double x, double y, double z, float yaw, float pitch);

// Advance one simulation tick. `mask` is the tick's input, `pressed`
// the actions that went down since the last tick (so a held key places
// one block, not twenty).
void player_tick(player_t* p, sm_actions_t mask, sm_actions_t pressed);

// The eye for the frame being drawn. `alpha` is how far through the
// current tick it is, 0..1.
void player_eye(player_t const* p, float alpha, double* x, double* y, double* z, float* yaw, float* pitch);
