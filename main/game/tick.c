// =====================================================================
//  SynthMiner  --  the simulation clock (see tick.h)
// =====================================================================

#include "game/tick.h"

void tick_reset(tick_clock_t* c, double now) {
    c->accum   = 0.0;
    c->last    = now;
    c->count   = 0;
    c->started = true;
    c->frozen  = false;
}

void tick_freeze(tick_clock_t* c, bool on) {
    c->frozen = on;
}

int tick_due(tick_clock_t* c, double now) {
    if (!c->started) {
        tick_reset(c, now);
        return 0;
    }
    double dt = now - c->last;
    c->last   = now;
    if (dt < 0.0) dt = 0.0;  // the clock was set (the `shots` test does this)
    c->accum += dt;

    if (c->frozen) {
        c->accum = 0.0;
        return 0;
    }

    int n = 0;
    while (c->accum >= TICK_SECONDS && n < TICK_MAX_RUN) {
        c->accum -= TICK_SECONDS;
        n++;
    }
    // Whatever is still owed after the cap is FORGIVEN, not carried.
    // Carrying it means a frame that ran long is followed by frames
    // that also run long catching up, which is how a stutter becomes a
    // spiral.
    if (c->accum >= TICK_SECONDS) c->accum = 0.0;
    c->count += (uint64_t)n;
    return n;
}

float tick_alpha(tick_clock_t const* c) {
    float const a = (float)(c->accum / TICK_SECONDS);
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}
