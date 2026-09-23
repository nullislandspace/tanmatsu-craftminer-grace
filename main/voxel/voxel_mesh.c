// =====================================================================
//  CraftMiner  --  meshing blocks (see voxel_mesh.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/craftminer/voxel/voxel_mesh.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/voxel_mesh.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "common/psram.h"
#include "world/blocks.h"

int voxel_face_mat(uint8_t block, vox_face_t face) {
    block_def_t const* d = block_def(block);
    return d->kind == K_AIR ? -1 : (int)d->mat[face];
}

// Whether the face of cube `b` towards neighbour `n` shows.
static bool face_shows(uint8_t b, uint8_t n, vox_mesh_mode_t mode) {
    switch (block_kind(n)) {
        case K_CUBE:
            return false;
        case K_SEE:
            // A see-through block against a different block always shows;
            // against its own id only if it is marked BF_SEE_SELF (leaves
            // are a canopy you see into, glass hides glass).
            return mode == VOX_MESH_FANCY && (n != b || (block_def(b)->flags & BF_SEE_SELF) != 0);
        case K_LIQUID:
            // A liquid never hides anything, in either mode. Without
            // this the bed of a lake is never meshed and the surface is
            // a lid over nothing (D-86).
            return true;
        default:
            return true;  // air, a plant, a torch
    }
}

// The face's material in this kind of mesh: leaves are opaque unless
// fancy.
static int mode_mat(uint8_t b, vox_face_t face, vox_mesh_mode_t mode) {
    int const m = voxel_face_mat(b, face);
    return m == VM_LEAVES && mode != VOX_MESH_FANCY ? VM_LEAVES_FAST : m;
}

// The six face directions: the axis the face looks along (0 x, 1 y,
// 2 z) and which way.
typedef struct {
    int axis, sign;
} dir_t;

static dir_t const DIRS[6] = {{0, +1}, {0, -1}, {1, +1}, {1, -1}, {2, +1}, {2, -1}};

// One merged rectangle of faces, emitted as a quad whose corners run
// counter-clockwise seen from outside, texture u along its first edge
// and v along its second (v downwards on side faces, so the grass strip
// is at the top). The slice is the block layer the faces belong to; the
// rectangle spans [p0, p0+wp) x [q0, q0+hq) of the plane's two axes:
// (z, y) for x faces, (x, y) for z faces, (x, z) for y faces.
static void emit_f(mesh_t* m, dir_t dir, float plane, float P0, float P1, float Q0, float Q1, float W, float H,
                   uint8_t mat) {
    // Which way this face looks, so the renderer can cull it with one
    // compare instead of a cross product (mesh.h, MESH_DIR_*).
    mesh_set_dir(m, (uint8_t)(dir.axis * 2 + (dir.sign > 0 ? 0 : 1)));
    vec3_t c[4];
    float  uv[4][2] = {{0.0f, 0.0f}, {W, 0.0f}, {W, H}, {0.0f, H}};
    switch (dir.axis * 2 + (dir.sign < 0)) {
        case 0: {  // +x: u along +z, v down
            float const x = plane;
            c[0] = v3(x, Q1, P0), c[1] = v3(x, Q1, P1), c[2] = v3(x, Q0, P1), c[3] = v3(x, Q0, P0);
        } break;
        case 1: {  // -x: u along -z, v down
            float const x = plane;
            c[0] = v3(x, Q1, P1), c[1] = v3(x, Q1, P0), c[2] = v3(x, Q0, P0), c[3] = v3(x, Q0, P1);
        } break;
        case 4: {  // +z: u along -x, v down
            float const z = plane;
            c[0] = v3(P1, Q1, z), c[1] = v3(P0, Q1, z), c[2] = v3(P0, Q0, z), c[3] = v3(P1, Q0, z);
        } break;
        case 5: {  // -z: u along +x, v down
            float const z = plane;
            c[0] = v3(P0, Q1, z), c[1] = v3(P1, Q1, z), c[2] = v3(P1, Q0, z), c[3] = v3(P0, Q0, z);
        } break;
        case 2: {  // +y: u along +z, v along +x
            float const y = plane;
            c[0] = v3(P0, y, Q0), c[1] = v3(P0, y, Q1), c[2] = v3(P1, y, Q1), c[3] = v3(P1, y, Q0);
            float const t[4][2] = {{0.0f, 0.0f}, {H, 0.0f}, {H, W}, {0.0f, W}};
            memcpy(uv, t, sizeof(uv));
        } break;
        default: {  // -y: u along +x, v along +z
            float const y = plane;
            c[0] = v3(P0, y, Q0), c[1] = v3(P1, y, Q0), c[2] = v3(P1, y, Q1), c[3] = v3(P0, y, Q1);
        } break;
    }
    int const a = mesh_vert(m, c[0]), b = mesh_vert(m, c[1]), cc = mesh_vert(m, c[2]), d = mesh_vert(m, c[3]);
    if (a < 0 || b < 0 || cc < 0 || d < 0) return;
    mesh_quad(m, a, b, cc, d, mat, uv);
}

// One merged rectangle of whole cube faces in block layer `slice`.
static void emit(mesh_t* m, dir_t dir, int slice, int p0, int q0, int wp, int hq, float step, uint8_t mat) {
    emit_f(m, dir, ((float)slice + (dir.sign > 0 ? 1.0f : 0.0f)) * step, (float)p0 * step, (float)(p0 + wp) * step,
           (float)q0 * step, (float)(q0 + hq) * step, (float)wp * step, (float)hq * step, mat);
}

// A box lo..hi with its texture once across every face (the torch).
static void emit_box(mesh_t* m, vec3_t lo, vec3_t hi, uint8_t mat) {
    mesh_set_dir(m, MESH_DIR_NONE);  // not a greedy face; cull it the general way
    emit_f(m, DIRS[0], hi.x, lo.z, hi.z, lo.y, hi.y, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[1], lo.x, lo.z, hi.z, lo.y, hi.y, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[2], hi.y, lo.x, hi.x, lo.z, hi.z, 0.2f, 1.0f, mat);  // the glowing tip (v 0..0.2)
    emit_f(m, DIRS[3], lo.y, lo.x, hi.x, lo.z, hi.z, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[4], hi.z, lo.x, hi.x, lo.y, hi.y, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[5], lo.z, lo.x, hi.x, lo.y, hi.y, 1.0f, 1.0f, mat);
}

// A box lo..hi with `front` on its +x face and `mat` on the others, each
// face's texture once across it (the sign's post and board).
static void emit_box_mats(mesh_t* m, vec3_t lo, vec3_t hi, uint8_t mat, uint8_t front) {
    mesh_set_dir(m, MESH_DIR_NONE);
    emit_f(m, DIRS[0], hi.x, lo.z, hi.z, lo.y, hi.y, 1.0f, 1.0f, front);
    emit_f(m, DIRS[1], lo.x, lo.z, hi.z, lo.y, hi.y, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[2], hi.y, lo.x, hi.x, lo.z, hi.z, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[3], lo.y, lo.x, hi.x, lo.z, hi.z, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[4], hi.z, lo.x, hi.x, lo.y, hi.y, 1.0f, 1.0f, mat);
    emit_f(m, DIRS[5], lo.z, lo.x, hi.x, lo.y, hi.y, 1.0f, 1.0f, mat);
}

int voxel_sign_text(int32_t x, int32_t y, int32_t z) {
    uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u ^ (uint32_t)z * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (int)(h % VOX_SIGN_TEXTS);
}

// A standing sign, facing east (+x): a post up to half the cell, and a
// board across the whole cell above it, its front the text (one texture
// across the board: 64 x 32 texels on 1 x 0.5 blocks).
static void emit_sign(mesh_t* m, int X, int Y, int Z) {
    float const cx = (float)X + 0.5f, cz = (float)Z + 0.5f, t = 1.0f / 16.0f;
    uint8_t const front = (uint8_t)(VM_SIGN_0 + voxel_sign_text(X, Y, Z));
    emit_box_mats(m, v3(cx - t, (float)Y, cz - t), v3(cx + t, (float)Y + 0.5f, cz + t), VM_PLANKS, VM_PLANKS);
    emit_box_mats(m, v3(cx - t, (float)Y + 0.5f, (float)Z), v3(cx + t, (float)Y + 1.0f, (float)Z + 1.0f), VM_PLANKS, front);
}

// A plant: two vertical quads along the cell's diagonals, each twice
// (one per side), the texture upright and unmirrored from both.
static void emit_plant(mesh_t* m, int x, int y, int z, uint8_t mat) {
    // Both windings of each quad are emitted, so a plant is visible from
    // either side. That is the opposite of a directional face: leave it
    // MESH_DIR_NONE and let the general cull drop whichever copy faces
    // away.
    mesh_set_dir(m, MESH_DIR_NONE);
    float const  X = (float)x, Y = (float)y, Z = (float)z;
    float const  uv[4][2]   = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    vec3_t const ends[2][2] = {{{X, 0, Z}, {X + 1.0f, 0, Z + 1.0f}}, {{X + 1.0f, 0, Z}, {X, 0, Z + 1.0f}}};
    for (int k = 0; k < 2; k++) {
        for (int side = 0; side < 2; side++) {
            vec3_t const p = ends[k][side], q = ends[k][1 - side];
            int const    a = mesh_vert(m, v3(p.x, Y + 1.0f, p.z)), b = mesh_vert(m, v3(q.x, Y + 1.0f, q.z));
            int const    c = mesh_vert(m, v3(q.x, Y, q.z)), d = mesh_vert(m, v3(p.x, Y, p.z));
            if (a < 0 || b < 0 || c < 0 || d < 0) return;
            mesh_quad(m, a, b, c, d, mat, uv);
        }
    }
}

void voxel_build_cube(mesh_t* m, float half) {
    int v[8];
    for (int i = 0; i < 8; i++)
        v[i] = mesh_vert(m, v3(i & 1 ? half : -half, i & 2 ? half : -half, i & 4 ? half : -half));
    float const uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    // The corner orders of emit_f(): seen from outside, u right, v down.
    mesh_quad(m, v[3], v[7], v[5], v[1], 1, uv);  // +x
    mesh_quad(m, v[6], v[2], v[0], v[4], 1, uv);  // -x
    mesh_quad(m, v[2], v[6], v[7], v[3], 0, uv);  // +y
    mesh_quad(m, v[0], v[1], v[5], v[4], 2, uv);  // -y
    mesh_quad(m, v[7], v[6], v[4], v[5], 1, uv);  // +z
    mesh_quad(m, v[2], v[3], v[1], v[0], 1, uv);  // -z
}

void voxel_mesh_build(mesh_t* m, vox_grid_t const* g, vox_mesh_mode_t mode) {
    int const      w = g->w, h = g->h, d = g->d, H = h + 2, W = w + 2;
    uint8_t const* cells = g->cells;
#define CIDX(x, y, z) (((size_t)((z) + 1) * (size_t)W + (size_t)((x) + 1)) * (size_t)H + (size_t)((y) + 1))
#define CELL(x, y, z) cells[CIDX(x, y, z)]
    uint8_t const* lights = g->lights;
#define LIGHT(x, y, z) (lights != NULL ? lights[CIDX(x, y, z)] : (uint8_t)MESH_LIGHT_FULL)

    // Faces only exist between a cube and a cell that is not one: from
    // one below the lowest such cell (border included) up to the highest
    // non-air cell of the box. Below is rock, above is sky.
    int ylo = h, yhi = -1;
    for (int z = -1; z <= d; z++) {
        for (int x = -1; x <= w; x++) {
            for (int y = -1; y <= h; y++) {
                block_kind_t const k = block_kind(CELL(x, y, z));
                if (k != K_CUBE && y - 1 < ylo) ylo = y - 1;
                if (k != K_AIR && x >= 0 && x < w && z >= 0 && z < d && y >= 0 && y < h && y > yhi) yhi = y;
            }
        }
    }
    if (ylo < 0) ylo = 0;
    if (yhi < ylo) return;
    int const band = yhi - ylo + 1;

    // The mask of one slice: the face at (p, q) as light << 8 | material
    // + 1, 0 none. Light in the key is what stops a lit face merging
    // with a dark one.
    int const side = w > d ? w : d;
    uint16_t* mask = cm_calloc((size_t)side * (size_t)(side > band ? side : band),
                               sizeof(uint16_t));  // F-12: PSRAM, not the scarce internal heap
    if (!mask) {
        m->failed = true;
        return;
    }
    float const step = (float)g->step;
    for (int k = 0; k < 6; k++) {
        dir_t const dir = DIRS[k];
        // The slices along the face's axis, and each slice's (p, q) plane.
        int         s0, s1, pn, qn, q0;
        switch (dir.axis) {
            case 0:
                s0 = 0, s1 = w, pn = d, q0 = ylo, qn = band;
                break;
            case 2:
                s0 = 0, s1 = d, pn = w, q0 = ylo, qn = band;
                break;
            default:
                // ONE SLICE HIGHER than the highest block, because the
                // underside of a water surface is emitted by the AIR
                // cell above the liquid (see the mask below) and that
                // cell is, by definition, above the highest block.
                //
                // NEVER past the box, though: a cell outside it belongs
                // to the neighbouring section and is only ever a
                // neighbour here. Letting one act as an owner emitted
                // its faces twice, once from each side of the seam --
                // which meshcheck's sectioning test caught at once
                // (volume 186.667 against 148).
                //
                // A lake whose top cell IS the box's top needs no slice
                // here: the section above owns the air over it and emits
                // the underside from its own y = 0.
                s0 = ylo, s1 = yhi + 2 > h ? h : yhi + 2, pn = w, q0 = 0, qn = d;
                break;
        }
        int const        nx = dir.axis == 0 ? dir.sign : 0, ny = dir.axis == 1 ? dir.sign : 0;
        int const        nz    = dir.axis == 2 ? dir.sign : 0;
        vox_face_t const face  = dir.axis != 1 ? VF_SIDE : dir.sign > 0 ? VF_TOP : VF_BOTTOM;
        bool const       sides = dir.axis != 1;
        for (int slice = s0; slice < s1; slice++) {
            for (int q = 0; q < qn; q++) {
                for (int p = 0; p < pn; p++) {
                    int x, y, z;
                    switch (dir.axis) {
                        case 0:
                            x = slice, y = q0 + q, z = p;
                            break;
                        case 2:
                            x = p, y = q0 + q, z = slice;
                            break;
                        default:
                            x = p, y = slice, z = q0 + q;
                            break;
                    }
                    uint8_t const      b    = CELL(x, y, z);
                    uint8_t const      nb   = CELL(x + nx, y + ny, z + nz);
                    block_kind_t const kb   = block_kind(b);
                    uint16_t           v    = 0;
                    bool const         edge = g->skirt && (x + nx < 0 || x + nx >= w || z + nz < 0 || z + nz >= d);
                    if ((kb == K_CUBE || kb == K_SEE) && (edge || face_shows(b, nb, mode))) {
                        // A skirt takes the block's top material: it only
                        // closes a seam, so it should match the ground.
                        v = (uint16_t)(((unsigned)LIGHT(x + nx, y + ny, z + nz) << 8) |
                                       (unsigned)(mode_mat(b, edge ? VF_TOP : face, mode) + 1));
                    } else if (kb == K_LIQUID && ny > 0 && block_kind(nb) == K_AIR) {
                        // THE SURFACE, and the only face a liquid has: no
                        // sides, no bottom, and a top only where there is
                        // air above it. Lit by that air, so it reads as
                        // sky on water rather than as the gloom below.
                        v = (uint16_t)(((unsigned)LIGHT(x + nx, y + ny, z + nz) << 8) |
                                       (unsigned)(voxel_face_mat(b, VF_TOP) + 1));
                    } else if (kb == K_AIR && ny < 0 && block_kind(nb) == K_LIQUID) {
                        // THE UNDERSIDE of that same surface, emitted by
                        // the air cell above it so that it lands on the
                        // same plane (emit() puts a +y face at y+1 and a
                        // -y face at y). An axis-aligned face is visible
                        // only from the side its normal points at, so
                        // without this second copy the surface disappears
                        // the moment the eye goes under it -- which is
                        // exactly how swimming used to look.
                        v = (uint16_t)(((unsigned)LIGHT(x, y, z) << 8) |
                                       (unsigned)(voxel_face_mat(nb, VF_TOP) + 1));
                    }
                    mask[q * pn + p] = v;
                }
            }
            // Greedy: grow each rectangle along p, then along q while
            // every row matches; clear what it covers.
            for (int q = 0; q < qn; q++) {
                for (int p = 0; p < pn;) {
                    uint16_t const v = mask[q * pn + p];
                    if (v == 0) {
                        p++;
                        continue;
                    }
                    int wp = 1;
                    while (p + wp < pn && mask[q * pn + p + wp] == v) wp++;
                    int        hq       = 1;
                    bool const no_stack = sides && (v & 0xFFu) == VM_GRASS_SIDE + 1;
                    while (!no_stack && q + hq < qn) {
                        bool row = true;
                        for (int i = 0; i < wp && row; i++) row = mask[(q + hq) * pn + p + i] == v;
                        if (!row) break;
                        hq++;
                    }
                    for (int j = 0; j < hq; j++) memset(&mask[(q + j) * pn + p], 0, (size_t)wp * sizeof(uint16_t));
                    // Into the box's own coordinates. Which of x0/y0/z0
                    // applies depends on which axis the face looks
                    // along: `slice` runs along that axis, `p` and `q`
                    // across the other two.
                    int const sw = slice + (dir.axis == 0 ? g->x0 : dir.axis == 1 ? g->y0 : g->z0);
                    int const pw = p + (dir.axis == 0 ? g->z0 : g->x0);
                    int const qw = q0 + q + (dir.axis == 1 ? g->z0 : g->y0);
                    mesh_set_light(m, (uint8_t)(v >> 8));
                    emit(m, dir, sw, pw, qw, wp, hq, step, (uint8_t)((v & 0xFFu) - 1));
                    p += wp;
                }
            }
        }
    }
    cm_free(mask);
    // The plants (fancy meshes only), torches and signs, one by one (whole
    // blocks only: a coarse grid has neither).
    if (g->step != 1) return;
    for (int z = 0; z < d; z++) {
        for (int x = 0; x < w; x++) {
            for (int y = ylo; y <= yhi; y++) {
                uint8_t const      b = CELL(x, y, z);
                block_kind_t const k = block_kind(b);
                int const          X = x + g->x0, Y = y + g->y0, Z = z + g->z0;
                if (k == K_PLANT || k == K_TORCH || k == K_SIGN) mesh_set_light(m, LIGHT(x, y, z));  // lit by its own cell
                if (k == K_SIGN) emit_sign(m, X, Y, Z);
                if (k == K_PLANT && mode == VOX_MESH_FANCY) emit_plant(m, X, Y, Z, (uint8_t)voxel_face_mat(b, VF_SIDE));
                if (k == K_TORCH) {
                    // Upright in the middle of the cell, or shifted to
                    // one wall and lifted, which is what a torch on a
                    // wall looks like without the mesher having to
                    // tilt anything. A greedy voxel mesher emits
                    // axis-aligned boxes; a rotated stick would be a
                    // second kind of geometry for one block.
                    uint8_t const how = g->data != NULL ? g->data[CIDX(x, y, z)] : TORCH_FLOOR;
                    float const   r   = 1.0f / 16.0f;
                    float         cx = (float)X + 0.5f, cz = (float)Z + 0.5f, base = (float)Y;
                    if (how != TORCH_FLOOR) {
                        base += 0.2f;  // brackets hold a torch above the floor
                        switch (how) {
                            case TORCH_WALL_NX: cx -= 0.30f; break;
                            case TORCH_WALL_PX: cx += 0.30f; break;
                            case TORCH_WALL_NZ: cz -= 0.30f; break;
                            case TORCH_WALL_PZ: cz += 0.30f; break;
                            default: break;
                        }
                    }
                    emit_box(m, v3(cx - r, base, cz - r), v3(cx + r, base + 0.625f, cz + r), VM_TORCH);
                }
            }
        }
    }
    mesh_set_light(m, MESH_LIGHT_FULL);
#undef LIGHT
#undef CELL
#undef CIDX
}
