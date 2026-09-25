// =====================================================================
//  SynthMiner  --  the music scheduler. See music.h.
// =====================================================================

#include "audio/music.h"

#include "audio/midi_seq.h"
#include "audio/midi_synth.h"
#include "se_audio.h"
#include "se_audio_dsp.h"
#include "se_audio_source.h"
#include "ui/settings.h"
#include "world/datadir.h"
#include "world/vfs_compat.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "graceloader.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "sm_music";

// --- How long the silences are ----------------------------------------
//
// Minecraft Beta's gaps were minutes, and that is what makes a piece
// starting feel like something rather than like a loop coming round. The
// first one is shorter, so a player who puts the badge down after five
// minutes has still heard the music exist.
#define FIRST_GAP_MIN_S 25.0f
#define FIRST_GAP_MAX_S 70.0f
#define GAP_MIN_S       240.0f
#define GAP_MAX_S       600.0f

#define MUSIC_MAX_TRACKS 32
#define MUSIC_NAME_MAX   64
// A Standard MIDI File of a piano piece is a few kilobytes; a whole
// symphony movement with every repeat is tens. Past this we assume the
// file is not what it claims to be and skip it.
#define MUSIC_MAX_BYTES (192u * 1024u)

// --- The hand-over ----------------------------------------------------
//
// The game thread reads the card; the mixer task plays. They share one
// buffer, and which of them owns it is this word. Each state has exactly
// one writer, which is what makes a single atomic enough:
//
//   IDLE    game owns the buffer (there is none); it may load
//   LOADED  game has filled it and let go; the mixer may take it
//   PLAYING mixer owns it
//   DONE    mixer has finished with it; the game may free it
typedef enum { MUS_IDLE = 0, MUS_LOADED, MUS_PLAYING, MUS_DONE } mus_state_t;

static _Atomic int s_state = MUS_IDLE;

static uint8_t* s_buf;  // the file, in PSRAM; written by the game thread only
static size_t   s_len;

static midi_seq_t   s_seq;
static midi_synth_t s_synth;
static bool         s_ending;  // the mixer task's own: the piece is over, the tails are not

static char s_tracks[MUSIC_MAX_TRACKS][MUSIC_NAME_MAX];
static char s_dirs[MUSIC_MAX_TRACKS][2];  // which directory each came from: "0" or "1"
static char s_dir_path[2][128];
static int  s_n_tracks;
static int  s_last = -1;  // the piece that played last, never chosen twice running
static char s_now[MUSIC_NAME_MAX];

static float    s_gap_left = 0.0f;
static bool     s_started  = false;
static uint32_t s_rng      = 0x243F6A88u;

// Deliberately not the world's generator: choosing a piece must never
// perturb terrain (Part T).
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rnd_range(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rnd() >> 8) * (1.0f / 16777216.0f));
}

// --- The source the mixer holds ---------------------------------------

// One block's worth of mono float, summed from the voices before it is
// spread across the stereo pair. 256 frames is the mixer's chunk; the
// loop below copes with a larger one anyway.
#define MUSIC_BLOCK 256
static float s_mix[MUSIC_BLOCK];

static void music_render(music_source_t* self, int16_t* out, size_t frames) {
    (void)self;

    int state = atomic_load_explicit(&s_state, memory_order_acquire);

    if (state == MUS_LOADED) {
        // Take the buffer the game thread put there. Parsing is a few
        // hundred bytes of header walking -- no I/O, no allocation.
        if (midi_seq_load(&s_seq, s_buf, s_len, AUDIO_SAMPLE_RATE_HZ)) {
            midi_synth_init(&s_synth);
            s_ending = false;
            state    = MUS_PLAYING;
        } else {
            state = MUS_DONE;  // not a MIDI file after all; the game thread frees it
        }
        atomic_store_explicit(&s_state, state, memory_order_release);
    }

    if (state != MUS_PLAYING) return;  // the buffer is pre-zeroed: silence

    for (size_t done = 0; done < frames;) {
        size_t const n = (frames - done) > MUSIC_BLOCK ? MUSIC_BLOCK : (frames - done);

        // When the last event has gone by the strings are still ringing.
        // `s_ending` keeps us rendering the voices through their release
        // so the piece dies away instead of being cut off mid-chord; only
        // once they are all silent does the buffer go back to the game
        // thread. This flag is the mixer task's alone.
        if (!midi_seq_advance(&s_seq, &MIDI_SYNTH_SINK, &s_synth, (uint32_t)n)) s_ending = true;
        midi_synth_render(&s_synth, s_mix, n);
        if (s_ending && midi_synth_idle(&s_synth)) {
            s_ending = false;
            atomic_store_explicit(&s_state, MUS_DONE, memory_order_release);
        }

        for (size_t i = 0; i < n; i++) {
            int16_t const v = audio_dsp_to_s16(s_mix[i]);
            out[2 * (done + i) + 0] = v;
            out[2 * (done + i) + 1] = v;
        }
        done += n;
    }
}

static music_source_t s_source = {.render = music_render};

// --- Finding the pieces -----------------------------------------------

// Case-insensitive compare, ours rather than the C library's.
//
// NOT for speed: graceloader does not EXPORT strcasecmp, and an app that
// calls a symbol the loader cannot resolve links perfectly well and then
// fails to LOAD -- no message, no console, nothing on screen. It is the
// same trap se_mp3.c's header warns about for opendir(). A file
// extension is ASCII, so this is four lines.
static int ieq(char const* a, char const* b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

static bool has_mid_ext(char const* name) {
    char const* dot = strrchr(name, '.');
    if (dot == NULL) return false;
    return ieq(dot, ".mid") || ieq(dot, ".midi");
}

static void scan_dir(int which, char const* path) {
    snprintf(s_dir_path[which], sizeof(s_dir_path[which]), "%s", path);
    sm_dir_t* d = sm_dir_open(path);
    if (d == NULL) return;

    int found = 0;
    char const* name;
    bool        is_dir;
    while (s_n_tracks < MUSIC_MAX_TRACKS && (name = sm_dir_next(d, &is_dir)) != NULL) {
        if (is_dir || !has_mid_ext(name)) continue;
        if (strlen(name) >= MUSIC_NAME_MAX) continue;
        // A piece the player has put in their own directory REPLACES the
        // shipped one of the same name rather than doubling it, which is
        // how they swap an arrangement they do not like.
        bool dup = false;
        for (int i = 0; i < s_n_tracks; i++) {
            if (ieq(s_tracks[i], name)) {
                s_dirs[i][0] = (char)('0' + which);
                dup          = true;
                break;
            }
        }
        if (dup) continue;
        snprintf(s_tracks[s_n_tracks], MUSIC_NAME_MAX, "%s", name);
        s_dirs[s_n_tracks][0] = (char)('0' + which);
        s_dirs[s_n_tracks][1] = '\0';
        s_n_tracks++;
        found++;
    }
    sm_dir_close(d);
    ESP_LOGI(TAG, "%s: %d piece%s", path, found, found == 1 ? "" : "s");
}

void music_init(void) {
    if (s_started) return;
    s_started  = true;
    s_n_tracks = 0;

    char shipped[160];
    snprintf(shipped, sizeof(shipped), "%s/music", graceloader_get_install_basepath());
    scan_dir(0, shipped);

    char own[160];
    snprintf(own, sizeof(own), "%s/music", SM_DATA_DIR);
    scan_dir(1, own);

    if (s_n_tracks == 0) {
        ESP_LOGW(TAG, "no music found; the game plays in silence");
        return;
    }
    ESP_LOGI(TAG, "%d piece%s in the pool", s_n_tracks, s_n_tracks == 1 ? "" : "s");

    // Seed from the clock so two runs do not open with the same piece.
    s_rng ^= (uint32_t)esp_log_timestamp() * 2654435761u;
    if (s_rng == 0) s_rng = 0x243F6A88u;

    s_gap_left = rnd_range(FIRST_GAP_MIN_S, FIRST_GAP_MAX_S);
    audio_mixer_set_music(&s_source);
}

// --- Choosing and loading ---------------------------------------------

static int choose_track(void) {
    if (s_n_tracks <= 1) return 0;
    int pick = s_last;
    // With two pieces this always lands on the other one; with more it
    // is a fair draw among the rest.
    for (int guard = 0; guard < 16 && pick == s_last; guard++) {
        pick = (int)(rnd() % (uint32_t)s_n_tracks);
    }
    return pick;
}

static bool load_track(int idx) {
    if (idx < 0 || idx >= s_n_tracks || idx >= MUSIC_MAX_TRACKS) return false;

    char path[sizeof(s_dir_path[0]) + MUSIC_NAME_MAX + 2];
    int const which = s_dirs[idx][0] - '0';
    snprintf(path, sizeof(path), "%.*s/%.*s", (int)sizeof(s_dir_path[0]) - 1, s_dir_path[which & 1],
             MUSIC_NAME_MAX - 1, s_tracks[idx]);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long const size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 14 || (unsigned long)size > MUSIC_MAX_BYTES) {
        ESP_LOGW(TAG, "%s: %ld bytes is not a MIDI file we will play", s_tracks[idx], size);
        fclose(f);
        return false;
    }

    uint8_t* buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (buf == NULL) buf = heap_caps_malloc((size_t)size, MALLOC_CAP_DEFAULT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "no memory for %s (%ld bytes)", s_tracks[idx], size);
        fclose(f);
        return false;
    }
    size_t const got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        ESP_LOGW(TAG, "%s: short read", s_tracks[idx]);
        heap_caps_free(buf);
        return false;
    }

    s_buf = buf;
    s_len = got;
    snprintf(s_now, sizeof(s_now), "%.*s", MUSIC_NAME_MAX - 1, s_tracks[idx]);
    s_last = idx;
    // Release: everything above must be visible to the mixer task before
    // it sees LOADED.
    atomic_store_explicit(&s_state, MUS_LOADED, memory_order_release);
    ESP_LOGI(TAG, "playing %s (%u bytes)", s_now, (unsigned)got);
    return true;
}

void music_frame(float dt) {
    if (!s_started || s_n_tracks == 0) return;

    int const state = atomic_load_explicit(&s_state, memory_order_acquire);

    if (state == MUS_DONE) {
        // The mixer has let go. Free the file and start a new silence.
        heap_caps_free(s_buf);
        s_buf      = NULL;
        s_len      = 0;
        s_now[0]   = '\0';
        s_gap_left = rnd_range(GAP_MIN_S, GAP_MAX_S);
        atomic_store_explicit(&s_state, MUS_IDLE, memory_order_release);
        ESP_LOGI(TAG, "quiet for %.0f s", (double)s_gap_left);
        return;
    }
    if (state != MUS_IDLE) return;  // loaded or playing: nothing to do here

    // Music switched off in the settings: let the clock run anyway, so
    // turning it back on does not start a piece the same second.
    if (dt > 0.0f) s_gap_left -= dt;
    if (s_gap_left > 0.0f) return;
    if (!settings_music()) {
        s_gap_left = rnd_range(GAP_MIN_S, GAP_MAX_S);
        return;
    }

    if (!load_track(choose_track())) {
        // A bad file must not spin: wait a while before trying another.
        s_gap_left = rnd_range(GAP_MIN_S, GAP_MAX_S);
    }
}

void music_skip(void) {
    if (!s_started) return;
    int const state = atomic_load_explicit(&s_state, memory_order_acquire);
    if (state == MUS_PLAYING) {
        // Let the render loop notice and hand the buffer back.
        atomic_store_explicit(&s_state, MUS_DONE, memory_order_release);
        s_gap_left = 0.0f;
    } else if (state == MUS_IDLE) {
        s_gap_left = 0.0f;
    }
}

void music_stop(void) {
    if (!s_started) return;
    // ORDER MATTERS, and the caller has already got it right: sm_audio_
    // shutdown() parks the mixer task BEFORE calling this. Freeing the
    // file while the mixer might still be rendering out of it is a
    // use-after-free on the audio task, which is the one place it would
    // be hardest to recognise. Clearing the slot here as well is belt and
    // braces -- ours is a static source with no shutdown() to fire.
    audio_mixer_set_music(NULL);
    atomic_store_explicit(&s_state, MUS_IDLE, memory_order_release);
    heap_caps_free(s_buf);
    s_buf    = NULL;
    s_len    = 0;
    s_now[0] = '\0';
    s_started = false;
}

int music_track_count(void) {
    return s_n_tracks;
}

char const* music_now_playing(void) {
    return s_now[0] ? s_now : NULL;
}

float music_seconds_to_next(void) {
    if (atomic_load_explicit(&s_state, memory_order_relaxed) != MUS_IDLE) return 0.0f;
    return s_gap_left > 0.0f ? s_gap_left : 0.0f;
}
