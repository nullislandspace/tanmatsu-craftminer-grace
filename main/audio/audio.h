#pragma once
// =====================================================================
//  CraftMiner  --  the audio system's lifetime
// ---------------------------------------------------------------------
//  The engine's mixer (se_audio.h) owns the one I2S channel and sleeps
//  the amplifier when nothing is playing. This file starts it, stops it,
//  and is where the game asks for the things that are not a single
//  effect: the walking sounds, which need to know how far the feet have
//  travelled, and the music, which needs to know how long the silence
//  has lasted.
//
//  Effects themselves are sfx.h; the music is music.h. Neither is
//  reachable before cm_audio_init(), and both are silent rather than
//  broken if it failed -- a badge whose speaker will not start still
//  plays the game.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game/player.h"

// Start the mixer and push the player's music / effects choices into it.
// After this the game may make noise. Idempotent; false if the mixer
// would not start, in which case everything below is a silent no-op.
bool cm_audio_init(void);

// Stop the speaker NOW. Before restarting into the launcher, so it does
// not sit on residual DMA samples and squeal.
void cm_audio_shutdown(void);

// Once per rendered frame, with the seconds since the last one. Drives
// the music scheduler's long silences (music.h). Safe before init.
void cm_audio_frame(float dt);

// Once per fixed tick, while a world is being played. Walking sounds
// live here rather than in physics.c because they are a consequence of
// the tick, not a part of it: the tick must produce the same world with
// the speaker muted (Part T), so nothing here may touch game state.
void cm_audio_player_tick(player_t const* p);

// Leaving a world: stop the footsteps mid-stride and forget how far the
// player had walked, so re-entering does not start on a half step.
void cm_audio_leave_world(void);
