// =====================================================================
//  CraftMiner  --  the item registry (see items.h)
// =====================================================================

#include "items/items.h"

#include <string.h>

// The items that are not blocks. Indexed by id - BLK_COUNT.
static item_def_t const ITEMS[ITEM_COUNT - BLK_COUNT] = {
    [ITEM_COAL - BLK_COUNT]  = {"coal", ITEM_STACK_MAX, TOOL_NONE, 0, 0, 0xFF2A2A2Eu},
    [ITEM_STICK - BLK_COUNT] = {"stick", ITEM_STACK_MAX, TOOL_NONE, 0, 0, 0xFF9A7040u},

    // Tools. Durability is uses, not ticks: a wooden pickaxe is 60
    // blocks of stone, a stone one 130 -- enough that running out is a
    // thing that happens without being the thing that happens.
    [ITEM_PICK_WOOD - BLK_COUNT]    = {"pickaxe_wood", 1, TOOL_PICK, 1, 60, 0xFFB08040u},
    [ITEM_PICK_STONE - BLK_COUNT]   = {"pickaxe_stone", 1, TOOL_PICK, 2, 130, 0xFF9098A0u},
    [ITEM_AXE_WOOD - BLK_COUNT]     = {"axe_wood", 1, TOOL_AXE, 1, 60, 0xFFC08848u},
    [ITEM_AXE_STONE - BLK_COUNT]    = {"axe_stone", 1, TOOL_AXE, 2, 130, 0xFFA0A8B0u},
    [ITEM_SHOVEL_WOOD - BLK_COUNT]  = {"shovel_wood", 1, TOOL_SHOVEL, 1, 60, 0xFFA07838u},
    [ITEM_SHOVEL_STONE - BLK_COUNT] = {"shovel_stone", 1, TOOL_SHOVEL, 2, 130, 0xFF888F98u},
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
    [BLK_SIGN] = 0xFFA4804Eu,
};

item_def_t item_def(uint16_t id) {
    if (item_is_block(id)) {
        // Synthesised, not stored: the block table is the authority on
        // what a block is called and what it looks like, and a second
        // copy here would be a second thing to keep right.
        block_def_t const* b = block_def((uint8_t)id);
        return (item_def_t){
            .name       = b->name,
            .stack_max  = ITEM_STACK_MAX,
            .tool       = TOOL_NONE,
            .tool_level = 0,
            .durability = 0,
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
    if (b->tool != TOOL_NONE && t.tool == b->tool && t.tool_level > 0) {
        ticks /= (int)t.tool_level + 1;
    }
    return ticks < 1 ? 1 : ticks;
}

bool item_can_harvest(uint8_t block, uint16_t tool_item) {
    block_def_t const* b = block_def(block);
    if (b->tool_level == 0) return true;  // hands are enough
    item_def_t const t = item_def(tool_item);
    return t.tool == b->tool && t.tool_level >= b->tool_level;
}
