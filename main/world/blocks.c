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

#include "items/items.h"    // the ITEM_* ids the drop column names
#include "world/blockent.h"  // the BE_* kinds the record column names

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
                   .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_SOFT},

    [BLK_DIRT] = {.name     = "dirt", .drop_item = BLK_DIRT, .drop_min = 1, .drop_max = 1,
                  .kind     = K_CUBE,
                  .mat      = M1(VM_DIRT),
                  .hardness = 20,
                  .tool     = TOOL_SHOVEL,
                  .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_GRAVEL},

    [BLK_STONE] = {.name       = "stone", .drop_item = BLK_COBBLE, .drop_min = 1, .drop_max = 1,
                   .kind       = K_CUBE,
                   .mat        = M1(VM_STONE),
                   .hardness   = 150,
                   .tool       = TOOL_PICK,
                   .tool_level = 1,
                   .flags      = BF_SOLID | BF_OPAQUE, .sound = SND_STONE},

    [BLK_COBBLE] = {.name       = "cobblestone", .drop_item = BLK_COBBLE, .drop_min = 1, .drop_max = 1,
                    .kind       = K_CUBE,
                    .mat        = M1(VM_COBBLE),
                    .hardness   = 160,
                    .tool       = TOOL_PICK,
                    .tool_level = 1,
                    .flags      = BF_SOLID | BF_OPAQUE, .sound = SND_STONE},

    [BLK_SAND] = {.name     = "sand", .drop_item = BLK_SAND, .drop_min = 1, .drop_max = 1,
                  .kind     = K_CUBE,
                  .mat      = M1(VM_SAND),
                  .hardness = 15,
                  .tool     = TOOL_SHOVEL,
                  .flags    = BF_SOLID | BF_OPAQUE | BF_GRAVITY, .sound = SND_SAND},

    // The engine has no blending, so water is not see-through in the
    // usual sense. It is K_LIQUID instead: only its surface is drawn,
    // and it hides nothing, so you look through a lake at the bed of it
    // (D-86). BF_OPAQUE stays on for the lighting, which reads
    // BF_LIQUID first and dims by 2 a block either way.
    [BLK_WATER] = {.name     = "water",
                   .kind     = K_LIQUID,
                   .mat      = M1(VM_WATER),
                   .hardness = HARDNESS_UNBREAKABLE,
                   .flags    = BF_OPAQUE | BF_REPLACEABLE | BF_LIQUID, .sound = SND_SPLASH},

    [BLK_LOG] = {.name     = "log", .drop_item = BLK_LOG, .drop_min = 1, .drop_max = 1,
                 .kind     = K_CUBE,
                 .mat      = M3(VM_LOG_TOP, VM_LOG_SIDE, VM_LOG_TOP),
                 .hardness = 40,
                 .tool     = TOOL_AXE,
                 .flags    = BF_SOLID | BF_OPAQUE | BF_FELLABLE, .sound = SND_WOOD},

    [BLK_PLANKS] = {.name     = "planks", .drop_item = BLK_PLANKS, .drop_min = 1, .drop_max = 1,
                    .kind     = K_CUBE,
                    .mat      = M1(VM_PLANKS),
                    .hardness = 40,
                    .tool     = TOOL_AXE,
                    .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_WOOD},

    [BLK_LEAVES] = {.name     = "leaves",
                    .kind     = K_SEE,
                    .mat      = M1(VM_LEAVES),
                    .hardness = 8,
                    .tool     = TOOL_SHEARS,
                    .flags    = BF_SOLID | BF_FELLABLE | BF_SEE_SELF, .sound = SND_SOFT},

    [BLK_COAL_ORE] = {.name       = "coal_ore", .drop_item = ITEM_COAL, .drop_min = 1, .drop_max = 1,
                      .kind       = K_CUBE,
                      .mat        = M1(VM_COAL),
                      .hardness   = 200,
                      .tool       = TOOL_PICK,
                      .tool_level = 1,
                      .flags      = BF_SOLID | BF_OPAQUE, .sound = SND_STONE},

    [BLK_GLASS] = {.name = "glass", .kind = K_SEE, .mat = M1(VM_GLASS), .hardness = 12, .flags = BF_SOLID, .sound = SND_GLASS},

    [BLK_TORCH] = {.name = "torch", .drop_item = BLK_TORCH, .drop_min = 1, .drop_max = 1, .kind = K_TORCH, .mat = M1(VM_TORCH), .hardness = 1, .light = 14, .sound = SND_WOOD},

    [BLK_FLOWER_RED] =
        {.name = "flower_red", .drop_item = BLK_FLOWER_RED, .drop_min = 1, .drop_max = 1, .kind = K_PLANT, .mat = M1(VM_FLOWER_RED), .hardness = 1, .flags = BF_REPLACEABLE, .sound = SND_SOFT},

    [BLK_FLOWER_YELLOW] =
        {.name = "flower_yellow", .drop_item = BLK_FLOWER_YELLOW, .drop_min = 1, .drop_max = 1, .kind = K_PLANT, .mat = M1(VM_FLOWER_YELLOW), .hardness = 1, .flags = BF_REPLACEABLE, .sound = SND_SOFT},

    [BLK_TALL_GRASS] =
        {.name = "tall_grass", .kind = K_PLANT, .mat = M1(VM_TALL_GRASS), .hardness = 1, .flags = BF_REPLACEABLE, .sound = SND_SOFT},

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
                     .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_STONE},

    // The one block a player has to make before they can make anything
    // else (Part C). Wood, so an axe is the tool and it comes back when
    // broken -- a table left in a cave is not lost.
    [BLK_CRAFTING_TABLE] = {.name     = "crafting_table", .drop_item = BLK_CRAFTING_TABLE, .drop_min = 1, .drop_max = 1,
                            .kind     = K_CUBE,
                            .mat      = M3(VM_TABLE_TOP, VM_TABLE_SIDE, VM_PLANKS),
                            .hardness = 50,
                            .tool     = TOOL_AXE,
                            .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_WOOD,
                            .flags2   = BF2_USABLE},

    // Stone, so it wants a pickaxe, and it keeps a block entity for
    // its three slots and its fire (world/blockent.h). THE OPENING IS
    // ON ALL FOUR SIDES: there is no facing bit in the state byte yet,
    // and a furnace you can always recognise is worth more than one
    // whose single front happens to face the wall you built it into.
    [BLK_FURNACE] = {.name       = "furnace", .drop_item = BLK_FURNACE, .drop_min = 1, .drop_max = 1,
                     .kind       = K_CUBE,
                     .mat        = M3(VM_FURNACE_TOP, VM_FURNACE_FRONT, VM_FURNACE_TOP),
                     .hardness   = 180,
                     .tool       = TOOL_PICK,
                     .tool_level = 1,
                     .flags      = BF_SOLID | BF_OPAQUE, .sound = SND_STONE,
                     .flags2     = BF2_USABLE | BF2_RECORD},

    // Iron. Needs a stone pickaxe and REFUSES the swing without one
    // (BF2_TOOL_REQUIRED, the user's call) -- the ore drops itself and
    // the furnace turns it into an ingot.
    [BLK_IRON_ORE] = {.name       = "iron_ore", .drop_item = BLK_IRON_ORE, .drop_min = 1, .drop_max = 1,
                      .kind       = K_CUBE,
                      .mat        = M1(VM_IRON_ORE),
                      .hardness   = 220,
                      .tool       = TOOL_PICK,
                      .tool_level = 2,
                      .flags      = BF_SOLID | BF_OPAQUE, .sound = SND_STONE,
                      .flags2     = BF2_TOOL_REQUIRED},

    [BLK_CHEST] = {.name     = "chest", .drop_item = BLK_CHEST, .drop_min = 1, .drop_max = 1,
                   .kind     = K_CUBE,
                   .mat      = M3(VM_CHEST_TOP, VM_CHEST_SIDE, VM_CHEST_TOP),
                   .hardness = 50,
                   .tool     = TOOL_AXE,
                   .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_WOOD,
                   .flags2   = BF2_USABLE | BF2_RECORD},

    // The same box, and what goes in it does not stay: the user's
    // trashcan, emptied by the clock rather than by a button.
    [BLK_TRASH] = {.name     = "trash_chest", .drop_item = BLK_TRASH, .drop_min = 1, .drop_max = 1,
                   .kind     = K_CUBE,
                   .mat      = M3(VM_TRASH_TOP, VM_TRASH_SIDE, VM_TRASH_TOP),
                   .hardness = 50,
                   .tool     = TOOL_AXE,
                   .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_WOOD,
                   .flags2   = BF2_USABLE | BF2_RECORD},

    // Takes things apart. No record: it holds nothing between uses.
    [BLK_BENCH] = {.name     = "disassembly_bench", .drop_item = BLK_BENCH, .drop_min = 1, .drop_max = 1,
                   .kind     = K_CUBE,
                   .mat      = M3(VM_BENCH_TOP, VM_TABLE_SIDE, VM_PLANKS),
                   .hardness = 50,
                   .tool     = TOOL_AXE,
                   .flags    = BF_SOLID | BF_OPAQUE, .sound = SND_WOOD,
                   .flags2   = BF2_USABLE},

    [BLK_GRAVEL] = {.name     = "gravel", .drop_item = BLK_GRAVEL, .drop_min = 1, .drop_max = 1,
                    .kind     = K_CUBE,
                    .mat      = M1(VM_GRAVEL),
                    .hardness = 18,
                    .tool     = TOOL_SHOVEL,
                    .flags    = BF_SOLID | BF_OPAQUE | BF_GRAVITY, .sound = SND_GRAVEL},

    // Not solid, as in Minecraft: you walk through a sign. It drops
    // nothing, since there is no sign item yet (D-79).
    [BLK_SIGN] = {.name = "sign", .kind = K_SIGN, .mat = M1(VM_PLANKS), .hardness = 40, .tool = TOOL_AXE, .sound = SND_WOOD},
};

// Which kind of record each block keeps. A function rather than a
// column, because blockent.h includes this file's header and a be_kind_t
// in block_def_t would be a cycle. One line per block, and a block
// carrying BF2_RECORD without a line here is caught by worldcheck.
uint8_t block_record_kind(uint8_t id) {
    switch (id) {
        case BLK_FURNACE: return BE_FURNACE;
        case BLK_CHEST: return BE_CHEST;
        case BLK_TRASH: return BE_TRASH;
        default: return BE_NONE;
    }
}
