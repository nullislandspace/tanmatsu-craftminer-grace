#pragma once
// =====================================================================
//  SynthMiner  --  the music, and the long silences between it
// ---------------------------------------------------------------------
//  Music here works the way it did in the Minecraft betas this game is
//  modelled on: a piece starts, it ends, and then there is nothing for
//  several minutes. It is not a soundtrack playing under the game, it is
//  something that happens to the game occasionally, and the silence is
//  as much the point as the music. So:
//
//    * the next piece is CHOSEN AT RANDOM, never a playlist in order;
//    * never the same piece twice running, however small the library;
//    * the gap is random too, minutes long, and a fresh one each time.
//
//  What plays is a Standard MIDI File (audio/midi_seq.h) through our own
//  synth (audio/midi_synth.h) -- kilobytes per piece and no decoder task,
//  where an MP3 of the same music is megabytes and a 32 KB stack. The
//  repertoire is out-of-copyright classical, and both the composition
//  and the particular engraving have to be free: see audio/MUSIC.md,
//  which records where every file came from and under what licence.
//
//  WHERE THE FILES ARE. Two directories, both scanned, and a piece in
//  either is in the pool:
//
//    <install>/music/      what the game ships with
//    /sd/synthminer/music/ whatever the player has put there
//
//  So a player can add their own pieces, or delete ours, without a
//  toolchain -- the same arrangement the translations use.
//
//  THREADS. The mixer renders on its own task and may not touch the SD
//  card, so reading the next file is the game thread's job (music_frame)
//  and the two hand over through one atomic state word. Nothing here
//  blocks the audio path; if a file is slow to load, the silence simply
//  lasts a moment longer.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// Scan for pieces and install the music source. Called by sm_audio_init;
// harmless if there are none, in which case the game is simply quiet.
void music_init(void);

// The scheduler. Once per frame from the game thread, with the seconds
// since the last one. Reads the SD card when it is time for a new piece,
// so it must NOT be called from the audio task.
void music_frame(float dt);

// Fade nothing, stop now: for leaving the app. Safe to call twice.
void music_stop(void);

// Start the next piece as soon as the current one ends, rather than
// after the usual gap -- and if nothing is playing, start one now.
void music_skip(void);

// How many pieces were found across both directories.
int music_track_count(void);

// The file currently sounding, or NULL during a silence. For the log and
// the debug overlay; the string belongs to the module.
char const* music_now_playing(void);

// Seconds until the next piece, or 0 while one is playing. For the
// debug overlay -- a silence that is meant to last eight minutes is
// otherwise indistinguishable from a bug.
float music_seconds_to_next(void);
