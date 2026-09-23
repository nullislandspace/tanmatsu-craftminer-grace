#pragma once
// =====================================================================
//  CraftMiner  --  blocks that remember more than a byte
// ---------------------------------------------------------------------
//  THE THIRD TIER (chunk.h). A cell is a block id and a state byte, and
//  that is enough for most of the world -- which way a sign faces, how
//  grown a crop is, who placed a log. It is nowhere near enough for a
//  furnace, which has to remember three stacks of items, how much fuel
//  is still alight and how far through the current item it is.
//
//  So those blocks get a SIDE RECORD, keyed by where they stand and
//  saved with the chunk they are in (chunk_codec.h,
//  SECTION_BLOCK_ENTITIES -- a section reserved when the format was
//  designed and, until the furnace, never written to).
//
//  A FIXED GLOBAL POOL, NOT A LIST PER CHUNK. The decode runs on the
//  core-1 worker and Part K's contract is that the worker does not
//  allocate; a pool taken once at world open means a chunk arriving
//  never has to. Records find their chunk by their own coordinates, and
//  BE_MAX of them across the resident ring is far more containers than
//  anyone will build within sight of themselves.
//
//  THE RACE THAT ISN'T. The worker serialises a chunk's records while
//  the main task could in principle be stirring a furnace. It cannot:
//  a chunk is only saved when it is being evicted or when the world is
//  being saved, and a furnace can only be opened when the player is
//  standing next to it -- which is to say in a chunk that is resident
//  and nowhere near eviction. The residency contract already separates
//  them, so there is no lock here and should not be one.
//
//  Pure: no engine, no RTOS. The one allocation goes through psram.h,
//  which is the sanctioned seam. tools/worldcheck.c builds it as-is.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "items/inventory.h"
#include "world/chunk.h"

// Containers within reach at once. A furnace is 3 slots and a chest 27,
// but a record is one size for all of them -- a pool of mixed-size
// records is a heap, and a heap is the thing this is avoiding.
#define BE_MAX   192
#define BE_SLOTS 27

typedef enum {
    BE_NONE = 0,
    BE_FURNACE,
    BE_CHEST,
    BE_TRASH,
} be_kind_t;

typedef struct {
    int32_t    x, y, z;
    uint8_t    kind;              // be_kind_t; BE_NONE means the pool slot is free
    inv_slot_t slot[BE_SLOTS];

    // WHEN THIS RECORD WAS LAST BROUGHT UP TO DATE, in world ticks.
    // Everything here is worked out lazily from `now - stamp` when the
    // player opens it (Part C): no furnace ticks, no catch-up pass, and
    // a furnace in a chunk nobody has visited costs nothing at all.
    uint32_t   stamp;

    uint16_t   burn_left;  // furnace: ticks of fuel still alight
    uint16_t   burn_max;   // ... and what it started at, for the flame
    uint16_t   cook;       // furnace: ticks into the item being smelted
} blockent_t;

// The slots a furnace uses, by name. A chest uses all BE_SLOTS.
#define BE_FURNACE_INPUT  0
#define BE_FURNACE_FUEL   1
#define BE_FURNACE_OUTPUT 2

// Take the pool (once, at world open) and give it back. Safe to call
// twice either way.
bool blockent_init(void);
void blockent_shutdown(void);

// Everything forgotten, for a world being closed or swapped.
void blockent_clear(void);

// The record at (x, y, z), or NULL. A walk over the pool -- see the
// note in blockent.c for why that is the right shape here.
blockent_t* blockent_at(int32_t x, int32_t y, int32_t z);

// Make one. Returns NULL if the pool is full, which the caller must
// report rather than swallow -- a chest that silently did not become a
// chest is a chest somebody is about to put their things in.
blockent_t* blockent_add(int32_t x, int32_t y, int32_t z, uint8_t kind);

// Forget the one at (x, y, z), if any. What breaking the block does.
void blockent_remove(int32_t x, int32_t y, int32_t z);

// SAY THAT A RECORD HAS CHANGED. Call it after putting anything into a
// furnace or a chest, or after taking anything out.
//
// This exists because of a hole that is easy to miss and expensive to
// find: a chunk is only written to the card when it is marked EDITED,
// and only world_set() marks it -- so smelting, which changes what is
// in a block without changing any block, would be lost on the next
// eviction, and nobody would notice until they walked back to their
// furnace. Nothing else here can mark it, because a record does not
// know it is in a chunk until it is asked.
void blockent_touch(blockent_t const* be);

// Forget every record in a chunk: what EVICTION does, after the save.
void blockent_drop_chunk(int32_t cx, int32_t cz);

// How many records are live, and how many are in one chunk. For the
// checks and the memory report.
int blockent_count(void);
int blockent_count_in(int32_t cx, int32_t cz);

// --- Saving -----------------------------------------------------------

// Write the records of (cx, cz) as a SECTION_BLOCK_ENTITIES section,
// header included, ready to hand to chunk_encode. Returns the bytes
// written, or 0 if there are none (in which case nothing is written and
// the chunk simply has no such section).
size_t blockent_encode_chunk(int32_t cx, int32_t cz, uint8_t* out, size_t cap);

// Read one back. `data` and `len` are the section's CONTENTS, as
// chunk_decode_ex hands them over. Records whose kind this build does
// not know are skipped, which is the whole point of the tagged format.
void blockent_decode_section(uint8_t const* data, size_t len);
