// =====================================================================
//  CraftMiner  --  the block table (see blocks.h)
// ---------------------------------------------------------------------
//  Hardness is in 20 Hz ticks, bare-handed. Rough scale: 5 = instant
//  (a flower), 20 = one second (dirt), 150 = seven seconds (stone by
//  hand). A tool of the right class divides it; step 4 owns that maths.
//
//  `drop_item` is an ITEM id (items/items.h), and for anything that
//  drops itself that is simply its own block id -- the two id spaces
//  are deliberately the same below BLK_COUNT. The interesting rows are
//  the ones where they differ: stone drops cobblestone, grass drops
//  dirt, coal ore drops coal. A block with no drop row drops nothing,
//  which is right for water, leaves, glass and tall grass.
// =====================================================================

#include "world/blocks.h"

#include "items/items.h"  // the ITEM_* ids the drop column names

#define M3(t, s, b) \
    { (t), (s), (b) }
#define M1(m) \
    { (m), (m), (m) }

block_def_t const BLOCKS[BLK_COUNT] = {
    [BLK_AIR] = {.name = "air", .kind = K_AIR, .mat = M1(0), .hardness = HARDNESS_UNBREAKABLE, .flags = BF_REPLACEABLE},

    [BLK_GRASS] = {.name     = "grass", .drop_item = BLK_DIRT, .drop_min = 1, .drop_max = 1,
                   .kind     = K_CUBE,
                   .mat      = M3(VM_GRASS_TOP, VM_GRASS_SIDE, VM_DIRT),
                   .hardness = 20,
                   .tool     = TOOL_SHOVEL,
                   .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_DIRT] = {.name     = "dirt", .drop_item = BLK_DIRT, .drop_min = 1, .drop_max = 1,
                  .kind     = K_CUBE,
                  .mat      = M1(VM_DIRT),
                  .hardness = 20,
                  .tool     = TOOL_SHOVEL,
                  .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_STONE] = {.name       = "stone", .drop_item = BLK_COBBLE, .drop_min = 1, .drop_max = 1,
                   .kind       = K_CUBE,
                   .mat        = M1(VM_STONE),
                   .hardness   = 150,
                   .tool       = TOOL_PICK,
                   .tool_level = 1,
                   .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_COBBLE] = {.name       = "cobblestone", .drop_item = BLK_COBBLE, .drop_min = 1, .drop_max = 1,
                    .kind       = K_CUBE,
                    .mat        = M1(VM_COBBLE),
                    .hardness   = 160,
                    .tool       = TOOL_PICK,
                    .tool_level = 1,
                    .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_SAND] = {.name     = "sand", .drop_item = BLK_SAND, .drop_min = 1, .drop_max = 1,
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

    [BLK_LOG] = {.name     = "log", .drop_item = BLK_LOG, .drop_min = 1, .drop_max = 1,
                 .kind     = K_CUBE,
                 .mat      = M3(VM_LOG_TOP, VM_LOG_SIDE, VM_LOG_TOP),
                 .hardness = 40,
                 .tool     = TOOL_AXE,
                 .flags    = BF_SOLID | BF_OPAQUE | BF_FELLABLE},

    [BLK_PLANKS] = {.name     = "planks", .drop_item = BLK_PLANKS, .drop_min = 1, .drop_max = 1,
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

    [BLK_COAL_ORE] = {.name       = "coal_ore", .drop_item = ITEM_COAL, .drop_min = 1, .drop_max = 1,
                      .kind       = K_CUBE,
                      .mat        = M1(VM_COAL),
                      .hardness   = 200,
                      .tool       = TOOL_PICK,
                      .tool_level = 1,
                      .flags      = BF_SOLID | BF_OPAQUE},

    [BLK_GLASS] = {.name = "glass", .kind = K_SEE, .mat = M1(VM_GLASS), .hardness = 12, .flags = BF_SOLID},

    [BLK_TORCH] = {.name = "torch", .drop_item = BLK_TORCH, .drop_min = 1, .drop_max = 1, .kind = K_TORCH, .mat = M1(VM_TORCH), .hardness = 1, .light = 14},

    [BLK_FLOWER_RED] =
        {.name = "flower_red", .drop_item = BLK_FLOWER_RED, .drop_min = 1, .drop_max = 1, .kind = K_PLANT, .mat = M1(VM_FLOWER_RED), .hardness = 1, .flags = BF_REPLACEABLE},

    [BLK_FLOWER_YELLOW] =
        {.name = "flower_yellow", .drop_item = BLK_FLOWER_YELLOW, .drop_min = 1, .drop_max = 1, .kind = K_PLANT, .mat = M1(VM_FLOWER_YELLOW), .hardness = 1, .flags = BF_REPLACEABLE},

    [BLK_TALL_GRASS] =
        {.name = "tall_grass", .kind = K_PLANT, .mat = M1(VM_TALL_GRASS), .hardness = 1, .flags = BF_REPLACEABLE},

    // Never generated, never placed, never meshed: what world_block()
    // answers for a chunk that is not resident (D-14).
    [BLK_BARRIER] = {.name     = "barrier",
                     .kind     = K_CUBE,
                     .mat      = M1(VM_STONE),
                     .hardness = HARDNESS_UNBREAKABLE,
                     .flags    = BF_SOLID | BF_OPAQUE},

    // The floor of the world, as in Beta: nothing breaks it. Only the
    // Far Lands put it down so far (D-79).
    [BLK_BEDROCK] = {.name     = "bedrock",
                     .kind     = K_CUBE,
                     .mat      = M1(VM_BEDROCK),
                     .hardness = HARDNESS_UNBREAKABLE,
                     .flags    = BF_SOLID | BF_OPAQUE},

    [BLK_GRAVEL] = {.name     = "gravel", .drop_item = BLK_GRAVEL, .drop_min = 1, .drop_max = 1,
                    .kind     = K_CUBE,
                    .mat      = M1(VM_GRAVEL),
                    .hardness = 18,
                    .tool     = TOOL_SHOVEL,
                    .flags    = BF_SOLID | BF_OPAQUE | BF_GRAVITY},

    // Not solid, as in Minecraft: you walk through a sign. It drops
    // nothing, since there is no sign item yet (D-79).
    [BLK_SIGN] = {.name = "sign", .kind = K_SIGN, .mat = M1(VM_PLANKS), .hardness = 40, .tool = TOOL_AXE},
};
