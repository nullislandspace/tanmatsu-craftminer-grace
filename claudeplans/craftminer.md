# Implementation plan: CraftMiner

Living document: design, step-by-step status, findings and decisions.
Updated whenever a step starts or finishes, something is measured, or
something is decided. Plan approved by the user on 2026-09-20.

## Context

The repository is the `tanmatsu-template-grace` template plus a skeleton
(`main/main.c`: one block turning in front of the camera). The goal is a
basic-but-extendable Minecraft-like game for the Tanmatsu, on SynthEngine3D,
slug `at.cavac.craftminer`, SD card only (`external_only`).

The decisive find: **the showreel already contains a complete, measured voxel
engine** — `../tanmatsu-showreel-grace/main/craftminer/voxel/` (greedy mesher,
chunk LOD renderer, break/place effects, blocky sky) on top of its mesh, math,
camera and texture-cache layers, with 20 generated 16x16 block textures. That is
the hardest ~1500 lines of this project, already written and already tuned
against device measurements. This plan lifts it and builds a *game* around it.

What the showreel does **not** have, and this project must write: streaming
chunks (its world is a fixed 128x32x128 array), player physics and collision,
DDA block picking, items/inventory/crafting, entities and mobs, menus and key
bindings, audio, and persistence.

**User requirements.** Mining, crafting, farming (wheat, carrots, seeds, tree
seeds), animal husbandry (pigs, cows, chickens, dogs), cooking, fishing. Dogs are
found wild and tamed with steak. Mobs: zombies, skeletons, spiders — **no
creepers, and no mob griefing**. Beds set spawn. Death **keeps the inventory**
and respawns at spawn with half health. Health + hunger + tool durability.
Multiple seeded worlds on the SD card, written **only when needed**. Far Lands
west of x = -100000. Core 1 for background work. Every key remappable in a menu.

**User decisions (D-05..D-08 below).** F1-F6 are hotbar slots, so the engine's
F1-exits is off and leaving goes through a pause menu that saves first. Textures
*and* flat shading, selectable in a graphics menu, default textured at half
resolution. The first playable build is walk / mine / place / save, without mobs
or crafting. Survival is the full version: health, hunger and tool durability.

## About this document

This file is committed with the work. It contains the design (Parts L, W, K, P,
T, H, G, X, F), the step-by-step plan with a status table (Part D), and the
findings and decisions logs (Part E).

Numbering starts at **F-01 / D-01** — this is a different repository from the
showreel, whose logs run to F-39 / D-54. Standing rules carried over from it:

- **Engine, graceloader or launcher problems: stop and ask.** No workarounds.
- Builds run in the **foreground**, never backgrounded with sleep-polling
  (`.claude-memory/feedback_no_background_builds.md`).
- **Commit and push only when the user asks.**
- Tight timeouts; no routine BadgeLink downloads (`--fetch` only when a look has
  to be judged).

---

## Part L: source layout

Three principles, in order:

1. **Pure modules first.** Anything that can compile with a plain host `cc`
   does. That is what makes the hands-free host harness (Part H) possible, and
   it keeps world, physics and crafting logic away from engine and RTOS
   dependencies.
2. **Data tables, not switch statements.** Adding a block, an item, a recipe or
   a mob is a table row plus a PNG — never a code edit in five files.
3. **One directory per concern.**

```
main/
  main.c                  app_main, se_app_config_t, the five callbacks
  app.h                   cm_app_t -- the context threaded through se_run's `user`
  common/
    psram.h               cm_alloc/cm_calloc/cm_free -- the ONLY host/badge seam
    rng.{c,h}             xorshift64*, hash2/hash3, value noise          (pure)
    texcache.{c,h}        LIFTED
  math/
    xform.{c,h}           LIFTED (vec3_t, mat3_t, smoothstep)            (pure)
    mesh.{c,h}            LIFTED + mesh_tri_t.dir + per-direction ranges (pure)
    mesh_render.{c,h}     LIFTED + mesh_submit_world()
    camera.{c,h}          LIFTED
  voxel/
    voxel_mesh.{c,h}      LIFTED; vox_grid_t UNCHANGED, tables not switches (pure)
    voxel_sky.{c,h}       LIFTED
    voxel_fx.{c,h}        LIFTED, retargeted from the edit list to the live world
    backdrop.{c,h}        LIFTED
    horizon.{c,h}         LIFTED
  world/
    blocks.{c,h}          BLOCK REGISTRY                                 (pure)
    worldgen.{c,h}        pure (seed, cx, cz) -> id/state planes         (pure)
    farlands.{c,h}        the far-lands density field and its ramp       (pure)
    chunk.{c,h}           chunk_t, the ring store, world_block/set/state
    chunk_codec.{c,h}     RLE over a chunk's two planes                  (pure)
    region.{c,h}          region file: header, dual directory, append, compaction
    worldstore.{c,h}      world dir, level.cmw, worlds.idx, FatFs enumeration
    chunk_worker.{c,h}    the core-1 task, queues, the ownership contract
    chunk_render.{c,h}    per-chunk LOD cache, frustum cull, submission
  game/
    tick.{c,h}            fixed step, tick_input_t, replay record/play
    physics.{c,h}         swept AABB against voxels                      (pure)
    raycast.{c,h}         DDA block pick                                 (pure)
    player.{c,h}          movement, health, hunger, spawn/respawn
    interact.{c,h}        mine/place/use, durability, drops, TREE FELLING
    entity.{c,h}          entity pool + entity_def_t type table
    mob_*.c animal_*.c    one file per creature
  items/
    items.{c,h}           ITEM REGISTRY                                  (pure)
    inventory.{c,h}       slots, hotbar, stacking, transfer              (pure)
    recipes.{c,h}         RECIPE TABLE + resolver                        (pure)
  ui/
    screens.{c,h}         title / worlds / new / play / pause / settings
    hud.{c,h}             crosshair, hotbar, bars
    keybind_ui.{c,h}      ADAPTED from synthracer
    worldlist_ui.{c,h}    world select / create / delete
    textentry.{c,h}       on-screen name and seed entry
  input/
    controls.{c,h}        se_bindings declaration + NVS (the synthracer pattern)
    input.{c,h}           action/axis abstraction -- the only thing call sites see
    look_source.{c,h}     pluggable look provider (keys now, mouse later)
  settings/
    gfx_settings.{c,h}    textures, render scale, view distance (NVS)
  testkit/                as shipped, wired into CMakeLists
tools/
  worldcheck.c            host test of every pure module
  scenecheck.c            host budget test via synthengine3D/host/se_host_stub.c
  meshcheck.c             LIFTED
  make_textures.py        LIFTED, extended
textures/craftminer/      the block PNGs
```

### The three registries (the extendability contract)

```c
/* main/world/blocks.h */
typedef struct {
    char const* name;        /* "stone" -- the stable id for any future save format */
    uint8_t  kind;           /* K_AIR/K_CUBE/K_SEE/K_PLANT/K_TORCH -- drives voxel_mesh.c */
    uint8_t  mat[3];         /* VM_* for VF_TOP / VF_SIDE / VF_BOTTOM */
    uint16_t hardness;       /* ticks to break bare-handed; 0xFFFF unbreakable */
    uint8_t  tool, tool_level;
    uint16_t drop_item;      /* ITEM_NONE drops nothing */
    uint8_t  drop_min, drop_max;
    uint8_t  flags;          /* BF_SOLID|BF_OPAQUE|BF_FELLABLE|BF_CROP|BF_GRAVITY|BF_REPLACEABLE */
    uint8_t  light;          /* emitted light 0..15 (reserved) */
    uint8_t  growth_max;     /* BF_CROP: highest growth stage */
} block_def_t;
extern block_def_t const BLOCKS[BLK_COUNT];
```

`voxel_mesh.c`'s `kind()` becomes `BLOCKS[b].kind` and `voxel_face_mat()` becomes
`BLOCKS[b].mat[face]`. That is the **whole** change to the donor mesher, and
`vox_grid_t` is untouched (F-07). Adding a block is one row, one 16x16 PNG and
one `metadata.json` line.

`item_def_t` (name, stack_max, place_block, tool/level, durability, food values,
icon-or-NULL) and `recipe_t` (w, h, in[9], out, out_n, shapeless, station) follow
the same shape. `entity_def_t` is a type vtable (`tick`, `submit`, `interact`,
`on_death`) over a fixed `entity_t` pool, so adding a mob is one file plus one
row.

---

## Part W: chunk format and world dimensions

| | Value | Why |
|---|---|---|
| Chunk XZ | **16 x 16** | Keeps `VOX_CHUNK == 16`, so the donor `fill_fine`/`fill_coarse` and the mesher's greedy mask sizing are unchanged. |
| Chunk Y | **64**, one column, no vertical chunking (D-11) | The showreel's 32 is too shallow for mining plus build room plus a bedrock-to-sky Far Lands wall. 64 keeps the **single-column** layout, which is what lets a column stay contiguous for the mesher. Vertical chunking would save memory but break that for no gameplay gain at this scale. |
| Sea level | **24** | ~20 blocks of stone and caves below, ~40 above. |
| Coordinates | x, z `int32_t`; y `0..63` | x = -100000 fits trivially. |

Two parallel 1-byte planes, column-major exactly as the donor mesher expects:

```c
#define CH_W 16
#define CH_H 64
#define CH_D 16
#define CH_CELLS (CH_W*CH_H*CH_D)                          /* 16384 */
#define CH_IDX(x,y,z) ((((size_t)(z)*CH_W + (size_t)(x))*CH_H) + (size_t)(y))
```

| plane | bytes | contents |
|---|---|---|
| `id[CH_CELLS]` | 16 KiB | block id (0 = air) — the mesher's input layout, verbatim |
| `st[CH_CELLS]` | 16 KiB | bit 0 `ST_PLACED`; bits 1-3 growth stage; bits 4-6 variant/facing; bit 7 reserved |

Two planes rather than one interleaved `uint16` plane, because
`voxel_mesh_build()` takes a `uint8_t const* cells`. Interleaving would force a
de-interleave pass on every remesh (D-12).

**The store is a 16x16 ring with an identity check, not a hash** (D-13):

```c
#define RING 16                                    /* 256 slots, radius up to 7 */
#define SLOT(cx,cz) ((((cx) & (RING-1)) * RING) + ((cz) & (RING-1)))
static inline chunk_t* chunk_find(int32_t cx, int32_t cz) {
    chunk_t* c = &s_slots[SLOT(cx,cz)];
    return (c->cstate == CS_READY && c->cx == cx && c->cz == cz) ? c : NULL;
}
```

Two ANDs and a compare. `chunk_find` is the hottest function in the program —
mesher border fetches, collision, DDA all go through it — and a probing hash
would cost several times more on that path. The `cx`/`cz` identity check makes
aliasing safe: a stale occupant simply reads as absent. Residency radius 6, evict
at 8 (hysteresis), so a wanted slot is free before it is wanted.

Everything is allocated **once at boot as a slab**: no runtime allocation in the
worker, no fragmentation, the worst case known at start. 256 x (16 + 16) KiB =
**8 MiB**, plus ~3.2 MiB of chunk meshes.

**Measured on the badge (F-22):** 28796 KiB of PSRAM is free once the engine has
booted, and the slab leaves 20604 KiB. So this is comfortable with about 20 MiB
to spare, and none of the fallbacks (`CH_H` 64->48, `RING` 16->8, a pool of `st`
planes) is needed. Internal SRAM is untouched at 160 KiB free / 62 KiB largest.

**Unloaded chunks read as `BLK_BARRIER`** (D-14): `{K_CUBE, BF_SOLID,
hardness 0xFFFF}`, never meshed (a chunk is meshed only when its eight
neighbours are `CS_READY`). So the player *stops* at the edge of generated
terrain instead of falling through it, and after half a second pressed against
one the HUD says "Generating...". That is the honest answer to outrunning
generation.

---

## Part K: the core-1 chunk worker

```c
#define WORKER_PRIO  (configMAX_PRIORITIES - 6)  /* below mixer -2, PPA -3, MP3 -4 */
#define WORKER_CORE  1
#define WORKER_STACK 6144
```

`-6` leaves `-5` free for a second worker later without re-tuning. The ~22 KiB
mesher scratch grid lives in PSRAM and is **owned by the worker** — the donor
meshes through a *shared static* scratch (`voxel_render.c:121`), which races the
moment a second task meshes (F-08).

Two queues of 32: `chunk_job_t {kind, lod, seq, slot, cx, cz}` in, and
`chunk_result_t {kind, lod, seq, ok, slot, cx, cz, mesh_t mesh}` out. Load jobs
are enqueued by the main thread nearest-first, at most 2 a frame, and only while
the queue has room to spare so a save can always get in.

### The ownership contract (no locks on the hot path)

| Data | Written by | Rule |
|---|---|---|
| `id[]`, `st[]` while `CS_LOADING` | worker | Main treats the chunk as absent (`BLK_BARRIER`). |
| `id[]`, `st[]` while `CS_READY` | **main only** | An edit bumps `edit_seq`. A mesh result whose `seq` differs is discarded and re-queued. A save job snapshots `edit_seq`; if it moved, the chunk stays `CF_EDITED`. |
| `lod[]` meshes | **main only** | The worker builds a fresh `mesh_t` in its own storage and passes the struct **by value**. Main does `mesh_free(&c->lod[l]); c->lod[l] = r.mesh;` — a swap the renderer can never observe half-done, because the renderer *is* main. |
| `cstate` | main | Only main transitions; the worker reports, main applies. |

The worker allocates mesh storage (PSRAM, through `mesh_vert`/`mesh_tri`
growth); main owns and frees it. Main drains the result queue with a budget of
**2 mesh + 2 load results per frame**, so a burst cannot blow a frame.

**Synchronous escape hatch** (D-15): `chunk_store_set_synchronous(true)` runs
every request inline on the caller. Required for the testkit `shots` test and for
the spawn neighbourhood below. Built in from day one — retrofitting it is painful.

### Entering a world (D-25, D-26)

The three ways in are deliberately different, because their costs are:

| | what happens | cost |
|---|---|---|
| **Create** | "Creating world": pre-generate the spawn area and **save it**, behind a progress bar. | ~9.5 s of generation (F-23) plus the write — once, at a moment a player already expects to wait. |
| **Load** | Read the 3x3 around the saved player position out of the region file. | a disk read; to be measured, but expected far under generation's 56 ms a chunk |
| **Respawn** | The same, at the bed or at world spawn. | as Load — both are places that have been visited or were pre-generated, so both are on disk |

Pre-generating at creation is what makes the other two cheap: the spawn area is
never generated twice, and the single large write at creation fits the
"write only when needed" rule better than a drip of writes during play.

In all three cases the sequence is the same:

1. **Physics frozen.** The player is placed but does not fall, and no tick runs.
2. **The 3x3 around the player** is loaded (or, only at creation, generated),
   synchronously. Nine chunks — not the whole view.
3. **Play starts.** The gate is those nine and nothing else, so a large view
   distance never delays the start of the game.
4. **The rest of the view distance streams in** on the worker over the next few
   seconds, nearest first, while the player is already walking. What has not
   arrived yet is fog.

This is the concrete form of determinism rule 4 (Part T): a tick never branches
on load state, because a tick only ever runs with its 3x3 present.

---

## Part N: what saves have to survive

Every format here will grow: more block types, more player attributes,
creatures with more to remember. A save that cannot absorb that is a save that
gets thrown away, so the shape of the answer is decided once, here, rather than
per feature.

### Three tiers of per-block state (D-29)

| tier | what it holds | cost |
|---|---|---|
| **the id alone** | stone, dirt, planks — most of the world | 1 byte |
| **id + 7 bits of data** | a crop's growth stage, which way a stair or rail faces, a redstone wire's power, a lit furnace | already there, and RLEs to nothing because it is zero almost everywhere |
| **a block entity** | a furnace's slots and burn timer, a chest's 27, a sign's text | a record in the chunk's side list |

The state byte is `ST_PLACED` (bit 0, universal — the felling rule reads it) plus
**7 bits whose meaning belongs to the block type**, 0..127. That is far more than
the 4 bits Minecraft managed with for years, and it costs nothing.

Creatures and dropped items are not blocks and live in the chunk's **entity**
list.

### Growth without migrations (D-30)

Anything variable-sized — a block entity, a creature — is stored as **named,
typed, skippable fields** (`common/tags.h`), NBT's model over a byte buffer
because a chunk payload is built in memory on the worker, not through a `FILE*`.

Every load is the same three steps, and this is the whole mechanism:

1. fill the struct with **defaults**
2. walk the fields present, overwrite what is **recognised**
3. **skip** anything else

So a world opened by a newer build picks up new defaults for fields it never
had, and is written back complete the next time its chunk is unloaded — worlds
upgrade themselves, gradually, with no upgrade pass and no version check. A
world opened by an *older* build keeps working too: unknown fields are stepped
over. Adding "is this dog sitting", "is this mob aggressive" or a nametag is one
line in the writer and one case in the reader, and no migration.

The same applies one level up: the chunk payload ends in a list of
**sections** (`u8 id, u32 length, bytes`), and a reader skips ids it does not
know. Block entities and entities arrived that way and whatever comes next can
too.

### Block ids can move (D-31)

The chunk planes store one byte per cell, so a saved id only means something
next to the table current when it was written. `level.cmw` records that table:
every block's **name** against the id it had. On open each name is looked up in
today's registry and a remap is built, which `chunk_decode` applies as it
unpacks. Blocks can then be added anywhere, reordered, or removed and old worlds
still load. A name this build no longer has becomes air, reported rather than
hidden.

Entities and block entities name their **type as a string** in their own record
instead, since there are few enough of them for that to cost nothing — so they
need no palette and cannot be misread after a renumbering.

### Major versions, for the changes tags cannot absorb (D-32)

Tags and palettes handle everything additive. A change that alters a layout
wholesale is what the **major version in each file's magic** is for:
`"CMR" + digit` for regions, `"CMW" + digit` for `level.cmw`. A mismatched
major is **refused, not guessed at**, so a future upgrader has something
definite to act on. It moves only for such a change, never for a new field.

### What persists, and for how long (D-33)

Animals, mobs and dropped items are saved with the chunk they stand in, and come
back when it loads — the point being that the world is *consistent*: a cow left
in a field is in that field tomorrow, and a chest of ore dropped in a cave is
still there.

- **Creatures persist indefinitely**, like blocks. They are not despawned for
  being far away.
- **Dropped items despawn after 10 minutes** — 12000 ticks at 20 Hz. The age
  advances only while the chunk is loaded, so walking away and coming back does
  not cost the player their drops; it is idling next to them that does.
- **Hostile mobs keep spawning** in unlit places, up to a **cap per loaded
  chunk**. Because entities are stored per chunk, that cap is a count of what is
  already there — and it is what bounds the population, given nothing despawns.

## Part P: on-disk format

Root is `graceloader_get_install_basepath()` = `/sd/apps/at.cavac.craftminer`.

```
<base>/worlds/worlds.idx              NBT index (an optimisation, not the truth)
<base>/worlds/<slug>/level.cmw        NBT: seed, player, spawn, time, inventory
<base>/worlds/<slug>/r.<rx>.<rz>.cmr  region: 8x8 chunks
```

**`se_save.h` is not used for worlds** (F-05): it is a numbered-slot framework
(`SE_SAVE_SLOT_COUNT` 3, one directory) and does not fit many named worlds.
`se_nbt.h` is used directly on our own paths, and the peek idea is reimplemented
in `worlds.idx`.

**Region files, 8x8 chunks** (D-16). Per-chunk files lose twice on FAT and a slow
SD card: a directory scan per open, and a wasted cluster per file (32 KiB
clusters against ~2 KiB of RLE'd chunk). 8x8 rather than Minecraft's 32x32
because 1024 chunks a region is far more than this world touches at once, and a
smaller region makes compaction cheap.

```c
/* r.<rx>.<rz>.cmr, little-endian */
struct cmr_header {              /* 0x000, 64 bytes */
    char     magic[4];           /* "CMR1" */
    uint16_t version;
    uint16_t region_dim;         /* 8 */
    int32_t  rx, rz;
    uint16_t chunk_w, chunk_h, chunk_d;  /* a mismatch = refuse to load */
    uint32_t waste;              /* bytes orphaned by rewrites */
    uint32_t dir_serial;
    uint8_t  reserved[28];
};
struct cmr_entry {               /* 8 bytes, index = lcz*8 + lcx */
    uint32_t offset;             /* 0 = absent */
    uint16_t length;
    uint8_t  codec;              /* 0 raw, 1 RLE */
    uint8_t  flags;              /* bit0 has_entities */
};
/* 0x040 directory copy A + crc32; 0x240 copy B + crc32; 0x440 payloads */
```

**Dual directory + serial + CRC32** is the durability mechanism (D-17). `fsync`
*is* exported (F-06 corrects an earlier assumption), but a FAT driver's
power-loss semantics are not something to bet a world on. Write order: append the
payload and flush; write the *stale* directory copy with the new entry, its CRC
and `serial = cur + 1`, and flush; update `waste` and close. A power loss between
any two steps leaves one valid copy, and the reader takes the higher serial whose
CRC validates. Worst case one chunk's last save is lost, never the region.

**Codec: RLE**, `(value, count)` pairs over each plane in memory order. Vertical
runs of stone and air dominate, so a chunk goes 16 KiB -> 1-3 KiB per plane.
Deliberately **not** zlib/miniz: `main/testkit/screenshot.c` documents the trap —
a compressor wants ~130 KiB of state out of a heap that lands in scarce internal
SRAM. RLE needs no state, is thirty lines, and round-trips in a host test.

Rewrites append and add the old length to `waste`; when `waste > filesize/2`,
and only at an explicit save or unload, the region is rewritten to a temp file
and renamed.

**World enumeration, two tier** (D-18): `worlds.idx` answers the world-select
screen in one file open; when it is missing, stale, or an entry fails to resolve,
the list is rebuilt by enumerating `worlds/` with FatFs. `opendir`/`readdir` are
**not exported** (F-06) — copy `dir_open()` from `synthengine3D/src/se_mp3.c:198`
verbatim, including its volume-path probing. Deleting a world uses `f_unlink`
(`remove` and `unlink` are not exported either). A world hand-copied onto the
card therefore still works, which it must.

**Writes happen only on**: chunk eviction, the pause menu's Save, quit to
launcher, and death/respawn. Never per tick.

---

## Part T: determinism and the tick

The simulation runs at a fixed **20 Hz** (`TICK_DT = 0.05f`), decoupled from
rendering, which interpolates. Physics is then stable at any frame rate, a
recorded input stream replays exactly, and the frame becomes a pure function of
`showtime_now()` — the precondition the testkit's `shots` test needs.

```c
s_acc += dt;                              /* already clamped by SE_FRAME_DT_MAX */
int n = 0;
while (s_acc >= TICK_DT && n < TICK_MAX_CATCHUP /*4*/) { tick_once(&in); s_acc -= TICK_DT; n++; }
if (s_acc >= TICK_DT) s_acc = 0.0f;       /* give up; never spiral */
float const alpha = s_acc / TICK_DT;      /* render lerp(prev, cur, alpha) */
```

**Look is split**: sampled at frame rate into float yaw/pitch so the camera is
smooth, and snapshotted quantised into the tick input. The simulation (what the
mine ray hits) uses the snapshot; the camera uses the continuous value. A <=50 ms
crosshair disagreement is imperceptible and buys both smoothness and exact
replay.

```c
typedef struct {            /* 8 bytes; 160 B/s; a 10-minute replay is 96 KiB */
    uint16_t buttons;       /* FWD|BACK|LEFT|RIGHT|JUMP|SNEAK|MINE|USE|... */
    int16_t  yaw_q, pitch_q;/* absolute, 1/65536 turn */
    uint8_t  hotbar;        /* 0..5, 0xFF unchanged */
    uint8_t  flags;
} tick_input_t;
```

**The four determinism rules** (they go in `tick.h`'s header comment — they are
the contract):

1. Nothing in a tick reads wall-clock time or `dt`. The step is the constant.
2. All randomness comes from explicit seeded streams (`world_rng`, `entity.rng`),
   advanced only inside ticks. Never `esp_random()`, never `rand()`.
3. **Worldgen is a pure function of (seed, cx, cz)** and never reads a neighbour
   chunk. Cross-chunk decorations (trees) iterate the 3x3 neighbourhood's
   candidate positions and write only cells inside *this* chunk, so load order
   cannot change content.
4. **A tick runs only when the 3x3 chunks around the player are `CS_READY`**
   (D-19). Otherwise the tick is skipped and the frame still renders. This is
   what async streaming forces: no tick ever branches on load state, so a replay
   is exact however fast the SD card was that day.

In `shots` mode main turns on synchronous chunk loading, fixes the seed, drives
the tick count from `round(showtime_now() / TICK_DT)` and reads inputs from a
canned script compiled into the build. Without rule 4 and the synchronous hatch
the hashes would never be stable.

---

## Part H: the host harness

Everything under `main/world/` (bar `chunk_worker.c` and the FatFs half of
`worldstore.c`), `main/game/{physics,raycast,interact}`, `main/items/*`,
`main/voxel/voxel_mesh.c` and `main/math/*` compiles with a plain `cc`. The one
seam is `main/common/psram.h` (`cm_alloc` -> `malloc` or `heap_caps_malloc`).
No module in the pure set may include `esp_heap_caps.h`, `esp_log.h` or FreeRTOS
headers — enforced by a grep rule in `make check`.

`make worldcheck` (`tools/worldcheck.c`):

| Check | What it proves |
|---|---|
| Worldgen determinism | The same chunk twice is byte-identical; a 3x3 span generated chunk-by-chunk equals the same span generated whole (rule 3). Includes coordinates near x = -100000. |
| Far Lands shape | Part X. |
| Mesher validity | The donor `meshcheck` cases (closed, consistently wound, outward, volume = cells, area = exposed faces) plus: no face between two solids; border cells emit nothing outside the chunk AABB (a streaming-only bug the showreel could not have); every triangle's `dir` matches its computed normal and the direction groups are contiguous. |
| AABB sweeps | 10000 random (start, velocity) pairs: never ends inside a solid; zero velocity is a fixed point; no tunnelling to 40 m/s; a 1.0-block step-up succeeds and 1.5 does not; a 0.6-wide body fits a 1-wide gap and not a 0.5 one. |
| DDA picking | 10000 random rays against a brute-force march at 1/64 block: same block, same face normal, reach honoured, normal points at the cell a placement would fill. |
| Crafting | Every recipe resolves at every legal grid offset; no two collide; every output and every `drop_item` exists. |
| Save round trip | Chunk -> RLE -> chunk byte-identical; a 64-chunk region written and fully read back; a corrupted directory copy A falls back to B and reports the chunk *absent*, never corrupt; compaction preserves every chunk. |
| Tree felling | Part F. |
| Registry invariants | `BLK_COUNT < 256`; every block has materials; every item has a name and `stack_max >= 1`; inventory index maths at every boundary. |

`make scenecheck` (`tools/scenecheck.c` on `synthengine3D/host/se_host_stub.c`)
is the **budget** test, so a cap overflow is never discovered on the badge. It
drives a canned camera path through a generated world with the real
`chunk_render.c` + `mesh_render.c` + `voxel_mesh.c` and asserts: triangle counts
under the caps at each view distance (fail over, warn at 90%); nothing crosses
`RENDER_NEAR_CLIP_Z`; `outside_view()` really culls; the per-chunk AABB really
bounds its geometry.

The caps are grepped straight out of `CMakeLists.txt`, as the showreel does
(`Makefile:327`), so the checker can never test against different caps than the
app builds with:

```make
ENGINE_DEFS := $(shell sed -n 's/^add_compile_definitions(\(SE_[A-Z_]*=[0-9]*\))/-D\1/p' CMakeLists.txt)
check: worldcheck scenecheck meshcheck hostpurity
```

`make build` runs `make check` first, so a broken invariant stops the build.

---

## Part G: render budget

Measured baseline (quarter resolution, textured walk): rasterize 40 ms + submit
14-16 ms + prepare 4 ms + PPA wait 6.5 ms = about 64 ms, 15.5 fps. Target 33-50 ms.

### G1 — remove the submit overhead (the 14-16 ms)

**(a) `mesh_submit_world()`.** Chunk meshes are already world-space and their
xform is identity (F-02), yet `mesh_submit` runs a full `xform_apply` per vertex
through a PSRAM scratch array. A variant reading `m->v[]` directly removes tens
of thousands of transforms and hundreds of KiB of PSRAM write-then-read per frame.

**(b) Direction-grouped back-face culling.** `tri_faces_point()` is a cross
product, a centroid and a dot per triangle; greedy-mesher faces are axis-aligned,
so the answer is a sign test. `mesh_tri_t` has a **free pad byte** (F-01), so a
`uint8_t dir` costs nothing, the mesher's `emit_f()` already knows it, and
triangles sort into six contiguous runs at build time. Submission becomes six
loops with one compare per triangle.

Expected: submit **14-16 ms -> 3-5 ms**. Both changes are build-time and
host-testable.

### G2 — the LOD ladder, driven by the graphics menu

| View distance | fancy | tex | coarse | draw | fog0/fog1 | chunks |
|---|---|---|---|---|---|---|
| Near | 8 | 14 | 24 | 40 | 18 / 44 | ~25 |
| **Medium (default)** | 12 | 20 | 32 | 56 | 24 / 60 | ~49 |
| Far | 12 | 20 | 40 | 72 | 30 / 78 | ~81 |

`vox_view_t` is reused verbatim. Mesh residency follows the ladder by rule rather
than by LRU, so the ~3.2 MB is predictable. `outside_view()` gains the per-chunk
`bottom` as well as `top`: a 64-tall AABB culls badly along the horizon, a
terrain-hugging one does not.

### G3 — settings and engine flags

- **Textures on/off**: off swaps in the flat `mean_argb` material table, which
  `voxel_render.c` already builds for its fog path. 3-4x cheaper fill.
- **Render scale**: `scene_set_render_scale(2)` default; Full offered, labelled slow.
- `scene_set_options({.frustum_cull = true, .depth_order = true})` always.
- **Stay on `SE_RENDER_ZBUFFER`.** Raycast is 2.5-5x slower on voxels.
- `SE_SCENE_TRI_CAP` stays 4096; **`SE_SCENE_TEXTURED_TRI_CAP` -> 2048**
  (~70 KiB PSRAM, no per-frame cost). The showreel peaked at 1785 textured with a
  showreel camera; a player facing a forest will pass 1024. `scenecheck` guards it.
- Use the `scene_prepare()` / `scene_rasterize()` split around the PPA backdrop
  so cull and sort overlap the hardware fills.

### G4 — the projected budget, and the risk

| Phase | Target (quarter, textured, Medium) |
|---|---|
| backdrop (PPA) | 0 CPU, overlapped |
| submit (G1) | 3-5 ms |
| prepare | 3-4 ms |
| rasterize | 25-30 ms |
| PPA wait + upscale | 6.5 ms |
| tick + entities | 1-2 ms |
| HUD | 1-2 ms |
| **total** | **40-50 ms, 20-25 fps** |

**Risk, with evidence:** 40 ms rasterize was measured on a showreel camera; a
player under a canopy measured 46 ms. G1 buys back ~11 ms of a ~64 ms frame,
landing at 20-25 fps, not 30. If the device reports under 18 fps the levers, in
order, are: `tex_dist` 20 -> 16; drop FANCY leaves (see-through canopies were 58%
of near triangles, showreel F-36); default textures off. **Do not reach for the
raycaster.** Measure at step 2.4, not at the end.

### G5 — what step 2.4 actually measured (F-31..F-34)

**16.0 fps**, quarter resolution, near view distance, textures on. The
projection above was optimistic, and the phase split says why:

| phase | ms | whose |
|---|---:|---|
| rasterize | 37-46 | **engine** — software fill at ~5 Mpx/s |
| submit | 15-16 | **game** — ~4.4 us per triangle, and far too many are sent |
| PPA wait + upscale | 6.0 | engine, unavoidable at quarter resolution |
| engine prepare | 4.5 | game — cull + sort, proportional to what is submitted |
| sky fill | 0.9 | game |
| residual | 1.3 | — |

The split matters because it says where work pays:

- **About 21 ms is the game's**, and most of it is waste: 68% of every chunk's
  mesh is cave walls (F-33), so ~7000 triangles are submitted to draw ~1000.
  Vertical render sections (D-34) are the fix and they touch no engine code.
  **Read F-35 with this**: sectioning is done and measured, and it is worth
  about 8%, not the 2.6x F-33 implied. The 68% was measured from one chunk, and
  that chunk was sea floor.
- **About 43 ms is the engine's**, and it is the documented cost of a software
  rasteriser, not a defect. The game chooses the pixel count -- view distance,
  render scale, textured against flat -- but not the rate per pixel.

**The engine's rasteriser is plain scalar C** (checked: no PIE/SIMD anywhere in
`src/se_scene.c`; the only SIMD in the engine is minimp3's x86/ARM paths, which
are explicitly disabled). The ESP32-P4 has a 128-bit SIMD unit suited to exactly
what a span loop does. So 5 Mpx/s is this implementation's speed, **not a
hardware ceiling** -- but how much headroom there is has not been profiled and
should not be guessed at.

Two engine-side routes exist if the game-side work is not enough, and both are
**stop and ask** under the standing rule, because the engine is shared:

- `se_renderer_register` -- the engine offers a documented seam for a custom
  rasteriser with no change to any `scene_tri` call site. A voxel-specialised
  one could exploit every face being axis-aligned.
- SIMD the existing span loops, which would help the engine's other users too.

Order of work: game side first. It is the larger single win, it carries no
engine risk, and it makes any later engine measurement cleaner by removing
geometry that should never have been submitted.

---

## Part X: the Far Lands

Beta 1.7.3's Far Lands came from noise coordinates exceeding the generator's
precision: the vertical density gradient saturated and the result was a
full-height wall riddled with horizontal tunnels, with stretched floating shelves
above. Here it is deliberate, **west only**, and ramped.

```c
#define FARLANDS_X    (-100000)
#define FARLANDS_RAMP   4096      /* about five minutes of walking */

static inline float farlands_mix(int32_t x) {         /* x ONLY -- never z, never +x */
    if (x > FARLANDS_X) return 0.0f;
    return smoothstep(0.0f, 1.0f, (float)(FARLANDS_X - x) / (float)FARLANDS_RAMP);
}
```

Generation is a **density field**, so the blend is a blend of densities, not a
switch — which is what makes it an approach rather than a cliff edge:

- **the wall**: a solid bias held from bedrock to sky, with the height falloff
  dropped entirely;
- **"precision loss"**: y advances only every `FL_YQ` (12) blocks, so the same 2D
  pattern repeats vertically — the horizontal tunnels;
- **the smear**: features elongated about 8:1 along x — the stretched look;
- **above the wall top**: thin shelves at quantised heights where a sparse noise
  crosses a high threshold.

`d = normal*(1-m) + far*m`, solid where `d > 0`. Materials are unchanged — stone,
with a dirt/grass skin where a solid cell has air above — so it reads as *the
same world gone wrong*, which is the joke.

**Cost, and the mitigation** (F-09): the far-lands path needs a 3D density
evaluation per cell (16384 a chunk) instead of 256 heightmap columns, about 50x
the work. Mitigate as Minecraft does — evaluate on a **4x4x4 lattice and
trilinearly interpolate** (5x5x17 = 425 samples a chunk) — and use the same
lattice for cave carving in normal terrain, so one path serves both. This is the
biggest worldgen performance unknown; measure it on the device.

**Host tests**: `solid_fraction(x)` at -99000, -100000, -101024, -102048,
-104096, -110000 is non-decreasing and > 0.55 at -104096; at -110000 at least 90%
of columns are solid at both y = 1 and y = 46; a z-scan at fixed (x, y) has >= 4
solid-to-air transitions per 256 blocks; shelves exist above the wall top with
air below; **asymmetry** — the statistic at x = +110000 and z = +/-110000 matches
normal terrain within 5% (the guard against a sign bug far-landsing the whole
world); determinism holds at those coordinates.

---

## Part F: the tree-felling rule

**The bit** is `st` **bit 0, `ST_PLACED`**, set by `world_place_block()` for every
block a player places, never by worldgen. It round-trips through the RLE with the
rest of the state plane, so a felled tree behaves the same after a reload. It
also gives "placed leaves never decay" for free later.

- not `BF_FELLABLE` -> normal single drop;
- `BF_FELLABLE` **with** `ST_PLACED` -> single drop, no fell;
- `BF_FELLABLE` **without** `ST_PLACED` -> fell.

Connectivity, two phases:

1. **Logs.** BFS from the seed, **6-neighbour**, visiting only cells with
   `cy >= y`, block `BLK_LOG`, `ST_PLACED` clear. Same-`y` is allowed, so a 2x2
   trunk or a branch at the break height falls; below-`y` never is. 6-way rather
   than 26-way so a trunk merely touching a neighbour diagonally is not dragged in.
2. **Leaves.** BFS from every log found, **26-neighbour**, carrying a distance
   counter capped at `LEAF_DIST = 5` (Minecraft's decay radius), visiting only
   `BLK_LEAVES` with `ST_PLACED` clear and `cy >= y`. The cap stops a leaf bridge
   between two touching canopies from felling both trees.

`FELL_MAX = 512` cells; hitting it fells what was found and stops — a hard bound
against a grove of touching trunks. The visited set is an open-addressed hash of
1024 packed coordinates, 4 KiB of PSRAM owned by `interact.c` and allocated once.
The BFS goes through `world_block()`, so it spans chunks naturally; an unloaded
chunk stops expansion, which never fires in practice because a tick requires the
3x3 neighbourhood loaded (Part T rule 4) and a tree is at most ~7 blocks wide.
Removal is one batch, marking every touched chunk `CF_EDITED` and its LODs stale
(plus the neighbour where a cell sits on a border). **Drops are merged into
stacks** and spawned as one item entity — sixty item entities would cost more
frame time than the fell.

---

## Part D: step-by-step plan with status tracking

**Status values:** `todo` / `in progress` / `done` / `blocked (why)` /
`skipped (why)`. The Notes column records the result, with links to findings
(F-n) and decisions (D-n).

| # | Step | Status | Notes |
|---|---|---|---|
| **0** | **Scaffolding (host only, no device)** | | |
| 0.1 | This document, with the status table, F-01..F-09 and D-01..D-19 | done | 2026-09-20 |
| 0.2 | Create the layout; lift `mesh`, `xform`, `mesh_render`, `camera`, `texcache`, `backdrop`, `horizon`, `voxel_mesh`, `voxel_sky`, `voxel_fx`, `textures/*.png`, `tools/{make_textures.py,meshcheck.c}`, with provenance in each header | done | 2026-09-20: 22 files carry a provenance line. Textures flattened to `textures/` (one app, not a reel with segments) and `make_textures.py` trimmed to CraftMiner's 20; regenerated **byte-identical** to the showreel's (F-14). `backdrop`/`horizon`/`voxel_sky`/`voxel_fx` are in the tree but not yet in `APP_SOURCES` — they wait for `world/chunk_render.h` in step 2. |
| 0.3 | `common/psram.h` seam; `world/blocks.c` registry; `voxel_mesh.c`'s `kind()`/`voxel_face_mat()` become `BLOCKS[]` lookups (`vox_grid_t` unchanged) | done | 2026-09-20: 17 blocks incl. `BLK_BARRIER`. The leaves-see-leaves / glass-hides-glass rule became the `BF_SEE_SELF` flag instead of a hard-coded id. **All eleven of the showreel's mesher cases pass with its exact triangle counts** (F-15), so the conversion is behaviour-preserving. `voxel_mesh.c`'s greedy mask moved to `cm_calloc` (F-12 closed). |
| 0.4 | `tools/worldcheck.c` skeleton, `make check`, the host-purity grep rule | done | 2026-09-20: `make check` = `hostpurity` + `meshcheck` + `worldcheck`, and `make build` **depends on it**. `worldcheck` has the registry section; the rest arrive with their milestones. `scenecheck` waits for `chunk_render.c` (step 2). |
| 0.5 | CMake: `SE_SCENE_TEXTURED_TRI_CAP=2048`, `SE_BINDINGS_MAX=24`; testkit sources in; `SCREENSHOT_DIR`; `metadata.json` gains the PNGs | done | 2026-09-20: both definitions sit before `add_subdirectory(synthengine3D)` and `make check` greps them back out with sed, so app and checker can never disagree. Testkit compiled in; `SCREENSHOT_DIR="/sd/apps/at.cavac.craftminer/test"`. `make install` uploads the 20 textures to `<app>/textures/`. |
| | **Accept:** `make check` green, `make build` clean, `make verify` passes | **done** | 2026-09-20: all three. `app.so` 25605 text / 43252 data / 15649 bss. |
| **1** | **World data, generation and persistence (host only)** | | |
| 1.1 | `chunk.{c,h}` ring store, `world_block/set/state`, `BLK_BARRIER`; **measure free PSRAM on the badge** and record it | done | 2026-09-20: measured on the badge (F-22). 28.1 MiB PSRAM free after the engine boots; the 8 MiB slab leaves 20.1 MiB. The residency radius of 6 is comfortable and could grow. Chunk generation measured at 56 ms (F-23).
| 1.2 | `worldgen.c`: heightmap, strata, water, ores, caves, trees, plants; cross-chunk decoration by neighbourhood iteration | done | 2026-09-20: two octave stacks (broad coast field + fine hills), soil depth, beaches, two-field cave worms, coal, trees on a jittered 5-block grid, flowers and tall grass. Measured over 327k columns: relief y17-38, **31.3% at or below sea level**, centred at y27 with sea level 24. Host-tested for determinism (including x=-100000 and z=1400) and load-order independence (F-17). |
| 1.3 | `chunk_codec.c` RLE; `region.c` header, dual directory, append, compaction | done | 2026-09-20: a generated chunk packs to **3962 bytes** from 32768 raw (8.3x), with a raw fallback so no input can fail to store. Region files host-tested for round trip, damage, **torn-write recovery** and compaction (F-25), then measured on the card (F-26). `world/vfs_compat.{c,h}` added for the calls graceloader does not export (D-27).
| 1.4 | `worldstore.c`: `level.cmw` schema, `worlds.idx`, the `se_mp3.c` FatFs enumeration | done | 2026-09-20: worlds create / list / open / save / delete, slugs made FAT-safe and unique, player state round-tripped including a position out at x=-100000. Palette written and remap proven (F-29). `common/tags.{c,h}` added for D-30, tested both ways (F-28). Enumeration is a live FatFs directory scan; `worlds.idx` is **not** written yet -- the scan is the truth and the index is only an optimisation, so it waits until a listing is measured as slow.
| | **Accept:** `make worldcheck` passes determinism, cross-chunk equivalence, RLE and region round trips, torn-write recovery, compaction | | |
| **2** | **Streaming render on the device** | | |
| 2.1 | `chunk_worker.c`: task, queues, ownership contract, synchronous mode | done | 2026-09-20: core 1 at `configMAX_PRIORITIES-6`, 48-deep job and result queues, worker-owned 21 KiB mesher scratch (F-08 closed). Host-tested through the synchronous path (F-30). |
| 2.2 | `chunk_render.c`: streamed LOD cache, `outside_view` with `top`/`bottom`, the view table | done | 2026-09-20: ring-at-a-time nearest-first loading, eviction with hysteresis and save-before-drop, three LOD bands, fog-tinted flat palette cached per fog step. |
| 2.3 | G1: `mesh_tri_t.dir`, direction-grouped triangles, `mesh_submit_world()` | done | 2026-09-20: the free pad byte holds the face direction (F-01), so an axis-aligned back-face test is one compare. `meshcheck` proves every greedy face's direction matches its real normal and that plants carry none. Grouping was **skipped** — the per-triangle test is already cheap and submit time turned out to be elsewhere (F-32). |
| 2.4 | A free-flying debug camera over a streamed world | done | 2026-09-20: a circular flight, a pure function of the show clock. 16.0 fps measured (F-31, F-32, F-33) — **that figure is wrong, see F-36**: the camera was pointing at the sky and flying sideways. Corrected, the same build is **12.3 fps**. |
| 2.7 | The world reloading itself, reported from free flight | done | 2026-09-21: three causes (F-41). The far preset did not fit the chunk ring — two chunks per slot, evicting each other forever. A LOD change drew nothing until its mesh arrived. The result budget was still 2 a frame from before sectioning. Two of the three were mine, from D-34. |
| 2.6 | Where the frame time really goes; the engine at -O2 | done | 2026-09-21: the user asked for SIMD and for internal-SRAM textures. **Neither is the answer, and both were measured rather than assumed** (F-37, F-38, F-40). What was: `-O2` and inline rounding, worth **13.4 -> 15.3 fps** (F-39). Spans average **6 pixels**, so the cost is per-span setup, not per-pixel work. |
| 2.5 | Vertical render sections (D-34) and a hand-flown camera | done | 2026-09-21: `vox_grid_t.y0`; 4 sections a chunk, each culled and meshed on its own; meshes moved out of the static `chunk_t` into the PSRAM slab (**−35 KiB bss**). **12.6 → 13.6 fps** (F-35, two runs each). The mesh check proves the seam is exact by meshing a lump whole and in slices and comparing surface area and volume — and fails when the offset or the border is broken. Free flight (WASD / arrows / Space / Shift, T and V toggles) whenever no test is running (D-38). |
| | **Accept host:** `make scenecheck` no cap overflow at any view distance; `make meshcheck` `dir` matches every normal. **Accept device:** `make cycle TEST="perf scene=flyover secs=20"`; **submit must be under 6 ms** | | |
| **3** | **The player** | | |
| 3.1 | `physics.c`: swept AABB (0.6 x 1.8), gravity, jump, step-up | done | 2026-09-21: axis-at-a-time sweep in sub-steps of 0.25 so nothing tunnels at terminal velocity (3 blocks a tick, against a body 1.8 tall). Step height is **a whole block, not Minecraft's 0.6** (D-45). Host-tested: rests exactly on the floor, slides along walls, climbs a step and a staircase, refuses a 2-block wall, fits a 2-high gap and not a 1-high one, stops at the edge of the resident world. |
| 3.2 | `raycast.c`: DDA pick, block + face normal, reach 4.5 | done | 2026-09-21: Amanatides-Woo, reporting the face entered through and the cell in front of it. Host-tested against a **brute-force march over 576 directions**, every one agreeing on hit/miss and on which block. |
| 3.3 | `tick.c`: fixed step, interpolation, replay record/play | done (replay deferred) | 2026-09-21: fixed 20 Hz off the show clock, capped at 5 ticks a frame with the remainder forgiven rather than carried (a stutter must not spiral), position AND view angles interpolated. `tick_freeze` for D-26. **Replay record/play is not written**: it needs the input stream stored somewhere, which is step 5's save format, and the `shots` determinism it serves also needs synchronous chunk loading turned on for the test. Listed in block 5. |
| 3.4 | `input.c` + `look_source.c` + `controls.c` (se_bindings, the user's defaults) | done (menu in 6.1) | 2026-09-21: 21 actions through `se_bindings`, every one remappable and persisted; the defaults the user specified. Looking goes through `input_look()`, so a mouse replaces the cursor keys without touching a call site. The menu itself is step 6.1. |
| 3.5 | `interact.c`: break/place with `ST_PLACED`; `voxel_fx` retargeted; **tree felling** | done (`voxel_fx` deferred) | 2026-09-21: break and place on the key edge, `ST_PLACED` set on every placement, and **the logging rule** -- a placed log drops itself, a grown one fells the tree. Host-tested both ways, including that a tree 12 blocks away keeps all its logs and that the stump below the break survives. `voxel_fx` (crack overlay, particles) waits for block 4's break-progress timer, which is what it would animate. |
| | **Accept host:** collision fuzz, DDA against brute force, felling tests. **Accept device:** `shots scene=replay_walk` gives **identical hashes across two runs** — the proof Part T works | | |
| **4** | **Items** | | |
| 4.1 | `items.c`, `inventory.c`, merged-stack drops, `item_entity.c` | todo | |
| 4.2 | Tool durability; hotbar (F1-F6) and the inventory screen (Tab) | todo | |
| 4.3 | `hud.c`: crosshair, hotbar, health, hunger | todo | |
| **5** | **First playable — worlds on the SD card** | | |
| 5.1 | `screens.c`: title -> world list -> new world (name + seed, or rolled) -> play | todo | |
| 5.2 | Save policy: chunk unload, pause-menu Save, quit. Never per tick | todo | |
| 5.5 | **Pre-generate and save the spawn area on world creation**, behind a "Creating world" progress bar (D-25) | todo | |
| 5.6 | **The entering sequence** (D-26): physics frozen, 3x3 synchronous, play, then stream the rest | todo | |
| 5.3 | Pause menu; `f1_exits = false`; quitting saves first | todo | |
| 5.4 | `worldlist_ui.c` with index + FatFs rebuild; delete a world | todo | |
| | **Accept:** a scripted device test creates a world, edits 200 blocks across 3 chunks, saves, reloads, and reports whether every edit survived. **-> hand to the user** | | |
| **6** | **Settings** | | |
| 6.1 | Controls menu, the synthracer pattern, all actions rebindable | todo | |
| 6.2 | Graphics menu: textures, render scale, view distance; NVS | todo | |
| 6.3 | Audio and display via `se_hw.h` | todo | |
| **7** | **Far Lands** | | |
| 7.1 | `farlands.c` density and ramp; the density lattice for far lands and caves alike | todo | |
| | **Accept:** the far-lands host section; `shots scene=farlands` for a look | | |
| **8+** | **The game** | | |
| 8 | Crafting: grid, recipe table, crafting table, furnace | todo | |
| 9 | Farming: tilled soil, wheat, carrots, seeds, saplings, growth on the tick | todo | |
| 10 | Cooking and the hunger loop | todo | |
| 11 | Animals: pigs, cows, chickens; breeding; dogs (wild, tamed with steak) | todo | |
| 12 | Mobs: zombies, skeletons, spiders; spawning, pathing, combat; beds and spawn; death keeps the inventory | todo | |
| 13 | Fishing | todo | |
| 14 | Audio: SFX voices and procedural music | todo | |
| 15 | Block and sky lighting | todo | |

---

## Part E: findings and decisions log

### Findings (F-n), each with date and source

- **F-01** 2026-09-20, `../tanmatsu-showreel-grace/main/mesh.h:29`: `mesh_tri_t`
  is `uint16 a,b,c; uint8 mat; float uv[3][2]`, so there is **one byte of padding
  free** before `uv`. A `uint8_t dir` face-direction field costs nothing, which
  is what makes the direction-grouped back-face cull (G1b) free.
- **F-02** 2026-09-20, `../tanmatsu-showreel-grace/main/mesh_render.c:22-60`:
  `mesh_submit()` transforms every vertex through `xform_apply()` into a PSRAM
  scratch array and runs `tri_faces_point()` per triangle — even for chunk meshes
  whose xform is identity and whose faces are axis-aligned. This is the measured
  14-16 ms.
- **F-03** 2026-09-20, showreel `devdocs/performance.md`: textured fill runs at
  about 5 Mpx/s, so a screen-sized textured layer costs ~50 ms whatever the
  triangle count; flat fill is 3-4x cheaper; the PPA backdrop is free. Quarter
  resolution roughly doubles the frame rate (overview 10.4 -> 20.6, walk 6.3 ->
  15.5). `depth_order` helps voxel scenes; the raycast renderer is 2.5-5x
  *slower* on them. Vsync caps at 30 fps.
- **F-04** 2026-09-20, showreel F-36: all chunks meshed full-detail overflowed
  both lists (4746 flat, 2181 textured); with the LOD ladder, 2262 flat and 910
  textured. Leaves were 58% of near triangles, plants 15%.
- **F-05** 2026-09-20, `synthengine3D/include/se_save.h`: the save framework is
  **numbered slots** (`SE_SAVE_SLOT_COUNT` 3) in one directory. It does not fit
  many named worlds, so worlds use `se_nbt.h` directly on our own paths.
  `se_nbt.h` also has **no byte array** type (int32/int64/double/string/compound
  only), so chunk payloads need their own container.
- **F-06** 2026-09-20, `nm -D fakelib/liball.so`: **not exported** — `opendir`,
  `readdir`, `remove`, `unlink`. **Exported** — `f_opendir`, `f_readdir`,
  `f_unlink`, `f_rename`, `xTaskCreatePinnedToCore`, `esp_random`, `crc32`, and
  (correcting an earlier assumption) **`fsync`**. Directory work and deletion go
  through FatFs; the dual-directory region scheme is kept anyway, because a FAT
  driver's power-loss semantics are not worth betting a world on.
- **F-07** 2026-09-20, `voxel_mesh.c`: the mesher is already pure data over a
  borrowed `vox_grid_t`. Its only coupling is the `kind()` and
  `voxel_face_mat()` switches over `vox_block_t`; replacing those two with table
  lookups is the whole change, and the `vox_grid_t` ABI is preserved.
- **F-08** 2026-09-20, `voxel_render.c:121`: `build()` meshes through a **shared
  static scratch grid** (`s_grid`, filled by `fill_fine`/`fill_coarse`). That
  races the moment a second task meshes, so the core-1 worker must own its own
  scratch buffer.
- **F-09** 2026-09-20, Part X: the far-lands path needs a 3D density evaluation
  per cell (16384 a chunk) against 256 heightmap columns for normal terrain —
  about 50x the work. Mitigated by a 4x4x4 lattice with trilinear interpolation
  (425 samples a chunk), shared with cave carving. Untested on this hardware;
  the biggest worldgen performance unknown.
- **F-10** 2026-09-20, `voxel_world.c:56`, measured: the donor's lattice noise
  hashes a point as `hash01(ix * 7919 + iz * 104729, seed)` -- two coordinates
  folded into one `int` key. Two concrete defects, both latent in a 128x128
  world:
  - **It aliases.** Both factors are prime, so `(ix, iz)` and
    `(ix + 104729, iz - 7919)` produce the identical key and therefore the
    identical terrain. In *lattice* units, which at a terrain scale of ~32
    blocks puts the twin about 3.4 million blocks away -- far enough not to
    matter for the heightmap.
  - **It overflows.** `iz * 104729` leaves `int32` beyond `|iz| = 20505`, and
    `ix * 7919` beyond `|ix| = 271181`. At terrain scales those are millions of
    blocks out, but the Far Lands and the cave carver sample a **3D density
    field at roughly block resolution**, where the lattice index *is* the world
    coordinate. There `|z| = 20505` blocks is well inside ordinary play.
  An earlier draft of this finding claimed the donor hash collides in a local
  block near x = -100000; measured, it does not (0 collisions in 40000 points),
  because the smallest aliasing offset is 104729. The replacement
  (`common/rng.h`, SplitMix64 over separate 64-bit lanes) is still the right
  fix -- it cannot alias or overflow at any `int32` coordinate -- but the
  reason is the density field, not the heightmap.
- **F-11** 2026-09-20, `synthengine3D/include/se_config.h:127`: `SE_BINDINGS_MAX`
  is **16**, and CraftMiner declares about 20 actions. It **clamps silently**, so
  the last actions would simply not work. Raise it in CMake *and* add a
  `_Static_assert(ACT_COUNT <= SE_BINDINGS_MAX)` in `controls.c`.
- **F-12** 2026-09-20, `voxel_mesh.c:230`: the mesher still `calloc`s its greedy
  mask from the default heap, i.e. internal SRAM, and it will run on the worker
  task. Only about 1 KiB at `CH_H = 64`, but internal SRAM is ~150 KiB free with
  a 62 KiB largest block — hoist it to worker-owned PSRAM scratch.
- **F-13** 2026-09-20, showreel `mesh.h`: `mesh_t` caps at 65535 vertices. A
  greedy-merged 16x16x64 chunk stays far below, but a Far Lands wall chunk is
  unusually face-dense — `worldcheck` asserts no generated chunk exceeds 40000
  vertices at any LOD.

- **F-14** 2026-09-20, step 0.2: `tools/make_textures.py`, trimmed to
  CraftMiner's twenty generators and re-keyed to flat names, regenerates all
  twenty PNGs **byte-identical** to the showreel's committed ones. The
  generators are seeded per texture, not off a shared stream, so dropping the
  sixteen space textures moved none of the others.
- **F-15** 2026-09-20, step 0.3: after `voxel_mesh.c`'s two block switches
  became `BLOCKS[]` lookups, all eleven of the showreel's mesher cases pass
  with its exact triangle counts, and every case's volume equals its solid
  cells and surface area its exposed faces. The conversion is
  behaviour-preserving, which is what made it safe to do before anything else
  is built on the mesher.
- **F-17** 2026-09-20, step 1.2: the load-order-independence check is only
  meaningful if decoration actually crosses chunk borders, so it counts what
  crosses: chunk (0,0) at the test seed has 122 leaf cells, **71 of them on an
  edge**. Generating the 3x3 neighbourhood first and then the middle chunk
  again gives a byte-identical chunk, so `stamp()`'s
  walk-the-neighbourhood-and-clip approach holds.
- **F-18** 2026-09-20, step 1.2, measured over 327184 columns: the height
  field is a single bell from y17 to y38 centred on y27-28, with 31.3% of it
  at or below sea level (y24). That is a reasonable land/water split, but the
  relief is unimodal — there are no distinct plains and mountains, because two
  added fbm stacks tend to a bell. If the world reads as samey when played, a
  ridged or terraced third term is the lever; noted rather than pre-optimised.
- **F-16** 2026-09-20, step 0.5: the cross toolchain is at
  `/home/cavac/idf/tools-6.0`, found through the `IDF_TOOLS_PATH` environment
  variable. The repository has no `.IDF_TOOLS_PATH` file (nor do the showreel
  or synthracer), so builds here pass it explicitly.

- **F-19** 2026-09-20, first device session: the two bridge ports behave
  independently, and the badgelink one can be down while the console is fine.
  Observed: `localhost:4001` (console) negotiates and the badge answers
  `BADGELINK` with `OK switching to badgelink mode` /
  `I (51877) USB device: Switching to BadgeLink USB mode`; `localhost:4003`
  (badgelink) accepts the TCP connection and **closes it immediately**, before
  any protocol, so every `fs` operation dies with `ConnectionResetError`. The
  badge is in the right mode; the proxy is not serving. This is the showreel's
  F-33 seen again, and it is infrastructure, not the app -- the standing rule
  applies: stop and ask, do not work around it.
- **F-20** 2026-09-20, first device session: `ConnectionResetError` from
  badgelink also means "an app is running and holding the USB link", which is
  indistinguishable from F-19 at the tool level. `tools/recover.py` reported
  "no app answered after the reset (probably in the launcher)" while CraftMiner
  was in fact still running, so its guess is not evidence. Check by exiting the
  app before concluding anything about the bridge.
- **F-21** 2026-09-20: `pyserial`'s `rfc2217://` client intermittently fails
  the handshake against this bridge (`Remote does not seem to support RFC2217
  or BINARY mode`) even though the server offers BINARY, SGA and
  COM-PORT-OPTION. `tools/testrun.py` already handles this -- it opens with
  `do_not_open=True`, sets RTS then DTR low, and retries the whole connect
  (its own F-16/F-18). Anything new that talks to the console should go
  through that path rather than opening the port itself.

- **F-22** 2026-09-20, measured on the badge (`results/20260920T201508Z-perf-block/`):
  after `se_run()` has booted the display, audio, input and scene --
  i.e. with both framebuffers, the depth plane and the geometry lists already
  allocated -- **PSRAM: 28796 KiB free, largest block 28672 KiB**. The chunk
  slab takes 8192 KiB (256 slots x 32 KiB) and leaves **20604 KiB free,
  largest 20480 KiB**. Internal SRAM is 160 KiB free / 62 KiB largest both
  before and after, matching the showreel exactly (the slab is PSRAM-only, as
  intended). So the residency radius of 6 is comfortable with about 20 MiB to
  spare, and the planned ~3.2 MiB of chunk meshes fits easily. Nothing here
  forces `CH_H` down or the ring smaller.
- **F-23** 2026-09-20, measured on the badge: **one chunk generates in 56 ms**
  (16 x 16 x 64 cells, `worldgen_chunk`, including trees and plants). On the
  host the same call is 0.519 ms, so the badge is about 108x slower, which is
  in line with the rest of the port. What that means for play:
  - **steady-state walking is fine.** Crossing a chunk boundary needs about
    13 new chunks, i.e. 0.73 s of work, and a player at 4.3 blocks/s crosses
    one every 3.7 s -- a 20% duty cycle on core 1, before meshing.
  - **filling a fresh residency is not.** 169 chunks is about 9.5 s. That is
    a one-time cost at world creation, where it belongs and where it is
    expected (D-25); afterwards the spawn area is on disk and is *loaded*, not
    generated. The 3 x 3 the first tick needs (D-19, D-26) is about 0.5 s of
    generation and less than that of loading, so entering a world is not the
    problem; the problem was only ever the first fill.
  - the lever, if it is ever needed, is F-09's 4 x 4 x 4 density lattice with
    trilinear interpolation. The cave field is two `cm_noise3` calls per stone
    cell below the surface and dominates the cost. **Not done yet** -- 56 ms is
    survivable and meshing has not been measured, so the two get optimised
    together or not at all.
- **F-24** 2026-09-20, the first green cycle: `make cycle` end to end gives
  **30.0 fps** on the placeholder block (300 frames, rast 2.74 ms mean /
  3.09 max, submit 0.09 ms), vsync-capped as the showreel found. The app
  returned to the launcher by itself, so the loop really is hands-free.

- **F-25** 2026-09-20, step 1.3, host checks: the region format's durability
  claims are tested, not asserted. Destroying directory copy A still reads the
  chunk; destroying both reports an error rather than handing back rubbish; a
  corrupted payload reads as "no chunk" (so it is regenerated) and leaves
  nothing half-written. The **torn-write** case is the important one and it
  passes: after two saves, destroying the copy the second save wrote returns
  the **first** save intact, and the region is still writable afterwards. A
  chunk packs to 3962 bytes from 32768; compaction took a region from 339022
  to 29306 bytes with every chunk preserved and no stale copy resurrected.
- **F-26** 2026-09-20, measured on the badge's SD card: **write 21.5 ms,
  read 6.6 ms** for one chunk, round trip identical. **Loading is 8.54x
  cheaper than generating** (6.6 ms against 56.3 ms), which is the number
  D-25 and D-26 were betting on, so the whole "pre-generate once, load ever
  after" shape holds. What it means:
  - **Creating a world**: 169 chunks x (56.3 gen + 21.5 write) = about
    **13 s** behind the progress bar. Long, but once, and expected.
  - **Loading one**: 169 x 6.6 ms = about **1.1 s** for the whole view
    distance; the 3 x 3 the first tick needs is **59 ms**, i.e. instant.
  - **Walking through explored land** is essentially free: 13 chunks a
    boundary is 86 ms of reading, against 730 ms if it has to generate.
  - Saving on eviction at 21.5 ms a chunk is cheap enough to stay off the
    frame budget entirely.
  - Compaction is 43 ms and works on the card.
- **F-27** 2026-09-20: `make verify` **cannot** catch a missing symbol in code
  nothing calls -- `--gc-sections` removes it from `app.so` first, so the check
  passes and the app still fails to LOAD on the badge. The region and
  compaction paths only started referencing `f_unlink` / `f_rename` once
  `main.c` actually called them. Anything that exists to work around a missing
  export has to be exercised on the device, not merely compiled.

- **F-28** 2026-09-20, step 1.4: the compatibility guarantee is tested in both
  directions rather than assumed. A record written by a "newer build" (a cow
  with `aggressive`, `sitting`, a nametag and a breeding timestamp) is read
  correctly by an "older" reader that knows only position and health; a record
  written by the older one is read by the newer with every absent field at its
  default, not at rubbish. Skipping a nested compound lands exactly on the next
  field, which is the case a furnace's inventory needs.
- **F-29** 2026-09-20, step 1.4: `level.cmw` names all 17 blocks in its palette,
  and a remap applied at decode moved 6361 stone cells in a test chunk. A block
  the remap drops becomes air rather than whatever now sits at that number.

- **F-30** 2026-09-20, step 2.1: the first streaming loop drew an empty world
  at a confident 30 fps. `do_load()` reached for its chunk with `chunk_find()`,
  which deliberately hides a chunk while it is `CS_LOADING` so no game code can
  read a half-filled one -- and the loader is the code doing the filling. Added
  `chunk_slot_claimed()` for the one caller that is allowed to see it, and a
  host streaming check that reproduces the bug when the fix is reverted. Nothing
  else in the suite noticed; an empty world is not a crash.
- **F-31** 2026-09-20, step 2.4, measured: **the engine's framebuffer clear was
  13 ms a frame.** With no `on_backdrop` registered, `se_run` clears the whole
  800x480 buffer to `backdrop_argb` every frame -- and the quarter-resolution
  upscale then covers every pixel of it. Registering an empty `on_backdrop` took
  the residual from 14.4 ms to 1.3 ms and the frame rate from 11.5 to 16.0 fps.
  It showed up as unattributed time, not as any phase.
- **F-32** 2026-09-20, step 2.4, measured: submit costs about **4.4 us per
  triangle submitted**, and it is the engine's projection, not the cull. The
  cull is working: 8886 triangles tested, 4356 passed. What the profile actually
  said was that far too many are being submitted -- medium view distance put
  ~4400 a frame past the engine's 4096 flat cap, where the overflow is **dropped
  silently**. Deferring the second and third vertex loads past the cull, and
  caching the fog palette per step instead of per chunk, moved submit by under a
  millisecond: both were the wrong suspects.
- **F-33** 2026-09-20, step 2.4, measured on the host: **68% of a chunk's
  triangles are cave walls**, none of them visible from the surface. Chunk (0,0)
  meshes to 1094 triangles, of which 744 sit well below the surface; filling the
  caves in gives 426, a 2.6x reduction. That is why 15 visible chunks submit
  ~7000 triangles for ~1000 drawn.
  **The fix is vertical render sections** (D-34): a 16 x 16 x 64 chunk is one
  mesh with one bounding box spanning bedrock to sky, so the frustum test cannot
  reject the underground half. Splitting it into 16-high sections, each with its
  own box, lets the cull drop what is beneath the ground the player is standing
  on. This is what the original plan called for (a `y0` field on `vox_grid_t`)
  before D-11 traded it away for keeping the donor mesher untouched; the trade
  is measurably worse than expected.

  **CORRECTED 2026-09-21 by F-35. The 68% figure does not survive a wider
  sample, and the single chunk it came from was a bad one to have picked.**
  Chunk (0,0) is sea floor: it meshes to 346 triangles, not 1094, and 344 of
  them are in one section. The finding should never have been stated from one
  chunk. Sectioning is still right, for the reasons F-35 gives -- but not for
  the reason given here, and not by the factor claimed.

- **F-35** 2026-09-21, step 2.5, measured on the host over **49 chunks sampled
  across 4000 blocks of world** (not one chunk, which is what went wrong in
  F-33) and on the badge:

  | where the triangles are | share |
  |---|---|
  | y 0-15 (underground, caves) | 34% |
  | y 16-31 (the surface band) | 47% |
  | y 32-47 (hills above it) | 17% |
  | y 48-63 (sky) | 0% |

  Only 6 of the 49 chunks have 90% or more of their triangles in a single
  section, so **the geometry really is spread over three sections** and a
  frustum test per section has something to reject. But it is 34% underground,
  not 68%, and a box test is conservative -- the near chunks' underground
  sections are partly in view from three blocks above the ground.

  On the badge, same camera path, same 20-second window: **12.32 and 12.83 fps
  without sections, 13.95 and 13.25 with** -- about **8% faster**, rasterise
  51.2 -> 49.9 ms, ~15% fewer triangles submitted. Two runs each; the spread
  within one build is ~5%, so this is a real gain but a modest one, and it is
  smaller than F-33 promised.

  The terrain itself is fine and was briefly suspected of not being: over 94864
  samples the height runs y14..41, median 27, 24% at or below sea level, which
  matches what step 1.2 recorded. **The origin is simply ocean**, which is why
  the one chunk F-33 measured looked the way it did.

  Sectioning earns its keep in two further ways that are not in the frame time:
  a section is only meshed when it is about to be drawn, so the underground of
  a chunk you never look into is **never built at all**; and a block edit now
  dirties 4096 cells instead of 16384, which is what break-and-place in step 3.5
  will pay for.

- **F-36** 2026-09-21, step 2.5: **the 16.0 fps in F-31 was measured with the
  camera pointing at the sky.** Two bugs, both in the debug flight and both
  invisible until the camera became steerable:
  * the engine's forward vector is `(sin yaw, cos yaw)` in x and z, and the
    scripted flight's `yaw = a + pi/2` matched *neither* component of its own
    direction of travel -- it had been flying sideways since it was written.
    The yaw that matches both is `-a`.
  * **positive pitch looks DOWN** (`se_scene.c`, `camera_build_basis`:
    `fwd.y = -sin pitch`). The flight passed `-0.18`, tilting it up into empty
    sky, which is cheap to fill.
  Corrected, the same build measures 12.3 rather than 16.0. Every frame rate
  recorded before this one is optimistic by roughly that much. The convention
  is now written down at both places that use it.

- **F-34** 2026-09-20, step 2.4 (**its speculation is corrected by F-40**; the
  fill rate is set by span setup, not by scalar arithmetic): the engine's
  rasteriser is **scalar C**. No
  PIE/SIMD appears anywhere in `synthengine3D/src/se_scene.c`; the engine's only
  SIMD mention is minimp3's x86/ARM paths, disabled by `MINIMP3_NO_SIMD`. The
  ESP32-P4 has a 128-bit SIMD unit whose 16-bit lane arithmetic and saturating
  ops match what a textured span loop does. So the measured ~5 Mpx/s is the
  speed of this implementation rather than of the hardware. **Unprofiled** --
  how much headroom that represents is not known and is not worth guessing.

- **F-37** 2026-09-21, the user asked where the internal SRAM goes: **the app's
  own statics are in PSRAM, not internal SRAM.** kbelf loads `app.so` there --
  a static probe sits at `0x4801e6f0`, in the same region as a PSRAM
  allocation, while an internal allocation is at `0x4ff37ee8`. So `app.so`'s
  224 KiB of `.bss` costs no internal SRAM at all, and **the 35 KiB that D-34's
  commit message claims to have taken "off the internal-SRAM bss" was PSRAM**.
  The saving is real; the memory it came from was misnamed.

  What is actually using internal SRAM: **462 KiB of 622 KiB, in 301 blocks,
  all of it allocated before `on_init` runs** -- ESP-IDF, graceloader and the
  engine's boot. Low-water equals free, so nothing has been released since. The
  app itself takes about 18 KiB (the worker's stack and queues, the texture
  cache). 160 KiB free, largest block 62 KiB.

- **F-38** 2026-09-21: **the block textures fit in internal SRAM with room to
  spare, and it makes no measurable difference.** All eighteen are 16x16 RGB565
  -- 512 bytes each, **9216 bytes for the set** -- against 160 KiB free, so no
  freeing was needed for the move the user asked about. Textured fill measured
  36.0 ms a frame with them internal against 35.7 and 37.0 with them in PSRAM:
  **a null result**, inside the run-to-run spread. The reason is in F-40: the
  texel fetch is not what the loop is waiting for. Kept anyway, because 9 KiB
  is nothing and the argument only gets better as textures are added.

- **F-39** 2026-09-21: **the engine and the app were both built `-Os`**, and at
  `-Os` the compiler would not inline `scene_index()` or the three span
  functions despite their `static inline` -- so every one of 28000 spans a
  frame paid a function call. `-O2` on the engine: flat fill 22.0 -> 19.4 ms,
  textured 37.7 -> 34.7. `-O2` on the app as well: submit 11-13 -> 10.2 ms.
  `.text` grew 71 -> 87 KiB, which is PSRAM and therefore free (F-37).

  And **`ceilf`/`floorf` are library calls even at `-O2`**: they must set
  `errno`, so GCC cannot fold them into the single RISC-V convert the value
  needs. The column scans call them twice per span -- **57000 library calls a
  frame** to round numbers already in float registers. Replaced with inline
  `ceil_i`/`floor_i`: flat 19.4 -> 13.4 ms, textured 34.7 -> 31.7.

  Altogether **rasterize 49.7 -> 43.6 ms and 13.4 -> 15.3 fps**, with no SIMD
  written.

- **F-40** 2026-09-21, the answer to "would SIMD help?", measured rather than
  argued. The chain of measurements matters as much as the conclusion, because
  the first two readings each pointed the wrong way:

  | measured | result |
  |---|---|
  | flat vs textured, per pixel | 230 vs 214 ns -- textured is **not** dearer |
  | span-loop memory pattern alone, PSRAM | 55 ns/px (internal SRAM: 39) |
  | the same with a 16-bit depth plane | 56 ns/px -- **no change** |
  | the loop's arithmetic alone | 45 ns/px (16 cycles at 360 MHz) |
  | the real rasterizer | 172-199 ns/px (62-72 cycles) |
  | **average span length** | **6.0 pixels flat, 12.5 textured** |

  Read in order: textured costing the same as flat per pixel says the
  arithmetic is not the wall. A 16-bit depth plane not helping says the bytes
  are not either -- the memory cost is per-access latency, not bandwidth, so
  halving the depth plane would have bought nothing and the engine change it
  would have needed was avoided. Arithmetic (16 cyc) plus memory (20 cyc) is
  **36 of the 62-72 cycles a pixel costs**, so the rest is neither: it is
  per-span setup, over spans **six pixels long**.

  **So SIMD is the wrong tool here.** The ESP32-P4's PIE vector unit is real,
  128-bit with 16-bit lanes, and this toolchain already enables it -- the app
  compiles with `xesploop_xespv2p1` today, and the assembler accepts
  `esp.vld.128`, `esp.vadd.s16`, `esp.vcmp.gt.s16` and the rest. But a 6-pixel
  span does not fill one 8-lane vector; the depth test needs a per-pixel
  conditional store; and the textured loop's texel fetch is a **data-dependent
  gather**, which PIE has no instruction for. Vectorising the inner loops would
  attack the 16 cycles that are already the cheapest part.

  **F-34 is therefore wrong where it speculates.** "~5 Mpx/s is the speed of
  this implementation rather than of the hardware" is true, but its implied
  cause -- scalar arithmetic in the span loops -- is not. The fill rate is set
  by how many spans the geometry breaks into, and the fix is fewer and longer
  spans (bigger on-screen triangles, more aggressive distance LOD), which is
  game-side work.

  The measurements live in `main/game/membench.c` and in the engine's
  `scene_fill_stats()`, so any of this can be re-checked rather than believed.

- **F-41** 2026-09-21, **the user, flying by hand**: "it sometimes seems to
  reload all the chunks". Three separate causes, found by logging the
  streaming's flow per second (`stream/s:` in the app log) rather than by
  reading the code:

  1. **The far view preset did not fit the chunk ring, and this one really
     does reload everything.** `evict_radius` 8 keeps a 17-chunk square; the
     ring is 16 across and a slot is the low four bits of the coordinate, so
     two resident chunks land on the same slot. Each evicts the other, each is
     then read as `BLK_BARRIER` and requested again, **forever, even while the
     player stands still.** `CH_RING`'s own comment said "a residency radius up
     to 7" and the preset asked for 8. Fixed to 7, clamped in
     `chunk_render_set_view()`, and `_Static_assert`-ed against `CH_EVICT_MAX`
     so the build stops rather than the world thrashing -- verified by putting
     8 back and watching it fail to compile.

  2. **A level-of-detail change drew nothing until its mesh arrived.** Each
     level is a separate mesh, so crossing a distance band asks for one that
     has never existed. Flying *upwards* moves every chunk into `LOD_COARSE` at
     once: ~160 section meshes that do not exist, at ~30 applied a second. The
     renderer now falls back to whichever level IS built while the wanted one
     is queued. A frame at the wrong detail is not noticeable; a hole in the
     ground is.

  3. **The result budget had not kept up with D-34.** `CHUNK_RESULTS_PER_FRAME`
     was 2 -- about 30 a second -- chosen when a chunk had 3 meshes. Sectioning
     made it 12, and a fast flight wants ~90 a second. The worker then fills
     its 48-deep result queue and **blocks**, which stops loads as well as
     meshes. Measured at startup: `asked 80, applied 36, queue 44/48`. At a
     budget of 8: `asked 82, applied 82, queue 0/48`.

  Causes 2 and 3 are **regressions I introduced with D-34** and did not think
  to look for: sectioning multiplied the number of mesh jobs by four and I left
  every budget around it alone.

- **F-42** 2026-09-21, step 3.1: the first step-up test **passed while the code
  was wrong and then failed while it was right**, because it checked the body's
  height after a walk that had already crossed the step and dropped off the far
  side. A physics assertion has to name the moment as well as the value. The
  test now walks onto a plateau wide enough to end standing on it.

- **F-43** 2026-09-21, **the user, playing it**: four faults, and three of them
  were invisible from reading the code.

  1. **The jump could not clear a block.** Apex **0.83 blocks**, so pillaring up
     -- how you get out of a hole -- was impossible. Cause: gravity was applied
     BEFORE the move, so the first tick of the jump was spent decelerating
     instead of rising. The same three constants give 1.25 the other way round.
     Nothing in the code looks wrong; it took simulating the arc. The order now
     lives in `phys_gravity()`, which the player and the host test both call,
     and the test asserts the apex in blocks and the hang time in seconds.
  2. **Too fast to aim.** At 15 fps a Minecraft jump is nine frames from
     take-off to landing. Retuned to the same apex over a longer arc -- the
     pair `(v0 * k, g * k^2)` with k = 0.7 -- giving **1.33 blocks, 0.85 s**.
  3. **You could see through a wall you were touching.** The engine's near clip
     plane is **0.5** and the player's eye is **0.3** from a wall they are
     flush against and **0.18** below a ceiling they stand under. Both were
     being clipped away. Now 0.1, with a `_Static_assert` tying it to
     `PHYS_PLAYER_W` and `PHYS_PLAYER_EYE` so it cannot drift back --
     verified by restoring 0.5 and watching the build fail. The cost is depth
     range and precision, both proportional to the near plane: the far limit
     falls from 32000 blocks to 6400, still ninety times what this draws.
  4. **Breaking a block blanked the surrounding area for a frame.** An edit
     marks every level of its section stale, and the renderer treated stale as
     undrawable, so the chunk disappeared until the worker caught up.
     **"Stale" and "never built" are not the same state** and conflating them
     was the bug: `chunk_t` now carries `lod_built` alongside `lod_stale`, a
     mesh one block out of date goes on being drawn while its replacement is
     queued, and only a mesh that has never existed is skipped.

- **F-44** 2026-09-21, the user: **aiming was guesswork** -- no crosshair, no
  highlight. Two things came out of adding them.

  **The crosshair does not go in the middle of the screen.** The engine
  projects the camera's forward axis to `(RENDER_HALF_W, RENDER_HORIZON_Y)`,
  and `RENDER_HORIZON_Y` is **256 on a 480-row display**, not 240 -- a game can
  move its horizon (se_config.h). Drawn at the geometric centre it would sit
  16 pixels below where the pick actually points, which is an aiming error
  nobody would think to suspect. Derived from the projection constants instead.
  Verified by pulling the framebuffer off the badge and looking at it
  (`badgelink fs download` on a `shots` PNG).

  **The picker reported `BLK_BARRIER`.** An unloaded chunk is deliberately
  solid so the player stops at the edge of the world rather than falling out of
  it (D-14) -- but it is not a block, and the picker was happy to report one,
  which put a highlight box round a piece of fog and offered to mine it. Found
  by accident: a throwaway raycast from the scripted camera drew a box at
  point-blank range and it came out as a black line across the whole screen.
  `ray_pick` now stops at a barrier without reporting a hit, and the host test
  asserts both halves -- solid to the body, invisible to the picker.

- **F-45** 2026-09-21: **the `shots` test cannot yet photograph the world.** It
  SETS the clock rather than running it, so only a few frames render and the
  chunks never stream in -- every shot is empty sky. That is the async chunk
  worker, and the fix is the synchronous mode D-15 put there for exactly this,
  switched on for the duration of a shots run. Not done: it belongs with the
  replay work deferred out of step 3.3, and until then shot hashes cover the
  overlay but not the world.

### Decisions (D-n), each with date and who decided

- **D-01** 2026-09-20, Claude: **a floating render origin.** The engine subtracts
  the camera after world floats reach it, so coordinates near 100000 lose ~0.008
  blocks of precision — exactly where the Far Lands are. Chunk meshes are built
  in chunk-local coordinates and offset at submit; **no absolute world coordinate
  ever reaches `se_scene`**, `voxel_fx` included.
- **D-02** 2026-09-20, Claude: **fixed 20 Hz simulation, rendering interpolates.**
  Stable physics, exact replays, and the pure-function-of-showtime property the
  `shots` test needs. See Part T.
- **D-03** 2026-09-20, Claude: **one texture per material, no atlas.** The greedy
  mesher emits UVs in block units and relies on the rasterizer's power-of-two
  mask wrap to tile them; an atlas would force per-block quads.
- **D-04** 2026-09-20, Claude: **chunk payloads get their own binary format**
  (F-05); NBT keeps the world header, the player and the world index.
- **D-05** 2026-09-20, the user: **F1-F6 are hotbar slots**, so `f1_exits` is
  false and leaving goes through a pause menu that saves first.
- **D-06** 2026-09-20, the user: **both textured and flat shading**, exposed in a
  graphics menu with render scale and view distance; default textured, half scale.
- **D-07** 2026-09-20, the user: **the first playable build is walk / mine /
  place / save** — terrain, streaming, collision, break and place, hotbar and
  inventory, worlds on the SD card. No mobs, crafting or farming yet.
- **D-08** 2026-09-20, the user: **full survival** — health, hunger and tool
  durability.
- **D-09** 2026-09-20, Claude: **`SE_SCENE_TEXTURED_TRI_CAP` -> 2048**, flat stays
  at 4096 (F-04); set before `add_subdirectory(synthengine3D)`.
- **D-10** 2026-09-20, Claude: **stay on `SE_RENDER_ZBUFFER`** with `frustum_cull`
  and `depth_order` on (F-03).
- **D-11** 2026-09-20, Claude (render half **reversed** by D-34, storage half
  still standing): **chunk 16 x 16 x 64, one column, no vertical
  chunking.** 32 is too shallow for mining plus build room plus a bedrock-to-sky
  wall; 64 keeps the single-column layout the donor mesher and `fill_fine` want.
  Revisit sectioning only if remeshing shows up in a profile.
- **D-12** 2026-09-20, Claude: **two parallel 1-byte planes** (id, state), not one
  interleaved `uint16` plane, because `voxel_mesh_build()` takes a
  `uint8_t const*` and interleaving would force a de-interleave on every remesh.
- **D-13** 2026-09-20, Claude: **a 16x16 ring with a `cx`/`cz` identity check**,
  not a hash. `chunk_find` is the hottest function in the program.
- **D-14** 2026-09-20, Claude: **unloaded chunks read as `BLK_BARRIER`** — the
  player stops at the edge of generated terrain rather than falling through it.
- **D-15** 2026-09-20, Claude: **a synchronous chunk-loading mode from day one**,
  required by the `shots` test and by new-world spawn.
- **D-16** 2026-09-20, Claude: **region files of 8x8 chunks**, not per-chunk files
  (FAT cluster waste and directory scans) and not Minecraft's 32x32.
- **D-17** 2026-09-20, Claude: **dual directory + serial + CRC32** for region
  durability, with a strict write order, rather than trusting `fsync` (F-06).
- **D-18** 2026-09-20, Claude: **a world index plus a FatFs rebuild**, so a world
  hand-copied onto the card still appears.
- **D-36** 2026-09-20, Claude: **fix the game side before touching the
  engine.** The 66 ms frame splits roughly 21 ms game / 43 ms engine (G5), and
  the game's share is mostly geometry that should never have been submitted.
  Engine work -- a custom renderer through `se_renderer_register`, or SIMD in
  the span loops -- stays a stop-and-ask, and would in any case be measured
  more honestly once the noise is gone.
- **D-34** 2026-09-20, Claude: **render in vertical sections**, reversing the
  render half of D-11. Chunk storage stays one 16 x 16 x 64 column -- the
  planes, the codec and the save format are all fine -- but its *mesh* splits
  into 16-high sections with a bounding box each, so the frustum cull can reject
  everything below the ground. Measured cause in F-33.
  **Done 2026-09-21.** `vox_grid_t` gained `y0`; a chunk keeps `CH_MESH_N` = 12
  meshes (3 levels x 4 sections) in the store's PSRAM slab rather than in the
  static `chunk_t`, which took 35 KiB *off* the internal-SRAM bss as a
  side-effect. The level of detail stays the **chunk's**, not the section's:
  two stacked sections at different resolutions would not line up where they
  meet and the coarse skirt only closes the sides. Worth about 8% (F-35), which
  is less than F-33 predicted -- kept because the structure is right, the
  meshing saving is real, and it is what step 3.5's block edits need.

- **D-37** 2026-09-21, Claude: **a finding measured from one chunk is not a
  finding.** F-33 was stated from chunk (0,0), which turned out to be the one
  place in the world where the claim was both true and meaningless (it is sea
  floor, and 99% of its triangles are in one section). The rule now: any claim
  about "the terrain" or "a chunk" is measured over a sample spread across the
  world, and the sample size goes in the finding.

- **D-50** 2026-09-21, Claude: **the crosshair is inverted, not painted.** A
  white one vanishes against sand and a black one against a cave mouth; the
  inverse of whatever is behind it is legible against everything. Minecraft's
  does the same, for the same reason.

- **D-48** 2026-09-21, Claude: **movement constants are chosen by simulating
  the arc, not by feel, and the result is asserted in blocks and seconds.** A
  jump is three constants and an integration order, and the height that comes
  out is not something you can read off the source -- the first version was a
  third short and looked perfectly reasonable.

- **D-49** 2026-09-21, Claude: **a mesh that is out of date is still worth
  drawing.** Only one that has never been built is not. See F-43.4: treating
  the two as one state made the world blink at every block break.

- **D-45** 2026-09-21, Claude: **a step is a whole block, not Minecraft's
  0.6.** There, 0.6 exists so that stairs and slabs are walkable and full blocks
  are not; you jump for those. This is a handheld with a keyboard and no mouse,
  there are no stairs yet, and tapping jump at every clod of terrain is tiring
  in a way it is not with a hand already on a mouse. Walls are still walls: a
  step is kept only if the body can settle onto something afterwards, so two
  blocks stops you, and the host test asserts exactly that. Easy to reverse --
  it is one constant, `PHYS_STEP`.

- **D-46** 2026-09-21, Claude: **Esc leaves, until the pause menu exists.**
  `f1_exits` is now false because F1-F6 are the hotbar (D-05), so the engine's
  own way out is gone. Esc becomes "open the pause menu" in step 5.3, and that
  menu saves before it quits -- which is the reason the key had to come back
  from the engine in the first place.

- **D-47** 2026-09-21, Claude: **the hotbar holds six placeable blocks until
  block 4 gives it an inventory.** Cobble, planks, dirt, glass, torch, sand on
  F1-F6. Breaking and placing are the thing worth testing by hand now, and
  waiting for the inventory to test them would be waiting for the wrong reason.

- **D-43** 2026-09-21, Claude: **a limit the ring imposes is checked by the
  compiler, not by a comment.** `CH_RING`'s comment had said "a residency
  radius up to 7" since it was written, and a preset asked for 8 anyway
  (F-41). `CH_EVICT_MAX` plus `_Static_assert` makes that a build failure.

- **D-44** 2026-09-21, Claude: **never draw nothing when something is
  available.** If the level of detail a section wants is not built yet, draw
  one that is. A streaming world's visible quality is set by what it does while
  it waits, and "wait with a hole in the ground" is the worst of the options.

- **D-39** 2026-09-21, the user: **engine work is authorised**, and the engine
  stays at **version 2.1** while it is being worked on -- no bump per change.
  The standing "an engine problem means stop and ask" rule (D-15's sibling)
  still holds for anything beyond what has been asked for.

- **D-40** 2026-09-21, Claude: **`-O2`, not `-Os`, for both the engine and the
  app.** Both run from PSRAM, where code size is the resource that is not
  scarce, and `-Os` was costing a function call per span. Measured in F-39.

- **D-41** 2026-09-21, Claude: **measure the shape of the work before
  optimising it.** Three plausible theories here -- scalar arithmetic, PSRAM
  bandwidth, texel-fetch locality -- were each worth a day and each wrong, and
  a fourth (per-span setup over 6-pixel spans) was not on the list until the
  spans were counted. The engine now reports pixels *and spans* per pass
  (`scene_fill_stats`) precisely so the next person does not have to guess.

- **D-42** 2026-09-21, Claude: **no SIMD in the span loops.** Available,
  enabled, and the wrong tool: see F-40. Revisit only if spans get much longer,
  which is a geometry change, not an engine one.

- **D-38** 2026-09-21, Claude: **free flight when no test is running, the
  scripted path when one is.** `devtest_running()` decides. A `shots` test needs
  the frame to be a pure function of the show clock, and a camera driven by
  which keys are held is not -- but a world nobody can steer through is a world
  whose bugs are only found by accident. Both, chosen automatically, is the way
  to have the two properties at once. The debug camera deliberately does **not**
  go through `se_bindings`: those slots are the player's (D-05).
- **D-35** 2026-09-20, Claude: **an empty `on_backdrop` is required**, not
  optional, for any game that covers the screen itself. See F-31: the default
  clear costs 13 ms a frame and is invisible in the phase split.
- **D-29** 2026-09-20, the user (prompted by stairs, rails, redstone and
  furnaces): **three tiers of block state** — id, id + 7 bits of block-owned
  data, and a block entity for anything variable-sized. The state byte was
  originally split into fixed growth/variant fields; those were too small and
  too presumptuous, so bits 1-7 are now simply the block type's own.
- **D-30** 2026-09-20, the user: **extensible per-record data, so upgrades need
  no upgrade system.** Load = defaults, then overwrite what is recognised, then
  skip the rest; save always writes today's format, so a world rewrites itself
  as its chunks are unloaded. Implemented as `common/tags.h` (NBT's model over a
  byte buffer, because a chunk payload is built in memory on the worker) and, a
  level up, as skippable sections on the chunk payload.
- **D-31** 2026-09-20, Claude: **a block palette in `level.cmw`** maps saved ids
  to names, so block ids can be added, reordered or removed. Entities and block
  entities name their type as a string instead and need no palette.
- **D-32** 2026-09-20, the user: **a major version in each file's magic**
  (`CMR1`, `CMW1`), bumped only for a change tags cannot absorb, and a mismatch
  is refused rather than guessed at so an upgrader has a definite hook.
- **D-33** 2026-09-20, the user: **creatures and dropped items persist with
  their chunk.** Creatures indefinitely, like blocks; dropped items for 10
  minutes of *loaded* time, so walking away does not cost the player their
  drops. Hostile mobs keep spawning in unlit places up to a cap per loaded
  chunk, which is what bounds the population given nothing despawns.
- **D-27** 2026-09-20, Claude: **`world/vfs_compat.{c,h}`** for the file
  operations graceloader does not export -- `remove`, `rename`, `unlink`,
  `opendir`/`readdir` (F-06). They go through FatFs, with the same
  try-the-plausible-spellings path mapping the engine uses
  (`se_mp3.c:198`), and through plain stdio under `CM_HOST` so the region
  checks still run on a PC. A missing export here is a **load-time** failure,
  which is why the device self-test exercises compaction rather than trusting
  `make verify` (F-27).
- **D-28** 2026-09-20, Claude: **region files carry their own CRC-32**
  rather than calling graceloader's exported zlib one. A region written on the
  badge has to validate on a PC and the other way round, and "two zlibs agree"
  is an assumption; a 64-byte nibble table makes it a fact. Verified against
  the standard check value (`"123456789"` -> `0xCBF43926`).
- **D-25** 2026-09-20, the user: **pre-generate the spawn area when a world
  is created**, as Minecraft does, behind a "Creating world" progress screen.
  This is where the 9.5 s of F-23 goes: once, visibly, at a moment the player
  already expects to wait -- instead of as a stutter every time they load the
  world. The chunks are *saved* as they are generated, so it also pays for
  itself twice over: every later visit to the spawn area is a disk read rather
  than a re-generation, and the one big write fits the
  "write only when needed" rule (one batch, at creation).
- **D-26** 2026-09-20, the user: **the spawn/respawn sequence is
  3 x 3 first, then play, then fill.** Load (or generate) the 3 x 3 chunks
  around the player **with physics frozen**, so they cannot fall through an
  absent world; the moment those nine are resident, gameplay runs normally;
  the rest of the view distance streams in over the next few seconds while the
  player is already moving. This refines D-19: the gate on running a tick is
  the **3 x 3 only**, never the full view distance, so a large view distance
  never delays the start of play. It also means respawning after death is
  normally a *load* of saved chunks, not a generation -- a bed is somewhere
  the player built, and world spawn was pre-generated at creation.
- **D-22** 2026-09-20, Claude: **`install` depends on a `mode` target** that
  calls `tools/testrun.py`'s own `ensure_badgelink_mode()` -- which probes
  BadgeLink first and only then asks for it on the console, retrying six
  times. The badge leaves USB in debug mode whenever the launcher comes back,
  and badgelink then fails with `ConnectionResetError`. An earlier attempt made
  `install` depend on the raw `mode_badgelink` target instead; that turned the
  console's flaky handshake (F-21) into a hard install failure, which was
  worse than the problem. Reuse the tool that already handles it.
- **D-23** 2026-09-20, the user: **`make ping`**, plus `make mode` and
  `make exitapp` -- thin wrappers in `tools/badgectl.py` over `testrun.py`'s
  connection code. When a cycle fails it is rarely clear whether the app is
  alive, wedged or never started, and `ping` answers that in two seconds
  (F-20 was exactly that confusion costing half an hour).
- **D-24** 2026-09-20, Claude: **generate `app_version.h`** from
  `main/app_version.h.in` with `git describe --always --dirty`, as the
  showreel does. Without it the app reports `git unknown` and `testrun.py`
  refuses every result as coming from an unidentifiable build -- correctly,
  but it looks like a link failure.
- **D-20** 2026-09-20, Claude: **textures live flat in `textures/`**, not in a
  per-segment subdirectory. CraftMiner is one app, not a reel with segments, so
  the showreel's `segcheck` separation buys nothing here. They install to
  `<app>/textures/`.
- **D-21** 2026-09-20, Claude: **the leaves-see-leaves rule became a flag**
  (`BF_SEE_SELF`) rather than the donor's hard-coded `b == VB_LEAVES`, so a
  future see-through block chooses its own behaviour in the table.
- **D-19** 2026-09-20, Claude: **a tick runs only when the 3x3 chunks around the
  player are ready.** No tick branches on load state, so replays are exact
  regardless of SD-card timing. This is the crux of Part T.

---

## Verification

- **Host:** `make check` = `worldcheck` + `scenecheck` + `meshcheck` +
  `hostpurity`. Seconds, no badge. Runs as part of `make build`.
- **Device:** `make cycle TEST="perf scene=flyover secs=20"` for the frame rate
  and phase split; `make testrefs` / `make testcompare` with
  `TEST="shots scene=replay_walk ms=..."` for framebuffer-hash regressions.
- **Build hygiene:** `make build` clean with no new warnings, `make verify`
  (every undefined symbol exists in fakelib), `make format`.
- **By hand:** the user plays step 5 and gives feedback before step 8 starts.

### Where it stands after block 1 (2026-09-20)

`make check` runs eleven sections on the host in about a second: blocks,
hashing and noise, tagged fields, chunk coordinates, the state byte, the store,
worldgen, the codec, regions (damage, torn writes, compaction), sections, the
world store, the palette, and streaming. `make cycle` builds, installs, runs and
measures on the badge without anyone touching it, and the app returns to the
launcher by itself. `make ping` / `mode` / `exitapp` answer "is it alive" when a
cycle goes wrong.

## Critical files

- **Lifted from `../tanmatsu-showreel-grace`:** `main/craftminer/voxel/*`,
  `main/{mesh,mesh_render,xform,camera,backdrop,horizon}.{c,h}`,
  `main/common/texcache.*`, `textures/craftminer/*.png`,
  `tools/{make_textures.py,meshcheck.c,scenecheck.c}`.
- **Adapted from `../tanmatsu-synthracer-grace`:** `main/controls_settings.{c,h}`
  and `main/keybind_ui.{c,h}` (the `se_bindings` + `se_ui` + `se_ui_capture_key`
  pattern), and the `main/screens.c` menu shape.
- **Copied verbatim:** `dir_open()` from `synthengine3D/src/se_mp3.c:198-245`,
  the FatFs path probing for world enumeration and deletion.
- **Changed:** `CMakeLists.txt`, `Makefile`, `metadata/metadata.json`,
  `main/main.c`, `README.md`.
- **Engine:** no changes planned. Anything that turns up there -> stop and ask.
