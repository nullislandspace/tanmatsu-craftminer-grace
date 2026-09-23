#pragma once
// =====================================================================
//  CraftMiner  --  breaking and placing
// ---------------------------------------------------------------------
//  THE LOGGING RULE (claudeplans/craftminer.md, Part F) lives here, and
//  it is CraftMiner's one deliberate departure from Minecraft:
//
//    break a wood or leaf block the PLAYER placed  -> that block drops
//    break one the world GREW                      -> the tree falls
//
//  Which is which is the ST_PLACED bit, set on everything a player puts
//  down and never by generation (chunk.h). So a forest can be cleared
//  without twenty minutes of jumping at trunks, and a player's own
//  timber-framed house does not collapse when they mis-click a beam.
//
//  Felling is a flood fill through GROWN tree blocks only, and it is
//  bounded three ways, all of which matter:
//
//    * y >= the broken block's y -- so breaking a trunk at head height
//      does not take the stump you are standing on, and a tree growing
//      out of a cliff does not reach down it;
//    * FELL_RADIUS horizontally and FELL_HEIGHT up -- so one tree whose
//      canopy touches another cannot take the whole forest;
//    * FELL_MAX blocks in total -- a hard stop, so a pathological shape
//      cannot stall a tick however the other two are tuned.
//
//  Pure: no engine, no allocation (the fill has a fixed-size stack).
//  tools/worldcheck.c tests the rule both ways.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game/physics.h"
#include "game/raycast.h"
#include "items/inventory.h"

#define FELL_RADIUS 8   // blocks from the break, horizontally
#define FELL_HEIGHT 24  // ... and upwards
#define FELL_MAX    512 // total blocks one fell can take

// What a break did, so the caller can make the right noise, spawn the
// right drops and show the right particles.
typedef struct {
    bool    ok;
    // The block will not break with what is in hand (BF2_TOOL_REQUIRED)
    // and this is the tool it wants -- an ITEM id, so the caller can
    // name it in the player's language rather than saying only "no".
    uint16_t needs_tool;
    uint8_t block;     // what was there
    int     felled;    // blocks removed IN TOTAL (1 for an ordinary break)
    bool    was_tree;  // the felling rule applied
    int     dropped;   // items spawned on the ground
} break_result_t;

// Break the block at (x, y, z) with `tool_item` in hand. Refuses
// bedrock, air and anything outside the resident world. Applies the
// logging rule, and drops what the block table says -- but only if the
// tool qualifies (items.h, item_can_harvest): a block mined with too
// soft a tool still breaks and simply yields nothing, which is what
// makes a stone pickaxe progress rather than a permission slip.
break_result_t interact_break(int32_t x, int32_t y, int32_t z, uint16_t tool_item);

// Put `block` in the empty cell the ray reported (hit->p*), if it is
// free and the player's own box is not in the way. `avoid` is the
// player's box, or NULL to skip that test.
//
// Sets ST_PLACED, which is what makes the logging rule work and what
// will later stop placed leaves decaying.
bool interact_place(ray_hit_t const* hit, uint8_t block, phys_body_t const* avoid);

// Fell the tree reachable from (x, y, z), which must already have been
// checked as a grown tree block. Returns how many blocks it took,
// including the one at (x, y, z). Exposed for the host test.
int interact_fell(int32_t x, int32_t y, int32_t z);
