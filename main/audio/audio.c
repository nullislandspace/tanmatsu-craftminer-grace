// =====================================================================
//  SynthMiner  --  starting the speaker, and the sounds that need state
//  See audio.h.
// =====================================================================

#include "audio/audio.h"

#include "audio/music.h"
#include "audio/sfx.h"
#include "se_audio.h"
#include "ui/settings.h"
#include "world/chunk.h"

#include "esp_log.h"

#include <math.h>

static char const TAG[] = "sm_audio";

static bool s_up = false;

// How far the player has walked since the last footstep, in blocks. A
// step every STEP_DISTANCE blocks rather than every N ticks, so walking
// and being pushed sound different and a slow walk does not machine-gun.
#define STEP_DISTANCE 2.1f
// Falling further than this lands audibly. Below it, stepping off a kerb
// is silent, which is what it should be.
#define LAND_SPEED 0.14f  // blocks per tick, downwards, at the moment of landing

static float  s_step_accum = 0.0f;
static double s_last_x = 0.0, s_last_z = 0.0;
static bool   s_have_last = false;
static float  s_fall_speed = 0.0f;  // the last airborne vy, kept for the landing
static bool   s_was_wet = false;    // in the water last tick, for the splash on entry

bool sm_audio_init(void) {
    if (s_up) return true;
    esp_err_t const err = audio_mixer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mixer would not start (%s); the game runs silent", esp_err_to_name(err));
        return false;
    }
    // The gates the settings file already restored -- settings_load()
    // pushed them at a point when there was no mixer to receive them.
    audio_mixer_set_music_enabled(settings_music());
    audio_mixer_set_group_enabled(SETTINGS_SFX_GROUP, settings_sfx());
    audio_mixer_set_music_volume(settings_music_volume());
    audio_mixer_set_group_volume(SETTINGS_SFX_GROUP, settings_sfx_volume());
    // Hold the speaker up for as long as the game is running. Without
    // this the mixer powers the amplifier down after ~46 ms of quiet,
    // and a 35 ms tool click registered against a cold amp is over
    // before the speaker is listening -- which showed up as "the tool
    // sounds only play when the music is on", because an installed
    // music source keeps the mixer busy every chunk whether it is
    // making any sound or not (se_audio.h).
    audio_mixer_keep_awake(true);
    s_up = true;
    music_init();
    ESP_LOGI(TAG, "audio up: music %s, effects %s",
             settings_music() ? "on" : "off", settings_sfx() ? "on" : "off");
    return true;
}

void sm_audio_shutdown(void) {
    if (!s_up) return;
    audio_mixer_keep_awake(false);
    // The mixer FIRST: audio_mixer_shutdown() is synchronous and parks
    // the mixer task, so after it nothing is rendering. Only then may
    // music_stop() free the file the music source was reading out of.
    audio_mixer_shutdown();
    music_stop();
    s_up = false;
}

void sm_audio_frame(float dt) {
    if (!s_up) return;
    music_frame(dt);
}

void sm_audio_leave_world(void) {
    s_step_accum = 0.0f;
    s_have_last  = false;
    s_fall_speed = 0.0f;
    s_was_wet    = false;
    if (s_up) audio_mixer_stop_all_voices();
}

// What the player is standing ON: the block below their feet. A little
// below, because the body rests exactly on the boundary and the cell at
// the feet is the air they are standing in.
static uint8_t ground_block(player_t const* p) {
    int32_t const bx = (int32_t)floor(p->body.x);
    int32_t const by = (int32_t)floor(p->body.y - 0.1);
    int32_t const bz = (int32_t)floor(p->body.z);
    return world_block(bx, by, bz);
}

// The block the player is standing IN, for the splash: wading through
// water sounds like water, whatever the riverbed is made of.
static uint8_t body_block(player_t const* p) {
    int32_t const bx = (int32_t)floor(p->body.x);
    int32_t const by = (int32_t)floor(p->body.y + 0.2);
    int32_t const bz = (int32_t)floor(p->body.z);
    return world_block(bx, by, bz);
}

void sm_audio_player_tick(player_t const* p) {
    if (!s_up || p == NULL) return;

    // Hitting the water. The same edge as a landing, and the same
    // reason: a fall that ends in a lake should be heard, and at the
    // moment it ends rather than a tick later.
    bool const wet = block_liquid(body_block(p));
    if (wet && !s_was_wet && s_fall_speed > LAND_SPEED) {
        sfx_play_pitched(SFX_BREAK_SPLASH, -2.0f);
    }
    s_was_wet = wet;

    // Landing. player_t.in_air_last is the previous tick's answer, which
    // is exactly the edge we want: airborne then not.
    if (p->body.on_ground && p->in_air_last) {
        if (s_fall_speed > LAND_SPEED) {
            uint8_t const under = ground_block(p);
            // A long drop is louder and lower; a hop is barely there.
            float const drop = s_fall_speed > 0.8f ? 0.8f : s_fall_speed;
            sfx_play_pitched(SFX_LAND, -6.0f * (drop / 0.8f));
            if (block_sound(under) != SND_NONE) sfx_play_step(under);
        }
        s_step_accum = 0.0f;  // the next step starts from the landing
    }
    s_fall_speed = p->body.on_ground ? 0.0f : -p->body.vy;
    if (s_fall_speed < 0.0f) s_fall_speed = 0.0f;

    // Footsteps, by distance travelled on the ground.
    if (!s_have_last) {
        s_last_x    = p->body.x;
        s_last_z    = p->body.z;
        s_have_last = true;
        return;
    }
    double const dx = p->body.x - s_last_x;
    double const dz = p->body.z - s_last_z;
    s_last_x = p->body.x;
    s_last_z = p->body.z;

    if (!p->body.on_ground) return;
    float const moved = (float)sqrt(dx * dx + dz * dz);
    if (moved < 0.0005f) return;  // standing still, or turning on the spot

    s_step_accum += moved;
    if (s_step_accum < STEP_DISTANCE) return;
    s_step_accum -= STEP_DISTANCE;

    uint8_t const in = body_block(p);
    if (block_sound(in) == SND_SPLASH) {
        sfx_play(SFX_STEP_SPLASH);
    } else {
        sfx_play_step(ground_block(p));
    }
}
