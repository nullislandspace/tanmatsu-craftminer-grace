#pragma once
// =====================================================================
//  SynthMiner  --  the title, written in blocks
// ---------------------------------------------------------------------
//  "SynthMiner" standing in the sky over the meadow, in real blocks --
//  "Synth" in grass, "Miner" in cobblestone -- popping in column by
//  column while the camera drifts past below and looks up at them.
//
//  The idea and the 7-row block font are the showreel's
//  (tanmatsu-showreel-grace, main/craftminer/scenes/cm_title.c). What
//  is different here is that the showreel's world was a fixed
//  128x32x128 array it could stamp letters into, and this one is
//  streamed, saved, and belongs to the player. So the title runs on a
//  SCRATCH WORLD (worldstore.h): generated from a fixed seed, never
//  written, gone when the player picks a world. The letters are real
//  blocks in it, placed through world_set() like any other -- which
//  means the greedy mesher, the streamer and the lighting all treat
//  them as what they are, and the title screen is a genuine view of the
//  game rather than a picture of one.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "world/chunk_render.h"

// Where the camera should be, `t` seconds in. Loops.
typedef struct {
    double wx, wz;
    float  wy, yaw, pitch;
} title_view_t;

// The seed the title's landscape uses. The chunk worker needs it too,
// and two copies of a constant is one copy too many.
uint32_t title_seed(void);

// Build the scratch world and work out the camera path. False if the
// world could not be opened.
bool title_begin(void);
void title_end(void);

// Place whatever letters are due by `t` seconds. Call once a frame
// before streaming, so the chunks the letters live in are resident.
void title_update(double t);

// The camera for `t` seconds in.
title_view_t title_camera(double t);

// The view the title wants: textures out to the letters and the fog
// pushed past them, so the word reads as blocks rather than as grey.
sm_view_t title_view(void);

// Where the camera is, for the chunk streamer.
void title_stream_at(double t, double* wx, double* wz);
