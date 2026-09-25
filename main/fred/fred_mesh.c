// =====================================================================
//  SynthMiner  --  the miner's meshes (see fred_mesh.h)
// =====================================================================

#include "fred/fred_mesh.h"

// A box with its own material on each face (+x, -x, +y, -y, +z, -z), the
// texture once across each: the head, whose front is the face. Eight
// shared corners, so it is a closed solid; faces wound outward.
static void box6(mesh_t* m, vec3_t lo, vec3_t hi, uint8_t const mat[6]) {
    int v[8];
    for (int i = 0; i < 8; i++) v[i] = mesh_vert(m, v3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
    float const uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    // Corners top-left, top-right, bottom-right, bottom-left, seen from
    // outside (u right, v down on the sides).
    mesh_quad(m, v[3], v[7], v[5], v[1], mat[0], uv);  // +x
    mesh_quad(m, v[6], v[2], v[0], v[4], mat[1], uv);  // -x
    mesh_quad(m, v[2], v[6], v[7], v[3], mat[2], uv);  // +y
    mesh_quad(m, v[0], v[1], v[5], v[4], mat[3], uv);  // -y
    mesh_quad(m, v[7], v[6], v[4], v[5], mat[4], uv);  // +z: the front
    mesh_quad(m, v[2], v[3], v[1], v[0], mat[5], uv);  // -z
}

void fred_build_head(mesh_t* m) {
    mesh_init(m);
    m->name               = "fred_head";
    uint8_t const head[6] = {FM_SKIN, FM_SKIN, FM_HAIR, FM_SKIN, FM_FACE, FM_HAIR};
    box6(m, v3(-0.25f, 0.0f, -0.25f), v3(0.25f, 0.5f, 0.25f), head);
    // The hard hat: a crown, a brim sticking out in front, the lamp.
    mesh_box(m, v3(-0.28f, 0.44f, -0.28f), v3(0.28f, 0.64f, 0.28f), FM_HAT, 1.0f);
    mesh_box(m, v3(-0.30f, 0.42f, 0.20f), v3(0.30f, 0.47f, 0.42f), FM_HAT, 1.0f);
    mesh_box(m, v3(-0.07f, 0.49f, 0.27f), v3(0.07f, 0.61f, 0.33f), FM_LAMP, 1.0f);
}

void fred_build_body(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_body";
    mesh_box(m, v3(-0.25f, 0.0f, -0.15f), v3(0.25f, 0.36f, 0.15f), FM_OVERALLS, 1.0f);
    mesh_box(m, v3(-0.25f, 0.36f, -0.15f), v3(0.25f, FRED_BODY_H, 0.15f), FM_SHIRT, 1.0f);
    // The bib of the overalls, on the chest.
    mesh_box(m, v3(-0.15f, 0.36f, 0.15f), v3(0.15f, 0.58f, 0.17f), FM_OVERALLS, 1.0f);
}

void fred_build_arm(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_arm";
    mesh_box(m, v3(-0.1f, -0.30f, -0.1f), v3(0.1f, 0.0f, 0.1f), FM_SHIRT, 1.0f);
    mesh_box(m, v3(-0.09f, -FRED_ARM_H, -0.09f), v3(0.09f, -0.30f, 0.09f), FM_SKIN, 1.0f);
}

void fred_build_fp_arm(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_fp_arm";
    // The sleeve runs to +0.55 rather than stopping at the shoulder:
    // rotated forward for first person that puts the open end behind
    // the camera and below the view, where its cap cannot be seen.
    mesh_box(m, v3(-0.1f, -0.30f, -0.1f), v3(0.1f, 0.55f, 0.1f), FM_SHIRT, 1.0f);
    mesh_box(m, v3(-0.09f, -FRED_ARM_H, -0.09f), v3(0.09f, -0.30f, 0.09f), FM_SKIN, 1.0f);
}

void fred_build_leg(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_leg";
    mesh_box(m, v3(-0.11f, -0.56f, -0.12f), v3(0.11f, 0.0f, 0.12f), FM_OVERALLS, 1.0f);
    mesh_box(m, v3(-0.12f, -FRED_LEG_H, -0.13f), v3(0.12f, -0.56f, 0.15f), FM_BOOTS, 1.0f);
}

void fred_build_pick(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_pick";
    // The handle through the fist, forward; the head across its end,
    // pointed at both tips.
    mesh_box(m, v3(-0.035f, -0.035f, -0.12f), v3(0.035f, 0.035f, 0.62f), FM_WOOD, 1.0f);
    mesh_box(m, v3(-0.045f, -0.24f, 0.52f), v3(0.045f, 0.24f, 0.62f), FM_IRON, 1.0f);
    mesh_box(m, v3(-0.03f, 0.24f, 0.50f), v3(0.03f, 0.34f, 0.58f), FM_IRON, 1.0f);
    mesh_box(m, v3(-0.03f, -0.34f, 0.50f), v3(0.03f, -0.24f, 0.58f), FM_IRON, 1.0f);
}

void fred_build_axe(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_axe";
    // The handle, then a broad blade to one side of its end.
    mesh_box(m, v3(-0.035f, -0.035f, -0.12f), v3(0.035f, 0.035f, 0.62f), FM_WOOD, 1.0f);
    mesh_box(m, v3(-0.03f, 0.03f, 0.40f), v3(0.03f, 0.24f, 0.62f), FM_IRON, 1.0f);
}

void fred_build_shovel(mesh_t* m) {
    mesh_init(m);
    m->name = "fred_shovel";
    // A longer handle and a flat spade across its end.
    mesh_box(m, v3(-0.035f, -0.035f, -0.12f), v3(0.035f, 0.035f, 0.56f), FM_WOOD, 1.0f);
    mesh_box(m, v3(-0.10f, -0.02f, 0.54f), v3(0.10f, 0.02f, 0.78f), FM_IRON, 1.0f);
}
