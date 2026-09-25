#pragma once
// =====================================================================
//  SynthMiner  --  the block registry
// ---------------------------------------------------------------------
//  ONE table describes every block: how it meshes, how it looks, how
//  long it takes to break, what it drops, and how it behaves. The mesher
//  (voxel/voxel_mesh.c), the collider, the picker and the interaction
//  code all read it and none of them has a switch over block ids.
//
//  Adding a block is: one row here, one BLK_ id below, one 16x16 PNG
//  (plus its VM_ material and its row in MAT_FILES), one metadata.json
//  line. Nothing else. That is the whole extendability contract
//  (claudeplans/synthminer.md, Part L).
//
//  Pure: no engine, no RTOS, no allocation. tools/worldcheck.c compiles
//  this as-is.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "voxel/voxel_mesh.h"  // vox_mat_t (VM_*), vox_face_t (VF_*)

// How a block meshes. Drives voxel_mesh.c; see its header for what each
// kind emits.
typedef enum {
    K_AIR = 0,  // nothing
    K_CUBE,     // a full cube; hides the faces of its neighbours
    K_SEE,      // a cube with cut-out texels (leaves, glass)
    K_PLANT,    // two crossed double-sided quads (flowers, crops)
    K_TORCH,    // a thin stick in the middle of the cell
    K_SIGN,     // a post with a board on it, facing east, its text a texture
    // A liquid is ONLY ever its surface. It draws no sides and no
    // bottom, and a top only where there is air above it, and it never
    // hides the faces of its neighbours -- so a lake is a lid over
    // terrain you can see into, rather than a solid box with nothing
    // inside it. The engine has no blending, so this is what
    // "transparent" has to mean here (D-86). See voxel_mesh.c.
    K_LIQUID,
} block_kind_t;

// Behaviour flags.
#define BF_SOLID       (1u << 0)  // stops the player and mobs
#define BF_OPAQUE      (1u << 1)  // blocks light (reserved for step 15)
#define BF_FELLABLE    (1u << 2)  // part of a tree: the felling rule applies (Part F)
#define BF_CROP        (1u << 3)  // grows through state bits 1..3
#define BF_GRAVITY     (1u << 4)  // falls when unsupported (sand, gravel)
#define BF_REPLACEABLE (1u << 5)  // a placement overwrites it (air, water, tall grass)
#define BF_LIQUID      (1u << 6)
// A K_SEE block that shows its faces against ITS OWN id: leaves are a
// canopy you can see into, glass hides glass.
#define BF_SEE_SELF    (1u << 7)

// The Use key OPENS this block instead of placing against it: a
// crafting table, and later a furnace, a chest and the benches. The
// player only reports which block was used -- what a screen is and
// how to show one is main.c's business, not the registry's.
//
// It is a second flag word because the first is full: eight bits,
// eight flags, and BF_SEE_SELF took the last one.
#define BF2_USABLE     (1u << 0)

// The block keeps a side record: three slots and a fire for a furnace,
// twenty-seven for a chest (world/blockent.h). Placing one takes a
// record from the pool and breaking one gives it back, contents first.
#define BF2_RECORD     (1u << 1)

// THE BLOCK WILL NOT BREAK AT ALL without a tool of at least its
// `tool_level`. Minecraft's rule is that it breaks and drops nothing,
// and this game used that until iron arrived -- the user asked for the
// stricter one (2026-09-23), so iron ore refuses the swing outright.
//
// A refusal has to SAY SO, or a swing that does nothing and explains
// nothing reads as a bug -- which is exactly why Minecraft chose the
// other rule. See break_result_t.needs_tool.
#define BF2_TOOL_REQUIRED (1u << 2)

// Tool classes. `tool_level` is 0 hand, 1 wood, 2 stone, 3 iron.
typedef enum {
    TOOL_NONE = 0,
    TOOL_PICK,
    TOOL_AXE,
    TOOL_SHOVEL,
    TOOL_SHEARS
} tool_t;

#define HARDNESS_UNBREAKABLE 0xFFFFu
#define ITEM_NONE            0u

// What a block SOUNDS like: walked on, broken, put down. One class per
// material, not one per block, because a footstep on cobble and one on
// coal ore are the same noise. audio/sfx.c turns a class into the three
// sounds; a block that makes none (air, the barrier) is SND_NONE.
//
// The order matters: sfx.h's SFX_STEP_* / SFX_BREAK_* rows run in this
// same order, so the lookup is an addition rather than a switch.
typedef enum {
    SND_NONE = 0,
    SND_SOFT,    // grass, leaves, flowers
    SND_GRAVEL,  // dirt, gravel
    SND_STONE,   // stone, cobble, ore, bedrock
    SND_WOOD,    // log, planks, a sign
    SND_SAND,
    SND_GLASS,
    SND_SPLASH,  // water
    SND_COUNT
} block_sound_t;

typedef struct {
    char const* name;        // stable id; the string a future save format would key on
    uint8_t     kind;        // block_kind_t
    uint8_t     mat[3];      // vox_mat_t for VF_TOP / VF_SIDE / VF_BOTTOM
    uint16_t    hardness;    // ticks to break bare-handed; HARDNESS_UNBREAKABLE never
    uint8_t     tool;        // tool_t that speeds it up
    uint8_t     tool_level;  // minimum level that drops anything at all
    uint16_t    drop_item;   // ITEM_NONE drops nothing
    uint8_t     drop_min, drop_max;
    uint8_t     flags;
    uint8_t     light;       // light emitted, 0..15 (world/light.h)
    uint8_t     growth_max;  // BF_CROP: the highest growth stage
    uint8_t     sound;       // block_sound_t: what it sounds like (audio/sfx.h)
    uint8_t     flags2;      // BF2_*
} block_def_t;

// The block ids -- AND THEY ARE PERMANENT (D-74).
//
// A chunk stores one byte per cell, and every chunk on every card was
// written with these numbers. So once a block has shipped its id NEVER
// changes, its NAME never changes (a world's palette matches blocks by
// name when it opens), and it is never deleted: a block the game stops
// using is RETIRED -- it keeps its row and its number, and nothing else
// ever gets that number. New blocks go at the END, with the next free id,
// wherever they belong in a menu.
//
// tools/ids.txt lists every id and name ever shipped, and `make check`
// fails the build if this table disagrees with it: a renumbered, renamed
// or missing block, or a new one not yet added to the list. Adding a
// block is appending one line there, in the same commit.
//
// 0 is air; 255 is reserved. One byte is 255 blocks for the life of the
// game, retired ones included; past that is a format change (a new major
// version and a one-time upgrade of every world, D-32).
enum {
    BLK_AIR           = 0,
    BLK_GRASS         = 1,
    BLK_DIRT          = 2,
    BLK_STONE         = 3,
    BLK_COBBLE        = 4,
    BLK_SAND          = 5,
    BLK_WATER         = 6,
    BLK_LOG           = 7,
    BLK_PLANKS        = 8,
    BLK_LEAVES        = 9,
    BLK_COAL_ORE      = 10,
    BLK_GLASS         = 11,
    BLK_TORCH         = 12,
    BLK_FLOWER_RED    = 13,
    BLK_FLOWER_YELLOW = 14,
    BLK_TALL_GRASS    = 15,
    // A chunk that is not resident reads as this: solid, unbreakable,
    // never meshed. The player stops at the edge of generated terrain
    // instead of falling through it (D-14). Never stored in a chunk, but
    // it has a number like any other and keeps it.
    BLK_BARRIER = 16,
    BLK_BEDROCK = 17,
    BLK_GRAVEL  = 18,
    // Generated only, for now (D-79): nothing places or writes one, and
    // there is no sign item. Its text follows from where it stands
    // (voxel_mesh.h, voxel_sign_text).
    BLK_SIGN = 19,
    BLK_CRAFTING_TABLE = 20,
    BLK_FURNACE        = 21,
    BLK_IRON_ORE       = 22,
    BLK_CHEST          = 23,
    BLK_TRASH          = 24,
    BLK_BENCH          = 25,
    BLK_BIRCH_LOG      = 26,
    BLK_BIRCH_LEAVES   = 27,
    BLK_CACTUS         = 28,
    BLK_SNOW           = 29,
    BLK_SANDSTONE      = 30,
    // New blocks here: BLK_SOMETHING = 31, and a line in tools/ids.txt.
    BLK_COUNT
};

extern block_def_t const BLOCKS[BLK_COUNT];

// Safe accessor: an id past the table reads as barrier, so a corrupt
// save can never index out of bounds.
static inline block_def_t const* block_def(uint8_t id) {
    return &BLOCKS[id < BLK_COUNT ? id : BLK_BARRIER];
}

static inline uint8_t block_kind(uint8_t id) {
    return block_def(id)->kind;
}
static inline uint8_t block_sound(uint8_t id) {
    return block_def(id)->sound;
}
static inline bool block_solid(uint8_t id) {
    return (block_def(id)->flags & BF_SOLID) != 0;
}
static inline bool block_liquid(uint8_t id) {
    return (block_def(id)->flags & BF_LIQUID) != 0;
}
static inline bool block_replaceable(uint8_t id) {
    return (block_def(id)->flags & BF_REPLACEABLE) != 0;
}
static inline bool block_fellable(uint8_t id) {
    return (block_def(id)->flags & BF_FELLABLE) != 0;
}
static inline bool block_usable(uint8_t id) {
    return (block_def(id)->flags2 & BF2_USABLE) != 0;
}

static inline bool block_keeps_record(uint8_t id) {
    return (block_def(id)->flags2 & BF2_RECORD) != 0;
}

static inline bool block_tool_required(uint8_t id) {
    return (block_def(id)->flags2 & BF2_TOOL_REQUIRED) != 0;
}

// The be_kind_t a block's record is. Not in the table: blockent.h
// includes this header, so the number cannot be named here without a
// cycle -- and there are few enough of these for a line each.
uint8_t block_record_kind(uint8_t id);
