#pragma once
// =====================================================================
//  SynthMiner  --  the synthesiser the MIDI files play through
// ---------------------------------------------------------------------
//  se_voice.h says a voice is one note and that "a future MIDI player
//  will keep a pool of voices and route note-on / note-off events to
//  them (with voice stealing via `active`)". This is that player. It is
//  a midi_sink_t (midi_seq.h), so the sequencer neither knows nor cares
//  that the other end is a synth rather than a note counter.
//
//  It is NOT a General MIDI sound module and does not pretend to be.
//  The 128 GM programs are collapsed onto a handful of voice shapes --
//  a struck string, a plucked one, a sustained pad, a bass, a reed, a
//  bell -- chosen so that the piano and chamber repertoire we ship
//  (audio/MUSIC.md) comes out recognisable. A file that leans on a
//  specific patch will sound like something else, which is the price of
//  a synth that costs kilobytes instead of a megabyte of samples.
//
//  Percussion (channel 9 by the GM convention) is a short filtered noise
//  burst: enough for a piece that has a timpani roll in it, not a drum
//  machine.
//
//  Every callback runs on the mixer task: no allocation, no logging, no
//  blocking. All state is in the struct the caller owns.
// =====================================================================

#include <stddef.h>
#include <stdint.h>

#include "audio/midi_seq.h"
#include "se_voice.h"

// How many notes may sound at once. A piano piece with the sustain pedal
// down runs to a dozen; past this the quietest voice is stolen. Each one
// is a se_voice_synth_t, so this is the module's whole memory cost.
#define MIDI_SYNTH_VOICES 16

#define MIDI_CHANNELS 16

typedef struct {
    se_voice_synth_t synth;
    uint8_t          ch;       // the channel that owns it
    uint8_t          note;     // the MIDI note it is playing
    uint8_t          program;  // the voice shape it was started with
    bool             held;     // note-on seen, note-off not yet
    uint32_t         started;  // render blocks since it began, for stealing
} midi_voice_t;

typedef struct {
    midi_voice_t voice[MIDI_SYNTH_VOICES];
    uint8_t      program[MIDI_CHANNELS];   // the last program change per channel
    uint8_t      volume[MIDI_CHANNELS];    // CC 7, 0..127
    uint8_t      expression[MIDI_CHANNELS];// CC 11, 0..127
    bool         sustain[MIDI_CHANNELS];   // CC 64: hold notes past their note-off
    int16_t      bend[MIDI_CHANNELS];      // -8192..8191
    uint32_t     clock;                    // render blocks, for stealing
} midi_synth_t;

// Silence everything and reset the channels to their defaults.
void midi_synth_init(midi_synth_t* s);

// WRITE the synth's output to `mix[0..frames)`, mono, at full scale --
// the buffer is overwritten, not added to. Includes the master gain and
// the soft limiter (see MIDI_MASTER in the .c), so every caller gets the
// same level and there is no second place for it to drift. The caller
// spreads the result across the stereo pair.
void midi_synth_render(midi_synth_t* s, float* mix, size_t frames);

// True when no voice is still sounding. After the last event of a piece
// the strings are still ringing; the caller keeps rendering until this
// says they have stopped, so a piece ends by dying away rather than by
// being cut off mid-chord.
bool midi_synth_idle(midi_synth_t const* s);

// The sequencer sink. Pass a `midi_synth_t*` as its context.
extern midi_sink_t const MIDI_SYNTH_SINK;
