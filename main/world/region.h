#pragma once
// =====================================================================
//  CraftMiner  --  region files
// ---------------------------------------------------------------------
//  Chunks are stored 8 x 8 to a file, `r.<rx>.<rz>.cmr`. Per-chunk
//  files would lose twice on FAT and a slow SD card: a directory scan
//  on every open, and a whole cluster wasted per file (32 KiB clusters
//  against a chunk that RLEs to 1-3 KiB). A region amortises a whole
//  residency to a handful of opens and packs the payloads contiguously.
//  8 x 8 rather than Minecraft's 32 x 32 because 1024 chunks a region
//  is far more than this world touches at once, and a small region
//  makes compaction cheap.
//
//  DURABILITY WITHOUT TRUSTING fsync. The directory is stored TWICE,
//  each copy with its own CRC32 and a serial number, and a write
//  updates the STALE copy. So whatever happens, one copy is intact:
//  the reader takes the higher serial whose CRC validates. `fsync` is
//  exported here (F-06), but a FAT driver's behaviour across a power
//  cut is not something to bet a world on, and this costs 512 bytes.
//
//  Write order, and it matters:
//    1. append the payload at end of file, flush
//    2. write the stale directory copy with the new entry, its CRC and
//       serial = current + 1, flush
//    3. update the header's waste counter, flush, close
//  A loss between any two steps leaves the other copy valid and loses
//  at most that one chunk's newest save -- never the region.
//
//  REWRITES APPEND. A chunk that no longer fits its old slot goes on
//  the end and the directory points at the new place; the old bytes
//  become waste. When waste passes half the file the region is
//  compacted -- but only at an explicit save or unload, never mid-frame.
//
//  stdio only: no engine, no RTOS. The host checks build it as-is.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "world/chunk.h"

#define REGION_DIM   8
#define REGION_CHUNKS (REGION_DIM * REGION_DIM)
// "CMR" + the MAJOR version digit. Like level.cmw's, it moves only when
// the layout changes wholesale; a mismatched major is refused, not
// guessed at. REGION_VERSION below is the minor revision within it.
#define REGION_MAGIC   "CMR1"
#define REGION_VERSION 1

// World chunk coordinate -> region coordinate / index within it.
// Shift and mask, so negatives land correctly (chunk -1 is in region -1
// at local 7, not in region 0).
#define REGION_SHIFT 3
static inline int32_t region_of(int32_t c) {
    return c >> REGION_SHIFT;
}
static inline int region_local(int32_t c) {
    return (int)(c & (REGION_DIM - 1));
}

// Build "<dir>/r.<rx>.<rz>.cmr". False if it would not fit.
bool region_path(char* out, size_t cap, char const* dir, int32_t rx, int32_t rz);

// Read one chunk. `c` must already carry cx/cz and have its planes.
// `remap` translates saved block ids to current ones (chunk_codec.h);
// NULL if they are already current.
// Returns:
//    1  read and decoded
//    0  the region or the chunk is simply not there (not an error)
//   -1  the file exists but is unreadable or corrupt
// A corrupt directory falls back to the other copy; a corrupt payload
// reads as 0, so a damaged region loses chunks rather than the world.
int region_read_chunk(char const* dir, chunk_t* c, uint8_t const* remap);

// Write one chunk, creating the region if needed. False on IO failure.
bool region_write_chunk(char const* dir, chunk_t const* c);

// Rewrite a region without its waste. Called at an explicit save or an
// unload when region_should_compact() says so -- never during a frame.
bool region_compact(char const* dir, int32_t rx, int32_t rz);
bool region_should_compact(char const* dir, int32_t rx, int32_t rz);
