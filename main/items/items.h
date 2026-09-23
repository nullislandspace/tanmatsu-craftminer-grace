#pragma once
// =====================================================================
//  CraftMiner  --  the item registry
// ---------------------------------------------------------------------
//  ONE TABLE, like blocks.h, and for the same reason: adding a thing is
//  a row, not a search for every switch that needs a new case.
//
//  ITEM IDS 1..BLK_COUNT-1 *ARE* THE BLOCK IDS. A stack of cobblestone
//  is item id BLK_COBBLE, and placing it puts block BLK_COBBLE down.
//  That is not a coincidence to be tidied away later -- it is what
//  stops the two tables drifting, and it is why the inventory can draw
//  a block without a sprite for it: block_def() already knows what it
//  looks like. Real items -- coal, a pickaxe, bread -- start at
//  ITEM_FIRST and carry their own row here.
//
//  Pure: no engine, no allocation. tools/worldcheck.c builds it as-is.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "i18n/strings_gen.h"  // cm_str_t: the name the player reads
#include "world/blocks.h"

// The items that are not blocks. The first is BLK_COUNT, so the two id
// spaces meet without a gap and `id < BLK_COUNT` is the whole test for
// "this is a block".
enum {
    ITEM_COAL = BLK_COUNT,
    ITEM_STICK,
    ITEM_PICK_WOOD,
    ITEM_PICK_STONE,
    ITEM_AXE_WOOD,
    ITEM_AXE_STONE,
    ITEM_SHOVEL_WOOD,
    ITEM_SHOVEL_STONE,
    ITEM_IRON_INGOT,
    ITEM_PICK_IRON,
    ITEM_AXE_IRON,
    ITEM_SHOVEL_IRON,
    ITEM_COUNT
};

#define ITEM_STACK_MAX 64

typedef struct {
    char const* name;        // stable id, as blocks have one
    cm_str_t    label;       // what the player reads, in their language (i18n.h)
    uint8_t     stack_max;   // 1 for a tool, ITEM_STACK_MAX for most things
    uint8_t     tool;        // tool_t this counts as, TOOL_NONE for anything else
    uint8_t     tool_level;  // 1 wood, 2 stone, 3 iron
    uint16_t    durability;  // uses before it breaks; 0 = never wears
    uint16_t    fuel;        // ticks it burns in a furnace; 0 = it does not (game/furnace.h)
    uint32_t    argb;        // the icon, until items have sprites of their own
} item_def_t;

// Everything about an item, blocks included. For a block id this is
// synthesised from the block table rather than stored twice.
item_def_t item_def(uint16_t id);

static inline bool item_is_block(uint16_t id) {
    return id != BLK_AIR && id < BLK_COUNT;
}

// The block this item places, or BLK_AIR if it places nothing.
static inline uint8_t item_block(uint16_t id) {
    return item_is_block(id) ? (uint8_t)id : BLK_AIR;
}

// The item called `name`, or 0 if this build has no such item. How a
// saved inventory survives items being added or renumbered: it stores
// names, the way level.cmw's palette does for blocks (D-31).
uint16_t item_by_name(char const* name);

// What to call `id` on screen, in the player's language. The stable
// `name` is the id a save file keys on and is never translated; this is
// the other one. Every item a player can carry has a label, and
// worldcheck fails the build over one that does not -- a nameless row
// in the crafting book is not a thing anybody would notice by playing.
static inline cm_str_t item_label(uint16_t id) {
    return item_def(id).label;
}

// The tool of class `tool` at exactly `level`, or 0 if this build has
// none. What lets a refusal NAME what is needed ("Needs a stone
// pickaxe") instead of saying only that it will not budge.
uint16_t item_tool_for(uint8_t tool, uint8_t level);

// How many ticks `block` takes to break while holding `tool_item`.
//
// The right tool class divides the time by its level plus one, and a
// tool below the block's `tool_level` still breaks it -- it just drops
// nothing, which is Minecraft's rule and the one that makes a stone
// pickaxe feel like progress rather than a permission slip.
int item_break_ticks(uint8_t block, uint16_t tool_item);

// Does `tool_item` qualify to collect what `block` drops?
bool item_can_harvest(uint8_t block, uint16_t tool_item);
