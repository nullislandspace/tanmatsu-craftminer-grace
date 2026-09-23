// =====================================================================
//  CraftMiner  --  the MIDI voice pool. See midi_synth.h.
// =====================================================================

#include "audio/midi_synth.h"

#include "se_audio_source.h"

#include <math.h>
#include <string.h>

// --- The voice shapes -------------------------------------------------
//
// Six of them, and every General MIDI program is one of the six. The
// names are what they are trying to sound like, not what GM calls them.
typedef enum {
    SHAPE_STRUCK = 0,  // piano, harpsichord, tuned percussion: hit and decay
    SHAPE_PLUCK,       // guitar, harp, pizzicato: shorter, brighter
    SHAPE_SUSTAIN,     // strings, organ, choir: holds while the key is down
    SHAPE_BASS,        // the low register, kept simple so it stays audible
    SHAPE_REED,        // flute, oboe, clarinet: breathy, a little vibrato
    SHAPE_BELL,        // celesta, glockenspiel, tubular bells: pure and long
    SHAPE_DRUM,        // channel 9
    SHAPE_COUNT
} midi_shape_t;

// A struck string is a triangle (odd harmonics, mellow) through a low
// pass, with no sustain: the envelope's decay IS the note's decay, which
// is what makes a piano roll off rather than sit there.
static se_voice_spec_t const SHAPES[SHAPE_COUNT] = {
    [SHAPE_STRUCK] = {.osc = SE_OSC_TRIANGLE, .osc_count = 2, .detune = 0.0015f,
                      .env = {.attack = 0.004f, .decay = 2.20f, .sustain = 0.00f, .release = 0.25f},
                      .filter = SE_FILTER_LPF, .cutoff_hz = 2600.0f, .q = 0.70f, .gain = 0.55f},

    [SHAPE_PLUCK] = {.osc = SE_OSC_SAW, .osc_count = 1,
                     .env = {.attack = 0.002f, .decay = 1.10f, .sustain = 0.00f, .release = 0.15f},
                     .filter = SE_FILTER_LPF, .cutoff_hz = 2200.0f, .q = 0.80f, .gain = 0.40f},

    [SHAPE_SUSTAIN] = {.osc = SE_OSC_SAW, .osc_count = 3, .detune = 0.004f,
                       .env = {.attack = 0.090f, .decay = 0.35f, .sustain = 0.65f, .release = 0.40f},
                       .filter = SE_FILTER_LPF, .cutoff_hz = 1900.0f, .q = 0.70f, .gain = 0.30f},

    [SHAPE_BASS] = {.osc = SE_OSC_SQUARE, .osc_count = 1,
                    .env = {.attack = 0.006f, .decay = 1.40f, .sustain = 0.20f, .release = 0.20f},
                    .filter = SE_FILTER_LPF, .cutoff_hz = 900.0f, .q = 0.80f, .gain = 0.45f},

    [SHAPE_REED] = {.osc = SE_OSC_SINE, .osc_count = 1,
                    .env = {.attack = 0.055f, .decay = 0.25f, .sustain = 0.75f, .release = 0.25f},
                    .filter = SE_FILTER_LPF, .cutoff_hz = 3000.0f, .q = 0.70f, .gain = 0.38f,
                    .amp_lfo_hz = 5.2f, .amp_lfo_depth = 0.14f},

    [SHAPE_BELL] = {.osc = SE_OSC_SINE, .osc_count = 2, .detune = 0.006f,
                    .env = {.attack = 0.002f, .decay = 2.80f, .sustain = 0.00f, .release = 0.40f},
                    .filter = SE_FILTER_NONE, .gain = 0.34f},

    [SHAPE_DRUM] = {.osc = SE_OSC_NOISE, .osc_count = 1,
                    .env = {.attack = 0.001f, .decay = 0.16f, .sustain = 0.00f, .release = 0.05f},
                    .filter = SE_FILTER_BPF, .cutoff_hz = 1200.0f, .q = 1.10f, .gain = 0.40f,
                    .pitch_env_amt = 0.0f},
};

// General MIDI's 128 programs come in sixteen families of eight, and the
// family is enough to choose a shape. One row per family, in GM order:
// piano, chromatic percussion, organ, guitar, bass, strings, ensemble,
// brass, reed, pipe, synth lead, synth pad, synth effects, ethnic,
// percussive, sound effects.
static uint8_t const FAMILY_SHAPE[16] = {
    SHAPE_STRUCK,   // 0   piano
    SHAPE_BELL,     // 8   chromatic percussion
    SHAPE_SUSTAIN,  // 16  organ
    SHAPE_PLUCK,    // 24  guitar
    SHAPE_BASS,     // 32  bass
    SHAPE_SUSTAIN,  // 40  strings
    SHAPE_SUSTAIN,  // 48  ensemble
    SHAPE_SUSTAIN,  // 56  brass
    SHAPE_REED,     // 64  reed
    SHAPE_REED,     // 72  pipe
    SHAPE_PLUCK,    // 80  synth lead
    SHAPE_SUSTAIN,  // 88  synth pad
    SHAPE_BELL,     // 96  synth effects
    SHAPE_PLUCK,    // 104 ethnic
    SHAPE_STRUCK,   // 112 percussive
    SHAPE_DRUM,     // 120 sound effects
};

// --- Pitch ------------------------------------------------------------

// MIDI note to Hz: 440 * 2^((n - 69) / 12). A 128-entry table beats a
// powf per note-on, and it is 512 bytes of flash.
static float note_hz(int note, int bend_cents) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    // 2^(x/12) for x in 0..11, so the octave is a shift.
    static float const SEMI[12] = {1.000000f, 1.059463f, 1.122462f, 1.189207f,
                                   1.259921f, 1.334840f, 1.414214f, 1.498307f,
                                   1.587401f, 1.681793f, 1.781797f, 1.887749f};
    int const   oct = note / 12;
    float       hz  = 8.175799f * SEMI[note % 12];  // note 0 is C-1
    for (int i = 0; i < oct; i++) hz *= 2.0f;
    if (bend_cents != 0) {
        // Two terms of exp() are plenty over a +/- 2 semitone range.
        float const x = (float)bend_cents * 0.0005776f;  // ln(2)/1200
        hz *= 1.0f + x + 0.5f * x * x;
    }
    return hz;
}

// --- The pool ---------------------------------------------------------

void midi_synth_init(midi_synth_t* s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
        se_voice_synth_init(&s->voice[i].synth, &SHAPES[SHAPE_STRUCK]);
        s->voice[i].program = 0xFFu;  // nothing yet: the first note reconfigures
    }
    for (int c = 0; c < MIDI_CHANNELS; c++) {
        s->program[c]    = 0;
        s->volume[c]     = 100;
        s->expression[c] = 127;
        s->sustain[c]    = false;
        s->bend[c]       = 0;
    }
}

static bool voice_sounding(midi_voice_t const* v) {
    se_voice_t const* base = &v->synth.base;
    return base->active == NULL || base->active(base);
}

// A free voice, or the one worth stealing: the oldest that is no longer
// held, and failing that simply the oldest.
static midi_voice_t* pick_voice(midi_synth_t* s) {
    midi_voice_t* best_free = NULL;
    midi_voice_t* best_rel  = NULL;
    midi_voice_t* best_any  = NULL;
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
        midi_voice_t* v = &s->voice[i];
        if (!v->held && !voice_sounding(v)) {
            if (best_free == NULL || v->started < best_free->started) best_free = v;
        } else if (!v->held) {
            if (best_rel == NULL || v->started < best_rel->started) best_rel = v;
        }
        if (best_any == NULL || v->started < best_any->started) best_any = v;
    }
    if (best_free) return best_free;
    if (best_rel) return best_rel;
    return best_any;
}

static void sink_note_on(void* ctx, uint8_t ch, uint8_t note, uint8_t vel) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    if (ch >= MIDI_CHANNELS) return;

    // Channel 9 is percussion whatever program it was sent: the note
    // number picks the drum, not the pitch.
    uint8_t const shape = (ch == 9) ? SHAPE_DRUM : FAMILY_SHAPE[(s->program[ch] >> 3) & 0x0Fu];

    midi_voice_t* v = pick_voice(s);
    if (v == NULL) return;

    if (v->program != shape) {
        se_voice_synth_init(&v->synth, &SHAPES[shape]);
        v->program = shape;
    }
    v->ch      = ch;
    v->note    = note;
    v->held    = true;
    v->started = s->clock;

    // Velocity, the channel volume and the expression pedal all scale the
    // same thing. GM says they are multiplied, and so does this.
    float const level = ((float)vel / 127.0f) * ((float)s->volume[ch] / 127.0f) *
                        ((float)s->expression[ch] / 127.0f);

    float freq;
    if (ch == 9) {
        // A drum's "pitch" is which noise burst it is: low notes are
        // toms and bass drums, high ones are cymbals and sticks.
        freq = note_hz(note, 0);
    } else {
        freq = note_hz(note, (s->bend[ch] * 200) / 8192);  // +/- 2 semitones, in cents
    }
    v->synth.base.note_on(&v->synth.base, freq, level);
}

static void release_voice(midi_synth_t* s, midi_voice_t* v) {
    v->held = false;
    if (v->synth.base.note_off) v->synth.base.note_off(&v->synth.base);
    (void)s;
}

static void sink_note_off(void* ctx, uint8_t ch, uint8_t note) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    if (ch >= MIDI_CHANNELS) return;
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
        midi_voice_t* v = &s->voice[i];
        if (!v->held || v->ch != ch || v->note != note) continue;
        // With the sustain pedal down the key is up but the note is not:
        // mark it un-held so the pedal's release can find it, but leave
        // the envelope alone.
        if (s->sustain[ch]) {
            v->held = false;
        } else {
            release_voice(s, v);
        }
    }
}

static void sink_program(void* ctx, uint8_t ch, uint8_t prog) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    if (ch < MIDI_CHANNELS) s->program[ch] = prog & 0x7Fu;
}

static void sink_control(void* ctx, uint8_t ch, uint8_t cc, uint8_t val) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    if (ch >= MIDI_CHANNELS) return;
    switch (cc) {
        case 7: s->volume[ch] = val & 0x7Fu; break;
        case 11: s->expression[ch] = val & 0x7Fu; break;
        case 64: {  // sustain pedal
            bool const down = val >= 64;
            if (s->sustain[ch] && !down) {
                // Pedal up: everything it was holding decays now.
                for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
                    midi_voice_t* v = &s->voice[i];
                    if (v->ch == ch && !v->held && voice_sounding(v)) release_voice(s, v);
                }
            }
            s->sustain[ch] = down;
            break;
        }
        case 120:  // all sound off
        case 123:  // all notes off
            for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
                if (s->voice[i].ch == ch) release_voice(s, &s->voice[i]);
            }
            break;
        default: break;  // the rest of GM's controllers are not modelled
    }
}

static void sink_bend(void* ctx, uint8_t ch, int bend) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    if (ch >= MIDI_CHANNELS) return;
    // The bend applies to notes started AFTER it. Re-pitching a sounding
    // voice would need the built-in synth to expose its frequency, which
    // it does not; a held note bending is a thing the classical
    // repertoire hardly ever asks for.
    s->bend[ch] = (int16_t)(bend - 8192);
}

static void sink_all_off(void* ctx) {
    midi_synth_t* s = (midi_synth_t*)ctx;
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) release_voice(s, &s->voice[i]);
    for (int c = 0; c < MIDI_CHANNELS; c++) s->sustain[c] = false;
}

midi_sink_t const MIDI_SYNTH_SINK = {
    .note_on    = sink_note_on,
    .note_off   = sink_note_off,
    .program    = sink_program,
    .control    = sink_control,
    .pitch_bend = sink_bend,
    .all_off    = sink_all_off,
};

// The master level, and why there is a limiter at all.
//
// Sixteen voices summing is not bounded by anything: a dense Schumann
// chord peaked at 1.64 where 1.0 is full scale, and the honest int16
// conversion simply clipped it -- a thousand clipped samples in
// Traeumerei, heard as a crackle on every loud chord. Turning the gain
// down far enough to fix that by itself would have made the Satie, which
// never went above 0.89, too quiet to hear under the effects.
//
// So: scale, then round the tops off. The cubic 1.5x - 0.5x^3 is the
// cheap soft clipper -- three multiplies, no table, no branch on the
// common path -- and it has a useful second property: its slope at
// small signals is 1.5, so it lifts quiet music while it compresses
// loud music. The pieces end up nearer each other in level than they
// went in, which is what we want from a soundtrack nobody is mixing by
// hand. MASTER is set so the loudest bar of the loudest piece lands at
// 0.995 and nothing in the repertoire hard-clips at all.
#define MIDI_MASTER 0.58f

bool midi_synth_idle(midi_synth_t const* s) {
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
        if (s->voice[i].held || voice_sounding(&s->voice[i])) return false;
    }
    return true;
}

void midi_synth_render(midi_synth_t* s, float* mix, size_t frames) {
    memset(mix, 0, frames * sizeof(mix[0]));
    for (int i = 0; i < MIDI_SYNTH_VOICES; i++) {
        midi_voice_t* v = &s->voice[i];
        if (!v->held && !voice_sounding(v)) continue;
        v->synth.base.render(&v->synth.base, mix, frames);
    }
    for (size_t i = 0; i < frames; i++) {
        float x = mix[i] * MIDI_MASTER;
        if (x >= 1.0f) x = 1.0f;
        else if (x <= -1.0f) x = -1.0f;
        else x = 1.5f * x - 0.5f * x * x * x;
        mix[i] = x;
    }
    s->clock++;
}
