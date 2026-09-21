// =====================================================================
//  CraftMiner  --  controls (see input.h)
// =====================================================================

#include "game/input.h"

#include "bsp/input.h"
#include "gl_input.h"
#include "synthengine3d.h"

// How fast the cursor keys turn the view, radians a tick. At 20 Hz this
// is about 2.2 radians a second held down -- brisk without being
// impossible to aim with. A mouse will not use it.
#define LOOK_RATE 0.11f

// The declared control set. `nvs_key` is at most 15 characters, which
// is the NVS limit, and must never change once a build has shipped or
// everyone's bindings move.
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
};

// The navigation key each action ALSO answers to, where there is one.
// Which of a scancode and a navigation event a keyboard produces
// depends on the keyboard -- the built-in one and a USB one through the
// graceloader do not agree about the cursor keys -- so the ones that
// matter are asked both ways.
static bsp_input_navigation_key_t const NAV[CM_ACTION_COUNT] = {
    [CM_LOOK_UP]    = BSP_INPUT_NAVIGATION_KEY_UP,
    [CM_LOOK_DOWN]  = BSP_INPUT_NAVIGATION_KEY_DOWN,
    [CM_LOOK_LEFT]  = BSP_INPUT_NAVIGATION_KEY_LEFT,
    [CM_LOOK_RIGHT] = BSP_INPUT_NAVIGATION_KEY_RIGHT,
    [CM_JUMP]       = BSP_INPUT_NAVIGATION_KEY_SPACE_M,
};

static cm_actions_t s_last, s_pressed;

void input_init(void) {
    static se_bindings_config_t const cfg = {
        .nvs_namespace = "craftminer",
        .defs          = BINDINGS,
        .count         = CM_ACTION_COUNT,
    };
    se_bindings_init(&cfg);
}

char const* input_action_label(cm_action_t a) {
    return (a >= 0 && a < CM_ACTION_COUNT) ? BINDINGS[a].label : "";
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
        if (held_sc(se_bindings_get(a)) || held_nav(NAV[a])) m |= (cm_actions_t)1u << a;
    }
    s_pressed = m & ~s_last;
    s_last    = m;
    return m;
}

cm_actions_t input_pressed(void) {
    return s_pressed;
}

void input_look(cm_actions_t mask, float* dyaw, float* dpitch) {
    float const x = (act_held(mask, CM_LOOK_RIGHT) ? 1.0f : 0.0f) - (act_held(mask, CM_LOOK_LEFT) ? 1.0f : 0.0f);
    // Positive pitch looks DOWN (raycast.h), so "look up" is negative.
    float const y = (act_held(mask, CM_LOOK_DOWN) ? 1.0f : 0.0f) - (act_held(mask, CM_LOOK_UP) ? 1.0f : 0.0f);
    if (dyaw != NULL) *dyaw = x * LOOK_RATE;
    if (dpitch != NULL) *dpitch = y * LOOK_RATE;
}
