// =====================================================================
//  SynthMiner  --  a body moving through blocks (see physics.h)
// =====================================================================

#include "game/physics.h"

#include "world/chunk.h"

// floor() for a double, without libm: (int) truncates towards zero,
// which is floor only for a positive value.
static inline int32_t fl(double v) {
    int32_t const i = (int32_t)v;
    return (v < (double)i) ? i - 1 : i;
}

void phys_body_init(phys_body_t* b, double x, double y, double z) {
    b->x         = x;
    b->y         = y;
    b->z         = z;
    b->vx = b->vy = b->vz = 0.0f;
    b->w         = PHYS_PLAYER_W;
    b->h         = PHYS_PLAYER_H;
    b->on_ground = false;
    b->hit_x = b->hit_z = b->hit_head = false;
}

// Is anything solid inside the box lo..hi?
//
// The box is shrunk by PHYS_SKIN before the cells are worked out, so a
// body resting exactly on y = 12.0 does not count the block at y = 11
// as an overlap. Without that, every contact reads as a collision on
// the next move and the body sticks.
static bool solid_in(double x0, double y0, double z0, double x1, double y1, double z1) {
    int32_t const ix0 = fl(x0 + PHYS_SKIN), ix1 = fl(x1 - PHYS_SKIN);
    int32_t const iy0 = fl(y0 + PHYS_SKIN), iy1 = fl(y1 - PHYS_SKIN);
    int32_t const iz0 = fl(z0 + PHYS_SKIN), iz1 = fl(z1 - PHYS_SKIN);
    for (int32_t y = iy0; y <= iy1; y++) {
        for (int32_t z = iz0; z <= iz1; z++) {
            for (int32_t x = ix0; x <= ix1; x++) {
                if (block_solid(world_block(x, y, z))) return true;
            }
        }
    }
    return false;
}

static bool body_overlaps(phys_body_t const* b, double x, double y, double z) {
    double const hw = (double)b->w * 0.5;
    return solid_in(x - hw, y, z - hw, x + hw, y + (double)b->h, z + hw);
}

bool phys_fits(phys_body_t const* b, double x, double y, double z) {
    return !body_overlaps(b, x, y, z);
}

// One axis, one sub-step. Returns true if it moved the whole way.
//
// On a block, the body is placed against the face rather than left
// where it was: walking into a wall at an angle has to end up touching
// it, or the next frame's step-up test is asking about the wrong place.
static bool step_axis(phys_body_t* b, int axis, double d) {
    if (d == 0.0) return true;
    double const nx = b->x + (axis == 0 ? d : 0.0);
    double const ny = b->y + (axis == 1 ? d : 0.0);
    double const nz = b->z + (axis == 2 ? d : 0.0);

    if (!body_overlaps(b, nx, ny, nz)) {
        b->x = nx;
        b->y = ny;
        b->z = nz;
        return true;
    }

    // Blocked. Put the leading face flush against the cell boundary it
    // ran into. The sub-step is at most PHYS_SUBSTEP, so the boundary
    // crossed is the one the leading edge is now inside.
    double const hw = (double)b->w * 0.5;
    if (axis == 1) {
        if (d > 0.0) {
            b->y      = (double)fl(ny + (double)b->h) - (double)b->h - PHYS_SKIN;
            b->hit_head = true;
        } else {
            b->y         = (double)(fl(ny) + 1) + PHYS_SKIN;
            b->on_ground = true;
        }
    } else if (axis == 0) {
        b->x     = d > 0.0 ? (double)fl(nx + hw) - hw - PHYS_SKIN : (double)(fl(nx - hw) + 1) + hw + PHYS_SKIN;
        b->hit_x = true;
    } else {
        b->z     = d > 0.0 ? (double)fl(nz + hw) - hw - PHYS_SKIN : (double)(fl(nz - hw) + 1) + hw + PHYS_SKIN;
        b->hit_z = true;
    }
    return false;
}

// A whole axis, cut into sub-steps so nothing is crossed untested.
static bool move_axis(phys_body_t* b, int axis, double d) {
    bool whole = true;
    while (d != 0.0) {
        double step = d;
        if (step > PHYS_SUBSTEP) step = PHYS_SUBSTEP;
        if (step < -PHYS_SUBSTEP) step = -PHYS_SUBSTEP;
        if (!step_axis(b, axis, step)) return false;  // stopped; the rest of d is gone
        d -= step;
        // Guard against a step so small the subtraction does not change
        // d -- a body would otherwise spin here forever.
        if (step == 0.0) break;
        (void)whole;
    }
    return true;
}

void phys_move(phys_body_t* b, double dx, double dy, double dz) {
    b->on_ground = false;
    b->hit_x = b->hit_z = b->hit_head = false;

    // Vertical first, so `on_ground` is true before the horizontal move
    // asks whether it may step up. A body that has just walked off a
    // ledge should not get a free step in mid-air.
    move_axis(b, 1, dy);

    if (dx == 0.0 && dz == 0.0) return;

    // Remember where we were, in case the step-up has to be undone.
    double const ox = b->x, oy = b->y, oz = b->z;
    bool const   was_on_ground = b->on_ground;

    move_axis(b, 0, dx);
    move_axis(b, 2, dz);

    if (!(b->hit_x || b->hit_z) || !was_on_ground) return;

    // Blocked at foot level while standing on something: try the same
    // move from a step higher. Only keep it if the body can then settle
    // back onto something -- otherwise this is a wall, not a step, and
    // climbing it would let the player walk up sheer cliffs.
    double const blocked_x = b->x, blocked_y = b->y, blocked_z = b->z;
    bool const   blocked_hit_x = b->hit_x, blocked_hit_z = b->hit_z;

    b->x = ox;
    b->y = oy;
    b->z = oz;
    b->hit_x = b->hit_z = false;

    if (!move_axis(b, 1, (double)PHYS_STEP)) {
        // No headroom to lift into: keep the blocked result.
        b->x = blocked_x;
        b->y = blocked_y;
        b->z = blocked_z;
        b->hit_x = blocked_hit_x;
        b->hit_z = blocked_hit_z;
        return;
    }
    move_axis(b, 0, dx);
    move_axis(b, 2, dz);

    // Settle back down onto the step. Not more than we lifted, or a
    // body could gain height by walking at a wall.
    double const lifted = b->y - oy;
    b->on_ground        = false;
    move_axis(b, 1, -lifted);

    bool const gained = (b->x != ox || b->z != oz);
    if (!gained || !b->on_ground) {
        // The step did not help, or left us hanging. Take the blocked
        // result instead, which at least touches the wall.
        b->x         = blocked_x;
        b->y         = blocked_y;
        b->z         = blocked_z;
        b->hit_x     = blocked_hit_x;
        b->hit_z     = blocked_hit_z;
        b->on_ground = was_on_ground;
    }
}

void phys_gravity(phys_body_t* b, float gravity, float drag, float terminal) {
    if (b->on_ground && b->vy < 0.0f) b->vy = 0.0f;
    if (b->hit_head && b->vy > 0.0f) b->vy = 0.0f;
    b->vy = (b->vy - gravity) * drag;
    if (b->vy < -terminal) b->vy = -terminal;
}

double phys_settle(phys_body_t const* b, double x, double y, double z, int max_drop) {
    for (int i = 0; i <= max_drop; i++) {
        double const yy = y - (double)i;
        if (!phys_fits(b, x, yy, z)) continue;          // inside something: keep looking down
        if (body_overlaps(b, x, yy - 0.05, z)) return yy;  // something solid just below: here
    }
    return y;
}
