// =====================================================================
//  CraftMiner  --  controls (see input.h)
// =====================================================================

#include "game/input.h"

#include <math.h>
#include <stdio.h>

#include "bsp/input.h"
#include "esp_log.h"
#include "graceloader_imu.h"
#include "gl_input.h"
#include "i18n/i18n.h"
#include "ui/settings.h"
#include "synthengine3d.h"

// How fast the cursor keys turn the view, radians a tick. At 20 Hz this
// is about 2.2 radians a second held down -- brisk without being
// impossible to aim with. A mouse will not use it.
#define LOOK_RATE 0.11f

// The declared control set. The third column is the action's stable id,
// what settings.txt keys its binding by (it was the NVS key when the
// bindings lived in NVS): it must never change once a build has shipped
// or everyone's bindings move.
static se_binding_def_t const BINDINGS[CM_ACTION_COUNT] = {
    [CM_FORWARD]    = {CM_FORWARD, "Forward", "fwd", BSP_INPUT_SCANCODE_W},
    [CM_BACK]       = {CM_BACK, "Back", "back", BSP_INPUT_SCANCODE_S},
    [CM_LEFT]       = {CM_LEFT, "Left", "left", BSP_INPUT_SCANCODE_A},
    [CM_RIGHT]      = {CM_RIGHT, "Right", "right", BSP_INPUT_SCANCODE_D},
    [CM_JUMP]       = {CM_JUMP, "Jump", "jump", BSP_INPUT_SCANCODE_SPACE},
    [CM_SNEAK]      = {CM_SNEAK, "Sneak", "sneak", BSP_INPUT_SCANCODE_LEFTSHIFT},
    [CM_ATTACK]     = {CM_ATTACK, "Break", "attack", BSP_INPUT_SCANCODE_Q},
    [CM_USE]        = {CM_USE, "Place", "use", BSP_INPUT_SCANCODE_E},
    [CM_LOOK_UP]    = {CM_LOOK_UP, "Look up", "lookup", BSP_INPUT_SCANCODE_ESCAPED_GREY_UP},
    [CM_LOOK_DOWN]  = {CM_LOOK_DOWN, "Look down", "lookdown", BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN},
    [CM_LOOK_LEFT]  = {CM_LOOK_LEFT, "Look left", "lookleft", BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT},
    [CM_LOOK_RIGHT] = {CM_LOOK_RIGHT, "Look right", "lookright", BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT},
    [CM_SLOT1]      = {CM_SLOT1, "Slot 1", "slot1", BSP_INPUT_SCANCODE_F1},
    [CM_SLOT2]      = {CM_SLOT2, "Slot 2", "slot2", BSP_INPUT_SCANCODE_F2},
    [CM_SLOT3]      = {CM_SLOT3, "Slot 3", "slot3", BSP_INPUT_SCANCODE_F3},
    [CM_SLOT4]      = {CM_SLOT4, "Slot 4", "slot4", BSP_INPUT_SCANCODE_F4},
    [CM_SLOT5]      = {CM_SLOT5, "Slot 5", "slot5", BSP_INPUT_SCANCODE_F5},
    [CM_SLOT6]      = {CM_SLOT6, "Slot 6", "slot6", BSP_INPUT_SCANCODE_F6},
    [CM_INVENTORY]  = {CM_INVENTORY, "Inventory", "inv", BSP_INPUT_SCANCODE_TAB},
    [CM_PAUSE]      = {CM_PAUSE, "Pause", "pause", BSP_INPUT_SCANCODE_ESC},
    [CM_DROP]       = {CM_DROP, "Drop", "drop", BSP_INPUT_SCANCODE_G},
    [CM_INFO]       = {CM_INFO, "Show position", "info", BSP_INPUT_SCANCODE_BACKSPACE},
    [CM_SCREENSHOT] = {CM_SCREENSHOT, "Screenshot", "screenshot", BSP_INPUT_SCANCODE_0},
    [CM_CRAFT]      = {CM_CRAFT, "Crafting", "craft", BSP_INPUT_SCANCODE_C},
};

// The navigation key a scancode ALSO arrives as, where there is one.
// Which of the two a keyboard produces depends on the keyboard -- the
// built-in one and a USB one through the graceloader do not agree about
// the cursor keys -- so a bound key is asked both ways. It follows the
// BINDING, not the action: bind Look up to W and the up arrow stops
// looking up.
static bsp_input_navigation_key_t nav_for(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP: return BSP_INPUT_NAVIGATION_KEY_UP;
        case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN: return BSP_INPUT_NAVIGATION_KEY_DOWN;
        case BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT: return BSP_INPUT_NAVIGATION_KEY_LEFT;
        case BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT: return BSP_INPUT_NAVIGATION_KEY_RIGHT;
        case BSP_INPUT_SCANCODE_SPACE: return BSP_INPUT_NAVIGATION_KEY_SPACE_M;
        case BSP_INPUT_SCANCODE_TAB: return BSP_INPUT_NAVIGATION_KEY_TAB;
        case BSP_INPUT_SCANCODE_ENTER: return BSP_INPUT_NAVIGATION_KEY_RETURN;
        case BSP_INPUT_SCANCODE_BACKSPACE: return BSP_INPUT_NAVIGATION_KEY_BACKSPACE;
        case BSP_INPUT_SCANCODE_F1: return BSP_INPUT_NAVIGATION_KEY_F1;
        case BSP_INPUT_SCANCODE_F2: return BSP_INPUT_NAVIGATION_KEY_F2;
        case BSP_INPUT_SCANCODE_F3: return BSP_INPUT_NAVIGATION_KEY_F3;
        case BSP_INPUT_SCANCODE_F4: return BSP_INPUT_NAVIGATION_KEY_F4;
        case BSP_INPUT_SCANCODE_F5: return BSP_INPUT_NAVIGATION_KEY_F5;
        case BSP_INPUT_SCANCODE_F6: return BSP_INPUT_NAVIGATION_KEY_F6;
        default: return BSP_INPUT_NAVIGATION_KEY_NONE;
    }
}

_Static_assert(CM_ACTION_COUNT <= SE_BINDINGS_MAX,
               "more actions than the engine keeps bindings for: se_bindings_init clamps, and the last ones "
               "would silently never fire (F-11)");

static cm_actions_t s_last, s_pressed;

// --- The gyroscope ------------------------------------------------------
//
// The RATE gyroscope, not the accelerometer synthracer steers by. Tilt
// is an absolute angle, which is right for a steering wheel and wrong
// for looking round: which way you face is not something gravity can
// tell you. So the turn rate is added up frame by frame and handed to
// the look like the cursor keys' delta -- turn the badge 30 degrees and
// the view turns 30 degrees.
//
// Held upright (graceloader_imu.h): device X runs top to bottom, so it
// is the vertical axis and turning left or right is a rotation about
// it; Y runs across, so tipping the screen back or forward is a rotation
// about that. The two signs are set from the axis diagram and are the
// first thing to flip if the badge turns the view the wrong way.
#define GYRO_YAW_SIGN   (+1.0f)  // gx > 0: turned right -> yaw grows (turns right)
#define GYRO_PITCH_SIGN (+1.0f)  // gy > 0 -> pitch grows (looks DOWN); measured on the badge, the diagram said -1
// One-to-one: a real degree is a view degree.
#define GYRO_GAIN 1.0f
// A resting gyroscope does not read zero. Readings this slow are taken
// as the sensor's own offset and tracked, not turned into a slow spin.
#define GYRO_REST_DPS  3.0f
#define GYRO_BIAS_RATE 0.02f
// However long a frame took, nothing turns further than this in one.
#define GYRO_MAX_STEP 0.6f

static bool  s_gyro_enabled;
static float s_bias_x, s_bias_y;
static float s_owed_yaw, s_owed_pitch;

static float gyro_axis(float rate, float* bias) {
    float const r = rate - *bias;
    if (fabsf(r) < GYRO_REST_DPS) {
        *bias += r * GYRO_BIAS_RATE;
        return 0.0f;
    }
    return r;
}

void input_gyro_frame(float dt, bool on) {
    if (!on) {
        s_owed_yaw = s_owed_pitch = 0.0f;
        return;
    }
    if (!s_gyro_enabled) {
        // Once, the first time it is wanted; left on after that, since
        // the draw is negligible (graceloader_imu.h).
        esp_err_t const e = bsp_orientation_enable_gyroscope();
        if (e != ESP_OK) {
            ESP_LOGW("input", "gyroscope did not start: %d -- looking stays on the keys", e);
            return;
        }
        s_gyro_enabled = true;
    }
    bool  ready = false;
    float gx = 0.0f, gy = 0.0f;
    if (bsp_orientation_get(&ready, NULL, &gx, &gy, NULL, NULL, NULL, NULL) != ESP_OK || !ready) return;

    float const k  = GYRO_GAIN * dt * (3.14159265f / 180.0f);
    float       dy = GYRO_YAW_SIGN * gyro_axis(gx, &s_bias_x) * k;
    float       dp = GYRO_PITCH_SIGN * gyro_axis(gy, &s_bias_y) * k;
    if (dy > GYRO_MAX_STEP) dy = GYRO_MAX_STEP;
    if (dy < -GYRO_MAX_STEP) dy = -GYRO_MAX_STEP;
    if (dp > GYRO_MAX_STEP) dp = GYRO_MAX_STEP;
    if (dp < -GYRO_MAX_STEP) dp = -GYRO_MAX_STEP;
    s_owed_yaw += dy;
    s_owed_pitch += dp;
}

void input_init(void) {
    // No NVS namespace: the engine keeps the bindings in memory, and
    // settings.c saves them to the SD card with everything else (D-67).
    static se_bindings_config_t const cfg = {
        .nvs_namespace = NULL,
        .defs          = BINDINGS,
        .count         = CM_ACTION_COUNT,
    };
    se_bindings_init(&cfg);
}

// The label a PLAYER sees, in their language. The English in BINDINGS
// above stays as it is: that column is the engine's, and the id column
// beside it is what settings.txt writes, so neither may move.
static cm_str_t const ACTION_STRINGS[CM_ACTION_COUNT] = {
    [CM_FORWARD]    = CM_STR_ACTION_FORWARD,
    [CM_BACK]       = CM_STR_ACTION_BACK,
    [CM_LEFT]       = CM_STR_ACTION_LEFT,
    [CM_RIGHT]      = CM_STR_ACTION_RIGHT,
    [CM_JUMP]       = CM_STR_ACTION_JUMP,
    [CM_SNEAK]      = CM_STR_ACTION_SNEAK,
    [CM_ATTACK]     = CM_STR_ACTION_ATTACK,
    [CM_USE]        = CM_STR_ACTION_USE,
    [CM_LOOK_UP]    = CM_STR_ACTION_LOOKUP,
    [CM_LOOK_DOWN]  = CM_STR_ACTION_LOOKDOWN,
    [CM_LOOK_LEFT]  = CM_STR_ACTION_LOOKLEFT,
    [CM_LOOK_RIGHT] = CM_STR_ACTION_LOOKRIGHT,
    [CM_SLOT1]      = CM_STR_ACTION_SLOT1,
    [CM_SLOT2]      = CM_STR_ACTION_SLOT2,
    [CM_SLOT3]      = CM_STR_ACTION_SLOT3,
    [CM_SLOT4]      = CM_STR_ACTION_SLOT4,
    [CM_SLOT5]      = CM_STR_ACTION_SLOT5,
    [CM_SLOT6]      = CM_STR_ACTION_SLOT6,
    [CM_INVENTORY]  = CM_STR_ACTION_INV,
    [CM_PAUSE]      = CM_STR_ACTION_PAUSE,
    [CM_DROP]       = CM_STR_ACTION_DROP,
    [CM_INFO]       = CM_STR_ACTION_INFO,
    [CM_SCREENSHOT] = CM_STR_ACTION_SCREENSHOT,
    [CM_CRAFT]      = CM_STR_ACTION_CRAFT,
};

char const* input_action_label(cm_action_t a) {
    return (a >= 0 && a < CM_ACTION_COUNT) ? T(ACTION_STRINGS[a]) : "";
}

char const* input_action_id(cm_action_t a) {
    return (a >= 0 && a < CM_ACTION_COUNT) ? BINDINGS[a].nvs_key : "";
}

static bool held_sc(uint16_t sc) {
    if (sc == 0) return false;
    bool state = false;
    return gl_input_read_scancode((bsp_input_scancode_t)sc, &state) == ESP_OK && state;
}

static bool held_nav(bsp_input_navigation_key_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_NONE) return false;
    bool state = false;
    return gl_input_read_navigation_key(key, &state) == ESP_OK && state;
}

cm_actions_t input_sample(void) {
    cm_actions_t m = 0;
    for (int a = 0; a < CM_ACTION_COUNT; a++) {
        // The CURRENT binding, not the default: that is the whole point
        // of going through se_bindings rather than the table above.
        uint16_t const sc = se_bindings_get(a);
        if (held_sc(sc) || held_nav(nav_for(sc))) m |= (cm_actions_t)1u << a;
    }
    s_pressed = m & ~s_last;
    s_last    = m;
    return m;
}

cm_actions_t input_feed(cm_actions_t mask) {
    s_pressed = mask & ~s_last;
    s_last    = mask;
    return mask;
}

void input_gyro_owed(float* dyaw, float* dpitch) {
    if (dyaw != NULL) *dyaw = s_owed_yaw;
    if (dpitch != NULL) *dpitch = s_owed_pitch;
}

void input_gyro_set_owed(float dyaw, float dpitch) {
    s_owed_yaw   = dyaw;
    s_owed_pitch = dpitch;
}

cm_actions_t input_pressed(void) {
    return s_pressed;
}

void input_look(cm_actions_t mask, float* dyaw, float* dpitch) {
    float const x = (act_held(mask, CM_LOOK_RIGHT) ? 1.0f : 0.0f) - (act_held(mask, CM_LOOK_LEFT) ? 1.0f : 0.0f);
    // Positive pitch looks DOWN (raycast.h), so "look up" is negative.
    float const y = (act_held(mask, CM_LOOK_DOWN) ? 1.0f : 0.0f) - (act_held(mask, CM_LOOK_UP) ? 1.0f : 0.0f);
    // The keys, plus whatever the badge turned since the last tick.
    if (dyaw != NULL) *dyaw = x * LOOK_RATE + s_owed_yaw;
    if (dpitch != NULL) *dpitch = y * LOOK_RATE + s_owed_pitch;
    s_owed_yaw = s_owed_pitch = 0.0f;
}

uint16_t input_key(cm_action_t a) {
    return (a >= 0 && a < CM_ACTION_COUNT) ? se_bindings_get(a) : 0;
}

uint16_t input_default_key(cm_action_t a) {
    return (a >= 0 && a < CM_ACTION_COUNT) ? BINDINGS[a].default_sc : 0;
}

void input_bind(cm_action_t a, uint16_t sc) {
    if (a < 0 || a >= CM_ACTION_COUNT || sc == 0) return;
    uint16_t const old = se_bindings_get(a);
    if (old == sc) return;
    for (int b = 0; b < CM_ACTION_COUNT; b++) {
        if (b != (int)a && se_bindings_get(b) == sc) se_bindings_set(b, old);
    }
    se_bindings_set(a, sc);
    settings_save();
}

void input_reset_defaults(void) {
    for (int a = 0; a < CM_ACTION_COUNT; a++) se_bindings_set(a, BINDINGS[a].default_sc);
    settings_save();
}

bool input_key_bound(uint16_t sc) {
    for (int a = 0; a < CM_ACTION_COUNT; a++) {
        if (se_bindings_get(a) == sc) return true;
    }
    return false;
}

char const* input_key_name(uint16_t sc, char* buf, int cap) {
    static struct {
        uint16_t    sc;
        char const* name;
    } const NAMES[] = {
        {BSP_INPUT_SCANCODE_ESC, "Esc"},
        {BSP_INPUT_SCANCODE_SPACE, "Space"},
        {BSP_INPUT_SCANCODE_ENTER, "Enter"},
        {BSP_INPUT_SCANCODE_BACKSPACE, "Backspace"},
        {BSP_INPUT_SCANCODE_TAB, "Tab"},
        {BSP_INPUT_SCANCODE_CAPSLOCK, "Caps Lock"},
        {BSP_INPUT_SCANCODE_LEFTSHIFT, "Left Shift"},
        {BSP_INPUT_SCANCODE_RIGHTSHIFT, "Right Shift"},
        {BSP_INPUT_SCANCODE_LEFTCTRL, "Ctrl"},
        {BSP_INPUT_SCANCODE_LEFTALT, "Alt"},
        {BSP_INPUT_SCANCODE_FN, "Fn"},
        {BSP_INPUT_SCANCODE_MINUS, "-"},
        {BSP_INPUT_SCANCODE_EQUAL, "="},
        {BSP_INPUT_SCANCODE_LEFTBRACE, "["},
        {BSP_INPUT_SCANCODE_RIGHTBRACE, "]"},
        {BSP_INPUT_SCANCODE_SEMICOLON, ";"},
        {BSP_INPUT_SCANCODE_APOSTROPHE, "'"},
        {BSP_INPUT_SCANCODE_GRAVE, "`"},
        {BSP_INPUT_SCANCODE_BACKSLASH, "\\"},
        {BSP_INPUT_SCANCODE_COMMA, ","},
        {BSP_INPUT_SCANCODE_DOT, "."},
        {BSP_INPUT_SCANCODE_SLASH, "/"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_UP, "Up"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN, "Down"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT, "Left"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT, "Right"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_HOME, "Home"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_END, "End"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_PGUP, "Page Up"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_PGDN, "Page Down"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_INSERT, "Insert"},
        {BSP_INPUT_SCANCODE_ESCAPED_GREY_DEL, "Delete"},
        {BSP_INPUT_SCANCODE_ESCAPED_RCTRL, "Right Ctrl"},
        {BSP_INPUT_SCANCODE_ESCAPED_RALT, "Right Alt"},
        {BSP_INPUT_SCANCODE_ESCAPED_KPENTER, "Keypad Enter"},
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (NAMES[i].sc == sc) {
            snprintf(buf, (size_t)cap, "%s", NAMES[i].name);
            return buf;
        }
    }
    // The rows the scancode set lays out in order.
    static char const ROW1[] = "1234567890";
    static char const ROWQ[] = "QWERTYUIOP";
    static char const ROWA[] = "ASDFGHJKL";
    static char const ROWZ[] = "ZXCVBNM";
    if (sc >= BSP_INPUT_SCANCODE_1 && sc <= BSP_INPUT_SCANCODE_0) {
        snprintf(buf, (size_t)cap, "%c", ROW1[sc - BSP_INPUT_SCANCODE_1]);
    } else if (sc >= BSP_INPUT_SCANCODE_Q && sc <= BSP_INPUT_SCANCODE_P) {
        snprintf(buf, (size_t)cap, "%c", ROWQ[sc - BSP_INPUT_SCANCODE_Q]);
    } else if (sc >= BSP_INPUT_SCANCODE_A && sc <= BSP_INPUT_SCANCODE_L) {
        snprintf(buf, (size_t)cap, "%c", ROWA[sc - BSP_INPUT_SCANCODE_A]);
    } else if (sc >= BSP_INPUT_SCANCODE_Z && sc <= BSP_INPUT_SCANCODE_M) {
        snprintf(buf, (size_t)cap, "%c", ROWZ[sc - BSP_INPUT_SCANCODE_Z]);
    } else if (sc >= BSP_INPUT_SCANCODE_F1 && sc <= BSP_INPUT_SCANCODE_F10) {
        snprintf(buf, (size_t)cap, "F%d", sc - BSP_INPUT_SCANCODE_F1 + 1);
    } else if (sc == BSP_INPUT_SCANCODE_F11 || sc == BSP_INPUT_SCANCODE_F12) {
        snprintf(buf, (size_t)cap, "F%d", sc == BSP_INPUT_SCANCODE_F11 ? 11 : 12);
    } else if (sc == 0) {
        snprintf(buf, (size_t)cap, "(none)");
    } else {
        snprintf(buf, (size_t)cap, "Key %04X", (unsigned)sc);
    }
    return buf;
}
