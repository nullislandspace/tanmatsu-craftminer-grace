// =====================================================================
//  CraftMiner  --  the greedy mesher under the mesh check
// ---------------------------------------------------------------------
//  Included by tools/meshcheck.c. Lifted from the showreel's
//  tools/meshcheck_assets.h (its voxel half), which is what proves the
//  mesher still behaves exactly as it did after its block switches
//  became table lookups in world/blocks.h (F-07).
//
//  The cases assert volume == solid cells and surface area == exposed
//  faces, which is a complete statement of "the mesher emitted the right
//  faces and merged them without changing the solid".
// =====================================================================

#include "voxel/voxel_mesh.h"
#include "world/blocks.h"

#define VG 6  // test grids: 6 x 6 x 6 cells inside a border of air
static uint8_t s_vg[(VG + 2) * (VG + 2) * (VG + 2)];

static uint8_t* vg_cell(int x, int y, int z) {
    return &s_vg[((z + 1) * (VG + 2) + (x + 1)) * (VG + 2) + (y + 1)];
}

static float vg_volume(mesh_t const* m) {
    double v = 0.0;
    for (int i = 0; i < m->tn; i++) {
        vec3_t const a = m->v[m->t[i].a], b = m->v[m->t[i].b], c = m->v[m->t[i].c];
        v += (double)v3_dot(a, v3_cross(b, c)) / 6.0;
    }
    return (float)v;
}

static float vg_area(mesh_t const* m) {
    double s = 0.0;
    for (int i = 0; i < m->tn; i++) {
        vec3_t const a = m->v[m->t[i].a], b = m->v[m->t[i].b], c = m->v[m->t[i].c];
        s += 0.5 * (double)v3_len(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    }
    return (float)s;
}

// Mesh the test grid; check volume and area (only for a lump in open
// air: `closed`) and the triangle count (-1: any).
static void vg_case(char const* name, vox_mesh_mode_t mode, int step, bool skirt, bool closed, int tris) {
    int solid = 0, exposed = 0;
    for (int z = -1; z <= VG; z++) {
        for (int x = -1; x <= VG; x++) {
            for (int y = -1; y <= VG; y++) {
                bool const in = x >= 0 && x < VG && y >= 0 && y < VG && z >= 0 && z < VG;
                uint8_t    b  = *vg_cell(x, y, z);
                if (!in || (b != BLK_STONE && b != BLK_GRASS && b != BLK_LEAVES)) continue;
                solid++;
                int const nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int k = 0; k < 6; k++) exposed += *vg_cell(x + nb[k][0], y + nb[k][1], z + nb[k][2]) == BLK_AIR;
            }
        }
    }
    mesh_t m;
    mesh_init(&m);
    vox_grid_t const g = {s_vg, VG, VG, VG, 0, 0, step, skirt};
    voxel_mesh_build(&m, &g, mode);
    check_mesh(name, &m, false);
    float const st = (float)step, vol = vg_volume(&m), area = vg_area(&m);
    printf("  volume %.3f (cells %d), area %.3f (exposed faces %d)\n", vol, solid, area, exposed);
    if (closed) {
        CHECK(fabsf(vol - (float)solid * st * st * st) < 1e-3f, "%s: volume %g, expected %d cells", name, vol, solid);
        CHECK(fabsf(area - (float)exposed * st * st) < 1e-3f, "%s: area %g, expected %d faces", name, area, exposed);
    }
    if (tris >= 0) CHECK(m.tn == tris, "%s: %d triangles, expected %d", name, m.tn, tris);
    mesh_free(&m);
}

static void vg_clear(void) {
    memset(s_vg, BLK_AIR, sizeof(s_vg));
}

static void vg_fill(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t b) {
    for (int z = z0; z <= z1; z++) {
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) *vg_cell(x, y, z) = b;
        }
    }
}

static void check_voxel_mesher(void) {
    vg_clear();
    vg_fill(2, 2, 2, 2, 2, 2, BLK_STONE);
    vg_case("voxel: one block", VOX_MESH_FAST, 1, false, true, 12);
    vg_fill(3, 2, 2, 3, 2, 2, BLK_STONE);
    vg_case("voxel: two blocks (no inner face, merged)", VOX_MESH_FAST, 1, false, true, 12);
    vg_clear();
    vg_fill(1, 1, 1, 3, 3, 3, BLK_STONE);
    vg_case("voxel: 3x3x3 (one quad a side)", VOX_MESH_FAST, 1, false, true, 12);
    vg_case("voxel: 3x3x3 in half-resolution cells", VOX_MESH_FAST, 2, false, true, 12);
    // A random lump: the counts vary, volume and area must not.
    vg_clear();
    unsigned seed = 7;
    for (int i = 0; i < 90; i++) {
        seed = seed * 1103515245u + 12345u;
        vg_fill((int)(seed >> 8) % VG, (int)(seed >> 12) % VG, (int)(seed >> 16) % VG, (int)(seed >> 8) % VG,
                (int)(seed >> 12) % VG, (int)(seed >> 16) % VG, BLK_STONE);
    }
    vg_case("voxel: random lump", VOX_MESH_FAST, 1, false, true, -1);
    // Grass sides never stack: a 3-high column has 3 x 4 side quads.
    vg_clear();
    vg_fill(2, 1, 2, 2, 3, 2, BLK_GRASS);
    vg_case("voxel: grass column (sides one block tall)", VOX_MESH_FAST, 1, false, true, 2 * (2 + 12));
    // Leaves: fancy shows the faces between two leaf blocks, fast does not.
    vg_clear();
    vg_fill(2, 2, 2, 3, 2, 2, BLK_LEAVES);
    {
        mesh_t m;
        mesh_init(&m);
        vox_grid_t const g = {s_vg, VG, VG, VG, 0, 0, 1, false};
        voxel_mesh_build(&m, &g, VOX_MESH_FANCY);
        printf("voxel: two leaf blocks, fancy: %d tris\n", m.tn);
        // 6 outer quads (merged across both) + the 2 inner faces.
        CHECK(m.tn == 2 * (6 + 2), "voxel: fancy leaves: %d triangles, expected 16 (with the 2 inner faces)", m.tn);
        mesh_free(&m);
    }
    vg_case("voxel: two leaf blocks, fast", VOX_MESH_FAST, 1, false, true, 12);
    // A plant: two crossed quads, each from both sides; none when fast.
    vg_clear();
    *vg_cell(2, 0, 2) = BLK_FLOWER_RED;
    {
        mesh_t m;
        mesh_init(&m);
        vox_grid_t const g = {s_vg, VG, VG, VG, 0, 0, 1, false};
        voxel_mesh_build(&m, &g, VOX_MESH_FANCY);
        check_mesh("voxel: a flower (fancy)", &m, false);
        CHECK(m.tn == 8, "voxel: flower: %d triangles, expected 8", m.tn);
        mesh_free(&m);
        mesh_init(&m);
        voxel_mesh_build(&m, &g, VOX_MESH_FAST);
        CHECK(m.tn == 0, "voxel: flower in a fast mesh: %d triangles, expected none", m.tn);
        mesh_free(&m);
    }
    // A skirt: a box filled wall to wall with a solid border all round
    // shows its top only -- with a skirt, its four sides as well.
    memset(s_vg, BLK_STONE, sizeof(s_vg));
    for (int z = -1; z <= VG; z++) {
        for (int x = -1; x <= VG; x++) {
            for (int y = 3; y <= VG; y++) *vg_cell(x, y, z) = BLK_AIR;
        }
    }
    vg_case("voxel: no skirt (top only)", VOX_MESH_FAST, 1, false, false, 2);
    vg_case("voxel: skirt (top and four sides)", VOX_MESH_FAST, 1, true, false, 10);
}

// The miner: every piece a set of closed, outward solids (the head's box
// is built face by face, the rest by mesh_box).
// The standalone cube used for dropped items, block pops and the block
// in the hand. Closed, outward, exactly one cubic unit.
static void check_voxel_cube(void) {
    mesh_t m;
    mesh_init(&m);
    voxel_build_cube(&m, 0.5f);
    check_mesh("voxel cube (items, pops)", &m, true);
    CHECK(fabsf(mesh_signed_volume(&m, 0) - 1.0f) < 1e-4f, "voxel cube: volume %g, expected 1",
          mesh_signed_volume(&m, 0));
    mesh_free(&m);
}

// Every greedy face must declare the direction it actually looks along,
// because the renderer culls by that field alone and a wrong one makes
// a face vanish from the side it should be seen from -- a hole in the
// world, and a subtle one.
static void check_face_dirs(void) {
    vg_clear();
    vg_fill(1, 1, 1, 4, 3, 4, BLK_STONE);
    vg_fill(2, 4, 2, 3, 4, 3, BLK_GRASS);

    mesh_t m;
    mesh_init(&m);
    vox_grid_t const g = {s_vg, VG, VG, VG, 0, 0, 1, false};
    voxel_mesh_build(&m, &g, VOX_MESH_FAST);

    int checked = 0, none = 0;
    for (int i = 0; i < m.tn; i++) {
        mesh_tri_t const* t = &m.t[i];
        if (t->dir == MESH_DIR_NONE) {
            none++;
            continue;
        }
        CHECK(t->dir < 6, "triangle %d has dir %u", i, t->dir);

        // The normal the winding actually gives.
        vec3_t const a = m.v[t->a], b = m.v[t->b], c = m.v[t->c];
        vec3_t const n = v3_cross(v3_sub(b, a), v3_sub(c, a));
        float const  comp[3] = {n.x, n.y, n.z};

        int const   axis = t->dir >> 1;
        float const want = (t->dir & 1) ? -1.0f : 1.0f;

        // It must point along its declared axis, and along no other.
        for (int k = 0; k < 3; k++) {
            if (k == axis) continue;
            CHECK(fabsf(comp[k]) < 1e-4f, "triangle %d says dir %u but its normal has a component on axis %d", i,
                  t->dir, k);
        }
        CHECK(comp[axis] * want > 0.0f, "triangle %d says dir %u but its normal points the other way", i, t->dir);
        checked++;
    }
    printf("voxel: %d greedy faces carry a direction, %d generic\n", checked, none);
    CHECK(checked > 0, "no greedy face declared a direction: the renderer's cull would do nothing");
    CHECK(none == 0, "%d greedy-pass triangles have no direction", none);
    mesh_free(&m);
}

// Plants are emitted with both windings so they can be seen from either
// side, so they must NOT claim a direction -- one of the two copies
// would be culled from the wrong side.
static void check_plant_dirs(void) {
    vg_clear();
    vg_fill(2, 1, 2, 2, 1, 2, BLK_GRASS);
    *vg_cell(2, 2, 2) = BLK_FLOWER_RED;

    mesh_t m;
    mesh_init(&m);
    vox_grid_t const g = {s_vg, VG, VG, VG, 0, 0, 1, false};
    voxel_mesh_build(&m, &g, VOX_MESH_FANCY);

    int plant_tris = 0;
    for (int i = 0; i < m.tn; i++) {
        if (m.t[i].mat == VM_FLOWER_RED) {
            plant_tris++;
            CHECK(m.t[i].dir == MESH_DIR_NONE, "a plant triangle claims direction %u", m.t[i].dir);
        }
    }
    printf("voxel: %d plant triangles, all direction-free\n", plant_tris);
    CHECK(plant_tris == 8, "expected 8 plant triangles, got %d", plant_tris);
    mesh_free(&m);
}

// The entry point tools/meshcheck.c calls.
static void check_assets(void) {
    check_voxel_mesher();
    check_voxel_cube();
    check_face_dirs();
    check_plant_dirs();
}
