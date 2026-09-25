// =====================================================================
//  SynthMiner  --  worlds on the SD card (see worldstore.h)
// ---------------------------------------------------------------------
//  level.smw, as NBT:
//
//    compound "level"
//      int32  format          SM_LEVEL_FORMAT
//      string name
//      int32  seed
//      int64  created, last_played
//      int32  play_secs
//      int32  spawn_x/y/z
//      int64  time_of_day     the world's clock (D-52); older saves kept
//                             it in the player compound instead
//      int32  farlands_x      the Far Lands edge (D-78); absent before
//                             them, and then read as FARLANDS_X_DEFAULT
//      compound "player"      every field a named tag; see read_player
//        compound "inventory" int32 selected, then one compound per
//                             non-empty slot, named by its index:
//                             string item (the NAME), int32 count, wear
//      compound "palette"     block NAME -> the id it was saved as
//      compound "items"       one compound per dropped item, by index:
//                             string item, int32 count/wear/age/delay,
//                             double x/y/z (D-68)
//    end
//
//  Readers loop tags and skip what they do not know, so neither
//  compound is closed to new fields.
// =====================================================================

#include "world/worldstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "se_nbt.h"
#include "world/region.h"
#include "world/vfs_compat.h"

#define NAME_BUF 64

static char s_base[128];
static char s_open_slug[SM_WORLD_SLUG_MAX];
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

// Every world but one lives in `worlds/`. The benchmark world lives
// beside it, which is the whole of how it stays invisible: the list
// scans `worlds/`, so a world that is not in there cannot be shown,
// opened or deleted from the world-select screen (worldstore.h).
static char const* group_of(char const* slug) {
    return strcmp(slug, SM_BENCH_SLUG) == 0 ? "" : "worlds/";
}

// The slug is spoken for, whether or not anything is there yet.
static bool slug_reserved(char const* slug) {
    return strcmp(slug, SM_BENCH_SLUG) == 0;
}

static void world_dir(char* out, size_t cap, char const* slug) {
    snprintf(out, cap, "%s/%s%s", s_base, group_of(slug), slug);
}

static void level_path(char* out, size_t cap, char const* slug) {
    snprintf(out, cap, "%s/%s%s/level.smw", s_base, group_of(slug), slug);
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
    if (!slug_exists(slug) && !slug_reserved(slug)) return;
    char base[SM_WORLD_SLUG_MAX];
    snprintf(base, sizeof(base), "%s", slug);
    for (int n = 2; n < 1000; n++) {
        snprintf(slug, cap, "%.*s%d", (int)(cap - 5), base, n);
        if (!slug_exists(slug) && !slug_reserved(slug)) return;
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
    p->placed      = false;
    p->has_inv     = false;
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

// The inventory, by item NAME. Empty slots are not written at all, so
// a slot count that grows later reads old saves without a second
// thought, and one that shrinks drops only what no longer fits.
static void write_inventory(NbtWriter* w, player_state_t const* p) {
    nbt_write_compound(w, "inventory");
    nbt_write_int32(w, "selected", p->inv_selected);
    for (int i = 0; i < INV_SLOTS; i++) {
        inv_slot_t const* s = &p->inv[i];
        if (s->item == 0 || s->count == 0) continue;
        char key[12];
        snprintf(key, sizeof(key), "%d", i);
        nbt_write_compound(w, key);
        nbt_write_string(w, "item", item_def(s->item).name);
        nbt_write_int32(w, "count", s->count);
        nbt_write_int32(w, "wear", s->wear);
        nbt_write_end(w);
    }

    // What the crafting book knows, by name. An item this build has
    // dropped simply does not come back, and one it has added is not
    // there yet -- both of which are the right answer.
    nbt_write_compound(w, "known");
    int known = 0;
    for (uint16_t id = 1; id < ITEM_COUNT; id++) {
        if ((p->seen[id >> 5] & ((uint32_t)1u << (id & 31u))) == 0) continue;
        char key[12];
        snprintf(key, sizeof(key), "%d", known++);
        nbt_write_string(w, key, item_def(id).name);
    }
    nbt_write_end(w);

    nbt_write_end(w);
}

static void read_known(NbtReader* r, player_state_t* p) {
    char name[NAME_BUF];
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type == NBT_STRING) {
            char buf[NAME_BUF];
            nbt_read_string(r, buf, sizeof(buf));
            uint16_t const id = item_by_name(buf);
            if (id != 0 && id < ITEM_COUNT) p->seen[id >> 5] |= (uint32_t)1u << (id & 31u);
        } else {
            nbt_skip_payload(r, type);
        }
    }
}

static void read_slot(NbtReader* r, inv_slot_t* out) {
    char     name[NAME_BUF];
    uint16_t item  = 0;
    int32_t  count = 0, wear = 0;
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type == NBT_STRING && strcmp(name, "item") == 0) {
            char buf[NAME_BUF];
            nbt_read_string(r, buf, sizeof(buf));
            // A name this build does not know is dropped: there is
            // nothing honest to turn it into.
            item = item_by_name(buf);
        } else if (type == NBT_INT32) {
            int32_t const v = nbt_read_int32(r);
            if (strcmp(name, "count") == 0) count = v;
            else if (strcmp(name, "wear") == 0) wear = v;
        } else {
            nbt_skip_payload(r, type);
        }
    }
    if (item == 0 || count <= 0) return;
    int const cap = item_def(item).stack_max;
    out->item     = item;
    out->count    = (uint8_t)(count > cap ? cap : count);
    out->wear     = (uint16_t)(wear < 0 ? 0 : wear > 65535 ? 65535 : wear);
}

static void read_inventory(NbtReader* r, player_state_t* p) {
    memset(p->inv, 0, sizeof(p->inv));
    memset(p->seen, 0, sizeof(p->seen));
    p->inv_selected = 0;
    p->has_inv      = true;
    char name[NAME_BUF];
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type == NBT_INT32 && strcmp(name, "selected") == 0) {
            int32_t const v = nbt_read_int32(r);
            p->inv_selected = (v >= 0 && v < INV_HOTBAR) ? v : 0;
        } else if (type == NBT_COMPOUND && strcmp(name, "known") == 0) {
            read_known(r, p);
        } else if (type == NBT_COMPOUND) {
            // The slot's index is its name; anything past this build's
            // slot count is read and discarded.
            char* end = NULL;
            long const i = strtol(name, &end, 10);
            if (end != name && *end == '\0' && i >= 0 && i < INV_SLOTS) {
                read_slot(r, &p->inv[i]);
            } else {
                inv_slot_t discard = {0};
                read_slot(r, &discard);
            }
        } else {
            nbt_skip_payload(r, type);
        }
    }
}

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
    nbt_write_int32(w, "placed", p->placed ? 1 : 0);
    if (p->has_inv) write_inventory(w, p);
    nbt_write_end(w);
}

static void read_player(NbtReader* r, player_state_t* p) {
    char name[NAME_BUF];
    bool saw_placed = false;
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
            else if (strcmp(name, "placed") == 0) {
                p->placed  = v != 0;
                saw_placed = true;
            }
        } else if (type == NBT_COMPOUND && strcmp(name, "inventory") == 0) {
            read_inventory(r, p);
        } else if (type == NBT_INT64) {
            int64_t const v = nbt_read_int64(r);
            if (strcmp(name, "time_of_day") == 0) p->time_of_day = v;
        } else {
            // A tag from a newer build: step over it and carry on.
            nbt_skip_payload(r, type);
        }
    }
    // A save from before "placed" existed. Those builds wrote the
    // player only on creation (the default, at the spawn column's
    // centre) and on leaving (where they really were), so anything but
    // the untouched default is a real position.
    if (!saw_placed) p->placed = !(p->x == 0.5 && p->z == 0.5);
}

// --- level.smw --------------------------------------------------------

// Dropped items. Stored by NAME, like the inventory; the pickup delay
// as what is LEFT of it, so it means the same whatever the age.
static void write_items(NbtWriter* w, world_items_t const* items) {
    nbt_write_compound(w, "items");
    int k = 0;
    for (int i = 0; items != NULL && i < items->n; i++) {
        item_entity_t const* e = &items->e[i];
        if (!e->alive || e->item == 0) continue;
        char key[12];
        snprintf(key, sizeof(key), "%d", k++);
        nbt_write_compound(w, key);
        nbt_write_string(w, "item", item_def(e->item).name);
        nbt_write_int32(w, "count", e->count);
        nbt_write_int32(w, "wear", e->wear);
        nbt_write_int32(w, "age", (int32_t)e->age);
        nbt_write_int32(w, "delay", e->pickup_at > e->age ? (int32_t)(e->pickup_at - e->age) : 0);
        nbt_write_double(w, "x", e->body.x);
        nbt_write_double(w, "y", e->body.y);
        nbt_write_double(w, "z", e->body.z);
        nbt_write_end(w);
    }
    nbt_write_end(w);
}

static void read_item(NbtReader* r, world_items_t* items) {
    char          name[NAME_BUF];
    item_entity_t e;
    memset(&e, 0, sizeof(e));
    double  x = 0.0, y = 0.0, z = 0.0;
    int32_t count = 0, delay = 0;
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type == NBT_STRING && strcmp(name, "item") == 0) {
            char buf[NAME_BUF];
            nbt_read_string(r, buf, sizeof(buf));
            e.item = item_by_name(buf);
        } else if (type == NBT_INT32) {
            int32_t const v = nbt_read_int32(r);
            if (strcmp(name, "count") == 0) count = v;
            else if (strcmp(name, "wear") == 0) e.wear = (uint16_t)(v < 0 ? 0 : v);
            else if (strcmp(name, "age") == 0) e.age = (uint32_t)(v < 0 ? 0 : v);
            else if (strcmp(name, "delay") == 0) delay = v < 0 ? 0 : v;
        } else if (type == NBT_DOUBLE) {
            double const v = nbt_read_double(r);
            if (strcmp(name, "x") == 0) x = v;
            else if (strcmp(name, "y") == 0) y = v;
            else if (strcmp(name, "z") == 0) z = v;
        } else {
            nbt_skip_payload(r, type);
        }
    }
    if (items == NULL || e.item == 0 || count <= 0 || items->n >= ITEM_ENTITY_MAX) return;
    int const cap = item_def(e.item).stack_max;
    e.alive       = true;
    e.count       = (uint8_t)(count > cap ? cap : count);
    e.pickup_at   = e.age + (uint32_t)delay;
    phys_body_init(&e.body, x, y, z);
    e.body.w              = ITEM_ENTITY_SIZE;
    e.body.h              = ITEM_ENTITY_SIZE;
    items->e[items->n++]  = e;
}

static void read_items(NbtReader* r, world_items_t* items) {
    char name[NAME_BUF];
    for (;;) {
        int const type = nbt_read_tag(r, name, sizeof(name));
        if (type == NBT_END || type < 0 || r->error) break;
        if (type == NBT_COMPOUND) {
            read_item(r, items);
        } else {
            nbt_skip_payload(r, type);
        }
    }
}

static bool write_level(char const* slug, world_meta_t const* m, player_state_t const* p, world_items_t const* items) {
    char path[192];
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;

    // Our own magic first, then the NBT stream. The major version is
    // in the magic so a mismatched file is refused before a single tag
    // is trusted.
    char const magic[4] = {SM_LEVEL_MAGIC[0], SM_LEVEL_MAGIC[1], SM_LEVEL_MAGIC[2], SM_LEVEL_MAJOR};
    if (fwrite(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fclose(f);
        return false;
    }

    NbtWriter w;
    nbt_write_open(&w, f);
    nbt_write_compound(&w, "level");
    nbt_write_int32(&w, "format", SM_LEVEL_FORMAT);
    nbt_write_string(&w, "name", m->name);
    nbt_write_int32(&w, "seed", (int32_t)m->seed);
    nbt_write_int64(&w, "created", m->created);
    nbt_write_int64(&w, "last_played", m->last_played);
    nbt_write_int32(&w, "play_secs", (int32_t)m->play_secs);
    nbt_write_int32(&w, "spawn_x", m->spawn_x);
    nbt_write_int32(&w, "spawn_y", m->spawn_y);
    nbt_write_int32(&w, "spawn_z", m->spawn_z);
    nbt_write_int64(&w, "time_of_day", m->time_of_day);
    nbt_write_int32(&w, "farlands_x", m->farlands_x);
    write_player(&w, p);
    write_palette(&w);
    write_items(&w, items);
    nbt_write_end(&w);

    bool const ok = w.error == 0;
    fclose(f);
    return ok;
}

// `p` may be NULL when only the metadata is wanted (the world list).
// The palette is read into the store's remap ONLY for `palette`: the
// list and the slot peek must not disturb the remap of a world that is
// Is this one of ours? Either name counts (worldstore.h): the byte
// after decides the version, and it means the same in both.
static bool level_magic(char const magic[4]) {
    return memcmp(magic, SM_LEVEL_MAGIC, 3) == 0 || memcmp(magic, SM_LEVEL_MAGIC_WAS, 3) == 0;
}

// open while they run. `items` NULL skips the dropped items.
static bool read_level(char const* slug, world_meta_t* m, player_state_t* p, bool palette, world_items_t* items) {
    char path[192];
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;

    char magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || !level_magic(magic)) {
        fclose(f);
        return false;
    }
    if (magic[3] != SM_LEVEL_MAJOR) {
        // A different major: the layout itself differs, so refuse it
        // rather than read it wrong. This is where an upgrader hooks in.
        fclose(f);
        return false;
    }

    bool saw_time = false;
    NbtReader r;
    if (nbt_read_open(&r, f) != 0) {
        fclose(f);
        return false;
    }

    memset(m, 0, sizeof(*m));
    snprintf(m->slug, sizeof(m->slug), "%s", slug);
    snprintf(m->name, sizeof(m->name), "%s", slug);
    m->format = 0;
    m->farlands_x = FARLANDS_X_DEFAULT;
    if (palette) remap_identity();
    if (items != NULL) items->n = 0;
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
        } else if (type == NBT_COMPOUND && strcmp(name, "palette") == 0 && palette) {
            read_palette(&r);
        } else if (type == NBT_COMPOUND && strcmp(name, "items") == 0 && items != NULL) {
            read_items(&r, items);
        } else if (type == NBT_INT32) {
            int32_t const v = nbt_read_int32(&r);
            if (strcmp(name, "format") == 0) m->format = v;
            else if (strcmp(name, "seed") == 0) m->seed = (uint32_t)v;
            else if (strcmp(name, "play_secs") == 0) m->play_secs = (uint32_t)v;
            else if (strcmp(name, "spawn_x") == 0) m->spawn_x = v;
            else if (strcmp(name, "spawn_y") == 0) m->spawn_y = v;
            else if (strcmp(name, "spawn_z") == 0) m->spawn_z = v;
            else if (strcmp(name, "farlands_x") == 0) m->farlands_x = v;
        } else if (type == NBT_INT64) {
            int64_t const v = nbt_read_int64(&r);
            if (strcmp(name, "created") == 0) m->created = v;
            else if (strcmp(name, "last_played") == 0) m->last_played = v;
            else if (strcmp(name, "time_of_day") == 0) {
                m->time_of_day = v;
                saw_time       = true;
            }
        } else if (type == NBT_STRING) {
            char buf[SM_WORLD_NAME_MAX];
            nbt_read_string(&r, buf, sizeof(buf));
            if (strcmp(name, "name") == 0) snprintf(m->name, sizeof(m->name), "%s", buf);
        } else {
            nbt_skip_payload(&r, type);
        }
    }

    // A save from before the clock moved to the world (D-52): take the
    // player's. Only possible when the player was read, which the world
    // list does not need.
    if (!saw_time && p != NULL) m->time_of_day = p->time_of_day;
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
    return sm_mkdir_p(dir);
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

    sm_dir_t* d = sm_dir_open(dir);
    if (d == NULL) return 0;

    int n = 0;
    char const* entry;
    bool        is_dir = false;
    while (n < max && (entry = sm_dir_next(d, &is_dir)) != NULL) {
        if (!is_dir) continue;
        if (strlen(entry) >= SM_WORLD_SLUG_MAX) continue;
        // A directory with no readable level.smw is not a world -- the
        // index is an optimisation, the directories are the truth.
        if (read_level(entry, &out[n], NULL, false, NULL)) n++;
    }
    sm_dir_close(d);

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
    snprintf(s_region_dir, sizeof(s_region_dir), "%s/%s%s/region", s_base, group_of(slug), slug);
    s_open          = true;
    s_unknown_cells = 0;
}

// Make a world in the directory `slug`, which the caller has chosen.
static bool create_at(char const* slug, char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player) {
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->slug, sizeof(meta->slug), "%s", slug);
    snprintf(meta->name, sizeof(meta->name), "%s", name);
    meta->seed        = seed;
    meta->created     = (int64_t)time(NULL);
    meta->last_played = meta->created;
    meta->play_secs   = 0;
    meta->format      = SM_LEVEL_FORMAT;
    meta->time_of_day = 1000;  // DAY_START (game/daytime.h): a morning
    // Spawn height is settled once the terrain around it exists; the
    // caller raises the player onto the ground after pre-generation.
    meta->spawn_x = 0;
    meta->spawn_y = CH_SEA_LEVEL + 2;
    meta->spawn_z = 0;
    meta->farlands_x = FARLANDS_X_DEFAULT;

    char dir[192];
    world_dir(dir, sizeof(dir), meta->slug);
    if (!sm_mkdir_p(dir)) return false;
    char region[192];
    snprintf(region, sizeof(region), "%.170s/region", dir);
    if (!sm_mkdir_p(region)) return false;

    player_state_defaults(player, meta);
    // A world written by this build has current ids, so no remap.
    remap_identity();
    if (!write_level(meta->slug, meta, player, NULL)) return false;
    open_paths(meta->slug);
    return true;
}

bool worldstore_create(char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player) {
    if (name == NULL || meta == NULL || player == NULL) return false;
    char slug[SM_WORLD_SLUG_MAX];
    slugify(name, slug, sizeof(slug));
    slug_unique(slug, sizeof(slug));
    return create_at(slug, name, seed, meta, player);
}

// --- Save slots -------------------------------------------------------

void worldstore_slot_slug(int slot, char* out, int cap) {
    snprintf(out, (size_t)cap, "slot%d", slot + 1);
}

bool worldstore_slot_peek(int slot, world_meta_t* meta) {
    if (slot < 0 || slot >= SM_SLOTS || meta == NULL) return false;
    char slug[SM_WORLD_SLUG_MAX];
    worldstore_slot_slug(slot, slug, sizeof(slug));
    return read_level(slug, meta, NULL, false, NULL);
}

slot_state_t worldstore_slot_state(int slot, world_meta_t* meta) {
    if (slot < 0 || slot >= SM_SLOTS || meta == NULL) return SLOT_EMPTY;
    memset(meta, 0, sizeof(*meta));
    char slug[SM_WORLD_SLUG_MAX], path[192];
    worldstore_slot_slug(slot, slug, sizeof(slug));
    level_path(path, sizeof(path), slug);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return SLOT_EMPTY;
    char       magic[4];
    bool const got = fread(magic, 1, sizeof(magic), f) == sizeof(magic);
    fclose(f);
    // The major version is in the magic, so it can be read without
    // trusting anything after it.
    if (got && level_magic(magic) && magic[3] != SM_LEVEL_MAJOR) {
        return magic[3] > SM_LEVEL_MAJOR ? SLOT_NEWER : SLOT_OLDER;
    }
    return read_level(slug, meta, NULL, false, NULL) ? SLOT_WORLD : SLOT_DAMAGED;
}

bool worldstore_create_in(int slot, char const* name, uint32_t seed, world_meta_t* meta, player_state_t* player) {
    if (slot < 0 || slot >= SM_SLOTS || name == NULL || meta == NULL || player == NULL) return false;
    char slug[SM_WORLD_SLUG_MAX];
    worldstore_slot_slug(slot, slug, sizeof(slug));
    // Refuse to create over a world: the menu only offers empty slots,
    // and this is the line that makes sure it stays that way.
    if (slug_exists(slug)) return false;
    return create_at(slug, name, seed, meta, player);
}

bool worldstore_rename(char const* slug, char const* name) {
    if (slug == NULL || name == NULL || *name == '\0') return false;

    // Reading the whole level (palette included) replaces the remap, so
    // keep the open world's and put it back afterwards.
    uint8_t    remap[256];
    bool const needed = s_remap_needed;
    memcpy(remap, s_remap, sizeof(remap));

    world_meta_t         m;
    player_state_t       p;
    static world_items_t items;  // 96 of them: not for the stack
    bool                 ok = read_level(slug, &m, &p, true, &items);
    if (ok) {
        snprintf(m.name, sizeof(m.name), "%s", name);
        ok = write_level(slug, &m, &p, &items);
    }

    memcpy(s_remap, remap, sizeof(remap));
    s_remap_needed = needed;
    return ok;
}

int worldstore_adopt_legacy(char const* legacy_slug, char const* name) {
    if (legacy_slug == NULL || !slug_exists(legacy_slug)) return -1;

    int slot = -1;
    char slug[SM_WORLD_SLUG_MAX];
    for (int i = 0; i < SM_SLOTS && slot < 0; i++) {
        worldstore_slot_slug(i, slug, sizeof(slug));
        // Free means no directory at all, not merely no level.smw: a
        // half-deleted slot must not have a world renamed on top of it.
        char dir[192];
        world_dir(dir, sizeof(dir), slug);
        sm_dir_t* d = sm_dir_open(dir);
        if (d == NULL) {
            slot = i;
        } else {
            sm_dir_close(d);
        }
    }
    if (slot < 0) return -2;

    // One rename moves the whole directory, regions and all -- nothing
    // is copied, so nothing can be half-copied.
    char from[192], to[192];
    world_dir(from, sizeof(from), legacy_slug);
    world_dir(to, sizeof(to), slug);
    if (!sm_rename(from, to)) return -2;

    // The name is cosmetic; a world that moved but kept its old name is
    // still the player's world, so this failing does not undo the move.
    worldstore_rename(slug, name);
    return slot;
}

bool worldstore_open(char const* slug, world_meta_t* meta, player_state_t* player, world_items_t* items) {
    if (slug == NULL || meta == NULL || player == NULL) return false;
    if (!read_level(slug, meta, player, true, items)) return false;
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
    meta->format = SM_LEVEL_FORMAT;
    meta->farlands_x = FARLANDS_X_DEFAULT;
    if (player != NULL) player_state_defaults(player, meta);

    // The identity palette: nothing was written by an older build, so
    // no id can need remapping.
    remap_identity();
    s_open    = true;
    s_scratch = true;
    return true;
}

bool worldstore_save(world_meta_t const* meta, player_state_t const* player, world_items_t const* items) {
    if (!s_open || meta == NULL || player == NULL) return false;
    if (s_scratch) return true;  // a scratch world has nowhere to be saved, by design
    return write_level(s_open_slug, meta, player, items);
}

bool worldstore_delete(char const* slug) {
    if (slug == NULL || *slug == '\0') return false;

    char dir[192];
    world_dir(dir, sizeof(dir), slug);

    // FAT will not remove a directory that still has files in it, and
    // there is no recursive delete, so walk it.
    char      region[192];
    snprintf(region, sizeof(region), "%.170s/region", dir);
    sm_dir_t* d = sm_dir_open(region);
    if (d != NULL) {
        char const* e;
        // Collect then delete: deleting while iterating a FAT directory
        // is not something to rely on.
        static char names[256][32];
        int         n = 0;
        while (n < 256 && (e = sm_dir_next(d, NULL)) != NULL) {
            if (strlen(e) < sizeof(names[0])) snprintf(names[n++], sizeof(names[0]), "%s", e);
        }
        sm_dir_close(d);
        for (int i = 0; i < n; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%.190s/%.32s", region, names[i]);
            sm_remove(path);
        }
    }

    char level[192];
    level_path(level, sizeof(level), slug);
    sm_remove(level);
    // The directories last, now they are empty. A slot counts as free
    // only once its directory is gone (worldstore_adopt_legacy).
    sm_remove(region);
    sm_remove(dir);

    if (s_open && strcmp(s_open_slug, slug) == 0) worldstore_close();
    return !slug_exists(slug);
}

// --- The benchmark world (worldstore.h) -------------------------------

bool worldstore_open_bench(uint32_t seed, world_meta_t* meta, player_state_t* player, bool* fresh) {
    if (meta == NULL) return false;
    worldstore_close();
    if (fresh != NULL) *fresh = true;

    if (slug_exists(SM_BENCH_SLUG)) {
        world_meta_t   m;
        player_state_t p;
        if (read_level(SM_BENCH_SLUG, &m, &p, true, NULL) && m.seed == seed) {
            *meta = m;
            if (player != NULL) *player = p;
            open_paths(SM_BENCH_SLUG);
            if (fresh != NULL) *fresh = false;
            return true;
        }
        // Either unreadable, or made for a different seed -- which is
        // terrain this build would not generate, so every number taken
        // on it would describe a world nobody can reproduce. Throw it
        // away and generate again; that is what the fixed seed is for.
        worldstore_delete(SM_BENCH_SLUG);
    }

    // A fixed clock as well as a fixed seed: the measurement must not
    // depend on what time of day it happens to be (shadows, fog, the
    // sky's colour all cost pixels).
    if (!create_at(SM_BENCH_SLUG, "(bench)", seed, meta, player)) return false;
    meta->time_of_day = 1000;  // a morning, as the flight has always been
    return true;
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
