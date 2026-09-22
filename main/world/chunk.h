#pragma once
// =====================================================================
//  CraftMiner  --  chunks and the resident world
// ---------------------------------------------------------------------
//  The world is unbounded in x and z and 64 blocks tall. It exists on
//  disk as generated-and-edited chunks and in memory as the few hundred
//  around the player. This file owns that resident set and is the only
//  way anything reads or writes a block.
//
//  LAYOUT. Two parallel planes of one byte per cell, column-major --
//  the same order voxel_mesh.c wants, so a chunk's own bytes can be
//  copied into the mesher's grid a column at a time and `vox_grid_t`
//  never changes (D-12).
//
//  THE STORE is a 16x16 ring indexed by the low bits of the chunk
//  coordinates, with the full coordinates stored alongside and checked
//  on every lookup (D-13). world_block() is the hottest function in the
//  program -- the mesher's border fetches, collision, block picking and
//  the tree flood-fill all go through it -- and this makes it two ANDs
//  and a compare. A chunk whose slot has been taken over by another
//  simply reads as absent, which is safe because eviction runs two
//  chunks further out than loading (hysteresis), so a wanted slot is
//  free before it is wanted.
//
//  NOT RESIDENT READS AS BLK_BARRIER (D-14): solid and unbreakable. The
//  player stops at the edge of the generated world instead of falling
//  through it, and no caller needs a "might be missing" branch.
//
//  THREADING. Only the main task writes `id` / `st` of a CS_READY
//  chunk; the core-1 worker writes them only while CS_LOADING, when
//  nothing else may look. See claudeplans/craftminer.md, Part K, for
//  the full ownership contract -- there are no locks on this path and
//  the rules are what keep it correct.
//
//  Pure: no engine, no RTOS. tools/worldcheck.c builds this as-is.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "math/mesh.h"
#include "world/blocks.h"

// --- Dimensions -------------------------------------------------------
//
// 16 x 16 keeps VOX_CHUNK == 16, so the donor mesher's greedy mask
// sizing is unchanged. 64 tall is the compromise (D-11): the showreel's
// 32 leaves no room for mining under a build, and 128 doubles both the
// memory and the meshing for sky nobody visits.

#define CH_W     16
#define CH_H     64
#define CH_D     16
#define CH_CELLS (CH_W * CH_H * CH_D)

#define CH_SEA_LEVEL 24
#define CH_BEDROCK   0

// --- Render sections --------------------------------------------------
//
// STORAGE is one 64-tall column (above). RENDERING is not: a chunk's
// mesh is cut into CH_SECT_N boxes of CH_SECT blocks, each with its own
// bounding box, so the frustum test can throw away the underground
// half instead of dragging it through the rasteriser. Measured cause:
// 68% of a chunk's triangles are cave walls nobody can see (F-33); the
// fix is D-34.
//
// This is a render-side split only. The planes, the codec, the region
// files and the save format all still see one column, and nothing on
// disk changes.
//
// 16 is the natural size: it matches x and z, it divides 64 exactly,
// and it is the granularity a block edit dirties -- placing a torch
// remeshes 4096 cells instead of 16384.
#define CH_SECT   16
#define CH_SECT_N (CH_H / CH_SECT)

static inline int ch_sect_of(int y) {
    return y / CH_SECT;
}

// A cell's index. Columns are contiguous: (x, z) picks a column, y runs
// along it. This is the mesher's order (voxel_mesh.h), not an accident.
#define CH_IDX(x, y, z) ((((size_t)(z) * CH_W + (size_t)(x)) * CH_H) + (size_t)(y))

// World coordinate -> chunk coordinate / offset within the chunk.
// Arithmetic shift and mask, so they are correct for negative x and z
// (a plain division would round towards zero and put x = -1 in chunk 0).
#define CH_SHIFT 4
static inline int32_t chunk_of(int32_t w) {
    return w >> CH_SHIFT;
}
static inline int chunk_off(int32_t w) {
    return (int)(w & (CH_W - 1));
}

// --- The state byte ---------------------------------------------------
//
// One byte per cell, split one way for everybody and one way per block:
//
//   bit 0     ST_PLACED   universal, and the only bit with a fixed
//                         meaning across all blocks.
//   bits 1-7  block data  0..127, meaning belongs to the block type.
//
// That is the middle of three tiers, and the split matters because the
// tiers cost wildly different amounts:
//
//   1. THE BLOCK ID alone -- stone, dirt, planks. Most of the world.
//   2. ID + 7 BITS OF DATA -- a crop's growth stage, which way a stair
//      or a rail faces, a redstone wire's power, whether a furnace is
//      lit. 128 values is far more than the 4 bits Minecraft managed
//      with for years, and it costs nothing: the byte is already there
//      and RLEs to nothing, because it is zero almost everywhere.
//   3. A BLOCK ENTITY -- a furnace's three slots and its burn timer, a
//      chest's twenty-seven, a sign's text. Variable-sized, rare, and
//      so stored per chunk in a side list rather than per cell
//      (chunk_codec.h, SECTION_BLOCK_ENTITIES).
//
// Creatures and dropped items are not blocks at all and live in the
// chunk's entity list (SECTION_ENTITIES), which is how a cow stays in
// the field it was left in.
//
// Set on every block a player places, never by generation. The logging
// rule reads it: breaking a placed log drops that log, breaking a grown
// one fells the tree (claudeplans/craftminer.md, Part F). It also gives
// "placed leaves never decay" for free when decay arrives.
#define ST_PLACED 0x01u

#define ST_DATA_SHIFT 1
#define ST_DATA_MASK  0xFEu  // bits 1..7: the block type's own 0..127
#define ST_DATA_MAX   127u

// The block's own data field. What it means is the block type's
// business -- see the BF_CROP / facing / power conventions in blocks.h.
static inline uint8_t st_data(uint8_t st) {
    return (uint8_t)((st & ST_DATA_MASK) >> ST_DATA_SHIFT);
}
static inline uint8_t st_with_data(uint8_t st, uint8_t d) {
    return (uint8_t)((st & (uint8_t)~ST_DATA_MASK) | ((uint8_t)(d << ST_DATA_SHIFT) & ST_DATA_MASK));
}

// Crops keep their growth stage in that field; named separately because
// the tick and the drop table both read it and "data" would not say so.
static inline uint8_t st_growth(uint8_t st) {
    return st_data(st);
}
static inline uint8_t st_with_growth(uint8_t st, uint8_t g) {
    return st_with_data(st, g);
}

// --- A chunk ----------------------------------------------------------

typedef enum {
    CS_FREE = 0,  // the slot holds nothing
    CS_LOADING,   // the worker is filling it; main must not look
    CS_READY,     // resident and readable
    CS_SAVING,    // resident and readable; a save is in flight
} chunk_state_t;

#define CF_GENERATED 0x01u  // terrain exists (as opposed to loaded-from-disk)
#define CF_EDITED    0x02u  // differs from what is on disk: must be written before eviction

// Levels of detail, nearest first. The same mesh serves FAST and the
// flat far view; COARSE is a half-resolution grid (voxel_mesh.h).
typedef enum {
    LOD_FANCY = 0,
    LOD_FAST,
    LOD_COARSE,
    LOD_COUNT
} chunk_lod_t;

// A chunk keeps one mesh per (level, section). There are 12, which fits
// a 16-bit mask exactly -- so "which are stale" and "which are queued"
// are each one word rather than an array to walk.
#define CH_MESH_N              (LOD_COUNT * CH_SECT_N)
#define CH_MESH_IDX(lod, sect) ((lod) * CH_SECT_N + (sect))
#define CH_MESH_BIT(lod, sect) ((uint16_t)1u << CH_MESH_IDX(lod, sect))
#define CH_MESH_ALL            ((uint16_t)((1u << CH_MESH_N) - 1u))

typedef struct {
    uint8_t* id;  // CH_CELLS block ids -- the mesher's input, verbatim
    uint8_t* st;  // CH_CELLS state bytes
    uint8_t* lt;  // CH_CELLS light bytes: sky << 4 | block (light.h). Derived, never saved

    int32_t cx, cz;
    uint8_t cstate;    // chunk_state_t
    uint8_t flags;     // CF_*
    uint8_t edit_seq;  // bumped on every write; a stale mesh result is dropped

    uint8_t top[CH_W * CH_D];  // highest non-air y + 1 per column: ground queries, AABB
    uint8_t top_max;           // the tallest of those, for the chunk's bounding box
    uint8_t bottom;            // lowest y that can have a face: tightens the render AABB

    // CH_MESH_N meshes, indexed by CH_MESH_IDX. They live in the
    // store's slab rather than in this struct: 12 mesh_t per slot times
    // 256 slots is PSRAM's business, and chunk_t is a static array.
    mesh_t*  lod;
    // THREE BITMASKS, AND THE DISTINCTION MATTERS. "Stale" and "never
    // built" are not the same state, and treating them as one is what
    // made the world blink every time a block was broken: an edit
    // marks every level of its section stale, and if stale meant
    // undrawable the whole chunk vanished until the worker caught up.
    // A mesh one block out of date is worth drawing. A mesh that does
    // not exist is not.
    uint16_t lod_built;     // CH_MESH_BIT: has real geometry, drawable
    uint16_t lod_stale;     // CH_MESH_BIT: needs (re)building
    uint16_t lod_inflight;  // CH_MESH_BIT: queued to the worker
    uint32_t last_seen_frame;
} chunk_t;

// The mesh for one level of detail of one vertical section.
static inline mesh_t* chunk_mesh(chunk_t* c, int lod, int sect) {
    return &c->lod[CH_MESH_IDX(lod, sect)];
}

// --- The resident set -------------------------------------------------

#define CH_RING       16  // 16 x 16 slots
#define CH_SLOT_COUNT (CH_RING * CH_RING)

// The largest residency radius this ring can hold, and it is a HARD
// limit, not a guideline.
//
// A radius R keeps a square 2R+1 chunks on a side. The slot is the low
// four bits of the coordinate, so two chunks 16 apart land on the same
// one. At 2R+1 > CH_RING that is guaranteed to happen inside the
// resident set: the two fight over the slot, each evicting the other,
// each then read as BLK_BARRIER and requested again -- a world that
// reloads itself forever, and does it while the player is standing
// still. R = 7 is the largest that cannot.
//
// chunk_render_set_view() clamps to this, and tools/worldcheck.c
// asserts every view preset obeys it, so the two cannot drift apart
// again.
#define CH_EVICT_MAX ((CH_RING - 1) / 2)

// Allocate the slab: every plane for every slot, once, at boot. Nothing
// under here allocates again, so there is no fragmentation and the
// worst case is known at start. Returns false if PSRAM is short.
bool chunk_store_init(void);
void chunk_store_shutdown(void);

// Drop every resident chunk WITHOUT saving. For changing worlds: the
// title screen's scratch terrain must not still be in the ring when a
// real world opens, or the player spawns inside somebody else's hill.
//
// The caller must have drained the worker first -- a load still in
// flight would land in a slot this has just freed.
void chunk_store_clear(void);

// How much the slab took, for the boot log and step 1.1's measurement.
// Fixed at boot: it does not depend on the view distance.
size_t chunk_store_bytes(void);

// The PSRAM the built meshes are holding right now, which is the part
// that DOES grow with view distance -- the slab is the same 8 MiB
// whether you can see three chunks or six. Capacity, not use: mesh.c
// grows its arrays and never gives the space back, so this is what is
// actually held. Also reports how many meshes are non-empty and how
// many chunks are resident.
size_t chunk_store_mesh_bytes(int* meshes, int* chunks);

// The slot a chunk coordinate maps to, whatever is in it.
static inline int chunk_slot(int32_t cx, int32_t cz) {
    return (int)(((uint32_t)cx & (CH_RING - 1)) * CH_RING + ((uint32_t)cz & (CH_RING - 1)));
}

// The resident chunk at (cx, cz), or NULL. Cheap enough to call per
// block: two ANDs, a multiply-add and a compare.
chunk_t* chunk_find(int32_t cx, int32_t cz);

// Claim the slot for (cx, cz) and mark it CS_LOADING, evicting whatever
// was there (which must already have been saved). NULL if the slot is
// busy -- the caller retries next frame rather than blocking.
chunk_t* chunk_claim(int32_t cx, int32_t cz);

// Iterate the resident set (for saving everything, or eviction sweeps).
chunk_t* chunk_slot_at(int index);

// The slot holding (cx, cz) WHATEVER its state -- including CS_LOADING,
// which chunk_find() deliberately hides so that no game code can read a
// half-filled chunk. The worker needs it precisely because it is the
// one filling it.
chunk_t* chunk_slot_claimed(int32_t cx, int32_t cz);

// --- Reading and writing the world ------------------------------------
//
// World coordinates. Out of the world vertically reads as air above and
// bedrock below; out of the resident set reads as BLK_BARRIER.

uint8_t world_block(int32_t x, int32_t y, int32_t z);
uint8_t world_state(int32_t x, int32_t y, int32_t z);

// Write a block (and its state). Bumps the chunk's edit_seq, marks it
// CF_EDITED, and marks the affected meshes stale -- including the
// neighbouring chunk's when the cell sits on a border, or the face
// between them would not be rebuilt. No-op outside the world or on a
// chunk that is not resident.
void world_set(int32_t x, int32_t y, int32_t z, uint8_t block, uint8_t state);

// Every mesh that could show the cell (x, y, z) is out of date: its
// section, the section next door when it sits on a section's edge, and
// the neighbouring chunk when it sits on a chunk's. Bumps edit_seq on
// each, so a mesh already in flight is not accepted over the change.
// world_set does this; light.c does it for a cell whose light changed.
void world_mark_dirty(int32_t x, int32_t y, int32_t z);

// The y a body standing at (x, z) rests on: one above the highest solid
// block. 0 if the column is empty, CH_H if it is full.
int world_ground(int32_t x, int32_t z);

// Is this block something to stand on / collide with?
static inline bool world_solid_at(int32_t x, int32_t y, int32_t z) {
    return block_solid(world_block(x, y, z));
}

// Recompute a chunk's `top` and `bottom` summaries. Called after
// generation or loading; world_set() maintains them incrementally.
void chunk_resummarise(chunk_t* c);
