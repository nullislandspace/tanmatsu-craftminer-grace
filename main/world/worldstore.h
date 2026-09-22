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

#include "items/inventory.h"
#include "items/item_entity.h"
#include "world/chunk.h"
#include "world/farlands.h"

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

// SAVE SLOTS. A world lives in one of CM_SLOTS numbered slots, in the
// directory "slot<n>" (n from 1), and carries the name the player gave
// it in level.cmw. The slot is where it is; the name is what it is
// called -- so renaming a world never moves a file, and a name can be
// anything the keyboard can type.
#define CM_SLOTS 8

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
    // The world's clock in ticks (game/daytime.h): ONE per world, however
    // many players it has had (D-52). Elapsed ticks, advanced only while
    // the world is being played (D-51).
    int64_t  time_of_day;
    // Where this world's Far Lands begin: every column west of this x
    // (farlands.h). Given at creation from FARLANDS_X_DEFAULT and kept
    // for ever after, so a later default reaches new worlds only and
    // never cuts a seam through an old one (D-78). A save from before
    // the Far Lands reads as the default of the build that opens it.
    int32_t  farlands_x;
} world_meta_t;

// Everything about the player that outlives a session. Add fields
// freely: see the header comment on why that is safe.
typedef struct {
    double  x, y, z;
    float   yaw, pitch;
    // The position is one the player actually stood at, not a guess.
    // A new world only knows its spawn column, so the player is stood
    // on the ground there; a saved player is put back EXACTLY where they
    // were, cave or cliff ledge, because the surface above them is not
    // where they left.
    bool    placed;
    int32_t health;
    int32_t hunger;
    int32_t bed_x, bed_y, bed_z;
    bool    has_bed;
    // Where the world's clock used to be kept. Read from old saves only,
    // and moved onto world_meta_t.time_of_day (D-52); no longer written.
    int64_t time_of_day;

    // What they were carrying. Stored by item NAME, so an inventory
    // survives items being added or renumbered (D-31's rule, applied to
    // items). `has_inv` false means nothing was saved -- a new player,
    // who gets the starting kit.
    bool       has_inv;
    inv_slot_t inv[INV_SLOTS];
    int32_t    inv_selected;
} player_state_t;

// What is lying on the ground: every dropped item in the world, saved
// with the world record (D-68) -- position, what it is (by NAME), how
// many, wear, age and pickup delay. Items in chunks that are not loaded
// hold still until they are (item_entity.h), so saving them all at once
// loses nothing.
typedef struct {
    int           n;
    item_entity_t e[ITEM_ENTITY_MAX];
} world_items_t;

// Sensible values for a player who has never played.
void player_state_defaults(player_state_t* p, world_meta_t const* meta);

// --- The store --------------------------------------------------------

// `base` is the player's data directory (CM_DATA_DIR, /sd/craftminer, on
// the badge -- NOT the install directory, which the launcher may empty;
// datadir.h -- and a temporary directory in the host checks). Creates
// <base>/worlds if missing.
bool worldstore_init(char const* base);

// List the worlds, newest played first. Returns how many were filled in.
int worldstore_list(world_meta_t* out, int max);

// Make a new world. `name` is what the player typed; the slug is derived
// from it and made unique. Writes level.cmw and leaves the world OPEN.
bool worldstore_create(char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player);

// --- Save slots ---------------------------------------------------------

// The directory slot `slot` (0-based) lives in: "slot1" for slot 0.
void worldstore_slot_slug(int slot, char* out, int cap);

// What is in a slot, without opening it. True, with `meta` filled in,
// if the slot holds a readable world.
bool worldstore_slot_peek(int slot, world_meta_t* meta);

// What a slot holds, told apart -- because "cannot read it" is not
// "empty". A world saved by a NEWER build (a major version this build
// does not know, D-32) is still somebody's world: the menu must say so,
// not offer the slot as free.
typedef enum {
    SLOT_EMPTY = 0,
    SLOT_WORLD,    // readable; `meta` filled in
    SLOT_NEWER,    // saved by a newer build: leave it alone
    SLOT_OLDER,    // an older major format: needs the upgrader (not written yet)
    SLOT_DAMAGED,  // a level.cmw that cannot be read
} slot_state_t;

slot_state_t worldstore_slot_state(int slot, world_meta_t* meta);

// Make a new world in an EMPTY slot. Leaves it open, like
// worldstore_create. False if the slot is taken.
bool worldstore_create_in(int slot, char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player);

// Give a world a new name. Only level.cmw changes. Safe with another
// world open: that world's state is left alone.
bool worldstore_rename(char const* slug, char const* name);

// Adopt a world saved before there were slots: if <worlds>/<legacy_slug>
// holds one, move it into the first free slot and call it `name`.
// Returns the slot it went to, -1 if there was nothing to adopt (the
// usual case, and the only one on a fresh install), -2 if there was
// something and it could not be moved -- in which case it is left
// exactly where it was.
int worldstore_adopt_legacy(char const* legacy_slug, char const* name);

// Open an existing world: reads level.cmw, builds the block remap.
// `items`, if not NULL, gets what was lying on the ground.
bool worldstore_open(char const* slug, world_meta_t* meta, player_state_t* player, world_items_t* items);

// Open a world that HAS NO DIRECTORY: every chunk is generated on
// demand and nothing is ever written. The title screen's landscape is
// one -- it must not appear in the world list or grow a save -- and so
// is any host test that only needs terrain.
//
// A chunk save SUCCEEDS without writing, deliberately: the streamer
// will not evict a chunk whose save failed, so a refusal would pin
// every edited chunk in the ring forever.
bool worldstore_open_scratch(uint32_t seed, world_meta_t* meta, player_state_t* player);

// Write level.cmw for the open world. Chunks are saved separately, as
// they are evicted (see world_chunk_save). `items` may be NULL: none.
bool worldstore_save(world_meta_t const* meta, player_state_t const* player, world_items_t const* items);

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
