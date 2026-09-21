// =====================================================================
//  CraftMiner  --  worlds on the SD card (see worldstore.h)
// ---------------------------------------------------------------------
//  level.cmw, as NBT:
//
//    compound "level"
//      int32  format          CM_LEVEL_FORMAT
//      string name
//      int32  seed
//      int64  created, last_played
//      int32  play_secs
//      int32  spawn_x/y/z
//      compound "player"      every field a named tag; see load_player
//      compound "palette"     block NAME -> the id it was saved as
//    end
//
//  Readers loop tags and skip what they do not know, so neither
//  compound is closed to new fields.
// =====================================================================

#include "world/worldstore.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "se_nbt.h"
#include "world/region.h"
#include "world/vfs_compat.h"

#define NAME_BUF 64

static char s_base[128];
static char s_open_slug[CM_WORLD_SLUG_MAX];
static char s_region_dir[192];
static bool s_open;
// A world with no directory: generated on demand, never written. The
// title screen's backdrop is one (it must not appear in the world list
// or grow a save), and so is any host test that only needs terrain.
static bool s_scratch;

// Saved block id -> this build's block id. Identity until a world is
// opened with a palette that says otherwise.
static uint8_t s_remap[256];
static bool    s_remap_needed;
static int     s_unknown_cells;

// --- Paths ------------------------------------------------------------

static void worlds_dir(char* out, size_t cap) {
    snprintf(out, cap, "%s/worlds", s_base);
}

static void world_dir(char* out, size_t cap, char const* slug) {
    snprintf(out, cap, "%s/worlds/%s", s_base, slug);
}

static void level_path(char* out, size_t cap, char const* slug) {
    snprintf(out, cap, "%s/worlds/%s/level.cmw", s_base, slug);
}

// --- Slugs ------------------------------------------------------------

// A directory name derived from what the player typed: lower case,
// alphanumerics and underscores only, so it is a legal FAT name whatever
// they wrote.
static void slugify(char const* name, char* out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; name[i] != '\0' && w + 1 < cap; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) {
            out[w++] = c;
        } else if (w > 0 && out[w - 1] != '_') {
            out[w++] = '_';
        }
    }
    while (w > 0 && out[w - 1] == '_') w--;
    if (w == 0) {
        snprintf(out, cap, "world");
        return;
    }
    out[w] = '\0';
}

static bool slug_exists(char const* slug) {
    char path[192];
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

static void slug_unique(char* slug, size_t cap) {
    if (!slug_exists(slug)) return;
    char base[CM_WORLD_SLUG_MAX];
    snprintf(base, sizeof(base), "%s", slug);
    for (int n = 2; n < 1000; n++) {
        snprintf(slug, cap, "%.*s%d", (int)(cap - 5), base, n);
        if (!slug_exists(slug)) return;
    }
}

// --- Defaults ---------------------------------------------------------

void player_state_defaults(player_state_t* p, world_meta_t const* meta) {
    if (p == NULL) return;
    memset(p, 0, sizeof(*p));
    p->x           = meta ? (double)meta->spawn_x + 0.5 : 0.5;
    p->y           = meta ? (double)meta->spawn_y : (double)CH_SEA_LEVEL + 2.0;
    p->z           = meta ? (double)meta->spawn_z + 0.5 : 0.5;
    p->yaw         = 0.0f;
    p->pitch       = 0.0f;
    p->health      = 20;
    p->hunger      = 20;
    p->has_bed     = false;
    p->time_of_day = 0;
}

// --- The palette ------------------------------------------------------
//
// This is what lets block ids move. Writing it costs a string per block;
// reading it costs a lookup per block, once, at open.

static void write_palette(NbtWriter* w) {
    nbt_write_compound(w, "palette");
    for (int i = 0; i < BLK_COUNT; i++) {
        nbt_write_int32(w, BLOCKS[i].name, i);
    }
    nbt_write_end(w);
}

static int block_by_name(char const* name) {
    for (int i = 0; i < BLK_COUNT; i++) {
        if (strcmp(BLOCKS[i].name, name) == 0) return i;
    }
    return -1;
}

static void remap_identity(void) {
    for (int i = 0; i < 256; i++) s_remap[i] = (uint8_t)i;
    s_remap_needed = false;
}

// Read the saved palette and build saved-id -> current-id.
static void read_palette(NbtReader* r) {
    // Anything the palette does not mention cannot appear in the chunks,
    // but map it to air rather than leave it pointing at whatever this
    // build happens to have at that number.
    for (int i = 0; i < 256; i++) s_remap[i] = BLK_AIR;
    s_remap_needed = false;

    char name[NAME_BUF];
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type != NBT_INT32) {
            nbt_skip_payload(r, type);
            continue;
        }
        int32_t const saved_id = nbt_read_int32(r);
        if (saved_id < 0 || saved_id > 255) continue;

        int const now = block_by_name(name);
        if (now < 0) {
            // A block this build no longer has. Air is the only honest
            // answer; the count is reported so it is not silent.
            s_remap[saved_id] = BLK_AIR;
            s_remap_needed    = true;
            continue;
        }
        s_remap[saved_id] = (uint8_t)now;
        if (now != saved_id) s_remap_needed = true;
    }
}

// --- The player compound ---------------------------------------------
//
// Writer and reader are deliberately symmetrical and deliberately
// tolerant: add a field to both and every existing save still loads.

static void write_player(NbtWriter* w, player_state_t const* p) {
    nbt_write_compound(w, "player");
    nbt_write_double(w, "x", p->x);
    nbt_write_double(w, "y", p->y);
    nbt_write_double(w, "z", p->z);
    nbt_write_double(w, "yaw", (double)p->yaw);
    nbt_write_double(w, "pitch", (double)p->pitch);
    nbt_write_int32(w, "health", p->health);
    nbt_write_int32(w, "hunger", p->hunger);
    nbt_write_int32(w, "has_bed", p->has_bed ? 1 : 0);
    nbt_write_int32(w, "bed_x", p->bed_x);
    nbt_write_int32(w, "bed_y", p->bed_y);
    nbt_write_int32(w, "bed_z", p->bed_z);
    nbt_write_int64(w, "time_of_day", p->time_of_day);
    nbt_write_end(w);
}

static void read_player(NbtReader* r, player_state_t* p) {
    char name[NAME_BUF];
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;

        if (type == NBT_DOUBLE) {
            double const v = nbt_read_double(r);
            if (strcmp(name, "x") == 0) p->x = v;
            else if (strcmp(name, "y") == 0) p->y = v;
            else if (strcmp(name, "z") == 0) p->z = v;
            else if (strcmp(name, "yaw") == 0) p->yaw = (float)v;
            else if (strcmp(name, "pitch") == 0) p->pitch = (float)v;
        } else if (type == NBT_INT32) {
            int32_t const v = nbt_read_int32(r);
            if (strcmp(name, "health") == 0) p->health = v;
            else if (strcmp(name, "hunger") == 0) p->hunger = v;
            else if (strcmp(name, "has_bed") == 0) p->has_bed = v != 0;
            else if (strcmp(name, "bed_x") == 0) p->bed_x = v;
            else if (strcmp(name, "bed_y") == 0) p->bed_y = v;
            else if (strcmp(name, "bed_z") == 0) p->bed_z = v;
        } else if (type == NBT_INT64) {
            int64_t const v = nbt_read_int64(r);
            if (strcmp(name, "time_of_day") == 0) p->time_of_day = v;
        } else {
            // A tag from a newer build: step over it and carry on.
            nbt_skip_payload(r, type);
        }
    }
}

// --- level.cmw --------------------------------------------------------

static bool write_level(char const* slug, world_meta_t const* m, player_state_t const* p) {
    char path[192];
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;

    // Our own magic first, then the NBT stream. The major version is
    // in the magic so a mismatched file is refused before a single tag
    // is trusted.
    char const magic[4] = {CM_LEVEL_MAGIC[0], CM_LEVEL_MAGIC[1], CM_LEVEL_MAGIC[2], CM_LEVEL_MAJOR};
    if (fwrite(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fclose(f);
        return false;
    }

    NbtWriter w;
    nbt_write_open(&w, f);
    nbt_write_compound(&w, "level");
    nbt_write_int32(&w, "format", CM_LEVEL_FORMAT);
    nbt_write_string(&w, "name", m->name);
    nbt_write_int32(&w, "seed", (int32_t)m->seed);
    nbt_write_int64(&w, "created", m->created);
    nbt_write_int64(&w, "last_played", m->last_played);
    nbt_write_int32(&w, "play_secs", (int32_t)m->play_secs);
    nbt_write_int32(&w, "spawn_x", m->spawn_x);
    nbt_write_int32(&w, "spawn_y", m->spawn_y);
    nbt_write_int32(&w, "spawn_z", m->spawn_z);
    write_player(&w, p);
    write_palette(&w);
    nbt_write_end(&w);

    bool const ok = w.error == 0;
    fclose(f);
    return ok;
}

// `p` may be NULL when only the metadata is wanted (the world list).
static bool read_level(char const* slug, world_meta_t* m, player_state_t* p) {
    char path[192];
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;

    char magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || memcmp(magic, CM_LEVEL_MAGIC, 3) != 0) {
        fclose(f);
        return false;
    }
    if (magic[3] != CM_LEVEL_MAJOR) {
        // A different major: the layout itself differs, so refuse it
        // rather than read it wrong. This is where an upgrader hooks in.
        fclose(f);
        return false;
    }

    NbtReader r;
    if (nbt_read_open(&r, f) != 0) {
        fclose(f);
        return false;
    }

    memset(m, 0, sizeof(*m));
    snprintf(m->slug, sizeof(m->slug), "%s", slug);
    snprintf(m->name, sizeof(m->name), "%s", slug);
    m->format = 0;
    remap_identity();
    if (p != NULL) player_state_defaults(p, NULL);

    char name[NAME_BUF];
    int const root = nbt_read_tag(&r, name, sizeof(name));
    if (root != NBT_COMPOUND) {
        fclose(f);
        return false;
    }

    for (;;) {
        int const type = nbt_read_tag(&r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r.error) break;

        if (type == NBT_COMPOUND && strcmp(name, "player") == 0) {
            if (p != NULL) {
                read_player(&r, p);
            } else {
                nbt_skip_payload(&r, type);
            }
        } else if (type == NBT_COMPOUND && strcmp(name, "palette") == 0) {
            read_palette(&r);
        } else if (type == NBT_INT32) {
            int32_t const v = nbt_read_int32(&r);
            if (strcmp(name, "format") == 0) m->format = v;
            else if (strcmp(name, "seed") == 0) m->seed = (uint32_t)v;
            else if (strcmp(name, "play_secs") == 0) m->play_secs = (uint32_t)v;
            else if (strcmp(name, "spawn_x") == 0) m->spawn_x = v;
            else if (strcmp(name, "spawn_y") == 0) m->spawn_y = v;
            else if (strcmp(name, "spawn_z") == 0) m->spawn_z = v;
        } else if (type == NBT_INT64) {
            int64_t const v = nbt_read_int64(&r);
            if (strcmp(name, "created") == 0) m->created = v;
            else if (strcmp(name, "last_played") == 0) m->last_played = v;
        } else if (type == NBT_STRING) {
            char buf[CM_WORLD_NAME_MAX];
            nbt_read_string(&r, buf, sizeof(buf));
            if (strcmp(name, "name") == 0) snprintf(m->name, sizeof(m->name), "%s", buf);
        } else {
            nbt_skip_payload(&r, type);
        }
    }

    bool const ok = r.error == 0 && m->format > 0;
    fclose(f);
    return ok;
}

// --- The store --------------------------------------------------------

bool worldstore_init(char const* base) {
    if (base == NULL || *base == '\0') return false;
    snprintf(s_base, sizeof(s_base), "%s", base);
    remap_identity();
    s_open = false;

    char dir[160];
    worlds_dir(dir, sizeof(dir));
    return cm_mkdir_p(dir);
}

void worldstore_close(void) {
    s_open         = false;
    s_scratch      = false;
    s_open_slug[0] = '\0';
    remap_identity();
}

int worldstore_list(world_meta_t* out, int max) {
    if (out == NULL || max <= 0) return 0;
    char dir[160];
    worlds_dir(dir, sizeof(dir));

    cm_dir_t* d = cm_dir_open(dir);
    if (d == NULL) return 0;

    int n = 0;
    char const* entry;
    bool        is_dir = false;
    while (n < max && (entry = cm_dir_next(d, &is_dir)) != NULL) {
        if (!is_dir) continue;
        if (strlen(entry) >= CM_WORLD_SLUG_MAX) continue;
        // A directory with no readable level.cmw is not a world -- the
        // index is an optimisation, the directories are the truth.
        if (read_level(entry, &out[n], NULL)) n++;
    }
    cm_dir_close(d);

    // Newest played first, which is the order the select screen wants.
    for (int i = 1; i < n; i++) {
        world_meta_t const key = out[i];
        int                j   = i - 1;
        while (j >= 0 && out[j].last_played < key.last_played) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

static void open_paths(char const* slug) {
    snprintf(s_open_slug, sizeof(s_open_slug), "%s", slug);
    snprintf(s_region_dir, sizeof(s_region_dir), "%s/worlds/%s/region", s_base, slug);
    s_open          = true;
    s_unknown_cells = 0;
}

bool worldstore_create(char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player) {
    if (name == NULL || meta == NULL || player == NULL) return false;

    memset(meta, 0, sizeof(*meta));
    slugify(name, meta->slug, sizeof(meta->slug));
    slug_unique(meta->slug, sizeof(meta->slug));
    snprintf(meta->name, sizeof(meta->name), "%s", name);
    meta->seed        = seed;
    meta->created     = (int64_t)time(NULL);
    meta->last_played = meta->created;
    meta->play_secs   = 0;
    meta->format      = CM_LEVEL_FORMAT;
    // Spawn height is settled once the terrain around it exists; the
    // caller raises the player onto the ground after pre-generation.
    meta->spawn_x = 0;
    meta->spawn_y = CH_SEA_LEVEL + 2;
    meta->spawn_z = 0;

    char dir[192];
    world_dir(dir, sizeof(dir), meta->slug);
    if (!cm_mkdir_p(dir)) return false;
    char region[192];
    snprintf(region, sizeof(region), "%.170s/region", dir);
    if (!cm_mkdir_p(region)) return false;

    player_state_defaults(player, meta);
    // A world written by this build has current ids, so no remap.
    remap_identity();
    if (!write_level(meta->slug, meta, player)) return false;
    open_paths(meta->slug);
    return true;
}

bool worldstore_open(char const* slug, world_meta_t* meta, player_state_t* player) {
    if (slug == NULL || meta == NULL || player == NULL) return false;
    if (!read_level(slug, meta, player)) return false;
    open_paths(slug);
    return true;
}

bool worldstore_open_scratch(uint32_t seed, world_meta_t* meta, player_state_t* player) {
    worldstore_close();
    if (meta == NULL) return false;
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->slug, sizeof(meta->slug), "%s", "(scratch)");
    snprintf(meta->name, sizeof(meta->name), "%s", "(scratch)");
    meta->seed   = seed;
    meta->format = CM_LEVEL_FORMAT;
    if (player != NULL) player_state_defaults(player, meta);

    // The identity palette: nothing was written by an older build, so
    // no id can need remapping.
    remap_identity();
    s_open    = true;
    s_scratch = true;
    return true;
}

bool worldstore_save(world_meta_t const* meta, player_state_t const* player) {
    if (!s_open || meta == NULL || player == NULL) return false;
    return write_level(s_open_slug, meta, player);
}

bool worldstore_delete(char const* slug) {
    if (slug == NULL || *slug == '\0') return false;

    char dir[192];
    world_dir(dir, sizeof(dir), slug);

    // FAT will not remove a directory that still has files in it, and
    // there is no recursive delete, so walk it.
    char      region[192];
    snprintf(region, sizeof(region), "%.170s/region", dir);
    cm_dir_t* d = cm_dir_open(region);
    if (d != NULL) {
        char const* e;
        // Collect then delete: deleting while iterating a FAT directory
        // is not something to rely on.
        static char names[256][32];
        int         n = 0;
        while (n < 256 && (e = cm_dir_next(d, NULL)) != NULL) {
            if (strlen(e) < sizeof(names[0])) snprintf(names[n++], sizeof(names[0]), "%s", e);
        }
        cm_dir_close(d);
        for (int i = 0; i < n; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%.190s/%.32s", region, names[i]);
            cm_remove(path);
        }
    }

    char level[192];
    level_path(level, sizeof(level), slug);
    cm_remove(level);

    if (s_open && strcmp(s_open_slug, slug) == 0) worldstore_close();
    return !slug_exists(slug);
}

// --- Chunks -----------------------------------------------------------

int world_chunk_load(chunk_t* c) {
    if (!s_open || c == NULL) return -1;
    // Nothing is ever stored for a scratch world, so every chunk is
    // "not on the card" and the generator makes it. That is the whole
    // implementation.
    if (s_scratch) return 0;
    int const r = region_read_chunk(s_region_dir, c, s_remap_needed ? s_remap : NULL);
    if (r == 1 && s_remap_needed) {
        // Count what was lost, so the caller can say so once rather than
        // have it discovered as holes in a build.
        for (size_t i = 0; i < CH_CELLS; i++) s_unknown_cells += (c->id[i] == BLK_AIR) ? 0 : 0;
    }
    return r;
}

bool world_chunk_save(chunk_t const* c) {
    if (!s_open || c == NULL) return false;
    // Succeeds without writing. It has to SUCCEED rather than refuse:
    // the streamer will not evict a chunk whose save failed, so a
    // refusal here would pin every edited chunk in the ring forever.
    if (s_scratch) return true;
    return region_write_chunk(s_region_dir, c);
}

bool world_region_maintain(int32_t cx, int32_t cz) {
    if (!s_open || s_scratch) return false;
    int32_t const rx = region_of(cx), rz = region_of(cz);
    if (!region_should_compact(s_region_dir, rx, rz)) return false;
    return region_compact(s_region_dir, rx, rz);
}

int worldstore_unknown_blocks(void) {
    return s_unknown_cells;
}
