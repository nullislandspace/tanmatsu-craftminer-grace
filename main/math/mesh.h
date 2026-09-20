#pragma once
// =====================================================================
//  Showreel  --  static meshes and procedural builders
// ---------------------------------------------------------------------
//  A mesh is a vertex list plus triangles, each with a material index
//  and its own three texture coordinates (so a texture seam never needs
//  a duplicated vertex). Pure data: no engine calls, so the host-side
//  check (tools/meshcheck.c) can build and verify every generator.
//  Submitting a mesh to the scene is mesh_render.h.
//
//  Winding: every builder emits triangles whose (b - a) x (c - a) points
//  OUT of the solid (xform.h), and shares vertices between the faces of
//  one solid, so each solid is a closed, consistently wound surface --
//  which is exactly what the mesh check verifies.
//
//  Builders work in the mesh's own model space, around the z axis where
//  they have one; mesh_transform_from() then moves the part just built
//  into place (e.g. a spoke rotated out to its angle).
//  Lifted from tanmatsu-showreel-grace,
//  main/mesh.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "math/xform.h"

// A triangle. `dir` names the axis-aligned direction its face looks
// along, when it has one: the greedy mesher's faces always do, and that
// turns back-face culling from a cross product and a dot into one
// compare (mesh_render.h, mesh_submit_world). MESH_DIR_NONE for
// anything else -- a ship's hull, a creature -- which is culled the
// general way.
//
// It costs nothing: the struct was already 32 bytes with a byte of
// padding here, and it still is.
#define MESH_DIR_PX   0
#define MESH_DIR_NX   1
#define MESH_DIR_PY   2
#define MESH_DIR_NY   3
#define MESH_DIR_PZ   4
#define MESH_DIR_NZ   5
#define MESH_DIR_NONE 0xFFu

typedef struct {
    uint16_t a, b, c;
    uint8_t  mat;
    uint8_t  dir;  // MESH_DIR_*, or MESH_DIR_NONE
    float    uv[3][2];
} mesh_tri_t;

// One closed solid of a mesh: what one builder call made (a box, a
// cylinder, a loft...). Its triangles reference only its own vertices.
typedef struct {
    int v0, vn;  // vertices [v0, v0 + vn)
    int t0, tn;  // triangles [t0, t0 + tn)
} mesh_part_t;

typedef struct {
    vec3_t*      v;
    int          vn, vcap;
    mesh_tri_t*  t;
    int          tn, tcap;
    bool         failed;  // an allocation failed; the mesh is incomplete
    char const*  name;    // for diagnostics (logs, make scenecheck); may be NULL
    // The solids, in build order: every builder call below adds one
    // (raw mesh_vert / mesh_tri do not). An exploding ship submits them
    // one by one, each with its own motion (mesh_submit_part).
    mesh_part_t* parts;
    int          pn, pcap;
    // Stamped onto every triangle added from now on. The mesher sets it
    // per face; everything else leaves it at MESH_DIR_NONE.
    uint8_t      dir;
} mesh_t;

// Start an empty mesh. Storage grows as parts are added (PSRAM on the
// badge).
void mesh_init(mesh_t* m);
void mesh_free(mesh_t* m);

// Raw access. Return the new vertex index, or -1 once the mesh has
// failed (or grown past 65535 vertices).
int  mesh_vert(mesh_t* m, vec3_t p);
// Every triangle added after this carries `dir` (MESH_DIR_*).
void mesh_set_dir(mesh_t* m, uint8_t dir);

void mesh_tri(mesh_t* m, int a, int b, int c, uint8_t mat, float const uv[3][2]);
// Quad a-b-c-d in outward (CCW-seen-from-outside) order, as (a,b,c) +
// (a,c,d). uv[4][2] in the same corner order.
void mesh_quad(mesh_t* m, int a, int b, int c, int d, uint8_t mat, float const uv[4][2]);

// Centre (mean of the vertices) of part `part`, in model space: the
// pivot a flying fragment tumbles about.
vec3_t mesh_part_centre(mesh_t const* m, int part);

// Apply `x` to every vertex from index `first` on (the part just built).
// Rotations and uniform scales keep the winding outward.
void mesh_transform_from(mesh_t* m, int first, xform_t const* x);

// Axis-aligned box lo..hi. Planar mapping per face: one texture repeat
// every `uv_repeat` model units.
void mesh_box(mesh_t* m, vec3_t lo, vec3_t hi, uint8_t mat, float uv_repeat);

// Cylinder around the z axis, radius r, from z0 to z1 (z0 < z1), with
// `sides` facets. Caps optional (a capless end leaves the solid open,
// fine for an end buried inside another part -- the mesh check then
// reports it as open, so only do that on purpose). The side wraps the
// texture round the circumference; caps map planar.
void mesh_cylinder(mesh_t* m, float r, float z0, float z1, int sides, bool cap0, bool cap1, uint8_t mat_side,
                   uint8_t mat_cap, float uv_repeat);

// Ring around the z axis with a rectangular cross-section: inner radius
// r_in, outer r_out, from z0 to z1, `segs` segments. Four surfaces:
// outer (facing away from the axis), inner (facing the axis) and the two
// flat annuli (facing -z at z0, +z at z1).
void mesh_ring(mesh_t* m, float r_in, float r_out, float z0, float z1, int segs, uint8_t mat_outer, uint8_t mat_inner,
               uint8_t mat_face, float uv_repeat);

// Cone/pyramid around the z axis: base circle radius r at z0, apex at z1
// (either direction), `sides` facets, base capped.
void mesh_cone(mesh_t* m, float r, float z0, float z1, int sides, uint8_t mat_side, uint8_t mat_base, float uv_repeat);

// Loft: `n_sec` cross-sections of `n_pts` points each, section k lying
// in the plane z = z[k] (z strictly increasing), joined side to side and
// capped at both ends. Point i of every section is (xy[k][i][0],
// xy[k][i][1]); list each section's points counter-clockwise seen from
// +z, and keep every section convex around (cx, cy) = its own centroid.
// A section may be tiny (a near-point nose) but must not be degenerate.
// Sides map u round the section, v along z; caps map planar.
#define MESH_LOFT_MAX_PTS 12
void mesh_loft(mesh_t* m, int n_sec, int n_pts, float const z[], float const (*xy)[MESH_LOFT_MAX_PTS][2],
               uint8_t mat_side, uint8_t mat_cap, float uv_repeat);

// Stroke: a path of `n` points in the xy plane, drawn `width` wide and
// extruded from z0 to z1 -- one stroke of a block letter. Joints are
// mitred, so the whole path is one closed solid: a closed path
// (`closed`) is a ring, an open one gets flat end caps exactly at its
// first and last point. Keep every turn at most 90 degrees and every
// segment long enough that the mitres of its two ends do not meet (the
// mesh check catches a stroke that folds over). The faces at z0 and z1
// map planar (u = x, v = -y); the sides map u along the path, v along z.
#define MESH_STROKE_MAX_PTS 16
void mesh_stroke(mesh_t* m, int n, float const (*pts)[2], bool closed, float width, float z0, float z1,
                 uint8_t mat_face, uint8_t mat_side, float uv_repeat);

// Sphere round the origin, radius r: `segs` segments round the equator
// (longitude) and `rings` from pole to pole (latitude), poles on +-y.
// Maps an equirectangular texture once: u = longitude (0..1 round, from
// +z towards +x), v = latitude (0 at the north pole, 1 at the south).
void mesh_sphere(mesh_t* m, float r, int segs, int rings, uint8_t mat);

// Blob: an icosahedron subdivided `subdiv` times (20 * 4^subdiv
// triangles), each vertex pushed out along its direction to
// radius(dir, user). Closed whatever radius() returns, as long as it is
// positive and smooth enough that no face folds over. Maps planar.
void mesh_blob(mesh_t* m, int subdiv, float (*radius)(vec3_t dir, void* user), void* user, uint8_t mat,
               float uv_repeat);

// Signed volume of the triangles from index `first_tri` on (positive
// for a closed outward-wound solid).
float mesh_signed_volume(mesh_t const* m, int first_tri);

// Where a ray first strikes the mesh posed by `x`: from world point
// `from` along `dir` (normalised), no further than `max`. True with the
// distance in *dist when it strikes a triangle (either side); a laser
// beam that hits stops there.
bool mesh_raycast(mesh_t const* m, xform_t const* x, vec3_t from, vec3_t dir, float max, float* dist);
