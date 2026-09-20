// =====================================================================
//  CraftMiner  --  the background chunk task (see chunk_worker.h)
// =====================================================================

#include "world/chunk_worker.h"

#include <string.h>

#include "common/psram.h"
#include "world/chunkmesh.h"
#include "world/worldgen.h"
#include "world/worldstore.h"

#ifndef CM_HOST
#include "esp_log.h"
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

typedef enum { JOB_LOAD = 0, JOB_MESH, JOB_SAVE } job_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t lod;
    uint8_t seq;  // the chunk's edit_seq when this was queued
    uint8_t pad;
    int32_t cx, cz;
} job_t;

typedef struct {
    uint8_t kind;
    uint8_t lod;
    uint8_t seq;
    uint8_t ok;
    int32_t cx, cz;
    mesh_t  mesh;  // JOB_MESH only; ownership passes to main on receive
} result_t;

static uint32_t s_seed;
static bool     s_sync = true;  // until the task starts, everything is inline
static bool     s_running;
static uint8_t* s_scratch;  // the worker's own mesher box (F-08)
static int      s_loaded_total, s_meshed_total;

#ifndef CM_HOST
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
        return true;
    }
    if (r < 0) return false;

    // Not on the card: this is the first time anyone has been here.
    worldgen_chunk(c, s_seed);
    // Freshly generated and not yet written, so it has to be saved
    // before the slot can be reused.
    c->flags |= CF_EDITED;
    return true;
}

static bool do_mesh(int32_t cx, int32_t cz, int lod, mesh_t* out) {
    return chunkmesh_build(cx, cz, lod, s_scratch, out);
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
    chunk_t* c = &(*chunk_slot_at(chunk_slot(r->cx, r->cz)));
    bool const mine = c->cx == r->cx && c->cz == r->cz;

    if (r->kind == JOB_LOAD) {
        if (mine && c->cstate == CS_LOADING) {
            c->cstate = r->ok ? CS_READY : CS_FREE;
            if (r->ok) {
                s_loaded_total++;
                for (int l = 0; l < LOD_COUNT; l++) c->lod_stale[l] = true;
            }
        }
        return;
    }

    if (r->kind == JOB_MESH) {
        // Stale if the slot moved on, or the chunk was edited after the
        // job was queued. Either way the mesh describes a world that no
        // longer exists, so throw it away rather than show it.
        bool const fresh = mine && c->cstate == CS_READY && c->edit_seq == r->seq && r->ok;
        if (fresh && r->lod < LOD_COUNT) {
            mesh_free(&c->lod[r->lod]);
            c->lod[r->lod]       = r->mesh;  // the swap: one pointer, between frames
            c->lod_stale[r->lod] = false;
            memset(&r->mesh, 0, sizeof(r->mesh));
            s_meshed_total++;
        }
        if (r->mesh.v != NULL || r->mesh.t != NULL) mesh_free(&r->mesh);
        if (mine && r->lod < LOD_COUNT) c->lod_inflight &= (uint8_t)~(1u << r->lod);
        return;
    }

    if (r->kind == JOB_SAVE) {
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
    r->cx   = j->cx;
    r->cz   = j->cz;

    switch (j->kind) {
        case JOB_LOAD: r->ok = do_load(j->cx, j->cz) ? 1 : 0; break;
        case JOB_MESH: r->ok = do_mesh(j->cx, j->cz, j->lod, &r->mesh) ? 1 : 0; break;
        case JOB_SAVE: r->ok = do_save(j->cx, j->cz) ? 1 : 0; break;
        default: break;
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
#ifndef CM_HOST
    if (xQueueSend(s_jobs, j, 0) != pdTRUE) return false;
    s_in_flight++;
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

bool chunk_worker_request_mesh(int32_t cx, int32_t cz, int lod) {
    chunk_t* c = chunk_find(cx, cz);
    if (c == NULL || lod < 0 || lod >= LOD_COUNT) return false;
    if ((c->lod_inflight & (1u << lod)) != 0) return true;  // already asked

    job_t const j = {.kind = JOB_MESH, .lod = (uint8_t)lod, .seq = c->edit_seq, .cx = cx, .cz = cz};
    if (!submit(&j)) return false;
    if (!s_sync) c->lod_inflight |= (uint8_t)(1u << lod);
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
#ifndef CM_HOST
    int      n = 0;
    result_t r;
    while (n < max_results && xQueueReceive(s_done, &r, 0) == pdTRUE) {
        apply(&r);
        s_in_flight--;
        n++;
    }
    return n;
#else
    (void)max_results;
    return 0;
#endif
}

bool chunk_worker_idle(void) {
#ifndef CM_HOST
    if (!s_sync) return s_in_flight == 0;
#endif
    return true;
}

void chunk_worker_stats(int* queued, int* loaded_total, int* meshed_total) {
#ifndef CM_HOST
    if (queued != NULL) *queued = s_sync ? 0 : s_in_flight;
#else
    if (queued != NULL) *queued = 0;
#endif
    if (loaded_total != NULL) *loaded_total = s_loaded_total;
    if (meshed_total != NULL) *meshed_total = s_meshed_total;
}

// --- Lifecycle --------------------------------------------------------

#ifndef CM_HOST
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
        s_scratch = cm_alloc(chunkmesh_scratch_bytes());
        if (s_scratch == NULL) return false;
    }

#ifndef CM_HOST
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
#ifndef CM_HOST
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
    cm_free(s_scratch);
    s_scratch = NULL;
    s_running = false;
    s_sync    = true;
}

void chunk_worker_set_synchronous(bool on) {
#ifndef CM_HOST
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
