#pragma once
// =====================================================================
//  CraftMiner  --  the block registry
// ---------------------------------------------------------------------
//  ONE table describes every block: how it meshes, how it looks, how
//  long it takes to break, what it drops, and how it behaves. The mesher
//  (voxel/voxel_mesh.c), the collider, the picker and the interaction
//  code all read it and none of them has a switch over block ids.
//
//  Adding a block is: one row here, one BLK_ id below, one 16x16 PNG
//  (plus its VM_ material and its row in MAT_FILES), one metadata.json
//  line. Nothing else. That is the whole extendability contract
//  (claudeplans/craftminer.md, Part L).
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
    uint8_t     light;       // light emitted, 0..15 (reserved for step 15)
    uint8_t     growth_max;  // BF_CROP: the highest growth stage
} block_def_t;

// The block ids. 0 is air; 255 is reserved. Order is free -- nothing
// depends on it but the save format's numbers, which is why `name` is
// there to migrate from if the order ever has to change.
enum {
    BLK_AIR = 0,
    BLK_GRASS,
    BLK_DIRT,
    BLK_STONE,
    BLK_COBBLE,
    BLK_SAND,
    BLK_WATER,
    BLK_LOG,
    BLK_PLANKS,
    BLK_LEAVES,
    BLK_COAL_ORE,
    BLK_GLASS,
    BLK_TORCH,
    BLK_FLOWER_RED,
    BLK_FLOWER_YELLOW,
    BLK_TALL_GRASS,
    // A chunk that is not resident reads as this: solid, unbreakable,
    // never meshed. The player stops at the edge of generated terrain
    // instead of falling through it (D-14).
    BLK_BARRIER,
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
static inline bool block_solid(uint8_t id) {
    return (block_def(id)->flags & BF_SOLID) != 0;
}
static inline bool block_replaceable(uint8_t id) {
    return (block_def(id)->flags & BF_REPLACEABLE) != 0;
}
static inline bool block_fellable(uint8_t id) {
    return (block_def(id)->flags & BF_FELLABLE) != 0;
}
