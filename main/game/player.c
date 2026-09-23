// =====================================================================
//  CraftMiner  --  the player (see player.h)
// =====================================================================

#include "audio/sfx.h"
#include "game/player.h"

#include <math.h>

#include "game/interact.h"
#include "items/item_entity.h"
#include "world/chunk.h"

// Just short of straight up or down. Exactly +-pi/2 makes the forward
// vector's horizontal part zero, and then "which way am I walking" has
// no answer.
#define PITCH_MAX 1.55f

// Ticks between one blow of the tool and the next while a block is being
// mined. Kept in step with FRED_STROKES in main.c (1.8 swings a second at
// 20 Hz), so the sound lands with the arm rather than beside it.
#define SWING_TICKS 11

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
    p->aim_valid   = false;
    p->mining      = false;
    p->mine_ticks  = 0;
}

bool player_place(player_t* p, double x, double y, double z, float yaw, float pitch) {
    // A fresh body for the test, so it has the player's dimensions even
    // on a player that has never been initialised.
    phys_body_t probe;
    phys_body_init(&probe, x, y, z);
    if (!phys_fits(&probe, x, y, z)) return false;
    p->body = probe;
    p->yaw        = yaw;
    p->pitch      = pitch;
    p->prev_x     = x;
    p->prev_y     = y;
    p->prev_z     = z;
    p->prev_yaw   = yaw;
    p->prev_pitch = pitch;
    p->in_air_last = false;
    p->aim_valid   = false;
    p->mining      = false;
    p->mine_ticks  = 0;
    return true;
}

void player_reset(player_t* p) {
    p->health     = PL_HEALTH_MAX;
    p->hunger     = PL_HUNGER_MAX;
    p->aim_valid  = false;
    p->mining     = false;
    p->mine_ticks = 0;

    inv_clear(&p->inv);
    // A starting kit, until crafting exists (step 8). Tools so that
    // durability and break speed can be felt, blocks so that placing
    // can be. Remove this the day a crafting table can make them.
    inv_add(&p->inv, ITEM_PICK_STONE, 1, 0);
    inv_add(&p->inv, ITEM_AXE_STONE, 1, 0);
    inv_add(&p->inv, ITEM_SHOVEL_STONE, 1, 0);
    inv_add(&p->inv, BLK_COBBLE, 64, 0);
    inv_add(&p->inv, BLK_PLANKS, 64, 0);
    inv_add(&p->inv, BLK_TORCH, 32, 0);
}

float player_mine_progress(player_t const* p) {
    if (!p->mining || p->mine_needed <= 0) return 0.0f;
    float const f = (float)p->mine_ticks / (float)p->mine_needed;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

void player_tick(player_t* p, cm_actions_t mask, cm_actions_t pressed) {
    // --- The inventory screen ----------------------------------------
    //
    // Open, it takes the movement and look keys for navigation and the
    // player stands still. Reading a grid while still walking is how
    // you end up in the lava you were standing next to.
    if (act_held(pressed, CM_INVENTORY)) {
        p->inv.open = !p->inv.open;
        p->mining   = false;
    }
    if (p->inv.open) {
        p->prev_x     = p->body.x;
        p->prev_y     = p->body.y;
        p->prev_z     = p->body.z;
        p->prev_yaw   = p->yaw;
        p->prev_pitch = p->pitch;

        int const dx = (act_held(pressed, CM_RIGHT) || act_held(pressed, CM_LOOK_RIGHT) ? 1 : 0) -
                       (act_held(pressed, CM_LEFT) || act_held(pressed, CM_LOOK_LEFT) ? 1 : 0);
        int const dy = (act_held(pressed, CM_BACK) || act_held(pressed, CM_LOOK_DOWN) ? 1 : 0) -
                       (act_held(pressed, CM_FORWARD) || act_held(pressed, CM_LOOK_UP) ? 1 : 0);
        if (dx || dy) inv_move_cursor(&p->inv, dx, dy);

        // A hotbar key SWAPS the cursor's stack into that slot -- the
        // one operation the screen has to support, since without it
        // everything past the sixth slot is unreachable.
        for (int i = 0; i < INV_HOTBAR; i++) {
            if (act_held(pressed, (cm_action_t)(CM_SLOT1 + i))) inv_swap(&p->inv, p->inv.cursor, i);
        }

        // Still fall while reading: standing over a hole and opening
        // the inventory must not make you hover.
        p->body.vx = 0.0f;
        p->body.vz = 0.0f;
        phys_move(&p->body, 0.0, (double)p->body.vy, 0.0);
        phys_gravity(&p->body, PL_GRAVITY, PL_DRAG, PL_TERMINAL);
        if (item_entity_tick(&p->inv, p->body.x, p->body.y, p->body.z) > 0) sfx_play(SFX_PICKUP);
        p->aim_valid = false;
        return;
    }

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

    // --- In the water? ------------------------------------------------
    //
    // Tested at the middle of the body (PL_WADE_Y): wading through a
    // shallow stream should not turn into swimming, and being up to the
    // chest in it should not stay walking.
    bool const in_water = block_liquid(world_block((int32_t)floor(p->body.x), (int32_t)floor(p->body.y + PL_WADE_Y),
                                                   (int32_t)floor(p->body.z)));

    // --- Walking ------------------------------------------------------
    //
    // Flattened: forward is where the player is facing, not where they
    // are looking. Looking at your feet must not slow you down.
    float const fwd = (act_held(mask, CM_FORWARD) ? 1.0f : 0.0f) - (act_held(mask, CM_BACK) ? 1.0f : 0.0f);
    float const str = (act_held(mask, CM_RIGHT) ? 1.0f : 0.0f) - (act_held(mask, CM_LEFT) ? 1.0f : 0.0f);
    // In water there is one speed: sneak means dive, not creep.
    float const speed = in_water ? PL_SWIM : act_held(mask, CM_SNEAK) ? PL_SNEAK : PL_WALK;

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
    // Water has purchase the way the ground does, but half of it, so
    // starting and stopping in it feel heavy.
    float const accel = in_water ? PL_ACCEL * 0.5f : p->body.on_ground ? PL_ACCEL : PL_ACCEL * 0.2f;
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
    if (in_water) {
        // Swimming: an impulse added every tick the key is held, which
        // the water's drag turns into a steady rise (player.h). Jump
        // goes up, sneak goes down; let go and you sink slowly.
        if (act_held(mask, CM_JUMP)) p->body.vy += PL_SWIM_UP;
        else if (act_held(mask, CM_SNEAK)) p->body.vy -= PL_SWIM_UP;
    } else if (act_held(mask, CM_JUMP) && p->body.on_ground) {
        p->body.vy = PL_JUMP;
    }

    p->in_air_last = !p->body.on_ground;
    phys_move(&p->body, (double)p->body.vx, (double)p->body.vy, (double)p->body.vz);

    // A stopped body should not keep a velocity it cannot use, or it
    // shoots off the moment the obstruction goes away.
    if (p->body.hit_x) p->body.vx = 0.0f;
    if (p->body.hit_z) p->body.vz = 0.0f;

    // The vertical for the NEXT tick, including the landing and
    // head-bump cases. In physics.c so that the host test runs the
    // same code rather than a copy of it.
    if (in_water) {
        phys_gravity(&p->body, PL_WATER_GRAV, PL_WATER_DRAG, PL_WATER_TERM);
    } else {
        phys_gravity(&p->body, PL_GRAVITY, PL_DRAG, PL_TERMINAL);
    }

    // --- The hotbar ---------------------------------------------------
    for (int i = 0; i < INV_HOTBAR; i++) {
        if (act_held(pressed, (cm_action_t)(CM_SLOT1 + i))) {
            p->inv.selected = i;
            p->mining       = false;  // switching tools abandons the dig
        }
    }

    // --- What the crosshair is on ------------------------------------
    double const ex = p->body.x, ey = p->body.y + (double)PHYS_PLAYER_EYE, ez = p->body.z;
    float        dx, dy, dz;
    ray_forward(p->yaw, p->pitch, &dx, &dy, &dz);
    // Anything that can be pointed at, not only solids: torches, flowers
    // and tall grass have to be breakable too (F-56).
    p->aim_valid = ray_pick(ex, ey, ez, dx, dy, dz, RAY_REACH, false, &p->aim);

    uint16_t const held = inv_held(&p->inv)->item;

    // --- Breaking, which is HELD ------------------------------------
    //
    // A block takes item_break_ticks() of them. That is what makes
    // hardness and the tool in hand mean anything, and it is what the
    // crack overlay animates once it is retargeted. Progress belongs to
    // a CELL: look away and it is abandoned, which is the behaviour
    // everyone expects and nobody states.
    if (p->aim_valid && act_held(mask, CM_ATTACK)) {
        bool const same = p->mining && p->mine_x == p->aim.x && p->mine_y == p->aim.y && p->mine_z == p->aim.z;
        if (!same) {
            p->mining      = true;
            p->mine_x      = p->aim.x;
            p->mine_y      = p->aim.y;
            p->mine_z      = p->aim.z;
            p->mine_ticks  = 0;
            p->mine_needed = item_break_ticks(p->aim.block, held);
        }
        uint8_t const aimed = p->aim.block;
        if (p->mine_needed < 0) {
            p->mining = false;  // unbreakable: bedrock, or the edge of the world
        } else if (++p->mine_ticks >= p->mine_needed) {
            break_result_t const r = interact_break(p->aim.x, p->aim.y, p->aim.z, held);
            if (r.ok) {
                // One use per BREAK, not per felled block: a tree is
                // one swing of the axe, not forty.
                inv_wear_held(&p->inv, 1);
                // A whole tree coming down is a different sound from one
                // block breaking, and it should be: the rule that made it
                // fall is the game's one deliberate departure from
                // Minecraft (Part F), so it is worth hearing.
                if (r.was_tree && r.felled > 1) sfx_play(SFX_FELL);
                else sfx_play_break(aimed);
            }
            p->mining    = false;
            p->aim_valid = false;  // whatever was aimed at is gone
        } else if ((p->mine_ticks % SWING_TICKS) == 1) {
            // The tool striking the block, at the rate Fred's arm swings
            // -- not once a tick, which would be a buzz rather than
            // a tapping.
            sfx_play_pitched(SFX_HIT, block_sound(aimed) == SND_STONE ? 3.0f : 0.0f);
        }
    } else {
        p->mining = false;
    }

    // --- Placing, which is a tap -------------------------------------
    if (p->aim_valid && act_held(pressed, CM_USE)) {
        uint8_t const block = item_block(held);
        if (block != BLK_AIR && interact_place(&p->aim, block, &p->body)) {
            inv_consume_held(&p->inv);
            sfx_play_place(block);
        }
    }

    // --- Dropping what is held ---------------------------------------
    //
    // THROWN, not placed. A dropped item lands inside the 1.4-block
    // pickup radius whichever way you face -- you cannot throw a thing
    // further than your own arm in one tick -- so it is the pickup
    // DELAY that makes G work at all, not the distance. Two seconds is
    // long enough to walk away from.
    if (act_held(pressed, CM_DROP)) {
        inv_slot_t* s = inv_held(&p->inv);
        if (s->item != 0) {
            double const ox = p->body.x + (double)(dx * 0.4f);
            double const oy = p->body.y + (double)PHYS_PLAYER_EYE - 0.3;
            double const oz = p->body.z + (double)(dz * 0.4f);
            if (item_entity_throw(ox, oy, oz, s->item, 1, s->wear, dx * 0.22f, 0.12f, dz * 0.22f, ITEM_THROW_DELAY) >
                0) {
                inv_consume_held(&p->inv);
            }
        }
    }

    // --- What is lying about -----------------------------------------
    if (item_entity_tick(&p->inv, p->body.x, p->body.y, p->body.z) > 0) sfx_play(SFX_PICKUP);
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
