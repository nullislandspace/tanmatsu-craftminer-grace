#pragma once
// =====================================================================
//  SynthMiner  --  light
// ---------------------------------------------------------------------
//  Every cell carries a light byte (chunk_t.lt): SKY light in the high
//  nibble, BLOCK light -- torches -- in the low one, each 0..15. It is
//  derived data: worked out when a chunk becomes resident and kept up to
//  date on every block change, never saved.
//
//    SKY    15 straight down from the open sky until something stops it,
//           then spreading sideways and down, one level less per block.
//           A cave mouth is lit a few blocks in and dark beyond. How
//           bright "15" actually is depends on the time of day, which is
//           applied at DRAW time (chunk_render.c), so night falls without
//           a single chunk being re-meshed.
//    BLOCK  a torch is 14 (block_def()->light) and spreads the same way.
//
//  What light passes through: air, plants, torches and glass freely;
//  leaves take one level more and water two; any other cube stops it.
//
//  THE UPDATE IS STATIC, as the user put it: nothing is recomputed per
//  frame. Placing or removing a block runs the standard two-queue flood
//  -- take away the light that came through the old block, then flood
//  back in from whatever still shines -- over the few hundred cells a
//  change can reach, and marks the meshes it touched stale. A chunk
//  arriving floods its own light in and trades light with the resident
//  chunks beside it, so a torch near a chunk border lights both sides
//  whichever loaded first.
//
//  Runs on the MAIN task, like every other write to a resident chunk
//  (the ownership contract, chunk_worker.h). Pure: the host checks run
//  it.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk.h"

#define LIGHT_MAX 15

static inline uint8_t light_sky(uint8_t l) {
    return (uint8_t)(l >> 4);
}
static inline uint8_t light_block(uint8_t l) {
    return (uint8_t)(l & 0x0Fu);
}

// The work queues. Call once after chunk_store_init(); false if they
// could not be allocated, and then the world is simply fully lit.
bool light_init(void);
void light_shutdown(void);

// The light byte at a world cell. Above the world it is full sky; in an
// unloaded chunk or below the world, 0.
uint8_t world_light(int32_t x, int32_t y, int32_t z);

// A chunk's light comes in two halves, split so the expensive one is
// not on the frame path:
//
//   light_chunk_local  its own sunlight and torchlight, as if the chunk
//                      stood alone (outside it is a wall). ON THE WORKER,
//                      core 1, while the chunk is still CS_LOADING -- it
//                      touches nothing but that chunk.
//   light_chunk_join   the light it trades across its four faces with
//                      the resident chunks next door. On the MAIN task,
//                      as the load is applied: a few hundred cells.
//
// light_chunk_ready does both, for code that has a chunk in hand and no
// worker (the host checks).
void light_chunk_local(chunk_t* c);
void light_chunk_join(chunk_t* c);
void light_chunk_ready(chunk_t* c);

// A block changed at (x, y, z) from `was` to `now`. world_set() calls
// this; it does nothing if the change cannot affect light.
void light_block_changed(int32_t x, int32_t y, int32_t z, uint8_t was, uint8_t now);

// How much a block dims light passing through it: 0 for air, 15 (all of
// it) for an ordinary cube. Exposed for the host checks.
int light_filter(uint8_t block);
