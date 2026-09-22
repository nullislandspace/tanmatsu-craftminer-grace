#pragma once
// =====================================================================
//  CraftMiner  --  what the crosshair is pointing at
// ---------------------------------------------------------------------
//  A ray walked through the block grid, cell by cell, in the order it
//  actually crosses them (Amanatides and Woo): step to whichever axis
//  boundary is nearest, test that cell, repeat. No sampling, so a
//  diagonal ray cannot slip through the corner between two blocks and
//  no step size has to be chosen.
//
//  It reports the FACE it came in through as well as the block, because
//  every use needs it: placing puts the new block against that face,
//  breaking draws the crack overlay on it, and a torch or a stair will
//  want to know which way it is being attached.
//
//  Pure: no engine, no allocation. tools/worldcheck.c checks it against
//  a brute-force march.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// How far the player can reach, in blocks. Minecraft's survival reach.
#define RAY_REACH 4.5f

typedef struct {
    int32_t x, y, z;     // the block struck
    int32_t px, py, pz;  // the empty cell in front of its face: where a placement goes
    uint8_t block;       // what was struck
    uint8_t face;        // MESH_DIR_* of the face entered through
    float   dist;        // along the ray, in blocks
} ray_hit_t;

// Walk from (ox, oy, oz) along (dx, dy, dz) -- which need not be
// normalised -- for at most `max` blocks. True when it strikes
// something the picker should report.
//
// `want_solid` picks what counts as a hit: true stops at anything that
// stops the player (BF_SOLID); false stops at anything you can POINT AT
// -- every block but air and liquids -- which is what the player's
// crosshair wants: a torch or a flower is not solid, and a ray that
// ignored them made a placed torch impossible to take back (F-56).
// Liquids are looked through either way, so water never hides the
// riverbed from the pick.
bool ray_pick(double ox, double oy, double oz, float dx, float dy, float dz, float max, bool want_solid,
              ray_hit_t* out);

// The direction a yaw/pitch pair looks along, in the engine's
// convention (se_scene.c, camera_build_basis):
//     forward = ( sin yaw cos pitch, -sin pitch, cos yaw cos pitch )
// POSITIVE PITCH LOOKS DOWN. Kept here so that the picker, the camera
// and the player's movement cannot drift apart on the sign.
void ray_forward(float yaw, float pitch, float* dx, float* dy, float* dz);
