#pragma once
// =====================================================================
//  CraftMiner  --  the simulation clock
// ---------------------------------------------------------------------
//  The world simulates at a fixed 20 Hz and the frame interpolates
//  between the last two ticks (D-02). Minecraft's rate, and the one
//  crop growth, mob AI and hunger all want.
//
//  Three things fall out of a fixed step, and the third is why it is
//  here from the start rather than added later:
//
//    * physics behaves the same at 10 fps and at 30, so the frame rate
//      is a picture-quality setting and not a difficulty one;
//    * a RECORDED BITMASK STREAM REPLAYS IDENTICALLY, which is the
//      automated-testing story this project was asked for;
//    * the frame becomes a pure function of the clock plus the replay,
//      which is exactly the precondition the `shots` framebuffer-hash
//      test needs (testkit/devtest.h).
//
//  The accumulator is CAPPED. A long stall -- an SD write, a chunk
//  generating on the main task -- must not be repaid as forty ticks in
//  one frame, which would teleport the player through the world and, on
//  a slow frame, never catch up.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#define TICK_HZ      20
#define TICK_SECONDS (1.0 / (double)TICK_HZ)
#define TICK_MAX_RUN 5  // ticks run in one frame before the rest is dropped

typedef struct {
    double   accum;    // seconds owed
    double   last;     // when we last looked at the clock
    uint64_t count;    // ticks since the start: the replay's timeline
    bool     started;
    bool     frozen;   // ticks owed are discarded, not run (D-26: entering a world)
} tick_clock_t;

void tick_reset(tick_clock_t* c, double now);

// How many ticks to run, given the clock now. Call once a frame and run
// the simulation that many times.
int tick_due(tick_clock_t* c, double now);

// How far through the current tick the frame is, 0..1: the blend for
// interpolating the pose.
float tick_alpha(tick_clock_t const* c);

// Stop the world without stopping the clock. Entering a world freezes
// the simulation until the chunks under the player exist, so they do
// not fall through terrain that has not arrived (D-26).
void tick_freeze(tick_clock_t* c, bool on);
