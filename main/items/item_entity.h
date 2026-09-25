#pragma once
// =====================================================================
//  SynthMiner  --  things lying on the ground
// ---------------------------------------------------------------------
//  The first thing in this world that is neither a block nor the
//  player, so the shape here is the shape every pig, cow and zombie
//  will reuse: a fixed pool, a tick that runs over the live ones, and
//  no allocation ever.
//
//  AGE IS IN TICKS ELAPSED, not seconds and not a timestamp (D-51).
//  A dropped item lives ITEM_DESPAWN_TICKS -- ten minutes at 20 Hz --
//  and the counter only advances while its chunk is being simulated.
//  Store a spawn time instead and a chunk that was unloaded for an hour
//  hands back items that expired while nothing was looking at them;
//  store seconds instead and pausing the game ages them. Walking away
//  must not cost the player their drops. Idling next to them is what
//  does.
//
//  SAVED WITH THE WORLD (D-68): every live item goes into level.smw on
//  a save, and comes back when the world is opened. An item whose chunk
//  is not loaded holds still -- no falling, no ageing, no pickup -- so
//  keeping them all in one list loses nothing by it.
//
//  Pure: no engine, no allocation. tools/worldcheck.c ticks it.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game/physics.h"
#include "items/inventory.h"

#define ITEM_ENTITY_MAX     96     // pool size; a burst of felled leaves is the peak
#define ITEM_DESPAWN_TICKS  12000u // 10 minutes at 20 Hz
#define ITEM_PICKUP_DELAY   10u    // ticks before a BLOCK's drop can be collected
// ... and much longer for one the player threw. A drop lands inside
// the pickup radius whatever direction you face -- the radius is 1.4
// blocks and you cannot throw a thing further than your own arm in one
// tick -- so a short delay means G appears to do nothing at all: the
// item leaves and is collected again half a second later. Two seconds
// is long enough to walk away from, and it is Minecraft's number.
#define ITEM_THROW_DELAY    40u
#define ITEM_PICKUP_RANGE   1.4f   // blocks, centre to centre
#define ITEM_ENTITY_SIZE    0.25f  // the box it falls with

typedef struct {
    bool        alive;
    uint16_t    item;
    uint8_t     count;
    uint16_t    wear;
    uint32_t    age;        // TICKS elapsed; see the header note
    uint32_t    pickup_at;  // age at which it may be collected
    phys_body_t body;
} item_entity_t;

void item_entity_reset(void);

// Drop `count` of `item` at the centre of block cell (x, y, z), with a
// small scatter so a felled tree does not stack every log on one point.
// Returns how many were actually dropped (the pool can be full).
int item_entity_spawn(int32_t x, int32_t y, int32_t z, uint16_t item, int count, uint16_t wear);

// Throw one, from a point rather than a cell: what the player does
// with G. `pickup_at` keeps it on the ground long enough to walk away
// from, which an ordinary block drop does not need.
int item_entity_throw(double x, double y, double z, uint16_t item, int count, uint16_t wear, float vx, float vy,
                      float vz, uint32_t pickup_delay);

// One tick: fall, age, despawn, and fly into `inv` when the player at
// (px, py, pz) is close enough. Returns how many were picked up.
int item_entity_tick(inventory_t* inv, double px, double py, double pz);

// Every live item, copied into `out` (at most `max`): what a save
// writes. Returns how many.
int item_entity_copy(item_entity_t* out, int max);

// Empty the pool and fill it with `in`: what opening a world does.
void item_entity_restore(item_entity_t const* in, int n);

// For the renderer and the tests.
int                  item_entity_live(void);
item_entity_t const* item_entity_at(int i);
