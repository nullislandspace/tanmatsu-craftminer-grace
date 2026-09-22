// =====================================================================
//  CraftMiner  --  the time of day (see daytime.h)
// =====================================================================

#include "game/daytime.h"

#include <math.h>
#include <stddef.h>

#define DAY_ARGB    0xFF8EC4F0u  // the world's daytime sky (CM_SKY_ARGB)
#define NIGHT_ARGB  0xFF0C1430u
#define SUNSET_SKY  0xFFF2A878u
#define SUNSET_FOG  0xFFE89468u
#define NIGHT_FOG   0xFF101A34u

// How many light levels night takes off the sky: 9 leaves a sunlit field
// at level 6 -- dim, much darker than a torch, and still readable on
// this screen (11, Minecraft's, left the title unreadable; F-57).
#define NIGHT_DARKENING 9

static uint32_t mix(uint32_t a, uint32_t b, float f) {
    if (f <= 0.0f) return a;
    if (f >= 1.0f) return b;
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float const ca = (float)((a >> s) & 0xFFu), cb = (float)((b >> s) & 0xFFu);
        out |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << s;
    }
    return out;
}

static float fraction(int64_t ticks) {
    int64_t t = ticks % DAY_TICKS;
    if (t < 0) t += DAY_TICKS;
    return (float)t / (float)DAY_TICKS;
}

daytime_t daytime_at(int64_t ticks) {
    // Sunrise at 0, noon at a quarter, sunset at a half, midnight at
    // three quarters. Tilted a little south (+z... the sun never stands
    // exactly overhead), so noon still models the sides of things.
    float const a  = 2.0f * 3.14159265f * fraction(ticks);
    float const h  = sinf(a);  // the sun's height, -1..1
    vec3_t const s = v3_norm(v3(cosf(a), h, 0.35f));

    float const day = smoothstep(-0.12f, 0.25f, h);
    // The orange band: strongest with the sun on the horizon, gone once
    // it is a third of the way up (or well below).
    float const band = h > -0.25f ? fmaxf(0.0f, 1.0f - fabsf(h + 0.02f) / 0.30f) : 0.0f;

    uint32_t const base_sky = mix(NIGHT_ARGB, DAY_ARGB, day);
    uint32_t const base_fog = mix(NIGHT_FOG, DAY_ARGB, day);
    return (daytime_t){
        .sun_dir  = s,
        .sky_argb = mix(base_sky, SUNSET_SKY, 0.65f * band),
        .fog_argb = mix(base_fog, SUNSET_FOG, 0.55f * band),
        .day      = day,
    };
}

void daytime_light_lut(float day, uint8_t lut[256]) {
    // The curve once: brightness of each effective level, as a fraction.
    // 0.8 per level is Minecraft's; the floor keeps a pitch-black cave
    // from being literally black, which on this screen reads as a hole.
    static float curve[16];
    static int   built;
    if (!built) {
        for (int l = 0; l <= 15; l++) curve[l] = 0.06f + 0.94f * powf(0.8f, (float)(15 - l));
        built = 1;
    }
    if (day < 0.0f) day = 0.0f;
    if (day > 1.0f) day = 1.0f;
    int const dark = (int)((1.0f - day) * (float)NIGHT_DARKENING + 0.5f);
    for (int i = 0; i < 256; i++) {
        int const sky = (i >> 4) - dark, blk = i & 0x0F;
        int const eff = sky > blk ? sky : blk;
        lut[i]        = (uint8_t)(curve[eff < 0 ? 0 : eff] * 32.0f + 0.5f);
    }
}

void daytime_clock(int64_t ticks, int* hours, int* minutes) {
    // Sunrise (tick 0) is 06:00; a day is 24 hours of 1000 ticks.
    int const m = (int)(fraction(ticks) * 24.0f * 60.0f + 0.5f) + 6 * 60;
    if (hours != NULL) *hours = (m / 60) % 24;
    if (minutes != NULL) *minutes = m % 60;
}
