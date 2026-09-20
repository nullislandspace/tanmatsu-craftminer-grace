#pragma once
// =====================================================================
//  Showreel  --  submitting meshes to the scene
// ---------------------------------------------------------------------
//  The materials are handed over at submit time, not stored in the mesh,
//  so one mesh can be drawn in several liveries (e.g. two marauders of
//  the same type in different paint).
//  Lifted from tanmatsu-showreel-grace,
//  main/mesh_render.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdint.h>
#include "math/mesh.h"
#include "synthengine3d.h"

typedef struct {
    se_texture_t const* tex;    // NULL: flat `argb`
    uint32_t            argb;   // flat colour, and the fallback if tex failed to load
    uint32_t            flags;  // SE_TRI_* (e.g. SE_TRI_EMISSIVE)
} mesh_mat_t;

// Transform `m` by `x`, cull faces turned away from the current camera
// eye, and submit the rest (scene_textured_tri / scene_tri). A triangle
// whose material index is >= mat_n is skipped. Call after the scene's
// camera is set.
// Submit a mesh that is ALREADY in world space, offset by `origin_rel`.
//
// Two things make this the path chunks take:
//
//  * no transform. mesh_submit() runs a full xform_apply (9 multiplies,
//    6 adds) per vertex through a PSRAM scratch array, which for a chunk
//    is pure waste -- its vertices are already where they belong. This
//    adds three floats and reads m->v directly. The showreel measured
//    mesh_submit at 14-16 ms a frame once quarter-resolution rendering
//    made it the bottleneck; this is that work removed.
//
//  * the offset IS the point, not a convenience. Chunk meshes are built
//    in chunk-local coordinates and placed relative to a moving render
//    origin, so the floats reaching the rasteriser stay small and exact
//    however far from 0,0 the player has walked. Out at the Far Lands a
//    world-space float has lost about 0.008 of a block -- visible
//    jitter, exactly where the terrain is meant to be strange for other
//    reasons (claudeplans/craftminer.md, D-01).
//
// Back-face culling uses mesh_tri_t.dir where it is set: for an
// axis-aligned face that is one subtract and one compare, against the
// cross product and dot that tri_faces_point needs. Triangles with
// MESH_DIR_NONE fall back to the general test.
void mesh_submit_world(mesh_t const* m, vec3_t origin_rel, mesh_mat_t const* mats, int mat_n);

// Triangles looked at and triangles actually handed to the scene since
// the last reset. The ratio says whether submit time is going on the
// cull or on the engine, which is not guessable from the outside.
void mesh_submit_counters(int* tested, int* passed);
void mesh_submit_counters_reset(void);

void mesh_submit(mesh_t const* m, xform_t const* x, mesh_mat_t const* mats, int mat_n);

// The same for one part of `m` (mesh.h: one builder call's solid), e.g.
// a fragment of an exploding ship, each with its own `x`. Only that
// part's vertices are transformed.
void mesh_submit_part(mesh_t const* m, int part, xform_t const* x, mesh_mat_t const* mats, int mat_n);

// World-space vertices of the last mesh_submit() (valid until the next
// one), e.g. for drawing an outline over it. After mesh_submit_part(),
// only that part's entries are current.
vec3_t const* mesh_last_world(void);
