// =====================================================================
//  CraftMiner  --  named, typed, skippable fields (see tags.h)
// =====================================================================

#include "common/tags.h"

#include <string.h>

// --- Writing ----------------------------------------------------------

void tag_write_init(tag_writer_t* w, uint8_t* buf, size_t cap) {
    w->buf      = buf;
    w->cap      = cap;
    w->len      = 0;
    w->overflow = false;
}

static bool room(tag_writer_t* w, size_t n) {
    if (w->overflow) return false;
    if (w->len + n > w->cap) {
        w->overflow = true;  // sticky: every later write is a no-op
        return false;
    }
    return true;
}

static void raw(tag_writer_t* w, void const* p, size_t n) {
    if (!room(w, n)) return;
    memcpy(&w->buf[w->len], p, n);
    w->len += n;
}

static void u8v(tag_writer_t* w, uint8_t v) {
    raw(w, &v, 1);
}

static void u16v(tag_writer_t* w, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    raw(w, b, 2);
}

static void head(tag_writer_t* w, uint8_t type, char const* name) {
    size_t n = name != NULL ? strlen(name) : 0;
    if (n > TAG_NAME_MAX) n = TAG_NAME_MAX;
    u8v(w, type);
    u8v(w, (uint8_t)n);
    raw(w, name, n);
}

void tag_put_i8(tag_writer_t* w, char const* name, int8_t v) {
    head(w, TAG_I8, name);
    u8v(w, (uint8_t)v);
}

void tag_put_i16(tag_writer_t* w, char const* name, int16_t v) {
    head(w, TAG_I16, name);
    u16v(w, (uint16_t)v);
}

void tag_put_i32(tag_writer_t* w, char const* name, int32_t v) {
    head(w, TAG_I32, name);
    uint32_t const u = (uint32_t)v;
    uint8_t const  b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    raw(w, b, 4);
}

void tag_put_i64(tag_writer_t* w, char const* name, int64_t v) {
    head(w, TAG_I64, name);
    uint64_t const u = (uint64_t)v;
    uint8_t        b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i));
    raw(w, b, 8);
}

void tag_put_f32(tag_writer_t* w, char const* name, float v) {
    // Through a union, not a cast: the bit pattern is what goes on the
    // card, and both ends of this are the same little-endian IEEE-754.
    union {
        float    f;
        uint32_t u;
    } c;
    c.f = v;
    head(w, TAG_F32, name);
    uint8_t const b[4] = {(uint8_t)c.u, (uint8_t)(c.u >> 8), (uint8_t)(c.u >> 16), (uint8_t)(c.u >> 24)};
    raw(w, b, 4);
}

// TAG_STR and TAG_BLOB share a body (u16 length, then bytes) and differ
// only in the type byte, so that a reader knows whether it is text.
static void put_sized(tag_writer_t* w, uint8_t type, char const* name, void const* p, size_t n) {
    if (n > 0xFFFFu) {
        w->overflow = true;
        return;
    }
    head(w, type, name);
    u16v(w, (uint16_t)n);
    raw(w, p, n);
}

void tag_put_str(tag_writer_t* w, char const* name, char const* v) {
    put_sized(w, TAG_STR, name, v, v != NULL ? strlen(v) : 0);
}

void tag_put_blob(tag_writer_t* w, char const* name, void const* p, size_t n) {
    put_sized(w, TAG_BLOB, name, p, n);
}

void tag_begin(tag_writer_t* w, char const* name) {
    head(w, TAG_COMPOUND, name);
}

void tag_end(tag_writer_t* w) {
    u8v(w, TAG_END);
    u8v(w, 0);  // no name
}

size_t tag_write_done(tag_writer_t const* w) {
    return w->overflow ? 0 : w->len;
}

// --- Reading ----------------------------------------------------------

void tag_read_init(tag_reader_t* r, void const* buf, size_t len) {
    r->buf   = (uint8_t const*)buf;
    r->len   = len;
    r->pos   = 0;
    r->error = false;
}

static bool have(tag_reader_t* r, size_t n) {
    if (r->error) return false;
    if (r->pos + n > r->len) {
        r->error = true;
        return false;
    }
    return true;
}

static uint8_t rd8(tag_reader_t* r) {
    if (!have(r, 1)) return 0;
    return r->buf[r->pos++];
}

static uint16_t rd16(tag_reader_t* r) {
    if (!have(r, 2)) return 0;
    uint16_t const v = (uint16_t)((uint16_t)r->buf[r->pos] | ((uint16_t)r->buf[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

int tag_next(tag_reader_t* r, char* name, size_t name_cap) {
    if (name != NULL && name_cap > 0) name[0] = '\0';
    if (r->error || r->pos >= r->len) return TAG_END;

    int const    type = rd8(r);
    size_t const n    = rd8(r);
    if (r->error) return TAG_END;
    if (!have(r, n)) return TAG_END;

    if (name != NULL && name_cap > 0) {
        size_t const copy = n < name_cap - 1 ? n : name_cap - 1;
        memcpy(name, &r->buf[r->pos], copy);
        name[copy] = '\0';
    }
    r->pos += n;
    return type;
}

int8_t tag_get_i8(tag_reader_t* r) {
    return (int8_t)rd8(r);
}

int16_t tag_get_i16(tag_reader_t* r) {
    return (int16_t)rd16(r);
}

int32_t tag_get_i32(tag_reader_t* r) {
    if (!have(r, 4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r->buf[r->pos + i] << (8 * i);
    r->pos += 4;
    return (int32_t)v;
}

int64_t tag_get_i64(tag_reader_t* r) {
    if (!have(r, 8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->buf[r->pos + i] << (8 * i);
    r->pos += 8;
    return (int64_t)v;
}

float tag_get_f32(tag_reader_t* r) {
    union {
        float    f;
        uint32_t u;
    } c;
    c.u = (uint32_t)tag_get_i32(r);
    return c.f;
}

size_t tag_get_blob(tag_reader_t* r, void* out, size_t cap) {
    uint16_t const n = rd16(r);
    if (!have(r, n)) return 0;
    size_t const copy = n < cap ? n : cap;
    if (out != NULL && copy > 0) memcpy(out, &r->buf[r->pos], copy);
    r->pos += n;
    return copy;
}

size_t tag_get_str(tag_reader_t* r, char* out, size_t cap) {
    if (cap == 0) return 0;
    size_t const n = tag_get_blob(r, out, cap - 1);
    out[n]         = '\0';
    return n;
}

void tag_skip(tag_reader_t* r, int type) {
    switch (type) {
        case TAG_I8:   (void)rd8(r); break;
        case TAG_I16:  (void)rd16(r); break;
        case TAG_I32:  (void)tag_get_i32(r); break;
        case TAG_F32:  (void)tag_get_i32(r); break;
        case TAG_I64:  (void)tag_get_i64(r); break;
        case TAG_STR:
        case TAG_BLOB: (void)tag_get_blob(r, NULL, 0); break;
        case TAG_COMPOUND: {
            // Step over the whole thing, nesting included.
            int depth = 1;
            char throwaway[TAG_NAME_MAX + 1];
            while (depth > 0 && !r->error && r->pos < r->len) {
                int const t = tag_next(r, throwaway, sizeof(throwaway));
                if (t == TAG_END) {
                    depth--;
                } else if (t == TAG_COMPOUND) {
                    depth++;
                } else {
                    tag_skip(r, t);
                }
            }
            break;
        }
        case TAG_END:
        default:
            break;
    }
}
