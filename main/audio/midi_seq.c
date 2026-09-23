// =====================================================================
//  CraftMiner  --  Standard MIDI File sequencer
//  PORTED from ../tanmatsu-tadoom/main/midi_player.c. See midi_seq.h
//  for what changed and why.
// =====================================================================

#include "audio/midi_seq.h"

#include <string.h>

// --- Reading the bytes ------------------------------------------------
//
// Every one of these is bounded by `end`. A file that stops in the
// middle of a value yields what it had and leaves `*p` at `end`, which
// the caller sees as the track ending.

static uint32_t read_u32be(uint8_t const* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t read_u16be(uint8_t const* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// A MIDI variable-length quantity: seven bits a byte, high bit means
// "another follows". Four bytes maximum, which is the format's own limit.
static uint32_t read_vlq(uint8_t const** p, uint8_t const* end) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t const b = *(*p)++;
        v = (v << 7) | (uint32_t)(b & 0x7Fu);
        if ((b & 0x80u) == 0) break;
    }
    return v;
}

// --- The clock --------------------------------------------------------
//
// One MIDI tick is `tempo / division` microseconds, and we count in
// samples: samples_per_tick = rate * tempo / (division * 1e6). Held as
// 32.32 fixed point so the accumulated error over a ten-minute piece is
// nothing, without a double on the audio task.
// (num << 32) / den without a 128-bit intermediate -- the P4 is a 32-bit
// core, so `__int128` is not on the table and `num << 32` would overflow
// for any realistic tempo. Long division instead: the whole part, then
// two sixteen-bit refinements, which is the full 32 bits of fraction.
static uint64_t q32_div(uint64_t num, uint64_t den) {
    if (den == 0) return 0;
    uint64_t out = (num / den) << 32;
    uint64_t r   = num % den;
    r <<= 16;
    out += (r / den) << 16;
    r = (r % den) << 16;
    out += r / den;
    return out;
}

static void set_tempo(midi_seq_t* s, uint32_t us_per_quarter) {
    if (s->division == 0) s->division = 96;
    // samples per MIDI tick = rate * us_per_quarter / (division * 1e6).
    s->samples_per_tick_q32 =
        q32_div((uint64_t)s->rate * (uint64_t)us_per_quarter, (uint64_t)s->division * 1000000ull);
    if (s->samples_per_tick_q32 == 0) s->samples_per_tick_q32 = 1;
}

// --- Loading ----------------------------------------------------------

static bool parse_tracks(midi_seq_t* s) {
    uint8_t const* const base = s->data;
    uint8_t const* const end  = s->data + s->len;
    uint32_t const header_len = read_u32be(base + 4);
    uint8_t const* p          = base + 8 + header_len;

    s->tracks = 0;
    while (s->tracks < MIDI_MAX_TRACKS && p + 8 <= end) {
        if (memcmp(p, "MTrk", 4) != 0) break;  // not a track chunk: stop where we are
        uint32_t const track_len = read_u32be(p + 4);
        p += 8;
        // A length that runs off the end is clamped rather than trusted.
        size_t const avail = (size_t)(end - p);
        size_t const use   = track_len > avail ? avail : track_len;

        midi_track_t* t = &s->track[s->tracks++];
        t->pos          = p;
        t->end          = p + use;
        t->running      = 0;
        t->finished     = false;
        t->next_tick    = read_vlq(&t->pos, t->end);

        p += use;
    }
    return s->tracks > 0;
}

bool midi_seq_load(midi_seq_t* s, uint8_t const* data, size_t len, uint32_t sample_rate) {
    memset(s, 0, sizeof(*s));
    if (data == NULL || len < 14 || memcmp(data, "MThd", 4) != 0) return false;

    uint16_t const division = read_u16be(data + 12);
    // SMPTE division (the high bit set) counts frames, not quarters. No
    // file we ship uses it, and guessing would play at the wrong speed,
    // so it is refused rather than mangled.
    if (division == 0 || (division & 0x8000u) != 0) return false;

    s->data     = data;
    s->len      = len;
    s->division = division;
    s->rate     = sample_rate ? sample_rate : 22050u;

    if (!parse_tracks(s)) return false;
    set_tempo(s, 500000u);  // 120 bpm until the file says otherwise
    s->playing = true;
    return true;
}

void midi_seq_rewind(midi_seq_t* s) {
    if (s->data == NULL) return;
    s->tick = 0;
    s->frac = 0;
    parse_tracks(s);
    set_tempo(s, 500000u);
    s->playing = s->tracks > 0;
}

// --- Running ----------------------------------------------------------

// Deliver every event on `t` that falls on or before s->tick.
static void run_track(midi_seq_t* s, midi_track_t* t, midi_sink_t const* sink, void* ctx) {
    while (!t->finished && t->next_tick <= s->tick) {
        if (t->pos >= t->end) {
            t->finished = true;
            return;
        }

        uint8_t status = *t->pos;
        if (status & 0x80u) {
            t->pos++;
            // F0..FF are not channel messages and do not set running status.
            if (status < 0xF0u) t->running = status;
        } else {
            status = t->running;
            if (status == 0) {  // data with no status ever seen: not recoverable
                t->finished = true;
                return;
            }
        }

        uint8_t const type = status & 0xF0u;
        uint8_t const ch   = status & 0x0Fu;

        switch (type) {
            case 0x80:  // note off
                if (t->pos + 2 > t->end) { t->finished = true; return; }
                if (sink->note_off) sink->note_off(ctx, ch, t->pos[0]);
                t->pos += 2;
                break;

            case 0x90:  // note on -- velocity 0 is a note off, and most files use it
                if (t->pos + 2 > t->end) { t->finished = true; return; }
                if (t->pos[1] == 0) {
                    if (sink->note_off) sink->note_off(ctx, ch, t->pos[0]);
                } else {
                    if (sink->note_on) sink->note_on(ctx, ch, t->pos[0], t->pos[1]);
                }
                t->pos += 2;
                break;

            case 0xA0:  // polyphonic aftertouch: two bytes, ignored
                if (t->pos + 2 > t->end) { t->finished = true; return; }
                t->pos += 2;
                break;

            case 0xB0:  // control change
                if (t->pos + 2 > t->end) { t->finished = true; return; }
                if (sink->control) sink->control(ctx, ch, t->pos[0], t->pos[1]);
                t->pos += 2;
                break;

            case 0xC0:  // program change
                if (t->pos + 1 > t->end) { t->finished = true; return; }
                if (sink->program) sink->program(ctx, ch, t->pos[0]);
                t->pos += 1;
                break;

            case 0xD0:  // channel pressure: one byte, ignored
                if (t->pos + 1 > t->end) { t->finished = true; return; }
                t->pos += 1;
                break;

            case 0xE0:  // pitch bend, low seven bits first
                if (t->pos + 2 > t->end) { t->finished = true; return; }
                if (sink->pitch_bend) sink->pitch_bend(ctx, ch, t->pos[0] | (t->pos[1] << 7));
                t->pos += 2;
                break;

            default:  // 0xF0: meta and system exclusive
                if (status == 0xFFu) {
                    if (t->pos + 1 > t->end) { t->finished = true; return; }
                    uint8_t const meta = *t->pos++;
                    uint32_t const n   = read_vlq(&t->pos, t->end);
                    if ((size_t)(t->end - t->pos) < n) { t->finished = true; return; }
                    if (meta == 0x51u && n == 3) {
                        set_tempo(s, ((uint32_t)t->pos[0] << 16) | ((uint32_t)t->pos[1] << 8) | t->pos[2]);
                    } else if (meta == 0x2Fu) {
                        t->finished = true;
                        return;
                    }
                    t->pos += n;
                } else if (status == 0xF0u || status == 0xF7u) {
                    uint32_t const n = read_vlq(&t->pos, t->end);
                    if ((size_t)(t->end - t->pos) < n) { t->finished = true; return; }
                    t->pos += n;
                } else {
                    t->finished = true;  // a system message in a file: stop here
                    return;
                }
                break;
        }

        if (t->pos >= t->end) {
            t->finished = true;
            return;
        }
        t->next_tick = s->tick + read_vlq(&t->pos, t->end);
    }
}

// The earliest tick any live track is waiting for, or 0 if none is.
static uint64_t next_event_tick(midi_seq_t const* s, bool* any) {
    uint64_t best = 0;
    *any          = false;
    for (int i = 0; i < s->tracks; i++) {
        if (s->track[i].finished) continue;
        if (!*any || s->track[i].next_tick < best) best = s->track[i].next_tick;
        *any = true;
    }
    return best;
}

bool midi_seq_advance(midi_seq_t* s, midi_sink_t const* sink, void* ctx, uint32_t frames) {
    if (!s->playing || sink == NULL) return false;

    // Time to spend, in 32.32 samples.
    uint64_t budget = (uint64_t)frames << 32;

    while (budget > 0) {
        bool     any;
        uint64_t const target = next_event_tick(s, &any);
        if (!any) {  // every track has ended
            s->playing = false;
            if (sink->all_off) sink->all_off(ctx);
            return false;
        }

        // Ticks to the next event. Zero means it is due now.
        uint64_t const to_go = target > s->tick ? target - s->tick : 0;
        if (to_go > 0) {
            // What we have to spend: the samples still in this call plus
            // the fraction of a tick already banked. Asking how many
            // whole ticks that buys -- rather than what `to_go` ticks
            // cost -- is what keeps the multiply from overflowing when a
            // damaged file claims a gap of a billion ticks.
            uint64_t const avail = s->frac + budget;
            uint64_t const can   = avail / s->samples_per_tick_q32;
            if (to_go > can) {
                s->tick += can;
                s->frac = avail - can * s->samples_per_tick_q32;
                return true;  // ran out of samples before the next event
            }
            budget  = avail - to_go * s->samples_per_tick_q32;
            s->tick = target;
            s->frac = 0;
        }

        for (int i = 0; i < s->tracks; i++) {
            if (!s->track[i].finished) run_track(s, &s->track[i], sink, ctx);
        }
    }
    return true;
}
