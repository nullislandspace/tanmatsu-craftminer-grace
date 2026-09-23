// =====================================================================
//  CraftMiner  --  region files (see region.h)
// ---------------------------------------------------------------------
//  On-disk layout, little-endian throughout:
//
//    0x000  header                      64 bytes
//    0x040  directory copy A           520 bytes: 64 x 8, u32 serial, u32 crc32
//    0x248  directory copy B           520 bytes: the same shape
//    0x480  payloads, appended, 4-byte aligned
//
//  Each directory copy carries its own serial and its own CRC over
//  (entries + serial). A reader takes the copy that validates and has
//  the higher serial; a writer always writes the OTHER one. So a power
//  cut mid-write leaves the previous copy intact and current.
// =====================================================================

#include "world/region.h"

#include "world/blockent.h"

#include <stdio.h>
#include <string.h>

#include "world/chunk_codec.h"
#include "world/vfs_compat.h"

#define HDR_SIZE    64u
#define ENT_SIZE    8u
#define DIR_ENTRIES ((uint32_t)REGION_CHUNKS * ENT_SIZE)      /* 512 */
#define DIR_SIGNED  (DIR_ENTRIES + 4u)                        /* entries + serial: what the CRC covers */
#define DIR_BLOCK   (DIR_SIGNED + 4u)                         /* + the CRC itself = 520 */
#define DIR_A_OFF   HDR_SIZE                                  /* 0x040 */
#define DIR_B_OFF   (DIR_A_OFF + DIR_BLOCK)                   /* 0x248 */
#define DATA_OFF    1152u                                     /* 0x480, past both copies */

// CRC-32 (ISO-HDLC, the zlib/PNG polynomial), nibble at a time.
//
// graceloader exports zlib's crc32 and the host has one too, but this
// carries its own so that the two are provably the same function: a
// region written on the badge has to validate on a PC and the other way
// round, and "two zlibs agree" is an assumption rather than a fact. The
// nibble table is 64 bytes, against 1 KiB for the byte-wise one, and a
// directory is 516 bytes checked twice a save -- nowhere near hot enough
// to want the bigger table.
static uint32_t cm_crc32(uint8_t const* buf, size_t len) {
    static uint32_t const T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= buf[i];
        c = (c >> 4) ^ T[c & 0x0Fu];
        c = (c >> 4) ^ T[c & 0x0Fu];
    }
    return ~c;
}

typedef struct {
    uint32_t offset;  // 0 = the chunk is not in this region
    uint16_t length;
    uint8_t  codec;   // reserved: the payload carries its own
    uint8_t  flags;
} entry_t;

typedef struct {
    entry_t  dir[REGION_CHUNKS];
    uint32_t serial;
    uint32_t waste;
    int      current;  // which copy was current: 0 = A, 1 = B
} region_t;

// --- Little-endian helpers (the file is bytes, not a struct) ----------

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(uint8_t const* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(uint8_t const* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool region_path(char* out, size_t cap, char const* dir, int32_t rx, int32_t rz) {
    int const n = snprintf(out, cap, "%s/r.%ld.%ld.cmr", dir, (long)rx, (long)rz);
    return n > 0 && (size_t)n < cap;
}

static long dir_off(int slot) {
    return (long)(slot == 0 ? DIR_A_OFF : DIR_B_OFF);
}

// --- Header -----------------------------------------------------------

static bool header_write(FILE* f, int32_t rx, int32_t rz, uint32_t waste) {
    uint8_t h[HDR_SIZE];
    memset(h, 0, sizeof(h));
    memcpy(h, REGION_MAGIC, 4);
    put_u16(h + 4, REGION_VERSION);
    put_u16(h + 6, REGION_DIM);
    put_u32(h + 8, (uint32_t)rx);
    put_u32(h + 12, (uint32_t)rz);
    put_u16(h + 16, CH_W);
    put_u16(h + 18, CH_H);
    put_u16(h + 20, CH_D);
    put_u32(h + 24, waste);
    return fseek(f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), f) == sizeof(h);
}

// True only if this really is one of our regions, built the same way.
// A dimension mismatch means the file was written by a different build
// of the game; refusing it is better than reading nonsense terrain.
static bool header_read(FILE* f, uint32_t* waste) {
    uint8_t h[HDR_SIZE];
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return false;
    if (memcmp(h, REGION_MAGIC, 4) != 0) return false;
    if (get_u16(h + 4) != REGION_VERSION) return false;
    if (get_u16(h + 6) != REGION_DIM) return false;
    if (get_u16(h + 16) != CH_W || get_u16(h + 18) != CH_H || get_u16(h + 20) != CH_D) return false;
    *waste = get_u32(h + 24);
    return true;
}

// --- Directory --------------------------------------------------------

static bool dir_read(FILE* f, int slot, entry_t* dir, uint32_t* serial) {
    uint8_t buf[DIR_BLOCK];
    if (fseek(f, dir_off(slot), SEEK_SET) != 0) return false;
    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) return false;
    if (get_u32(buf + DIR_SIGNED) != cm_crc32(buf, DIR_SIGNED)) return false;

    for (int i = 0; i < REGION_CHUNKS; i++) {
        uint8_t const* e = buf + (size_t)i * ENT_SIZE;
        dir[i].offset    = get_u32(e);
        dir[i].length    = get_u16(e + 4);
        dir[i].codec     = e[6];
        dir[i].flags     = e[7];
    }
    *serial = get_u32(buf + DIR_ENTRIES);
    return true;
}

static bool dir_write(FILE* f, int slot, entry_t const* dir, uint32_t serial) {
    uint8_t buf[DIR_BLOCK];
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < REGION_CHUNKS; i++) {
        uint8_t* e = buf + (size_t)i * ENT_SIZE;
        put_u32(e, dir[i].offset);
        put_u16(e + 4, dir[i].length);
        e[6] = dir[i].codec;
        e[7] = dir[i].flags;
    }
    put_u32(buf + DIR_ENTRIES, serial);
    put_u32(buf + DIR_SIGNED, cm_crc32(buf, DIR_SIGNED));
    if (fseek(f, dir_off(slot), SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) return false;
    return fflush(f) == 0;
}

// Load whichever directory copy is valid and newest. False if neither
// is -- the region is then beyond saving and reads as empty.
static bool region_open_state(FILE* f, region_t* r) {
    entry_t  a[REGION_CHUNKS], b[REGION_CHUNKS];
    uint32_t sa = 0, sb = 0;
    bool const oka = dir_read(f, 0, a, &sa);
    bool const okb = dir_read(f, 1, b, &sb);

    if (!oka && !okb) return false;
    // Serial numbers wrap; the newer one is the one the other is behind.
    bool const use_b = okb && (!oka || (int32_t)(sb - sa) > 0);
    memcpy(r->dir, use_b ? b : a, sizeof(r->dir));
    r->serial  = use_b ? sb : sa;
    r->current = use_b ? 1 : 0;
    return true;
}

// A brand new region: an empty directory in both copies.
static bool region_create(FILE* f, int32_t rx, int32_t rz, region_t* r) {
    memset(r, 0, sizeof(*r));
    r->serial  = 1;
    r->current = 0;
    if (!header_write(f, rx, rz, 0)) return false;
    if (!dir_write(f, 0, r->dir, 1)) return false;
    if (!dir_write(f, 1, r->dir, 0)) return false;
    // Make sure the file really extends to the data area, so the first
    // append lands where the directory says it will.
    if (fseek(f, (long)DATA_OFF - 1, SEEK_SET) != 0) return false;
    uint8_t const zero = 0;
    if (fwrite(&zero, 1, 1, f) != 1) return false;
    return fflush(f) == 0;
}

// --- Reading ----------------------------------------------------------

// A section of a chunk being read. Ids this build does not know are
// ignored, which is the promise chunk_codec.h makes and the reason the
// format has not had to change to gain block entities.
static void take_section(uint8_t id, uint8_t const* data, size_t len, void* user) {
    (void)user;
    if (id == SECTION_BLOCK_ENTITIES) blockent_decode_section(data, len);
}

int region_read_chunk(char const* dir, chunk_t* c, uint8_t const* remap) {
    if (c == NULL) return -1;
    char path[192];
    int32_t const rx = region_of(c->cx), rz = region_of(c->cz);
    if (!region_path(path, sizeof(path), dir, rx, rz)) return -1;

    FILE* f = fopen(path, "rb");
    if (f == NULL) return 0;  // no region: not an error, just no chunk

    uint32_t waste = 0;
    region_t r;
    if (!header_read(f, &waste) || !region_open_state(f, &r)) {
        fclose(f);
        return -1;
    }

    int const     idx = region_local(c->cz) * REGION_DIM + region_local(c->cx);
    entry_t const e   = r.dir[idx];
    if (e.offset == 0 || e.length == 0) {
        fclose(f);
        return 0;
    }

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    if (e.length > sizeof(buf) || fseek(f, (long)e.offset, SEEK_SET) != 0 ||
        fread(buf, 1, e.length, f) != e.length) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // A payload that will not decode is a lost chunk, not a lost world:
    // report "not there" and let it be generated again.
    return chunk_decode_ex(buf, e.length, c, remap, take_section, NULL) ? 1 : 0;
}

// --- Writing ----------------------------------------------------------

bool region_write_chunk(char const* dir, chunk_t const* c) {
    if (c == NULL || c->id == NULL) return false;
    char path[192];
    int32_t const rx = region_of(c->cx), rz = region_of(c->cz);
    if (!region_path(path, sizeof(path), dir, rx, rz)) return false;

    // The chunk's block entities -- furnaces, and chests after them --
    // built first, then handed to the encoder as the section it has
    // always had a number for and never had a byte in (chunk_codec.h).
    static uint8_t sections[CHUNK_SECTIONS_MAX];
    size_t const   sn = blockent_encode_chunk(c->cx, c->cz, sections, sizeof(sections));

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    size_t const   n = chunk_encode(c, sn > 0 ? sections : NULL, sn, buf, sizeof(buf));
    if (n == 0) return false;

    FILE* f = fopen(path, "r+b");
    region_t r;
    uint32_t waste = 0;
    if (f == NULL) {
        f = fopen(path, "w+b");
        if (f == NULL) return false;
        if (!region_create(f, rx, rz, &r)) {
            fclose(f);
            return false;
        }
    } else if (!header_read(f, &waste) || !region_open_state(f, &r)) {
        fclose(f);
        return false;
    }
    r.waste = waste;

    int const idx = region_local(c->cz) * REGION_DIM + region_local(c->cx);

    // 1. Append the payload and flush it, BEFORE any directory points at
    //    it. Bytes nothing references are harmless; a directory pointing
    //    at bytes that were never written is not.
    if (fseek(f, 0, SEEK_END) != 0) goto fail;
    long end = ftell(f);
    if (end < (long)DATA_OFF) end = (long)DATA_OFF;
    long const pad = (4 - (end & 3)) & 3;
    for (long i = 0; i < pad; i++) {
        uint8_t const zero = 0;
        if (fseek(f, end + i, SEEK_SET) != 0 || fwrite(&zero, 1, 1, f) != 1) goto fail;
    }
    long const at = end + pad;
    if (fseek(f, at, SEEK_SET) != 0) goto fail;
    if (fwrite(buf, 1, n, f) != n) goto fail;
    if (fflush(f) != 0) goto fail;

    // The bytes the old copy of this chunk occupied are now dead.
    if (r.dir[idx].offset != 0) r.waste += r.dir[idx].length;

    r.dir[idx].offset = (uint32_t)at;
    r.dir[idx].length = (uint16_t)n;
    r.dir[idx].codec  = 0;
    r.dir[idx].flags  = 0;

    // 2. Write the STALE directory copy, so the current one survives a
    //    cut here. Its higher serial is what makes it current.
    if (!dir_write(f, r.current ^ 1, r.dir, r.serial + 1)) goto fail;

    // 3. The waste counter last: losing it only delays a compaction.
    if (!header_write(f, rx, rz, r.waste)) goto fail;
    if (fflush(f) != 0) goto fail;
    fclose(f);
    return true;

fail:
    fclose(f);
    return false;
}

// --- Compaction -------------------------------------------------------

bool region_should_compact(char const* dir, int32_t rx, int32_t rz) {
    char path[192];
    if (!region_path(path, sizeof(path), dir, rx, rz)) return false;
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;
    uint32_t waste = 0;
    bool     yes   = false;
    if (header_read(f, &waste)) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long const size = ftell(f);
            yes = size > (long)DATA_OFF && waste > (uint32_t)(size / 2);
        }
    }
    fclose(f);
    return yes;
}

bool region_compact(char const* dir, int32_t rx, int32_t rz) {
    char path[192], tmp[200];
    if (!region_path(path, sizeof(path), dir, rx, rz)) return false;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;

    FILE* in = fopen(path, "rb");
    if (in == NULL) return false;
    uint32_t waste = 0;
    region_t r;
    if (!header_read(in, &waste) || !region_open_state(in, &r)) {
        fclose(in);
        return false;
    }

    FILE* out = fopen(tmp, "w+b");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    region_t nr;
    if (!region_create(out, rx, rz, &nr)) goto fail;

    static uint8_t buf[CHUNK_PAYLOAD_MAX];
    long           at = (long)DATA_OFF;
    for (int i = 0; i < REGION_CHUNKS; i++) {
        entry_t const e = r.dir[i];
        if (e.offset == 0 || e.length == 0 || e.length > sizeof(buf)) continue;
        if (fseek(in, (long)e.offset, SEEK_SET) != 0) goto fail;
        if (fread(buf, 1, e.length, in) != e.length) goto fail;
        at += (4 - (at & 3)) & 3;
        if (fseek(out, at, SEEK_SET) != 0) goto fail;
        if (fwrite(buf, 1, e.length, out) != e.length) goto fail;
        nr.dir[i].offset = (uint32_t)at;
        nr.dir[i].length = e.length;
        nr.dir[i].flags  = e.flags;
        at += e.length;
    }
    if (!dir_write(out, 0, nr.dir, 2)) goto fail;
    if (!dir_write(out, 1, nr.dir, 1)) goto fail;
    if (!header_write(out, rx, rz, 0)) goto fail;
    if (fflush(out) != 0) goto fail;

    fclose(in);
    fclose(out);
    // Not remove()/rename(): graceloader exports neither (F-06).
    if (!cm_remove(path)) return false;
    return cm_rename(tmp, path);

fail:
    fclose(in);
    fclose(out);
    cm_remove(tmp);
    return false;
}
