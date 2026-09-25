#pragma once
// =====================================================================
//  CraftMiner  --  the benchmark flight
// ---------------------------------------------------------------------
//  ONE FIXED PATH THROUGH ONE FIXED WORLD, for comparing renderers.
//
//  The debug flight (`flight`) could not do this job. It opens a
//  SCRATCH world -- no directory, nothing on disk -- so every chunk is
//  generated from noise on every run: eleven seconds of loading screen
//  charged to the average, and then a chunk worker generating flat out
//  on core 1 for the whole measurement, saturating the PSRAM bus that
//  the banded renderer exists to relieve (F-91). Measuring a rasteriser
//  against a moving noise generator tells you about the generator.
//
//  So the bench world is PERSISTED and pre-generated, and the flight
//  streams it off the card the way play does (the user's call,
//  2026-09-25). It lives outside `worlds/` so it cannot be seen or
//  deleted from the world-select screen (worldstore.h, CM_BENCH_SLUG).
//
//  THE PATH WAS CHOSEN BY SEARCH, not by eye: 4000 seeds x 8 headings,
//  keeping only paths whose ground never steps more than two blocks,
//  then the one crossing the most biomes with the busiest skyline
//  (tools/worldcheck.c, check_bench_path, which re-runs the assertions
//  on every build). Seed 1030 due +z from the origin crosses ALL FIVE
//  biomes in 240 blocks --
//
//      mountain @0   birch @94   forest @112   plains @120   sand @190
//
//  -- with 23 blocks of relief and no step bigger than ONE block. So
//  snow, birch trunks, cactus and sandstone all get drawn, the camera
//  following the ground can never be buried, and nothing has to be
//  carved out of the way. At the flight's own 6 blocks a second that is
//  forty seconds, which is why the path is not faster: variety came
//  from choosing the seed, not from covering more ground.
//
//  Pure: constants and one function. main.c flies it and worldcheck
//  tests it, from here, so the two can never describe different paths.
// =====================================================================

// The world. Changing it invalidates what is on the card -- which is
// handled, not guarded against: worldstore_open_bench() throws away a
// bench world whose seed does not match and generates again.
#define BENCH_SEED 1030u

// The flight. BENCH_SPEED is the debug flight's own pace, kept so the
// numbers stay comparable with the flight scene's.
#define BENCH_SPEED 6.0    // blocks a second
#define BENCH_DIST  240.0  // blocks, end to end
#define BENCH_SECS  (BENCH_DIST / BENCH_SPEED)

// Where the flight is `t` seconds in, clamped to the ends so a run that
// overruns sits still at the finish rather than wandering off the
// pre-generated ground. Heading is +z, so forward (sin yaw, cos yaw) is
// (0, 1) and the yaw is zero.
//
// The half-block offsets put the camera in the middle of a column
// rather than on the seam between two, which is where a ground lookup
// is least ambiguous.
static inline void bench_path_at(double t, double* wx, double* wz, float* yaw) {
    double d = BENCH_SPEED * t;
    if (d < 0.0) d = 0.0;
    if (d > BENCH_DIST) d = BENCH_DIST;
    if (wx != NULL) *wx = 0.5;
    if (wz != NULL) *wz = 0.5 + d;
    if (yaw != NULL) *yaw = 0.0f;
}
