// =====================================================================
//  SynthMiner  --  light (see light.h)
// =====================================================================

#include "world/light.h"

#include <string.h>

#include "common/psram.h"

enum { SKY = 0, BLOCK = 1 };

// A cell waiting in a flood, with the level it had (for the removal
// queue) -- a ring buffer of them, in PSRAM.
typedef struct {
    int32_t x, z;
    int16_t y;
    uint8_t v;
} node_t;

// A torch reaches 14 cells in every direction, so one change touches at
// most a few thousand; a chunk's first sky flood seeds a few thousand
// more. 64 Ki is room for both with plenty over, and costs 768 KiB a
// context (two of them: 1.5 MiB of PSRAM).
#define QCAP (1u << 16)

typedef struct {
    node_t*  n;
    uint32_t head, tail;
} queue_t;

static bool s_on;
static int  s_overflows;

static int const DX[6] = {1, -1, 0, 0, 0, 0};
static int const DY[6] = {0, 0, 1, -1, 0, 0};
static int const DZ[6] = {0, 0, 0, 0, 1, -1};
#define DIR_UP   2
#define DIR_DOWN 3

typedef struct lctx lctx_t;
static void ctx_free(lctx_t* k);
static bool ctx_alloc(lctx_t* k);
static lctx_t* ctx_main(void);
static lctx_t* ctx_local(void);

bool light_init(void) {
    if (s_on) return true;
    if (!ctx_alloc(ctx_main()) || !ctx_alloc(ctx_local())) {
        light_shutdown();
        return false;
    }
    s_on = true;
    return true;
}

void light_shutdown(void) {
    ctx_free(ctx_main());
    ctx_free(ctx_local());
    s_on = false;
}

static void push(queue_t* q, int32_t x, int y, int32_t z, int v) {
    if (q->tail - q->head >= QCAP) {
        // Losing a cell leaves a patch a little darker than it should
        // be until the next change nearby; better than blocking.
        s_overflows++;
        return;
    }
    q->n[q->tail++ & (QCAP - 1)] = (node_t){x, z, (int16_t)y, (uint8_t)v};
}

static bool pop(queue_t* q, node_t* out) {
    if (q->head == q->tail) return false;
    *out = q->n[q->head++ & (QCAP - 1)];
    return true;
}

int light_filter(uint8_t b) {
    if (b == BLK_BARRIER) return LIGHT_MAX;  // unloaded: nothing leaks into it
    block_def_t const* d = block_def(b);
    if ((d->flags & BF_LIQUID) != 0) return 2;
    switch (d->kind) {
        case K_CUBE: return LIGHT_MAX;
        case K_SEE: return (d->flags & BF_SEE_SELF) != 0 ? 1 : 0;  // leaves dim it; glass does not
        default: return 0;                                         // air, plants, torches
    }
}

static int emission(uint8_t b) {
    return block_def(b)->light;
}

// --- Cells ------------------------------------------------------------------
//
// A flood runs in a CONTEXT: its own queues, and what it may see. The
// main task's floods see the whole resident world. The worker's -- a
// chunk's own light, worked out on core 1 while the chunk is still
// loading -- see that one chunk and nothing else: everything outside it
// is a wall, and chunk_find() would not even show it the chunk itself,
// which is CS_LOADING and hidden on purpose. The two never share a
// queue, so they can run at the same time.

struct lctx {
    queue_t        add, rem;
    chunk_t*       only;     // local mode: the one chunk that exists
    chunk_t const* filling;  // main mode: the chunk being joined (see set)
};

static lctx_t s_main, s_local;

static lctx_t* ctx_main(void) {
    return &s_main;
}
static lctx_t* ctx_local(void) {
    return &s_local;
}

static bool ctx_alloc(lctx_t* k) {
    k->add.n    = sm_alloc(QCAP * sizeof(node_t));
    k->rem.n    = sm_alloc(QCAP * sizeof(node_t));
    k->add.head = k->add.tail = k->rem.head = k->rem.tail = 0;
    return k->add.n != NULL && k->rem.n != NULL;
}

static void ctx_free(lctx_t* k) {
    sm_free(k->add.n);
    sm_free(k->rem.n);
    k->add.n = k->rem.n = NULL;
}

static chunk_t* chunk_at(lctx_t const* k, int32_t x, int32_t z) {
    int32_t const cx = chunk_of(x), cz = chunk_of(z);
    if (k->only != NULL) return (k->only->cx == cx && k->only->cz == cz) ? k->only : NULL;
    return chunk_find(cx, cz);
}

static uint8_t block_at(lctx_t const* k, int32_t x, int y, int32_t z) {
    if (y >= CH_H) return BLK_AIR;
    if (y < 0) return BLK_BARRIER;
    chunk_t const* c = chunk_at(k, x, z);
    return c == NULL ? BLK_BARRIER : c->id[CH_IDX(chunk_off(x), y, chunk_off(z))];
}

static int get(lctx_t const* k, int32_t x, int y, int32_t z, int ch) {
    if (y >= CH_H) return ch == SKY ? LIGHT_MAX : 0;
    if (y < 0) return 0;
    chunk_t const* c = chunk_at(k, x, z);
    if (c == NULL) return 0;
    uint8_t const l = c->lt[CH_IDX(chunk_off(x), y, chunk_off(z))];
    return ch == SKY ? light_sky(l) : light_block(l);
}

static void set(lctx_t const* k, int32_t x, int y, int32_t z, int ch, int v) {
    if (y < 0 || y >= CH_H) return;
    chunk_t* c = chunk_at(k, x, z);
    if (c == NULL) return;
    int const lx = chunk_off(x), lz = chunk_off(z);
    uint8_t*  l  = &c->lt[CH_IDX(lx, y, lz)];
    *l           = ch == SKY ? (uint8_t)((*l & 0x0Fu) | (v << 4)) : (uint8_t)((*l & 0xF0u) | v);
    // A local flood's chunk has never been drawn: nothing to tell.
    if (k->only != NULL) return;
    // Every mesh that shows a face lit by this cell has to be rebuilt --
    // except inside a chunk still being joined, whose meshes are all
    // stale already; only its edges concern the neighbours.
    if (c == k->filling && lx != 0 && lx != CH_W - 1 && lz != 0 && lz != CH_D - 1) return;
    world_mark_dirty(x, y, z);
}

uint8_t world_light(int32_t x, int32_t y, int32_t z) {
    if (y >= CH_H) return (uint8_t)(LIGHT_MAX << 4);
    if (y < 0) return 0;
    chunk_t const* c = chunk_find(chunk_of(x), chunk_of(z));
    if (c == NULL || c->lt == NULL) return 0;
    return c->lt[CH_IDX(chunk_off(x), y, chunk_off(z))];
}

// --- The floods ---------------------------------------------------------------

// Spread light outwards from everything in the add queue.
static void flood_add(lctx_t* k, int ch) {
    node_t n;
    while (pop(&k->add, &n)) {
        int const lv = get(k, n.x, n.y, n.z, ch);
        if (lv <= 1) continue;
        for (int d = 0; d < 6; d++) {
            int32_t const x = n.x + DX[d], z = n.z + DZ[d];
            int const     y = n.y + DY[d];
            if (y < 0 || y >= CH_H) continue;
            int const f = light_filter(block_at(k, x, y, z));
            if (f >= LIGHT_MAX) continue;
            int nv = lv - 1 - f;
            // Full sky light falls straight down without fading: the
            // column under a newly opened hole is daylight, not dusk.
            if (ch == SKY && d == DIR_DOWN && lv == LIGHT_MAX && f == 0) nv = LIGHT_MAX;
            if (nv <= 0 || nv <= get(k, x, y, z, ch)) continue;
            set(k, x, y, z, ch, nv);
            push(&k->add, x, y, z, nv);
        }
    }
}

// Take away the light that came through the cells in the removal queue.
// A neighbour dimmer than the cell it was lit from was lit BY it and goes
// dark too; one at least as bright has a source of its own and floods
// back in afterwards.
static void flood_remove(lctx_t* k, int ch) {
    node_t n;
    while (pop(&k->rem, &n)) {
        for (int d = 0; d < 6; d++) {
            int32_t const x = n.x + DX[d], z = n.z + DZ[d];
            int const     y = n.y + DY[d];
            if (y < 0 || y >= CH_H) continue;
            int const nl = get(k, x, y, z, ch);
            if (nl == 0) continue;
            bool const from_it = nl < n.v || (ch == SKY && d == DIR_DOWN && n.v == LIGHT_MAX && nl == LIGHT_MAX);
            if (from_it) {
                set(k, x, y, z, ch, 0);
                push(&k->rem, x, y, z, nl);
            } else {
                push(&k->add, x, y, z, nl);
            }
        }
    }
}

void light_block_changed(int32_t x, int32_t y, int32_t z, uint8_t was, uint8_t now) {
    if (!s_on || y < 0 || y >= CH_H) return;
    if (light_filter(was) == light_filter(now) && emission(was) == emission(now)) return;

    lctx_t* const k = &s_main;
    int const     f = light_filter(now);
    for (int ch = SKY; ch <= BLOCK; ch++) {
        int const old = get(k, x, (int)y, z, ch);
        if (old > 0) {
            set(k, x, (int)y, z, ch, 0);
            push(&k->rem, x, (int)y, z, old);
            flood_remove(k, ch);
        }
        // What the cell gets now: its own glow, and what comes in from
        // its neighbours through whatever it has become.
        int nv = ch == BLOCK ? emission(now) : 0;
        if (f < LIGHT_MAX) {
            for (int d = 0; d < 6; d++) {
                int const nl   = get(k, x + DX[d], (int)y + DY[d], z + DZ[d], ch);
                int       cand = nl - 1 - f;
                if (ch == SKY && d == DIR_UP && nl == LIGHT_MAX && f == 0) cand = LIGHT_MAX;
                if (cand > nv) nv = cand;
            }
        }
        if (nv > 0) {
            set(k, x, (int)y, z, ch, nv);
            push(&k->add, x, (int)y, z, nv);
        }
        flood_add(k, ch);
    }
}

// The lowest cell of a column the sky reaches (0 if it reaches the
// bottom), after the straight-down pass: below it the column is dark.
static int sky_floor(chunk_t const* c, int x, int z) {
    int y = CH_H;
    while (y > 0 && light_sky(c->lt[CH_IDX(x, y - 1, z)]) > 0) y--;
    return y;
}

void light_chunk_local(chunk_t* c) {
    if (c == NULL || c->lt == NULL) return;
    if (!s_on) {
        // No queues: a world without light is a fully lit one, not a
        // black one.
        memset(c->lt, (LIGHT_MAX << 4), CH_CELLS);
        return;
    }
    lctx_t* const k   = &s_local;
    k->only           = c;
    int32_t const wx0 = c->cx * CH_W, wz0 = c->cz * CH_D;

    // Direct sunlight, straight down each column, and the torches.
    for (int z = 0; z < CH_D; z++) {
        for (int x = 0; x < CH_W; x++) {
            int lv = LIGHT_MAX;
            for (int y = CH_H - 1; y >= 0; y--) {
                size_t const  i = CH_IDX(x, y, z);
                uint8_t const b = c->id[i];
                int const     f = light_filter(b);
                lv              = f >= LIGHT_MAX ? 0 : lv - f;
                if (lv < 0) lv = 0;
                c->lt[i] = (uint8_t)((lv << 4) | emission(b));
            }
        }
    }

    // SKY. Sideways spread only happens where a column's daylight reaches
    // lower than its neighbour's: the side of a hill, a cave mouth, a
    // hole. So each column seeds just the cells between its own floor and
    // the highest floor next to it -- a handful, not the whole sky. The
    // neighbouring chunks' columns are light_chunk_join's business.
    uint8_t floor_of[CH_D][CH_W];
    for (int z = 0; z < CH_D; z++)
        for (int x = 0; x < CH_W; x++) floor_of[z][x] = (uint8_t)sky_floor(c, x, z);
    for (int z = 0; z < CH_D; z++) {
        for (int x = 0; x < CH_W; x++) {
            int const own = floor_of[z][x];
            int       top = own;
            if (x > 0 && floor_of[z][x - 1] > top) top = floor_of[z][x - 1];
            if (x < CH_W - 1 && floor_of[z][x + 1] > top) top = floor_of[z][x + 1];
            if (z > 0 && floor_of[z - 1][x] > top) top = floor_of[z - 1][x];
            if (z < CH_D - 1 && floor_of[z + 1][x] > top) top = floor_of[z + 1][x];
            for (int y = own; y < top; y++) {
                int const lv = light_sky(c->lt[CH_IDX(x, y, z)]);
                if (lv > 1) push(&k->add, wx0 + x, y, wz0 + z, lv);
            }
        }
    }
    flood_add(k, SKY);

    // BLOCK: every torch in the chunk.
    for (int z = 0; z < CH_D; z++)
        for (int x = 0; x < CH_W; x++)
            for (int y = 0; y < CH_H; y++)
                if (light_block(c->lt[CH_IDX(x, y, z)]) > 1) push(&k->add, wx0 + x, y, wz0 + z, 0);
    flood_add(k, BLOCK);
    k->only = NULL;
}

void light_chunk_join(chunk_t* c) {
    if (c == NULL || c->lt == NULL || !s_on) return;
    lctx_t* const k   = &s_main;
    k->filling        = c;
    int32_t const wx0 = c->cx * CH_W, wz0 = c->cz * CH_D;

    // Each face the chunk shares with a resident neighbour, both ways:
    // whichever side of a pair could light the other more is a seed.
    for (int ch = SKY; ch <= BLOCK; ch++) {
        for (int side = 0; side < 4; side++) {
            for (int a = 0; a < CH_W; a++) {
                int32_t const nx = side == 0 ? wx0 - 1 : side == 1 ? wx0 + CH_W : wx0 + a;
                int32_t const nz = side == 2 ? wz0 - 1 : side == 3 ? wz0 + CH_D : wz0 + a;
                int32_t const ox = side == 0 ? wx0 : side == 1 ? wx0 + CH_W - 1 : wx0 + a;
                int32_t const oz = side == 2 ? wz0 : side == 3 ? wz0 + CH_D - 1 : wz0 + a;
                if (chunk_find(chunk_of(nx), chunk_of(nz)) == NULL) break;
                for (int y = 0; y < CH_H; y++) {
                    int const nl = get(k, nx, y, nz, ch), ol = get(k, ox, y, oz, ch);
                    if (nl > 1 && nl - 1 - light_filter(block_at(k, ox, y, oz)) > ol) push(&k->add, nx, y, nz, nl);
                    if (ol > 1 && ol - 1 - light_filter(block_at(k, nx, y, nz)) > nl) push(&k->add, ox, y, oz, ol);
                }
            }
        }
        flood_add(k, ch);
    }
    k->filling = NULL;
}

void light_chunk_ready(chunk_t* c) {
    light_chunk_local(c);
    light_chunk_join(c);
}
