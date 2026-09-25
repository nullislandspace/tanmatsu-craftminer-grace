#pragma once
// =====================================================================
//  SynthMiner  --  Fred's meshes (engine-free)
// ---------------------------------------------------------------------
//  Fred, the player's figure: a yellow hard hat with a lamp, a big
//  brown moustache, a red shirt, blue overalls, brown boots -- and a
//  pickaxe, an axe or a shovel in his fist.
//  One mesh per moving piece, each built round its joint so a pose is a
//  rotation about the origin (miner.c):
//
//    head   neck at the origin; the hat, its brim and the lamp on it
//    body   hips at the origin, shoulders at y = FRED_BODY_H
//    arm    shoulder at the origin, hanging down -y
//    leg    hip at the origin, hanging down -y
//    pick   held in the fist: the handle along +z, the head at its end
//
//  Model units are blocks, +y up, the figure facing +z (its right hand
//  on -x). About 2 blocks tall; fred.c scales it to 0.9.
//  Ported from tanmatsu-showreel-grace,
//  main/craftminer/assets/miner_mesh.h, where he was "the miner".
//  Changes here are SynthMiner's; the showreel stays the origin to diff
//  against.
// =====================================================================

#include "math/mesh.h"

typedef enum {
    FM_SKIN,
    FM_SHIRT,
    FM_OVERALLS,
    FM_BOOTS,
    FM_HAT,
    FM_LAMP,
    FM_HAIR,
    FM_FACE,
    FM_WOOD,
    FM_IRON,
    FM_COUNT
} fred_mat_t;

#define FRED_LEG_H      0.72f  // hip height
#define FRED_BODY_H     0.70f  // hips to shoulders
#define FRED_ARM_H      0.62f
#define FRED_HIP_X      0.12f     // each leg's hip off the middle
#define FRED_SHOULDER_X 0.36f     // each arm's shoulder off the middle
#define FRED_FIST_Y     (-0.56f)  // the fist, down the arm from the shoulder

void fred_build_head(mesh_t* m);
void fred_build_body(mesh_t* m);
void fred_build_arm(mesh_t* m);

// THE SAME ARM, FOR FIRST PERSON, and longer at the shoulder.
//
// Third person looks at a whole miner, so his arm ends where his
// shoulder is. First person looks along it -- and the flat cap where
// the arm was cut off sat just inside the bottom of the view, which is
// what made it read as a severed arm hanging in the air rather than as
// the player's own (the user's catch). This one runs back past the
// camera, so the cap is off the screen and there is nothing to see the
// end of.
void fred_build_fp_arm(mesh_t* m);
void fred_build_leg(mesh_t* m);
void fred_build_pick(mesh_t* m);
// SynthMiner has three tools; the showreel's miner only needed the
// pickaxe. The same handle, a different head.
void fred_build_axe(mesh_t* m);
void fred_build_shovel(mesh_t* m);
