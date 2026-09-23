#pragma once
// =====================================================================
//  CraftMiner  --  the sound effects
// ---------------------------------------------------------------------
//  Every sound the game makes is ONE ROW in a table (sfx.c), not one
//  file per noise. The synthracer pattern this is descended from gave
//  each effect its own module -- right when there are seven of them and
//  each is a different idea, wrong here, where a footstep on gravel and
//  a footstep on sand are the same idea with different numbers.
//
//  So: `sfx_def_t` describes a one-shot as a tone layer plus a noise
//  layer through a filter, with an attack/decay envelope and a pitch
//  sweep. That covers a footstep, a pick striking stone, glass
//  breaking, an item picked up and a player hurt. A sound that genuinely
//  cannot be said in those terms gets its own voice module next to this
//  one -- but none of the first dozen needed to.
//
//  WHICH sound a block makes is the block registry's business, not this
//  file's: `block_def_t.sound` names a material class (SND_STONE,
//  SND_WOOD, ...) and the step / break / place sounds follow from it.
//  Adding a block therefore adds its sounds too, in the same row, with
//  no edit here (claudeplans/craftminer.md, Part L).
//
//  Every voice is registered with the engine mixer and frees itself when
//  its envelope closes. Playing is fire-and-forget and never blocks; if
//  the mixer is full the sound is simply dropped, because a missing
//  footstep is better than a stalled tick.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "world/blocks.h"

// The sounds, by name. Footsteps, breaks and places are per material and
// are NOT listed here one by one -- ask for them with sfx_play_step()
// and friends, which map a block's SND_ class onto the right row.
typedef enum {
    SFX_STEP_SOFT = 0,  // grass, leaves, tall grass
    SFX_STEP_GRAVEL,    // gravel, dirt
    SFX_STEP_STONE,     // stone, cobble, ore, bedrock
    SFX_STEP_WOOD,      // log, planks
    SFX_STEP_SAND,      // sand
    SFX_STEP_GLASS,     // glass
    SFX_STEP_SPLASH,    // water

    SFX_BREAK_SOFT,
    SFX_BREAK_GRAVEL,
    SFX_BREAK_STONE,
    SFX_BREAK_WOOD,
    SFX_BREAK_SAND,
    SFX_BREAK_GLASS,
    SFX_BREAK_SPLASH,

    SFX_PLACE,        // putting a block down: a short dull knock
    SFX_HIT,          // the tool striking a block, once per swing
    SFX_PICKUP,       // an item into the inventory
    SFX_HURT,         // the player takes damage
    SFX_LAND,         // feet hitting the ground after a fall
    SFX_CLICK,        // a menu row, a hotbar change
    SFX_FELL,         // a tree comes down (Part F)
    SFX_CRAFT,        // something was made in the crafting book
    SFX_DENY,         // ... and the refusal when it could not be

    SFX_COUNT
} sfx_id_t;

// Play one. Returns false if the mixer had no free voice or the effect
// group is muted -- callers ignore it; it is there for the tests.
bool sfx_play(sfx_id_t id);

// Play one detuned by `semitones` (may be fractional, may be negative).
// The table's own jitter is applied on top, so two calls never sound
// identical.
bool sfx_play_pitched(sfx_id_t id, float semitones);

// The material sounds, chosen by the block. An id past the table, or
// air, is silent rather than wrong.
bool sfx_play_step(uint8_t block);
bool sfx_play_break(uint8_t block);
bool sfx_play_place(uint8_t block);

// The name of a row, for the logs and the host test.
char const* sfx_name(sfx_id_t id);
