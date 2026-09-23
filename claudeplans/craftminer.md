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
west of spawn, a short walk away (D-78). Core 1 for background work. Every key remappable in a menu.

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

**`*` marks what exists today; everything else is still the plan.** Kept
honest deliberately -- a layout document that quietly disagrees with the tree
is worse than none, because it is the thing a newcomer trusts.

```
main/
* main.c                  app_main, se_app_config_t, the five callbacks
  app.h                   cm_app_t -- the context threaded through se_run's `user`
  common/
*   psram.h               cm_alloc/cm_calloc/cm_free -- the ONLY host/badge seam
*   rng.{c,h}             xorshift64*, hash2/hash3, value noise          (pure)
*   tags.{c,h}            the NBT-like tagged-field codec (D-30)         (pure)
*   texcache.{c,h}        LIFTED
  math/
*   xform.{c,h}           LIFTED (vec3_t, mat3_t, smoothstep)            (pure)
*   mesh.{c,h}            LIFTED + mesh_tri_t.dir                        (pure)
*   mesh_render.{c,h}     LIFTED + mesh_submit_world()
*   camera.{c,h}          LIFTED
  voxel/
*   voxel_mesh.{c,h}      LIFTED + sections (D-34) + light in the merge key (pure)
*   voxel_sky.{c,h}       LIFTED: sun, moon, world-space clouds, stars
*   starfield.{c,h}       LIFTED from the showreel's common/
    voxel_fx.{c,h}        LIFTED, not yet built: waits on the crack overlay
    backdrop.{c,h}        LIFTED, not yet built
    horizon.{c,h}         LIFTED, not yet built
  world/
*   blocks.{c,h}          BLOCK REGISTRY                                 (pure)
*   worldgen.{c,h}        pure (seed, cx, cz) -> id/state planes         (pure)
*   farlands.{c,h}        Beta 1.7.3's density generator, overflowed     (pure)
*   datadir.{c,h}         /sd/craftminer, and moving old data into it (D-80) (pure)
*   chunk.{c,h}           chunk_t, the ring store, world_block/set/state
*   light.{c,h}           sky + block light: floods on arrival and on change (pure)
*   chunk_codec.{c,h}     RLE over a chunk's two planes                  (pure)
*   chunkmesh.{c,h}       one vertical section into a mesh               (pure)
*   region.{c,h}          region file: header, dual directory, compaction
*   vfs_compat.{c,h}      the FatFs calls graceloader does not export (D-27)
*   worldstore.{c,h}      save slots, level.cmw (player + inventory), FatFs
                          enumeration, adopting the pre-slots Testworld (D-60)
*   chunk_worker.{c,h}    the core-1 task, queues, the ownership contract
*   chunk_render.{c,h}    per-section LOD cache, frustum cull, submission
  game/
*   tick.{c,h}            fixed step
*   replay.{c,h}          record / play the per-tick input (pure)
*   daytime.{c,h}         the clock: sun, sky, fog, the light table (pure)
*   physics.{c,h}         swept AABB against voxels                      (pure)
*   raycast.{c,h}         DDA block pick                                 (pure)
*   player.{c,h}          movement, mining, spawn/place/reset; health/hunger shown only
*   interact.{c,h}        break/place, drops, TREE FELLING               (pure)
*   input.{c,h}           se_bindings, the look abstraction, key names, swap-binding
*   hud.{c,h}             crosshair, block outline, hotbar, bars
*   flycam.{c,h}          the debug camera, on F
*   membench.{c,h}        what the memory costs, at boot (F-40)
    entity.{c,h}          the general pool; item_entity is its first case
    mob_*.c animal_*.c    one file per creature
  items/
*   items.{c,h}           ITEM REGISTRY; ids below BLK_COUNT are blocks  (pure)
*   inventory.{c,h}       slots, hotbar, stacking, the Tab grid          (pure)
*   item_entity.{c,h}     dropped items: pool, tick, despawn             (pure)
    recipes.{c,h}         RECIPE TABLE + resolver                        (pure)
  fred/
*   fred.{c,h}            PORTED showreel miner: the player's figure and arm
*   fred_mesh.{c,h}       his meshes, plus an axe and a shovel
  audio/
*   audio.{c,h}           the mixer's lifetime; footsteps, landings
*   sfx.{c,h}             THE EFFECT TABLE: one row per sound
*   music.{c,h}           which piece, and how long the silence before it
*   midi_seq.{c,h}        Standard MIDI File sequencer, PORTED from tadoom  (pure)
*   midi_synth.{c,h}      a voice pool over se_voice.h; GM families -> six shapes
  i18n/
*   i18n.{c,h}            T(id), the language, and a printf that reorders    (pure)
*   strings_gen.{c,h}     GENERATED from lang/*.txt by tools/make_lang.py
  ui/
*   title.{c,h}           "CraftMiner" in blocks, on a scratch world (D-58)
*   menu.{c,h}            every menu: title strip, then se_ui panels for slots,
                          new world, typing, settings, controls, pause
*   keybind_ui.{c,h}      PORTED from synthracer: a binding as a key cap or label
*   icons.{c,h}           PORTED from synthracer: the launcher's key-cap PNGs
*   settings.{c,h}        settings.txt on the SD card: language, graphics, audio, gyro, bindings (D-67)
* testkit/                wired into CMakeLists; PROF_HUD added (F-46)
assets/
* music/*.mid             the soundtrack, public domain (assets/music/MUSIC.md)
* music/manifest.json     where each file came from, and its hash
tools/
* worldcheck.c            host test of every pure module
* get_music.py            fetch/verify the soundtrack; refuses anything not PD
* symcheck.sh             every symbol we call, the loader can resolve (F-74)
  scenecheck.c            host budget test via synthengine3D/host/se_host_stub.c
* meshcheck.c             LIFTED + the sectioning check (D-34)
* hostpurity.sh           the pure set really is pure
* badgectl.py             ping / mode / exitapp
* make_textures.py        LIFTED
* textures/               the block PNGs
```

Two departures from the plan above, both deliberate: `hud.{c,h}` and
`input.{c,h}` live in `game/` rather than `ui/` and `input/`, because each is
one file that only the player uses and a directory holding one file is a
directory you forget to look in. They move the day a second file joins them.

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

`item_def_t` (name, label, stack_max, place_block, tool/level, durability, food
values, icon-or-NULL) follows the same shape. `recipe_t` was sketched here as
(w, h, in[9], out, out_n, shapeless, station) and **Part C replaced it with an
ingredient multiset** once the grid went away. `entity_def_t` is a type vtable (`tick`, `submit`, `interact`,
`on_death`) over a fixed `entity_t` pool, so adding a mob is one file plus one
row.

---

## Part W: chunk format and world dimensions

| | Value | Why |
|---|---|---|
| Chunk XZ | **16 x 16** | Keeps `VOX_CHUNK == 16`, so the donor `fill_fine`/`fill_coarse` and the mesher's greedy mask sizing are unchanged. |
| Chunk Y | **64**, one column, no vertical chunking (D-11) | The showreel's 32 is too shallow for mining plus build room plus a bedrock-to-sky Far Lands wall. 64 keeps the **single-column** layout, which is what lets a column stay contiguous for the mesher. Vertical chunking would save memory but break that for no gameplay gain at this scale. |
| Sea level | **24** | ~20 blocks of stone and caves below, ~40 above. |
| Coordinates | x, z `int32_t`; y `0..63` | The Far Lands edge (x = -2048) fits trivially. |

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
- **Every persisted duration is a count of TICKS ELAPSED**, never seconds and
  never a timestamp to compare against. See D-51: this is the rule for crop
  growth, furnace burn, breeding cooldowns and hunger as much as for a dropped
  item, and getting it wrong is only visible weeks later.
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
| Crafting | Distinct output + station per recipe; every ingredient, output and `drop_item` exists; every reversible recipe round-trips; discovery, auto-crafting and the search fold, each in its own row (Part C). **The old promise -- "resolves at every grid offset; no two collide" -- is retired with the grid it was written for**: nothing infers a recipe from ingredients any more, so a pickaxe and an axe are free to want the same three planks and two sticks. |
| Water | A pool, counted per material and per direction: the surface is one merged quad up and one down, the water has no side or bottom faces at all, the stone under it keeps its top face, and a pool with a block over it draws no water at all (D-86). |
| Menu labels | Every settings-row label, in all 32 languages, is narrower than the value column it sits beside -- measured with the engine's own glyph advances at the menu's row height (F-76). |
| Music | Every file in `assets/music` loads, has notes, and ENDS inside twenty minutes at the real sample rate; a rewind replays it identically; every truncation of it still terminates. Junk, a header with no tracks and an SMPTE division are all refused. |
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

**Redesigned 2026-09-22 by the user (D-78).** The first design -- a
west-only zone at x = -100000 behind a 4096-block ramp, with the look
imitated by hand (a biased wall, y quantised every 12 blocks, an 8:1 smear)
-- is dropped. What replaces it:

- **Near.** The edge is at **x = -2048**: about 8 minutes' walk west of
  spawn at walking speed (`PL_WALK` 0.215 blocks a tick, 20 ticks a second,
  4.3 blocks a second), and on a chunk boundary. Every column **west of**
  it (x < -2048, chunk x -129 and beyond) is Far Lands; chunk -128 is the
  last ordinary one.
- **Changeable later, per world.** The edge is a field of the world,
  `farlands_x` in level.cmw, written when a world is first created or
  opened by a build that knows it, from `FARLANDS_X_DEFAULT`. Changing the
  default changes NEW worlds only: an existing world keeps its edge, so its
  generated chunks and its ungenerated ones always agree and no seam is
  ever cut through a world someone is playing (D-78). It must be a multiple
  of 16. A world first opened before this existed has no such chunks west
  of it unless its player walked there; any that were generated stay as
  they are, ordinary terrain inside the Far Lands, like any saved chunk.
- **Sudden.** No ramp. East of the edge is ordinary terrain; from the edge
  on, every chunk is Far Lands. A cliff of jumbled terrain, as Kurt found it.
- **The real thing.** As close as possible to the Edge Far Lands of
  **Minecraft Beta 1.7.3** (the version of KurtJMac's *Far Lands or Bust*).

### Why they looked like that (minecraft.wiki, "Far Lands (Java Edition)")

Beta's terrain is a 3D density field: two 16-octave Perlin noises ("low"
and "high") blended by an 8-octave "selector" noise, minus a height
falloff, sampled every 4 blocks across and 8 up (5 x 17 x 5 samples a
chunk) and interpolated in between. Positive is solid.

Each Perlin octave casts its coordinate to a 32-bit int. The finest octave
of low and high advances 171.103 a block, which passes 2^31 at 12,550,824:
beyond it, Java's cast **saturates** at 2^31-1, so the "fraction" left over
is no longer 0..1 but enormous (about 10^11 one block in on the positive
side, 10^49 on the negative), and the smoothing polynomial EXTRAPOLATES it.
That octave's output then "completely dwarfs all other terms that would
normally give the terrain its shape" -- the height falloff included -- so:

- **the wall** runs from the bottom of the world to the top (Beta: y 127);
- **the holes** are long tunnels **perpendicular to the edge**: the
  overflowed axis always hits the same noise values, so the pattern does
  not change along it ("long unchanging tunnels", the "Swiss cheese wall");
- **water**: every air cell below sea level is flooded (about 23% of the
  Edge Far Lands is water; 36% stone, 25% air, 10% dirt and grass);
- **surface**: grass on top, dirt under it, sand and gravel near sea
  level; ordinary caves still carve it, "limited and small"; trees only
  high up, where there is sky;
- the edge starts three blocks early (12,550,821), because the 4-block
  sample spacing interpolates the overflow outwards.

### How we do it: run Beta's maths, broken the same way

Not an imitation. `world/farlands.{c,h}` (pure) **ports the Beta 1.7.3
density generator** -- the octave Perlin noise with its permutation
tables, the low / high / selector / depth noises, the 5 x 17 x 5 sampling,
the top slide and the interpolation -- in doubles, like Java, and feeds it
coordinates **past the overflow**:

    beta_x = x - FARLANDS_X - 12550821     (x <= FARLANDS_X, so beta_x <= -12550821)
    beta_z = z

so our edge IS Beta's west edge. Two things must be done by hand, because
C is not Java:

- **the saturating cast.** A C cast of an out-of-range double to int is
  undefined behaviour, not saturation. A `java_d2i()` that clamps to
  INT32_MIN / INT32_MAX exactly as the JVM does is the one line the whole
  effect depends on; the host tests pin it.
- **overflowing int arithmetic** in the noise (Java wraps) is done in
  uint32_t and cast back.

The permutation tables are seeded with **`java.util.Random`'s 48-bit LCG**
(a dozen lines), consuming it in Beta's order. That costs nothing and
makes the wall a given seed produces the wall Beta produced for that seed,
not just one like it.

**Fitting it into our world** (64 high, sea level 24, where Beta was 128
and 64):

- **Height: squashed 2:1.** The 17 vertical samples are spread every 4
  blocks instead of 8, so the whole Beta column -- wall top, tunnels and
  all -- fits in 64. Tunnels come out half as tall; the proportions of the
  face stay.
- **Water**: air below OUR sea level (24) floods. Beta's sea level was
  half-way up its world, ours is 3/8, so a little less of the face is water.
- **Surface**: Beta's own surface pass (grass on top, dirt below, sand and
  gravel beaches near sea level, bedrock at the bottom), run on the
  full-height Beta column before it is fitted into ours. **Bedrock and
  gravel** are new blocks for this (D-79); ordinary terrain does not use
  them yet.
- **Per chunk, not per column.** A chunk is either ordinary or Far Lands
  (the edge is chunk-aligned), so `worldgen_chunk` picks one generator;
  trees from the ordinary side may still lean over the edge.
- **Left out of the port** (for now): biomes -- every column is grass over
  dirt, and the temperature and humidity the density reads are constants,
  which in the Far Lands only touch the height falloff the overflow drowns;
  ice; and Beta's decoration pass -- its caves, ores, lakes, trees and
  flowers. Beta's sandstone becomes sand.
- Not reproduced: Beta's falling-sand lag, and the precision jitter (that
  needs millions of blocks, and D-01's floating origin prevents it anyway).
  Only the Edge Far Lands exist here -- one edge, west -- so no Corner Far
  Lands and no Farther Lands.

### Costs, and what to measure

- **Generation.** 5 x 17 x 5 = 425 density samples a chunk, each about
  40 octave evaluations (16 + 16 + 8) plus the 2D depth noise, and the
  surface pass's three noises over 256 columns. In doubles, which the
  badge does in software. **The fast path** takes low and high from their
  first octave only and skips the falloff's two noises: in the Far Lands
  the first octave is worth ~10^49 and the rest at most 2^15, so they
  cannot change a block -- and worldcheck proves it, block for block
  against the full port. **Measured on the badge (F-68): 134 ms a Far
  Lands chunk, against 33 ms for an ordinary one.** On the core-1 worker,
  so it costs loading time, not frame rate.
- **Meshes.** Expected to be heavy (a wall of holes) and measured to be
  light: the tunnels do not change along x, so nearly every face spans a
  chunk's width and the greedy mesher merges it whole -- about 200
  triangles a chunk. F-13's 40000-vertex assertion covers it in worldcheck.
- **The frame.** Walking up to the wall at Medium: 11.0 fps, no geometry
  dropped (F-68).

### Host tests (worldcheck)

- `java_d2i()` saturates at both ends and truncates toward zero inside;
  `java.util.Random` reproduces known Java outputs for a known seed.
- **The noise overflows where Beta's did**: the finest octave's integer
  coordinate is 2^31-1 (or INT32_MIN) at beta_x = -12550825, and ordinary
  one block east of the overflow point.
- **The cliff is sudden**: x = -2047 generates ordinary terrain (identical to
  worldgen without Far Lands); x <= -2051 has at least 90% of columns solid
  at y = 1 and near the top.
- **Tunnels run along x**: at fixed (y, z) inside the Far Lands, solidity
  stays the same for long runs of x; along z it changes often (>= 4 solid-air
  transitions per 256 blocks).
- **Composition** roughly as the wiki's (stone, air, water, grass/dirt), with
  generous bounds -- our squash and sea level shift it.
- **Asymmetry guard**: x = +2048 and z = +-2048 (and well beyond) are
  ordinary terrain -- the test against a sign bug turning the whole world
  into Far Lands.
- Determinism, and no chunk over 40000 vertices.

On the badge: `shots scene=farlands` looks at the wall; a `perf` run there
measures generation and frame cost.

### Signs at the edge

A few signs stand at random along the edge, on the ordinary side, facing
east -- towards whoever walks up to the wall -- with texts like **"Kurt
was here"** and **"Wolfie was here"** (Wolfie: Kurt's dog in the series).
Placed by the generator from the seed, so every world has its own and they
are the same every time.

For now signs are **generated only** (D-79): no sign item, nothing places
or edits one. The texts are a fixed list, so each is a ready-made texture
-- the planks with the lettering on it, drawn by `tools/make_textures.py`
-- and a sign's text follows from the world's seed and where it stands, so
there is nothing per sign to store. Player-written signs, when they come,
will need their text saved with the chunk (a tagged record, a format
addition).

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

## Part C: crafting, containers and the recipe book

Designed with the user on 2026-09-23, before step 8 started, and almost every
line of it departs from Minecraft on purpose. Their brief opens with the
reason: *"Placing stuff in a grid like the original minecraft seems a bit
tedious and pointless (especially since we only have a keyboard but no mouse or
touch screen). Let's use a recipe book instead."*

The badge has a keyboard and no pointer. A 3x3 grid is a pointer interface
wearing a keyboard costume, and everything below follows from not building one.

### A recipe is a multiset, not a grid

```c
typedef struct { uint16_t item; uint8_t count; } ingredient_t;

typedef struct {
    uint16_t     out;      uint8_t out_n;
    uint8_t      station;  // RS_INVENTORY / RS_TABLE / RS_FURNACE
    uint8_t      flags;    // RF_REVERSIBLE
    ingredient_t in[RECIPE_IN_MAX];  uint8_t n_in;
} recipe_t;
```

No `w`, no `h`, no `in[9]`, no offsets, no shapeless flag. Part L sketched
`recipe_t (w, h, in[9], out, out_n, shapeless, station)` back when step 8 meant
a grid; with a recipe book the shape is not merely unused, it is **unobservable**
-- there is no surface on which a player could ever see or express it.

**This retires a rule this document has carried since the first week.** Part H
promised the host check proves *"every recipe resolves at every legal grid
offset; no two collide"*. In Minecraft a pickaxe and an axe are both three
material and two sticks, and only the shape separates them, so with a grid that
rule is load-bearing. The user asked for Minecraft's resource counts, so the
collision is real and arrives with the first tool.

It does not matter, because **nothing in CraftMiner ever infers a recipe from a
pile of ingredients.** Forward, the player names the recipe. Backward, the
disassembly bench starts from the item, which is equally known. The requirement
that replaces it is the one that is actually true: distinct output *and*
station per recipe, every ingredient and output exists, every reversible recipe
round-trips.

Quantities are Minecraft's, as asked.

### Discovery is stored as every item ever held

The user's rule: a recipe appears in the book once the player has picked up at
least one of the materials it needs. Stored **the other way round** -- a bit per
item, set by `inv_add`, and a recipe is known when any one of its ingredients'
bits is set.

Three reasons, and the first is the one that decides it:

1. A recipe bitmask would make **recipe numbering permanent**, the way block ids
   are permanent (D-74, `tools/ids.txt`), and every recipe inserted in a later
   build would need a migration or would silently shift what every existing
   player knows. Item names are already the thing saves key on -- inventories,
   dropped items and replays all store them.
2. A recipe added by a later build is then correctly **already known** to a
   player who has handled its ingredients, rather than locked behind materials
   they have had in a chest for a month.
3. It is smaller, and it is a more meaningful thing to have in a save file.

Saved in `level.cmw` as a list of item names, beside the inventory.

### Three stations, and the furnace has three slots

`RS_INVENTORY` is the short list the user specified -- planks, sticks, torches,
a crafting table -- reachable from the Tab screen anywhere. `RS_TABLE` is
everything else. `RS_FURNACE` is smelting.

**The furnace was designed twice.** The first version here was a recipe book
filtered to smelting: pick *Iron ingot*, it queues the work, walk away. The
user replaced it the same day with the real thing -- *"The furnace has three
slots (input, output, fuel). When selecting the output slot, the item(s) go
into our inventory, for input and fuel slots the inventory pops up so we can
select an item stack to put into that slot."*

They were right, and the reason is worth keeping: a recipe book answers *"what
can I make"*, and that is not the question anybody has in front of a furnace.
The question there is *"what is in it and what is it doing"*, and three rows
answer it at a glance where a filtered book never could.

**The picker is what makes it work without a pointer.** Nothing is ever
dragged: selecting Input or Fuel opens a list of the player's own stacks,
narrowed to what fits the slot, with a right-hand column saying what each one
would BECOME (*makes Glass*) or how far it would go (*smelts 8*). That column
is a thing a mouse-and-grid furnace cannot tell you at all, which makes this
better than the interface it replaces rather than a substitute for it.

**Fuel is anything that burns**, as asked -- logs, planks, sticks, wooden tools,
and coal. It is a column in the ITEM table (`item_def_t.fuel`, in ticks), so a
new wooden thing brings its burn time with it instead of needing a line in the
furnace.

**Wood becomes coal, not charcoal** (the user's simplification): one fewer item
that burns exactly like another one.

### Reversal is one bit, not a special case

*"Doesn't work for basic resources like iron ingots turning into ore or sticks
turning into planks"* is `RF_REVERSIBLE`, a column in the table. Tools, the
crafting table, the chest, the furnace, the disassembly bench and the trashcan
carry it. Log-to-planks, planks-to-sticks, torches and every smelt do not.

A worn tool returns its **full** ingredient list (the user's call, asked
explicitly). Salvage-and-recraft is therefore a repair that costs the one-time
coal in the bench's own recipe, and that is the intended price.

### Lazy time: the furnace and the trashcan never tick

The user's rule for the trashcan -- *"Items in it expire (get deleted) after 10
in-game minutes (calculated the next time we open it)"* -- is the right
mechanism for the furnace as well. Both do their arithmetic from
`now - last_touched` **when opened**, and store nothing but that stamp.

No per-tick furnace list, no catch-up pass, no work at all for a furnace in a
chunk nobody has visited -- and it is automatically correct across a chunk being
evicted and reloaded, a world being closed for a week, and the clock being
stepped by the N key. Crop growth in step 9 should use the same trick wherever
it can.

Ten in-game minutes is **12000 ticks of playing** (the user's call: not the
in-world clock, where a minute is a sixtieth of a 20-minute day and ten of them
would be three seconds).

### Containers need the block-entity section, which has never been written

`SECTION_BLOCK_ENTITIES` has been reserved in the chunk format since Part W was
written and **nothing has ever put a byte in it**: `region.c` calls
`chunk_encode(c, NULL, 0, ...)`. A chest is what finally needs it, and so
`world/blockent.{c,h}` is the real cost of step 8.

A **fixed global pool keyed by world position**, not a list allocated per chunk,
because the decode runs on the core-1 worker and Part K's contract is that the
worker does not allocate. A chunk's save walks the pool for cells inside it. The
pool is bounded, and placing a container when it is full fails with a message
rather than losing one quietly.

Steps 11, 12 and 13 all land on the same machinery: a crop's stage fits in the
state byte, but a cow does not.

### The search box, on a keyboard that has one alphabet

The user's catch, and it would have broken the feature in 25 of the 32
languages: **the Tanmatsu has one fixed QWERTY**, so a player reading
*Кирка* cannot type К -- and it is not only the non-Latin scripts, since Turkish
*Kömür*, Polish *Łopata* and Czech *Dřevo* are equally untypable.

So the filter never matches what is on the screen. It matches a folded form of
it: `i18n/fold.{c,h}`, UTF-8 in, lowercase ASCII out, applied to **both** sides.
Accented Latin folds to its base letter, Cyrillic and Greek transliterate
(`Кирка` -> `kirka`, `щ` -> `shch`, `χ` -> `ch`). The item's **stable English
name matches too, always**, so `pick` works in every language -- which is also
the way out for someone who sets the language to Greek by accident and cannot
read their way back.

One letter, not a digraph: `ö` -> `o`, the user's call. A German would type
*loeffel* and a Turk *komur*, only one of those can win, and the single base
letter is right for more of the 32 than the digraph is.

**The check is what makes the table trustworthy**, and it is the same shape as
the font's "every character of every string is drawable" from 6.5: worldcheck
asserts the fold **covers the whole domain** -- every character occurring in any
item name in any of the 32 languages has an entry, and every entry lands in
ASCII. A translation using a letter the fold does not know fails `make check` on
the machine that builds it, not on a card where a recipe simply cannot be found.

### Tools: three tiers, and a wrong tool that is actually slow

*"Mining with the wrong tool is a lot slower."* It was not, and no hardness
number has to change to fix it: the right tool divided the time by
`tool_level + 1`, which is 2x for wood and 3x for stone -- a rounding error next
to swinging a fist. It becomes `2 x tool_level`: hand 1x, wood 2x, stone 4x,
iron 6x. A stone block is 7.5 seconds by hand and 1.25 with an iron pickaxe.

Iron ore **refuses to break** without a stone pickaxe (the user's call, over the
existing breaks-but-drops-nothing rule, which is Minecraft's). That needs
`BF_TOOL_REQUIRED` and it needs to *say so* on the HUD, because a swing that
does nothing at all and explains nothing reads as a bug -- which is the entire
reason Minecraft chose the other rule.

### What the host checks prove

| Check | What it proves |
|---|---|
| Recipe table | Distinct output + station; every ingredient and output exists; every reversible recipe round-trips to its own ingredient list; no recipe needs more than `RECIPE_IN_MAX` kinds. |
| Discovery | An empty set knows nothing; picking up one ingredient reveals exactly the recipes naming it; the set survives a save and a reload by name. |
| Auto-craft | A plan for a target the player cannot make directly is found, executed, and leaves the inventory exactly as the plan said; a target that is genuinely unreachable is refused without consuming anything; depth and cycles are bounded. |
| The fold | Covers the whole domain (above); folding is idempotent; a folded needle found in a folded haystack matches what a player would expect for a sample of each script. |
| Containers | A chest survives encode, evict, decode; the pool full is refused, not lost; a trashcan's expiry is computed from the stamp and not from when it was opened; a furnace's queue completes across a reload. |

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
| 3.6 | Aiming: crosshair and the block highlight | done | 2026-09-21: added after the user reported aiming was guesswork (F-44). The crosshair is derived from `RENDER_HORIZON_Y`, **not** the middle of the screen — the two differ by 16 rows and the difference is an aiming error nobody would think to suspect. Verified by pulling the framebuffer off the badge. Found and fixed a picker bug on the way: `BLK_BARRIER` was reportable, so an unloaded chunk could be highlighted and offered for mining. |
| | **Accept host:** collision fuzz, DDA against brute force, felling tests | **done** | 2026-09-21: all in `make check`. Collision over hand-built fixtures (rest, terminal-velocity tunnelling, wall slide, step, staircase, 2-block wall, 2-high and 1-high gaps, world edge) plus the jump arc asserted in blocks and seconds; DDA against a brute-force march over 576 directions; felling both ways with the neighbouring tree and the stump checked. |
| | **Accept device:** `shots scene=replay_walk` gives **identical hashes across two runs** — the proof Part T works | **blocked** (F-45) | The replay scene needs the input stream stored (block 5's save format) and `shots` needs the synchronous chunk mode switched on, or every shot is empty sky. Both carried into block 5. |
| **4** | **Items** | | |
| 4.1 | `items.c`, `inventory.c`, merged-stack drops, `item_entity.c` | done (not persisted) | 2026-09-21: **item ids below `BLK_COUNT` ARE block ids** (D-53), so a stack of cobblestone needs no second table and the inventory can draw a block the block registry already describes. Stacking fills partial stacks before empty slots — host-tested, because the other order silently gives you five slots of three cobblestone. Drops spawn from the block table's new drop column, scatter deterministically, fall, and fly to the player. **Age is elapsed ticks** (D-51), despawn asserted at exactly 12000. **Not saved yet**: `SECTION_ENTITIES` is reserved and nothing writes it, so drops are lost on reload — block 5. |
| 4.2 | Tool durability; hotbar (F1-F6) and the inventory screen (Tab) | done | 2026-09-21: breaking is **held, not tapped** — `item_break_ticks()` of them, so hardness and the tool in hand finally mean something, and progress belongs to a cell so looking away abandons it. The right tool class only: a pickaxe does not dig dirt faster. Too soft a tool still breaks the block and drops nothing. One use of durability per **break**, so felling a tree is one swing of the axe, not forty. Tab opens the grid; F1-F6 swaps a stack onto the hotbar, which is the one operation without which everything past the sixth slot is unreachable. |
| 4.3 | `hud.c`: crosshair, hotbar, health, hunger | done | 2026-09-21: hotbar with counts and a wear bar, ten hearts, ten drumsticks, and a mining progress bar. **Tools are drawn as shapes, not colours** (D-54) — three stone tools as flat squares are three identical grey squares. Drawn through `se_direct565.h`, not PAX: that is the difference between **13.4 ms a frame and 0.8** (F-46). Health and hunger are displayed but nothing moves them yet; the systems are block 12. |
| | **Accept:** the item registry, stacking, durability and drops are host-tested; breaking yields what the block table says and only to a tool that qualifies | **done** | 2026-09-21: in `make check`. Tool speed by class, harvest qualification, partial-stack filling, a full inventory refusing the overflow, two differently-worn tools staying two, a wooden pickaxe lasting exactly its durability, drops appearing and being collected only after `ITEM_PICKUP_DELAY`, despawn at exactly 12000 ticks, felling dropping every block it takes, and a full entity pool refusing rather than corrupting. |
| **5** | **First playable — worlds on the SD card** | | |
| 5.1 | `screens.c`: title -> world list -> new world (name + seed, or rolled) -> play | done | 2026-09-21: the **title** is the showreel's idea rebuilt on a streamed world (D-58). 2026-09-22: **Play / Settings / Quit** along the bottom of it, under the word; Play opens the **eight save slots** (D-60), a used slot offers Play / Rename / Delete, an empty one the new-world form — name, and a seed that is a number, any text (hashed), or blank for random. All in `ui/menu.c`, drawn with the engine's list menu, which has no scrolling of its own, so long lists window round the cursor. |
| 5.2 | Save policy: chunk unload, pause-menu Save, quit. Never per tick | done | 2026-09-21: `save_world()` writes the player and every edited resident chunk; eviction already saved. 2026-09-22: saves on **opening the pause menu** (D-61), on its Save row, and on Save and quit. Nothing saves on a tick or a timer. |
| 5.10 | **Player position and inventory in the save** | done | 2026-09-22, asked for by the user. The inventory is stored **by item name** (D-62) — each slot's item, count and wear, plus the selected slot. The position is restored **exactly** (`player_place`), falling back to standing on the ground only if the body would not fit there; before this a returning player was always put on the surface, whatever cave they had left from. A `placed` flag tells a real position from a new world's spawn guess, with a rule for saves from before the flag (F-51). Host-tested: a worn pickaxe, a partial stack and the last slot round-trip; a position 11 blocks down comes back as 11. |
| 5.11 | **Adopt the pre-slots Testworld** | done | 2026-09-22, asked for by the user: people are already playing. The one world earlier builds kept, `worlds/flyover`, is moved by **one directory rename** into the first free slot and renamed *Testworld*. Nothing is copied, so nothing can be half-copied; a card that never had it is left alone, and a second start finds nothing to do. Host-tested against a hand-written level.cmw in the old format, with a chunk in it: terrain, the placed block, the player's exact position and the seed all survive. **Not yet run on the badge** — it was unreachable when this was written. |
| 5.7 | **Entities in the save** | done | 2026-09-22: every dropped item goes into level.cmw with the world (D-68) -- by item name, with count, wear, age and what is left of its pickup delay -- and comes back on opening. An item whose chunk is not loaded holds still (no falling, ageing or pickup), which keeps D-33's promise without per-chunk entity sections. Host-tested: a round trip, and an item in an unloaded chunk neither falling nor ageing. |
| 5.8 | **Replay record/play**, and synchronous chunks for the `shots` test | done | 2026-09-22: `game/replay.{c,h}`. R (a debug key, unless bound) records from where the player stands to `replays/last.cmr`: the start (seed, clock, position, inventory) and each tick's action mask and gyro turn. The `replay` and `replay_third` test scenes play `replays/test.cmr` (else `last.cmr`) on a SCRATCH world of that seed, so no save is touched; under a test the replay runs exactly the ticks that belong to the set clock. Two ordering bugs found making it deterministic (F-59, F-60). Host-tested round trip; on the badge a scripted replay placed a torch at night and mined, photographed in both views. Earlier notes: | 2026-09-21: the synchronous half is done (D-59) — a `shots` run switches the worker inline and settles the world, so a shot photographs the world instead of the sky, and three captures of one instant now hash identically. **Replay record/play is still not written.** Carried in from 3.3, was blocked on F-45. A `shots` run SETS the clock rather than running it, so the chunks never stream and every screenshot is empty sky — the fix is the synchronous mode D-15 put there for exactly this. Until both exist, block 3's device accept line cannot be met and shot hashes cover the overlay but not the world. |
| 5.9 | Move `time_of_day` from the player record to the world (D-52) | done | 2026-09-22: `world_meta_t.time_of_day`, advanced one per simulation tick and never by the wall clock. A save from before reads the player's copy instead (host-tested with a hand-written old save). New worlds start at a morning. |
| 5.5 | **Pre-generate and save the spawn area on world creation**, behind a "Creating world" progress bar (D-25) | done | 2026-09-22: an `APP_LOADING` state generates a 60 ms slice a frame and draws "Creating world" / "Loading world" with the world's name and a bar, for the title at boot too. Under a deterministic test it loads in one step, and the frame that finishes carries straight on (F-60). |
| 5.6 | **The entering sequence** (D-26): physics frozen, 3x3 synchronous, play, then stream the rest | done | 2026-09-21: the tick is frozen until the chunk under the player is resident, and the spawn area is pre-generated before play starts. The "carry on streaming the rest while walking" half already worked. **2026-09-23: the gate itself.** Everything else had been built and `loading_step()` was still asking the wrong question -- it waited for `missing == 0`, the whole view distance, which is the one thing D-26 was written to avoid. Now `chunk_render_nine()` counts the nine chunks around the player and play starts at nine, with the rest streaming in behind them. Creation still waits for all of it (it pre-generates and SAVES the spawn area, 5.5), as do the title, the debug flight and a replay, and `devtest_deterministic()` forces the old behaviour so `shots` is untouched. The progress bar measures the nine it is waiting for rather than crawling across the view and jumping. |
| 5.3 | Pause menu; `f1_exits = false`; quitting saves first | done | 2026-09-22: Resume / Save / Settings / Save and quit to title. Opened by the Pause binding **or Esc, always** (D-63); Esc closes the inventory first if it is open. |
| 5.4 | `worldlist_ui.c` with index + FatFs rebuild; delete a world | done | 2026-09-22: the slot list reads each slot's level.cmw when it opens (eight file opens, not per frame). Delete asks first with **No** under the cursor, and removes the directories as well as the files, so the slot is really free. `worlds.idx` is still unneeded (1.4). |
| | **Accept:** a scripted device test creates a world, edits 200 blocks across 3 chunks, saves, reloads, and reports whether every edit survived. **-> hand to the user** | **done** | 2026-09-22: the `savecheck` scene (`make cycle TEST="perf scene=savecheck secs=3"`), in a hidden world deleted afterwards. **200 of 200 edits survived** on the badge. The test kit gained `devtest_content_failed()` so a miss ends the test "bad". |
| **6** | **Settings** | | |
| 6.1 | Controls menu, the synthracer pattern, all actions rebindable | done | 2026-09-22: all 21 actions, plus Reset to defaults. Binding a key another action has **swaps** them, so no two actions share a key and none is left with none. The capture is the engine's `se_ui_capture_key`, fixed to take the cursor keys (F-50), and the key column is synthracer's `keybind_ui.c` with its key-cap icons (`ui/icons.c`), both ported with provenance headers. The main menu is a strip under the title (the user kept it); every other screen is an `se_ui` panel. The polled fallback for keyboards that send arrows as navigation events now follows the binding rather than the action, and the debug keys stand aside for any key a player has bound (D-64). |
| 6.4 | **Gyroscope look** | done | 2026-09-22, asked for by the user: a Controls checkbox, off by default. The **rate** gyroscope is added up frame by frame and handed to the look beside the cursor keys, one real degree per view degree, so both work at once (D-65). A resting gyroscope's offset is tracked rather than turned into a slow spin. Yaw sign as the diagram in `graceloader_imu.h` predicts; the **pitch sign had to be flipped**, reported by the user on the badge. |
| 6.2 | Graphics menu: textures, render scale, view distance; settings.txt | done | 2026-09-22: view distance (default near -- medium for a few hours, D-66 then D-76), textures, half / full resolution, in `settings.txt` on the SD card (D-67; NVS under `craftminer` at first, moved the same day). Replaces the T and V debug keys. Full resolution clears its own sky now, which it never had to while it was only the no-PPA fallback. |
| 6.3 | Audio and display via `se_hw.h` | done | 2026-09-22: device volume and the three brightnesses through `se_hw` (shared with the launcher); music and effects switches stored and wired to the mixer's gates, and labelled as waiting for block 14, since the game makes no sound yet. |
| 6.5 | **The UI in six languages** (D-81) | done | 2026-09-23, asked for by the user. `lang/*.txt` (English the reference, plus German, Dutch, Flemish, French, Bulgarian), baked by `tools/make_lang.py` into `main/i18n/strings_gen.c`: 126 strings x 6, a lookup is an array index. Language is the first row of Settings, each named in its own language, stored in settings.txt; a player with no toolchain can correct any line from `/sd/craftminer/lang/<code>.txt` on the card. The font was the work, not the text (F-69): the engine drew ASCII only, and now draws Cyrillic, accented Latin, both dashes and the European quotation marks, generated from Hershey's own database with a check that every letter of every declared alphabet exists. `i18n_fmt` does its own `%2$s` substitution and takes the argument types from English, so an edited lang file cannot mislead it (F-70). `make` regenerates the tables whenever a lang file changes -- an ordinary make rule with known inputs, so nobody has to remember a second command -- and the generator validates while it generates (keys, placeholders, no word mixing two alphabets). `make langcheck` is the CI form, asking whether what is committed is up to date. worldcheck's "languages" section covers the rest: every character drawable, the formatter against six nasty strings. On the badge: the language list, Settings in Bulgarian, Controls in German. The language list first drew tick boxes, which the user rejected -- one choice out of many is a radio button -- so the engine gained `SE_MENU_VAL_RADIO` (their call to add it there rather than work round it). **Then 26 more languages, the user's call after asking what was missing** (F-71): 32 in all, English first and the rest alphabetical by the name each calls itself. The font grew seven accents (caron, breve, double acute, macron, dot above, ogonek, comma below), Greek out of Hershey's own SIMPLEX face -- the same weight as the Latin, which the Cyrillic never was -- and a dozen letterforms nobody can compose. 126 strings x 32 = 4032, every character of every one of them drawable -- 128 once step 14 added the volume sliders, and a label-width check came with them (F-76). |
| | **Accept:** every menu reached on the badge, a key rebound and used, a world created, played, saved, reopened with its inventory; the Testworld adopted | **in progress** | 2026-09-22: the Testworld adoption **ran on the user's card** — `worlds/flyover` became `slot1`, named *Testworld*, all five region files with it (a copy of the original is kept off the badge). The title strip renders (screenshot). The user is testing the rest by hand: the gyroscope works after one sign flip (F-55), and the inventory cursor bug (F-54) was found that way. `make check` covers slots, the inventory round trip and the adoption. |
| **7** | **Far Lands** | | |
| 7.0 | **Bedrock, gravel, and generated signs** (D-79) | done | 2026-09-22, asked for by the user for the Far Lands: bedrock (unbreakable) and gravel (shovel, drops itself) as blocks 17 and 18; a sign, block 19, a post with a board facing east (`K_SIGN`), not solid, breakable with nothing dropped. Its text -- "Kurt / was here", "Wolfie / was here", "Far Lands / or Bust!" -- is one of three 64x32 textures drawn by `make_textures.py` with its own 5x7 pixel font (so the PNGs do not depend on PIL's fonts), chosen by a hash of where the sign stands (`voxel_sign_text`). Existing textures regenerate byte-identical. |
| 7.1 | `farlands.c`: Beta 1.7.3's density generator, ported, fed coordinates past its overflow; the edge at x = -2048, sudden, stored per world (Part X, D-78) | done | 2026-09-22: redesigned by the user -- near spawn and a sudden cliff, as close to Beta's Edge Far Lands as possible, instead of a ramp at -100000. Built the same day (F-68): java.util.Random and Java's saturating cast ported, NoiseGeneratorPerlin / Octaves and ChunkProviderGenerate's density, terrain and surface passes in doubles; Beta chunk -784428 onwards is our chunk -129 onwards. `farlands_x` in level.cmw; `chunk_worker_set_world(seed, farlands_x)` replaces set_seed. Composition 42% rock / 30% air / 19% water / 9% dirt and grass against the wiki's 36 / 25 / 23 / 10; tunnels run west (98.7% of neighbours alike along x, 87% along z); ground 27 high at the edge, the wall 61 one block on. A `farlands` test scene walks up to the wall. 134 ms a chunk on the badge. |
| 7.2 | Signs along the edge: "Kurt was here", "Wolfie was here" | done | 2026-09-22, asked for by the user; signs themselves are 7.0. One chunk in four of the last ordinary chunk column gets a sign, 0-2 blocks from the edge, on dry ground, facing east: 26 along 2048 blocks of edge in worldcheck's seed. |
| | **Accept:** the far-lands host section; `shots scene=farlands` for a look; generation and frame cost measured at the wall | **done** | 2026-09-22: worldcheck's "far lands" section passes; shots of the wall from 30 and 12 blocks on the badge; 134 ms a chunk, 11 fps at Medium walking up to it. Left for the user: a look at it in play. |
| **8+** | **The game** | | |
| **8** | **Crafting: a recipe book, containers, iron** (Part C) | | 2026-09-23: designed with the user, who replaced the grid with a searchable recipe book. Delivered in stages, their call, so the book's feel can be judged before the rest is built on it. |
| 8.1 | `items/recipes.{c,h}`, the discovery set, `i18n/fold.{c,h}`, inventory crafting and the book | done | 2026-09-23: recipes as ingredient multisets; discovery stored as every item ever held; the search box folding 32 languages onto one QWERTY (F-78). Three fixes straight from the user's first play: an empty pack on a new world, the opening keystroke no longer lands in the search box, and a recipe that says "missing" now opens a panel saying what is missing. Then four more from the second: a crafting table shows the inventory recipes too (one-way), Tab no longer opens the inventory behind an open screen, and two lines that ran off the screen (F-81). And F-80, which was two reports and one bug. |
| 8.2 | The crafting table and the wood and stone tools | done | 2026-09-23: block 20, its two textures, and the seven recipes. `BF2_USABLE` -- the Use key OPENS a block rather than placing against it, and which blocks do is a registry flag while what each one opens is main.c's business. **Tool speeds fixed**: the right tool divided by `level + 1`, so wood was 2x a bare fist and stone 3x -- the user's "mining with the wrong tool is a lot slower" simply was not true. Now `2 x level`: hand 1x, wood 2x, stone 4x, iron 6x, and no hardness number had to move. Iron moved out to 8.4, since ingots need the furnace. |
| 8.3 | `world/blockent.{c,h}` -- the chunk format's unused block-entity section -- and the **furnace** | done | 2026-09-23: pulled forward from 8.4 the moment the user found that iron needs smelting and coal needs finding, so a furnace early is what makes wood into fuel. `world/blockent.{c,h}`: a fixed pool of 192 records keyed by world position, written into `SECTION_BLOCK_ENTITIES` -- **a section the format has had a number for since Part W and never had a byte in**. `game/furnace.{c,h}` never ticks; it catches up from `now - stamp` when opened, in a loop that runs once per EVENT rather than once per tick (4 billion ticks in 0.01 ms, host-measured). Smelting: log to coal, sand to glass, cobblestone to stone -- the last two unasked for, but glass and stone had no way of being obtained at all. Two real catches on the way, both in F-77. |
| 8.4 | Chests, the trashcan, the disassembly bench, iron, auto-crafting | done | 2026-09-23. **Chests and the trashcan** are the block-entity pool's second and third customers, and cost almost nothing on top of the furnace: a record with 24 slots and no timers. Their screen is two grids side by side -- Tab swaps sides, enter moves a stack -- and the trashcan is the SAME screen, emptying by the same lazy clock the furnace runs on (`blockent_rot_trash`, one stamp for the bin, refreshed when anything goes in). **The bench** takes apart anything whose recipe carries `RF_REVERSIBLE`, giving back the full ingredient list however worn the tool -- the user's call, so salvage-and-recraft is a repair priced at the one coal in the bench's own recipe. **Iron** is block 22, deeper and rarer than coal, and the first block that REFUSES the swing (`BF2_TOOL_REQUIRED`) -- with a line on the HUD naming the tool it wants, because a swing that does nothing and says nothing is a bug as far as anyone can tell. **Auto-crafting** is Tab in the book, and F-82 is the part worth reading. |
| 8.6 | The 8.4 screens in all 32 languages | done | 2026-09-23: 24 more keys each. Three languages spell "disassembly bench" wider than the column that holds an item name (Portuguese, Greek, Bulgarian) and were shortened rather than the column widened -- it is already the widest layout in the game. F-83. |
| 8.5 | Item names and the crafting UI in all 32 languages | done | 2026-09-23: 63 keys x 31 languages -- every block and item a player can carry, the crafting book, the furnace and its picker. **Six overflowed and the check caught all six** before the badge did (French, Irish, Albanian, Greek, Bulgarian, Serbian), and widening the two crafting panels to hold them exposed something nothing had been measuring: **the book's row labels are ITEM NAMES**, and Russian "Деревянная лопата" is half as wide again as "Wooden shovel". `item.` joined `LABEL_COLUMNS`, the panels went to the wide layout, and the fold table grew to cover 82978 characters across the 32 languages. |
| 9 | Farming: tilled soil, wheat, carrots, seeds, saplings, growth on the tick | todo | |
| 10 | Cooking and the hunger loop | todo | |
| 11 | Animals: pigs, cows, chickens; breeding; dogs (wild, tamed with steak) | todo | |
| 12 | Mobs: zombies, skeletons, spiders; spawning, pathing, combat; beds and spawn; death keeps the inventory | todo | |
| 13 | Fishing | todo | |
| 14 | **Audio: sound effects, and music that is mostly silence** (D-82, D-83, D-84, D-85) | done | 2026-09-23: the mixer starts at boot. 21 effects as table rows (`audio/sfx.c`), and which one a block makes is its own registry row (`block_def_t.sound`), so a new block brings its sounds with it. Music is eleven public-domain MIDI files played by a ported sequencer and a six-shape synth: 72 KB for half an hour, against megabytes for the same music as MP3. `worldcheck`'s `check_midi` proves every shipped file parses, ends, rewinds identically and survives truncation at any length (F-72). The raw voice sum clipped, so the synth carries a master gain and a cubic soft limiter (F-73). **Three volume sliders** in Settings -> Audio (D-85): the badge's own, then how loudly the music and the effects are each mixed in. The effects turned out to be inaudible whenever the music was off -- the amplifier was asleep and eating them (F-75) -- which is why the engine is 2.2. Two checks came out of the round and stay behind: `tools/symcheck.sh`, after an unexported `strcasecmp` made the app link clean and then refuse to start with no message at all (F-74), and `check_label_widths()`, after the sliders' labels turned out to be the least of it -- three settings screens had been overlapping their own text in a dozen languages since the day the language count went to 32 (F-76). |
| 15 | Block and sky lighting | done | 2026-09-22, asked for by the user (torches that light the area, computed when a block changes). Pulled forward from the end of the plan: a light plane per chunk (sky and block light, 0..15 each, D-69), flooded when a chunk arrives -- its own light on core 1, the border exchange on the main task (F-58) -- and updated with the two-queue flood on every block change. The mesher keys faces on light; a per-frame table turns light into brightness for the time of day, through the engine's new `SE_TRI_LIGHT`. Host-tested: fall-off, removal, a shaft opened and capped, across a chunk border and into a chunk arriving late. On the badge: a placed torch lights the ground at night. |
| 16 | **Day and night; sun, moon, clouds, stars** | done | 2026-09-22, asked for by the user. `game/daytime.{c,h}`: a 20-minute day from the world's tick count -- sun direction, sky and fog colours with an orange band at sunrise and sunset, a daylight fraction, the light table. The showreel's blocky sun, moon and clouds (`voxel/voxel_sky.c`, clouds laid out in world coordinates) and its starfield. Night takes 9 light levels off the sky, not Minecraft's 11 (F-57). Clouds are a Graphics toggle; the title has none (they flew through its letters). |
| 17 | **Fred: first-person arm, held item, third person** | done | 2026-09-22, asked for by the user: the showreel's miner, ported as `fred/fred.{c,h}` and `fred_mesh.{c,h}` with provenance, given an axe and a shovel beside the pickaxe, blocks in their own textures, flowers as sprites and a torch as its stick. Lit by the cell he stands in. First person draws the arm at a third of the showreel's distance and size (D-70). Third person (Settings -> Graphics -> Camera) puts the camera 4 blocks behind his eyes, pulled in by a ray when something is in the way; the crosshair is hidden there, the block outline kept. |
| 18 | **Position overlay** | done | 2026-09-22, asked for by the user: a new action, `Show position`, Backspace by default and rebindable, toggles coordinates, compass heading (D-71), the world's clock and day, and replay status. |
| 19 | **Torches could not be taken back** (bug) | done | 2026-09-22, reported by the user (F-56). |
| 20 | **Terrain vanishing; "walking through" steps** (bug) | done | 2026-09-22, reported by the user as critical: at a step the view went into the ground, and looking round left holes. **Not physics** -- the same collision code walked the user's own terrain on the host for 6400 ticks without once ending inside a block, and the badge log showed the player climbing. It was the engine's geometry lists overflowing and DROPPING triangles, which the engine never reported (F-63). Caps raised, the line list shrunk to pay for it, far meshes' light rounded, the engine's drop counter fixed (D-72). The same walk now peaks at about 3000/6144 flat and 1900/4096 textured, nothing dropped -- at **7-9 fps** at the medium view. |
| 21 | **Fred's hand**, and the handedness bug | done | 2026-09-22, reported by the user: first person held things in the right hand, third person in the left (F-64). Now a Graphics setting, right by default, and both views follow it. Checked on the badge mid-swing, both ways: right-handed the pickaxe rises by his right shoulder, left-handed by his left. |
| 23 | **Permanent block ids; slots that say why they cannot be opened** | done | 2026-09-22, the user's call after F-52 was explained (D-74, D-75). Every block numbered explicitly; `tools/ids.txt` lists every block id and name and every item name ever shipped, and `make check` fails on a renumbered, renamed, removed or unlisted one -- each of the three cases tried and caught. Replays now store their inventory by item name (format 2). The slot list tells a world from a newer build ("from a newer version"), an older format ("needs upgrading") and a damaged one apart from an empty slot; nothing can be created over any of them. The upgrader itself is left until there is a format change for it to do. |
| 24 | **Lighting's triangle cost, and the frame rate** | done | 2026-09-22, the user asked why the frame rate had fallen so far. Measured, not guessed: on the same scripted flight as 2026-09-21, today's build is as fast as yesterday's (14.5 vs 14.6 fps with lighting and clouds off, 14.1 with everything on) -- **no regression** (F-66). What had changed is the scene and the setting: walking at eye height puts close-up textured ground over the whole screen (rasterize 46 -> 72 ms), and the user plays at Far. Light in the merge key rounded in the near meshes, sky to every 4th level and torch to every 2nd (F-65): 25% fewer near triangles with torches about, +4% fps on the walk. Default view back to near (D-76). The `flight` and `replay_*` test scenes make these comparisons repeatable. |
| 22 | **N: step the clock** | done | 2026-09-22, asked for by the user: a debug key moving the world's clock a quarter of a day, for looking at night without waiting. |
| 27 | **The player's data out of the app's directory** | done | 2026-09-22, the user's catch (D-80): worlds, settings.txt, replays and screenshots move from `/sd/apps/at.cavac.craftminer` to `/sd/craftminer`, which the launcher does not manage. `world/datadir.c` moves what an earlier build left there on the first start -- a rename per entry, never over an existing one, nothing on a second start -- host-tested. Test-kit shots go to `/sd/craftminer/test`. |
| 26 | **Screenshots** | done | 2026-09-22, asked for by the user: a new action, `Screenshot`, **0** by default and rebindable, saves the frame as the player sees it (HUD included) to `/sd/craftminer/screenshots/shotNNN.png` with the test kit's PNG writer; a "Saved ..." line shows for 2.5 s on the frames after, so it is never in the picture. |
| 25 | **The depth plane in internal SRAM, and a key sort** | done | 2026-09-22, the user's call (D-77). Engine option `SE_SCENE_DEPTH16_INTERNAL`: at quarter resolution a plain 16-bit depth plane (188 KB) in internal SRAM, cleared each frame (0.29 ms), instead of the stamped PSRAM plane; the flat list it displaced went to PSRAM. Depth order is now a radix sort of 32-bit keys in internal SRAM plus one gather, not a qsort of the records. Same scenes as F-66: flight 13.97 -> 16.30 fps, near walk 10.22 -> 12.36, Far walk 7.08 -> 7.99 (F-67). |
| 29 | **Torches on walls** | done | 2026-09-23, asked for by the user. The mesher can now see each cell's block-data field -- a third plane beside cells and lights, carrying `st_data()` already extracted, because `voxel_mesh.c` may not include `chunk.h` -- and the torch reads it: upright in the middle of its cell, or shifted 0.30 to whichever wall was pointed at and lifted 0.20 off the floor. No tilt: a greedy voxel mesher emits axis-aligned boxes, and a rotated stick would be a second kind of geometry for one block. Placing picks the wall from the face that was struck, refuses the underside of a block (nothing here hangs) and refuses a wall that is not solid -- the first placement in this game to say no for a reason other than the cell being full. meshcheck pins where the stick ends up, not merely that something was drawn. **Known gap:** breaking the block a torch leans on leaves it floating; that wants block-update propagation, which leaves-decay and falling sand will want too, so it is worth building once rather than special-casing here. |
| 30 | **Item icons, a bigger inventory, and the arm** | done | 2026-09-23, asked for by the user: the slots were flat average colours (D-03's placeholder) and three stone tools were three grey squares. A block is now drawn with **its own side texture** -- one table of files, no second copy, and a new block brings its icon with it -- and the eight things that are not blocks got drawn 16x16s with cut-out backgrounds. Tab slots 44 -> 60 px. The first-person arm got a mesh of its own, the sleeve running back past the camera: its flat cut end had been sitting just inside the bottom of the frustum, which is what made it read as a severed arm hanging in the air. |
| 28 | **Water you can see into, and swim in** (D-86) | done | 2026-09-23, the user: water was an opaque cube, so putting your eyes under it broke the picture, and there was no swimming. Their rule, and it is the whole of it: **do not draw the sides or the bottom of a water block, and draw its top only when the block above is air.** That became `K_LIQUID`. Two things follow that the rule does not say out loud and the picture needs: a liquid must stop HIDING its neighbours, or the lake bed is never meshed and the surface is a lid over nothing; and the surface needs a second, downward-facing copy, emitted by the air cell above it, because an axis-aligned face is visible only from the side its normal points at -- which is exactly why it vanished as the eye went under. `water.png` became a cut-out checkerboard (the engine's one-bit alpha, the leaves' mechanism) so you see through the surface both ways. Swimming is buoyancy in `player.c`: jump rises, sneak dives, and the numbers come from `phys_gravity`'s recurrence rather than from feel. meshcheck pins the rule per material and per direction, and caught a real bug on the way -- the extra slice let a border cell act as an owner and doubled every face at a section seam. **And the blue.** The user's read of it was right and mine was wrong: the renderer already touches the brightness of every pixel, so the tint belongs there. `se_scene_set_tint()` scales the red and green of every triangle by one factor and the blue by another; the sky and the fog go to a dark blue and the sun, moon, clouds and stars are not drawn from under the surface. |

---

## Part E: findings and decisions log

- **F-82** 2026-09-23: **the auto-crafting planner was wrong in a way that
  only a worked example shows.** The user's own example was the test: *"when we
  need a pickaxe, but only have blocks of wood"*. Two logs is eight planks; a
  pickaxe is three planks and two sticks, and sticks are two more planks.

  The first version provided each ingredient in turn and then crafted. It turns
  a log into four planks (three needed -- done), then turns two of those planks
  into four sticks -- **and the three planks it had a moment ago are now two.**
  Nothing reserves anything, so a later ingredient quietly eats what an earlier
  one was given.

  The fix is not reservation, which would need a whole allocator's worth of
  bookkeeping for a tree four deep. It is to **ask again**: each pass
  re-provides whatever is short, and the loop ends when the recipe can actually
  be made, when a pass achieves nothing, or when something cannot be provided at
  all. Two logs now come out as a pickaxe with three planks over, which is the
  arithmetic done by hand.

  Two smaller things fell out of writing the check. The planner would have made
  a crafting-table recipe in your bare hands, because nothing asked whether the
  RECIPE belonged at the station -- only its ingredients. And it must never
  smelt: an iron pickaxe planned from iron ORE would have the menu lighting a
  furnace on the player's behalf.

- **F-83** 2026-09-23: **a translation helper that dropped keys in silence.**
  The script appending translations to `lang/*.txt` grouped them under four
  known prefixes and wrote nothing for a key outside that list -- so when the
  chest, the bench and the HUD brought `chest.`, `bench.` and `hud.`, 13 keys x
  31 languages went in and nowhere, with no error. `make_lang.py` caught it
  ("13 of 215 untranslated"), which is the only reason it was a round trip and
  not a shipped bug.

  The helper now refuses to write anything unless every key it was given is
  accounted for. A tool that quietly does part of the job is worse than one
  that fails, because the part it did looks like the whole.

- **F-81** 2026-09-23: **the width check only measured labels, so it passed
  while the user was looking at the bug.** F-76 added `check_label_widths()`
  after three settings screens turned out to be overlapping their own text in a
  dozen languages -- and it measured each LABEL against its value column, which
  was the failure of the day. The failure this time was the VALUE: "have 0,
  need 2 more" ran out of the crafting panel and off the right of the screen,
  and the inventory's legend did the same, **in English**, which is the
  shortest language the game ships.

  `check_text_fits()` measures the other side: values, hints and free-standing
  lines against the room each actually gets, worked out the way `se_ui.c` works
  it out, with every format string filled in the worst way the game can fill it
  -- every count a full stack, every `%s` the longest item name in that
  language. It found **five more of the same bug immediately**, two of them on
  the furnace screen, which the user had not reached yet: `64 Wooden pickaxe`
  wanted 326 px in a 190 px column and `makes Wooden pickaxe` 367 px in 198.
  The furnace picker lost its value column as a result -- what a stack would do
  moved to the footer, which is 14 px and has the whole panel.

  The lesson is not "measure text". It is that **a check written for one
  failure tends to measure exactly that failure and nothing next to it**, and
  the cheapest time to widen it is the next time something in the same family
  goes wrong. 30 lines x 32 languages now, tightest 33 px.

- **F-80** 2026-09-23: **two bugs the user reported turned out to be one**, and
  the lesson is worth more than the fix. They saw *"sometimes the cursor keys
  don't work in crafting and furnace menus"* and *"crafting torches took the
  items from my inventory without adding torches"* -- which read as an input
  bug and an inventory bug, in different files, neither reproducible on the
  host.

  `player_t.used_block` says "the player opened something THIS TICK". It was
  set and cleared in the branch of `player_tick` that handles using a block --
  and once a screen is open the player is frozen, so that branch is never
  reached again and **the flag stayed set forever**. main.c dutifully reopened
  the screen every tick. Opening one resets its cursor to the first row, empties
  its search box and swallows the keystroke that opened it, so: the arrows did
  nothing, typing did nothing, and enter crafted whatever was FIRST in the list
  instead of what was under the cursor. Hence materials gone and the wrong thing
  back.

  Only when the screen was opened by USING a block, never by the Craft key --
  which is exactly the "sometimes".

  **A field that means "this tick" is cleared at the top of the tick**, not
  where it happens to be written. It is now, and main.c also refuses to open a
  screen that is already showing: two locks, because the failure is invisible
  and the symptom points somewhere else entirely. The two-ingredient craft path
  the report blamed was then exercised end to end in worldcheck and was correct
  all along -- which is the other half of the lesson, since the obvious suspect
  cost nothing to clear and would have cost a day to keep suspecting.

- **F-77** 2026-09-23: **two ways a furnace could have quietly eaten your
  things**, both found while building it and neither by playing.

  1. **A chunk is only written to the card when `CF_EDITED` is set, and only
     `world_set()` sets it.** Smelting changes what is inside a block without
     changing any block, so a furnace full of coal would have been thrown away
     on the next eviction -- and the player would not have found out until they
     walked back to it, by which time there is nothing to debug. Fixed with
     `chunk_mark_edited()` and `blockent_touch()`, which every put and take
     calls; the comment on `blockent_touch` says why it exists, because the next
     container will need it and nothing about the call site suggests it.
  2. **Breaking a container dropped the container and not its contents.** Fixed
     in `interact_break`, which now spawns every slot before the record goes.

  Neither is the kind of bug a test suite finds by accident, and both are
  unrecoverable for the player, so they are written down rather than merely
  fixed.

- **F-78** 2026-09-23: **the badge has one QWERTY and the game speaks 32
  languages**, which the user spotted as soon as a search box was proposed:
  *"the Tanmatsu always has the same QWERTY keyboard, how will this work with
  other languages?"* A Bulgarian player reading *Кирка* cannot type К -- and
  neither can a Turk type the o-umlaut in *Kömür*, a Pole the stroked l in
  *Łopata* or a Czech the r-caron in *Dřevo*, so it is not only the non-Latin
  scripts. `i18n/fold.{c,h}` folds both sides of the match down to plain ASCII;
  `tools/make_fold.py` derives the table from every character in every lang file
  and **refuses to emit one with a hole in it**; worldcheck walks 73568
  characters across the 32 languages and asserts every one folds. One letter,
  not a digraph (the user's call): a German would type *loeffel* and a Turk
  *komur*, and only one of those can win.

- **F-79** 2026-09-23: **`symcheck` caught a missing source file**, not a
  missing loader export. `main/game/furnace.c` went into the Makefile's pure
  list and `hostpurity.sh` and not into `CMakeLists.txt`, so the host checks
  passed, the app linked, and five `furnace_*` symbols were left for graceloader
  to resolve -- which it could not, and the app would have started and died in
  silence exactly as F-74 did. The check written for one failure caught a
  different one on the same path.


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

- **F-46** 2026-09-21, step 4.3: **the overlay cost 13.4 ms a frame through
  PAX** — fifteen per cent of the frame, for about 120 rectangles covering some
  14000 pixels. At the memory speeds this hardware actually has (F-40) those
  pixels are worth about 1.5 ms, so the rest was per-call overhead: a matrix, a
  clip, a shader dispatch and a function-pointer setter, per rectangle.

  Redrawn through `se_direct565.h` — which is public for exactly this reason —
  it is **0.8 ms**, a 16x cut, and the screenshot is pixel-identical. A
  rectangle becomes one contiguous halfword run per column, because the display
  is rotated and a vertical run is what is contiguous in memory.

  It was found only because the phase split did not add up: 71.7 ms of phases
  in an 86.7 ms frame. `PROF_HUD` now exists so the next person does not have
  to notice a subtraction.

- **F-47** 2026-09-21, **the user asking "how do I drop items?"**: G was bound
  and wired, and it **appeared to do nothing**. The item left the inventory and
  was collected again half a second later.

  The arithmetic makes it unavoidable rather than unlucky: the pickup radius is
  1.4 blocks and nothing can be thrown further than an arm's length in one
  tick, so a dropped item ALWAYS lands inside it. Distance was never going to
  be what made G work -- **the pickup delay is**. A block's drop needs 10 ticks
  (so breaking the floor under you does not snatch it back); a thrown one needs
  40, which is two seconds and Minecraft's number.

  Now thrown with velocity from eye height rather than placed in a cell, so it
  arcs, slides and comes to rest **1.7 blocks clear** -- an item you have to
  walk back to. The host test asserts both halves: that standing on it for 39
  ticks does not collect it, and that walking to it does.

  Worth noting how it was found: not by testing, but by a question. The feature
  was "done", host-tested, and committed.

- **F-48** 2026-09-21, step 5.1: **half the title screen was invisible, and
  four wrong explanations were tested before the right one.** "CraftMiner" is
  written in real blocks in the world; "Miner" drew and "Craft" did not.

  Ruled out, each by measurement rather than argument: the blocks were not
  placed (216 of 216 were, and a printed map of the world showed the whole
  word); the chunks were not resident (`chunk_find` returned all four); the
  meshes were not built (140/222/142/78 triangles in the right sections); the
  geometry was culled (nothing was); the engine's lists overflowed (a new drop
  counter said zero); it was a `shots` warm-up artefact (three captures of the
  same instant hashed identically).

  What fixed it: **raising the letters and the camera**. The sight line from
  the old camera to the missing letters passed a few blocks over the treetops,
  and something along it was writing depth without being visible -- distant
  terrain flat-shaded towards a fog colour that IS the sky colour is the
  obvious candidate. **Not confirmed.** The fix is empirical and the cause is
  recorded as unproven rather than dressed up.

  The lasting value is in the two things added while chasing it, both kept:
  `scene_drop_stats()` and pre-generation.

- **F-49** 2026-09-21: **the engine's geometry lists dropped silently.** A full
  list returns without drawing, in submission order, so what vanishes is
  whatever the game submitted last -- a corner of the world, a chunk, half a
  title. That is a hole in the picture that looks like a bug in the game, and
  it had already cost one debugging session (F-32) before it cost another here.
  `scene_drop_stats()` now counts it and the app warns; **anything non-zero is
  a hole**.

- **F-50** 2026-09-22, step 6.1: **the engine's key capture could not bind the
  arrow keys.** `se_ui_capture_key` refused every escaped scancode (>= 0xE000)
  and mapped only F1-F12 from navigation events -- and the cursor keys are
  CraftMiner's default look keys, so once rebound they could never be bound
  back. First worked round with a capture of the game's own; **the user asked
  for the engine to be fixed instead** and synthracer's menu code to be used
  as it is. Fixed in the engine (2.1, `src/se_run.c`, `se_bindable_scancode`):
  escaped grey keys bind as their scancodes, and navigation-only cursor, Home,
  End and Page keys map onto the same scancodes, so a key binds to one value
  whichever keyboard pressed it.
- **F-51** 2026-09-22, step 5.10: **saves from before `placed` need a rule, and
  there is a sound one.** Those builds wrote the player on creation (x and z at
  the spawn column's centre, 0.5, 0.5) and on leaving (where they really were),
  and at no other time. So a save whose position is anything but that default
  is a real position. The one wrong answer this can give is a player who left
  standing on exactly (0.5, 0.5) — who then lands on the ground at the same
  column, which is where they were anyway, give or take a cave.
- **F-52** 2026-09-22, found while writing 5.11: **re-saving level.cmw rewrites
  the palette as today's, but untouched chunks keep yesterday's ids.** The
  palette is written from the current registry on every save, while a chunk on
  the card is only re-encoded when it is edited. The day a block id moves, the
  next save relabels every unedited chunk with the new numbering. Harmless
  until then — no id has ever moved — but D-31's promise does not hold across
  two saves. The fix is for a chunk to carry the palette generation it was
  written with, or for a renumbering to rewrite every region. Not fixed here;
  recorded so it is fixed before the first renumbering, not after.
  **Closed 2026-09-22 by D-74:** ids never move, so there is no renumbering
  for it to go wrong in, and `make check` enforces that.
- **F-54** 2026-09-22, reported by the user: **the inventory cursor walked the
  rows upside down.** The Tab screen draws the hotbar at the bottom and the
  storage rows above it, but the cursor moved in slot order, where the hotbar
  (slots 0-5) comes first: up from the hotbar went nowhere, and the bottom
  storage row was reached by going down from the top edge. Now the cursor moves
  in screen rows through `inv_screen_row()`, which the screen also draws with,
  so the two cannot disagree again. Host-tested: up from the hotbar lands on
  the row above it, both edges clamp, and walking the grid reaches every slot
  exactly once. Block 4's tests covered stacking and swapping but never
  movement, which is how it shipped.
- **F-55** 2026-09-22, reported by the user: **the gyroscope's pitch sign was
  wrong; its yaw sign was right.** Both came from the axis diagram in
  `graceloader_imu.h`. Turning left and right worked first time; tipping the
  badge looked the wrong way up. Flipped (`GYRO_PITCH_SIGN`), not investigated
  further -- whether the diagram or my reading of it is wrong is worth
  checking before the next app trusts it for pitch.
- **F-53** 2026-09-22: **`strtoll` is not exported by the graceloader.** It
  linked, and `make verify` caught it before the badge did. The seed parser
  does its own decimal.

- **F-56** 2026-09-22, reported by the user: **a placed torch could not be
  picked up.** The crosshair's ray stopped only at SOLID blocks, and a torch is
  not solid, so it could never be aimed at -- nor could a flower or tall grass.
  The ray now stops at anything but air and liquids (so water still never hides
  the riverbed), and placing onto tall grass replaces it as the highlight says.
  Host-tested both ways.
- **F-57** 2026-09-22: **Minecraft's night (sky light minus 11) is unreadable
  on this screen.** Measured by photographing the title at midnight: the
  letters were barely there. Minus 9 leaves a moonlit field dim but legible and
  still far darker than torchlight.
- **F-58** 2026-09-22: **lighting a chunk as it arrived cost 23 ms on the main
  task** -- title pre-generation went 2757 -> 4593 ms. Three wastes removed
  (marking meshes of a chunk nobody has drawn, seeding the sky flood from every
  lit cell, re-flooding the neighbours' lit cells) brought it to 8.4 ms, and
  splitting the work -- a chunk's own light on core 1 while it loads, only the
  border exchange on the main task -- to about 5 ms a chunk in total, most of it
  off the frame path.
- **F-59** 2026-09-22: **settling the player after the frame's ticks undid
  them.** Entering a world puts the player back at their saved position on the
  first frame their chunk is resident; that ran AFTER the ticks, so it threw
  them away. Invisible in play (one frame), fatal to a test that jumps the clock
  and runs a hundred ticks in that frame. Now it runs before.
- **F-60** 2026-09-22: **the frame that finished loading was drawn with a
  camera nobody had set** -- on_update returned early for the loading step, and
  a test photographed exactly that frame. Now the finishing frame carries on.
- **F-61** 2026-09-22, found while adding light: **an edit on a chunk border
  could lose the neighbour's remesh.** world_set marked the neighbour stale
  without moving its edit_seq, so a mesh of it already in flight came back,
  was accepted, and cleared the stale bit. `world_mark_dirty` now bumps edit_seq
  on every chunk it marks.
- **F-62** 2026-09-22: free PSRAM at boot is now about 13 MiB, down from 20:
  the light plane adds 4 MiB to the chunk slab and the two flood contexts 1.5 MiB.

- **F-63** 2026-09-22, reported by the user as a critical bug: **nearby
  terrain vanished and the player seemed to walk into steps.** The engine's
  geometry lists overflowed and dropped triangles in submission order -- which
  is slot order, not distance -- so whole patches near the player went missing,
  a step among them. Two causes stacked: the medium view (already over the flat
  cap before lighting, the reason near had been the default) and lighting,
  which splits merged faces (+46% triangles in the fancy meshes, +16% fast,
  +10% coarse, measured on the user's terrain). It went unseen because the
  engine only counted drops that happened during near-plane clipping; the
  common case, a whole triangle arriving at a full list, returned without
  counting. The walk that showed it submitted 6000-8400 triangles a frame
  against caps of 4096 + 2048.
- **F-64** 2026-09-22, reported by the user: **Fred held things in his left
  hand in third person and his right in first.** The showreel built the miner
  with his right hand on -x; in this engine the camera's right at yaw 0 is +x
  (host-checked: a model's +x lands on the right of a camera behind it). One
  caution from checking it: a screenshot of a figure seen from behind at night
  easily shows the WORLD's torch where you expect his -- it took a mid-swing
  shot, pickaxe above the shoulder, to see which hand was which.

- **F-65** 2026-09-22: **where lighting's extra triangles come from.**
  Measured on the host, 25 chunks of the user's terrain, nearest meshes:
  no lighting 32034; sky light at full precision 46806 (+46%) -- all of it sky,
  since generated terrain has no torches; rounded to every 2nd level 43712;
  to every 4th 37514. Then with 30 torches placed (one a chunk or so, as round
  a base) full precision is 67792: each torch lays a fourteen-level ring of
  strips round itself. Sky every 4th and torch every 2nd: 50868, 25% under full
  precision. On the badge that took the medium walk from 7.61 to 7.94 fps.
  The torchlight bands are mild (checked at night); cave-mouth bands were not
  photographed.
- **F-66** 2026-09-22, the user: "didn't we get like 15 FPS yesterday?" **Yes,
  and today still does, on the same scene.** Yesterday's 15.3 (F-39) was the
  scripted debug flight -- 3 blocks up, looking across the land -- at near.
  Rebuilding that commit and adding the same flight to today's build:
  14.60 fps yesterday, 14.51 today with lighting and clouds off, 14.09 with
  everything on. The frame rates people see come from WALKING: at eye height
  the screen is close-up textured ground, rasterize 72 ms instead of 46, 9.6
  fps at near, 7.9 at medium, 7.35 at Far (the user's own setting). So the
  work that would move the frame rate is the rasterizer's per-span setup
  (F-40), not undoing features. Lesson recorded with it: a frame rate belongs
  to a scene, and two numbers from two scenes compare nothing -- the same
  mistake as F-36, the other way round.
- **F-71** 2026-09-23, going from 6 languages to 32 (6.5): **the alphabet
  check turned a vague question into a costed one.** Asked which major
  European languages were missing, the generator answered exactly, because
  coverage was already declared per language rather than per string: nine were
  free (Russian above all -- 110M speakers, and the Cyrillic import had
  already taken all 32 letters and composed Ё precisely so it would be), and
  the rest sorted themselves by what they actually needed. The shape of the
  answer was not what it looked like from the missing-glyph counts. Greek
  looked like the most expensive at 58 missing and was among the cheapest:
  Hershey drew a Greek SIMPLEX face (527-550, 627-650), the same weight as our
  Latin, so it is a two-line import, while the Cyrillic had only ever been
  available as the heavier complex. And one accent, the caron, unlocked six
  languages at once. What cost real work was what no accent can make: Polish's
  stroked Ł, Icelandic's þ and ð, Serbian's five new Cyrillic forms -- though
  two of those (Љ Њ) turned out to be ligatures of letters Hershey already
  drew, joined so they share the upright, which is what they are made of.
  A mixed-script check was added after a machine translation put a Latin `a`
  inside a Cyrillic word: identical on screen, and a box on the badge.
- **F-69** 2026-09-23, translating the UI (6.5): **the font was the whole
  job; the strings were the easy half.** The engine drew ASCII and nothing
  else -- Hershey roman simplex, 95 glyphs, indexed by `char - 32` -- so
  German needed umlauts, French its accents and Bulgarian an alphabet the
  font had never heard of. Hershey's own database (public domain, the same
  source the 95 came from) turned out to hold a Cyrillic face, 32 letters in
  each case, and the quotation marks, comma and dash the European languages
  want; the accented Latin letters are composed (one acute, fifty vowels),
  `Æ` `æ` `Ĳ` `ĳ` `“` `”` `„` `…` are Hershey's glyphs placed side by side, and
  `ß` `ẞ` `œ` `Œ` `«` `»` were drawn by hand. Two things make that trustworthy
  rather than hopeful: the generator regenerates the 95 ASCII glyphs from
  the raw database on every run and refuses to write anything unless they
  match the committed table point for point (so the coordinate conversion is
  provably the renderer's own), and it checks every letter of every declared
  alphabet has a glyph and names those that do not. A codepoint with no
  glyph draws an empty box, so a gap is visible rather than silently
  dropped. Cost: about 5 KB of tables, and a bisection over 83 entries only
  for characters that are not ASCII. Measured on the badge: Bulgarian menus,
  German umlauts and French cedillas all render at menu size.
- **F-70** 2026-09-23, translating the UI (6.5): **positional printf is not
  promised, so the substitution is ours.** A translation may need the values
  in a different order than English puts them, which printf spells `%2$s` --
  a POSIX extension newlib only has when built with `_WANT_IO_POS_ARGS`.
  The graceloader's sdkconfig suggests full formatting, which probably has
  it; "probably" is not something a UI should rest on, and an IDF upgrade
  could take it away. `i18n_fmt` therefore does the substitution itself and
  hands the C library one value and one plain specifier at a time, which
  every libc can do. It also made the thing safe: the TYPES come from the
  English string, never from the translation, so a lang file a stranger
  edited on the SD card can get the padding wrong or drop a value but can
  never make the formatter read the wrong kind of argument off the stack.
  Host-tested against `%s` where English says `%d`, `%9$d`, a lone per-cent,
  more values than exist, and a reorder.
- **F-68** 2026-09-22, building the Far Lands (7.1): **porting Beta's generator
  and overflowing it reproduces the Edge Far Lands without being told what
  they look like.** Over 32 chunks at the edge: 42% rock, 30% air, 19% water,
  9% dirt and grass -- the wiki gives 36 / 25 / 23 / 10 for Beta's own, and
  ours has half the height and a lower sea, so the match is closer than it
  had any right to be. Neighbouring blocks agree 98.7% of the time along x
  and 87% along z: the tunnels run west. Things that were not what the
  design expected: (1) the mesh is LIGHT, not heavy -- unchanging along x
  means every face spans the chunk and merges whole, ~200 triangles a
  chunk; (2) the fast path (first octave of low and high, no falloff) makes
  the same blocks as the full port in all 524288 cells compared, a third of
  the host time; (3) on the badge, with doubles in software, a Far Lands
  chunk still takes 134 ms against 33 ms for an ordinary one. That is
  worker time, so loading is slower out there, not the frame rate: walking
  up to the wall at Medium ran at 11.0 fps with nothing dropped. Not yet
  profiled; by operation count the noises outside the overflow (the
  selector, 3400 octave samples, and the surface pass, 3072) should be
  most of it, against 850 for the overflowed octave. Floats there would
  cut it, at the price of the block-for-block proof, which would have to
  become a bound.
- **F-67** 2026-09-22, the user asked whether a pixel's cost was the pixel or
  the z-buffer, then to move the z-buffer into internal SRAM. **Moving it cut
  the cost per pixel by about 30%, far more than F-40's memory benchmark
  predicted** (which put all of memory at ~20 of 62-72 cycles): flat 178 ->
  122 ns/px, textured 190-210 -> 140-155. The depth test was already the first
  thing a pixel does -- a pixel that loses never pays the divide, the texel
  fetch or the framebuffer write -- so that part needed no change. The plane
  cannot fit at full resolution (750 KB) and did not fit beside the 240 KB flat
  list either (133 KB free, largest block 62 KB), and the list cannot shrink:
  its peak is 4897 (far). So the list went to PSRAM, which it reads in order.
  That cost prep time where the list is SORTED: qsort moving 40-byte records in
  PSRAM took the Far walk's prep from 12.4 to 17.3 ms and ate a third of its
  gain. The sort is now keys in internal SRAM (16-bit depth over 16-bit index,
  two 8-bit radix passes) and one gather into a second buffer that swaps with
  the first: prep 10.5 ms, under the 12.4 it was before. Costs 48 KB internal
  for the keys, 512 KB PSRAM for the gather buffers; internal SRAM free went
  133 -> 161 KB. Same scenes as F-66, 25 s each:

  | Scene | fps before | depth internal | + key sort |
  |---|---|---|---|
  | flight | 13.97 | 16.30 | -- |
  | walk, near | 10.22 | 12.36 | -- |
  | walk, far | 7.08 | 7.59 | 7.99 |

  Rasterize on the Far walk 66.2 -> 49.1 ms. What is left is still the
  per-span setup of very short spans (5-7 px flat, 10-13 textured) and about
  4x overdraw on the walk -- the next levers, not memory. Not visually
  checked (the user's call): neither change can move a pixel, the sort only
  changes which triangle the depth test meets first.

- **F-72** 2026-09-23, step 14: **the sequencer could be proved on the host,
  so it was.** `midi_seq.c` is pure -- no engine, no RTOS, no allocation --
  which meant `worldcheck` could run every shipped file through the real
  sequencer at the real sample rate rather than anyone listening for a
  problem on the badge. What that caught is not that the files play, which
  was never in doubt, but the shapes of failure that only show up later:
  a file that never ENDS (the audio task would hang on it), a rewind that
  differs from the first pass (the scheduler replays pieces), and a
  truncated copy -- a file half-copied onto an SD card -- read past its
  buffer. The check truncates every file at every 97th byte and requires
  each one to terminate.

  The clock needed fixing for the same reason. The donor player kept its
  tempo in `double`; the P4's FPU is single precision, so that is a call
  into a software library, on the audio task, per tick. It is 32.32 fixed
  point here -- and the obvious `(num << 32) / den` **overflows**: a 32-bit
  core has no `__int128`, and `rate * us_per_quarter` is already 1.1e10
  before the shift. Long division with two sixteen-bit refinements instead.

- **F-73** 2026-09-23, step 14: **sixteen voices summing is not bounded by
  anything.** Rendered on the host, a dense Schumann chord peaked at 1.64
  where 1.0 is full scale, and the honest int16 conversion clipped it --
  1036 clipped samples in Traeumerei, heard as a crackle on every loud
  chord. Turning the master gain down far enough to fix that alone would
  have made the Satie, which never went above 0.89, too quiet to hear under
  the footsteps.

  The fix is a gain of 0.58 and the cubic soft clipper `1.5x - 0.5x^3`:
  three multiplies, no table, no branch on the common path. It has a second
  property worth having -- its slope at small signals is 1.5, so it lifts
  quiet music while it compresses loud music, and the eleven pieces end up
  nearer each other in level than they went in. Afterwards: peaks 0.71 to
  1.00 and **not one clipped sample** in any of them.

  The general lesson is the one worth keeping: the host render was a WAV
  file and a peak meter, and it found in a minute what would have been
  "the music sounds a bit crunchy sometimes" on the badge.

- **F-74** 2026-09-23, step 14: **the app built, linked, uploaded and then
  would not start, and nothing said why.** No console output, no panic, no
  splash -- `make ping` answered "the launcher is up, or the app is wedged",
  which is true and useless. Three cycles went into the tooling before the
  cause turned out to be one line of ours: `music.c` called **`strcasecmp`**,
  and graceloader does not export it.

  An ELF app for graceloader links against NOTHING. Every libc and IDF
  function is resolved at LOAD time from the loader's export table, so a
  call to something that table does not carry compiles clean, links clean,
  uploads clean, and then fails at the one moment when there is no way left
  to report it. se_mp3.c's header records the same trap with `opendir()`,
  which is how it was eventually recognised.

  Two things came out of it, and the second matters more than the fix:

  * `ieq()` in music.c, four lines, because a file extension is ASCII;
  * **`tools/symcheck.sh`, run by `make build` after the link**: every
    undefined symbol in app.so, checked against
    `../tanmatsu-graceloader/main/symbol_export/all`, naming anything
    missing. It found exactly one symbol -- ours -- and it will find the
    next one in the second it is introduced rather than after an hour of
    suspecting the badge. Where the loader's source is not checked out it
    SKIPS and says so, rather than failing a clone that only wants to
    build.

  The general shape is one this project keeps meeting: a seam between two
  builds that the compiler cannot see across wants a check that CAN. It is
  the same argument as `hostpurity.sh` and the language-table staleness
  rule, and the same argument for having written `check_midi` before ever
  putting a MIDI file on the badge.

- **F-75** 2026-09-23, **the user**, reporting it: **"the tool sounds only
  play when music is enabled."** Which is exactly what it looked like, and
  exactly what it was not.

  Nothing in the effects path reads the music setting. What actually
  happened is the mixer's idle power policy: it mutes the amplifier and
  disables I2S about 46 ms after the last sound, and powers back up when the
  next voice is registered. The audio is mixed and written correctly either
  way -- but **an amplifier's turn-on is not instantaneous**, and CraftMiner's
  effects are short. `SFX_HIT`, the tool striking a block, is 35 ms. Against a
  cold amp it is over before the speaker is listening.

  Why the music setting appeared to control it: a music source that is
  INSTALLED counts as an active source every chunk, whether or not it is
  making any sound. With music on, our source renders silence through the
  four-to-ten-minute gaps and the amplifier never sleeps, so the effects are
  fine. Turn the music off, the gate skips the source, the mixer idles, and
  every effect now has to wake the speaker up.

  The badge said so plainly once asked the right question. With `music=0`:
  `power_down: amp+I2S off` at 4662 ms and never up again. With the fix:
  `power_up` at 7992 ms and no power_down in the next 43 seconds.

  Fixed in the engine with `audio_mixer_keep_awake()` rather than in the
  game, because the sharp edge is the engine's and the next game to meet it
  would lose the same afternoon. CraftMiner holds it on for as long as it
  runs. The cost is the amplifier's idle draw -- which, note, **this game was
  already paying** whenever the music was switched on.

  The lesson is not about amplifiers. It is that "feature A only works when
  feature B is on" almost never means A reads B; it means both depend on
  something neither of them mentions.

- **F-76** 2026-09-23, adding the volume sliders: **three settings screens
  had been overlapping their own text in a dozen languages, and nobody had
  measured.** A row draws its label at the left and its value -- a slider, a
  tick box, a word -- at a fixed offset (`value_dx`). The offsets were chosen
  by eye against ENGLISH, when English was the only language there. Going to
  32 (F-71) checked that every letter could be DRAWN and never asked whether
  the words would FIT.

  What was already broken before a single slider was added: Graphics
  overflowed in 23 of 32 languages, Settings in 7, Audio in 4. The worst was
  Ukrainian's "Дальність промальовування" at 492 px against a 260 px column
  -- nearly double.

  Two kinds of fix, and both were needed:

  * the panels and columns widened where the language is simply longer than
    English (Audio and Graphics now use a wider panel; every column grew);
  * eight translations shortened where no panel could have held them. These
    were descriptions where a label was wanted -- Bulgarian's "Разделителна
    способност" for a resolution toggle is correct and is not what a control
    is called.

  The third fix is the one that lasts: **`check_label_widths()` in
  worldcheck**, which measures every label of every screen in every language
  with the engine's OWN `hershey_advance` at the row height the menu uses. A
  label that passes there fits on the badge. It reports the tightest fit in
  the whole game (16 px, Spanish "Volumen de los efectos") so the margin is
  visible rather than assumed, and it was proved to bite by lengthening a
  Ukrainian label and watching it name the language, the key, the text, the
  width and the column.

  This is the same lesson as F-74 and `hostpurity.sh`: a property the
  compiler cannot see wants a check that can. "Every character is drawable"
  and "every label fits" look like the same question and are not.

### Decisions (D-n), each with date and who decided

- **D-81** 2026-09-23, **the user**: **the UI is translated, English by
  default, into German, Dutch, Flemish, French and Bulgarian**, with the
  language an easy reach in Settings. Machine translations to begin with,
  "but the translations should be easy to adapt by humans" -- so the text is
  text files (`lang/*.txt`, `key = text`, UTF-8) and never a string literal
  in the code, and the cheapest thing at run time was asked for as well.
  Both: the files are baked into arrays by `tools/make_lang.py`, so a lookup
  is an array index and nothing is parsed while the game runs, and a player
  with no toolchain can still drop `/sd/craftminer/lang/<code>.txt` on the
  card to correct what ships. `lang/en.txt` defines the keys; a language
  missing one shows English, and a key English does not have is an error.
  Language names are never translated -- the player who needs that row is
  the one who cannot read the language the game is in. Not translated:
  the name CraftMiner, world names, the key names printed on the badge's
  own keys, and every log line (the user: "the debug output stays
  untouched").
  **Amended the same day, the user:** support the *languages*, not the
  strings that exist today -- the font must cover every letter of every
  alphabet, so that adding or changing a string can never meet a character
  the font lacks. The check therefore runs over alphabets, not over
  translations, and how to extend the font is written down for the next
  language (`synthengine3D/tools/hershey/README.md`).
  **Amended again 2026-09-23, the user:** all of them -- every European
  language the font could be made to reach, Turkish included, 32 in total.
  English first in the list and the rest **alphabetical by the name each
  language calls itself**, "to make it easy to find": the name a player is
  looking for is the one they can read, so the list sorts by that and not by
  English name or by code. The translations past English stay a machine's
  work, and the README now asks for pull requests by people who speak them.
- **D-82** 2026-09-23, **the user**: **the music is MIDI files of classic
  pieces that are out of copyright**, played "at random intervals with long
  pauses between them, similar to old minecraft versions", chosen randomly
  rather than in playlist order, "with the only limit to not play the same
  piece twice in a row".

  Three things follow, and the third is the one that needed care.

  * **MIDI, not MP3.** The engine has `se_mp3.h` and it would have worked,
    but the cost is a decoder task with a 32 KB stack, a 64 KB PCM ring and a
    16 KB read buffer, and the music itself is megabytes. The eleven pieces we
    ship are **72 KB in total**, and the sequencer allocates nothing but the
    file. `se_voice.h` had said all along that "a future MIDI player will keep
    a pool of voices and route note-on / note-off events to them"; this is
    that player, so the engine did not have to change at all.
  * **The silence is the feature.** A piece, then four to ten minutes of
    nothing, and a fresh random gap each time. The first gap is shorter
    (25-70 s) so that a player who puts the badge down after five minutes has
    at least heard that the music exists.
  * **TWO COPYRIGHTS, not one.** A MIDI file carries the copyright of its
    *engraving* as well as of the composition. Satie, Debussy, Chopin,
    Schumann and Bach are all long out of copyright; the particular typeset
    edition a file was generated from is a new work and usually is not. A
    MIDI file found loose on the web is almost never licensed to
    redistribute, whoever wrote the tune. So every file comes from the
    Mutopia Project, which publishes an explicit licence per piece -- and
    only from its **Public Domain** set, never its Creative Commons one.
    That cost us the Gnossiennes, which are BY-SA, and share-alike would
    have put conditions on anyone redistributing CraftMiner. The
    Gymnopedies are Public Domain, so the mood survived.
    `tools/get_music.py` **refuses to download anything that is not Public
    Domain**, so extending the set cannot go wrong by accident, and
    `assets/music/MUSIC.md` records where each file came from.

- **D-83** 2026-09-23, Claude: **what a block sounds like is a field in the
  block registry, not a switch in the audio code.** `block_def_t.sound` names
  a material class (SND_STONE, SND_WOOD, ...) and the footstep, the break and
  the place all follow from it. This is the same extendability contract as
  the rest of the table (Part L): adding a block is one row, and the row now
  carries its sounds. The alternative -- a lookup in `sfx.c` keyed on block id
  -- would have been a second table to keep in step with the first, and the
  one that got forgotten.

  The effects themselves are **one table, not one file per noise**. The
  synthracer modules this is descended from gave each effect its own file,
  which is right when there are seven and each is a different idea, and wrong
  here, where a footstep on gravel and a footstep on sand are the same idea
  with different numbers. A row is a tone layer and a noise layer through one
  filter and one envelope, with per-play pitch jitter so no two footsteps are
  identical.

- **D-84** 2026-09-23, Claude: **the game thread reads the card; the mixer
  only plays.** `se_audio_source.h` forbids blocking in `render()`, and a
  MIDI file lives on the SD card. Unlike the MP3 source, which needs a whole
  decoder task to bridge that, a MIDI file is small enough to read whole:
  `music_frame()` loads it on the game thread and hands it over through a
  single atomic state word, each state having exactly one writer. Parsing
  happens in `render()` because it is a few hundred bytes of header walking
  with no I/O. If a file is slow to load the silence simply lasts a moment
  longer; nothing on the audio path ever waits.

- **D-80** 2026-09-22, **the user**: **the player's data lives in
  /sd/craftminer, not in the app's install directory.** The launcher owns
  /sd/apps/<slug> and may empty it on an update or a reinstall; worlds,
  settings, replays and screenshots must survive both. The install
  directory keeps only what ships with the app. Data an earlier build left
  there is moved across on start (a rename per entry, never overwriting),
  so nobody's world is lost in the move. Amends D-67's "next to the
  worlds, in the app's directory".
- **D-78** 2026-09-22, **the user**: **the Far Lands are a short walk west,
  sudden, and Beta 1.7.3's own.** The edge moves from x = -100000 to about
  5-10 minutes' walk from spawn (x = -2048, about 8 minutes at walking
  speed); the ramp goes -- a cliff of jumbled terrain, as in *Far Lands or
  Bust*; and the look should be as close as possible to Beta 1.7.3's Edge Far
  Lands, which Part X gets by porting Beta's generator and overflowing it
  rather than imitating the result. Later, signs such as "Kurt was here" and
  "Wolfie was here" at random along the edge. Replaces the first Part X.
  Confirmed the same day: -2048 it is, **as long as it can be changed
  later** -- hence the edge is stored per world, and a new default only
  reaches new worlds.
- **D-79** 2026-09-22, **the user**: **bedrock and gravel are blocks, and
  signs exist -- generated only, for now.** Bedrock (17, unbreakable) and
  gravel (18) come with the Far Lands, which Beta built from them. Signs
  (19) stand where the generator puts them and cannot yet be placed,
  written or carried; breaking one drops nothing. Their texts are a fixed
  list drawn into textures, so a sign needs no stored text until players
  can write their own.
- **D-77** 2026-09-22, **the user**: **the quarter-resolution depth plane
  lives in internal SRAM**, and the flat triangle list gives up its internal
  SRAM for it (the list's cap stays at 6144, which F-63 needed). An engine
  option, off by default, so other games keep their layout; CraftMiner turns
  it on in CMakeLists.txt. Full resolution keeps the PSRAM plane (F-67).
- **D-76** 2026-09-22, **the user**: **near is the default view distance
  again**, replacing D-66, once the walk measured medium at 7.9 fps against
  near's 9.6. It only changes what a player gets before choosing: a
  settings.txt that says otherwise (the user's own says Far) is kept.
- **D-74** 2026-09-22, **the user**: **a block id, once shipped, never
  changes** -- nor its name, and a block is never removed, only retired. Item
  names likewise. That makes F-52 impossible instead of fixing it, costs a
  lifetime limit of 255 blocks (past which is a format change anyway), and is
  enforced rather than remembered: explicit ids in `blocks.h` and a registry,
  `tools/ids.txt`, that `make check` holds the code to in both directions.
  Adding a block is appending a line there in the same commit. The palette
  stays, as a safety net that now has nothing to translate.
- **D-75** 2026-09-22, **the user** (the upgrader), Claude (the slot states):
  **a structural change is met by a one-time upgrade of the whole world,
  written when the first such change exists.** The major version in each
  file's magic is the hook (D-32). Meanwhile the slot list says what an
  unreadable slot is -- newer, older, damaged -- rather than showing it as
  empty. When the upgrader is written it must be crash-safe: convert region by
  region into new files, rename each over its original, record each region's
  version, and move the world's version on last, so an interrupted upgrade
  resumes rather than leaving a mixture.
- **D-72** 2026-09-22, Claude: **the geometry caps are 6144 flat (internal
  SRAM), 256 lines, 4096 textured (PSRAM).** The line list was 112 KB of
  internal SRAM for the dozen lines of a block outline; shrinking it pays for
  the bigger flat list, which stays in fast memory. The far meshes round light
  to every fourth level for merging (16% -> 4% extra triangles there); the
  near ones keep every level, where banding would show. The frame stats now
  print how full each list is. Medium stays the default (D-66); at 7-9 fps
  that is the user's call to revisit, with the frame-rate work they deferred.
- **D-73** 2026-09-22, **the user**: **Fred's handedness is a setting**, right
  by default, and both views follow it.
- **D-68** 2026-09-22, Claude: **dropped items are saved with the world
  record, not per chunk.** D-33 wanted them in each chunk's own section; that
  means handing entity data to and from the worker with every chunk save and
  load, for a pool of 96. A list in level.cmw is far simpler and keeps what
  D-33 actually promised, because an item in an unloaded chunk holds still (no
  fall, no ageing, no pickup) until its chunk returns. Revisit when there are
  creatures, whose numbers will not fit one list.
- **D-69** 2026-09-22, Claude (the design; the user asked for static torch
  light): **light is a derived plane, drawn through a table.** One byte a cell,
  sky and block light, never saved; flooded on arrival and on every change.
  Faces carry the byte; brightness = a table of it, rebuilt each frame from the
  time of day and handed to the engine as `SE_TRI_LIGHT` -- so night falls
  without re-meshing anything. Costs 4 MiB of PSRAM and four bytes a mesh
  triangle (mesh_tri_t 32 -> 36 bytes).
- **D-70** 2026-09-22, Claude: **the first-person arm is drawn at a third of
  the showreel's distance and size.** Same picture; but at 0.9 blocks out it
  vanished into any block the player was touching, which is every block they
  mine.
- **D-71** 2026-09-22, Claude: **+z is north, +x east.** The camera turns right
  from +z to +x, which is north-to-east on a map with north up, and the sun
  rises at +x. The position overlay's compass says so.
- **D-60** 2026-09-22, **the user**: **named worlds in save slots, and the
  Testworld moves into slot 1.** Eight slots, directories `slot1`..`slot8`, the
  name in level.cmw — so a name can be anything the keyboard types and renaming
  never moves a file. The pre-slots world goes into the first free slot as
  *Testworld*, by a single directory rename, the first time the new build
  starts.
- **D-61** 2026-09-22, Claude: **opening the pause menu saves.** On a handheld
  the way people stop is to switch it off, and pausing first is the habit. This
  is still "only when needed" in Part N's sense: it happens once per pause,
  never on a tick or a timer. It costs a short stall while edited chunks are
  written.
- **D-62** 2026-09-22, Claude: **the inventory is saved by item name**, the rule
  D-31 applies to blocks, and for the same reason. A name the build no longer
  has drops that stack rather than turning it into something else.
- **D-63** 2026-09-22, Claude: **Esc always pauses**, whatever Pause is bound
  to. Rebinding Pause must not be able to remove the way out of the game, and
  Esc is also the menus' Back, so it is the key everyone will try.
- **D-65** 2026-09-22, **the user** (the feature), Claude (the sensor): **look
  by turning the badge, with the keys still live.** The RATE gyroscope, not the
  accelerometer synthracer steers with: tilt is an absolute angle, right for a
  steering wheel, and cannot say which way you face. Integrated per frame,
  consumed per tick with the key delta. Not in the replay mask, so a replay of a
  gyro session will not look where the player looked -- replay is unwritten
  (5.8), and when it is written the gyro delta has to be recorded with it.
- **D-67** 2026-09-22, **the user**: **the game's settings live on the SD card**,
  so a player backs up everything with one copy of the app directory.
  `settings.txt` beside `worlds/`: plain `key=value`, unknown keys ignored,
  missing ones defaulted, written to `settings.tmp` and renamed into place. Key
  bindings included, keyed by each action's stable short id; for that the
  engine's `se_bindings` gained a mode with no NVS at all (a NULL namespace,
  engine 2.1), which leaves games that pass a namespace, synthracer among them,
  exactly as they were. Volume and brightness stay the launcher's. The NVS
  values written by the builds of the same day are not migrated: they existed
  for hours, on one badge.
- **D-66** 2026-09-22, **the user** (replaced by D-76 the same day): **medium is the default view distance.**
  Near was chosen when medium measured about 4400 flat triangles, past the 4096
  cap (the comment that said so went with the settings rewrite). Sectioning and
  culling have changed that number since; the GEOMETRY DROPPED log line is what
  will say if it has not changed enough.
- **D-64** 2026-09-22, Claude: **debug keys yield to bindings.** F (free
  camera) and P (pause the scripted flight) act only on a key no action is
  bound to. T and V are gone: their jobs are in Settings -> Graphics. N (added
  later the same day, asked for by the user) moves the world's clock a quarter
  of a day on, for looking at night without waiting for it.
- **D-57** 2026-09-21, **the user**: **generate every chunk before the intro
  runs.** The streamer asks for four chunks a frame so that walking never
  stalls, but "never stalls" and "is complete" are different promises and an
  opening sequence needs the second. The title writes its letters with
  `world_set()`, which no-ops on a chunk that is not resident, so a title that
  starts before its world exists spells half a word -- and which half depends
  on the SD card that morning. `pregenerate()` runs the streamer to completion
  inline first: 2.8 s for the title's 81 chunks, all of it behind a screen that
  is not yet showing anything.

- **D-58** 2026-09-21, Claude: **the title runs on a scratch world.** Generated
  from a fixed seed, never written, and absent from the world list. The letters
  are real blocks placed through `world_set()`, so the greedy mesher, the
  streamer and the fog treat them as what they are -- the title screen is a
  view of the game rather than a picture of one. The seed was CHOSEN, not
  picked: the first one put the camera over open ocean, so every seed under
  4000 was scored on the ground it gives across the camera's field of view.

- **D-59** 2026-09-21, Claude: **a `shots` test switches the chunk worker to
  synchronous and settles the world before drawing.** That is what D-15 put
  synchronous mode there for. Until now every shot was a photograph of empty
  sky (F-45), so the hashes covered the overlay and nothing else.

- **D-56** 2026-09-21, **the user**: **the engine's splash goes first, and the
  game's name waits for its title screen.** `se_splash()` -- the SynthEngine
  wordmark over `se_version_string()`, so the version tracks the engine instead
  of going stale in a string here. The "CraftMiner / a block world" card that
  was there was a placeholder; the game's own title belongs on the title screen
  (step 5.1), not on a second text splash the player sits through every boot.

- **D-53** 2026-09-21, Claude: **item ids below `BLK_COUNT` are block ids.** A
  stack of cobblestone is item id `BLK_COBBLE`; `id < BLK_COUNT` is the whole
  test for "this is a block". Not a coincidence to be tidied away later — it is
  what stops two registries drifting, and it is why the inventory can draw a
  block without a sprite: `block_def()` already describes it. Real items — coal,
  a pickaxe — start at `BLK_COUNT` and carry their own row.

- **D-54** 2026-09-21, Claude: **a tool's icon is a shape, not a colour.** Three
  stone tools drawn as flat squares are three near-identical grey squares, and
  picking the wrong one is then something you discover by swinging it. A handle
  plus a head in the class's outline is tellable apart at a glance and needs no
  texture. Real sprites (D-03) replace it without a caller changing.

- **D-55** 2026-09-21, Claude: **the player starts with a stone pickaxe, axe and
  shovel and some blocks.** Crafting is step 8, and without a kit neither
  durability nor break speed nor placing can be tried at all. It is six lines in
  `player_spawn` and it goes the day a crafting table can make them.

- **D-51** 2026-09-21, **the user**, before block 4 was written: **a persisted
  duration is a count of ticks elapsed — never seconds, never a timestamp.**
  Prompted by dropped items, but it is the rule for every timer the world
  saves: crop growth, furnace burn, breeding cooldown, hunger, mob despawn.

  Two failures it prevents, and the second is why the distinction is not
  pedantry:

  * **wall-clock seconds**: the player pauses, or the frame rate changes, and
    the timer no longer matches the simulation. Part T rule 1 already bans
    reading wall time inside a tick; this extends the ban to what a tick
    *stores*.
  * **an absolute "spawned at tick N", compared against a world clock**: a
    world loaded a week later is fine — the world clock only advances while
    someone is playing — but a chunk that was *unloaded* for an hour of play
    is not. Its items would come back already expired, having aged while
    nothing was simulating them. **The stored field is elapsed ticks,
    incremented by the entity's own tick**, so an unsimulated chunk's clocks
    simply stand still. That is also what Minecraft does, and it is what makes
    "walking away does not cost you your drops" true rather than aspirational.

  Width is **uint32**: 10 minutes is 12000 ticks, but a uint16 caps at 54
  minutes, which a furnace timer or a crop over a long session would reach.

  Wall-clock time survives in exactly one place — `world_meta.last_played`,
  for the world-select screen. That is a thing to show a human, not a thing a
  tick reads.

- **D-52** 2026-09-21, Claude (raised by D-51): **the world's tick count is
  world state and belongs in `level.cmw`.** `player_state_t.time_of_day` is on
  the wrong record — a world has one time of day however many players it has
  had, and a day/night cycle (step 29) reads it. Moving it is a block 5 job,
  and cheap because both records are tagged and skippable (D-30): the old
  field is simply ignored where it was and defaulted where it now lives.

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
  stays at one version while it is being worked on -- no bump per change. It
  sat at **2.1** through the rasteriser work, the menus and the font, and went
  to **2.2** when the mixer gained public calls that a game outside this one
  would want (D-85, F-75).
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

- **D-85** 2026-09-23, **the user**: **three volume sliders, and they are not
  the same kind of thing.** The existing one stays what it is -- the BADGE's
  volume, the launcher's setting, shared with every app -- and two new ones
  set "how it is mixed in" for the music and for the effects, inside the game.

  The distinction is the whole point and the menu is laid out to make it
  obvious: the device slider first, then each toggle paired with its own
  slider. Turning the badge down quietens everything including the launcher;
  turning Music down leaves the footsteps exactly where they were.

  It went in the engine rather than in our own render paths. Scaling our
  samples before the mixer's fixed gain would have been arithmetically
  identical and half the work, but the mixer is where mixing belongs, and
  `se_config.h` already owned the music-versus-effects balance -- the sliders
  ride on top of that rather than replacing it. Two calls,
  `audio_mixer_set_music_volume()` and `_set_group_volume()`, both additive,
  and the engine went to 2.2.

  One trap worth writing down: **a volume of 0 is not the same as off.** A
  silent source is still a playing source, so it still holds the speaker up
  (F-75). The enable gates remain the way to turn a class off.

- **D-86** 2026-09-23, **the user**: **water draws only its surface.** Not
  "no sides and bottom" as an optimisation -- as the definition of what a
  liquid IS in a renderer with no blending.

  The problem was stated plainly: water is an opaque cube, "that's sort of
  fine for now. But if we enter a place where the water covers our 'eyes',
  the rendering breaks down." And the fix, equally plainly: "don't render
  sides and bottoms of water blocks, and only render the top of water blocks
  when the block above them is air", plus "maybe we could adapt the water
  surface texture so it has a checkerboard of transparent pixels (sort of
  like the leaves of trees), so you can sort of see through".

  That is the rule, and `K_LIQUID` is it. Two consequences the rule implies
  but does not say, and the picture is wrong without either:

  * **a liquid hides nothing.** `face_shows` returns true against a liquid in
    both mesh modes. Water used to be K_CUBE, so the stone under a lake had
    its top face culled: remove water's own faces and all you would see is a
    surface over a void.
  * **the surface is drawn twice, up and down.** An axis-aligned face is
    visible only from the side its normal points at -- that is the cheap cull
    the whole renderer is built on. One face means the surface disappears the
    instant the eye goes under it, which is the reported bug in a different
    costume. The downward copy is emitted by the AIR cell above the water, so
    it lands on the same plane for free (`emit()` puts a +y face at y+1 and a
    -y face at y) and merges greedily like any other.

  The checkerboard was the user's idea and it is the right one: alpha here is
  one bit (se_texture.h), so "see through" can only mean holes, and a regular
  grid reads as a half-transparent sheet where the leaves' random scatter
  would read as damage. The wave crests stay solid, or the water looks torn.

  **Swimming** came with it, since water you can see into is water you will
  end up in. Buoyancy cancels almost all of gravity, jump rises and sneak
  dives, and the three speeds are derived from `phys_gravity`'s recurrence
  (`vy = (vy - g) * drag`) rather than tuned by feel, so they can be stated:
  sinking 0.6 blocks a second, swimming up 1.8, diving 3.0.

  **The blue cast, and a wrong answer corrected.** Asked how big a change it
  would be, Claude priced three options and recommended the cheap one --
  force the whole view through the flat, untextured path, which fog already
  tints, and lose the textures while submerged. The user pushed back with one
  sentence: *"But you are touching the brightness of every textured and
  non-textured pixel anyway, right?"* They were right, and the estimate was
  wrong for a specific reason worth recording: it assumed a branch inside the
  hot loop, when the engine already establishes the better pattern one
  function below, for cut-out textures -- *"A sibling rather than a flag in
  the loop above, so opaque textures run exactly the code they always did."*

  So the tint went where the shading already is. The textured loop scales all
  three RGB565 channels with ONE multiply, by spreading them into separate
  fields of a 32-bit word; one multiply cannot scale fields by different
  amounts, two can, and two is what `se_scene_set_tint()` is. Red and green
  share a factor -- which is what water does anyway, absorbing both far
  faster than blue. The factors ride on the triangle's own shade, folded in
  at setup, so the per-pixel cost really is the one extra multiply. The flat
  path costs nothing: a flat triangle is shaded once.

  It is free when off, and that is checked rather than asserted: with a tint
  of (32, 32) the two-multiply form is **bit-identical** to the one-multiply
  form over all 65536 texels at all 33 shade levels.

  The sky is filled dark blue rather than sky blue (the user asked for that
  explicitly), the fog takes the same colour -- from under the surface the
  distance IS more water -- and `voxel_sky_submit` is skipped entirely.
  The eye-in-water test is on the RENDER side, never the tick: what the
  camera is inside of must not reach the simulation (Part T).

  The engine did NOT take a version number for this. The user's call: it is
  still unreleased and still being worked on, so 2.2 collects the whole
  round (D-39's rule, restated).

## Verification

- **Host:** `make check` = `worldcheck` + `scenecheck` + `meshcheck` +
  `hostpurity`. Seconds, no badge. Runs as part of `make build`.
- **Device:** `make cycle TEST="perf scene=flyover secs=20"` for the frame rate
  and phase split; `make testrefs` / `make testcompare` with
  `TEST="shots scene=replay_walk ms=..."` for framebuffer-hash regressions.
- **Build hygiene:** `make build` clean with no new warnings, `make verify`
  (every undefined symbol exists in fakelib), `make format`.
- **By hand:** the user plays step 5 and gives feedback before step 8 starts.

### Where it stands after block 4 (2026-09-21)

`make check` runs on the host in about a second and now covers, beyond block
1's world sections: **the mesher's vertical sectioning** (a lump meshed whole
and in slices must have the same surface area and enclosed volume);
**collision** (resting, terminal-velocity tunnelling, wall slide, a step, a
staircase, a 2-block wall, a 2-high gap and a 1-high one, the edge of the
world); **the jump arc**, asserted in blocks and seconds; **picking**, against a
brute-force march over 576 directions; **the logging rule**, both ways, with the
stump and the neighbouring tree checked; and **items** -- tool speed by class,
harvest qualification, partial-stack filling, a full inventory refusing the
overflow, durability to the exact use, drops, the throw delay, despawn at
exactly 12000 ticks, and a full entity pool refusing rather than corrupting.

`scenecheck` is still not written; the budget it would guard is instead watched
by the device `perf` run's primitive counts.

**A screenshot can be looked at, not just hashed.** `badgelink fs download` on
a `shots` PNG pulls the framebuffer off the badge, which is how the crosshair's
position and the HUD were checked rather than assumed. What it cannot yet show
is the world: a `shots` run SETS the clock instead of running it, so the chunks
never stream and every shot is empty sky (F-45, step 5.8).

### Where it stands after the menus (2026-09-22)

People are already playing the pre-alpha, so this round was about their
saves and their hands: named worlds in eight slots, with the one world
earlier builds kept moved into slot 1 as *Testworld*; the player's exact
position and inventory in the save; a full menu tree -- the title's strip,
then `se_ui` panels for worlds, the new-world form, settings, controls,
graphics, audio, display and pause; every key rebindable; looking by turning
the badge; and all of the game's settings in one `settings.txt` on the SD card,
so one copy of the app directory backs up everything.

Three engine changes came with it, all in 2.1 and all leaving existing games
as they were: the key capture takes the cursor keys (F-50), list menus scroll
(`visible_rows`), and a game may persist its bindings itself (a NULL NVS
namespace). The user's rule from this round: **use the donor's code and fix
the engine, rather than working round either** -- synthracer's `keybind_ui`
and `icons` are ported as they were.

Not done in this round, and still owed from block 5: entities in the save
(5.7), replay (5.8), the world's time of day (5.9), a progress bar for
creating a world (5.5). F-52 (the palette is rewritten on every save while
untouched chunks keep their old ids) must be fixed before any block id moves.

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
  `main/main.c`, `README.md`, and `main/testkit/profile.{c,h}` (`PROF_HUD`,
  F-46).
- **Engine: CHANGED, with permission (D-39).** No longer "no changes planned",
  which it was until the user asked for the rasteriser to be looked at.
  SynthEngine3D is a submodule and its commits are its own; the version went
  to **2.2** when the mixer gained public calls (see below).
  * `include/se_ui.h`, `src/se_ui.c`: `SE_MENU_VAL_RADIO`, a ring filled on
    the chosen row, for a single-choice list (the language menu). The user's
    call: "If there is no radio button in the engine, add it."
  * `src/internal/hershey*.h`, `tools/hershey/`: text is UTF-8 and the font
    has Cyrillic, accented Latin, both dashes and the European quotation
    marks (F-69). The user's call, and their permission to amend the fonts:
    "If you need to ammend the fonts (use the full set) ... you are allowed
    to." Hershey's database is vendored with the generator that reads it;
    `simplex` itself is untouched and still public.
  * `include/se_scene.h`, `src/se_scene.c`: `se_scene_set_tint()`, a
    scene-wide colour cast for being underwater, as sibling raster loops
    so an untinted scene is unchanged (D-86).
  * `include/se_audio.h`, `src/audio_mixer.c`: per-class volume
    (`audio_mixer_set_music_volume` / `_set_group_volume`) and
    `audio_mixer_keep_awake()`, which holds the amplifier up through the
    quiet so a short one-shot is not eaten by its turn-on (D-85, F-75).
    The engine is **2.2** from here.
  * `CMakeLists.txt`: built `-O2`, not `-Os` (F-39).
  * `src/se_scene.c`: `ceil_i` / `floor_i` instead of the libm calls in the
    column scans, and per-pass pixel and span counters.
  * `include/se_scene.h`: `scene_fill_stats()` -- the one public addition.
  * `CHANGELOG.md`: all of the above, with the measurements.
  The standing rule still holds for everything NOT asked for: an engine problem
  that turns up in passing is still stop-and-ask.
