#pragma once
// =====================================================================
//  SynthMiner  --  a Standard MIDI File sequencer
// ---------------------------------------------------------------------
//  PORTED from ../tanmatsu-tadoom/main/midi_player.c (Doom's music
//  player, GPL-2.0 like the rest of that tree), which already reads
//  format 0 and 1 files, running status, tempo changes and the meta
//  events in between. What changed on the way in:
//
//    * it is an INSTANCE, not a file of globals, so the host test can
//      run two at once and the game can pre-roll the next piece;
//    * the events go to a SINK the caller supplies rather than straight
//      to an OPL chip, so the same parse drives our synth on the badge
//      and a counter in tools/worldcheck.c;
//    * the clock is 32.32 fixed point, not `double`. The P4's FPU is
//      single precision, so a double is a call into a software library,
//      and this clock ticks on the audio task;
//    * it runs to the next EVENT rather than stepping one MIDI tick at
//      a time. A 960-ticks-per-quarter file at 120 bpm is 1920 ticks a
//      second, nearly all of them empty.
//
//  Pure: no engine, no RTOS, no allocation, no logging. The caller owns
//  the file bytes and must keep them alive until it stops the sequence.
//  A truncated or malformed file ends the track it is in rather than
//  reading past the end -- every read is bounded.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// As many tracks as a sequencer follows at once. Format 1 files of the
// classical repertoire run to a track per voice plus a conductor track;
// 24 is far beyond anything we ship, and a file with more simply has its
// extra tracks ignored rather than being refused.
#define MIDI_MAX_TRACKS 24

// Where the events go. Any callback may be NULL. `ch` is 0..15 (channel
// 9 is percussion, by the General MIDI convention).
typedef struct {
    void (*note_on)(void* ctx, uint8_t ch, uint8_t note, uint8_t vel);
    void (*note_off)(void* ctx, uint8_t ch, uint8_t note);
    void (*program)(void* ctx, uint8_t ch, uint8_t prog);
    void (*control)(void* ctx, uint8_t ch, uint8_t cc, uint8_t val);
    void (*pitch_bend)(void* ctx, uint8_t ch, int bend);  // 0..16383, 8192 centre
    void (*all_off)(void* ctx);
} midi_sink_t;

typedef struct {
    uint8_t const* pos;
    uint8_t const* end;
    uint64_t       next_tick;  // the MIDI tick this track's next event falls on
    uint8_t        running;    // running status byte
    bool           finished;
} midi_track_t;

typedef struct {
    uint8_t const* data;
    size_t         len;
    midi_track_t   track[MIDI_MAX_TRACKS];
    int            tracks;
    uint16_t       division;      // ticks per quarter note
    uint64_t       tick;          // MIDI ticks elapsed
    uint64_t       frac;          // 32.32 remainder of a tick, in samples
    uint64_t       samples_per_tick_q32;
    uint32_t       rate;          // sample rate the clock is counted in
    bool           playing;
} midi_seq_t;

// Point a sequencer at a file. False if it is not a MIDI file or has no
// readable track -- the sequencer is then simply not playing, and
// midi_seq_advance() is a no-op. `data` must outlive the sequence.
bool midi_seq_load(midi_seq_t* s, uint8_t const* data, size_t len, uint32_t sample_rate);

// Rewind to the beginning. Called by load; call it again to repeat a
// piece without re-reading the card.
void midi_seq_rewind(midi_seq_t* s);

// Run the sequence forward by `frames` samples, delivering every event
// that falls in that span to `sink`. Returns false once the last track
// has ended -- the caller decides whether to rewind, move on, or fall
// silent. Safe to call after that; it stays false.
bool midi_seq_advance(midi_seq_t* s, midi_sink_t const* sink, void* ctx, uint32_t frames);

// True while there is more to play.
static inline bool midi_seq_playing(midi_seq_t const* s) {
    return s->playing;
}
