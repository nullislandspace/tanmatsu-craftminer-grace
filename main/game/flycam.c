// =====================================================================
//  CraftMiner  --  a camera you can fly by hand (see flycam.h)
// =====================================================================

#include "game/flycam.h"
#include <math.h>
#include "bsp/input.h"
#include "gl_input.h"
#include "world/chunk.h"

#define FLY_SPEED 14.0f  // blocks a second
#define FLY_BOOST 3.0f   // ... times this, with Ctrl down
#define FLY_LOOK  1.8f   // radians a second
#define FLY_EYE_H 3.0f   // above the ground, when first placed
#define PITCH_MAX 1.5f   // just short of straight up or down

// True while the key is held. A key the keyboard does not have simply
// reads false, which is what makes it safe to ask about several.
static bool held(bsp_input_scancode_t sc) {
    bool state = false;
    return gl_input_read_scancode(sc, &state) == ESP_OK && state;
}

// A pair of keys read as -1, 0 or +1.
static float axis(bsp_input_scancode_t neg, bsp_input_scancode_t pos) {
    return (held(pos) ? 1.0f : 0.0f) - (held(neg) ? 1.0f : 0.0f);
}

// The cursor keys, which are the one thing here that is not a plain
// letter. A PC keyboard sends them as escaped "grey" scancodes; the
// BSP also exposes them as navigation keys, and which of the two a
// given keyboard (built-in, or USB through the graceloader) produces is
// not something to guess at from here. Ask both and take either.
static bool nav_held(bsp_input_navigation_key_t key) {
    bool state = false;
    return gl_input_read_navigation_key(key, &state) == ESP_OK && state;
}

static float look_axis(bsp_input_scancode_t neg_sc, bsp_input_navigation_key_t neg_nav, bsp_input_scancode_t pos_sc,
                       bsp_input_navigation_key_t pos_nav) {
    bool const pos = held(pos_sc) || nav_held(pos_nav);
    bool const neg = held(neg_sc) || nav_held(neg_nav);
    return (pos ? 1.0f : 0.0f) - (neg ? 1.0f : 0.0f);
}

void flycam_reset(flycam_t* f, double wx, double wz, float wy, float yaw) {
    f->wx     = wx;
    f->wz     = wz;
    f->wy     = wy;
    f->yaw    = yaw;
    f->pitch  = 0.18f;  // slightly down; see flycam_update for the sign
    f->placed = false;
}

void flycam_update(flycam_t* f, float dt) {
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;  // a long stall must not teleport the camera

    // Once, when the chunk under it has arrived: stand it on the
    // ground. Until then it hangs at whatever height it was reset to,
    // which is above sea level, so the world streams in underneath it
    // rather than around it.
    if (!f->placed) {
        int const g = world_ground((int32_t)floor(f->wx), (int32_t)floor(f->wz));
        if (g > 0) {
            f->wy     = (float)g + FLY_EYE_H;
            f->placed = true;
        }
    }

    // Looking. The engine's convention, which is not the obvious one and
    // is worth stating where it is used (se_scene.c, camera_build_basis):
    //
    //     forward = ( sin yaw cos pitch, -sin pitch, cos yaw cos pitch )
    //     right   = ( cos yaw,            0,        -sin yaw           )
    //
    // So yaw turns towards `right` as it grows, and POSITIVE PITCH LOOKS
    // DOWN. Getting that backwards is how a camera ends up staring at
    // the sky while the code says it is looking at the ground.
    float const look_x  = look_axis(BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT, BSP_INPUT_NAVIGATION_KEY_LEFT,
                                    BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT, BSP_INPUT_NAVIGATION_KEY_RIGHT);
    float const look_y  = look_axis(BSP_INPUT_SCANCODE_ESCAPED_GREY_UP, BSP_INPUT_NAVIGATION_KEY_UP,
                                    BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN, BSP_INPUT_NAVIGATION_KEY_DOWN);
    f->yaw             += look_x * FLY_LOOK * dt;
    f->pitch           += look_y * FLY_LOOK * dt;
    if (f->pitch > PITCH_MAX) f->pitch = PITCH_MAX;
    if (f->pitch < -PITCH_MAX) f->pitch = -PITCH_MAX;

    // Moving. Forward is where it is looking, flattened: a debug camera
    // that sinks when you look down is exhausting to steer.
    float const fwd    = axis(BSP_INPUT_SCANCODE_S, BSP_INPUT_SCANCODE_W);
    float const strafe = axis(BSP_INPUT_SCANCODE_A, BSP_INPUT_SCANCODE_D);
    bool const  up     = held(BSP_INPUT_SCANCODE_SPACE) || nav_held(BSP_INPUT_NAVIGATION_KEY_SPACE_L) ||
                    nav_held(BSP_INPUT_NAVIGATION_KEY_SPACE_M) || nav_held(BSP_INPUT_NAVIGATION_KEY_SPACE_R);
    bool const  down  = held(BSP_INPUT_SCANCODE_LEFTSHIFT) || held(BSP_INPUT_SCANCODE_RIGHTSHIFT);
    float const climb = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
    float const speed = FLY_SPEED * (held(BSP_INPUT_SCANCODE_LEFTCTRL) ? FLY_BOOST : 1.0f) * dt;

    float const cy = cosf(f->yaw), sy = sinf(f->yaw);
    f->wx += (double)((fwd * sy + strafe * cy) * speed);
    f->wz += (double)((fwd * cy - strafe * sy) * speed);
    f->wy += climb * speed;

    // Bedrock underfoot and a ceiling well above the tallest build:
    // outside those the view is only sky or only rock.
    if (f->wy < 1.0f) f->wy = 1.0f;
    if (f->wy > (float)CH_H + 24.0f) f->wy = (float)CH_H + 24.0f;
}
