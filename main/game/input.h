#pragma once
// =====================================================================
//  CraftMiner  --  controls
// ---------------------------------------------------------------------
//  Every key is remappable, which is why nothing below the surface ever
//  names one. The game declares its ACTIONS; se_bindings.h owns which
//  scancode each is bound to and persists it to NVS; the forthcoming
//  controls menu (step 6.1) rebinds them. A binding is a raw scancode,
//  so it works both polled (held, for movement) and matched against an
//  event (an edge, for a hotbar slot).
//
//  LOOKING GOES THROUGH AN ABSTRACTION on purpose. input_look() returns
//  a delta per tick, and today it comes from the cursor keys being
//  held. When a mouse or the accelerometer arrives it comes from there
//  instead and NOT ONE CALL SITE CHANGES -- which is the whole point,
//  and the user asked for it by name.
//
//  THE TICK SAMPLES, THE FRAME DOES NOT. Movement is read once per
//  simulation tick into a bitmask (D-02), never per frame, so the same
//  bitmask stream replays to the same world whatever the frame rate.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// The actions, in binding order. The values are the bit positions in
// the per-tick mask, so THEY MUST NOT BE REORDERED once a replay or a
// saved binding exists.
typedef enum {
    CM_FORWARD = 0,
    CM_BACK,
    CM_LEFT,
    CM_RIGHT,
    CM_JUMP,
    CM_SNEAK,
    CM_ATTACK,     // "left mouse button" -- break
    CM_USE,        // "right mouse button" -- place
    CM_LOOK_UP,
    CM_LOOK_DOWN,
    CM_LOOK_LEFT,
    CM_LOOK_RIGHT,
    CM_SLOT1,
    CM_SLOT2,
    CM_SLOT3,
    CM_SLOT4,
    CM_SLOT5,
    CM_SLOT6,
    CM_INVENTORY,
    CM_PAUSE,
    CM_DROP,
    CM_ACTION_COUNT
} cm_action_t;

// The per-tick mask. 21 actions, so a uint32 with room to spare.
typedef uint32_t cm_actions_t;

static inline bool act_held(cm_actions_t m, cm_action_t a) {
    return (m & ((cm_actions_t)1u << a)) != 0;
}

// Register the control set with the engine and load any saved
// bindings. Call once at boot.
void input_init(void);

// Read the keyboard NOW and return the mask. Called once per
// simulation tick, not once per frame.
cm_actions_t input_sample(void);

// Edges: actions that went down between the last two samples. What a
// hotbar slot, the inventory key and a single block placement want --
// holding a key must not fire them sixty times.
cm_actions_t input_pressed(void);

// The look delta for this tick, in radians. The abstraction a mouse
// will one day feed instead of the cursor keys; `mask` is the tick's
// own sample, so a replay looks exactly where the recording did.
void input_look(cm_actions_t mask, float* dyaw, float* dpitch);

// A human label for a scancode, for the controls menu (step 6.1).
char const* input_action_label(cm_action_t a);
