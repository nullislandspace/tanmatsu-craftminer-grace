#pragma once
// =====================================================================
//  CraftMiner  --  turning a chunk into triangles
// ---------------------------------------------------------------------
//  The greedy mesher (voxel/voxel_mesh.h) reads a dense box of cells
//  with a one-cell border, because whether a face shows depends on the
//  neighbour behind it -- including neighbours in the next chunk along.
//  This copies a resident chunk and that border out of the world into
//  such a box and runs the mesher over it.
//
//  IN CHUNK-LOCAL COORDINATES. The mesh comes out with its origin at
//  the chunk's corner, not in world space, so the floats stay small
//  however far the player has walked; the renderer places it with an
//  offset (mesh_render.h, mesh_submit_world). That is D-01.
//
//  THE SCRATCH BOX IS THE CALLER'S. The donor meshed through a shared
//  static buffer, which races the moment a second task meshes (F-08).
//  Here whoever meshes brings their own, so the core-1 worker and a
//  synchronous build on the main task cannot collide.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/mesh.h"
#include "world/chunk.h"

// Bytes of scratch a caller must provide. Sized for the largest level
// of detail; the coarse one needs less and simply uses part of it.
size_t chunkmesh_scratch_bytes(void);

// Build `lod` of the chunk at (cx, cz) into `out`, which is initialised
// here (the caller frees it). `scratch` must be at least
// chunkmesh_scratch_bytes() and is used only during the call.
//
// Reads the chunk and its four neighbours through world_block(), so a
// neighbour that is not resident reads as BLK_BARRIER and the border
// comes out solid -- which is right: an unloaded neighbour hides the
// faces behind it rather than opening a hole that would pop when it
// arrives.
//
// False if the chunk is not resident or the mesh could not be built.
bool chunkmesh_build(int32_t cx, int32_t cz, int lod, uint8_t* scratch, mesh_t* out);
