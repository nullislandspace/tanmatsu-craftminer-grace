// =====================================================================
//  SynthMiner  --  the background chunk task (see chunk_worker.h)
// =====================================================================

#include "world/chunk_worker.h"
#include "world/light.h"
#include <string.h>
#include "common/psram.h"
#include "world/chunkmesh.h"
#include "world/worldgen.h"
#include "world/worldstore.h"

#ifndef SM_HOST
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static char const TAG[] = "cmworker";

// Below the engine's residents on core 1: the audio mixer sits at
// configMAX_PRIORITIES-2, the PPA pump at -3, the MP3 decoder at -4.
// -6 leaves -5 free for a second worker later without re-tuning
// anything. Chunks are never more urgent than sound.
#define WORKER_PRIO  (configMAX_PRIORITIES - 6)
#define WORKER_CORE  1
#define WORKER_STACK 6144
#define QUEUE_DEPTH  48
#endif

typedef enum {
    JOB_LOAD = 0,
    JOB_MESH,
    JOB_SAVE
} job_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t lod;
    uint8_t seq;   // the chunk's edit_seq when this was queued
    uint8_t sect;  // JOB_MESH only: which vertical section (D-34)
    int32_t cx, cz;
} job_t;

typedef struct {
    uint8_t kind;
    uint8_t lod;
    uint8_t seq;
    uint8_t sect;
    uint8_t ok;
    int32_t cx, cz;
    mesh_t  mesh;  // JOB_MESH only; ownership passes to main on receive
} result_t;

static uint32_t s_seed;
static int32_t  s_farlands_x = FARLANDS_X_DEFAULT;
// Generation time, ordinary chunks [0] and Far Lands [1], since boot.
// Written by the worker, read by the stats log: a torn read costs one
// log line a wrong average, nothing more.
static int64_t  s_gen_us[2];
static int32_t  s_gen_n[2];

void chunk_worker_gen_stats(int* n_ord, int64_t* us_ord, int* n_far, int64_t* us_far) {
    *n_ord  = s_gen_n[0];
    *us_ord = s_gen_us[0];
    *n_far  = s_gen_n[1];
    *us_far = s_gen_us[1];
}
static bool     s_sync = true;  // until the task starts, everything is inline
static bool     s_running;
static uint8_t* s_scratch;  // the worker's own mesher box (F-08)
static int      s_loaded_total, s_meshed_total;
// Why a frame's worth of work did not get done. A streaming world can
// fail in three ways that all look identical on screen -- the world
// lagging behind the camera -- and only these tell them apart: the job
// queue full (the worker is behind), the result queue not drained (MAIN
// is behind), or nothing asked for in the first place.
static int      s_saved_total, s_refused_total, s_applied_total, s_asked_total;

#ifndef SM_HOST
static QueueHandle_t s_jobs;
static QueueHandle_t s_done;
static TaskHandle_t  s_task;
static volatile int  s_in_flight;
#endif

// --- The work itself, wherever it runs --------------------------------

// Bring a chunk into residence: from the card if it is there, generated
// if it is not. Runs on the worker while the chunk is CS_LOADING, so
// nothing else may look at it.
static bool do_load(int32_t cx, int32_t cz) {
    // chunk_slot_claimed, not chunk_find: the chunk is CS_LOADING, which
    // chunk_find hides on purpose so no game code reads a half-filled
    // one. This is the code doing the filling.
    chunk_t* c = chunk_slot_claimed(cx, cz);
    if (c == NULL || c->cstate != CS_LOADING) return false;  // main gave up the slot

    int const r = world_chunk_load(c);
    if (r == 1) {
        c->flags |= CF_GENERATED;
        chunk_resummarise(c);
        light_chunk_local(c);  // its own light, here on core 1 (light.h)
        return true;
    }
    if (r < 0) return false;

    // Not on the card: this is the first time anyone has been here.
#ifndef SM_HOST
    int64_t const t0 = esp_timer_get_time();
#endif
    worldgen_chunk(c, s_seed, s_farlands_x);
#ifndef SM_HOST
    int const kind = farlands_chunk_is(cx, s_farlands_x) ? 1 : 0;
    s_gen_us[kind] += esp_timer_get_time() - t0;
    s_gen_n[kind]++;
#endif
    light_chunk_local(c);
    // Freshly generated and not yet written, so it has to be saved
    // before the slot can be reused.
    c->flags |= CF_EDITED;
    return true;
}

static bool do_mesh(int32_t cx, int32_t cz, int lod, int sect, mesh_t* out) {
    return chunkmesh_build(cx, cz, lod, sect, s_scratch, out);
}

static bool do_save(int32_t cx, int32_t cz) {
    chunk_t const* c = chunk_find(cx, cz);
    if (c == NULL) return false;
    if (!world_chunk_save(c)) return false;
    // Rewrites leave dead bytes behind. Tidying them up is a whole-file
    // rewrite, so it happens here on core 1 and only when the waste has
    // actually built up -- never on the frame path.
    world_region_maintain(cx, cz);
    return true;
}

// Apply a finished job on the MAIN task. Everything that changes a
// chunk's visible state happens here, which is what makes the contract
// hold without locks.
static void apply(result_t* r) {
    chunk_t*   c    = &(*chunk_slot_at(chunk_slot(r->cx, r->cz)));
    bool const mine = c->cx == r->cx && c->cz == r->cz;

    if (r->kind == JOB_LOAD) {
        if (mine && c->cstate == CS_LOADING) {
            c->cstate = r->ok ? CS_READY : CS_FREE;
            if (r->ok) {
                s_loaded_total++;
                c->lod_stale = CH_MESH_ALL;
                c->lod_built = 0;
                // The light it trades with the chunks already here -- its
                // own was worked out on the worker. On the main task,
                // like every write to a resident chunk (light.h).
                light_chunk_join(c);
            }
        }
        return;
    }

    if (r->kind == JOB_MESH) {
        // Stale if the slot moved on, or the chunk was edited after the
        // job was queued. Either way the mesh describes a world that no
        // longer exists, so throw it away rather than show it.
        bool const valid = r->lod < LOD_COUNT && r->sect < CH_SECT_N;
        bool const fresh = mine && c->cstate == CS_READY && c->edit_seq == r->seq && r->ok;
        if (fresh && valid) {
            mesh_t* slot = chunk_mesh(c, r->lod, r->sect);
            mesh_free(slot);
            *slot         = r->mesh;  // the swap: one pointer, between frames
            // Only now is it not stale -- and only if nothing marked it
            // again while the job was in flight, which the edit_seq
            // check above has already ruled out.
            c->lod_stale &= (uint16_t)~CH_MESH_BIT(r->lod, r->sect);
            // Empty is a legitimate answer -- solid rock, open sky --
            // and an empty mesh is not "drawable", it is "nothing to
            // draw". Only real geometry counts as built, or the
            // fallback below would pick an empty mesh over a good one.
            if (slot->tn > 0) c->lod_built |= CH_MESH_BIT(r->lod, r->sect);
            else c->lod_built &= (uint16_t)~CH_MESH_BIT(r->lod, r->sect);
            memset(&r->mesh, 0, sizeof(r->mesh));
            s_meshed_total++;
        }
        if (r->mesh.v != NULL || r->mesh.t != NULL) mesh_free(&r->mesh);
        if (mine && valid) c->lod_inflight &= (uint16_t)~CH_MESH_BIT(r->lod, r->sect);
        return;
    }

    if (r->kind == JOB_SAVE) {
        if (r->ok) s_saved_total++;
        if (mine && r->ok) c->flags &= (uint8_t)~CF_EDITED;
        if (mine && c->cstate == CS_SAVING) c->cstate = CS_READY;
    }
}

// Do one job and produce its result. Runs on the worker, or inline in
// synchronous mode.
static void run_job(job_t const* j, result_t* r) {
    memset(r, 0, sizeof(*r));
    r->kind = j->kind;
    r->lod  = j->lod;
    r->seq  = j->seq;
    r->sect = j->sect;
    r->cx   = j->cx;
    r->cz   = j->cz;

    switch (j->kind) {
        case JOB_LOAD:
            r->ok = do_load(j->cx, j->cz) ? 1 : 0;
            break;
        case JOB_MESH:
            r->ok = do_mesh(j->cx, j->cz, j->lod, j->sect, &r->mesh) ? 1 : 0;
            break;
        case JOB_SAVE:
            r->ok = do_save(j->cx, j->cz) ? 1 : 0;
            break;
        default:
            break;
    }
}

// --- Submitting -------------------------------------------------------

static bool submit(job_t const* j) {
    if (s_scratch == NULL) return false;

    if (s_sync) {
        result_t r;
        run_job(j, &r);
        apply(&r);
        return true;
    }
#ifndef SM_HOST
    if (xQueueSend(s_jobs, j, 0) != pdTRUE) {
        s_refused_total++;
        return false;
    }
    s_in_flight++;
    s_asked_total++;
    return true;
#else
    return false;
#endif
}

bool chunk_worker_request_load(int32_t cx, int32_t cz) {
    chunk_t* c = chunk_claim(cx, cz);
    if (c == NULL) return false;  // the slot is busy; ask again next frame
    job_t const j = {.kind = JOB_LOAD, .cx = cx, .cz = cz};
    if (!submit(&j)) {
        c->cstate = CS_FREE;  // never leave a slot stuck in CS_LOADING
        return false;
    }
    return true;
}

bool chunk_worker_request_mesh(int32_t cx, int32_t cz, int lod, int sect) {
    chunk_t* c = chunk_find(cx, cz);
    if (c == NULL || lod < 0 || lod >= LOD_COUNT || sect < 0 || sect >= CH_SECT_N) return false;
    if ((c->lod_inflight & CH_MESH_BIT(lod, sect)) != 0) return true;  // already asked

    job_t const j = {
        .kind = JOB_MESH, .lod = (uint8_t)lod, .sect = (uint8_t)sect, .seq = c->edit_seq, .cx = cx, .cz = cz};
    if (!submit(&j)) return false;
    if (!s_sync) c->lod_inflight |= CH_MESH_BIT(lod, sect);
    return true;
}

bool chunk_worker_request_save(int32_t cx, int32_t cz) {
    chunk_t* c = chunk_find(cx, cz);
    if (c == NULL) return false;
    job_t const j = {.kind = JOB_SAVE, .seq = c->edit_seq, .cx = cx, .cz = cz};
    if (!s_sync) c->cstate = CS_SAVING;
    if (!submit(&j)) {
        c->cstate = CS_READY;
        return false;
    }
    return true;
}

// --- Collecting -------------------------------------------------------

int chunk_worker_collect(int max_results) {
    if (s_sync) return 0;  // already applied, inline
#ifndef SM_HOST
    int      n = 0;
    result_t r;
    while (n < max_results && xQueueReceive(s_done, &r, 0) == pdTRUE) {
        apply(&r);
        s_in_flight--;
        n++;
    }
    s_applied_total += n;
    return n;
#else
    (void)max_results;
    return 0;
#endif
}

bool chunk_worker_idle(void) {
#ifndef SM_HOST
    if (!s_sync) return s_in_flight == 0;
#endif
    return true;
}

void chunk_worker_stats(int* queued, int* loaded_total, int* meshed_total) {
#ifndef SM_HOST
    if (queued != NULL) *queued = s_sync ? 0 : s_in_flight;
#else
    if (queued != NULL) *queued = 0;
#endif
    if (loaded_total != NULL) *loaded_total = s_loaded_total;
    if (meshed_total != NULL) *meshed_total = s_meshed_total;
}

void chunk_worker_flow(chunk_worker_flow_t* f) {
    if (f == NULL) return;
#ifndef SM_HOST
    f->in_flight = s_sync ? 0 : s_in_flight;
    f->capacity  = QUEUE_DEPTH;
#else
    f->in_flight = 0;
    f->capacity  = 0;
#endif
    f->asked   = s_asked_total;
    f->applied = s_applied_total;
    f->refused = s_refused_total;
    f->loaded  = s_loaded_total;
    f->meshed  = s_meshed_total;
    f->saved   = s_saved_total;
}

// --- Lifecycle --------------------------------------------------------

#ifndef SM_HOST
static void worker_main(void* arg) {
    (void)arg;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        result_t r;
        run_job(&j, &r);
        // Block if main is behind: dropping a result would leak its mesh
        // and leave a chunk permanently in flight.
        if (xQueueSend(s_done, &r, portMAX_DELAY) != pdTRUE) {
            if (r.mesh.v != NULL || r.mesh.t != NULL) mesh_free(&r.mesh);
        }
    }
}
#endif

bool chunk_worker_start(uint32_t seed) {
    s_seed = seed;
    if (s_scratch == NULL) {
        s_scratch = sm_alloc(chunkmesh_scratch_bytes());
        if (s_scratch == NULL) return false;
    }

#ifndef SM_HOST
    if (s_running) return true;
    s_jobs = xQueueCreate(QUEUE_DEPTH, sizeof(job_t));
    s_done = xQueueCreate(QUEUE_DEPTH, sizeof(result_t));
    if (s_jobs == NULL || s_done == NULL) return false;
    if (xTaskCreatePinnedToCore(worker_main, "cmworker", WORKER_STACK, NULL, WORKER_PRIO, &s_task, WORKER_CORE) !=
        pdPASS) {
        return false;
    }
    ESP_LOGI(TAG, "chunk worker on core %d, priority %d, %u B scratch", WORKER_CORE, WORKER_PRIO,
             (unsigned)chunkmesh_scratch_bytes());
    s_sync = false;
#endif
    s_running = true;
    return true;
}

void chunk_worker_stop(void) {
#ifndef SM_HOST
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_jobs != NULL) {
        vQueueDelete(s_jobs);
        s_jobs = NULL;
    }
    if (s_done != NULL) {
        vQueueDelete(s_done);
        s_done = NULL;
    }
    s_in_flight = 0;
#endif
    sm_free(s_scratch);
    s_scratch = NULL;
    s_running = false;
    s_sync    = true;
}

void chunk_worker_set_world(uint32_t seed, int32_t farlands_x) {
    s_seed       = seed;
    s_farlands_x = farlands_x;
}

void chunk_worker_set_synchronous(bool on) {
#ifndef SM_HOST
    if (!on && !s_running) return;  // no task to be asynchronous with
    if (on) {
        // Drain first: a result arriving after the switch would be
        // applied twice, once inline and once on collect.
        while (s_in_flight > 0) chunk_worker_collect(64);
    }
#endif
    s_sync = on;
}

bool chunk_worker_synchronous(void) {
    return s_sync;
}
