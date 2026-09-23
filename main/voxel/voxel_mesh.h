#pragma once
// =====================================================================
//  CraftMiner  --  meshing blocks
// ---------------------------------------------------------------------
//  Turns a box of blocks into a mesh (mesh.h) of the faces that can be
//  seen. Four kinds of block:
//
//    solid cubes   grass, dirt, stone, water... A face is emitted where
//                  the neighbour does not hide it: air, a plant, a
//                  torch, or a see-through cube. Water is opaque (the
//                  engine has no blending), as in Minecraft's "fast"
//                  mode.
//    see-through   leaves and glass: cubes with cut-out texels (holes,
//                  se_texture.h), so what is behind them shows. Leaves
//                  show their faces towards other leaves too (a canopy
//                  you see into); glass hides glass.
//    plants        flowers and tall grass: two crossed quads through
//                  the cell, each drawn from both sides.
//    torches       a thin stick in the middle of the cell.
//    signs         a post with a board on top, facing east (+x); the
//                  board's front is the sign's text texture.
//
//  Neighbouring cube faces of one material in one plane merge into one
//  rectangle (greedy meshing); textures repeat once per block, since the
//  UVs run in block units and the engine repeats them. Grass sides merge
//  only sideways, so each block keeps its own strip of grass.
//
//  Three kinds of mesh, chosen by the caller (voxel_render.c) by
//  distance:
//
//    VOX_MESH_FANCY  everything: canopies you see into, plants.
//    VOX_MESH_FAST   see-through cubes count as solid (no insides) and
//                    leaves take the opaque "fast" texture; no plants.
//
//  The same fast mesh serves flat colours further off; a grid of cells
//  `step` 2 blocks across (a half-resolution world, the caller's
//  choice of block per cell) gives about a quarter of the triangles for
//  the distance. A half-resolution chunk rounds the ground a block up or
//  down, so where it meets a full-resolution one the two surfaces step;
//  its `skirt` closes the step (its outer side faces are always drawn),
//  or the sky would show through the crack.
//
//  Pure data, no engine calls: the host mesh check builds and verifies
//  it (tools/meshcheck_assets.h).
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_mesh.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "math/mesh.h"

// The materials, one per texture (voxel_render.c maps them to textures
// or flat colours).
typedef enum {
    VM_GRASS_TOP = 0,
    VM_GRASS_SIDE,
    VM_DIRT,
    VM_STONE,
    VM_COBBLE,
    VM_SAND,
    VM_WATER,
    VM_LOG_SIDE,
    VM_LOG_TOP,
    VM_PLANKS,
    VM_LEAVES,
    VM_COAL,
    VM_GLASS,
    VM_TORCH,
    VM_FLOWER_RED,
    VM_FLOWER_YELLOW,
    VM_TALL_GRASS,
    VM_LEAVES_FAST,
    VM_BEDROCK,
    VM_GRAVEL,
    // A sign's front, one texture per text (tools/make_textures.py draws
    // them): VM_SIGN_0 + voxel_sign_text().
    VM_SIGN_0,
    VM_SIGN_1,
    VM_SIGN_2,
    VM_TABLE_TOP,
    VM_TABLE_SIDE,
    VM_FURNACE_FRONT,
    VM_FURNACE_TOP,
    VM_COUNT
} vox_mat_t;

// How many sign texts there are, and which one the sign standing at
// (x, y, z) shows. Signs are generated only (D-79) and their texts are a
// fixed list, so a sign's text needs no storage: it follows from where it
// stands, the same every time the chunk is meshed.
//
//   0  "Kurt" / "was here"
//   1  "Wolfie" / "was here"
//   2  "Far Lands" / "or Bust!"
#define VOX_SIGN_TEXTS 3
int voxel_sign_text(int32_t x, int32_t y, int32_t z);

typedef enum {
    VOX_MESH_FANCY,
    VOX_MESH_FAST
} vox_mesh_mode_t;

typedef enum {
    VF_TOP,
    VF_SIDE,
    VF_BOTTOM
} vox_face_t;

// The material of `face` of a block (voxel_world.h's vox_block_t); -1
// for air.
int voxel_face_mat(uint8_t block, vox_face_t face);

// What the mesher reads: a box of w x h x d cells plus a border of one
// cell all round (the neighbours decide which faces show), stored like
// the world -- columns of h + 2 cells, bottom up, the columns row by row
// in x then z: cell (x, y, z), each from -1 to w/h/d, is
// cells[((z + 1) * (w + 2) + (x + 1)) * (h + 2) + (y + 1)].
typedef struct {
    uint8_t const* cells;
    int            w, h, d;
    // Where cell (0, 0, 0) sits, in cells -- multiplied by `step` like
    // every other coordinate, so a half-resolution box offsets by the
    // same number of blocks as a full-resolution one.
    //
    // `y0` is what makes a VERTICAL SECTION possible: a tall chunk is
    // meshed as several short boxes stacked up, each carrying its own
    // bounding box so the frustum test can throw away the half that is
    // underground (D-34). The cells above and below a section come from
    // the sections next door, as the one-cell border, so the faces at
    // the seam come out exactly as they would have from one tall box --
    // which is what tools/meshcheck_assets.h checks.
    int            x0, y0, z0;
    int            step;   // blocks per cell: 1, or 2 for a half-resolution world
    bool           skirt;  // side faces on the box's outer edges whatever is beyond
    // The light of each cell (light.h: sky << 4 | block), laid out
    // exactly like `cells`, or NULL for full daylight everywhere. A face
    // takes the light of the cell IN FRONT of it -- the air it faces --
    // and faces only merge with neighbours lit the same, so a torch's
    // pool of light is not smeared across a whole wall.
    uint8_t const* lights;
    // Each cell's BLOCK DATA FIELD -- chunk.h's st_data(), already
    // extracted by the caller, because this file may not include
    // chunk.h (blocks.h includes this one). Laid out exactly like
    // `cells`, or NULL for "all zero".
    //
    // Only blocks whose SHAPE depends on it read it: a torch, which
    // stands in the middle of its cell or against one of four walls.
    // The coarse levels pass NULL and get upright torches, which at
    // half resolution nobody can tell from the other kind.
    uint8_t const* data;
} vox_grid_t;

// A torch's ST_DATA: where it is, which is to say which wall is holding
// it up. blocks.h keeps the block, this keeps its shape.
#define TORCH_FLOOR    0
#define TORCH_WALL_NX  1  // the wall is at -x, so the torch sits at that edge
#define TORCH_WALL_PX  2
#define TORCH_WALL_NZ  3
#define TORCH_WALL_PZ  4

// A single block for drawing outside the world (a dropped item, a block
// popping into place, one in the hand): a closed cube of side 2 * half
// round the origin, the texture once on each face; material 0 on top, 1
// on the four sides, 2 underneath (voxel_cube_mats() fills them in for a
// block).
void voxel_build_cube(mesh_t* m, float half);

// Append the visible faces of the grid's box to `m`, in world
// coordinates (block units: cell coordinates times `step`).
void voxel_mesh_build(mesh_t* m, vox_grid_t const* g, vox_mesh_mode_t mode);
