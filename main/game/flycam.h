#pragma once
// =====================================================================
//  SynthMiner  --  a camera you can fly by hand
// ---------------------------------------------------------------------
//  There is no player yet (that is block 2: physics, picking, the tick).
//  This is what stands in until there is: a camera with no body and no
//  gravity, driven straight off the keyboard, so the world can be
//  looked at from any angle it is suspected of being wrong from.
//
//  IT IS NOT THE PLAYER AND MUST NOT BECOME ONE. When a test runs, the
//  camera follows a scripted path instead -- a pure function of the
//  show clock -- because the `shots` test hashes the framebuffer and a
//  frame that depends on which keys were held is not reproducible
//  (devtest.h). Free flight is strictly the no-test case.
//
//  Keys are read directly rather than through se_bindings, deliberately:
//  the binding table is the player's (D-05, step 6.1), and a debug
//  camera has no business taking up slots in it or appearing in the
//  controls menu.
//
//    W / S        forward / back, along the way you are looking
//    A / D        sideways
//    Space        up            Left Shift   down
//    cursor keys  look
//    Left Ctrl    three times the speed
// =====================================================================

#include <stdbool.h>

typedef struct {
    double wx, wz;  // world position, in blocks
    float  wy;      // the eye height, absolute (no ground following)
    float  yaw;     // radians, 0 looking along +x
    float  pitch;   // radians, negative looking down
    bool   placed;  // the ground has been found and the eye put above it
} flycam_t;

// Put the camera at (wx, wz) looking along `yaw`, at `wy`. `placed` is
// cleared, so the next flycam_update() drops it onto the ground once a
// chunk is there to stand on.
void flycam_reset(flycam_t* f, double wx, double wz, float wy, float yaw);

// Read the keyboard and move. `dt` is the frame's own seconds -- this
// is the one thing in the game that is allowed to use it, precisely
// because nothing reproducible depends on it.
void flycam_update(flycam_t* f, float dt);
