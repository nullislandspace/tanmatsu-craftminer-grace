// =====================================================================
//  CraftMiner  --  the sound effects, one row each
//  See sfx.h for why this is a table and not a file per noise.
// =====================================================================

#include "audio/sfx.h"

#include "se_audio.h"
#include "se_audio_dsp.h"
#include "se_audio_source.h"
#include "ui/settings.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static char const TAG[] = "cm_sfx";

// --- What a sound is --------------------------------------------------

// A one-shot is a tone layer and a noise layer, summed, put through one
// filter and one attack/decay envelope. Both layers and the filter may
// sweep from their start value to their end value across the sound, which
// is what turns a buzz into a thump and a hiss into a splash.
typedef enum { O_SINE = 0, O_SAW, O_SQUARE, O_TRI } sfx_osc_t;
typedef enum { F_NONE = 0, F_LPF, F_HPF, F_BPF } sfx_filt_t;

typedef struct {
    char const* name;
    uint8_t     osc;        // sfx_osc_t
    float       f0, f1;     // tone Hz at the start and the end (f1 <= 0: no sweep)
    float       tone_amp;   // 0 -> no tone layer
    float       noise_amp;  // 0 -> no noise layer
    uint8_t     filt;       // sfx_filt_t, over the sum of both layers
    float       fc0, fc1;   // cutoff Hz at the start and the end (fc1 <= 0: fixed)
    float       q;
    float       attack_s, decay_s;
    float       amp;     // the row's own level, before AUDIO_SFX_GAIN
    float       jitter;  // +/- this fraction on pitch and cutoff, per play
} sfx_def_t;

// The rows. Footsteps first, in block_sound_t order (SND_SOFT ... 
// SND_SPLASH), then the breaks in the same order -- sfx_play_step() and
// sfx_play_break() index by adding, so that order is load-bearing.
static sfx_def_t const SFX[SFX_COUNT] = {
    // --- footsteps: quiet, short, and different every time -------------
    [SFX_STEP_SOFT]   = {"step_soft",   O_SINE, 0, 0, 0.00f, 0.55f, F_LPF, 1500, 700,  0.80f, 0.004f, 0.070f, 0.22f, 0.22f},
    [SFX_STEP_GRAVEL] = {"step_gravel", O_SINE, 0, 0, 0.00f, 0.70f, F_BPF, 1900, 1200, 1.20f, 0.003f, 0.060f, 0.26f, 0.28f},
    [SFX_STEP_STONE]  = {"step_stone",  O_TRI,  210, 150, 0.18f, 0.60f, F_HPF, 1500, 2300, 0.70f, 0.002f, 0.050f, 0.24f, 0.20f},
    [SFX_STEP_WOOD]   = {"step_wood",   O_SQUARE, 190, 125, 0.30f, 0.40f, F_LPF, 1300, 800,  0.90f, 0.003f, 0.075f, 0.24f, 0.18f},
    [SFX_STEP_SAND]   = {"step_sand",   O_SINE, 0, 0, 0.00f, 0.60f, F_LPF, 950,  480,  0.70f, 0.005f, 0.090f, 0.20f, 0.25f},
    [SFX_STEP_GLASS]  = {"step_glass",  O_SINE, 2600, 2400, 0.14f, 0.50f, F_HPF, 3200, 4200, 0.80f, 0.002f, 0.050f, 0.20f, 0.15f},
    [SFX_STEP_SPLASH] = {"step_splash", O_SINE, 0, 0, 0.00f, 0.80f, F_BPF, 900,  2400, 1.00f, 0.006f, 0.180f, 0.26f, 0.20f},

    // --- breaking: the same materials, louder and longer ---------------
    [SFX_BREAK_SOFT]   = {"break_soft",   O_SINE, 0, 0, 0.00f, 0.80f, F_LPF, 1800, 600,  0.80f, 0.004f, 0.160f, 0.36f, 0.15f},
    [SFX_BREAK_GRAVEL] = {"break_gravel", O_SINE, 0, 0, 0.00f, 0.90f, F_BPF, 1600, 700,  0.90f, 0.003f, 0.200f, 0.40f, 0.18f},
    [SFX_BREAK_STONE]  = {"break_stone",  O_TRI,  180, 110, 0.25f, 0.80f, F_BPF, 1400, 600,  0.80f, 0.002f, 0.220f, 0.42f, 0.15f},
    [SFX_BREAK_WOOD]   = {"break_wood",   O_SQUARE, 150, 90,  0.40f, 0.50f, F_LPF, 1200, 500,  0.90f, 0.003f, 0.250f, 0.40f, 0.12f},
    [SFX_BREAK_SAND]   = {"break_sand",   O_SINE, 0, 0, 0.00f, 0.80f, F_LPF, 1000, 400,  0.70f, 0.004f, 0.200f, 0.36f, 0.18f},
    [SFX_BREAK_GLASS]  = {"break_glass",  O_SINE, 3100, 2200, 0.22f, 0.95f, F_HPF, 3000, 4500, 0.90f, 0.001f, 0.380f, 0.42f, 0.12f},
    [SFX_BREAK_SPLASH] = {"break_splash", O_SINE, 0, 0, 0.00f, 0.90f, F_BPF, 800,  2600, 1.00f, 0.006f, 0.260f, 0.36f, 0.15f},

    // --- everything else ------------------------------------------------
    [SFX_PLACE]  = {"place",  O_TRI,  270, 170, 0.45f, 0.35f, F_LPF, 1600, 700,  0.80f, 0.002f, 0.090f, 0.34f, 0.10f},
    // HIT plays on every swing while a block is being mined, so it is
    // deliberately the quietest row here: at 20 Hz it would otherwise
    // become a buzz rather than a tapping.
    [SFX_HIT]    = {"hit",    O_TRI,  320, 240, 0.20f, 0.45f, F_HPF, 2600, 3200, 0.70f, 0.001f, 0.035f, 0.16f, 0.30f},
    [SFX_PICKUP] = {"pickup", O_SINE, 880, 1400, 0.70f, 0.00f, F_NONE, 0, 0, 0.0f, 0.004f, 0.120f, 0.30f, 0.05f},
    [SFX_HURT]   = {"hurt",   O_SAW,  330, 170, 0.60f, 0.20f, F_LPF, 1700, 800,  0.90f, 0.003f, 0.280f, 0.40f, 0.08f},
    [SFX_LAND]   = {"land",   O_SINE, 95,  60,  0.45f, 0.55f, F_LPF, 700,  300,  0.80f, 0.002f, 0.140f, 0.38f, 0.12f},
    [SFX_CLICK]  = {"click",  O_SQUARE, 1250, 0, 0.35f, 0.00f, F_NONE, 0, 0, 0.0f, 0.001f, 0.030f, 0.22f, 0.00f},
    [SFX_FELL]   = {"fell",   O_SINE, 0, 0, 0.00f, 1.00f, F_LPF, 500,  160,  0.70f, 0.010f, 0.700f, 0.45f, 0.10f},
};

// --- The voice --------------------------------------------------------

// Recomputing a biquad is trigonometry, so a sweeping filter is updated
// every FILTER_BLOCK samples rather than every one. At 22050 Hz that is
// ~1.5 ms of stale cutoff, which nothing here is fast enough to notice.
#define FILTER_BLOCK 64

typedef struct {
    sfx_voice_t    voice;
    sfx_def_t      def;  // a COPY: jitter is applied to it, and the table is const
    uint32_t       phase;
    uint32_t       noise_state;
    audio_env_t    env;
    audio_biquad_t filter;
    bool           has_filter;
    uint32_t       elapsed;   // samples since the note started
    uint32_t       span;      // samples the sweeps run over
    uint32_t       next_filter_update;
} sfx_state_t;

static void set_filter(sfx_state_t* st, float fc) {
    if (fc < 40.0f) fc = 40.0f;
    if (fc > 9000.0f) fc = 9000.0f;  // below Nyquist at 22050 Hz, with room
    switch (st->def.filt) {
        case F_LPF: audio_biquad_lpf(&st->filter, fc, st->def.q); break;
        case F_HPF: audio_biquad_hpf(&st->filter, fc, st->def.q); break;
        case F_BPF: audio_biquad_bpf(&st->filter, fc, st->def.q); break;
        default: break;
    }
}

static void sfx_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    sfx_state_t* st = (sfx_state_t*)self;
    sfx_def_t const* d = &st->def;

    for (size_t i = 0; i < frames; i++) {
        // How far through the sound we are, 0..1, for every sweep.
        float const t = st->span ? (float)st->elapsed / (float)st->span : 1.0f;
        float const u = t > 1.0f ? 1.0f : t;

        float s = 0.0f;
        if (d->tone_amp > 0.0f) {
            float const f = (d->f1 > 0.0f) ? d->f0 + (d->f1 - d->f0) * u : d->f0;
            st->phase += audio_dsp_phase_inc(f);
            switch (d->osc) {
                case O_SAW:    s += audio_dsp_saw(st->phase) * d->tone_amp; break;
                case O_SQUARE: s += audio_dsp_square(st->phase) * d->tone_amp; break;
                case O_TRI:    s += audio_dsp_triangle(st->phase) * d->tone_amp; break;
                default:       s += audio_dsp_sin(st->phase) * d->tone_amp; break;
            }
        }
        if (d->noise_amp > 0.0f) {
            s += audio_dsp_noise(&st->noise_state) * d->noise_amp;
        }

        if (st->has_filter) {
            if (st->elapsed >= st->next_filter_update) {
                float const fc = (d->fc1 > 0.0f) ? d->fc0 + (d->fc1 - d->fc0) * u : d->fc0;
                set_filter(st, fc);
                st->next_filter_update = st->elapsed + FILTER_BLOCK;
            }
            s = audio_biquad_tick(&st->filter, s);
        }

        s *= audio_env_tick(&st->env) * d->amp;

        int16_t const v = audio_dsp_to_s16(s);
        out[2 * i + 0] = v;
        out[2 * i + 1] = v;
        st->elapsed++;
    }

    if (audio_env_is_idle(&st->env)) self->finished = true;
}

static void sfx_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

// --- Playing ----------------------------------------------------------

// The jitter PRNG. Deliberately NOT the world's: a footstep must never
// perturb terrain generation, and the tick must play back identically
// with the sound on or off (Part T). This is xorshift32, seeded once.
static uint32_t s_rng = 0x9E3779B9u;

static float jitter_unit(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;  // -1 .. +1
}

bool sfx_play_pitched(sfx_id_t id, float semitones) {
    if (id < 0 || id >= SFX_COUNT) return false;
    if (!settings_sfx()) return false;  // the mixer would silence it anyway; save the allocation

    sfx_state_t* st = (sfx_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "no memory for %s", SFX[id].name);
        return false;
    }

    st->def = SFX[id];
    sfx_def_t* d = &st->def;

    // A semitone is the twelfth root of two; near enough for a footstep,
    // and without a powf per play.
    float scale = 1.0f;
    if (semitones != 0.0f) {
        float const x = semitones * 0.0577623f;  // ln(2)/12
        scale = 1.0f + x + 0.5f * x * x;         // exp(x), two terms
    }
    scale *= 1.0f + d->jitter * jitter_unit();
    if (scale < 0.25f) scale = 0.25f;

    d->f0 *= scale;
    d->f1 *= scale;
    d->fc0 *= scale;
    d->fc1 *= scale;
    // The length varies a little too, or repeated steps tick like a clock.
    d->decay_s *= 1.0f + 0.15f * d->jitter * jitter_unit();

    st->noise_state = (uint32_t)(s_rng | 1u);
    st->has_filter  = d->filt != F_NONE;
    st->span        = (uint32_t)((d->attack_s + d->decay_s) * (float)AUDIO_SAMPLE_RATE_HZ);
    if (st->has_filter) {
        audio_biquad_reset(&st->filter);
        set_filter(st, d->fc0);
        st->next_filter_update = FILTER_BLOCK;
    }
    // sustain 0 -> a clean attack/decay one-shot that reaches IDLE by itself.
    audio_env_configure(&st->env, d->attack_s, d->decay_s, 0.0f, 0.001f);
    audio_env_trigger(&st->env);

    st->voice.render   = sfx_render;
    st->voice.shutdown = sfx_shutdown;
    st->voice.finished = false;
    st->voice.group    = SETTINGS_SFX_GROUP;

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;  // every slot busy: drop it rather than wait
    }
    return true;
}

bool sfx_play(sfx_id_t id) {
    return sfx_play_pitched(id, 0.0f);
}

char const* sfx_name(sfx_id_t id) {
    return (id >= 0 && id < SFX_COUNT) ? SFX[id].name : "?";
}

// --- The material sounds ----------------------------------------------

// SND_NONE and anything past the table are silent. The rest index the
// footstep and break runs directly, which is why those runs are in
// block_sound_t order.
static bool play_material(uint8_t block, sfx_id_t first, float semitones) {
    uint8_t const snd = block_sound(block);
    if (snd == SND_NONE || snd >= SND_COUNT) return false;
    return sfx_play_pitched((sfx_id_t)(first + (snd - SND_SOFT)), semitones);
}

bool sfx_play_step(uint8_t block) {
    return play_material(block, SFX_STEP_SOFT, 0.0f);
}

bool sfx_play_break(uint8_t block) {
    return play_material(block, SFX_BREAK_SOFT, 0.0f);
}

bool sfx_play_place(uint8_t block) {
    // Placing is one knock whatever the block, pitched by what it is: a
    // plank lands higher than a boulder. Cheaper than a third run of the
    // table, and it reads as the same action, which it is.
    uint8_t const snd = block_sound(block);
    static float const BY_SOUND[SND_COUNT] = {
        [SND_SOFT] = 4.0f, [SND_GRAVEL] = 1.0f, [SND_STONE] = -3.0f, [SND_WOOD] = 0.0f,
        [SND_SAND] = 2.0f, [SND_GLASS] = 7.0f,  [SND_SPLASH] = 3.0f,
    };
    return sfx_play_pitched(SFX_PLACE, snd < SND_COUNT ? BY_SOUND[snd] : 0.0f);
}
