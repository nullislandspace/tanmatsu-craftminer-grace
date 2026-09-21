// =====================================================================
//  CraftMiner  --  the player (see player.h)
// =====================================================================

#include "game/player.h"

#include <math.h>

#include "game/interact.h"
#include "world/chunk.h"

// Just short of straight up or down. Exactly +-pi/2 makes the forward
// vector's horizontal part zero, and then "which way am I walking" has
// no answer.
#define PITCH_MAX 1.55f

void player_spawn(player_t* p, double x, double z, float yaw) {
    int const g = world_ground((int32_t)floor(x), (int32_t)floor(z));
    phys_body_init(&p->body, x, (double)(g > 0 ? g : CH_SEA_LEVEL), z);
    p->yaw        = yaw;
    p->pitch      = 0.0f;
    p->prev_x     = p->body.x;
    p->prev_y     = p->body.y;
    p->prev_z     = p->body.z;
    p->prev_yaw   = yaw;
    p->prev_pitch = 0.0f;
    p->in_air_last = false;
    p->selected    = 0;
    p->aim_valid   = false;
}

// What the hotbar holds until block 4 gives it an inventory. Six slots
// of blocks a player can actually place, so break-and-place is testable
// now rather than after the inventory lands.
static uint8_t const HOTBAR[6] = {
    BLK_COBBLE, BLK_PLANKS, BLK_DIRT, BLK_GLASS, BLK_TORCH, BLK_SAND,
};

void player_tick(player_t* p, cm_actions_t mask, cm_actions_t pressed) {
    p->prev_x     = p->body.x;
    p->prev_y     = p->body.y;
    p->prev_z     = p->body.z;
    p->prev_yaw   = p->yaw;
    p->prev_pitch = p->pitch;

    // --- Looking ------------------------------------------------------
    float dyaw = 0.0f, dpitch = 0.0f;
    input_look(mask, &dyaw, &dpitch);
    p->yaw += dyaw;
    p->pitch += dpitch;
    if (p->pitch > PITCH_MAX) p->pitch = PITCH_MAX;
    if (p->pitch < -PITCH_MAX) p->pitch = -PITCH_MAX;

    // --- Walking ------------------------------------------------------
    //
    // Flattened: forward is where the player is facing, not where they
    // are looking. Looking at your feet must not slow you down.
    float const fwd = (act_held(mask, CM_FORWARD) ? 1.0f : 0.0f) - (act_held(mask, CM_BACK) ? 1.0f : 0.0f);
    float const str = (act_held(mask, CM_RIGHT) ? 1.0f : 0.0f) - (act_held(mask, CM_LEFT) ? 1.0f : 0.0f);
    float const speed = act_held(mask, CM_SNEAK) ? PL_SNEAK : PL_WALK;

    float wish_x = 0.0f, wish_z = 0.0f;
    if (fwd != 0.0f || str != 0.0f) {
        float const cy = cosf(p->yaw), sy = sinf(p->yaw);
        // The engine's basis: forward is (sin yaw, cos yaw) in x and z,
        // right is (cos yaw, -sin yaw). Kept identical to flycam.c and
        // raycast.c on purpose.
        wish_x = fwd * sy + str * cy;
        wish_z = fwd * cy - str * sy;
        float const len = sqrtf(wish_x * wish_x + wish_z * wish_z);
        if (len > 1e-6f) {
            wish_x = wish_x / len * speed;
            wish_z = wish_z / len * speed;
        }
    }
    // Ease towards the wanted velocity rather than snapping to it, so
    // stopping and turning have some weight. On the ground only: in the
    // air you keep what you had, which is what makes a jump commit.
    float const accel = p->body.on_ground ? PL_ACCEL : PL_ACCEL * 0.2f;
    p->body.vx += (wish_x - p->body.vx) * accel;
    p->body.vz += (wish_z - p->body.vz) * accel;

    // --- Jumping and falling -----------------------------------------
    //
    // MOVE WITH THE VELOCITY, THEN UPDATE IT. Applying gravity before
    // the move instead spends the first tick of a jump decelerating,
    // which costs a third of the height: the same three constants gave
    // an apex of 0.83 blocks that way and 1.25 this way. A jump that
    // cannot clear one block is not a jump, and it is not obvious from
    // reading the code -- only from simulating the arc.
    if (act_held(mask, CM_JUMP) && p->body.on_ground) p->body.vy = PL_JUMP;

    p->in_air_last = !p->body.on_ground;
    phys_move(&p->body, (double)p->body.vx, (double)p->body.vy, (double)p->body.vz);

    // A stopped body should not keep a velocity it cannot use, or it
    // shoots off the moment the obstruction goes away.
    if (p->body.hit_x) p->body.vx = 0.0f;
    if (p->body.hit_z) p->body.vz = 0.0f;

    // The vertical for the NEXT tick, including the landing and
    // head-bump cases. In physics.c so that the host test runs the
    // same code rather than a copy of it.
    phys_gravity(&p->body, PL_GRAVITY, PL_DRAG, PL_TERMINAL);

    // --- The hotbar ---------------------------------------------------
    for (int i = 0; i < 6; i++) {
        if (act_held(pressed, (cm_action_t)(CM_SLOT1 + i))) p->selected = i;
    }

    // --- What the crosshair is on ------------------------------------
    double const ex = p->body.x, ey = p->body.y + (double)PHYS_PLAYER_EYE, ez = p->body.z;
    float        dx, dy, dz;
    ray_forward(p->yaw, p->pitch, &dx, &dy, &dz);
    p->aim_valid = ray_pick(ex, ey, ez, dx, dy, dz, RAY_REACH, true, &p->aim);

    // --- Breaking and placing ----------------------------------------
    //
    // On the EDGE, not while held: block 4 gives breaking a progress
    // bar driven by hardness, and until then one press is one block.
    if (p->aim_valid && act_held(pressed, CM_ATTACK)) {
        interact_break(p->aim.x, p->aim.y, p->aim.z);
        p->aim_valid = false;  // whatever was aimed at is gone
    } else if (p->aim_valid && act_held(pressed, CM_USE)) {
        interact_place(&p->aim, HOTBAR[p->selected], &p->body);
    }
}

void player_eye(player_t const* p, float alpha, double* x, double* y, double* z, float* yaw, float* pitch) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    double const a = (double)alpha;
    if (x != NULL) *x = p->prev_x + (p->body.x - p->prev_x) * a;
    if (y != NULL) *y = p->prev_y + (p->body.y - p->prev_y) * a + (double)PHYS_PLAYER_EYE;
    if (z != NULL) *z = p->prev_z + (p->body.z - p->prev_z) * a;
    // The view angles interpolate too, or turning is visibly steppy at
    // 20 Hz however smooth the position is.
    if (yaw != NULL) *yaw = p->prev_yaw + (p->yaw - p->prev_yaw) * alpha;
    if (pitch != NULL) *pitch = p->prev_pitch + (p->pitch - p->prev_pitch) * alpha;
}
