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
    CM_INFO,        // the coordinates-and-heading overlay
    CM_SCREENSHOT,  // save what is on screen to screenshots/ on the card
    CM_ACTION_COUNT
} cm_action_t;

// The per-tick mask. 23 actions, so a uint32 with room to spare.
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

// A mask from somewhere other than the keyboard -- a replay -- put
// through the same edge detection input_sample() does, so
// input_pressed() means the same thing either way. Returns `mask`.
cm_actions_t input_feed(cm_actions_t mask);

// Edges: actions that went down between the last two samples. What a
// hotbar slot, the inventory key and a single block placement want --
// holding a key must not fire them sixty times.
cm_actions_t input_pressed(void);

// Looking by turning the badge. Called once a FRAME with the frame's
// length while the player is the one looking (not paused, no inventory,
// not the free camera): it reads the gyroscope and adds up how far the
// badge turned, and the next input_look() hands that over. `on` false
// (the setting is off) reads nothing and discards anything owed. The
// cursor keys keep working alongside: the two deltas add.
void input_gyro_frame(float dt, bool on);

// The gyroscope turn owed to the next tick, radians: read it to record
// it, set it to replay one.
void input_gyro_owed(float* dyaw, float* dpitch);
void input_gyro_set_owed(float dyaw, float dpitch);

// The look delta for this tick, in radians. The abstraction a mouse
// will one day feed instead of the cursor keys; `mask` is the tick's
// own sample, so a replay looks exactly where the recording did.
void input_look(cm_actions_t mask, float* dyaw, float* dpitch);

// The action's name, for the controls menu ("Forward", "Jump", ...).
// In the player's language (i18n.h). The stable id an action is saved
// by is input_action_id(), which is never translated.
char const* input_action_label(cm_action_t a);

// The action's stable short id ("fwd", "jump"): what settings.txt keys
// its binding by. Never changes once shipped.
char const* input_action_id(cm_action_t a);

// The scancode an action is bound to NOW, and the one it shipped with.
uint16_t input_key(cm_action_t a);
uint16_t input_default_key(cm_action_t a);

// Bind `a` to `sc`, saved to settings.txt. If another action already had `sc`, it
// takes `a`'s old key instead -- a SWAP, so two actions never share a
// key and no action is ever left with none.
void input_bind(cm_action_t a, uint16_t sc);

// Every action back to its default, saved to settings.txt.
void input_reset_defaults(void);

// Is `sc` bound to any action? The debug keys (main.c) stand aside for
// one that is, so rebinding onto F never also toggles the flying camera.
bool input_key_bound(uint16_t sc);

// A keycap label for a scancode: "W", "Space", "Up", "F3". Written
// into `buf`, which is also returned.
char const* input_key_name(uint16_t sc, char* buf, int cap);
