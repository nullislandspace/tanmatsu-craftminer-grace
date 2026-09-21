#pragma once
// =====================================================================
//  CraftMiner  --  a body moving through blocks
// ---------------------------------------------------------------------
//  An axis-aligned box swept against the voxel world. The player is one
//  of these; so is every animal and mob later, which is why nothing
//  here knows what a player is.
//
//  AXIS AT A TIME, IN SUB-STEPS. Each axis is moved and resolved on its
//  own -- the standard voxel approach, and the reason a body slides
//  along a wall instead of stopping dead against it. Each move is cut
//  into steps no longer than PHYS_SUBSTEP so that nothing can pass
//  through a block between two samples, however fast it is going. That
//  matters more here than in most games: a fall from build height
//  reaches 0.9 blocks a tick, which is more than half the player's own
//  height.
//
//  STEP-UP is what makes a one-block rise walkable without jumping. A
//  body that is blocked horizontally AND standing on something tries
//  the same move again from up to PHYS_STEP higher, and keeps it only
//  if that succeeds and it can settle back down.
//
//  AN UNLOADED CHUNK IS SOLID (D-14), because world_block() says so.
//  The player walks to the edge of the generated world and stops, which
//  is the behaviour that needs no special case anywhere else.
//
//  Pure: no engine, no RTOS, no allocation. tools/worldcheck.c runs the
//  collision tests against this directly.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// The player's box, in blocks. Minecraft's, because its dimensions are
// what the block puzzles people expect are built around: a 1-block gap
// is crawlable only by crouching, a 2-block gap walkable.
#define PHYS_PLAYER_W   0.6f
#define PHYS_PLAYER_H   1.8f
#define PHYS_PLAYER_EYE 1.62f  // above the feet

#define PHYS_SUBSTEP 0.25  // blocks: the longest move made without a test

// How high a body walks up without jumping. A WHOLE BLOCK, which is a
// deliberate difference from Minecraft's 0.6 -- there it buys stairs
// and slabs and nothing else, and you jump for a full block.
//
// This is a handheld with a keyboard and no mouse. Having to tap jump
// at every clod of terrain is tiring in a way it is not with a hand
// already on a mouse, and there are no stairs yet for 0.6 to be the
// right number for. Walls are still walls: a step is only kept if the
// body can settle onto something afterwards, so two blocks stops you.
#define PHYS_STEP 1.0f
#define PHYS_SKIN    0.001 // gap left at a contact face, so "touching" is never "inside"

// A body. Position is the CENTRE of the box in x and z and its BOTTOM
// in y -- the feet -- because that is what the ground query, the
// spawn point and the save format all want to talk about.
typedef struct {
    double x, y, z;
    float  vx, vy, vz;  // blocks per tick
    float  w, h;        // the box: w across in both x and z, h tall
    bool   on_ground;   // resting on something as of the last move
    bool   hit_x, hit_z, hit_head;  // what stopped it, for step-up and for sound later
} phys_body_t;

// Start a body at (x, y, z) with the player's dimensions.
void phys_body_init(phys_body_t* b, double x, double y, double z);

// Move by (dx, dy, dz) blocks, resolving against the world. Updates
// `on_ground` and the hit flags. Velocity is the caller's business:
// this moves, it does not integrate.
void phys_move(phys_body_t* b, double dx, double dy, double dz);

// One tick of falling, applied AFTER the body has moved.
//
// The order is the whole point of this being a function rather than
// three lines in the caller: a body moves with the velocity it HAS,
// and only then is the velocity updated for the next tick. Applying
// gravity first instead spends the first tick of a jump decelerating,
// which costs a third of the height -- the same three constants give
// an apex of 0.83 blocks that way and 1.33 this way, and nothing in
// the code says so. Stated once, here, where the host test and the
// player both call it.
//
// Also clears a velocity the body cannot use: landing zeroes a
// downward one, hitting a ceiling an upward one. Without that, a body
// standing still accumulates fall speed and shoots off the moment the
// floor is broken out from under it.
void phys_gravity(phys_body_t* b, float gravity, float drag, float terminal);

// Would the body fit here, with nothing solid overlapping it? Used to
// place a player at spawn without dropping them inside a hill.
bool phys_fits(phys_body_t const* b, double x, double y, double z);

// The lowest y at or below `y` where the body fits standing on
// something solid, searching down at most `max_drop` blocks. Returns
// `y` unchanged if there is nothing to stand on.
double phys_settle(phys_body_t const* b, double x, double y, double z, int max_drop);
