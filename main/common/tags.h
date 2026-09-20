#pragma once
// =====================================================================
//  CraftMiner  --  named, typed, skippable fields, in memory
// ---------------------------------------------------------------------
//  NBT's idea, without NBT's FILE*. se_nbt.h writes through stdio, which
//  is right for a whole file and wrong for the inside of a chunk: a
//  chunk payload is built in a buffer, on the core-1 worker, with no
//  allocation. So this is the same model -- a field is a name, a type
//  and a value, and a reader steps over anything it does not recognise
//  -- over a plain byte buffer.
//
//  WHY IT EXISTS. Everything saved per block or per creature will grow:
//  a furnace gains a fuel slot, a cow gains a breeding timer, a sign
//  gains a colour. Writing a migration for each of those is the sort of
//  work that never gets done and quietly breaks old saves. Instead every
//  load is:
//
//      1. fill the struct with defaults
//      2. walk the fields present and overwrite what is recognised
//      3. skip anything else
//
//  and every save writes today's format. A world opened by a newer build
//  picks up the new defaults for fields it never had, and is written
//  back complete the next time its chunk is unloaded -- so worlds
//  upgrade themselves, gradually, with no upgrade pass and no version
//  check. A world opened by an OLDER build keeps working too: the fields
//  it does not know are stepped over.
//
//  That only covers changes that ADD to the format. A change that alters
//  the layout itself is what the major version in each file's magic is
//  for (region.h, worldstore.h) -- that one an upgrader has to handle.
//
//  Pure: no engine, no RTOS, no allocation, no stdio.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wire format, little-endian:
//     u8 type, u8 name_len, name bytes, payload
// A compound's payload is fields until TAG_END, which carries no name.
enum {
    TAG_END = 0,
    TAG_I8,
    TAG_I16,
    TAG_I32,
    TAG_I64,
    TAG_F32,
    TAG_STR,    // u16 length, then bytes, no terminator
    TAG_BLOB,   // u16 length, then bytes
    TAG_COMPOUND,
};

#define TAG_NAME_MAX 31

// --- Writing ----------------------------------------------------------

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    bool     overflow;  // sticky: check once at the end, not per field
} tag_writer_t;

void tag_write_init(tag_writer_t* w, uint8_t* buf, size_t cap);

void tag_put_i8(tag_writer_t* w, char const* name, int8_t v);
void tag_put_i16(tag_writer_t* w, char const* name, int16_t v);
void tag_put_i32(tag_writer_t* w, char const* name, int32_t v);
void tag_put_i64(tag_writer_t* w, char const* name, int64_t v);
void tag_put_f32(tag_writer_t* w, char const* name, float v);
void tag_put_str(tag_writer_t* w, char const* name, char const* v);
void tag_put_blob(tag_writer_t* w, char const* name, void const* p, size_t n);

// Compounds nest; every tag_begin needs its tag_end.
void tag_begin(tag_writer_t* w, char const* name);
void tag_end(tag_writer_t* w);

// The bytes written, or 0 if anything overflowed.
size_t tag_write_done(tag_writer_t const* w);

// --- Reading ----------------------------------------------------------

typedef struct {
    uint8_t const* buf;
    size_t         len;
    size_t         pos;
    bool           error;  // sticky: malformed input
} tag_reader_t;

void tag_read_init(tag_reader_t* r, void const* buf, size_t len);

// The next field's type, with its name in `name`. TAG_END closes the
// current compound. After this, read the value with the matching
// tag_get_*, or call tag_skip() -- exactly one of the two, always.
int tag_next(tag_reader_t* r, char* name, size_t name_cap);

int8_t   tag_get_i8(tag_reader_t* r);
int16_t  tag_get_i16(tag_reader_t* r);
int32_t  tag_get_i32(tag_reader_t* r);
int64_t  tag_get_i64(tag_reader_t* r);
float    tag_get_f32(tag_reader_t* r);
size_t   tag_get_str(tag_reader_t* r, char* out, size_t cap);
size_t   tag_get_blob(tag_reader_t* r, void* out, size_t cap);

// Step over the value of the field tag_next() just reported, whatever it
// is -- a whole compound included. This is the line that makes an old
// build able to read a newer save.
void tag_skip(tag_reader_t* r, int type);
