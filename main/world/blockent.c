// =====================================================================
//  CraftMiner  --  blocks that remember more than a byte (see blockent.h)
// =====================================================================

#include "world/blockent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/psram.h"
#include "world/chunk_codec.h"
#include "common/tags.h"
#include "items/items.h"

static blockent_t* s_pool;

// What each kind is called on disk. A STRING, not a number, as the
// chunk format says (chunk_codec.h): there are a handful of these, the
// name costs nothing, and it can never be misread after a renumbering.
static char const* kind_name(uint8_t kind) {
    switch (kind) {
        case BE_FURNACE: return "furnace";
        case BE_CHEST: return "chest";
        case BE_TRASH: return "trash";
        default: return "";
    }
}

static uint8_t kind_by_name(char const* s) {
    for (uint8_t k = BE_FURNACE; k <= BE_TRASH; k++) {
        if (strcmp(kind_name(k), s) == 0) return k;
    }
    return BE_NONE;
}

bool blockent_init(void) {
    if (s_pool != NULL) return true;
    s_pool = (blockent_t*)cm_calloc(BE_MAX, sizeof(blockent_t));
    return s_pool != NULL;
}

void blockent_shutdown(void) {
    if (s_pool == NULL) return;
    cm_free(s_pool);
    s_pool = NULL;
}

void blockent_clear(void) {
    if (s_pool != NULL) memset(s_pool, 0, (size_t)BE_MAX * sizeof(blockent_t));
}

blockent_t* blockent_at(int32_t x, int32_t y, int32_t z) {
    if (s_pool == NULL) return NULL;
    // A linear walk over 192 records, and deliberately so: it is a
    // handful of comparisons against a table that is almost always
    // nearly empty, called once when a player presses a key -- not per
    // frame and not per block. A hash here would be a structure to keep
    // right for no measurable gain.
    for (int i = 0; i < BE_MAX; i++) {
        blockent_t* b = &s_pool[i];
        if (b->kind != BE_NONE && b->x == x && b->y == y && b->z == z) return b;
    }
    return NULL;
}

blockent_t* blockent_add(int32_t x, int32_t y, int32_t z, uint8_t kind) {
    if (s_pool == NULL || kind == BE_NONE) return NULL;

    // One per cell. Placing over an old record -- which can happen if a
    // break was missed somehow -- takes the cell rather than leaving
    // two records fighting over it.
    blockent_t* b = blockent_at(x, y, z);
    if (b == NULL) {
        for (int i = 0; i < BE_MAX && b == NULL; i++) {
            if (s_pool[i].kind == BE_NONE) b = &s_pool[i];
        }
    }
    if (b == NULL) return NULL;  // full: the caller has to say so

    memset(b, 0, sizeof(*b));
    b->x    = x;
    b->y    = y;
    b->z    = z;
    b->kind = kind;
    return b;
}

void blockent_remove(int32_t x, int32_t y, int32_t z) {
    blockent_t* b = blockent_at(x, y, z);
    if (b != NULL) memset(b, 0, sizeof(*b));
}

void blockent_touch(blockent_t const* be) {
    if (be == NULL || be->kind == BE_NONE) return;
    chunk_mark_edited(chunk_of(be->x), chunk_of(be->z));
}

int blockent_rot_trash(blockent_t* be, uint32_t now) {
    if (be == NULL || be->kind != BE_TRASH) return 0;

    // A stamp from the future means the clock moved back under it;
    // treat it as no time at all rather than as an instant emptying.
    uint32_t const elapsed = now >= be->stamp ? now - be->stamp : 0;
    if (elapsed < BE_TRASH_TICKS) return 0;

    int gone = 0;
    for (int i = 0; i < BE_SLOTS; i++) {
        if (be->slot[i].item == 0) continue;
        memset(&be->slot[i], 0, sizeof(be->slot[i]));
        gone++;
    }
    be->stamp = now;
    return gone;
}

void blockent_drop_chunk(int32_t cx, int32_t cz) {
    if (s_pool == NULL) return;
    for (int i = 0; i < BE_MAX; i++) {
        blockent_t* b = &s_pool[i];
        if (b->kind == BE_NONE) continue;
        if (chunk_of(b->x) == cx && chunk_of(b->z) == cz) memset(b, 0, sizeof(*b));
    }
}

int blockent_count(void) {
    int n = 0;
    if (s_pool == NULL) return 0;
    for (int i = 0; i < BE_MAX; i++) {
        if (s_pool[i].kind != BE_NONE) n++;
    }
    return n;
}

int blockent_count_in(int32_t cx, int32_t cz) {
    int n = 0;
    if (s_pool == NULL) return 0;
    for (int i = 0; i < BE_MAX; i++) {
        blockent_t const* b = &s_pool[i];
        if (b->kind != BE_NONE && chunk_of(b->x) == cx && chunk_of(b->z) == cz) n++;
    }
    return n;
}

// --- Saving -----------------------------------------------------------

// How many slots a kind actually uses. Only those are written, so a
// furnace costs three fields and not twenty-seven empty ones.
static int slots_used(uint8_t kind) {
    return kind == BE_FURNACE ? 3 : BE_SLOTS;
}

// One record's tagged fields. Items go in BY NAME, like everything else
// a save keeps (D-74), so a build that renumbers items still reads it.
static void write_record(tag_writer_t* w, blockent_t const* b) {
    tag_put_str(w, "kind", kind_name(b->kind));
    tag_put_i32(w, "x", b->x);
    tag_put_i32(w, "y", b->y);
    tag_put_i32(w, "z", b->z);
    tag_put_i32(w, "stamp", (int32_t)b->stamp);
    if (b->kind == BE_FURNACE) {
        tag_put_i16(w, "burn", (int16_t)b->burn_left);
        tag_put_i16(w, "burnmax", (int16_t)b->burn_max);
        tag_put_i16(w, "cook", (int16_t)b->cook);
    }
    int const n = slots_used(b->kind);
    for (int i = 0; i < n; i++) {
        inv_slot_t const* s = &b->slot[i];
        if (s->item == 0 || s->count == 0) continue;  // empty slots cost nothing
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        tag_begin(w, key);
        tag_put_str(w, "item", item_def(s->item).name);
        tag_put_i16(w, "n", (int16_t)s->count);
        tag_put_i16(w, "wear", (int16_t)s->wear);
        tag_end(w);
    }
}

size_t blockent_encode_chunk(int32_t cx, int32_t cz, uint8_t* out, size_t cap) {
    if (s_pool == NULL || out == NULL) return 0;

    int const n = blockent_count_in(cx, cz);
    if (n == 0) return 0;  // no records, no section

    // u8 id, u32 length, then u16 count and the records.
    if (cap < 7) return 0;
    size_t pos = 0;
    out[pos++] = SECTION_BLOCK_ENTITIES;
    size_t const len_at = pos;
    pos += 4;  // filled in once the contents are known
    size_t const body_at = pos;
    out[pos++]           = (uint8_t)(n & 0xFF);
    out[pos++]           = (uint8_t)((n >> 8) & 0xFF);

    int written = 0;
    for (int i = 0; i < BE_MAX; i++) {
        blockent_t const* b = &s_pool[i];
        if (b->kind == BE_NONE) continue;
        if (chunk_of(b->x) != cx || chunk_of(b->z) != cz) continue;
        if (pos + 2 > cap) return 0;

        // Each record is a u16 length then its fields, so a reader can
        // step over one whose kind it does not know.
        size_t const rec_len_at = pos;
        pos += 2;
        tag_writer_t w;
        tag_write_init(&w, out + pos, cap - pos);
        write_record(&w, b);
        size_t const rec = tag_write_done(&w);
        if (rec == 0) return 0;  // did not fit: write no section at all

        out[rec_len_at]     = (uint8_t)(rec & 0xFF);
        out[rec_len_at + 1] = (uint8_t)((rec >> 8) & 0xFF);
        pos += rec;
        written++;
    }

    size_t const body = pos - body_at;
    out[len_at]       = (uint8_t)(body & 0xFF);
    out[len_at + 1]   = (uint8_t)((body >> 8) & 0xFF);
    out[len_at + 2]   = (uint8_t)((body >> 16) & 0xFF);
    out[len_at + 3]   = (uint8_t)((body >> 24) & 0xFF);
    return written == n ? pos : 0;
}

static void read_slot(tag_reader_t* r, inv_slot_t* out) {
    char     name[TAG_NAME_MAX + 1];
    uint16_t item = 0;
    int      n = 0, wear = 0;
    for (;;) {
        int const t = tag_next(r, name, sizeof(name));
        if (t == TAG_END || t < 0 || r->error) break;
        if (t == TAG_STR && strcmp(name, "item") == 0) {
            char buf[48];
            tag_get_str(r, buf, sizeof(buf));
            // A name this build does not know becomes nothing: there is
            // nothing honest to turn it into.
            item = item_by_name(buf);
        } else if (t == TAG_I16 && strcmp(name, "n") == 0) {
            n = tag_get_i16(r);
        } else if (t == TAG_I16 && strcmp(name, "wear") == 0) {
            wear = tag_get_i16(r);
        } else {
            tag_skip(r, t);
        }
    }
    if (item == 0 || n <= 0) return;
    int const cap = item_def(item).stack_max;
    out->item     = item;
    out->count    = (uint8_t)(n > cap ? cap : n);
    out->wear     = (uint16_t)(wear < 0 ? 0 : wear);
}

static void read_record(uint8_t const* data, size_t len) {
    tag_reader_t r;
    tag_read_init(&r, data, len);

    // Defaults first, then whatever the record actually carries, then
    // anything unrecognised stepped over -- the three lines of tags.h
    // that make a save format survive being added to.
    blockent_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    bool have_pos = false;
    int  got_xyz  = 0;

    char name[TAG_NAME_MAX + 1];
    for (;;) {
        int const t = tag_next(&r, name, sizeof(name));
        if (t == TAG_END || t < 0 || r.error) break;
        if (t == TAG_STR && strcmp(name, "kind") == 0) {
            char buf[24];
            tag_get_str(&r, buf, sizeof(buf));
            tmp.kind = kind_by_name(buf);
        } else if (t == TAG_I32 && strcmp(name, "x") == 0) {
            tmp.x = tag_get_i32(&r);
            got_xyz++;
        } else if (t == TAG_I32 && strcmp(name, "y") == 0) {
            tmp.y = tag_get_i32(&r);
            got_xyz++;
        } else if (t == TAG_I32 && strcmp(name, "z") == 0) {
            tmp.z = tag_get_i32(&r);
            got_xyz++;
        } else if (t == TAG_I32 && strcmp(name, "stamp") == 0) {
            tmp.stamp = (uint32_t)tag_get_i32(&r);
        } else if (t == TAG_I16 && strcmp(name, "burn") == 0) {
            tmp.burn_left = (uint16_t)tag_get_i16(&r);
        } else if (t == TAG_I16 && strcmp(name, "burnmax") == 0) {
            tmp.burn_max = (uint16_t)tag_get_i16(&r);
        } else if (t == TAG_I16 && strcmp(name, "cook") == 0) {
            tmp.cook = (uint16_t)tag_get_i16(&r);
        } else if (t == TAG_COMPOUND && name[0] == 's' && name[1] >= '0' && name[1] <= '9') {
            int const i = atoi(name + 1);
            if (i >= 0 && i < BE_SLOTS) {
                read_slot(&r, &tmp.slot[i]);
            } else {
                inv_slot_t discard = {0};
                read_slot(&r, &discard);
            }
        } else {
            tag_skip(&r, t);
        }
    }
    have_pos = got_xyz == 3;

    // A record with no kind is one this build does not know: skipped,
    // which is exactly what the format promises.
    if (tmp.kind == BE_NONE || !have_pos || r.error) return;

    blockent_t* b = blockent_add(tmp.x, tmp.y, tmp.z, tmp.kind);
    if (b == NULL) return;  // pool full; the chunk keeps its copy on disk
    uint8_t const kind = b->kind;
    *b                 = tmp;
    b->kind            = kind;
}

void blockent_decode_section(uint8_t const* data, size_t len) {
    if (s_pool == NULL || data == NULL || len < 2) return;
    size_t pos    = 0;
    int const cnt = (int)((unsigned)data[0] | ((unsigned)data[1] << 8));
    pos           = 2;
    for (int i = 0; i < cnt; i++) {
        if (pos + 2 > len) return;
        size_t const rec = (size_t)((unsigned)data[pos] | ((unsigned)data[pos + 1] << 8));
        pos += 2;
        if (pos + rec > len) return;  // truncated: stop, keep what parsed
        read_record(data + pos, rec);
        pos += rec;
    }
}
