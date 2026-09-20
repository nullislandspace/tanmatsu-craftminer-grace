// =====================================================================
//  CraftMiner  --  the block table (see blocks.h)
// ---------------------------------------------------------------------
//  Hardness is in 20 Hz ticks, bare-handed. Rough scale: 5 = instant
//  (a flower), 20 = one second (dirt), 150 = seven seconds (stone by
//  hand). A tool of the right class divides it; step 4 owns that maths.
//
//  `drop_item` is 0 (ITEM_NONE) everywhere until the item registry
//  lands in step 4. Mining still works before then -- it just drops
//  nothing -- which is exactly what step 3 needs.
// =====================================================================

#include "world/blocks.h"

#define M3(t, s, b) \
    { (t), (s), (b) }
#define M1(m) \
    { (m), (m), (m) }

block_def_t const BLOCKS[BLK_COUNT] = {
    [BLK_AIR] = {.name = "air", .kind = K_AIR, .mat = M1(0), .hardness = HARDNESS_UNBREAKABLE, .flags = BF_REPLACEABLE},

    [BLK_GRASS] = {.name     = "grass",
                   .kind     = K_CUBE,
                   .mat      = M3(VM_GRASS_TOP, VM_GRASS_SIDE, VM_DIRT),
                   .hardness = 20,
                   .tool     = TOOL_SHOVEL,
                   .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_DIRT] = {.name     = "dirt",
                  .kind     = K_CUBE,
                  .mat      = M1(VM_DIRT),
                  .hardness = 20,
                  .tool     = TOOL_SHOVEL,
                  .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_STONE] = {.name       = "stone",
                   .kind       = K_CUBE,
                   .mat        = M1(VM_STONE),
                   .hardness   = 150,
                   .tool       = TOOL_PICK,
                   .tool_level = 1,
                   .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_COBBLE] = {.name       = "cobblestone",
                    .kind       = K_CUBE,
                    .mat        = M1(VM_COBBLE),
                    .hardness   = 160,
                    .tool       = TOOL_PICK,
                    .tool_level = 1,
                    .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_SAND] = {.name     = "sand",
                  .kind     = K_CUBE,
                  .mat      = M1(VM_SAND),
                  .hardness = 15,
                  .tool     = TOOL_SHOVEL,
                  .flags    = BF_SOLID | BF_OPAQUE | BF_GRAVITY},

    // Opaque, as in Minecraft's "fast" graphics: the engine has no
    // blending, so a see-through liquid is not on the table.
    [BLK_WATER] = {.name     = "water",
                   .kind     = K_CUBE,
                   .mat      = M1(VM_WATER),
                   .hardness = HARDNESS_UNBREAKABLE,
                   .flags    = BF_OPAQUE | BF_REPLACEABLE | BF_LIQUID},

    [BLK_LOG] = {.name     = "log",
                 .kind     = K_CUBE,
                 .mat      = M3(VM_LOG_TOP, VM_LOG_SIDE, VM_LOG_TOP),
                 .hardness = 40,
                 .tool     = TOOL_AXE,
                 .flags    = BF_SOLID | BF_OPAQUE | BF_FELLABLE},

    [BLK_PLANKS] = {.name     = "planks",
                    .kind     = K_CUBE,
                    .mat      = M1(VM_PLANKS),
                    .hardness = 40,
                    .tool     = TOOL_AXE,
                    .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_LEAVES] = {.name     = "leaves",
                    .kind     = K_SEE,
                    .mat      = M1(VM_LEAVES),
                    .hardness = 8,
                    .tool     = TOOL_SHEARS,
                    .flags    = BF_SOLID | BF_FELLABLE | BF_SEE_SELF},

    [BLK_COAL_ORE] = {.name       = "coal_ore",
                      .kind       = K_CUBE,
                      .mat        = M1(VM_COAL),
                      .hardness   = 200,
                      .tool       = TOOL_PICK,
                      .tool_level = 1,
                      .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_GLASS] = {.name = "glass", .kind = K_SEE, .mat = M1(VM_GLASS), .hardness = 12, .flags = BF_SOLID},

    [BLK_TORCH] = {.name = "torch", .kind = K_TORCH, .mat = M1(VM_TORCH), .hardness = 1, .light = 14},

    [BLK_FLOWER_RED] =
        {.name = "flower_red", .kind = K_PLANT, .mat = M1(VM_FLOWER_RED), .hardness = 1, .flags = BF_REPLACEABLE},

    [BLK_FLOWER_YELLOW] =
        {.name = "flower_yellow", .kind = K_PLANT, .mat = M1(VM_FLOWER_YELLOW), .hardness = 1, .flags = BF_REPLACEABLE},

    [BLK_TALL_GRASS] =
        {.name = "tall_grass", .kind = K_PLANT, .mat = M1(VM_TALL_GRASS), .hardness = 1, .flags = BF_REPLACEABLE},

    // Never generated, never placed, never meshed: what world_block()
    // answers for a chunk that is not resident (D-14).
    [BLK_BARRIER] = {.name     = "barrier",
                     .kind     = K_CUBE,
                     .mat      = M1(VM_STONE),
                     .hardness = HARDNESS_UNBREAKABLE,
                     .flags    = BF_SOLID | BF_OPAQUE},
};
