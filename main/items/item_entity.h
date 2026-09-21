#pragma once
// =====================================================================
//  CraftMiner  --  things lying on the ground
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
//  NOT YET PERSISTED. The chunk format reserves SECTION_ENTITIES
//  (chunk_codec.h) and nothing writes it, so drops are lost on a
//  reload. That is block 5's save work, and this struct is already the
//  shape it will be written in.
//
//  Pure: no engine, no allocation. tools/worldcheck.c ticks it.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game/physics.h"
#include "items/inventory.h"

#define ITEM_ENTITY_MAX     96     // pool size; a burst of felled leaves is the peak
#define ITEM_DESPAWN_TICKS  12000u // 10 minutes at 20 Hz
#define ITEM_PICKUP_DELAY   10u    // ticks before it can be picked up, so a break
                                   // does not instantly re-collect what you dropped
#define ITEM_PICKUP_RANGE   1.4f   // blocks, centre to centre
#define ITEM_ENTITY_SIZE    0.25f  // the box it falls with

typedef struct {
    bool        alive;
    uint16_t    item;
    uint8_t     count;
    uint16_t    wear;
    uint32_t    age;    // TICKS elapsed; see the header note
    phys_body_t body;
} item_entity_t;

void item_entity_reset(void);

// Drop `count` of `item` at the centre of block cell (x, y, z), with a
// small scatter so a felled tree does not stack every log on one point.
// Returns how many were actually dropped (the pool can be full).
int item_entity_spawn(int32_t x, int32_t y, int32_t z, uint16_t item, int count, uint16_t wear);

// One tick: fall, age, despawn, and fly into `inv` when the player at
// (px, py, pz) is close enough. Returns how many were picked up.
int item_entity_tick(inventory_t* inv, double px, double py, double pz);

// For the renderer and the tests.
int                  item_entity_live(void);
item_entity_t const* item_entity_at(int i);
