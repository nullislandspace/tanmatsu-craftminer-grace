#pragma once
// =====================================================================
//  CraftMiner  --  the background chunk task (core 1)
// ---------------------------------------------------------------------
//  Core 0 runs the game and draws it; core 1 does the work that would
//  otherwise land in a frame: generating terrain, meshing it, and the
//  SD card. Measured, those are 56 ms, some tens of ms, and 6.6/21.5 ms
//  a chunk (F-23, F-26) -- none of which can happen between two frames.
//
//  THE OWNERSHIP CONTRACT, because there are no locks on this path:
//
//    id[]/st[] while CS_LOADING   the worker's. Main treats the chunk
//                                 as absent, so nothing reads them.
//    id[]/st[] while CS_READY     MAIN's. The worker only reads them,
//                                 to mesh or to save.
//    the meshes                   MAIN's. The worker builds a NEW mesh
//                                 in its own storage and hands the
//                                 whole struct over; main swaps the
//                                 pointer between frames. The renderer
//                                 can never see a half-built mesh
//                                 because the renderer IS main.
//    cstate                       main's. The worker reports what it
//                                 finished; main decides what that
//                                 means.
//
//  A mesh result carries the `edit_seq` its chunk had when the job was
//  queued. If the chunk has been edited since, the mesh is stale and is
//  dropped rather than shown.
//
//  SYNCHRONOUS MODE runs every job inline on the calling task. The
//  `shots` test needs it (a frame must be a pure function of the show
//  clock), and so does entering a world, where the 3 x 3 around the
//  player has to exist before the first tick (D-15, D-26).
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include "world/chunk.h"

// Start the task. `seed` is the world's; the worker generates any chunk
// the store has no saved copy of. Call after chunk_store_init() and
// after a world is open.
bool chunk_worker_start(uint32_t seed);
void chunk_worker_stop(void);

// The seed the generator uses from now on. Changing worlds without
// restarting the task: drain, clear the store, then set this.
void chunk_worker_set_seed(uint32_t seed);

// Run everything inline instead of on the task. Safe to change between
// frames; a mode change waits for the queue to drain.
void chunk_worker_set_synchronous(bool on);
bool chunk_worker_synchronous(void);

// Ask for a chunk to become resident. Nearest-first ordering is the
// caller's: it queues in the order it wants them. Returns false if the
// queue is full, in which case the caller simply asks again next frame.
bool chunk_worker_request_load(int32_t cx, int32_t cz);

// Ask for a level of detail of one vertical section to be (re)built
// (chunk.h, CH_SECT). The chunk must be resident.
bool chunk_worker_request_mesh(int32_t cx, int32_t cz, int lod, int sect);

// Ask for a chunk to be written to the card. Used on eviction and at an
// explicit save.
bool chunk_worker_request_save(int32_t cx, int32_t cz);

// Apply what the worker has finished, up to a budget so a burst of
// completions cannot blow a frame. Call once a frame from the main
// task. Returns how many results were applied.
int chunk_worker_collect(int max_results);

// Nothing queued and nothing in flight.
bool chunk_worker_idle(void);

// For the boot log and the HUD's "Generating..." state.
void chunk_worker_stats(int* queued, int* loaded_total, int* meshed_total);

// Where the streaming is getting stuck, if it is.
//
// A world that lags behind the camera looks the same on screen whatever
// the cause, and there are three: the worker cannot keep up (jobs
// queued, `in_flight` near `capacity`, `refused` climbing), the MAIN
// task is not taking delivery fast enough (`asked` running away from
// `applied`), or nothing was asked for. Totals since boot; the caller
// takes differences.
typedef struct {
    int in_flight;  // jobs submitted and not yet collected
    int capacity;   // ... out of this many
    int asked;      // jobs successfully queued
    int applied;    // results taken delivery of
    int refused;    // jobs the queue had no room for
    int loaded;
    int meshed;
    int saved;
} chunk_worker_flow_t;

void chunk_worker_flow(chunk_worker_flow_t* f);
