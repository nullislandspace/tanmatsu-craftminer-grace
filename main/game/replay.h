#pragma once
// =====================================================================
//  CraftMiner  --  recording and replaying play
// ---------------------------------------------------------------------
//  The simulation is a pure function of its starting state and its
//  per-tick input (D-02), so a replay is exactly those two things:
//
//    the start    the world's seed and clock, where the player stood and
//                 faced, and what they carried
//    each tick    the action bitmask (input.h) and whatever the gyroscope
//                 turned the view by
//
//  Played back -- on a SCRATCH world of the same seed, so a replay never
//  touches anyone's save -- it walks the player through the same moves
//  and the same world, which makes a scripted walk a reproducible test:
//  `perf scene=replay` measures the same frames every run, and `shots
//  scene=replay` photographs the same moments (with the chunk worker
//  inline, D-59).
//
//  It reproduces the world only as far as the world is generated:
//  record in a world you have edited and the replay, on fresh terrain,
//  will walk into the difference. Record tests in a new world.
//
//  File, little-endian: "CMRP", u32 version, u32 ticks, the start, then
//  per tick u32 mask, f32 yaw, f32 pitch. Pure (stdio only): the host
//  checks round-trip it.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "items/inventory.h"

#define REPLAY_MAX_TICKS (TICK_HZ_REPLAY * 60 * 10)  // ten minutes
#define TICK_HZ_REPLAY   20

typedef struct {
    uint32_t   seed;
    int64_t    time_of_day;
    double     x, y, z;
    float      yaw, pitch;
    inv_slot_t inv[INV_SLOTS];
    int32_t    selected;
} replay_start_t;

// --- Recording ------------------------------------------------------------

// Start recording from `start`. False if the buffer cannot be had.
bool replay_record_begin(replay_start_t const* start);
bool replay_recording(void);
// One tick's input. Past REPLAY_MAX_TICKS the recording simply stops
// growing.
void replay_record_tick(uint32_t mask, float gyro_yaw, float gyro_pitch);
// Stop, and write what was recorded to `path` (NULL: throw it away).
bool replay_record_end(char const* path);

// --- Playing ----------------------------------------------------------------

// Read a replay from `path` and get ready to play it from its first tick.
bool replay_load(char const* path, replay_start_t* start);
bool replay_playing(void);
// The next tick's input; false once the replay has run out (and then it
// is no longer playing).
bool replay_next(uint32_t* mask, float* gyro_yaw, float* gyro_pitch);
int  replay_position(void);  // ticks played so far
int  replay_length(void);
void replay_stop(void);
