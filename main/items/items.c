// =====================================================================
//  CraftMiner  --  the item registry (see items.h)
// =====================================================================

#include "items/items.h"

#include <string.h>

// The items that are not blocks. Indexed by id - BLK_COUNT.
static item_def_t const ITEMS[ITEM_COUNT - BLK_COUNT] = {
    [ITEM_COAL - BLK_COUNT]  = {"coal", CM_STR_ITEM_COAL, ITEM_STACK_MAX, TOOL_NONE, 0, 0, 1600, 0xFF2A2A2Eu},
    [ITEM_STICK - BLK_COUNT] = {"stick", CM_STR_ITEM_STICK, ITEM_STACK_MAX, TOOL_NONE, 0, 0, 100, 0xFF9A7040u},

    // Tools. Durability is uses, not ticks: a wooden pickaxe is 60
    // blocks of stone, a stone one 130 -- enough that running out is a
    // thing that happens without being the thing that happens.
    [ITEM_PICK_WOOD - BLK_COUNT]    = {"pickaxe_wood", CM_STR_ITEM_PICKAXE_WOOD, 1, TOOL_PICK, 1, 60, 200, 0xFFB08040u},
    [ITEM_PICK_STONE - BLK_COUNT]   = {"pickaxe_stone", CM_STR_ITEM_PICKAXE_STONE, 1, TOOL_PICK, 2, 130, 0, 0xFF9098A0u},
    [ITEM_AXE_WOOD - BLK_COUNT]     = {"axe_wood", CM_STR_ITEM_AXE_WOOD, 1, TOOL_AXE, 1, 60, 200, 0xFFC08848u},
    [ITEM_AXE_STONE - BLK_COUNT]    = {"axe_stone", CM_STR_ITEM_AXE_STONE, 1, TOOL_AXE, 2, 130, 0, 0xFFA0A8B0u},
    [ITEM_SHOVEL_WOOD - BLK_COUNT]  = {"shovel_wood", CM_STR_ITEM_SHOVEL_WOOD, 1, TOOL_SHOVEL, 1, 60, 200, 0xFFA07838u},
    [ITEM_SHOVEL_STONE - BLK_COUNT] = {"shovel_stone", CM_STR_ITEM_SHOVEL_STONE, 1, TOOL_SHOVEL, 2, 130, 0, 0xFF888F98u},
};

// A flat colour standing in for the block's texture in the inventory.
// Approximately each block's average, which is what a 16x16 texture
// reads as at icon size anyway; the real mini-cube icon (D-03) can
// replace this without anything above changing.
static uint32_t const BLOCK_ARGB[BLK_COUNT] = {
    [BLK_AIR] = 0,
    [BLK_GRASS] = 0xFF5C9634u,        [BLK_DIRT] = 0xFF7A563Au,
    [BLK_STONE] = 0xFF7A7A7Cu,        [BLK_COBBLE] = 0xFF767676u,
    [BLK_SAND] = 0xFFD6C896u,         [BLK_WATER] = 0xFF3054C4u,
    [BLK_LOG] = 0xFF644C2Eu,          [BLK_PLANKS] = 0xFFA4804Eu,
    [BLK_LEAVES] = 0xFF3A7026u,       [BLK_COAL_ORE] = 0xFF606062u,
    [BLK_GLASS] = 0xFFC8D8DEu,        [BLK_TORCH] = 0xFF6E502Cu,
    [BLK_FLOWER_RED] = 0xFFD62824u,   [BLK_FLOWER_YELLOW] = 0xFFFAD428u,
    [BLK_TALL_GRASS] = 0xFF5C9634u,   [BLK_BARRIER] = 0xFF303030u,
    [BLK_BEDROCK] = 0xFF4A4A4Au,      [BLK_GRAVEL] = 0xFF847C78u,
    [BLK_SIGN] = 0xFFA4804Eu,         [BLK_CRAFTING_TABLE] = 0xFF9C7A4Au,
    [BLK_FURNACE] = 0xFF707072u,
};

// What each block is CALLED on screen, beside the colour above. A
// second per-block table in this file rather than a column in blocks.h,
// for the same reason BLOCK_ARGB is here: the block registry describes
// how a block behaves, and how it is spelled in 32 languages is the
// item layer's business.
static cm_str_t const BLOCK_LABEL[BLK_COUNT] = {
    [BLK_GRASS] = CM_STR_ITEM_GRASS,         [BLK_DIRT] = CM_STR_ITEM_DIRT,
    [BLK_STONE] = CM_STR_ITEM_STONE,         [BLK_COBBLE] = CM_STR_ITEM_COBBLESTONE,
    [BLK_SAND] = CM_STR_ITEM_SAND,           [BLK_WATER] = CM_STR_ITEM_WATER,
    [BLK_LOG] = CM_STR_ITEM_LOG,             [BLK_PLANKS] = CM_STR_ITEM_PLANKS,
    [BLK_LEAVES] = CM_STR_ITEM_LEAVES,       [BLK_COAL_ORE] = CM_STR_ITEM_COAL_ORE,
    [BLK_GLASS] = CM_STR_ITEM_GLASS,         [BLK_TORCH] = CM_STR_ITEM_TORCH,
    [BLK_FLOWER_RED] = CM_STR_ITEM_FLOWER_RED,
    [BLK_FLOWER_YELLOW] = CM_STR_ITEM_FLOWER_YELLOW,
    [BLK_TALL_GRASS] = CM_STR_ITEM_TALL_GRASS,
    [BLK_BEDROCK] = CM_STR_ITEM_BEDROCK,     [BLK_GRAVEL] = CM_STR_ITEM_GRAVEL,
    [BLK_SIGN] = CM_STR_ITEM_SIGN,
    [BLK_CRAFTING_TABLE] = CM_STR_ITEM_CRAFTING_TABLE,
    [BLK_FURNACE] = CM_STR_ITEM_FURNACE,
    // Air and the barrier are never in anybody's hands and have none.
};

// What a BLOCK burns for, in ticks. Wood and things made of wood, as
// the user asked -- "fuel is everything that burns (wood, wooden tools,
// planks, sticks)". Minecraft's numbers: a log or planks 300, a stick
// 100, coal 1600, a wooden tool 200.
static uint16_t const BLOCK_FUEL[BLK_COUNT] = {
    [BLK_LOG] = 300, [BLK_PLANKS] = 300, [BLK_CRAFTING_TABLE] = 300,
};

item_def_t item_def(uint16_t id) {
    if (item_is_block(id)) {
        // Synthesised, not stored: the block table is the authority on
        // what a block is called and what it looks like, and a second
        // copy here would be a second thing to keep right.
        block_def_t const* b = block_def((uint8_t)id);
        return (item_def_t){
            .name       = b->name,
            .label      = BLOCK_LABEL[id < BLK_COUNT ? id : BLK_BARRIER],
            .stack_max  = ITEM_STACK_MAX,
            .tool       = TOOL_NONE,
            .tool_level = 0,
            .durability = 0,
            .fuel       = BLOCK_FUEL[id < BLK_COUNT ? id : BLK_BARRIER],
            .argb       = BLOCK_ARGB[id < BLK_COUNT ? id : BLK_BARRIER],
        };
    }
    if (id >= BLK_COUNT && id < ITEM_COUNT) return ITEMS[id - BLK_COUNT];
    return (item_def_t){.name = "", .stack_max = 1, .argb = 0};
}

uint16_t item_by_name(char const* name) {
    if (name == NULL || *name == '\0') return 0;
    for (uint16_t id = 1; id < ITEM_COUNT; id++) {
        if (id == BLK_BARRIER) continue;  // never carried
        if (strcmp(item_def(id).name, name) == 0) return id;
    }
    return 0;
}

int item_break_ticks(uint8_t block, uint16_t tool_item) {
    block_def_t const* b = block_def(block);
    if (b->hardness == HARDNESS_UNBREAKABLE) return -1;

    int ticks = (int)b->hardness;
    if (ticks < 1) ticks = 1;

    item_def_t const t = item_def(tool_item);
    // The right class only. A pickaxe does not speed up dirt, which is
    // what makes carrying more than one tool worth the slots.
    //
    // TWICE THE LEVEL, not the level plus one. The old divisor -- 2x
    // for wood, 3x for stone -- was a rounding error next to a fist,
    // and the user's complaint was exactly that: "mining with the wrong
    // tool is a lot slower" was not true. Now hand 1x, wood 2x, stone
    // 4x, iron 6x, so a block of stone is 7.5 seconds by hand and 1.9
    // with a stone pickaxe. No hardness number had to move.
    if (b->tool != TOOL_NONE && t.tool == b->tool && t.tool_level > 0) {
        ticks /= 2 * (int)t.tool_level;
    }
    return ticks < 1 ? 1 : ticks;
}

bool item_can_harvest(uint8_t block, uint16_t tool_item) {
    block_def_t const* b = block_def(block);
    if (b->tool_level == 0) return true;  // hands are enough
    item_def_t const t = item_def(tool_item);
    return t.tool == b->tool && t.tool_level >= b->tool_level;
}
