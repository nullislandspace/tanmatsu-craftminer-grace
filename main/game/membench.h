#pragma once
// =====================================================================
//  SynthMiner  --  what the memory actually costs
// ---------------------------------------------------------------------
//  A rasteriser that is fill-bound is bound on something. This measures
//  which, by replaying the span loop's exact memory pattern -- read a
//  32-bit depth cell, write it back, write a 16-bit pixel, step
//  BACKWARDS (the display is rotated, so that is the direction the real
//  loop walks) -- over PSRAM and over internal SRAM, with no arithmetic
//  in between.
//
//  The number it prints is nanoseconds per pixel for memory alone. Put
//  it beside the rasteriser's own ns/px (scene_fill_stats) and the
//  question "would SIMD help?" answers itself: if the two are close,
//  the arithmetic is already free and vectorising it buys nothing.
// =====================================================================

// Log the results. Call once at boot, after the heaps are up.
void membench_run(void);
