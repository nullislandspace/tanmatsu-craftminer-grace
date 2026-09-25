// =====================================================================
//  SynthMiner  --  packing a chunk into bytes (see chunk_codec.h)
// ---------------------------------------------------------------------
//  Payload layout, little-endian throughout:
//
//    u8  version        CHUNK_PAYLOAD_VERSION
//    u8  flags          reserved, 0
//    u8  codec_id       0 raw, 1 RLE   } one per plane, so a chunk with
//    u8  codec_st       0 raw, 1 RLE   } a trivial state plane still
//    u16 len_id                        } compresses the block plane
//    <len_id bytes>
//    u16 len_st
//    <len_st bytes>
//
//  The run encoding is pairs of (value, count-1), so one pair covers 1
//  to 256 identical bytes. Nothing is aligned and nothing is padded: the
//  payload is written to an SD card, not mapped.
// =====================================================================

#include "world/chunk_codec.h"

#include <string.h>

// Bumped only if the PLANE encoding itself changes. Adding a section
// does not need it -- that is what sections are for.
#define CHUNK_PAYLOAD_VERSION 1
#define CODEC_RAW 0
#define CODEC_RLE 1

size_t chunk_rle_encode(uint8_t const* plane, size_t n, uint8_t* out, size_t out_cap) {
    size_t w = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t const v   = plane[i];
        size_t        run = 1;
        // 256 is the longest a (value, count-1) pair can express.
        while (i + run < n && plane[i + run] == v && run < 256) run++;
        if (w + 2 > out_cap) return 0;
        out[w++] = v;
        out[w++] = (uint8_t)(run - 1);
        i += run;
    }
    // Not smaller than raw: the caller should store it raw instead.
    return w < n ? w : 0;
}

bool chunk_rle_decode(uint8_t const* in, size_t in_n, uint8_t* plane, size_t n) {
    if ((in_n & 1u) != 0) return false;  // pairs, always
    size_t w = 0;
    for (size_t i = 0; i < in_n; i += 2) {
        size_t const run = (size_t)in[i + 1] + 1;
        if (w + run > n) return false;   // would overrun: corrupt
        memset(&plane[w], in[i], run);
        w += run;
    }
    return w == n;  // and it must fill the plane exactly
}

// Write one plane, RLE if that is smaller and raw if it is not.
static size_t put_plane(uint8_t const* plane, uint8_t* out, size_t out_cap, uint8_t* codec) {
    // Leave room for the u16 length the caller writes before the data.
    size_t const packed = chunk_rle_encode(plane, CH_CELLS, out, out_cap);
    if (packed > 0) {
        *codec = CODEC_RLE;
        return packed;
    }
    if (out_cap < CH_CELLS) return 0;
    memcpy(out, plane, CH_CELLS);
    *codec = CODEC_RAW;
    return CH_CELLS;
}

size_t chunk_encode(chunk_t const* c, uint8_t const* sections, size_t sections_n, uint8_t* out,
                    size_t out_cap) {
    if (c == NULL || c->id == NULL || c->st == NULL || out_cap < 8) return 0;

    size_t w = 0;
    out[w++] = CHUNK_PAYLOAD_VERSION;
    out[w++] = 0;
    size_t const codec_at = w;
    w += 2;  // codec_id, codec_st -- filled in below

    for (int plane = 0; plane < 2; plane++) {
        uint8_t const* src = plane == 0 ? c->id : c->st;
        if (w + 2 > out_cap) return 0;
        size_t const len_at = w;
        w += 2;
        uint8_t      codec = CODEC_RAW;
        size_t const n     = put_plane(src, &out[w], out_cap - w, &codec);
        if (n == 0) return 0;
        out[len_at]              = (uint8_t)(n & 0xFFu);
        out[len_at + 1]          = (uint8_t)(n >> 8);
        out[codec_at + plane]    = codec;
        w += n;
    }

    // Sections, verbatim. They are already framed (id + length each),
    // so this layer neither knows nor cares what is in them.
    if (sections != NULL && sections_n > 0) {
        if (w + sections_n > out_cap) return 0;
        memcpy(&out[w], sections, sections_n);
        w += sections_n;
    }
    return w;
}

bool chunk_decode_ex(uint8_t const* in, size_t in_n, chunk_t* c, uint8_t const* remap,
                     chunk_section_fn fn, void* user) {
    if (c == NULL || c->id == NULL || c->st == NULL) return false;

    // Clear first: a failure part way through must leave "no chunk", not
    // half the old one and half the new.
    memset(c->id, BLK_AIR, CH_CELLS);
    memset(c->st, 0, CH_CELLS);

    if (in == NULL || in_n < 8) return false;
    if (in[0] != CHUNK_PAYLOAD_VERSION) return false;

    uint8_t const codec[2] = {in[2], in[3]};
    size_t        r        = 4;

    for (int plane = 0; plane < 2; plane++) {
        if (r + 2 > in_n) goto bad;
        size_t const len = (size_t)in[r] | ((size_t)in[r + 1] << 8);
        r += 2;
        if (r + len > in_n) goto bad;

        uint8_t* dst = plane == 0 ? c->id : c->st;
        if (codec[plane] == CODEC_RLE) {
            if (!chunk_rle_decode(&in[r], len, dst, CH_CELLS)) goto bad;
        } else if (codec[plane] == CODEC_RAW) {
            if (len != CH_CELLS) goto bad;
            memcpy(dst, &in[r], CH_CELLS);
        } else {
            goto bad;
        }
        r += len;
    }

    // Whatever follows the planes is sections. An id this build does
    // not know is stepped over, which is what lets a newer save load
    // here at all.
    while (r + 5 <= in_n) {
        uint8_t const id  = in[r];
        size_t const  len = (size_t)in[r + 1] | ((size_t)in[r + 2] << 8) | ((size_t)in[r + 3] << 16) |
                           ((size_t)in[r + 4] << 24);
        r += 5;
        if (r + len > in_n) goto bad;  // a section that runs off the end is corruption
        if (fn != NULL) fn(id, &in[r], len, user);
        r += len;
    }

    // Translate saved ids into this build's ids. Done here, once, so
    // nothing downstream ever sees a stale id.
    if (remap != NULL) {
        for (size_t i = 0; i < CH_CELLS; i++) c->id[i] = remap[c->id[i]];
    }

    chunk_resummarise(c);
    return true;

bad:
    memset(c->id, BLK_AIR, CH_CELLS);
    memset(c->st, 0, CH_CELLS);
    return false;
}

bool chunk_decode(uint8_t const* in, size_t in_n, chunk_t* c, uint8_t const* remap) {
    return chunk_decode_ex(in, in_n, c, remap, NULL, NULL);
}
