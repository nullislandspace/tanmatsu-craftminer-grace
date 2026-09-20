#pragma once
// =====================================================================
//  CraftMiner  --  worlds on the SD card
// ---------------------------------------------------------------------
//  A world is a directory: its metadata and the player in `level.cmw`,
//  its terrain in `region/r.<rx>.<rz>.cmr`. This owns creating, listing,
//  opening, saving and deleting them, and is the only thing that knows
//  where any of it lives -- the chunk worker asks for a chunk, not for
//  a path.
//
//      <base>/worlds/worlds.idx          an index, an optimisation only
//      <base>/worlds/<slug>/level.cmw    metadata + player, NBT
//      <base>/worlds/<slug>/region/...   terrain
//
//  BOTH FORMATS ARE BUILT TO GROW. That is a requirement, not a nicety:
//  this game will gain player attributes and block types for as long as
//  anyone works on it, and a save that cannot survive that is a save
//  that gets thrown away.
//
//  Player state grows by NBT's nature. Every field is a named, typed
//  tag; the reader defaults the whole struct first, then loops the tags
//  it finds, dispatches the ones it knows and SKIPS the rest
//  (nbt_skip_payload). So an old save loads into a new build with the
//  new fields at their defaults, and a new save loads into an old build
//  with the unknown fields ignored. Adding an attribute is one line in
//  the writer and one case in the reader.
//
//  Block ids grow through a PALETTE. The chunk planes store one byte per
//  cell, so a saved id only means anything next to the table that was
//  current when it was written. `level.cmw` therefore records that
//  table: every block's NAME against the id it had. On open, each name
//  is looked up in today's registry and a remap is built, which
//  chunk_decode applies as it unpacks (chunk_codec.h). Blocks can then
//  be added anywhere, reordered, or removed, and old worlds still load.
//  A name this build no longer has becomes air, and the count is
//  reported rather than hidden.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk.h"

#define CM_WORLD_NAME_MAX 32
#define CM_WORLD_SLUG_MAX 24
// The MAJOR version, and it lives in the file's magic: "CMW" + digit.
// It is bumped only when the layout changes in a way a reader cannot
// absorb -- never for a new field, which tags and NBT handle by
// themselves. A file whose major does not match is refused rather than
// guessed at, so a future upgrader has something definite to act on.
#define CM_LEVEL_MAGIC  "CMW"
#define CM_LEVEL_MAJOR  '1'
#define CM_LEVEL_FORMAT 1
#define CM_WORLDS_MAX     32

// What the world-select screen shows without opening a world.
typedef struct {
    char     slug[CM_WORLD_SLUG_MAX];  // the directory name
    char     name[CM_WORLD_NAME_MAX];  // what the player called it
    uint32_t seed;
    int64_t  created;
    int64_t  last_played;
    uint32_t play_secs;
    int32_t  spawn_x, spawn_y, spawn_z;  // world spawn, chosen at creation
    int32_t  format;                     // CM_LEVEL_FORMAT when written
} world_meta_t;

// Everything about the player that outlives a session. Add fields
// freely: see the header comment on why that is safe.
typedef struct {
    double  x, y, z;
    float   yaw, pitch;
    int32_t health;
    int32_t hunger;
    int32_t bed_x, bed_y, bed_z;
    bool    has_bed;
    int64_t time_of_day;  // ticks since the world's dawn
} player_state_t;

// Sensible values for a player who has never played.
void player_state_defaults(player_state_t* p, world_meta_t const* meta);

// --- The store --------------------------------------------------------

// `base` is the app's install directory (graceloader_get_install_basepath()
// on the badge, a temporary directory in the host checks). Creates
// <base>/worlds if missing.
bool worldstore_init(char const* base);

// List the worlds, newest played first. Returns how many were filled in.
int worldstore_list(world_meta_t* out, int max);

// Make a new world. `name` is what the player typed; the slug is derived
// from it and made unique. Writes level.cmw and leaves the world OPEN.
bool worldstore_create(char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player);

// Open an existing world: reads level.cmw, builds the block remap.
bool worldstore_open(char const* slug, world_meta_t* meta, player_state_t* player);

// Write level.cmw for the open world. Chunks are saved separately, as
// they are evicted (see world_chunk_save).
bool worldstore_save(world_meta_t const* meta, player_state_t const* player);

void worldstore_close(void);
bool worldstore_delete(char const* slug);

// --- Chunks in the open world ----------------------------------------
//
// The palette remap is applied here, so nothing above this ever handles
// a stale block id.

int  world_chunk_load(chunk_t* c);       // 1 read, 0 not stored, -1 error
bool world_chunk_save(chunk_t const* c);

// Compact the region a chunk belongs to, if rewrites have left it more
// than half dead bytes. Call after saving, on the worker -- it rewrites
// a whole file (43 ms measured, F-26) and must never be on the frame
// path. Returns true if it actually compacted.
bool world_region_maintain(int32_t cx, int32_t cz);

// How many cells of the last load were blocks this build no longer has
// (they became air). For a warning at load time, not for logic.
int worldstore_unknown_blocks(void);
