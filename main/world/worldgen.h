#pragma once
// =====================================================================
//  CraftMiner  --  terrain generation
// ---------------------------------------------------------------------
//  A chunk's terrain is a pure function of (seed, cx, cz). Nothing here
//  reads another chunk, reads the clock, or draws from anything but a
//  seeded hash -- so a world is reproducible from its seed for ever,
//  and the order chunks happen to be generated in cannot change what
//  they contain. That is determinism rule 3
//  (claudeplans/craftminer.md, Part T), and it is what the host check
//  `worldgen: cross-chunk equivalence` exists to defend.
//
//  DECORATIONS SPAN CHUNKS. A tree rooted near a chunk's edge has
//  branches in the next one. Rather than let a chunk read its
//  neighbour (which would make the result depend on load order), every
//  chunk walks the candidate tree positions of the 3 x 3 neighbourhood
//  around it and writes only the cells that land inside itself. Each
//  chunk therefore reaches the same answer alone as it would in company.
//
//  Pure: no engine, no RTOS, no allocation.
// =====================================================================

#include <stdint.h>

#include "world/chunk.h"

// Fill `c`'s id and st planes with the terrain of its own coordinates.
// The chunk must already carry cx / cz (chunk_claim sets them). Marks
// CF_GENERATED and refreshes the column summaries; leaves cstate alone
// (the caller owns the state machine).
//
// `farlands_x` is the world's Far Lands edge (world_meta_t.farlands_x,
// farlands.h): chunks wholly west of it are Far Lands. FARLANDS_NONE for
// a world without them.
void worldgen_chunk(chunk_t* c, uint32_t seed, int32_t farlands_x);

// The surface height at a column: the y of the first air above the
// solid ground, before caves and decorations. Exposed because spawn
// selection and the host checks want it without generating a chunk.
int worldgen_height(int32_t x, int32_t z, uint32_t seed);
