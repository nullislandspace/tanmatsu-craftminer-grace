// =====================================================================
//  CraftMiner  --  chunks and the resident world (see chunk.h)
// =====================================================================

#include "world/chunk.h"
#include <string.h>
#include "common/psram.h"

static chunk_t  s_slots[CH_SLOT_COUNT];
static uint8_t* s_slab;  // one allocation for every plane of every slot
static size_t   s_slab_bytes;
static mesh_t*  s_meshes;  // CH_SLOT_COUNT x CH_MESH_N, likewise

// Two planes per slot, so a slot's bytes are contiguous and the whole
// resident set is one allocation. Nothing here allocates again.
#define SLOT_BYTES ((size_t)CH_CELLS * 2u)

// Free every mesh a slot holds. An empty section never allocated, so
// most of these are no-ops.
static void free_slot_meshes(chunk_t* c) {
    if (c->lod == NULL) return;
    for (int i = 0; i < CH_MESH_N; i++) mesh_free(&c->lod[i]);
    c->lod_built    = 0;
    c->lod_stale    = 0;
    c->lod_inflight = 0;
}

bool chunk_store_init(void) {
    if (s_slab != NULL) return true;

    s_slab_bytes = SLOT_BYTES * (size_t)CH_SLOT_COUNT;
    s_slab       = cm_calloc(s_slab_bytes, 1);
    // The mesh headers go to PSRAM too. They are only headers -- the
    // vertices and triangles each mesh_t points at are allocated by
    // mesh.c as they are built -- but 256 slots x 12 of them is far too
    // much to carry in a static array, which is where chunk_t lives.
    s_meshes     = cm_calloc((size_t)CH_SLOT_COUNT * CH_MESH_N, sizeof(mesh_t));
    if (s_slab == NULL || s_meshes == NULL) {
        cm_free(s_slab);
        cm_free(s_meshes);
        s_slab       = NULL;
        s_meshes     = NULL;
        s_slab_bytes = 0;
        return false;
    }

    memset(s_slots, 0, sizeof(s_slots));
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        uint8_t* base     = s_slab + (size_t)i * SLOT_BYTES;
        s_slots[i].id     = base;
        s_slots[i].st     = base + CH_CELLS;
        s_slots[i].lod    = s_meshes + (size_t)i * CH_MESH_N;
        s_slots[i].cstate = CS_FREE;
        for (int m = 0; m < CH_MESH_N; m++) mesh_init(&s_slots[i].lod[m]);
    }
    return true;
}

void chunk_store_shutdown(void) {
    for (int i = 0; i < CH_SLOT_COUNT; i++) free_slot_meshes(&s_slots[i]);
    cm_free(s_slab);
    cm_free(s_meshes);
    s_slab       = NULL;
    s_meshes     = NULL;
    s_slab_bytes = 0;
    memset(s_slots, 0, sizeof(s_slots));
}

size_t chunk_store_mesh_bytes(int* meshes, int* chunks) {
    size_t bytes = 0;
    int    m_n = 0, c_n = 0;
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t const* c = &s_slots[i];
        if (c->cstate == CS_FREE || c->lod == NULL) continue;
        c_n++;
        for (int m = 0; m < CH_MESH_N; m++) {
            mesh_t const* mesh = &c->lod[m];
            if (mesh->vcap == 0 && mesh->tcap == 0) continue;
            m_n++;
            // Capacity, not count: this is what is HELD, which is the
            // question. mesh.c grows these and never shrinks them.
            bytes += (size_t)mesh->vcap * sizeof(vec3_t);
            bytes += (size_t)mesh->tcap * sizeof(mesh_tri_t);
            bytes += (size_t)mesh->pcap * sizeof(mesh_part_t);
        }
    }
    if (meshes != NULL) *meshes = m_n;
    if (chunks != NULL) *chunks = c_n;
    return bytes;
}

void chunk_store_clear(void) {
    for (int i = 0; i < CH_SLOT_COUNT; i++) {
        chunk_t* c = &s_slots[i];
        free_slot_meshes(c);
        c->cstate = CS_FREE;
        c->flags  = 0;
        c->edit_seq++;  // so any result still in flight is seen as stale
    }
}

size_t chunk_store_bytes(void) {
    return s_slab_bytes + (s_slab_bytes != 0 ? (size_t)CH_SLOT_COUNT * CH_MESH_N * sizeof(mesh_t) : 0);
}

chunk_t* chunk_slot_at(int index) {
    return (index >= 0 && index < CH_SLOT_COUNT) ? &s_slots[index] : NULL;
}

chunk_t* chunk_find(int32_t cx, int32_t cz) {
    chunk_t* c = &s_slots[chunk_slot(cx, cz)];
    // The identity check is what makes the ring safe: a slot taken over
    // by a different chunk reads as absent rather than as the wrong
    // terrain.
    if ((c->cstate == CS_READY || c->cstate == CS_SAVING) && c->cx == cx && c->cz == cz) return c;
    return NULL;
}

chunk_t* chunk_slot_claimed(int32_t cx, int32_t cz) {
    chunk_t* c = &s_slots[chunk_slot(cx, cz)];
    return (c->cstate != CS_FREE && c->cx == cx && c->cz == cz) ? c : NULL;
}

chunk_t* chunk_claim(int32_t cx, int32_t cz) {
    if (s_slab == NULL) return NULL;  // the store was never started, or is shut down
    chunk_t* c = &s_slots[chunk_slot(cx, cz)];

    // Refuse rather than block: the caller asks again next frame. The
    // residency hysteresis (evict two chunks further out than we load)
    // is what stops this from happening in practice.
    if (c->cstate == CS_LOADING || c->cstate == CS_SAVING) return NULL;
    if (c->cstate == CS_READY && (c->flags & CF_EDITED) != 0) return NULL;

    free_slot_meshes(c);
    c->lod_stale = CH_MESH_ALL;
    memset(c->id, BLK_AIR, CH_CELLS);
    memset(c->st, 0, CH_CELLS);
    memset(c->top, 0, sizeof(c->top));

    c->cx     = cx;
    c->cz     = cz;
    c->cstate = CS_LOADING;
    c->flags  = 0;
    c->bottom = 0;
    c->edit_seq++;
    return c;
}

// --- Summaries --------------------------------------------------------

void chunk_resummarise(chunk_t* c) {
    if (c == NULL) return;
    int lowest  = CH_H;
    int tallest = 0;
    for (int z = 0; z < CH_D; z++) {
        for (int x = 0; x < CH_W; x++) {
            uint8_t const* col = &c->id[CH_IDX(x, 0, z)];
            int            t   = 0;
            for (int y = CH_H - 1; y >= 0; y--) {
                if (col[y] != BLK_AIR) {
                    t = y + 1;
                    break;
                }
            }
            c->top[z * CH_W + x] = (uint8_t)t;
            if (t > tallest) tallest = t;

            // The lowest y that can show a face: one below the first
            // cell that is not a full cube, walking up from bedrock.
            // Everything under it is buried stone the mesher skips.
            int b = 0;
            while (b < CH_H && block_kind(col[b]) == K_CUBE) b++;
            if (b > 0) b--;
            if (b < lowest) lowest = b;
        }
    }
    c->bottom  = (uint8_t)(lowest < CH_H ? lowest : 0);
    c->top_max = (uint8_t)tallest;
}

// --- Reading and writing ----------------------------------------------

uint8_t world_block(int32_t x, int32_t y, int32_t z) {
    if (y >= CH_H) return BLK_AIR;
    if (y < 0) return BLK_BARRIER;
    chunk_t const* c = chunk_find(chunk_of(x), chunk_of(z));
    if (c == NULL) return BLK_BARRIER;  // D-14: the edge of the world is a wall
    return c->id[CH_IDX(chunk_off(x), y, chunk_off(z))];
}

uint8_t world_state(int32_t x, int32_t y, int32_t z) {
    if (y < 0 || y >= CH_H) return 0;
    chunk_t const* c = chunk_find(chunk_of(x), chunk_of(z));
    if (c == NULL) return 0;
    return c->st[CH_IDX(chunk_off(x), y, chunk_off(z))];
}

// Mark the meshes a change at height `y` invalidates: every level (a
// block can be visible at any of them) of the section it sits in -- and
// of the section next door when it sits on the boundary, because the
// face between the two belongs to whichever section owns the cell, and
// the other side's mesh reads it as border.
static void mark_stale(chunk_t* c, int y) {
    if (c == NULL) return;
    int const s = ch_sect_of(y);
    for (int l = 0; l < LOD_COUNT; l++) {
        c->lod_stale |= CH_MESH_BIT(l, s);
        if (y % CH_SECT == 0 && s > 0) c->lod_stale |= CH_MESH_BIT(l, s - 1);
        if (y % CH_SECT == CH_SECT - 1 && s < CH_SECT_N - 1) c->lod_stale |= CH_MESH_BIT(l, s + 1);
    }
}

void world_set(int32_t x, int32_t y, int32_t z, uint8_t block, uint8_t state) {
    if (y < 0 || y >= CH_H) return;
    int32_t const cx = chunk_of(x), cz = chunk_of(z);
    chunk_t*      c = chunk_find(cx, cz);
    if (c == NULL) return;

    int const    lx = chunk_off(x), lz = chunk_off(z);
    size_t const i = CH_IDX(lx, y, lz);
    if (c->id[i] == block && c->st[i] == state) return;

    c->id[i]  = block;
    c->st[i]  = state;
    c->flags |= CF_EDITED;
    c->edit_seq++;
    mark_stale(c, (int)y);

    // The column summary, kept incrementally.
    uint8_t* t = &c->top[lz * CH_W + lx];
    if (block != BLK_AIR) {
        if (y + 1 > *t) *t = (uint8_t)(y + 1);
        if (y + 1 > c->top_max) c->top_max = (uint8_t)(y + 1);
    } else if (*t == y + 1) {
        uint8_t const* col = &c->id[CH_IDX(lx, 0, lz)];
        int            n   = 0;
        for (int yy = y - 1; yy >= 0; yy--) {
            if (col[yy] != BLK_AIR) {
                n = yy + 1;
                break;
            }
        }
        *t = (uint8_t)n;
    }

    // A cell on a border shows a face to the chunk next door, and that
    // face lives in the NEIGHBOUR's mesh. Without this the two would
    // disagree and a seam would open.
    if (lx == 0) mark_stale(chunk_find(cx - 1, cz), (int)y);
    if (lx == CH_W - 1) mark_stale(chunk_find(cx + 1, cz), (int)y);
    if (lz == 0) mark_stale(chunk_find(cx, cz - 1), (int)y);
    if (lz == CH_D - 1) mark_stale(chunk_find(cx, cz + 1), (int)y);
}

int world_ground(int32_t x, int32_t z) {
    chunk_t const* c = chunk_find(chunk_of(x), chunk_of(z));
    if (c == NULL) return 0;
    uint8_t const* col = &c->id[CH_IDX(chunk_off(x), 0, chunk_off(z))];
    for (int y = CH_H - 1; y >= 0; y--) {
        if (block_solid(col[y])) return y + 1;
    }
    return 0;
}
